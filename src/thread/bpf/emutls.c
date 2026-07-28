/*
 * emutls 运行时——为 C++ thread_local 提供每线程副本存储。机制见 README「模拟
 * TLS (emutls)」与 src/passes/BpfEmutls.cpp。
 *
 * 调用约定：pass 发出 `call ptr @__emutls_get_address(ptr ctrl)`（普通函数，
 * 走 PLT），返回本线程该变量的副本地址。
 *
 * 每线程存储：用一个 pthread_key 把本线程的副本指针数组挂到 struct pthread::tsd[]。
 * 数组 base[0] 存容量（cap），base[1..] 按 index 存副本指针（一块 malloc 管全部，
 * destructor 一次 free）。
 *
 * 控制块 index 的懒分配（emutls_get_index）用 double-checked 自旋锁保证同一变量
 * 只分到一个 index——多线程同时首次访问同一变量不会各自分配并交叉写回不同 index。
 * 由 test/test_cpp_tls_race.cpp 覆盖。
 *
 * 非平凡析构：控制块 dtor 字段（由 pass 从 _GLOBAL__sub_I_* 的 __cxa_atexit
 * 提取）非空时，__emutls_get_address 在首次为本线程分配副本后调
 * __cxa_thread_atexit 注册该副本的析构，使每个线程的副本各自析构（见
 * cxa_thread_atexit.c）。否则仅主线程副本会在进程 exit 时被析构，子线程副本丢失。
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "atomic.h"
#include "pthread_impl.h"

/* 在 cxa_thread_atexit.c 定义（同属 libc.a）。注册 dtor 到本线程的 thread_local
 * 析构链，线程退出/进程 exit 时由 __pthread_run_cxa_dtors 逆序调用。 */
int __cxa_thread_atexit(void (*dtor)(void *), void *arg, void *dso);

/* 与 BpfEmutls.cpp getEmutlsControlType 对齐：
 * {i64 size, i64 align, i64 index, ptr value, ptr dtor}。
 * dtor 由 pass 从 _GLOBAL__sub_I_* 的 __cxa_atexit(dtor,&var,dso) 提取后回填——
 * 平凡可析构类型为 NULL；非平凡类型由 __emutls_get_address 在每线程首次分配
 * 副本时经 __cxa_thread_atexit 注册本线程副本的析构（实现每线程独立析构）。 */
struct __emutls_control {
	uint64_t size;
	uint64_t align;
	uint64_t index;
	const void *value;
	void (*dtor)(void *);
};

/* index 全局递增计数器（>=1）与分配用自旋锁。 */
static volatile int g_next_index = 1;
static volatile int g_index_lock = 0;

static pthread_key_t g_slots_key;
static pthread_once_t g_slots_key_once = PTHREAD_ONCE_INIT;

static void emutls_slots_destructor(void *p) { free(p); }

static void emutls_slots_key_init(void)
{
	pthread_key_create(&g_slots_key, emutls_slots_destructor);
}

static void *alloc_aligned(size_t size, size_t align)
{
	if (align <= __BIGGEST_ALIGNMENT__)
		return malloc(size);
	void *raw = malloc(size + align - 1);
	if (!raw) return 0;
	uintptr_t base = (uintptr_t)raw;
	return (void *)((base + align - 1) & ~(uintptr_t)(align - 1));
}

static size_t emutls_get_index(struct __emutls_control *ctrl)
{
	size_t idx = ctrl->index;           /* 快路径：已分配直接返回 */
	if (idx) return idx;

	while (a_swap(&g_index_lock, 1)) ;  /* 抢锁 */
	idx = ctrl->index;                 /* double-check */
	if (!idx) {
		idx = a_fetch_add(&g_next_index, 1);
		ctrl->index = idx;
	}
	a_swap(&g_index_lock, 0);
	return idx;
}

/* 取本线程的 slots 数组基址（懒分配 / 扩容）。
 * base[0] = cap，base[idx] 为副本指针。失败返回 0。 */
static void **get_my_slots(size_t need_idx)
{
	pthread_once(&g_slots_key_once, emutls_slots_key_init);

	void **base = pthread_getspecific(g_slots_key);
	size_t cap = base ? (size_t)base[0] : 0;

	if (need_idx + 1 > cap) {           /* 含首次分配；预留 base[0] 给 cap */
		size_t newcap = cap ? cap : 8;
		while (newcap < need_idx + 1) newcap *= 2;
		void **newbase = realloc(base, newcap * sizeof(void *));
		if (!newbase) return 0;
		memset(newbase + cap, 0, (newcap - cap) * sizeof(void *));
		base = newbase;
		base[0] = (void *)newcap;
		pthread_setspecific(g_slots_key, base);
	}
	return base;
}

void *__emutls_get_address(struct __emutls_control *ctrl)
{
	size_t idx = emutls_get_index(ctrl);

	void **base = get_my_slots(idx);
	if (!base) return 0;

	void *addr = base[idx];             /* base[0] 是 cap，副本从 base[1] 起 */
	if (addr) return addr;

	addr = alloc_aligned(ctrl->size, ctrl->align);
	if (!addr) return 0;
	if (ctrl->value)
		memcpy(addr, ctrl->value, ctrl->size);
	else
		memset(addr, 0, ctrl->size);
	base[idx] = addr;

	/* 非平凡析构类型：为本线程副本注册析构。dtor 由 pass 从进程级 __cxa_atexit
	 * 转移而来——只有改为每线程注册，子线程副本的析构才不会丢失。
	 * __cxa_thread_atexit 内部按每线程链表登记，线程退出（或进程 exit）时由
	 * __pthread_run_cxa_dtors 逆序调用。dso 传 NULL：BPF 无 dlopen/dlclose，运行时
	 * 不按 DSO 过滤（__cxa_thread_atexit 实现里已 (void)dso 忽略）。 */
	if (ctrl->dtor)
		__cxa_thread_atexit(ctrl->dtor, addr, 0);

	return addr;
}
