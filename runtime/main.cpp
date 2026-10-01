// SDL2 frontend: window/video, audio, input, pacing, and startup.
#include <SDL.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "pgxp.h"
#include "psx.h"
#include "state.h"

extern "C" FILE* g_trace_file;
extern "C" long long g_break_cycles;

namespace psx {

bool g_quit = false;
std::string g_data_dir = ".";
void bios_flush_cards();

static SDL_Window* window = nullptr;
static SDL_Renderer* renderer = nullptr;
static SDL_Texture* texture = nullptr;
static int tex_w = 0, tex_h = 0;
static SDL_AudioDeviceID audio_dev = 0;
// every connected controller is read and their inputs combined: devices SDL has a mapping
// for use the standard layout, others (e.g. vJoy, DirectInput pads without a mapping) a
// generic one
static std::vector<SDL_GameController*> gamepads;
static std::vector<SDL_Joystick*> joysticks;
static std::vector<uint32_t> frame(1024 * 512);
static uint64_t frame_counter = 0;
static double next_frame_time = 0;
static bool fast_forward = false;
static bool paused = false;
static uint64_t exit_after_frames = 0;
static int state_slot = 1;      // save state slot (F2/F3)
static uint64_t save_at_frame = 0;  // --save-state-at N slot / --load-state slot (tests)
static int save_at_slot = 0, load_at_start = 0;
static uint64_t load_at_frame = 0;
static int load_at_slot = 0;
static int display_index = -1;  // monitor to open the window on (--display / display= in gt.cfg)

// --keys "frame:button[+button]:duration,..." scripted input for automated tests
struct KeyEvent { uint64_t frame, until; uint16_t mask; };
static std::vector<KeyEvent> key_script;
static uint16_t button_bit(const std::string& n) {
    static const char* names[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                    "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
    for (int i = 0; i < 16; i++) if (n == names[i]) return (uint16_t)(1u << i);
    return 0;
}
static void parse_keys(const std::string& spec) {
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t end = spec.find(',', pos);
        std::string item = spec.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? spec.size() : end + 1;
        size_t a = item.find(':'), b = item.rfind(':');
        if (a == std::string::npos) continue;
        KeyEvent e{};
        e.frame = strtoull(item.c_str(), nullptr, 10);
        std::string btns = item.substr(a + 1, (b > a ? b : item.size()) - a - 1);
        uint64_t dur = b > a ? strtoull(item.c_str() + b + 1, nullptr, 10) : 6;
        e.until = e.frame + (dur ? dur : 6);
        size_t p2 = 0;
        while (p2 <= btns.size()) {
            size_t q = btns.find('+', p2);
            e.mask |= button_bit(btns.substr(p2, q == std::string::npos ? std::string::npos : q - p2));
            if (q == std::string::npos) break;
            p2 = q + 1;
        }
        key_script.push_back(e);
    }
}
static uint64_t shot_frame = 0;
static uint64_t shot_every = 0;
static std::string shot_dir;
static std::string shot_path;

static int frame_w = 0, frame_h = 0;

static void save_screenshot(const std::string& path) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(frame.data(), frame_w, frame_h, 32, frame_w * 4, SDL_PIXELFORMAT_ARGB8888);
    if (s) {
        SDL_SaveBMP(s, path.c_str());
        SDL_FreeSurface(s);
    }
}

static double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// size of the displayed picture (internal resolution; 24-bit video mode stays native)
static void display_size(int& w, int& h) {
    DisplayInfo d = gpu_display();
    int sc = d.rgb24 ? 1 : gpu_scale();
    w = d.w * sc;
    h = d.h * sc;
}

// displayed area -> ARGB rows at dst (pitch in pixels)
static void convert_display(uint32_t* dst, int pitch, int w, int h) {
    DisplayInfo d = gpu_display();
    if (!d.enabled) {
        for (int y = 0; y < h; y++) memset(dst + (size_t)y * pitch, 0, (size_t)w * 4);
        return;
    }
    if (!d.rgb24) {
        gpu_convert_display(dst, pitch, d.x, d.y, d.w, d.h);
        return;
    }
    gpu_sync();
    for (int y = 0; y < h; y++) {
        int vy = (d.y + y) & 511;
        auto byte = [&](int bx) {
            uint16_t p = gpu_vram_native((bx >> 1) & 1023, vy);
            return (uint8_t)(bx & 1 ? p >> 8 : p);
        };
        for (int x = 0; x < w; x++) {
            int bx = (d.x * 2 + x * 3) % 2048;
            uint8_t r = byte(bx), g = byte((bx + 1) % 2048), b = byte((bx + 2) % 2048);
            dst[(size_t)y * pitch + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

// persistent user settings (gt.cfg next to the game)
static void load_settings() {
    FILE* f = fopen("gt.cfg", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "widescreen=%d", &v) == 1) set_widescreen(v != 0);
        if (sscanf(line, "scale=%d", &v) == 1) gpu_set_scale(v);
        if (sscanf(line, "pgxp=%d", &v) == 1) g_pgxp = v != 0;
        if (sscanf(line, "dither=%d", &v) == 1) gpu_set_dither(v != 0);
        if (sscanf(line, "display=%d", &v) == 1) display_index = v;
    }
    fclose(f);
}

static void save_settings() {
    FILE* f = fopen("gt.cfg", "w");
    if (!f) return;
    fprintf(f, "widescreen=%d\n", widescreen() ? 1 : 0);
    fprintf(f, "scale=%d\n", gpu_scale());
    fprintf(f, "pgxp=%d\n", g_pgxp ? 1 : 0);
    fprintf(f, "dither=%d\n", gpu_dither() ? 1 : 0);
    if (display_index >= 0) fprintf(f, "display=%d\n", display_index);
    fclose(f);
}

static void update_pad() {
    const uint8_t* k = SDL_GetKeyboardState(nullptr);
    uint16_t b = 0xFFFF;
    auto press = [&](int bit) { b &= ~(1u << bit); };
    // keyboard
    if (k[SDL_SCANCODE_SPACE]) press(0);
    if (k[SDL_SCANCODE_RETURN]) press(3);
    if (k[SDL_SCANCODE_UP]) press(4);
    if (k[SDL_SCANCODE_RIGHT]) press(5);
    if (k[SDL_SCANCODE_DOWN]) press(6);
    if (k[SDL_SCANCODE_LEFT]) press(7);
    if (k[SDL_SCANCODE_LCTRL]) press(8);
    if (k[SDL_SCANCODE_RCTRL]) press(9);
    if (k[SDL_SCANCODE_LSHIFT]) press(10);
    if (k[SDL_SCANCODE_RSHIFT]) press(11);
    if (k[SDL_SCANCODE_Z]) press(12);
    if (k[SDL_SCANCODE_V]) press(13);
    if (k[SDL_SCANCODE_C]) press(14);
    if (k[SDL_SCANCODE_X]) press(15);
    if (k[SDL_SCANCODE_LEFTBRACKET]) press(1);
    if (k[SDL_SCANCODE_RIGHTBRACKET]) press(2);
    for (SDL_GameController* gamepad : gamepads) {
        auto btn = [&](SDL_GameControllerButton sb, int bit) { if (SDL_GameControllerGetButton(gamepad, sb)) press(bit); };
        btn(SDL_CONTROLLER_BUTTON_BACK, 0);
        btn(SDL_CONTROLLER_BUTTON_LEFTSTICK, 1);
        btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK, 2);
        btn(SDL_CONTROLLER_BUTTON_START, 3);
        btn(SDL_CONTROLLER_BUTTON_DPAD_UP, 4);
        btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 5);
        btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN, 6);
        btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT, 7);
        btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 10);
        btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 11);
        btn(SDL_CONTROLLER_BUTTON_Y, 12);
        btn(SDL_CONTROLLER_BUTTON_B, 13);
        btn(SDL_CONTROLLER_BUTTON_A, 14);
        btn(SDL_CONTROLLER_BUTTON_X, 15);
        if (SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000) press(8);
        if (SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000) press(9);
        int lx = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTX);
        int ly = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTY);
        if (lx < -12000) press(7);
        if (lx > 12000) press(5);
        if (ly < -12000) press(4);
        if (ly > 12000) press(6);
    }
    for (SDL_Joystick* joystick : joysticks) {
        // generic layout: 0 X/Cross, 1 Circle, 2 Square, 3 Triangle, 4 L1, 5 R1, 6 L2, 7 R2, 8 Select, 9 Start
        static const int map[10] = {14, 13, 15, 12, 10, 11, 8, 9, 0, 3};
        int nb = std::min(SDL_JoystickNumButtons(joystick), 10);
        for (int i = 0; i < nb; i++) if (SDL_JoystickGetButton(joystick, i)) press(map[i]);
        if (SDL_JoystickNumHats(joystick) > 0) {
            Uint8 h = SDL_JoystickGetHat(joystick, 0);
            if (h & SDL_HAT_UP) press(4);
            if (h & SDL_HAT_RIGHT) press(5);
            if (h & SDL_HAT_DOWN) press(6);
            if (h & SDL_HAT_LEFT) press(7);
        }
        if (SDL_JoystickNumAxes(joystick) >= 2) {
            int x = SDL_JoystickGetAxis(joystick, 0), y = SDL_JoystickGetAxis(joystick, 1);
            if (x < -12000) press(7);
            if (x > 12000) press(5);
            if (y < -12000) press(4);
            if (y > 12000) press(6);
        }
    }
    for (const KeyEvent& e : key_script)
        if (frame_counter >= e.frame && frame_counter < e.until) b &= ~e.mask;
    if (b != g_pads[0].buttons) LOGD("pad: %04x", (uint16_t)~b);
    g_pads[0].buttons = b;
}

static bool device_open(SDL_JoystickID id) {
    for (SDL_GameController* g : gamepads)
        if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g)) == id) return true;
    for (SDL_Joystick* j : joysticks)
        if (SDL_JoystickInstanceID(j) == id) return true;
    return false;
}

static void open_device(int index) {
    if (device_open(SDL_JoystickGetDeviceInstanceID(index))) return;
    char guid[64];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof guid);
    if (SDL_IsGameController(index)) {
        if (SDL_GameController* g = SDL_GameControllerOpen(index)) {
            gamepads.push_back(g);
            LOGI("controller connected: '%s' (standard layout, guid %s)", SDL_GameControllerName(g), guid);
        }
        return;
    }
    if (SDL_Joystick* j = SDL_JoystickOpen(index)) {
        joysticks.push_back(j);
        LOGI("joystick connected: '%s' (generic layout: %d buttons, %d axes, %d hats, guid %s)", SDL_JoystickName(j),
             SDL_JoystickNumButtons(j), SDL_JoystickNumAxes(j), SDL_JoystickNumHats(j), guid);
    }
}

static void close_device(SDL_JoystickID id) {
    for (size_t i = 0; i < gamepads.size(); i++)
        if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepads[i])) == id) {
            LOGI("controller disconnected: '%s'", SDL_GameControllerName(gamepads[i]));
            SDL_GameControllerClose(gamepads[i]);
            gamepads.erase(gamepads.begin() + i);
            return;
        }
    for (size_t i = 0; i < joysticks.size(); i++)
        if (SDL_JoystickInstanceID(joysticks[i]) == id) {
            LOGI("joystick disconnected: '%s'", SDL_JoystickName(joysticks[i]));
            SDL_JoystickClose(joysticks[i]);
            joysticks.erase(joysticks.begin() + i);
            return;
        }
}

void frontend_poll_input() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: g_quit = true; break;
        case SDL_JOYDEVICEADDED: open_device(e.jdevice.which); break;
        case SDL_JOYDEVICEREMOVED: close_device(e.jdevice.which); break;
        case SDL_KEYDOWN:
            LOGD("key down: scancode %d (%s)", e.key.keysym.scancode, SDL_GetScancodeName(e.key.keysym.scancode));
            if (e.key.keysym.scancode == SDL_SCANCODE_F11 ||
                (e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                bool fs = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
            if (e.key.keysym.scancode == SDL_SCANCODE_F10) {  // internal resolution 1x -> 2x -> 4x
                gpu_set_scale(gpu_scale() >= 4 ? 1 : gpu_scale() * 2);
                save_settings();
            }
            if (e.key.keysym.scancode == SDL_SCANCODE_F8) {
                g_pgxp = !g_pgxp;
                save_settings();
            }
            if (e.key.keysym.scancode == SDL_SCANCODE_F7) {
                gpu_set_dither(!gpu_dither());
                save_settings();
            }
            if (e.key.keysym.scancode == SDL_SCANCODE_F9) {
                set_widescreen(!widescreen());
                save_settings();
            }
            if (e.key.keysym.scancode == SDL_SCANCODE_F5) state_request_save(state_slot);
            if (e.key.keysym.scancode == SDL_SCANCODE_F6) state_request_load(state_slot);
            if (e.key.keysym.scancode == SDL_SCANCODE_F2) state_slot = state_slot > 1 ? state_slot - 1 : 9;
            if (e.key.keysym.scancode == SDL_SCANCODE_F3) state_slot = state_slot < 9 ? state_slot + 1 : 1;
            if (e.key.keysym.scancode == SDL_SCANCODE_TAB) fast_forward = true;
            if (e.key.keysym.scancode == SDL_SCANCODE_PAUSE) paused = !paused;
            break;
        case SDL_KEYUP:
            if (e.key.keysym.scancode == SDL_SCANCODE_TAB) fast_forward = false;
            break;
        default: break;
        }
    }
    update_pad();
}

extern uint64_t g_gpu_stalls, g_gpu_stall_why[4], g_gpu_copies;
extern double g_gpu_wait_s, g_gpu_full_s;
static bool profile = getenv("GT_PROFILE") != nullptr;

void frontend_vblank() {
    frame_counter++;
    if (save_at_frame && frame_counter == save_at_frame) state_request_save(save_at_slot);
    if (load_at_frame && frame_counter == load_at_frame) state_request_load(load_at_slot);
    static double p_last = 0, p_sync = 0, p_conv = 0, p_present = 0;
    double t0 = now_seconds();
    gpu_sync();
    double t1 = now_seconds();
    display_size(frame_w, frame_h);
    if (!texture || frame_w > tex_w || frame_h > tex_h) {
        if (texture) SDL_DestroyTexture(texture);
        tex_w = std::max(frame_w, 1024);
        tex_h = std::max(frame_h, 512);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, tex_w, tex_h);
    }
    SDL_Rect src = {0, 0, frame_w, frame_h};
    bool shot_now = (shot_frame && frame_counter == shot_frame) || (shot_every && frame_counter % shot_every == 0);
    void* pixels;
    int pitch;
    if (!shot_now && SDL_LockTexture(texture, &src, &pixels, &pitch) == 0) {
        convert_display((uint32_t*)pixels, pitch / 4, frame_w, frame_h);
        SDL_UnlockTexture(texture);
    } else {
        // screenshots keep a copy of the frame
        if (frame.size() < (size_t)frame_w * frame_h) frame.resize((size_t)frame_w * frame_h);
        convert_display(frame.data(), frame_w, frame_w, frame_h);
        SDL_UpdateTexture(texture, &src, frame.data(), frame_w * 4);
    }
    double t2 = now_seconds();
    int ww, wh;
    SDL_GetRendererOutputSize(renderer, &ww, &wh);
    // the picture fills a 4:3 screen; widescreen renders a 16:9 field of view into the same
    // buffer (runtime/gte.cpp), shown stretched to 16:9
    int ax = widescreen() ? 16 : 4, ay = widescreen() ? 9 : 3;
    int dw = ww, dh = ww * ay / ax;
    if (dh > wh) { dh = wh; dw = wh * ax / ay; }
    SDL_Rect dst = {(ww - dw) / 2, (wh - dh) / 2, dw, dh};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, &src, &dst);
    SDL_RenderPresent(renderer);
    if (profile) {
        double t3 = now_seconds();
        p_sync += t1 - t0; p_conv += t2 - t1; p_present += t3 - t2;
        if (frame_counter % 300 == 0) {
            double total = t3 - p_last;
            LOGI("profile: %.2f ms/frame: emu %.2f, gpu wait %.2f, convert %.2f, present %.2f, stalls %.1f/frame (tex %llu upload %llu download %llu)",
                 total * 1000 / 300, (total - p_sync - p_conv - p_present) * 1000 / 300, p_sync * 1000 / 300,
                 p_conv * 1000 / 300, p_present * 1000 / 300, g_gpu_stalls / 300.0, (unsigned long long)g_gpu_stall_why[1],
                 (unsigned long long)g_gpu_stall_why[2], (unsigned long long)g_gpu_stall_why[3]);
            memset(g_gpu_stall_why, 0, sizeof g_gpu_stall_why);
            LOGI("profile: gpu sync wait %.2f ms/frame (incl. vblank), queue full %.2f ms/frame, vram copies %.1f/frame",
                 g_gpu_wait_s * 1000 / 300, g_gpu_full_s * 1000 / 300, g_gpu_copies / 300.0);
            g_gpu_wait_s = g_gpu_full_s = 0;
            g_gpu_copies = 0;
            p_last = t3; p_sync = p_conv = p_present = 0; g_gpu_stalls = 0;
        }
    }
    if (shot_frame && frame_counter == shot_frame) save_screenshot(shot_path);
    if (shot_every && frame_counter % shot_every == 0) {
        char name[64];
        snprintf(name, sizeof name, "/shot_%06llu.bmp", (unsigned long long)frame_counter);
        save_screenshot(shot_dir + name);
    }
    if (exit_after_frames && frame_counter >= exit_after_frames) g_quit = true;

    do {
        frontend_poll_input();
        if (g_quit) {
            bios_flush_cards();
            SDL_Quit();
            fflush(stderr);
            fflush(stdout);
            _Exit(0);
        }
        if (paused) SDL_Delay(16);
    } while (paused);

    // pace to NTSC field rate
    const double period = 1.0 / 59.826;
    double t = now_seconds();
    if (next_frame_time == 0 || t - next_frame_time > 0.25) next_frame_time = t;
    next_frame_time += period;
    if (!fast_forward) {
        double wait = next_frame_time - now_seconds();
        if (wait > 0.002) std::this_thread::sleep_for(std::chrono::duration<double>(wait - 0.001));
        while (now_seconds() < next_frame_time) {}
    }

    static double fps_t = 0;
    static uint64_t fps_n = 0;
    static double fps = 0;
    static std::string last_title;
    if (fps_t == 0) fps_t = t;
    if (t - fps_t >= 1.0) {
        fps = (frame_counter - fps_n) / (t - fps_t);
        fps_t = t;
        fps_n = frame_counter;
    }
    char title[256];
    const char* msg = state_message();
    snprintf(title, sizeof title, "Gran Turismo (static recomp) - %.1f fps%s - %dx%s - slot %d%s%s", fps,
             widescreen() ? " - 16:9" : "", gpu_scale(), g_pgxp ? " PGXP" : "", state_slot, *msg ? " - " : "", msg);
    if (last_title != title) {
        SDL_SetWindowTitle(window, title);
        last_title = title;
    }
}

// a save state was just loaded: drop audio from before it and restart frame pacing
void frontend_after_load() {
    if (audio_dev) SDL_ClearQueuedAudio(audio_dev);
    next_frame_time = 0;
}

void frontend_audio(const int16_t* stereo, int frames) {
    if (!audio_dev || fast_forward) return;
    static uint64_t total = 0;
    static int peak = 0;
    for (int i = 0; i < frames * 2; i++) peak = std::max(peak, std::abs((int)stereo[i]));
    total += frames;
    if (profile && total >= 44100 * 5) {
        LOGI("audio: %llu frames queued in the last 5 s, peak %d", (unsigned long long)total, peak);
        total = 0;
        peak = 0;
    }
    uint32_t queued = SDL_GetQueuedAudioSize(audio_dev);
    if (queued > 44100 * 4 / 5) return;  // > 200 ms behind: drop
    SDL_QueueAudio(audio_dev, stereo, frames * 4);
}

static std::string find_disc(int argc, char** argv) {
    if (argc > 1 && argv[1][0] != '-') return argv[1];
    const char* env = getenv("GT_DISC");
    if (env) return env;
    FILE* f = fopen("gt_disc.txt", "r");
    if (f) {
        char line[1024] = {};
        if (fgets(line, sizeof line, f)) {
            fclose(f);
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            return s;
        }
        fclose(f);
    }
    return "";
}

}  // namespace psx

using namespace psx;

int main(int argc, char** argv) {
#ifdef _WIN32
    // GUI build: no console, so keep the log in a file next to the game
    if (_fileno(stderr) < 0) freopen("gt.log", "w", stderr);
#endif
    std::string disc = find_disc(argc, argv);
    if (disc.empty()) {
        fprintf(stderr, "usage: %s <Gran Turismo (USA).cue|.bin>\n  (or put the path in gt_disc.txt)\n", argv[0]);
        return 1;
    }
    // defaults: 4x internal resolution with sub-pixel vertices (gt.cfg / options override)
    gpu_set_scale(4);
    g_pgxp = true;
    load_settings();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_log_level = LOG_DEBUG;
        else if (!strcmp(argv[i], "--widescreen")) set_widescreen(true);
        else if (!strcmp(argv[i], "--no-widescreen")) set_widescreen(false);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) gpu_set_scale(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--pgxp")) g_pgxp = true;
        else if (!strcmp(argv[i], "--no-pgxp")) g_pgxp = false;
        else if (!strcmp(argv[i], "--dither")) gpu_set_dither(true);
        else if (!strcmp(argv[i], "--no-dither")) gpu_set_dither(false);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) exit_after_frames = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--shot") && i + 2 < argc) {
            shot_frame = strtoull(argv[++i], nullptr, 10);
            shot_path = argv[++i];
        } else if (!strcmp(argv[i], "--shot-every") && i + 2 < argc) {
            shot_every = strtoull(argv[++i], nullptr, 10);
            shot_dir = argv[++i];
        } else if (!strcmp(argv[i], "--turbo")) fast_forward = true;
        else if (!strcmp(argv[i], "--display") && i + 1 < argc) display_index = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--save-state-at") && i + 2 < argc) {
            save_at_frame = strtoull(argv[++i], nullptr, 10);
            save_at_slot = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--load-state-at") && i + 2 < argc) {
            load_at_frame = strtoull(argv[++i], nullptr, 10);
            load_at_slot = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) load_at_start = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interp")) g_pure_interp = true;
        else if (!strcmp(argv[i], "--verify")) g_verify = true;
        else if (!strcmp(argv[i], "--check-sregs")) g_check_sregs = true;
        else if (!strcmp(argv[i], "--prim-log")) g_prim_log = true;
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) parse_keys(argv[++i]);
        else if (!strcmp(argv[i], "--hash-log") && i + 1 < argc) g_hash_log = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--break-cycles") && i + 1 < argc) g_break_cycles = strtoll(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--trace") && i + 3 < argc) {
            g_trace_start = strtoll(argv[++i], nullptr, 10);
            g_trace_end = strtoll(argv[++i], nullptr, 10);
            g_trace_file = fopen(argv[++i], "w");
        }
        else if (!strcmp(argv[i], "--dump-at") && i + 2 < argc) {
            g_dump_frame = strtoll(argv[++i], nullptr, 10);
            g_dump_path = argv[++i];
        }
        else if (!strcmp(argv[i], "--watch-value") && i + 1 < argc) g_watch_value = strtoul(argv[++i], nullptr, 16);
        else if (!strcmp(argv[i], "--watch-addr") && i + 1 < argc) g_watch_addr = strtoul(argv[++i], nullptr, 16) & 0x1FFFFC;
    }
    if (!cd_open(disc)) {
        fprintf(stderr, "cannot open disc image %s\n", disc.c_str());
        return 1;
    }

    // controllers keep working when the window is not focused
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    int ndisp = SDL_GetNumVideoDisplays();
    for (int d = 0; d < ndisp; d++) {
        SDL_Rect r;
        SDL_GetDisplayBounds(d, &r);
        LOGI("display %d: %dx%d at %d,%d%s", d, r.w, r.h, r.x, r.y, d == display_index ? " (used)" : "");
    }
    int disp = display_index >= 0 && display_index < ndisp ? display_index : 0;
    window = SDL_CreateWindow("Gran Turismo (static recomp)", SDL_WINDOWPOS_CENTERED_DISPLAY(disp),
                              SDL_WINDOWPOS_CENTERED_DISPLAY(disp), 1280, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!exit_after_frames) {  // scripted test runs leave the keyboard focus alone
        SDL_RaiseWindow(window);
        SDL_SetWindowInputFocus(window);
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_AudioSpec want{}, have{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audio_dev) {
        SDL_PauseAudioDevice(audio_dev, 0);
        LOGI("audio: default device via %s, %d Hz", SDL_GetCurrentAudioDriver(), have.freq);
    } else {
        LOGW("audio: cannot open device: %s", SDL_GetError());
    }
    for (int i = 0; i < SDL_NumJoysticks(); i++) open_device(i);

    hw_init();
    cpu_init();
    gpu_init();
    spu_init();
    cd_init();
    sio_init();
    mdec_init();
    bios_init();
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, sizeof exe_path);
    state_init(exe_path);
    if (load_at_start) state_request_load(load_at_start);  // handled at the first vblank
    // the game runs on a fixed-address stack so save states can capture it (state.cpp)
    state_run([] { bios_boot(&g_cpu); });
    bios_flush_cards();
    SDL_Quit();
    return 0;
}
