#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "[XREAL_BSP_RGB_Tool] 启动 BSP RGB 工具"
export RECORDLABC_ROOT="${PROJECT_ROOT}"
exec "${PROJECT_ROOT}/run.sh" "$@"
