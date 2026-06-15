#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "[XREAL_BSP_RGB_Tool] 更新项目"
if [[ ! -d "${PROJECT_ROOT}/.git" ]]; then
  echo "[XREAL_BSP_RGB_Tool] 当前目录不是 Git 仓库，无法自动更新。" >&2
  exit 1
fi

git -C "${PROJECT_ROOT}" pull --ff-only
exec "${PROJECT_ROOT}/tool_scripts/install_dependencies.sh"
