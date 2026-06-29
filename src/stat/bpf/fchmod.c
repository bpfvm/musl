#include <sys/stat.h>
#include <fcntl.h>
#include "syscall.h"

/*
 * BPF 移植：VM 只实现了 do_fchmodat（透传给宿主 fchmodat）。用 fchmodat 的 syscall
 * 形式配合 AT_EMPTY_PATH + 空 path 实现 fchmod（对 fd 本身操作），与 PDCLib 一致。
 *
 * 必须直接发 syscall(SYS_fchmodat, fd, "", mode, AT_EMPTY_PATH)，不能调 musl 自己的
 * fchmodat()：musl fchmodat.c 在 flag!=0 且无 SYS_fchmodat2 时，对非 AT_SYMLINK_NOFOLLOW
 * 的 flag 一律返回 EINVAL，会把 AT_EMPTY_PATH 误杀。直接 syscall 绕过这层检查，
 * 由 VM do_fchmodat 透传给宿主内核的 fchmodat（原生支持 AT_EMPTY_PATH）。
 *
 * 原版走 __syscall(SYS_fchmod, fd, mode)，但 SYS_fchmod 映射到 FCHMODAT id，
 * 2 参打到 4 参 handler 会参数错位（mode 被当 path 读 → EFAULT）。
 *
 * AT_EMPTY_PATH=0x1000（musl fcntl.h 仅在 _GNU_SOURCE/_BSD_SOURCE 下暴露，libc 内部
 * 编译用 _XOPEN_SOURCE 看不到，故直接写数值）。
 */
int fchmod(int fd, mode_t mode)
{
	return syscall(SYS_fchmodat, fd, "", mode, 0x1000);
}
