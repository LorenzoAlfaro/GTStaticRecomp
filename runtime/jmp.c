// Minimal setjmp/longjmp for the runtime's non-local control flow (interrupt return,
// CTR thread unwinding, multi-level returns). Unlike __builtin_setjmp, this saves and
// restores every callee-saved register of the Win64 ABI, including XMM6-XMM15, so
// compiled code that keeps values in those registers across calls stays correct.
// No SEH unwinding is performed (the skipped frames have nothing to clean up).
//
// Buffer layout (see rt_jmp_buf in recomp.h), offsets in bytes:
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
    "    jmpq *72(%rcx)\n");
#else
#error "rt_setjmp/rt_longjmp are implemented for Windows x86-64 only"
#endif
