#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include "syscall.h"

/*
 * BPF 移植版的 fchdir（覆盖 musl 原版 src/unistd/fchdir.c）。
 *
 * 原版在 __syscall(SYS_fchdir) 返回 -EBADF 时，fallback 到
 * syscall(SYS_chdir, "/proc/self/fd/N")。该 fallback 依赖 SYS_chdir，但本 arch
 * 不定义 __NR_chdir（chdir 1 参 path 与 fchdir 1 参 fd 共用 FCHDIR handler 会
 * 参数错位）。VM 的 do_fchdir 对有效 fd 直接成功、无效 fd 返回 -EBADF，不会触发
 * 原版的 fallback 路径，故覆盖版去掉 fallback，只发 SYS_fchdir。
 */
int fchdir(int fd)
{
    return __syscall_ret(__syscall(SYS_fchdir, fd));
}
