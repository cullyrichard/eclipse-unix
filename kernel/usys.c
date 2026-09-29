#include "usys.h"
#include "ksyscall.h"

/* The one function that issues SYC -- examples/ulib.c's __syscall(),
 * unchanged in mechanism: every value crosses the asm boundary through a
 * named global, never an "r" operand, and AC2/AC3 (the live frame
 * pointer and the caller's AC3) are saved/restored around SYC because
 * staging arg2 in AC2 clobbers the frame pointer. See examples/ulib.c's
 * comments for the full reasoning. */
static int __sys_reason;
static int __sys_arg1;
static int __sys_arg2;
static int __sys_ret;
static unsigned int __sys_save2;
static unsigned int __sys_save3;

static int __syscall(int reason, int arg1, int arg2) {
    __sys_reason = reason;
    __sys_arg1 = arg1;
    __sys_arg2 = arg2;
    __sys_save2 = 0; /* keep storage alive through globaldce */
    __sys_save3 = 0;
    asm volatile(
        "ESTA 2,__sys_save2,0\n\t"
        "ESTA 3,__sys_save3,0\n\t"
        "ELDA 0,__sys_reason,0\n\t"
        "ELDA 1,__sys_arg1,0\n\t"
        "ELDA 2,__sys_arg2,0\n\t"
        "SYC 1,1\n\t"
        "ESTA 0,__sys_ret,0\n\t"
        "ELDA 2,__sys_save2,0\n\t"
        "ELDA 3,__sys_save3,0\n\t"
    );
    return __sys_ret;
}

int sys_putchar(int c)    { return __syscall(SYS_PUTCHAR, c, 0); }
int sys_getchar(void)     { return __syscall(SYS_GETCHAR, 0, 0); }
void sys_exit(int status) { __syscall(SYS_EXIT, status, 0); }
int sys_getpid(void)      { return __syscall(SYS_GETPID, 0, 0); }
int sys_write(const char *buf, int len) {
    return __syscall(SYS_WRITE, (int)buf, len);
}
