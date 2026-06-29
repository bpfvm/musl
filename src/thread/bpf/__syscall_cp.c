/*
 * BPF 可取消 syscall 点（cancellation point）的 arch 实现。
 *
 * 标准 musl 设计：
 *   - pthread_cancel.c 强定义 __syscall_cp_c，调用 __syscall_cp_asm（arch 汇编）；
 *     __syscall_cp_asm 检查取消标志、标记 __cp_begin/__cp_end 区间、发 syscall。
 *   - src/thread/__syscall_cp.c（本文件的 base 版）用 weak_alias(sccp, __syscall_cp_c)
 *     提供弱版 __syscall_cp_c，并强定义 __syscall_cp 转发到 __syscall_cp_c。
 *   - __cp_begin/__cp_end/__cp_cancel 是汇编里的地址标签，pthread_cancel 的信号
 *     处理器用它们的范围判断「PC 是否落在可取消 syscall 区间」。
 *
 * 本 VM 已支持多线程（pthread_create / futex / mutex / condvar 等均已实现），
 * pthread_cancel 也能正常投递 SIGCANCEL。与标准 musl 的差异在于取消打断的方式：
 *   - 标准 musl 靠信号处理器在「PC 落在 __syscall_cp_asm 区间内」时把 PC 改写为
 *     __cp_cancel，从而在异步取消时打断正在阻塞的 syscall（打断点取消）。
 *   - 本 VM 不做这种 PC 区间改写，故可取消 syscall 的取消只能发生在 syscall
 *     返回 EINTR 之后（返回点取消）——这条路径由 pthread_cancel.c 的 __syscall_cp_c
 *     检查 self->cancel 完成判断，与本文件无关、且在多线程下有效。
 *
 * 此外 arch/bpf/syscall_arch.h + src/internal/syscall.h 末尾的折叠宏已把 libc 内部
 * 所有 __syscall_cp(nr, ...) 调用重定义为 __syscall(nr, ...)（inline、nr 保持常量，
 * 让 clang lower 出 src_reg=0 的 call）。故 __syscall_cp 函数不被内部调用。
 *
 * 本 BPF 覆盖版职责（替换 base 版 src/thread/__syscall_cp.c）：
 *   - __syscall_cp_asm：纯 C 转发 __syscall。因为 VM 不做 PC 区间改写，这里无需
 *     （也无法）检查 self->cancel——交给 __syscall_cp_c 的返回后检查即可。
 *     pthread_cancel.c 的 __syscall_cp_c 强定义会调用它。
 *   - __cp_begin/__cp_end/__cp_cancel：纯地址哨兵（被取地址、不调用），满足
 *     pthread_cancel.c 的符号引用；cancel_handler 会取它们的地址做区间比较，
 *     但因 VM 不会把 PC 跳进区间，比较恒为假，等价于「不在可取消 syscall 中」。
 *   - __syscall_cp：保留强定义（转发 __syscall_cp_c），与 src/internal/syscall.h:27
 *     的 hidden 声明对应、与 base 版符号集一致。折叠宏后不被内部调用，但保留以
 *     避免外部取地址（dlsym 等）时缺符号。省掉 base 版的 sccp/weak_alias——因为
 *     __syscall_cp_c 已由 pthread_cancel.c 强定义，不需要弱版兜底。
 */

#include "pthread_impl.h"
#include "syscall.h"

/* __syscall_cp_c 由 pthread_cancel.c 强定义（调用本文件的 __syscall_cp_asm）。 */
hidden long __syscall_cp_c(syscall_arg_t, syscall_arg_t, syscall_arg_t,
                           syscall_arg_t, syscall_arg_t, syscall_arg_t,
                           syscall_arg_t);

/* 地址哨兵：pthread_cancel.c 用它们的地址范围判断 PC 是否落在可取消区间。
 * VM 不会把 PC 跳进区间，比较恒为假；这些符号被取地址即可，不会被调用。 */
hidden const char __cp_begin[1];
hidden const char __cp_end[1];
hidden const char __cp_cancel[1];

/* __syscall_cp_asm —— 各 arch 的汇编版检查 self->cancel 后发 syscall。VM 不做
 * PC 区间改写，取消由 __syscall_cp_c 的返回后检查负责，这里直接转发即可。
 * 签名与 pthread_cancel.c 的声明一致（8 参数）。 */
long __syscall_cp_asm(volatile void *p, syscall_arg_t nr,
                      syscall_arg_t u, syscall_arg_t v, syscall_arg_t w,
                      syscall_arg_t x, syscall_arg_t y, syscall_arg_t z)
{
	(void)p;  /* VM 不在 syscall_cp_asm 内检查取消标志 */
	return __syscall(nr, u, v, w, x, y, z);
}

/* __syscall_cp —— 与 base 版一致，转发 __syscall_cp_c。折叠宏后不被内部调用，
 * 保留以匹配 hidden 声明 + 供外部取地址。 */
long (__syscall_cp)(syscall_arg_t nr,
                    syscall_arg_t u, syscall_arg_t v, syscall_arg_t w,
                    syscall_arg_t x, syscall_arg_t y, syscall_arg_t z)
{
	return __syscall_cp_c(nr, u, v, w, x, y, z);
}
