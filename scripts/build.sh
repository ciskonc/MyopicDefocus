#!/usr/bin/env bash
# ==========================================
# build.sh — 用 LLVM MinGW 构建（免 UAC / 免 Visual Studio 工作负载）
# ==========================================
# 用法:
#   bash scripts/build.sh            # 构建全部
#   bash scripts/build.sh tests      # 只构建测试 exe
#
# 工具链位置可用环境变量覆盖：
#   TC=/path/to/llvm-mingw-... bash scripts/build.sh
set -euo pipefail

PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$PROJ_ROOT/src/src"
BUILD="$PROJ_ROOT/build"

TC="${TC:-/c/Users/Gamer/Desktop/agent/03_TOOLS/toolchains/llvm-mingw-20261104-ucrt-x86_64}"
CXX="$TC/bin/x86_64-w64-mingw32-clang++.exe"
WINDRES="$TC/bin/x86_64-w64-mingw32-windres.exe"
INCLUDE="$TC/include"

if [ ! -x "$CXX" ]; then
  echo "找不到编译器: $CXX" >&2
  echo "请设置 TC 环境变量指向 llvm-mingw 目录" >&2
  exit 1
fi

mkdir -p "$BUILD"

# clang.exe 是原生 Windows 程序，拿 MSYS 的 /tmp 会导致 "unable to make temporary file"。
# 这里把 TEMP/TMP 指向项目内可写目录，并转成 Windows 路径。
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
  -I "$INCLUDE"
  -I "$SRC"
  -I "$SRC/third_party"
)

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

  # 资源脚本（图标 + 版本信息）→ windres
  echo "    编译资源 app.rc ..."
  "$WINDRES" --codepage 65001 -I "$SRC" -I "$INCLUDE" -O coff \
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
  # 去掉 Low 强制标签：文件标签会把进程 IL 压到 Low，
  # 导致向 explorer.exe 发消息被 UIPI 拦截 → 托盘图标建不出来。
  icacls "$BUILD/MyopicDefocus.exe" /setintegritylevel M >/dev/null 2>&1 || true
  icacls "$BUILD/MyopicDefocusTests.exe" /setintegritylevel M >/dev/null 2>&1 || true
  echo "    OK: $BUILD/MyopicDefocus.exe (integrity level set to Medium)"
}

case "${1:-all}" in
  tests) build_tests ;;
  app)   build_app ;;
  test)  build_tests
         # DSH 的 Job Object 会禁止子进程写工作区之外的目录（如 %APPDATA%），
         # 实测 CreateFileW 直接返回 ERROR_ACCESS_DENIED。
         # 因此测试时把 APPDATA 指向工作区内的可写目录；真实用户环境不受影响。
         TEST_APPDATA="$PROJ_ROOT/build/testappdata"
         mkdir -p "$TEST_APPDATA"
         APPDATA="$(cygpath -w "$TEST_APPDATA")" "$BUILD/MyopicDefocusTests.exe" ;;
  all)   build_tests; build_app ;;
  *) echo "未知目标: $1 (可用: tests / app / test / all)" >&2; exit 1 ;;
esac

echo "==> 构建完成"
