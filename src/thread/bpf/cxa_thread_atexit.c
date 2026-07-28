/*
 * thread_local 析构运行时——配合 BpfEmutls pass 的 dtor 转移机制，为非平凡
 * 可析构的 thread_local 变量提供「每线程副本各自析构」语义。
 *
 * 背景：C++ 标准 thread_local 的析构经 __cxa_thread_atexit(dtor, obj, dso) 注册到
 * 当前线程；线程退出或进程 exit 时逆序调用。本项目里 thread_local 被预处理期替换成
 * annotate("emutls") 标注的普通全局，clang 不再生成 __cxa_thread_atexit，而是走普通
 * 全局的进程级 __cxa_atexit（仅析构主线程副本）。BpfEmutls pass 把这条 __cxa_atexit
 * 的 dtor 转移进 emutls 控制块，由 emutls.c 的 __emutls_get_address 在每线程首次分配
 * 副本时调本文件的 __cxa_thread_atexit 注册——从而每个线程的副本都能析构。dso 句柄
 * 不被转移：BPF 无 dlopen/dlclose，本实现不按 DSO 过滤（见下方 (void)dso）。
 *
 * 存储：每线程一个 dtor 节点链表（头插 = 析构逆序），用一个 pthread_key 挂在
 * struct pthread::tsd[]。key 的 destructor 故意留空——析构时机必须由本文件的
 * __pthread_run_cxa_dtors 显式驱动（在 emutls 副本内存被 free 之前、且覆盖主线程
 * exit 路径），不能交给 pthread_key 的自动 destructor。
 */

#include <stdlib.h>
#include "pthread_impl.h"

struct tls_dtor {
	void (*dtor)(void *);
	void *arg;
	struct tls_dtor *next;
};

static pthread_key_t g_dtor_key;
static pthread_once_t g_dtor_once = PTHREAD_ONCE_INIT;

static void dtor_key_init(void)
{
	/* destructor 传 0：节点由 __pthread_run_cxa_dtors 显式 free，
	 * 不让 pthread_key 机制抢先（时序不可控，可能晚于 emutls 副本 free）。 */
	pthread_key_create(&g_dtor_key, 0);
}

int __cxa_thread_atexit(void (*dtor)(void *), void *arg, void *dso)
{
	(void)dso;  /* BPF 无 dlopen/dlclose，dso 过滤不适用，仅存档不使用 */
	pthread_once(&g_dtor_once, dtor_key_init);

	struct tls_dtor *n = malloc(sizeof *n);
	if (!n) return -1;
	n->dtor = dtor;
	n->arg = arg;
	n->next = pthread_getspecific(g_dtor_key);  /* 头插：后注册先析构 */
	pthread_setspecific(g_dtor_key, n);
	return 0;
}

/* 遍历当前线程的析构链，逆序（头插链表的正序遍历即注册逆序）调用各 dtor 并 free
 * 节点，最后清空 key。单趟遍历：标准允许 dtor 内重设 thread_local 值触发再次析构
 *（PTHREAD_DESTRUCTOR_ITERATIONS），但 thread_local 的 __cxa_thread_atexit 注册模型
 * 下不会出现「析构后又自动重建」的循环，故单趟足够。
 *
 * 调用点（见 pthread_key_create.c 的 __pthread_tsd_run_dtors 与 exit.c 的 exit）：
 *  - 子线程退出：__pthread_tsd_run_dtors 开头，先于 pthread_key destructor（含
 *    emutls 副本内存 free），保证析构时副本内存仍有效。
 *  - 主线程/进程 exit：exit() 在 __libc_exit_fini 之后调用——主线程不走
 *    __pthread_tsd_run_dtors（见 pthread_create.c __pthread_exit 对 self->next==self
 *    的特殊处理），故需在 exit 路径单独触发一次。
 * 两处互斥：子线程钩子只对真子线程跑（主线程经 exit 退出），不会重复。 */
void __pthread_run_cxa_dtors(void)
{
	pthread_once(&g_dtor_once, dtor_key_init);
	struct tls_dtor *cur = pthread_getspecific(g_dtor_key);
	if (!cur) return;
	pthread_setspecific(g_dtor_key, 0);
	while (cur) {
		struct tls_dtor *next = cur->next;
		cur->dtor(cur->arg);   /* dtor 失败无法回滚，忽略继续 */
		free(cur);
		cur = next;
	}
}
