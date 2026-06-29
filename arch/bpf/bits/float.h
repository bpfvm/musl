/*
 * BPF 浮点特性。
 *
 * BPF 无硬件浮点，long double 等同于 double（64-bit IEEE-754，MANT_DIG=53，
 * sizeof=8），与项目 AGENTS.md「long double == double」一致。故这里采用与
 * 64-bit double 相同的宏值（configure 的 ldcheck 接受 C(53,8) 组合）。
 *
 * float/double 的值与 x86_64 相同（标准 IEEE-754）。本 arch 的浮点运算由
 * BpfSoftFp pass 改写为 BPF_FP_* call、VM 用宿主硬件浮点执行。
 */

#ifdef __FLT_EVAL_METHOD__
#define FLT_EVAL_METHOD __FLT_EVAL_METHOD__
#else
#define FLT_EVAL_METHOD 0
#endif

#define LDBL_TRUE_MIN 4.9406564584124654418e-324L
#define LDBL_MIN     2.2250738585072014e-308L
#define LDBL_MAX     1.7976931348623157e+308L
#define LDBL_EPSILON 2.22044604925031308085e-16L

#define LDBL_MANT_DIG 53
#define LDBL_MIN_EXP (-1021)
#define LDBL_MAX_EXP 1024

#define LDBL_DIG 15
#define LDBL_MIN_10_EXP (-307)
#define LDBL_MAX_10_EXP 308

#define DECIMAL_DIG 17
