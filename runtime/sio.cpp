// SIO0: controller port serial interface (pads). Memory cards are handled by the HLE BIOS,
// so the card address (0x81) is left unanswered here.
#include <algorithm>

#include "psx.h"

namespace psx {

PadState g_pads[2];

static uint16_t ctrl = 0, mode = 0, baud = 0x88;
static uint8_t rx = 0xFF;
static bool rx_full = false;
static bool tx_busy = false;
static bool irq_flag = false;
static bool ack_level = false;
static int step[2] = {0, 0};
static bool device_active[2] = {false, false};  // this transfer sequence addressed a pad
static bool pending_ack = false;
static uint8_t pending_rx = 0xFF;
static enum { SIO_IDLE, SIO_XFER, SIO_ACK } sio_state = SIO_IDLE;

static int port() { return (ctrl >> 13) & 1; }

// returns response byte, sets *ack if the device acknowledges (more bytes follow)
static uint8_t pad_exchange(int p, uint8_t tx, bool* ack) {
    PadState& pad = g_pads[p];
    *ack = false;
    int s = step[p]++;
    if (s == 0) {
        if (tx == 0x01 && pad.connected) {
            device_active[p] = true;
            *ack = true;
            return 0xFF;
        }
        device_active[p] = false;
        return 0xFF;
    }
    if (!device_active[p]) return 0xFF;
    uint8_t id = pad.analog ? 0x73 : 0x41;
    int nbytes = pad.analog ? 9 : 5;
    uint8_t r = 0xFF;
    switch (s) {
    case 1: r = id; break;
    case 2: r = 0x5A; break;
    case 3: r = pad.buttons & 0xFF; break;
    case 4: r = pad.buttons >> 8; break;
    case 5: r = pad.rx; break;
    case 6: r = pad.ry; break;
    case 7: r = pad.lx; break;
    case 8: r = pad.ly; break;
    default: return 0xFF;
    }
    *ack = s < nbytes - 1;
    return r;
}

static void ev_sio(int64_t t) {
    if (sio_state == SIO_XFER) {
        tx_busy = false;
        rx = pending_rx;
        rx_full = true;
        if (pending_ack) {
            sio_state = SIO_ACK;
            ack_level = true;
            schedule(EV_SIO, t + 100);
        } else {
            sio_state = SIO_IDLE;
        }
        return;
    }
    if (sio_state == SIO_ACK) {
        ack_level = false;
        sio_state = SIO_IDLE;
        if (ctrl & 0x1000) {  // DSR (ACK) interrupt enable
            irq_flag = true;
            irq_raise(IRQ_SIO0);
        }
    }
}

uint32_t sio_read(uint32_t off, int size) {
    switch (off) {
    case 0x0: {
        uint8_t v = rx_full ? rx : 0xFF;
        rx_full = false;
        if (size == 4) return v | 0xFFFFFF00u;
        if (size == 2) return v | 0xFF00u;
        return v;
    }
    case 0x4: {
        uint32_t s = 0;
        if (!tx_busy) s |= 1 | 4;
        if (rx_full) s |= 2;
        if (ack_level) s |= 0x80;
        if (irq_flag) s |= 0x200;
        return s;
    }
    case 0x8: return mode;
    case 0xA: return ctrl;
    case 0xE: return baud;
    }
    return 0;
}

void sio_write(uint32_t off, uint32_t v, int) {
    switch (off) {
    case 0x0: {
        if (!(ctrl & 2)) { rx = 0xFF; rx_full = true; return; }  // not selected
        bool ack;
        pending_rx = pad_exchange(port(), (uint8_t)v, &ack);
        pending_ack = ack;
        tx_busy = true;
        sio_state = SIO_XFER;
        int64_t bit = (int64_t)baud * ((mode & 3) == 2 ? 16 : (mode & 3) == 3 ? 64 : 1);
        schedule(EV_SIO, now() + std::max<int64_t>(bit * 8, 256));
        break;
    }
    case 0x8: mode = (uint16_t)v; break;
    case 0xA:
        if (v & 0x40) {  // reset
            ctrl = 0; mode = 0; rx_full = false; tx_busy = false; irq_flag = false;
            step[0] = step[1] = 0;
            sio_state = SIO_IDLE;
            unschedule(EV_SIO);
            return;
        }
        if (v & 0x10) irq_flag = false;  // acknowledge
        if (!(v & 2)) {                  // deselect: end of the transfer sequence
            step[0] = step[1] = 0;
            device_active[0] = device_active[1] = false;
        } else if (((ctrl ^ v) & 0x2000) != 0) {
            step[0] = step[1] = 0;
        }
        ctrl = (uint16_t)(v & ~0x50u);
        break;
    case 0xE: baud = (uint16_t)v; break;
    }
}

void sio_init() {
    register_event(EV_SIO, ev_sio);
    g_pads[1].connected = false;
}

}  // namespace psx
