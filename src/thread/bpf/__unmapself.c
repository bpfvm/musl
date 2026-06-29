/*
 * BPF 版 __unmapself：detached 线程退出时 musl __pthread_exit 走 DT_DETACHED 分支
 * 会调 __unmapself(self->map_base, self->map_size)，意图是 munmap 掉本线程的栈映射
 * 再 SYS_exit（避免线程在自己即将被回收的栈上继续执行，故上游实现用 CRTJMP 切到
 * 一块共享小栈 shared_stack 再做 munmap + exit）。
 *
 * BPF 的难点：上游 arch/bpf/reloc.h 的 CRTJMP 是占位（__builtin_unreachable），
 * 仅在 ldso/dynld 路径被假设不可达，但 detached 线程退出路径在静态链接下会真实调到
 * __unmapself -> CRTJMP -> UB（掉进后续任意代码）。故必须为本 arch 提供本文件覆盖
 * 上游 src/thread/__unmapself.c（musl Makefile 的 ARCH_GLOBS 机制：arch 子目录下的
 * 同名源替换通用源）。
 *
 * 简化：不切栈、不 munmap，直接 SYS_exit。
 *   - 先 munmap 当前栈再继续执行本线程会掉进刚被 unmap 的栈页（bpfvm 的 do_munmap 会
 *     立即拆映射，后续访存即 fault），故「先 munmap 再 exit」在 BPF 上不可行（与上游
 *     切栈的动机一致）。
 *   - 不必 munmap：bpfvm 的 PosixSyscall::fini 在线程退出时已统一释放本 vm 的地址空间
 *     引用（maps_ptr = 空 list），线程栈映射由 VM 自动回收，不依赖 guest 侧 __unmapself
 *     的 munmap。所以这里只 SYS_exit 即可，VM 侧清理照常。
 *
 * 该函数是 _Noreturn（调用方 __pthread_exit 之后立即 for(;;) SYS_exit，不会返回）。
 */
#include <syscall.h>

void __unmapself(void *base, unsigned long size)
{
	(void)base;
	(void)size;
	for (;;) __syscall(SYS_exit);
}
