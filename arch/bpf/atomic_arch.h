#ifndef _INTERNAL_ATOMIC_ARCH_H
#define _INTERNAL_ATOMIC_ARCH_H

/*
 * BPF 原子原语 —— 用 __sync builtin 生成 BPF 原子指令（lock 前缀）。
 *
 * 单线程时期曾用纯 C 非原子实现（"VM 单核单线程，无需原子"）。引入多线程
 * （clone CLONE_THREAD）后必须真原子，否则 musl __lock 的 cas 循环状态被
 * 破坏，mutex/thread_list_lock 死锁。__sync_val_compare_and_swap 生成
 * BPF cmpxchg，__sync_fetch_and_add 生成 `lock *(u*)(p) += v`，VM 的
 * BPF_ATOMIC 路径正确执行。
 */

#include <stdint.h>

#define a_cas a_cas
static inline int a_cas(volatile int *p, int t, int s)
{
    return __sync_val_compare_and_swap(p, t, s);
}

#define a_cas_p a_cas_p
static inline void *a_cas_p(volatile void *p, void *t, void *s)
{
    return __sync_val_compare_and_swap((void *volatile *)p, t, s);
}

#define a_swap a_swap
static inline int a_swap(volatile int *p, int v)
{
    /* __sync_lock_test_and_set 在 BPF 上可能生成不支持的 xchg；用 cmpxchg 循环
     * 实现 swap：原子地写入 v，返回旧值。 */
    int old;
    do { old = *p; } while (__sync_val_compare_and_swap(p, old, v) != old);
    return old;
}

#define a_fetch_add a_fetch_add
static inline int a_fetch_add(volatile int *p, int v)
{
    return __sync_fetch_and_add(p, v);
}

#define a_fetch_and a_fetch_and
static inline int a_fetch_and(volatile int *p, int v)
{
    return __sync_fetch_and_and(p, v);
}

#define a_fetch_or a_fetch_or
static inline int a_fetch_or(volatile int *p, int v)
{
    return __sync_fetch_and_or(p, v);
}

#define a_and a_and
static inline void a_and(volatile int *p, int v)
{
    int old;
    do { old = *p; } while (__sync_val_compare_and_swap(p, old, old & v) != old);
}

#define a_or a_or
static inline void a_or(volatile int *p, int v)
{
    int old;
    do { old = *p; } while (__sync_val_compare_and_swap(p, old, old | v) != old);
}

#define a_and_64 a_and_64
static inline void a_and_64(volatile uint64_t *p, uint64_t v)
{
    uint64_t old;
    do { old = *p; } while (__sync_val_compare_and_swap(p, old, old & v) != old);
}

#define a_or_64 a_or_64
static inline void a_or_64(volatile uint64_t *p, uint64_t v)
{
    uint64_t old;
    do { old = *p; } while (__sync_val_compare_and_swap(p, old, old | v) != old);
}

#define a_inc a_inc
static inline void a_inc(volatile int *p)
{
    __sync_fetch_and_add(p, 1);
}

#define a_dec a_dec
static inline void a_dec(volatile int *p)
{
    __sync_fetch_and_sub(p, 1);
}

#define a_store a_store
static inline void a_store(volatile int *p, int x)
{
    /* release 语义：store 前的 memory 操作不重排到 store 后。compiler barrier
     * + volatile store 足够（host 内存模型保证多核可见性）。 */
    __asm__ __volatile__("" : : : "memory");
    *p = x;
}

#define a_barrier a_barrier
static inline void a_barrier()
{
    /* BPF 后端不支持 AtomicFence（__sync_synchronize 会 ISel 失败）。单核 guest
     * 不需要 hardware fence；多线程下 host 线程切换提供内存序保证。compiler
     * barrier 阻止编译器重排即可。 */
    __asm__ __volatile__("" : : : "memory");
}

#define a_spin a_spin
static inline void a_spin()
{
    __asm__ __volatile__("" : : : "memory");
}

#define a_crash a_crash
static inline void a_crash()
{
    *(volatile char *)0 = 0;
}

/* ctz/clz 用 clang 内建，避免依赖 BPF 上不存在的位扫描指令。 */
#define a_ctz_32 a_ctz_32
static inline int a_ctz_32(uint32_t x)
{
    return x ? __builtin_ctz(x) : 32;
}

#define a_clz_32 a_clz_32
static inline int a_clz_32(uint32_t x)
{
    return x ? __builtin_clz(x) : 32;
}

#define a_ctz_64 a_ctz_64
static inline int a_ctz_64(uint64_t x)
{
    return x ? __builtin_ctzll(x) : 64;
}

#define a_clz_64 a_clz_64
static inline int a_clz_64(uint64_t x)
{
    return x ? __builtin_clzll(x) : 64;
}

#endif
