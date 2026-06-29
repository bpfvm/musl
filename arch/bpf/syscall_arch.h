#ifndef _INTERNAL_SYSCALL_ARCH_H
#define _INTERNAL_SYSCALL_ARCH_H

/*
 * BPF syscall 调用约定对接。
 *
 * musl 内部所有 syscall 走 `__syscall(SYS_xxx, args...)`，期望返回 long。
 * 本 arch 下，bits/syscall.h.in 已把每个 SYS_xxx 直接定义成 VM 的
 * BPF_CALL_xxx 值（见 include/bpf_syscall.h，形如 0x10000 + id）。
 *
 * 关键技巧（与 PDCLib 平台层一致）：把 call id 当作函数指针直接调用。clang 在
 * BPF target 上会把 `((long(*)(long))id)(arg)` lower 成一条 `call <id-imm>`
 * 指令（src_reg=0 的 syscall 形式）。VM 的 do_syscall() 拦截该 call，按
 * BPF_CALL_TO_ID 分发到对应 do_xxx。
 *
 * 因此 `n`（第一个参数）既是 syscall 号，也是要 lower 进 call 指令的 imm。
 * 后续参数按 BPF 调用约定放进 r1..r5（>5 个由 BpfWideArgs pass 打包，见
 * src/passes/BpfWideArgs.cpp），返回值在 r0。这与 musl 期望的 long 返回一致。
 *
 * 注意：musl 的 __syscallN 第一个参数是 syscall 号 n，而非 PDCLib 的 call；
 * 因此这里的 inline 函数签名是 (long n, long a1, ...)，与 x86_64 版一致。
 */

/* BPF 是 64 位、统一整数 ABI，long/指针/off_t 都是 64 位，无需拆分。 */
#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wint-conversion"

static inline long __syscall0(long n)
{
    return ((long (*)(void))n)();
}

static inline long __syscall1(long n, long a)
{
    return ((long (*)(long))n)(a);
}

static inline long __syscall2(long n, long a, long b)
{
    return ((long (*)(long, long))n)(a, b);
}

static inline long __syscall3(long n, long a, long b, long c)
{
    return ((long (*)(long, long, long))n)(a, b, c);
}

static inline long __syscall4(long n, long a, long b, long c, long d)
{
    return ((long (*)(long, long, long, long))n)(a, b, c, d);
}

static inline long __syscall5(long n, long a, long b, long c, long d, long e)
{
    return ((long (*)(long, long, long, long, long))n)(a, b, c, d, e);
}

static inline long __syscall6(long n, long a, long b, long c, long d, long e, long f)
{
    return ((long (*)(long, long, long, long, long, long))n)(a, b, c, d, e, f);
}

#pragma GCC diagnostic pop

#endif
