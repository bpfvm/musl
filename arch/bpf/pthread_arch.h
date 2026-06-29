#ifndef _INTERNAL_PTHREAD_ARCH_H
#define _INTERNAL_PTHREAD_ARCH_H

/*
 * BPF thread pointer（TLS）模拟。
 *
 * BPF 无 TLS 寄存器（无 fs_base/tpidr），本 arch 用一对 VM syscall 维护 thread
 * pointer：
 *   - __set_thread_area(tp) → BPF_CALL_SET_TLS：把 tp 存进 VM 字段 v->tp_
 *   - __get_tp()            → BPF_CALL_GET_TLS：从 v->tp_ 读回
 * 启动时 musl __init_tp 调前者写入 struct pthread*，之后 __pthread_self()
 * （经 __get_tp）读回同一个指针。
 *
 * TLS 模型：BELOW_TP（不定义 TLS_ABOVE_TP，与 x86_64 一致）。此时 musl 的
 * pthread_impl.h 定义 TP_ADJ(p)==(p)、__pthread_self()==(pthread_t)__get_tp()，
 * 所以写入与读回的值就是 struct pthread* 本身，无需偏移调整。
 *
 * 性能：单线程下 __get_tp 调用稀疏。stdio 的 getc/putc 热路径（getc.h:19 /
 * putc.h:19）在 f->lock==0 时短路，不触发 __pthread_self；只有显式锁/退出等
 * 少数路径才调用。故经 syscall 读取开销可忽略。
 *
 * 多线程（pthread_create）需要每线程独立 TP，当前未实现。
 */

#include <stdint.h>
#include <syscall.h>

#ifdef __ASSEMBLER__
#error "BPF musl port does not use assembler"
#else

/*
 * 通过 VM syscall 读取 thread pointer。本头被 pthread_impl.h 在 syscall.h 之后
 * include，因此 syscall_arch.h 里的 static inline __syscall1 已可见，无需再
 * 重复声明（函数内 extern 声明会被 clang 误解析为「返回函数指针」而报错）。
 */
static inline uintptr_t __get_tp()
{
    return (uintptr_t)__syscall1(BPF_CALL_GET_TLS, 0);
}

#endif

/* mcontext 中 PC 字段；BPF 无 ucontext/getcontext，仅占位以满足
 * pthread_cancel 等信号上下文相关代码的编译（这些路径在单线程 VM 下不执行，
 * 真正的 SIGCANCEL 投递未实现）。直接用数字索引 16（= x86_64 REG_RIP 在
 * gregset_t[23] 中的位置），避免依赖 bits/reg.h 的 x86 寄存器编号宏——
 * BPF arch 不提供 bits/reg.h，走 generic/bits/reg.h（空）。 */
#define MC_PC gregs[16]

#endif
