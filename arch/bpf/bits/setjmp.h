/*
 * BPF jmp_buf 布局与 sigsetjmp 宏定义。
 *
 * sigsetjmp 由 bpfvm 的 syscall 实现，setjmp/_setjmp 均由libc封装
 *
 * sigsetjmp 必须在调用点内联，不能包成普通函数。
 *
 * 原因是调用帧：BPF VM 用 r10(sp) 上的链表管理函数调用帧，每次
 * `call <偏移>`（src_reg=1，BPF-to-BPF）push 一帧。若把 sigsetjmp 包成函数，
 * do_sigsetjmp 保存到的 r10 是该函数内部的 sp，而非调用者调用 sigsetjmp 时的
 * sp——于是在 sigsetjmp 与调用者之间多了一层 VM 帧。siglongjmp 只能恢复到这层
 * 多余的帧，pop 出的返回地址与 sigsetjmp 首次返回时不是同一处，栈彻底错乱，
 * sigsetjmp 的「第二次返回」与实际执行流不一致（dash 的 exit 经
 * exraise→longjmp 因此退出失败）。
 *
 * 宏方案直接把 BPF_CALL_SIGSETJMP 的 call 展开到调用者指令流里（src_reg=0），
 * 不 push 任何 VM 帧，do_sigsetjmp 保存的就是调用者的真实 sp，siglongjmp 完整
 * 恢复调用者现场。
 *
 * 本头是公共头（用户代码经 <setjmp.h> 引入），不能依赖 src/internal 的
 * syscall_arch.h，故这里自带一个与 __syscall2 同形的 static inline，而不是
 * 复用 __syscall2。
 */
typedef unsigned long __jmp_buf[8];

#include <syscall.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wint-conversion"

static inline long __bpf_sigsetjmp_syscall(long call, long env, long savemask)
{
    return ((long (*)(long, long))call)(env, savemask);
}

#pragma GCC diagnostic pop

/* sigsetjmp：单次 syscall 同时存寄存器现场与（savemask!=0 时）信号掩码。
 *   setjmp(env)  == sigsetjmp(env, 1)   总是保存掩码（与 glibc/musl 主线 setjmp 一致）
 *   _setjmp(env) == sigsetjmp(env, 0)   不保存掩码
 * 两者同样在调用点内联展开。 */
#define sigsetjmp(env, saves) \
    ((int)__bpf_sigsetjmp_syscall(BPF_CALL_SIGSETJMP, (long)(env), (saves)))
#define setjmp(env)  sigsetjmp(env, 1)
#define _setjmp(env) sigsetjmp(env, 0)
