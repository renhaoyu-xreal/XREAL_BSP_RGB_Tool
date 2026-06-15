#!/bin/bash
# ----------------------------------------------------------------------------
# RecordLabC - Doctor Script
#
# 无界面启动诊断入口：
# 1. 检查本地配置/指南/第三方资产是否可见
# 2. 输出 BSP/XREAL 预检结果
# 3. 给新机器上的排障一个统一入口
# ----------------------------------------------------------------------------

set -euo pipefail

PROJECT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="${PROJECT_DIR}/build"
BIN_FILE="${BUILD_DIR}/recordlabc_doctor"

if [ ! -f "${BIN_FILE}" ]; then
    echo "❌ 找不到 ${BIN_FILE}"
    echo "请先运行 ./build.sh"
    exit 1
fi

export RECORDLABC_ROOT="${PROJECT_DIR}"
cd "${PROJECT_DIR}"
exec "${BIN_FILE}" "$@"
