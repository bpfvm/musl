/*
 * BPF signal 头（本 port 自有形状，不镜像任何真实内核）。
 *
 * mcontext_t 是 BPF 的机器上下文：gregs[12] = r0..r10 + pc（REG_SP=10、
 * REG_PC=11，MC_PC 同此下标），无 FPU 上下文。SA_SIGINFO 投递时 VM 构造
 * ucontext_t（bpfvm src/posix/signal.cpp 的 build_siginfo；布局镜像见
 * src/posix/posix_internal.h，两侧 static_assert 锁 264B，勿单边改动）。
 * handler 正常返回后，VM 按（可能被 handler 改写过的）gregs/uc_sigmask
 * 恢复现场，等价 Linux rt_sigreturn 语义。
 */

#if defined(_POSIX_SOURCE) || defined(_POSIX_C_SOURCE) \
 || defined(_XOPEN_SOURCE) || defined(_GNU_SOURCE) || defined(_BSD_SOURCE)

#if defined(_XOPEN_SOURCE) || defined(_GNU_SOURCE) || defined(_BSD_SOURCE)
#define MINSIGSTKSZ 2048
#define SIGSTKSZ 8192
#endif

#ifdef _GNU_SOURCE
enum { REG_SP = 10 };
#define REG_SP REG_SP
enum { REG_PC = 11 };
#define REG_PC REG_PC
#endif

#if defined(_GNU_SOURCE) || defined(_BSD_SOURCE)
typedef unsigned long greg_t, gregset_t[12];
struct sigcontext {
	unsigned long gregs[12];
};
typedef struct {
	gregset_t gregs;
} mcontext_t;
#else
typedef struct {
	unsigned long __space[12];
} mcontext_t;
#endif

struct sigaltstack {
	void *ss_sp;
	int ss_flags;
	size_t ss_size;
};

typedef struct __ucontext {
	unsigned long uc_flags;
	struct __ucontext *uc_link;
	stack_t uc_stack;
	mcontext_t uc_mcontext;
	sigset_t uc_sigmask;
} ucontext_t;

#define SA_NOCLDSTOP  1
#define SA_NOCLDWAIT  2
#define SA_SIGINFO    4
#define SA_ONSTACK    0x08000000
#define SA_RESTART    0x10000000
#define SA_NODEFER    0x40000000
#define SA_RESETHAND  0x80000000
#define SA_RESTORER   0x04000000

#endif

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGIOT    SIGABRT
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   29
#define SIGPWR    30
#define SIGSYS    31
#define SIGUNUSED SIGSYS

#define _NSIG 65
