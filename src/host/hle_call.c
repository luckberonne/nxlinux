#include "hle_call.h"

// void hle_call(void *fn /* rdi */, hle_regs_t *r /* rsi */)
// Layout of hle_regs_t: x[8] at +0, d[8] at +64.
__asm__(
    ".text\n"
    ".globl hle_call\n"
    ".type hle_call,@function\n"
    "hle_call:\n"
    "    push %rbp\n"
    "    mov %rsp, %rbp\n"
    "    push %rbx\n"
    "    push %r12\n"                    // rsp is 16-byte aligned here
    "    mov %rsi, %rbx\n"
    "    mov %rdi, %r11\n"
    "    push 56(%rbx)\n"                // 8th integer argument on the stack
    "    push 48(%rbx)\n"                // 7th
    "    movq 64(%rbx), %xmm0\n"
    "    movq 72(%rbx), %xmm1\n"
    "    movq 80(%rbx), %xmm2\n"
    "    movq 88(%rbx), %xmm3\n"
    "    movq 96(%rbx), %xmm4\n"
    "    movq 104(%rbx), %xmm5\n"
    "    movq 112(%rbx), %xmm6\n"
    "    movq 120(%rbx), %xmm7\n"
    "    mov 0(%rbx), %rdi\n"
    "    mov 8(%rbx), %rsi\n"
    "    mov 16(%rbx), %rdx\n"
    "    mov 24(%rbx), %rcx\n"
    "    mov 32(%rbx), %r8\n"
    "    mov 40(%rbx), %r9\n"
    "    mov $8, %eax\n"
    "    call *%r11\n"
    "    mov %rax, 0(%rbx)\n"
    "    movq %xmm0, 64(%rbx)\n"
    "    lea -16(%rbp), %rsp\n"
    "    pop %r12\n"
    "    pop %rbx\n"
    "    pop %rbp\n"
    "    ret\n"
    ".size hle_call, .-hle_call\n");
