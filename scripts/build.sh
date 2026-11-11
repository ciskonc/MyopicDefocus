#!/usr/bin/env bash
# ==========================================
# build.sh — 用 LLVM MinGW 构建（免 Visual Studio / 免管理员）
# ==========================================
# 用法:
#   bash scripts/build.sh            # 构建全部（测试 + 主程序）
#   bash scripts/build.sh tests      # 只构建测试 exe
#   bash scripts/build.sh app        # 只构建主程序 exe
#   bash scripts/build.sh test       # 构建 + 运行自检
#
# 工具链：需要 LLVM MinGW（https://github.com/mstorsjo/llvm-mingw/releases）。
# 用环境变量 TC 指定工具链目录：
#   TC=/path/to/llvm-mingw-ucrt-x86_64 bash scripts/build.sh
set -euo pipefail

PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$PROJ_ROOT/src"
BUILD="$PROJ_ROOT/build"

# —— 工具链定位：TC 环境变量优先，其次 PATH 探测 ——
if [ -z "${TC:-}" ]; then
  CXX_ON_PATH="$(command -v x86_64-w64-mingw32-clang++.exe || command -v x86_64-w64-mingw32-clang++ || true)"
  if [ -n "$CXX_ON_PATH" ]; then
    TC="$(cd "$(dirname "$CXX_ON_PATH")/.." && pwd)"
  fi
fi
TC="${TC:-}"
CXX="${TC:+$TC/bin/}x86_64-w64-mingw32-clang++.exe"
WINDRES="${TC:+$TC/bin/}x86_64-w64-mingw32-windres.exe"
INCLUDE="${TC:+$TC/include}"

if ! command -v "$CXX" >/dev/null 2>&1 && [ ! -x "$CXX" ]; then
  echo "找不到 LLVM MinGW 编译器。" >&2
  echo "请从 https://github.com/mstorsjo/llvm-mingw/releases 下载解压后设置：" >&2
  echo "  TC=/path/to/llvm-mingw-ucrt-x86_64 bash scripts/build.sh" >&2
  exit 1
fi

mkdir -p "$BUILD"

# clang.exe 是原生 Windows 程序，拿 MSYS 的 /tmp 会导致 "unable to make temporary file"。
# 把 TEMP/TMP 指到项目内可写目录，并转成 Windows 路径。
TMP_WORK="${TMP_WORK:-$PROJ_ROOT/build/tmp}"
mkdir -p "$TMP_WORK"
if command -v cygpath >/dev/null 2>&1; then
  export TMP="$(cygpath -w "$TMP_WORK")"
else
  export TMP="$TMP_WORK"
fi
export TEMP="$TMP"
export TMPDIR="$TMP"

COMMON_FLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -Werror=unknown-escape-sequence
  -Werror=unknown-pragmas
  -Wno-unused-parameter

  -finput-charset=UTF-8
  -fexec-charset=UTF-8
  -DUNICODE -D_UNICODE
  -DWIN32_LEAN_AND_MEAN
  -D_WIN32_WINNT=0x0A00
  -DWINVER=0x0A00
  -DNTDDI_VERSION=0x0A000000
  -I "$SRC"
  -I "$SRC/third_party"
)
[ -n "$INCLUDE" ] && COMMON_FLAGS+=(-I "$INCLUDE")

LINK_FLAGS=(
  -static
  -static-libgcc
  -static-libstdc++
  -ld3d11
  -ldxgi
  -ld3dcompiler
  -ldwmapi
  -lshell32
  -lshlwapi
  -lole32
  -luuid
  -lcomctl32
  -ladvapi32
  -luser32
  -lgdi32
)

CORE_SRCS=(
  "$SRC/core/config.cpp"
  "$SRC/core/screens.cpp"
  "$SRC/core/diag_probe.cpp"
  "$SRC/optical_model.cpp"
)

TEST_SRCS=(
  "$SRC/ui/wizard.cpp"
  "$SRC/test_optical_model.cpp"
  "$SRC/test_config_io.cpp"
  "$SRC/test_wizard_template.cpp"
  "$SRC/app/selftest.cpp"
  "$SRC/app/test_main.cpp"
  "$SRC/app/util.cpp"
)

build_tests() {
  echo "==> 构建 MyopicDefocusTests.exe (console)"
  "$CXX" "${COMMON_FLAGS[@]}" \
    "${CORE_SRCS[@]}" "${TEST_SRCS[@]}" \
    -o "$BUILD/MyopicDefocusTests.exe" \
    "${LINK_FLAGS[@]}"
  echo "    OK: $BUILD/MyopicDefocusTests.exe"
}

build_app() {
  echo "==> 构建 MyopicDefocus.exe (GUI, 单 exe 自包含)"

  echo "    编译资源 app.rc ..."
  "$WINDRES" -I "$SRC" ${INCLUDE:+-I "$INCLUDE"} -O coff \
    -o "$BUILD/app_res.o" "$SRC/app/app.rc"

  local APP_SRCS=(
    "$SRC/app/main.cpp"
    "$SRC/app/util.cpp"
    "$SRC/app/integrity.cpp"
    "$SRC/app/selftest.cpp"
    "$SRC/gpu/gpu.cpp"
    "$SRC/ui/tray.cpp"
    "$SRC/ui/panel.cpp"
    "$SRC/ui/wizard.cpp"
    "$SRC/ui/controlbar.cpp"
  )
  "$CXX" "${COMMON_FLAGS[@]}" \
    -municode -mwindows \
    "${CORE_SRCS[@]}" "${APP_SRCS[@]}" \
    "$SRC/test_optical_model.cpp" \
    "$SRC/test_config_io.cpp" \
    "$SRC/test_wizard_template.cpp" \
    "$BUILD/app_res.o" \
    -o "$BUILD/MyopicDefocus.exe" \
    "${LINK_FLAGS[@]}"
  echo "    OK: $BUILD/MyopicDefocus.exe"
}

case "${1:-all}" in
  tests) build_tests ;;
  app)   build_app ;;
  test)  build_tests
         # CI/受限环境下 %APPDATA% 可能不可写：把 APPDATA 指向项目内目录跑自检
         TEST_APPDATA="$PROJ_ROOT/build/testappdata"
         mkdir -p "$TEST_APPDATA"
         if command -v cygpath >/dev/null 2>&1; then
           APPDATA="$(cygpath -w "$TEST_APPDATA")" "$BUILD/MyopicDefocusTests.exe"
         else
           APPDATA="$TEST_APPDATA" "$BUILD/MyopicDefocusTests.exe"
         fi ;;
  all)   build_tests; build_app ;;
  *) echo "未知目标: $1 (可用: tests / app / test / all)" >&2; exit 1 ;;
esac

echo "==> 构建完成"
