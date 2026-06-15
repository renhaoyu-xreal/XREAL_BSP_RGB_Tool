#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "[XREAL_BSP_RGB_Tool] 安装依赖"
exec "${PROJECT_ROOT}/install_dependencies.sh" "$@"
