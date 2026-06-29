/*
 * BPF siglongjmp / longjmp 实现 —— 走 VM 专用 syscall。
 *
 * longjmp调用 BPF_CALL_SIGLONGJMP 后，VM 的 do_siglongjmp 恢复
 * r6..r10 + pc + signal_depth，把 pc 指向当初 sigsetjmp syscall 的下一条指令，
 * r0 设为 val（val==0 时置 1）。若 jmp_buf 的 __fl 非 0（sigsetjmp 时
 * savemask!=0），VM 一并恢复信号掩码。故本函数在 guest 看来「不返回」——执行流
 * 已跳回 sigsetjmp 调用点；声明 _Noreturn 与各 arch 汇编版一致。
 *
 * siglongjmp 由通用 src/signal/siglongjmp.c 提供：它调用本 longjmp，掩码恢复
 * 与否由 env->__fl 在 VM 的 do_siglongjmp 内判定，故用户态无需特判。
 *
 * val==0 归一化为 1 的处理已在 VM 的 do_siglongjmp 内完成，这里原样透传。
 */

#include <setjmp.h>
#include <stdint.h>
#include "syscall.h"

_Noreturn void longjmp(jmp_buf buf, int val)
{
    __syscall2(BPF_CALL_SIGLONGJMP, (long)buf, val);
    __builtin_unreachable();
}

weak_alias(longjmp, _longjmp);
