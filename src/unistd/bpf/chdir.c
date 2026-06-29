#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "syscall.h"

/*
 * BPF 移植：VM 只实现了 fchdir(fd)，没有 chdir(path) syscall。
 * 用 open(path) + fchdir(fd) + close(fd) 组合实现 chdir（与 PDCLib 一致）。
 * 原版走 syscall(SYS_chdir, path)，但 SYS_chdir 在本 arch 无对应 VM handler。
 */
int chdir(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if(fd < 0) {
		return -1;
	}
	int rc = fchdir(fd);
	int saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return rc;
}
