# BPF __clone — 参考 riscv64/aarch64 模式，func/arg 不进 syscall。
#
# BpfWideArgs 把 __clone 的 7 参 pack 成 4 参 + pack 指针：
#   入口: r1=func, r2=stack, r3=flags, r4=arg, r5=pack{ptid,tls,ctid}
#   pack 布局（packed，每字段 8 字节）: [0]=ptid, [8]=tls, [16]=ctid
#
# syscall(CLONE, flags, stack, ptid, ctid, tls) — 5 参走 r1-r5:
#   r1=flags, r2=stack, r3=ptid, r4=ctid, r5=tls
#
# callee-save r6/r7 跨 syscall 保留：func 存 r6，arg 存 r7。
# child 从 syscall 返回点继续（pc=call 后，r(0)=0，r6=func，r7=arg）：
#   r1 = r7（func 的实参），callx r6 调 func(arg)。
#
# 父进程: r0 = child tid (>0)，ret。
# 子进程: r0 == 0，调 func(arg) 后 exit。

.text
.global __clone
.hidden __clone
.type __clone,@function
__clone:
    # callee-save 存 func(r6) 与 arg(r7)：跨 syscall 保留，子线程直接从寄存器取。
    r6 = r1                  # func
    r7 = r4                  # arg

    # syscall: r1=flags, r2=stack, r3=ptid, r4=ctid, r5=tls
    r1 = r3                  # flags
    # 新栈对齐到 16
    r2 &= -16
    r2 += -8

    # ptid/tls 从 pack（r5）读。
    r3 = *(u64*)(r5 + 0)     # ptid
    r4 = *(u64*)(r5 + 16)    # ctid
    r5 = *(u64*)(r5 + 8)     # tls
    call 65549               # BPF_CALL_CLONE = 0x10000 + 0x0d = 65549（BPF_SYS_CLONE=13）

    # 父子分流
    if r0 != 0 goto 1f       # 父：r0=child tid，返回

    # child: r0==0, r6=func, r7=arg。arg 从寄存器取，不碰栈顶（那是 VM 的哨兵帧）。
    r1 = r7                  # func 的实参
    callx r6                 # 调 func(arg)

    # exit
    r1 = 0
    call 65539               # BPF_CALL_EXIT = 0x10000 + 0x03 = 65539
    exit

1:  # 父进程返回
    exit
