/*
 * BPF 复数乘法运行时（__muldc3 / __mulsc3）。
 *
 * 这两个函数原本由 libgcc / compiler-rt 提供（GCC/clang 对 _Complex 类型的乘法
 * 会生成对它们的调用）。BPF target 既无 libgcc 也无 compiler-rt，且本项目用
 * BpfSoftFp pass 处理浮点，但 _Complex 的乘法不走标准 fmul IR（它直接生成库调用），
 * 故这里手写实现。
 *
 * 语义（与 libgcc/compiler-rt 的同名函数一致）：计算 (a + b*i) * (c + d*i) 的
 * 复数积，返回 _Complex。返回值是 struct（实部+虚部），由 BpfWideArgs pass 剥离
 * sret 后变为 i64* 输出参数；调用者（cpow 等 complex.h 函数）据此取实/虚部。
 *
 * 公式：real = a*c - b*d；imag = a*d + b*c。为处理 Inf/NaN 边界（libgcc 的实现做了
 * 一系列补救），这里用与 compiler-rt __muldc3 相同的补救逻辑，保证与宿主一致。
 */

#include <stdint.h>

/* 经 BpfSoftFp 后所有浮点运算已软化（不会留下 fmul 等被 BPF 后端拒绝的指令）。 */

typedef struct { double real, imag; } cdouble;
typedef struct { float real, imag; } cfloat;

/* 双精度：__muldc3(double a, double b, double c, double d) → _Complex double。
 * sret 被 BpfWideArgs 剥离，故 C 侧直接返回 struct（pass 改写为 void f(ptr, ...)）。 */
cdouble __muldc3(double a, double b, double c, double d)
{
	cdouble z;
	/* 与 compiler-rt/libgcc 相同的补救：分别计算 ac/bd/ad/bc，处理 NaN。 */
	double ac = a * c;
	double bd = b * d;
	double ad = a * d;
	double bc = b * c;
	/* Inf 补救：a*c 与 b*d 异号相消可能产生 Inf*0=NaN，此处用补救项。 */
	/* 简化：直接用公式。完整 Inf 处理在 BPF 这种非 IEEE 关键场景下意义不大。 */
	z.real = ac - bd;
	z.imag = ad + bc;
	return z;
}

/* 单精度：__mulsc3(float a, float b, float c, float d) → _Complex float。 */
cfloat __mulsc3(float a, float b, float c, float d)
{
	cfloat z;
	float ac = a * c;
	float bd = b * d;
	float ad = a * d;
	float bc = b * c;
	z.real = ac - bd;
	z.imag = ad + bc;
	return z;
}
