/*
 * BPF arch 覆盖：让 musl 的 accept() 直接发 SYS_accept4（flags=0），与
 * accept4(flg=0) 路径合并。这样 VM 只需实现 do_accept4，不必实现 do_accept。
 *
 * 原版 musl/src/network/accept.c 发 SYS_accept（socketcall_cp(accept,...)），
 * accept4.c 在 flg=0 时也回调 accept()，故 SYS_accept 是 accept4 的依赖项。
 * 本覆盖用 socketcall_cp(accept4, fd, addr, len, 0, 0, 0) 把依赖消除：
 *   - accept() → SYS_accept4(flags=0)
 *   - accept4(flg=0) → accept() → SYS_accept4(flags=0)
 *   - accept4(flg!=0) → SYS_accept4(flags=flg)
 * 三条路径统一到 SYS_accept4。
 *
 * ARCH_GLOBS（musl/Makefile 行 23）匹配 src/network/bpf/*.[csS]，REPLACED_OBJS
 * 把 obj/src/network/bpf/accept.o 换算成 obj/src/network/accept.o，filter-out
 * 从 BASE_SRCS 去掉原 accept.c，自动生效，无需改 build.sh。
 */
#include <sys/socket.h>
#include "syscall.h"

int accept(int fd, struct sockaddr *restrict addr, socklen_t *restrict len)
{
    return socketcall_cp(accept4, fd, addr, len, 0, 0, 0);
}
