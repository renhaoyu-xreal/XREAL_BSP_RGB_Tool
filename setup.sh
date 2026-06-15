#!/bin/bash
# ----------------------------------------------------------------------------
# RecordLab C++ - Environment Setup Script
#
# 这个脚本用于一键安装 Ubuntu 22.04 上的 C++ 和 Qt6 编译依赖。
# 它相当于 Python 里的 `pip install -r requirements.txt`。
# ----------------------------------------------------------------------------

set -euo pipefail

PROJECT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

echo "=================================================="
echo "    RecordLab C++ Environment Setup"
echo "=================================================="

if [ "$EUID" -ne 0 ]; then
  if command -v sudo >/dev/null 2>&1 && { [ -t 0 ] || [ -n "${SUDO_ASKPASS:-}" ]; }; then
    exec sudo -E bash "$0" "$@"
  fi

  if command -v pkexec >/dev/null 2>&1; then
    exec pkexec env DISPLAY="${DISPLAY:-}" XAUTHORITY="${XAUTHORITY:-}" bash "$0" "$@"
  fi

  if command -v sudo >/dev/null 2>&1; then
    exec sudo -E bash "$0" "$@"
  fi

  echo "请使用管理员权限运行此脚本，例如: sudo ${PROJECT_DIR}/setup.sh" >&2
  exit 1
fi

cd "${PROJECT_DIR}"

echo "[1/5] 更新 apt 包管理器列表..."
apt-get update -y

echo "[2/5] 安装基础构建工具与本机播放工具 (GCC, CMake, Ninja, SSHPass, ADB, MPV, Python Pip)..."
apt-get install -y \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  sshpass \
  adb \
  mpv \
  alsa-utils \
  lsof \
  rsync \
  python3-pip \
  python3-venv \
  xterm

echo "[3/5] 安装 Qt 6 和 OpenGL 开发库..."
# Ubuntu 22.04 默认仓库里的 Qt 6.2 对当前工程足够。
# 这里额外显式安装 OpenGL 相关开发头，避免 CMake 在 find_package(Qt6 ...)
# 阶段因为缺少 WrapOpenGL / OpenGL_INCLUDE_DIR 而把 Qt6Gui / Qt6Widgets 判定为不可用。
apt-get install -y \
  qt6-base-dev \
  qt6-declarative-dev \
  libqt6network6 \
  libgl-dev \
  libopengl-dev \
  libegl1-mesa-dev \
  libglx-dev \
  libxcb-cursor0 \
  libxcb-cursor-dev \
  libxcb-xinerama0 \
  libxcb-xinerama0-dev

echo "[4/5] 安装 ZeroMQ 开发库..."
apt-get install -y libzmq3-dev

echo "[5/5] 安装 Python 编排层依赖..."
# 说明：
# 1. C++ 主体已经不依赖旧版 RecordLab；
# 2. 但 Tab1 / Tab4 的 BSP 脚本兼容运行时仍然需要这些 Python 包；
# 3. 这里显式把 pyzmq 和 vendored message_system 装上，避免脚本页在新机器上启动即报错。
python3 -m pip install --upgrade pip
python3 -m pip install pyzmq paramiko numpy Pillow PySide6
python3 -m pip install -e ./third_party/echo_message_system/python

echo "=================================================="
echo "环境依赖安装完成！"
echo "下一步建议执行:"
echo "  ./build.sh"
echo "  ./run.sh"
echo "=================================================="
