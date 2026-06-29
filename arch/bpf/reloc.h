#ifndef RELOC_H
#define RELOC_H

/*
 * BPF 动态链接器（ldso）重定位定义。
 *
 * BPF（eBPF）的重定位 ABI（LLVM BPF 后端产生，bpfvm-ld 透传）：
 *   type 1  R_BPF_64_64      lddw 指令（16 字节双槽：64 位立即数拆写 imm@+4 低32 / imm@+12 高32）
 *   type 2  R_BPF_64_ABS64   8 字节数据指针（绝对地址）
 *   type 4  R_BPF_64_NODYLD32 4 字节（"NO Dynamic LinKer"——动态链接器不处理，构建期已 resolve）
 *   type 10 R_BPF_64_32      call 指令（构建期已 rewrite 为相对 call / PLT，不出现在 .rela.dyn）
 *
 * .so 的 .rela.dyn 实际只出现 type 1（lddw，占绝大多数）和 type 2（数据指针）。
 *
 * 与 musl do_relocs 框架的适配：
 *   - type 2（8 字节）→ REL_SYMBOLIC：musl 单次 8 字节写（*reloc_addr = sym_val + addend）兼容。
 *   - type 1（lddw 双槽）→ REL_SYM_OR_REL：IS_RELATIVE 对 type1+sym0 成立（dlstart 自举），
 *     但写法是双槽（写 imm@+4/@+12），需 dynlink.c 的 BPF 特化 case（见 do_relocs 的
 *     #ifdef __bpf__ 段）。sym_idx=0（本模块符号）时 addend 已含符号地址，V=base+addend；
 *     sym_idx!=0（跨模块）时 V=sym_val+addend。
 *   - type 4 → 不映射（落到 do_relocs default，但 .so 不含 type 4，不会触发）。
 *
 * 注意：musl/include/elf.h 把 type 1 定义成 R_BPF_MAP_FD（内核 eBPF map fd 约定），
 * 与本项目的 R_BPF_64_64=1 数值冲突。这里用本地 #define 覆盖，不依赖 musl 的 elf.h 定义。
 *
 * 本模块符号的重定位（sym_idx=0）：bpfvm-ld 生成时用 NULL 符号 + addend=符号相对地址，
 * do_relocs 的 sym_index==0 分支（sym=0, def.dso=dso）使 V=base+addend，正确。
 */

#define LDSO_ARCH "bpf"

/* BPF 重定位类型编号（LLVM BPF ABI；覆盖 musl elf.h 的 R_BPF_MAP_FD 命名冲突）。 */
#undef R_BPF_64_64
#define R_BPF_64_64      1   /* lddw 16 字节双槽 */
#undef R_BPF_64_ABS64
#define R_BPF_64_ABS64   2   /* 8 字节数据指针 */
#undef R_BPF_64_NODYLD32
#define R_BPF_64_NODYLD32 4  /* 4 字节（ldso 不处理） */

/* 映射到 musl do_relocs 框架的 REL_*。
 * type 2（8 字节符号地址）走标准 REL_SYMBOLIC（单次 8 字节写）。
 * type 1（lddw 双槽）走 REL_SYM_OR_REL：sym_idx=0 时 IS_RELATIVE 成立（dlstart 自举走相对
 * 路径），其余由 do_relocs 的 REL_SYM_OR_REL case 处理——BPF 下该 case 特化为双槽写
 *（dynlink.c 的 #ifdef __bpf__ 段）。 */
#define REL_SYMBOLIC     R_BPF_64_ABS64
#define REL_SYM_OR_REL   R_BPF_64_64

/* ldso 完成重定位 + TLS 建立后，把控制权交给主程序入口（aux[AT_ENTRY]）。
 * BPF guest 没有"改 sp + 裸跳"的指令，但 VM 的 siglongjmp 实现（do_siglongjmp）正是
 * "设 r10(sp) + 设 pc + return true 跳转"。jmp_buf 第一个成员 __jb（unsigned long[8]）：
 *   __jb[4] = r10(sp), __jb[5] = pc（sigsetjmp syscall 指令地址，longjmp 后 run 循环
 * pc+=8 落到下一条）。
 * 故 __jb[5] 设成 (entry - 8)，让 do_siglongjmp 后 pc+=8 正好落到 entry。仿 do_execve 的
 * pc-=sizeof(bpf_insn) 技巧（src/posix/process.cpp）。CRTJMP 后紧跟 for(;;);，longjmp 的
 * _Noreturn 不冲突。 */
/* sp 的取值（关键）：标准 musl/内核里 argc 在栈顶，CRTJMP 的 sp=argv-1（=argc 地址）
 * 恰好是栈顶，主程序能在其下方向下增长。但 BPF VM 的 setup_stack 把 argc 放在栈底
 * （STACK_BASE），栈从栈顶向下增长；若 r10=argv-1=栈底，主程序 _start 的第一次
 * push_frame 的下界检查（r10 - ... < STACK_BASE）立即失败 → 栈溢出。故 r10 不能取
 * argv-1（栈底），而应取 ldso 当前栈帧内的地址（CRTJMP 所在的 __dls3 帧，必然在栈帧
 * 增长区内）。主程序 _start 只用 r1（=p）读 argc，故把 argc 地址（原 sp 参数）放进
 * __jb[7]，do_siglongjmp 用它恢复 r1。
 * __jb[7] 在普通 sigsetjmp 里被 do_sigsetjmp 置 0，do_siglongjmp 对 __jb[7]==0 不改 r1
 * （r1 是 caller-saved，普通 longjmp 不恢复它），故复用 __jb[7] 无副作用。
 *
 * 对齐：_marker 的地址必须 16 字节对齐（AAPCS64/SysV 栈对齐要求；主程序的原子操作
 * 在严格对齐架构如 aarch64 上对未对齐地址会 SIGBUS）。栈向下生长，取 _marker 地址后
 * 向下 round 到 16 仍是 __dls3 栈帧内的合法地址（保留在栈增长区，不触发下界检查）。 */
#define CRTJMP(pc, sp) do { \
	jmp_buf _jb; \
	char _marker; \
	unsigned long _sp = (unsigned long)&_marker; \
	_sp &= ~(unsigned long)15; \
	_jb[0].__jb[4] = _sp; \
	_jb[0].__jb[5] = (unsigned long)(pc) - 8; \
	_jb[0].__jb[7] = (unsigned long)(sp); \
	longjmp(_jb, 1); \
} while (0)

/* dlstart stage-1 自举用：取 __dls2 等函数地址跳 stage2。其它 arch 用 PC-relative 汇编绕过
 * "自举前自身重定位未做"的问题；BPF 无 PC-relative，但 VM loader 加载 ldso 时已应用全部
 * 重定位（含 lddw），故直接取符号地址（经已重定位的 lddw）即是正确值。
 * 需前向声明 sym（其它 arch 的汇编版不要求 C 声明）。 */
#define GETFUNCSYM(fp, sym, got) do { \
	extern hidden void sym(); \
	(void)(got); \
	*(fp) = sym; \
} while (0)

#endif
