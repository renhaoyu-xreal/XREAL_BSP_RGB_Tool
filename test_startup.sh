#!/bin/bash
# ----------------------------------------------------------------------------
# RecordLabC - Startup Smoke Test
#
# 这个脚本的目的不是验证“BSP 真机录制已经完成”，而是一次性完成：
# 1. 构建
# 2. 无界面 doctor 诊断
# 3. BSP 子节点本地预检
# 4. Python 环境与脚本诊断
# 5. Qt 主程序无头启动烟测
# ----------------------------------------------------------------------------

set -euo pipefail

PROJECT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="${PROJECT_DIR}/build"

echo "=================================================="
echo "    RecordLabC Startup Smoke Test"
echo "=================================================="

echo "[1/5] 编译工程..."
"${PROJECT_DIR}/build.sh"

echo "[2/5] 运行 doctor 诊断..."
"${PROJECT_DIR}/doctor.sh"

echo "[3/5] 运行 BSP 本地预检..."
set +e
"${BUILD_DIR}/bsp_main_subnode" --preflight
PREFLIGHT_EXIT=$?
set -e

if [ "${PREFLIGHT_EXIT}" -eq 0 ]; then
    echo "✓ BSP 预检通过，当前环境已满足 createGlasses 前提。"
elif [ "${PREFLIGHT_EXIT}" -eq 2 ]; then
    echo "⚠ BSP 预检已输出阻塞项。"
    echo "  这不影响启动烟测继续执行，但表示真机链路仍有硬阻塞。"
else
    echo "❌ BSP 预检执行异常，退出码: ${PREFLIGHT_EXIT}"
    exit "${PREFLIGHT_EXIT}"
fi

echo "[4/5] 运行 Python 环境与脚本诊断..."
python3 "${PROJECT_DIR}/scripts/check_environment.py" \
    --project-root "${PROJECT_DIR}" \
    --strict

echo "[5/5] 执行无头启动烟测..."
set +e
timeout 6s env QT_QPA_PLATFORM=offscreen "${PROJECT_DIR}/run.sh"
RUN_EXIT=$?
set -e

if [ "${RUN_EXIT}" -eq 0 ]; then
    echo "✓ 主程序在烟测窗口内自行退出。"
elif [ "${RUN_EXIT}" -eq 124 ]; then
    echo "✓ 主程序已稳定运行 6 秒，无头烟测通过。"
else
    echo "❌ 主程序无头烟测失败，退出码: ${RUN_EXIT}"
    exit "${RUN_EXIT}"
fi

echo "=================================================="
echo "烟测结束。"
echo "如果要手动启动 UI: ./run.sh"
echo "如果要看诊断 JSON: ./doctor.sh --json"
echo "=================================================="
