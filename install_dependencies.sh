#!/bin/bash
# ----------------------------------------------------------------------------
# RecordLabC - User dependency installer
#
# Users can double-click this script or run it from a terminal. It installs the
# OS/Python dependencies first, then prepares the project-local XREAL runtime.
# ----------------------------------------------------------------------------

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

echo "=================================================="
echo "    RecordLabC 一键依赖安装"
echo "=================================================="

"${SCRIPT_DIR}/setup.sh" "$@"

if [[ "${RECORDLABC_SKIP_XREAL_RUNTIME:-0}" == "1" ]]; then
  echo "已跳过 XREAL runtime 初始化：RECORDLABC_SKIP_XREAL_RUNTIME=1"
  exit 0
fi

echo "=================================================="
echo "    安装 XREAL runtime / xreal_glasses wheel"
echo "=================================================="
"${SCRIPT_DIR}/setup_xreal_runtime.sh"

echo "=================================================="
echo "依赖安装完成，可以运行:"
echo "  ./RecordLabC.sh"
echo "=================================================="
