#include <unistd.h>
#include <fcntl.h>
#include "syscall.h"

/*
 * BPF 移植：VM 只实现了 do_execveat，未实现 do_execve（二者参数个数不同：execve
 * 3 参 vs execveat 5 参，共用 handler 会参数错位，同 fchmod/fchmodat 的情况）。
 * 用 execveat(AT_FDCWD, path, argv, envp, 0) 实现 execve，语义等价。
 *
 * 原版走 syscall(SYS_execve, path, argv, envp)，但 SYS_execve 在 bpf 的
 * bits/syscall.h 中未定义（同第 4 类旧版别名：stat/lstat/fchmod 等），故本覆盖版
 * 改发 SYS_execveat。
 */
int execve(const char *path, char *const argv[], char *const envp[])
{
	return syscall(SYS_execveat, AT_FDCWD, path, argv, envp, 0);
}
