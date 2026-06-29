# musl BPF 移植

musl 1.2.6 移植到 BPF target，作为 bpfvm 的 C 库。本文件描述移植设计与维护要点。

## 构建

```sh
sh musl/build.sh [install_prefix]
```

产物：
- `build/lib/libc.a` — 静态库（已合并 crt1，含 `_start`）
- `build/install/include/` — 头文件

CFLAGS（见 `build.sh`）：
- `-target bpf -mcpu=v4 -O1 -fno-builtin`
- `-mllvm -bpf-stack-size=16384` — 默认 4096，`crypt_blowfish` 的 `BF_crypt` 局部 struct ~8.5KB 会超限
- `-Xclang -target-feature -Xclang +alu32` — 启用 ALU32 子寄存器
- `-Xclang -target-feature -Xclang -dwarfris` — 禁止 DWARF 调试信息使用跨段重定位（`Disable MCAsmInfo DwarfUsesRelocationsAcrossSections`）
- `-fpass-plugin=build/libBpfWideArgs.so` — 突破 5 参数 / struct 返回 / 变参限制
- `-fpass-plugin=build/libBpfSoftFp.so` — 浮点 IR 改写为 `BPF_FP_*` call
- `-I$TOP/root/include` — bpfvm 的 `include/`，提供 `bpf_syscall.h` 等

configure 选项：
- `--disable-shared` — 本 VM 不支持动态 musl；`.so` 由 `bpfvm-ld -shared` 在测试构建时从 `libc.a` 合成
- `LIBCC=""` — BPF 无 gcc 运行时，浮点由 BpfSoftFp + VM 处理

`make install` 会编译 `rcrt1.o`（静态 PIE 自启动，依赖 `dlstart`，BPF 不支持）而失败，故脚本单独编译四个 CRT 对象，并手动 cp 头文件到 `install/include/`（先 `arch/generic/bits/`，后 `arch/bpf/bits/`，让 BPF 特化覆盖通用）。

**pass 改动后必须强制全量重建**：musl 的 Make 只看 `.c` 时间戳，不跟踪 pass `.so` 变化。改 pass 后执行 `find musl/build/obj -name '*.o' -delete && sh musl/build.sh`，否则会复用旧 pass 编的 `.o`。

## 架构头文件 (`arch/bpf/`)

### `syscall_arch.h` — 调用约定

`__syscallN(n, ...)` 把 call id `n` 当函数指针直接调用：`((long (*)(long))n)(a)`。clang 在 BPF target 上会把它 lower 成一条 `call <n-imm>` 指令（`src_reg=0` 的 syscall 形式），VM 的 `do_syscall` 按 `BPF_CALL_TO_ID` 分发。

`bits/syscall.h.in` 把每个 `SYS_xxx` 直接定义成 VM 的 `BPF_CALL_xxx` 值（`BPF_CALL_BASE=0x10000 + id`），所以 `n` 既是 syscall 号也是 lower 进 call 指令的 imm。参数走 r1..r5，>5 个由 BpfWideArgs pass 打包，返回值在 r0。

### `bits/syscall.h.in` — syscall 编号映射

Makefile 用 `sed -n -e s/__NR_/SYS_/p` 从 `__NR_` 前缀宏生成 `SYS_` 供 musl `#ifdef` 探测。syscall 分四类：

1. **VM 已实现**：映射到对应 `BPF_CALL_*`。参数布局不同的同名 syscall 用独立 id（dup2/dup3、mkdir/mkdirat、readlink/readlinkat、renameat/renameat2）。
2. **VM 未实现但 musl 引用**：映射到 `BPF_CALL_BASE` 占位。包括 musl `#ifdef SYS_xxx` 路径选择型（删除会改变运行时分支）和经 `socketcall_cp`/`__socketcall` 宏（`SYS_##name` 拼接）间接引用的网络系列。运行时若被调用，VM `do_syscall` default 返回 `-ENOSYS`，musl 据此降级。
3. **VM 未实现且 musl 完全不引用**：不定义 `__NR_` 宏。判定「完全不引用」须同时覆盖直接引用与宏间接引用，以「删除后 make 是否报 undeclared identifier」为准。
4. **参数布局与 *at handler 不兼容的旧版别名**（stat/lstat/fstat/fchmod/chdir/tgkill 等）：不定义 `__NR_` 宏。它们与对应 *at id 参数个数不同（如 stat 2 参 vs fstatat 4 参），共用 handler 会错位。libc 封装由 `src/*/bpf/` 覆盖版用正确的 *at syscall 实现，或走 musl 的 `#ifndef SYS_旧版` 分支。

**注释里不能出现 `__NR_` 子串**，否则 sed 会把注释行也替换并追加到生成的 `bits/syscall.h` 末尾造成乱码。

### `atomic_arch.h` — BPF 原子指令

用 `__sync` 内建生成真正的 BPF 原子指令（lock 前缀），满足多线程并发正确性。`__sync_val_compare_and_swap` 生成 BPF `cmpxchg`，`__sync_fetch_and_add` 生成 `lock *(u*)(p) += v`，VM 的 `BPF_ATOMIC` 路径执行它们。`a_cas`/`a_cas_p` 必须提供（`atomic.h` 的硬约束），其余 `a_*` 由 `atomic.h` 通用 fallback 给出；这里额外直接实现了常用的 `a_swap`/`a_fetch_add`/`a_and`/`a_or` 等，让 stdio getc/putc、mallocng 等热路径走单条原子指令而非 CAS 循环。

注意 `a_swap` 不用 `__sync_lock_test_and_set`（BPF 上可能 lower 出不支持的 xchg），改用 cmpxchg 循环实现。`a_store`/`a_barrier`/`a_spin` 只需 compiler barrier（`__asm__ __volatile__("":::"memory")`）：BPF 后端不支持 `AtomicFence`（`__sync_synchronize` 会 ISel 失败），单核 guest 不需要 hardware fence，多线程下 host 线程切换提供内存序保证。

### `pthread_arch.h` — TLS 模拟

BPF 无 TLS 寄存器（无 fs_base/tpidr），用一对 VM syscall 维护 thread pointer：
- `__set_thread_area(tp)` → `BPF_CALL_SET_TLS`：存进 VM 字段 `v->tp_`
- `__get_tp()` → `BPF_CALL_GET_TLS`：读回

启动时 musl `__init_tp` 调前者写入 `struct pthread*`，之后 `__pthread_self()`（经 `__get_tp`）读回同一个指针。`pthread_create` 创建新线程时，`__clone` 经 `CLONE_SETTLS` 把子线程的 TP 设为各自 `struct pthread` 的地址，VM 的 `do_clone` 据此为每线程维护独立 `tp_` 字段。

TLS 模型 BELOW_TP（不定义 TLS_ABOVE_TP，与 x86_64 一致）。此时 musl 的 `pthread_impl.h` 定义 `TP_ADJ(p)==(p)`、`__pthread_self()==(pthread_t)__get_tp()`，写入与读回的值就是 `struct pthread*` 本身，无需偏移调整。

`MC_PC = gregs[16]`（x86_64 `REG_RIP` 在 `gregset_t[23]` 中的位置），仅供 pthread_cancel 等信号上下文相关代码编译，本 arch 不提供 `bits/reg.h`，故直接用数字索引避免依赖 x86 寄存器编号宏。

### `crt_arch.h` — `_start`

纯 C 实现（其它 arch 用内联汇编）。VM 入口约定 `reg[1] = STACK_BASE`（指向 argc 的栈顶），`_start` 把它当 `long*` 传给 `_start_c(p)`（`p[0]=argc, p[1..]=argv`）。clang 把 `_start` 作普通函数编译，VM 从 ELF `e_entry` 进入。

### `reloc.h`

- `reloc.h`：静态链接占位，`LDSO_ARCH="bpf"`，`CRTJMP`/`GETFUNCSYM` 给不可达占位。

### `bits/` 其他

- `alltypes.h.in`：`_Addr/_Int64/_Reg = long`，小端，`max_align_t` 含 `long double`。
- `float.h`：`long double == double`（64-bit IEEE-754），`LDBL_MANT_DIG=53`。configure 的 `ldcheck` 接受 `C(53,8)` 组合。
- `setjmp.h`：`__jmp_buf[8]` + `__fl` + `__ss[16]`（沿用 musl `__jmp_buf_tag`）。VM 的 sigsetjmp/siglongjmp 在同一次 syscall 里保存/恢复 10 槽：r6..r10 + pc + signal_depth + 备用 + `__fl`(savemask 标志) + `__ss[0]`(sigmask)。**`sigsetjmp`/`setjmp`/`_setjmp` 以宏在调用点内联展开**（发射 `call BPF_CALL_SIGSETJMP`，src_reg=0），不包成函数——详见下文「sigsetjmp 必须内联」。
- `signal.h`：mcontext_t/ucontext_t 沿用 x86_64 布局占位，VM 用自己的 sigframe，这些结构只为让 musl 信号路径编译通过。
- `limits.h`：`PAGESIZE 4096`。

## 源码覆盖 (`src/*/bpf/`)

利用 musl 的 arch 覆盖机制（`REPLACED_OBJS`），在对应模块的 `bpf/` 子目录放同名 `.c` 替换原版。

- **`setjmp/longjmp.c`** — 走 VM 专用 syscall `BPF_CALL_SIGLONGJMP`，在同一次调用里恢复 r6..r10+pc+signal_depth，并按 `env->__fl` 决定是否一并恢复信号掩码。BPF 无法直接操纵寄存器做栈帧控制。`longjmp`/`siglongjmp` 是 `_Noreturn` 函数：do_siglongjmp 跳走后不再返回，它 push 的 VM 帧位于 siglongjmp 恢复后的 sp 之上，不会被复用，故无需像 sigsetjmp 那样内联。`siglongjmp` 由通用 `src/signal/siglongjmp.c` 提供（调用本 longjmp，掩码恢复由 VM 按 `__fl` 自动完成）。
- **sigsetjmp 必须内联（不在 `src/*/bpf/`，而在 `arch/bpf/bits/setjmp.h`）** — `sigsetjmp` 以**宏**展开，在调用点直接发射 `call BPF_CALL_SIGSETJMP`（src_reg=0），与 `__syscallN`/`__get_tp` 的内联原则一致；`setjmp`/`_setjmp` 是它的薄封装（`setjmp(env)==sigsetjmp(env,1)`、`_setjmp(env)==sigsetjmp(env,0)`）。不能把 sigsetjmp 包成普通函数：BPF VM 用 r10(sp) 上的链表管理调用帧，每次 BPF-to-BPF `call`（src_reg=1）push 一帧；函数化的 sigsetjmp 会在 sigsetjmp 与调用者之间多套一层 VM 帧，do_sigsetjmp 保存到的 r10 是该函数内部的 sp 而非调用者的 sp，siglongjmp 只能恢复到这层多余的帧，pop 出的返回地址与 sigsetjmp 首次返回时不同，栈错乱，sigsetjmp「第二次返回」与执行流不一致（dash 的 `exit` 因此退出失败）。`returns_twice` 只保证跨 sigsetjmp 的寄存器分配正确，挡不住这层多余帧——故宏内联是必须的。`include/setjmp.h` 用 `#ifndef __BPF__` 跳过 sigsetjmp/setjmp 的函数声明/`#define setjmp setjmp`（与函数式宏冲突；longjmp/siglongjmp 声明保留）。
- **`thread/clone.s`** — BPF 汇编 `__clone`，是 `pthread_create`/`fork` 的底层。`__clone` 有 7 个参数，超过 BPF 的 5 参限制，BpfWideArgs pass 把第 5 参起打包成结构体经 r5 传入；汇编从 pack 取出 ptid/tls/ctid，把 func arg 存入 callee-save r6、r7，再以 5 参形式 `call BPF_CALL_CLONE`（r1=flags, r2=stack, r3=ptid, r4=ctid, r5=tls）。父子在 syscall 返回点分流：父返回 child tid；子`callx r6` 调 func(arg)，返回后 `call BPF_CALL_EXIT`。子线程的 TP 由 `CLONE_SETTLS` 经 tls 参数直接设置（不经过 `__init_tp`）。
- **`thread/__syscall_cp.c`** — cancellation point 的 arch 实现。`__syscall_cp_asm` 直接转发 `__syscall`：标准 musl 靠信号处理器把「PC 落在 `__syscall_cp_asm` 区间内」时改写为 `__cp_cancel` 来打断阻塞中的 syscall，本 VM 不做这种 PC 区间改写，取消只发生在 syscall 返回 EINTR 之后（由 `__syscall_cp_c` 的返回后检查 `self->cancel` 完成）。`__cp_begin`/`__cp_end`/`__cp_cancel` 是纯地址哨兵（被取地址、不调用），满足 `pthread_cancel.c` 的符号引用；cancel_handler 取它们的地址做区间比较，但因 VM 不会把 PC 跳进区间，比较恒为假。保留 `__syscall_cp` 强定义转发 `__syscall_cp_c`，与 base 版符号集一致、供外部取地址（折叠宏后不被 libc 内部调用）。
- **`unistd/chdir.c`** — `open(path) + fchdir(fd) + close(fd)` 实现 chdir（VM 无 chdir syscall，只有 fchdir）。
- **`unistd/fchdir.c`** — 去掉原版的 `/proc/self/fd/N` fallback（依赖未定义的 `SYS_chdir`），只发 `SYS_fchdir`。
- **`stat/fchmod.c`** — `syscall(SYS_fchmodat, fd, "", mode, AT_EMPTY_PATH)`。必须直接发 syscall，不能调 musl 自己的 `fchmodat()`（它对非 `AT_SYMLINK_NOFOLLOW` 的 flag 一律返回 EINVAL，会误杀 `AT_EMPTY_PATH`）。
- **`math/mulsc3.c`** — `__muldc3`/`__mulsc3` 复数乘法 runtime，替代 libgcc/compiler-rt（BPF 两者皆无）。简化实现，不处理 Inf*0=NaN 边界。
- **`unistd/setxid.c`** — `__setxid` 覆盖版，绕开 `__synccall` 直接发常量 syscall。原版 `do_setxid` 回调里 `__syscall(c->nr,...)` 的 `c->nr` 是运行时值，会 lower 成间接调用 `call rX`（BPF_X），被 VM 当跳转地址 → 崩溃（详见「syscall 号运行时化的限制」）。覆盖版用编译期常量 `SYS_setuid` 让 clang lower 成 `call imm`（src_reg=0），VM 正常拦截返回 `-ENOSYS`（VM 尚未实现 setuid 系列 handler）。

## musl 源码改动

### `src/internal/syscall.h` — syscall_cp 折叠宏

`#ifdef __bpf__` 块把 `__syscall_cp0..6` / `__syscall_cp(...)` `#undef` 后重定义为直接走 `__syscall0..6` / `__syscall(...)`（arch/bpf 的 static inline，nr 保持编译期常量）。

原因：标准 musl 的 `syscall_cp` 走 `__syscall_cp` → `__syscall_cp_c`（`pthread_cancel.c` 的非内联函数）→ `__syscall_cp_asm`（arch 汇编）。这条链上 nr 作为函数参数逐层传递，到最底层是运行时变量，clang 无法把 `((long(*)(...))nr)(...)` 折叠成常量 `call <imm>`（`src_reg=0`），只能 lower 成 BPF 不支持的间接调用。故 libc 内部所有可取消 syscall 必须折叠回 inline、nr 保持常量；用户多线程程序的可取消 syscall 仍走 `__syscall_cp_c` 真函数链（`__syscall_cp_asm` 转发 `__syscall`，其中 nr 是运行时值）——这要求 VM 能处理 `__syscall_cp_asm` 内由 nr 变量驱动的间接 syscall 调用。

**位置关键**：折叠块必须在 `__syscall_cpN` 原定义之后（`#undef` 才合法），但又必须在 `__alt_socketcall` **之前**，位于第 61 行附近（紧接 `__syscall_cp`/`syscall_cp` 宏定义之后）。

为什么不能放文件末尾：`__alt_socketcall`（网络函数 `sendto`/`recvfrom`/`connect`/`accept`/... 的公共 inline 函数）体内调 `__syscall_cp(sys, ...)`。预处理器单遍扫描，`static inline` 函数体在**定义点**就被展开——若折叠块在它之后，函数体里的 `__syscall_cp` 走的是折叠前的原始定义（→ `__syscall_cp_c` 真函数 → 间接调用）。折叠块在前时，所有网络函数经 `__alt_socketcall` 内联 + 常量传播（`-O1` 下 clang 把 `SYS_##nm` 折叠进 `sys` 参数），正确 lower 成 `call imm`。

也不能挪到 `arch/bpf/syscall_arch.h`（那会在 `syscall.h` 顶部被引入，早于 `__syscall_cpN` 原定义，`#undef` 无对象）。

## syscall 号运行时化的限制

BPF 的 syscall 机制要求 nr 是编译期常量：`__syscall(n,...)` 展开成 `((long(*)(long...))n)(...)`，n 是常量时 clang lower 成 `call <imm>`（opcode `85 00`，src_reg=0 的 syscall 形式，VM 的 `do_syscall` 拦截）；n 是运行时值（函数参数 / 结构体字段 / va_arg）时只能 lower 成 `call rX`（opcode `85 1?`，`BPF_X` 间接调用形式）——该形式在 VM 里被当成**普通函数调用**处理（`push_frame` + `pc = r(dst) - sizeof(insn)`，即"跳转到 r(dst 指向的地址"），而非 syscall。把一个 syscall id（如 `0x10000`）当地址跳过去会落到未定义地址崩溃。

> 注：多线程可取消 syscall 走 `__syscall_cp_c`/`__syscall_cp_asm` 路径，其内 `nr` 也是运行时值，lowering 成上述间接调用形式。这与 syscall 语义无关——该路径靠 VM 能在「跳转目标恰好是合法代码地址」的前提下完成控制流，并不是"VM 支持间接 syscall"。详见「源码覆盖」的 `__syscall_cp.c`。

最典型、也是已修复的实例是 setuid 系列。

**`__setxid` / `do_setxid`**（原版 `src/unistd/setxid.c`）：
```c
static void do_setxid(void *p) {
    struct ctx *c = p;
    int ret = __syscall(c->nr, c->id, c->eid, c->sid);  // c->nr 运行时值
    ...
}
int __setxid(int nr, int id, int eid, int sid) {
    struct ctx c = { .nr = nr, ... };
    __synccall(do_setxid, &c);   // 经 synccall 上下文传递 nr
}
```
`c->nr` 经 `__synccall` 的上下文结构体传给回调 `do_setxid`，是真正的运行时值，会 lower 成间接调用 `call rX`，把 `c->nr`（一个 syscall id）当跳转地址 → 崩溃。`setuid`/`setgid`/`setreuid`/`setregid`/`setresuid`/`setresgid`/`seteuid`/`setegid`（`src/unistd/` 下 8 个公开函数）都经 `__setxid(SYS_setuid, ...)`，故全部受影响。

**解法**：`src/unistd/bpf/setxid.c` 覆盖版绕开 `__synccall`，直接在当前线程同步发常量 syscall：
```c
int __setxid(int nr, int id, int eid, int sid) {
    long ret = __syscall(SYS_setuid, id, eid, sid);  // 常量 nr → call imm（src_reg=0）
    return __syscall_ret(ret);
}
```
`SYS_setuid` 是编译期常量，clang lower 成 `call 0x10000`（src_reg=0），VM 的 `do_syscall` 正常拦截。当前所有 `__NR_set*` 均映射到 `BPF_CALL_BASE` 占位（VM 未实现 setuid 系列 handler，`do_syscall` default 分支统一返回 `-ENOSYS`），故 setuid 系列现返回 `-1`/`errno=ENOSYS`，**不再崩溃**。单线程语义：不广播到其它线程（本 arch 多线程场景不要求 POSIX 凭据一致性）。

**后续 VM 实现 setuid 系列**：在 `include/bpf_syscall.h` 注册 `BPF_CALL_SETUID`/`SETGID`/...，在 `posix_syscall.cpp` 加 handler，并把 `arch/bpf/bits/syscall.h.in` 里各 `__NR_set*` 从 `BPF_CALL_BASE` 改成真实 id。届时建议把 `setxid.c` 的单一调用改回 switch，每个 case 调对应的具体 `__syscall(SYS_setxxx, ...)`，让 clang 为每个调用点独立 lower 成 `call imm`（避免所有 set* 共用一个占位 id）。

### 维护要点

- 改动 syscall 相关代码后，用 `bpf-readelf -r <file>.o | grep __syscall_cp` 检查是否有残留引用（应为 0，除 `pthread_cancel.o`/`__syscall_cp.o` 自身）；用 `bpf-objdump -d <file>.o | grep 'call %r'` 检查是否有意外的间接调用。
- 新增 `__syscall(变量,...)` 调用前，确认 VM 已支持该间接 syscall 路径；若在 libc 内部、单线程即触发，须改为常量 `SYS_xxx` 或像 `syscall_cp` 那样做 arch 折叠。

### `src/process/posix_spawn.c`

`FDOP_CHDIR` 改用 `chdir()` 封装（VM 无 `SYS_chdir`）。`chdir()` 失败只返回 -1 并设 errno，须转成 `-errno` 匹配 fail 路径约定（`ret = -ret` 后写回父进程作为 errno）。

### `configure`

加 `bpf*) ARCH=bpf ;;` 分支识别 target。

## 限制与未实现

- **多线程**：已支持 `pthread_create`/`pthread_join`/mutex/condvar/futex 等。`__clone` 走 BPF 汇编（`src/thread/bpf/clone.s`），新线程的 TP 由 `CLONE_SETTLS` 设置；原子操作用真正的 BPF 原子指令（`__sync` 内建）。
- **可取消 syscall**：多线程用户代码的可取消 syscall 经 `__syscall_cp_c`/`__syscall_cp_asm` 走间接 syscall（src_reg=1），VM 支持；取消在 syscall 返回 EINTR 后由 `__syscall_cp_c` 检查 `self->cancel` 完成（详见上文「源码覆盖」的 `__syscall_cp.c`）。
- **setuid 系列返回 ENOSYS**：`__setxid` 覆盖版（`src/unistd/bpf/setxid.c`）绕开 `__synccall` 直接发常量 syscall。当前 VM 未实现 setuid 系列 handler，统一返回 `-1`/`errno=ENOSYS`（不再崩溃）。多线程下不广播到其它线程。详见上文「syscall 号运行时化的限制」。busybox 可保留 `CONFIG_FEATURE_SUID`（setuid 失败即降级，不影响运行）。
- **无动态 musl**：`--disable-shared`，`.so` 由 `bpfvm-ld -shared` 从 `libc.a` 合成。
- **long double == double**：64-bit，128-bit 精度代码须改用 double。
- **复数乘法**：`__muldc3`/`__mulsc3` 简化实现，不处理 Inf/NaN 边界。
