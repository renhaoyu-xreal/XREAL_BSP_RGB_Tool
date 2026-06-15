#!/bin/bash
# ----------------------------------------------------------------------------
# RecordLab C++ - Build Script
#
# 隔离的编译脚本。所有编译产物都在 build/ 文件夹内，不会污染代码区。
# 相当于 Python 里的无副作用虚拟环境。
# ----------------------------------------------------------------------------

set -euo pipefail

# 获取当前脚本所在目录作为工程根目录
PROJECT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="${PROJECT_DIR}/build"

echo "=================================================="
echo "    RecordLab C++ Build Engine"
echo "=================================================="

print_opengl_hint() {
    echo ""
    echo "检测到当前机器缺少 Qt6 构建所需的 OpenGL 开发依赖。"
    echo "在 Ubuntu 22.04 上请先执行："
    echo "  sudo apt-get update"
    echo "  sudo apt-get install -y libgl-dev libopengl-dev libegl1-mesa-dev libglx-dev"
    echo ""
    echo "如果这台机器是第一次配置，直接执行："
    echo "  sudo ./setup.sh"
    echo ""
}

# 如果加了 --clean 参数，则深度清理
if [ "${1:-}" == "--clean" ]; then
    echo "清理旧的构建文件..."
    rm -rf "${BUILD_DIR}"
fi

# 创建并进入隔离的 build 目录
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

echo "=> 正在配置 CMake (生成 Ninja 构建配置)..."
# 使用 Ninja 构建系统（比 Make 快很多）
CONFIGURE_LOG="${BUILD_DIR}/cmake_configure.log"
rm -f "${CONFIGURE_LOG}"
set +e
cmake -G Ninja .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tee "${CONFIGURE_LOG}"
CMAKE_EXIT=${PIPESTATUS[0]}
set -e
if [ "${CMAKE_EXIT}" -ne 0 ]; then
    if grep -Eq "WrapOpenGL|OPENGL_INCLUDE_DIR|Qt6Gui could not be found because dependency WrapOpenGL" "${CONFIGURE_LOG}"; then
        print_opengl_hint
    fi
    exit "${CMAKE_EXIT}"
fi

echo "=> 正在编译代码 (多核并行)..."
cmake --build .

echo "=================================================="
echo "编译成功！"
echo "可执行文件位于: ${BUILD_DIR}/recordlabc"
echo "你可以通过运行 ./run.sh 来启动控制中心。"
echo "=================================================="
