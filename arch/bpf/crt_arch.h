#ifndef CRT_ARCH_H
#define CRT_ARCH_H

/*
 * BPF 程序入口（纯 C，无内联汇编）。
 *
 * VM 的入口约定：reg[1] = 指向 argc 的栈顶（见 insn.cpp setup_stack）。其它 arch 用
 * 内联汇编写入口（从 %rsp 取栈指针），BPF 直接把 reg[1] 当 long* 传给下层 _start_c /
 * _dlstart_c。
 *
 * 本头被两处 include，由 START 宏区分：
 *   1) crt/crt1.c  定义 START="_start"  → 静态程序入口 _start，调 _start_c(p)。
 *   2) ldso/dlstart.c 定义 START="_dlstart" → 动态链接器入口 _dlstart，调 _dlstart_c(p, dynv)。
 *
 * _dlstart 需要 dynv（ldso 自身 .dynamic 段指针）。其它 arch 用 PC-relative 汇编
 *（lea _DYNAMIC(%rip)）在自举前（重定位未做时）拿到；BPF 无 PC-relative，取地址的
 * lddw 指令本身尚需重定位（鸡蛋问题），故改从 auxv 的 AT_BASE（=ldso 加载基址）
 * 定位：ld-bpf.so 的第一个 PT_LOAD 从文件 offset 0 开始（含 ELF header），故 base
 * 处即 Ehdr，读其 e_phoff/e_phnum/e_phentsize 扫 phdr 找 PT_DYNAMIC，得
 * dynv = base + phdr->p_vaddr。与 dlstart.c FDPIC fallback（dynv=NULL 分支）同思路。
 */

#include <stdint.h>
#include <stddef.h>

hidden void _start_c(long *);

/* _dlstart_c 由 ldso/dlstart.c 提供。 */
hidden void _dlstart_c(size_t *sp, size_t *dynv);

#ifdef SHARED
/* 动态链接器入口 _dlstart（dlstart.c 定义 SHARED 后 include 本头）。 */
#include <elf.h>
#ifndef AT_BASE
#define AT_BASE 7
#endif
__attribute__((visibility("default"), used))
void _dlstart(size_t *p)
{
    /* p[0]=argc, p[1..argc]=argv, p[argc+1..]=envp(以NULL结尾), 之后是 auxv(type,val对)。 */
    size_t argc = p[0];
    char **argv = (void *)(p + 1);
    char **envp = argv + argc + 1;
    size_t *auxv = (void *)(envp);
    while (*auxv) auxv++;
    auxv++;
    /* 从 auxv 取 AT_BASE（ldso 加载基址，VM 布入）。ld-bpf.so 首个 PT_LOAD 从 offset 0
     * 开始（含 ELF header），故 base 处即 Ehdr；扫 phdr 找 PT_DYNAMIC，得
     * dynv = base + phdr->p_vaddr。 */
    size_t base = 0;
    for (size_t *a = auxv; a[0]; a += 2)
        if (a[0] == AT_BASE) { base = a[1]; break; }
    Elf64_Ehdr *eh = (void *)base;
    Elf64_Phdr *ph = (void *)(base + eh->e_phoff);
    size_t *dynv = 0;
    for (size_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) { dynv = (void *)(base + ph[i].p_vaddr); break; }
    }
    _dlstart_c(p, dynv);
}
#else
/* 静态程序入口 _start（crt1.c 不定义 SHARED）：p=reg[1]（栈顶 argc）。 */
__attribute__((visibility("default"), used))
void _start(long *p)
{
    _start_c(p);
}
#endif

#endif
