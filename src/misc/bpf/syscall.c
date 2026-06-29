/*
 * BPF port: 用户可见的 `syscall()` 在本 arch 是宏，不是函数。
 *
 * 本文件存在仅为触发 musl Makefile 的 arch 覆盖（ARCH_GLOBS：
 * src/misc/bpf/syscall.c 优先于 src/misc/syscall.c），阻止通用版本进入 libc.a。
 * 没有任何符号定义。
 */
