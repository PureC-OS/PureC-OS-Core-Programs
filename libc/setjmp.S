
    .text
    .globl setjmp
    .type setjmp, @function
setjmp:
    mov %rbx, 0*8(%rdi)
    mov %rbp, 1*8(%rdi)
    mov %r12, 2*8(%rdi)
    mov %r13, 3*8(%rdi)
    mov %r14, 4*8(%rdi)
    mov %r15, 5*8(%rdi)
    lea 8(%rsp), %rax
    mov %rax, 6*8(%rdi)
    mov (%rsp), %rax
    mov %rax, 7*8(%rdi)
    xor %eax, %eax
    ret

    .globl longjmp
    .type longjmp, @function
longjmp:
    mov 7*8(%rdi), %rcx
    mov %esi, %eax
    test %eax, %eax
    jne 1f
    inc %eax
1:
    mov 0*8(%rdi), %rbx
    mov 1*8(%rdi), %rbp
    mov 2*8(%rdi), %r12
    mov 3*8(%rdi), %r13
    mov 4*8(%rdi), %r14
    mov 5*8(%rdi), %r15
    mov 6*8(%rdi), %rsp
    jmp *%rcx
