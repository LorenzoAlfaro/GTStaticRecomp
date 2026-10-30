// Minimal setjmp/longjmp for the runtime's non-local control flow (interrupt return,
// Exec, multi-level returns, save states). Unlike __builtin_setjmp, this saves and
// restores every callee-saved register of the host ABI (Win64: including XMM6-XMM15;
// AArch64: d8-d15; ARM: d8-d15), so compiled code that keeps values in those registers
// across calls stays correct. No unwinding is performed (the skipped frames have nothing
// to clean up). rt_run_on_stack runs a function on another stack (state.cpp).
//
// Win64 buffer layout (see rt_jmp_buf in recomp.h), offsets in bytes:
//   0 rbx, 8 rbp, 16 rdi, 24 rsi, 32 r12, 40 r13, 48 r14, 56 r15, 64 rsp, 72 rip,
//   80 mxcsr/fpcw, 96..255 xmm6..xmm15
#if defined(_WIN64) && defined(__x86_64__)
__asm__(
    ".text\n"
    ".globl rt_setjmp\n"
    ".def rt_setjmp; .scl 2; .type 32; .endef\n"
    "rt_setjmp:\n"
    "    movq %rbx, 0(%rcx)\n"
    "    movq %rbp, 8(%rcx)\n"
    "    movq %rdi, 16(%rcx)\n"
    "    movq %rsi, 24(%rcx)\n"
    "    movq %r12, 32(%rcx)\n"
    "    movq %r13, 40(%rcx)\n"
    "    movq %r14, 48(%rcx)\n"
    "    movq %r15, 56(%rcx)\n"
    "    leaq 8(%rsp), %rdx\n"
    "    movq %rdx, 64(%rcx)\n"
    "    movq (%rsp), %rdx\n"
    "    movq %rdx, 72(%rcx)\n"
    "    stmxcsr 80(%rcx)\n"
    "    fnstcw 84(%rcx)\n"
    "    movdqu %xmm6, 96(%rcx)\n"
    "    movdqu %xmm7, 112(%rcx)\n"
    "    movdqu %xmm8, 128(%rcx)\n"
    "    movdqu %xmm9, 144(%rcx)\n"
    "    movdqu %xmm10, 160(%rcx)\n"
    "    movdqu %xmm11, 176(%rcx)\n"
    "    movdqu %xmm12, 192(%rcx)\n"
    "    movdqu %xmm13, 208(%rcx)\n"
    "    movdqu %xmm14, 224(%rcx)\n"
    "    movdqu %xmm15, 240(%rcx)\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    ".globl rt_longjmp\n"
    ".def rt_longjmp; .scl 2; .type 32; .endef\n"
    "rt_longjmp:\n"
    "    movl %edx, %eax\n"
    "    testl %eax, %eax\n"
    "    jnz 1f\n"
    "    incl %eax\n"
    "1:\n"
    "    movq 0(%rcx), %rbx\n"
    "    movq 8(%rcx), %rbp\n"
    "    movq 16(%rcx), %rdi\n"
    "    movq 24(%rcx), %rsi\n"
    "    movq 32(%rcx), %r12\n"
    "    movq 40(%rcx), %r13\n"
    "    movq 48(%rcx), %r14\n"
    "    movq 56(%rcx), %r15\n"
    "    ldmxcsr 80(%rcx)\n"
    "    fldcw 84(%rcx)\n"
    "    movdqu 96(%rcx), %xmm6\n"
    "    movdqu 112(%rcx), %xmm7\n"
    "    movdqu 128(%rcx), %xmm8\n"
    "    movdqu 144(%rcx), %xmm9\n"
    "    movdqu 160(%rcx), %xmm10\n"
    "    movdqu 176(%rcx), %xmm11\n"
    "    movdqu 192(%rcx), %xmm12\n"
    "    movdqu 208(%rcx), %xmm13\n"
    "    movdqu 224(%rcx), %xmm14\n"
    "    movdqu 240(%rcx), %xmm15\n"
    "    movq 64(%rcx), %rsp\n"
    "    jmpq *72(%rcx)\n"
    // rt_run_on_stack(top, fn): call fn() with rsp = top (16-byte aligned), then return on
    // the original stack. Used to run the game on a fixed-address stack (save states).
    ".globl rt_run_on_stack\n"
    ".def rt_run_on_stack; .scl 2; .type 32; .endef\n"
    "rt_run_on_stack:\n"
    "    pushq %rbp\n"
    "    movq %rsp, %rbp\n"
    "    movq %rcx, %rsp\n"
    "    subq $32, %rsp\n"
    "    callq *%rdx\n"
    "    movq %rbp, %rsp\n"
    "    popq %rbp\n"
    "    ret\n");
#define RT_JMP_SP_OFFSET 64
#elif defined(__x86_64__) && defined(__ELF__)
// x86-64 System V (Linux / Android, e.g. the Android emulator). Callee-saved: rbx, rbp,
// r12-r15, rsp; no vector registers. Same offsets as the Win64 layout (rdi/rsi unused):
//   0 rbx, 8 rbp, 32 r12, 40 r13, 48 r14, 56 r15, 64 rsp, 72 rip, 80 mxcsr/fpcw
__asm__(
    ".text\n"
    ".globl rt_setjmp\n"
    ".type rt_setjmp, @function\n"
    "rt_setjmp:\n"
    "    movq %rbx, 0(%rdi)\n"
    "    movq %rbp, 8(%rdi)\n"
    "    movq %r12, 32(%rdi)\n"
    "    movq %r13, 40(%rdi)\n"
    "    movq %r14, 48(%rdi)\n"
    "    movq %r15, 56(%rdi)\n"
    "    leaq 8(%rsp), %rdx\n"
    "    movq %rdx, 64(%rdi)\n"
    "    movq (%rsp), %rdx\n"
    "    movq %rdx, 72(%rdi)\n"
    "    stmxcsr 80(%rdi)\n"
    "    fnstcw 84(%rdi)\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    ".size rt_setjmp, .-rt_setjmp\n"
    ".globl rt_longjmp\n"
    ".type rt_longjmp, @function\n"
    "rt_longjmp:\n"
    "    movl %esi, %eax\n"
    "    testl %eax, %eax\n"
    "    jnz 1f\n"
    "    incl %eax\n"
    "1:\n"
    "    movq 0(%rdi), %rbx\n"
    "    movq 8(%rdi), %rbp\n"
    "    movq 32(%rdi), %r12\n"
    "    movq 40(%rdi), %r13\n"
    "    movq 48(%rdi), %r14\n"
    "    movq 56(%rdi), %r15\n"
    "    ldmxcsr 80(%rdi)\n"
    "    fldcw 84(%rdi)\n"
    "    movq 64(%rdi), %rsp\n"
    "    jmpq *72(%rdi)\n"
    ".size rt_longjmp, .-rt_longjmp\n"
    ".globl rt_run_on_stack\n"
    ".type rt_run_on_stack, @function\n"
    "rt_run_on_stack:\n"
    "    pushq %rbp\n"
    "    movq %rsp, %rbp\n"
    "    movq %rdi, %rsp\n"
    "    callq *%rsi\n"
    "    movq %rbp, %rsp\n"
    "    popq %rbp\n"
    "    ret\n"
    ".size rt_run_on_stack, .-rt_run_on_stack\n");
#define RT_JMP_SP_OFFSET 64
#elif defined(__aarch64__) && defined(__ELF__)
// AArch64 (Linux / Android). Callee-saved per AAPCS64: x19-x28, x29 (fp), x30 (lr), sp,
// and the low 64 bits of v8-v15 (d8-d15). x18 is the platform register (Android's shadow
// call stack) and is left alone. Buffer layout, offsets in bytes:
//   0 x19..x28, 80 x29, 88 x30, 96 sp, 112 d8..d15, 176 fpcr
__asm__(
    ".text\n"
    ".globl rt_setjmp\n"
    ".type rt_setjmp, %function\n"
    "rt_setjmp:\n"
    "    stp x19, x20, [x0, #0]\n"
    "    stp x21, x22, [x0, #16]\n"
    "    stp x23, x24, [x0, #32]\n"
    "    stp x25, x26, [x0, #48]\n"
    "    stp x27, x28, [x0, #64]\n"
    "    stp x29, x30, [x0, #80]\n"
    "    mov x2, sp\n"
    "    str x2, [x0, #96]\n"
    "    stp d8, d9, [x0, #112]\n"
    "    stp d10, d11, [x0, #128]\n"
    "    stp d12, d13, [x0, #144]\n"
    "    stp d14, d15, [x0, #160]\n"
    "    mrs x2, fpcr\n"
    "    str x2, [x0, #176]\n"
    "    mov w0, #0\n"
    "    ret\n"
    ".size rt_setjmp, .-rt_setjmp\n"
    ".globl rt_longjmp\n"
    ".type rt_longjmp, %function\n"
    "rt_longjmp:\n"
    "    ldp x19, x20, [x0, #0]\n"
    "    ldp x21, x22, [x0, #16]\n"
    "    ldp x23, x24, [x0, #32]\n"
    "    ldp x25, x26, [x0, #48]\n"
    "    ldp x27, x28, [x0, #64]\n"
    "    ldp x29, x30, [x0, #80]\n"
    "    ldr x2, [x0, #96]\n"
    "    mov sp, x2\n"
    "    ldp d8, d9, [x0, #112]\n"
    "    ldp d10, d11, [x0, #128]\n"
    "    ldp d12, d13, [x0, #144]\n"
    "    ldp d14, d15, [x0, #160]\n"
    "    ldr x2, [x0, #176]\n"
    "    msr fpcr, x2\n"
    "    cmp w1, #0\n"
    "    csinc w0, w1, wzr, ne\n"  // return val, or 1 if val == 0
    "    br x30\n"
    ".size rt_longjmp, .-rt_longjmp\n"
    ".globl rt_run_on_stack\n"
    ".type rt_run_on_stack, %function\n"
    "rt_run_on_stack:\n"
    "    stp x29, x30, [sp, #-16]!\n"
    "    mov x29, sp\n"
    "    mov sp, x0\n"
    "    blr x1\n"
    "    mov sp, x29\n"
    "    ldp x29, x30, [sp], #16\n"
    "    ret\n"
    ".size rt_run_on_stack, .-rt_run_on_stack\n");
#define RT_JMP_SP_OFFSET 96
#elif defined(__arm__) && defined(__ELF__)
// 32-bit ARM (armeabi-v7a, e.g. Android TVs). Callee-saved per AAPCS: r4-r11, sp, lr, and
// VFP d8-d15. Assembled in ARM mode; callers in Thumb reach it through the linker's
// interworking, and `bx` returns to either mode. Buffer layout, offsets in bytes:
//   0 r4..r11, 32 sp, 36 lr, 48 d8..d15, 112 fpscr
__asm__(
    ".text\n"
    ".syntax unified\n"
    ".arm\n"
    ".align 2\n"
    ".globl rt_setjmp\n"
    ".type rt_setjmp, %function\n"
    "rt_setjmp:\n"
    "    stmia r0, {r4-r11}\n"
    "    str sp, [r0, #32]\n"
    "    str lr, [r0, #36]\n"
    "    add r1, r0, #48\n"
    "    vstmia r1, {d8-d15}\n"
    "    vmrs r2, fpscr\n"
    "    str r2, [r0, #112]\n"
    "    mov r0, #0\n"
    "    bx lr\n"
    ".size rt_setjmp, .-rt_setjmp\n"
    ".globl rt_longjmp\n"
    ".type rt_longjmp, %function\n"
    "rt_longjmp:\n"
    "    ldmia r0, {r4-r11}\n"
    "    ldr sp, [r0, #32]\n"
    "    ldr lr, [r0, #36]\n"
    "    add r2, r0, #48\n"
    "    vldmia r2, {d8-d15}\n"
    "    ldr r2, [r0, #112]\n"
    "    vmsr fpscr, r2\n"
    "    movs r0, r1\n"
    "    moveq r0, #1\n"  // return val, or 1 if val == 0
    "    bx lr\n"
    ".size rt_longjmp, .-rt_longjmp\n"
    ".globl rt_run_on_stack\n"
    ".type rt_run_on_stack, %function\n"
    "rt_run_on_stack:\n"
    "    push {r4, lr}\n"
    "    mov r4, sp\n"
    "    mov sp, r0\n"
    "    blx r1\n"
    "    mov sp, r4\n"
    "    pop {r4, pc}\n"
    ".size rt_run_on_stack, .-rt_run_on_stack\n");
#define RT_JMP_SP_OFFSET 32
#else
#error "rt_setjmp/rt_longjmp are implemented for x86-64 (Win64, System V), AArch64 and ARM ELF only"
#endif

#include <stdint.h>
#include <string.h>

// stack pointer saved in a jump buffer (where a saved stack begins, state.cpp)
uintptr_t rt_jmp_sp(const uint64_t* buf) {
    uintptr_t sp;
    memcpy(&sp, (const char*)buf + RT_JMP_SP_OFFSET, sizeof sp);
    return sp;
}
