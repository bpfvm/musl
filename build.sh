#!/bin/sh
# 编译 musl 为 BPF target 的静态库（lib/libc.a）。
#
# 用法：  sh musl/build.sh [install_prefix]
# 产物：  <prefix>/lib/libc.a（已合并 crt，含 _start）+ <prefix>/include 头
#         install_prefix 默认 $TOP/root（与 build_root.sh / test/Makefile 共用 root/{include,lib}）
#
# 依赖：  clang（>=19）、本项目 libBpfWideArgs.so 与 libBpfSoftFp.so（由 bpfvm 的 cmake 构建产出）。
#
# 设计要点（详见 musl/arch/bpf/ 各头文件注释）：
#   - ARCH=bpf 由 configure 的 bpf*) 分支识别（musl/configure 已加）。
#   - CFLAGS 注入 -target bpf + 两个 pass 插件（BpfWideArgs 突破 5 参数/struct
#     返回/变参限制，BpfSoftFp 把浮点 IR 改写成 BPF_FP_* call）。
#   - --disable-shared：本 VM 不支持动态 musl，只编静态库。
#   - LIBCC 置空：BPF 无 gcc 运行时（__adddf3 等由 BpfSoftFp + VM 处理）。
#
set -e

MUSL_DIR="$(cd "$(dirname "$0")" && pwd)"
TOP="$(cd "$MUSL_DIR/.." && pwd)"
BUILD="$MUSL_DIR/build"
PREFIX="${1:-$TOP/root}"

# pass 插件路径（优先用项目 build/ 下的，回退到环境变量）
WIDEARGS="${WIDEARGS:-$TOP/build/libBpfWideArgs.so}"
SOFTFP="${SOFTFP:-$TOP/build/libBpfSoftFp.so}"

CFLAGS="-target bpf -mcpu=v4 -O1 -fno-builtin -Werror=macro-redefined \
  -mllvm -bpf-stack-size=16384 \
  -Xclang -target-feature -Xclang +alu32 \
  -Xclang -target-feature -Xclang -dwarfris \
  -Wno-unused-command-line-argument \
  -fpass-plugin=$WIDEARGS \
  -fpass-plugin=$SOFTFP \
  -fstack-size-section \
  -I$TOP/root/include"

echo "==> configuring musl (ARCH=bpf) in $BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

# AR/RANLIB 必须显式指宿主 GNU 工具：configure 从 --target=bpf 推导
# CROSS_COMPILE=bpf-，不覆盖则内部规则调 bpf-ar/bpf-ranlib（binutils-bpf）。
# BPF 对象是标准 ELF64，宿主 ar 可直接打包。
CC=clang AR=ar RANLIB=ranlib CFLAGS="$CFLAGS" LDFLAGS="" LIBCC="" \
  sh "$MUSL_DIR/configure" --target=bpf --disable-shared --prefix="$PREFIX"

# configure 的 tryflag 用 C 文件测 -Wa,--noexecstack，C 编译不触发汇编器路径，
# 误判为 yes。但 BPF clang 集成汇编器对 .s 文件加该 flag 会 segfault（LLVM bug）。
# musl 其他 arch 的 .s 不受影响（GNU as 接受），BPF arch 有 .s（clone.s）须移除。
sed -i 's/ -Wa,--noexecstack//g' "$BUILD/config.mak"

echo "==> building libc.a"
make -j"$(nproc)" lib/libc.a

# 只构建 crt1.o。其他 crt 在 BPF 上不需要：
#   - crti.o/crtn.o：BPF 无 .init_array/.fini_array 框架，编出来是空 .o，merge 进
#     libc.a 不贡献任何代码/符号，删掉省事。
#   - Scrt1.o：BPF clang 对地址引用本来就走重定位（lddw 带 R_BPF_64_64、call 带
#     R_BPF_64_32），-fPIC 与否对 crt1 这种简单代码产出一致；crt1.o 已能覆盖
#     PIE/.so 场景，不需要 Scrt1.o。
#   - rcrt1.o：静态 PIE 自启动，依赖 dlstart 动态链接器逻辑，BPF 不支持，编译必
#     失败（_start_c 签名冲突 + GETFUNCSYM 无前向声明）。
# make install 会连带编译 rcrt1.o 导致失败，故只单独构建 crt1.o，cp 到 lib/。
# 把 crt1 merge 进 libc.a，使其像 pdclib 那样自带 _start——链接时只链 libc.a 即可，
# 无需显式传 crt、无需关心顺序。
echo "==> building crt1.o"
make obj/crt/crt1.o

echo "==> merging crt1 into libc.a"
ar rcs lib/libc.a obj/crt/crt1.o

# 编译 ldso（动态链接器）的 object：dlstart.lo / dynlink.lo。
# 标准 musl 把 ldso 代码放进 libc.so 而非 libc.a（静态链接的程序不需要 ldso），
# 故上面的 lib/libc.a 不含它们。BPF 的 ldso（ld-bpf.so）由 build_root.sh 从
# libc.a + 这两个 .lo 合成，故此处单独编译并安装。用 musl 自带 make 规则（它知道
# 正确的 -fPIC + pass 插件 flags）。
echo "==> building ldso objects (dlstart.lo, dynlink.lo)"
make obj/ldso/dlstart.lo obj/ldso/dynlink.lo

echo "==> installing headers + lib to $PREFIX"
# 手动安装（make install 会编译所有 crt 含 rcrt1.o 必失败）：
# 头到 <prefix>/include，库到 <prefix>/lib（libc.a 已合并 crt1 含 _start）。
# 默认 <prefix>=$TOP/root：与 build_root.sh / test/Makefile / scripts 共用同一份
# 安装目录（-isystem root/include、-L root/lib），不再经项目根 libc 软链抽象。
#
# bits/ 头来源：musl 的 arch 特化头在 arch/bpf/bits/，通用头在 arch/generic/bits/
# （fcntl.h/errno.h/dirent.h 等空占位或通用定义）。BPF arch 只覆盖了部分 bits
# （syscall.h.in/stat.h/signal.h 等），其余须从 generic 取。cp 顺序：先 generic，
# 后 bpf（bpf 特化覆盖 generic）。只装 bits/——arch 下的内部头（syscall_arch.h/
# atomic_arch.h 等）不属于公共 sysroot。
mkdir -p "$PREFIX/include/bits" "$PREFIX/lib"
cp -r obj/include/* "$PREFIX/include/"
cp -r "$MUSL_DIR/include/"* "$PREFIX/include/"
cp -r "$MUSL_DIR/arch/generic/bits/"* "$PREFIX/include/bits/" 2>/dev/null || true
cp -r "$MUSL_DIR/arch/bpf/bits/"* "$PREFIX/include/bits/" 2>/dev/null || true
cp -f lib/libc.a "$PREFIX/lib/"
# 子库（libm.a/librt.a/libpthread.a/...）软链接到 libc.a：musl 把所有符号都合并在
# libc.a 里，-lm/-lrt/-lpthread 等即等价于 -lc。软链接而非空归档，是为了 busybox trylink
# 等只传 -lm（不带 -lc）的场景能解析到 libc 符号（strlen/stat/...）。
for sub in m rt pthread crypt util xnet resolv dl; do
    ln -sf libc.a "$PREFIX/lib/lib${sub}.a"
done
# 安装 ldso object（供 build_root.sh 合成 ld-bpf.so 用）。
cp -f obj/ldso/dlstart.lo obj/ldso/dynlink.lo "$PREFIX/lib/"

echo "done: $PREFIX/lib/libc.a (含 _start)"
