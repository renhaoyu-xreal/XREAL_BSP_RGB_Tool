#!/bin/bash

set -euo pipefail

PROJECT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
export RECORDLABC_ROOT="${PROJECT_DIR}"

echo "=================================================="
echo "    准备 RecordLabC XREAL Runtime"
echo "=================================================="

cd "${PROJECT_DIR}"

if [[ -n "${RECORDLABC_XREAL_PYTHON:-}" ]]; then
  PYTHON_BIN="${RECORDLABC_XREAL_PYTHON}"
elif [[ -x /usr/bin/python3.10 ]]; then
  PYTHON_BIN="/usr/bin/python3.10"
elif [[ -x /usr/bin/python3 ]]; then
  PYTHON_BIN="/usr/bin/python3"
elif command -v python3 >/dev/null 2>&1; then
  PYTHON_BIN="$(command -v python3)"
else
  echo "未找到可用的 python3，请先安装 Python 3.10+。" >&2
  exit 2
fi

exec "${PYTHON_BIN}" scripts/bootstrap_xreal_runtime.py "$@"
