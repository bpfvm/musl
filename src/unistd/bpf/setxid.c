#include <unistd.h>
#include "syscall.h"

/*
 * BPF 移植版的 __setxid（覆盖 musl 原版 src/unistd/setxid.c）。
 *
 * 原版经 __synccall 把回调 do_setxid 广播到所有线程。do_setxid 里
 *     __syscall(c->nr, c->id, c->eid, c->sid);
 * 的 c->nr 来自结构体字段，是运行时值。本 arch 下 __syscall(n,...) 展开成
 * ((long(*)(long...))n)(...)，n 是编译期常量时 clang lower 成 `call imm`
 * （src_reg=0 的 syscall 形式，VM 的 do_syscall 拦截）；n 是运行时值时只能
 * lower 成 `call rX`（BPF_X 间接调用，src_reg=1）——该形式在 VM 里被当成
 * "跳转到 rX 指向的地址"的普通函数调用（push_frame + pc=r(dst)），而非
 * syscall，会跳到未定义地址崩溃。故原版 synccall 回调路径在本 arch 不可用。
 *
 * 覆盖版绕开 __synccall，直接在当前线程同步执行常量 syscall。当前所有
 * __NR_setuid/setgid/... 均映射到 BPF_CALL_BASE 占位（VM 未实现对应 handler，
 * do_syscall default 分支返回 -ENOSYS）；后续 VM 实现 BPF_CALL_SET* 后，把
 * bits/syscall.h.in 里各 __NR_set* 改成真实 id 即可自动生效（届时建议把下面
 * 单一调用改回 switch，每个 case 调对应的 __syscall(SYS_setxxx,...)，让 clang
 * 把每个调用点都 lower 成独立的 call imm）。
 *
 * 单线程语义：不广播到其它线程。本 arch 的多线程场景不要求 POSIX 凭据一致性。
 */
int __setxid(int nr, int id, int eid, int sid)
{
    (void)nr;
    (void)id;
    (void)eid;
    (void)sid;
    /* 任取一个 SYS_set* 常量（当前都是 BPF_CALL_BASE 占位）。nr 是运行时值
     * 不能直接用，故忽略入参，发常量 syscall 取 VM 的统一 -ENOSYS。 */
    long ret = __syscall(SYS_setuid, id, eid, sid);
    return __syscall_ret(ret);
}
