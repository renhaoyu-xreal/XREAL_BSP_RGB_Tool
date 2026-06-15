#!/bin/bash
# 兼容旧版使用习惯：直接转发到新的 robust launcher。

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
exec "${SCRIPT_DIR}/run.sh" "$@"
