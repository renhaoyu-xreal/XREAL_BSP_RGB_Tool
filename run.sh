#!/bin/bash
# ----------------------------------------------------------------------------
# RecordLabC - Robust Launcher
#
# 默认启动前会执行：
# 1. 清理上次残留的主 GUI / 子节点 / worker / XREAL bridge
# 2. 清理占用 RecordLabC 专用端口的旧进程
# 3. 清理相机共享内存相关的 Qt IPC 残留文件
# 4. 设置运行环境并启动主程序
# ----------------------------------------------------------------------------

set -Eeuo pipefail

PROJECT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="${PROJECT_DIR}/build"
BIN_FILE="${BUILD_DIR}/recordlabc"

SKIP_CLEANUP="${RECORDLABC_SKIP_CLEANUP:-0}"
TERM_TIMEOUT_SECONDS="${RECORDLABC_TERM_TIMEOUT_SECONDS:-5}"
APP_ARGS=()

usage() {
    cat <<'EOF'
Usage: ./run.sh [options] [-- <recordlabc args>]

Options:
  --skip-cleanup   不清理上次残留进程，直接启动
  -h, --help       显示帮助

Env:
  RECORDLABC_SKIP_CLEANUP=1       与 --skip-cleanup 等价
  RECORDLABC_TERM_TIMEOUT_SECONDS 设置清理旧进程时的等待秒数，默认 5
  RECORDLABC_DEBUG=1              打开 Qt debug 日志
EOF
}

while (($# > 0)); do
    case "$1" in
        --skip-cleanup)
            SKIP_CLEANUP=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            APP_ARGS+=("$@")
            break
            ;;
        *)
            APP_ARGS+=("$1")
            shift
            ;;
    esac
done

log() {
    printf '[run.sh] %s\n' "$*"
}

warn() {
    printf '[run.sh] WARNING: %s\n' "$*" >&2
}

die() {
    printf '[run.sh] ERROR: %s\n' "$*" >&2
    exit 1
}

have_cmd() {
    command -v "$1" >/dev/null 2>&1
}

pid_is_excluded() {
    local pid="$1"
    [[ -z "$pid" ]] && return 0
    [[ "$pid" == "$$" ]] && return 0
    [[ "$pid" == "$PPID" ]] && return 0
    [[ -n "${BASHPID:-}" && "$pid" == "${BASHPID}" ]] && return 0
    return 1
}

declare -A TARGET_PIDS=()

add_target_pid() {
    local pid="$1"
    local reason="$2"

    [[ "$pid" =~ ^[0-9]+$ ]] || return 0
    pid_is_excluded "$pid" && return 0
    kill -0 "$pid" 2>/dev/null || return 0

    if [[ -n "${TARGET_PIDS[$pid]:-}" ]]; then
        TARGET_PIDS["$pid"]+=", ${reason}"
    else
        TARGET_PIDS["$pid"]="${reason}"
    fi
}

collect_pids_by_substring() {
    local needle="$1"
    local label="$2"

    while read -r pid; do
        add_target_pid "$pid" "$label"
    done < <(
        ps -eo pid=,args= \
        | awk -v needle="$needle" '
            index($0, needle) &&
            index($0, "awk -v needle=") == 0 &&
            index($0, "ps -eo pid=,args=") == 0 {
                print $1
            }'
    )
}

collect_port_listener_pids() {
    local port="$1"
    local label="tcp:${port}"

    if have_cmd lsof; then
        while read -r pid; do
            add_target_pid "$pid" "$label"
        done < <(lsof -tiTCP:"${port}" -sTCP:LISTEN 2>/dev/null || true)
        return
    fi

    if have_cmd fuser; then
        while read -r pid; do
            add_target_pid "$pid" "$label"
        done < <(fuser -n tcp "${port}" 2>/dev/null | tr ' ' '\n' || true)
    fi
}

kill_collected_pids() {
    local pids=("${!TARGET_PIDS[@]}")
    local survivors=()

    if [[ ${#pids[@]} -eq 0 ]]; then
        log "未发现需要清理的残留 RecordLabC 进程。"
        return 0
    fi

    log "清理上次运行残留的 RecordLabC 进程..."
    for pid in "${pids[@]}"; do
        local cmdline
        cmdline="$(ps -p "$pid" -o args= 2>/dev/null | sed 's/^[[:space:]]*//' || true)"
        printf '  - TERM pid=%s [%s] %s\n' "$pid" "${TARGET_PIDS[$pid]}" "${cmdline}"
        kill -TERM "$pid" 2>/dev/null || true
    done

    local deadline=$((SECONDS + TERM_TIMEOUT_SECONDS))
    while (( SECONDS < deadline )); do
        local alive=0
        for pid in "${pids[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then
                alive=1
                break
            fi
        done
        (( alive == 0 )) && break
        sleep 0.2
    done

    for pid in "${pids[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            survivors+=("$pid")
        fi
    done

    if [[ ${#survivors[@]} -eq 0 ]]; then
        return 0
    fi

    warn "以下进程在 ${TERM_TIMEOUT_SECONDS}s 内未退出，改为强制杀掉："
    for pid in "${survivors[@]}"; do
        local cmdline
        cmdline="$(ps -p "$pid" -o args= 2>/dev/null | sed 's/^[[:space:]]*//' || true)"
        printf '  - KILL pid=%s [%s] %s\n' "$pid" "${TARGET_PIDS[$pid]}" "${cmdline}"
        kill -KILL "$pid" 2>/dev/null || true
    done

    sleep 0.2
}

cleanup_qt_ipc_artifacts() {
    local patterns=(
        "/tmp/qipc_*RecordLabC_CameraSHM*"
        "/dev/shm/qipc_*RecordLabC_CameraSHM*"
    )
    local removed=0

    for pattern in "${patterns[@]}"; do
        while IFS= read -r path; do
            [[ -e "$path" ]] || continue
            if rm -f -- "$path" 2>/dev/null; then
                removed=$((removed + 1))
                log "已清理 IPC 残留: ${path}"
            fi
        done < <(compgen -G "$pattern" || true)
    done

    if (( removed == 0 )); then
        log "未发现需要清理的 Qt IPC 残留。"
    fi
}

cleanup_stale_runtime() {
    declare -a stale_patterns=(
        "${BIN_FILE}"
        "${BUILD_DIR}/bsp_main_subnode"
        "${BUILD_DIR}/helen_main_subnode"
        "${BUILD_DIR}/imu_sim_main_subnode"
        "${BUILD_DIR}/nviz_node_subnode"
        "${BUILD_DIR}/android_subnode"
        "${BUILD_DIR}/recordlabc_agent_probe"
        "build/bsp_main_subnode"
        "build/helen_main_subnode"
        "build/imu_sim_main_subnode"
        "build/nviz_node_subnode"
        "build/android_subnode"
        "scripts/runtime/run_recordlab_script.py"
        "scripts/runtime/run_recording_worker.py"
        "scripts/xreal_bridge_worker.py"
    )
    declare -a stale_ports=(5590 5690 5691 5692 5693 5694 5695 5698 5699)

    TARGET_PIDS=()
    for needle in "${stale_patterns[@]}"; do
        collect_pids_by_substring "$needle" "$needle"
    done
    for port in "${stale_ports[@]}"; do
        collect_port_listener_pids "$port"
    done

    kill_collected_pids
    cleanup_qt_ipc_artifacts
}

prepare_environment() {
    if [[ -z "${QT_LOGGING_RULES:-}" ]]; then
        if [[ "${RECORDLABC_DEBUG:-0}" == "1" ]]; then
            export QT_LOGGING_RULES="*.debug=true"
        else
            export QT_LOGGING_RULES="*.debug=false"
        fi
    fi

    if [[ "${XDG_SESSION_TYPE:-}" == "wayland" && -z "${QT_QPA_PLATFORM:-}" ]]; then
        export QT_QPA_PLATFORM=xcb
        log "检测到 Wayland，会话已切换为 QT_QPA_PLATFORM=xcb 以稳定多窗口行为。"
    fi

    export RECORDLABC_ROOT="${PROJECT_DIR}"
    export PYTHONUNBUFFERED=1
    umask 022

    mkdir -p "${PROJECT_DIR}/data" "${PROJECT_DIR}/output"
}

launch_app() {
    local child_pid
    local exit_code

    log "工程根目录: ${RECORDLABC_ROOT}"
    log "执行二进制: ${BIN_FILE}"
    if [[ ${#APP_ARGS[@]} -gt 0 ]]; then
        log "透传参数: ${APP_ARGS[*]}"
    fi

    cd "${PROJECT_DIR}"
    "${BIN_FILE}" "${APP_ARGS[@]}" &
    child_pid=$!

    forward_signal() {
        local signal_name="$1"
        if kill -0 "${child_pid}" 2>/dev/null; then
            warn "收到 ${signal_name}，转发给主程序 pid=${child_pid}"
            kill "-${signal_name}" "${child_pid}" 2>/dev/null || true
        fi
    }

    trap 'forward_signal TERM' TERM
    trap 'forward_signal INT' INT

    set +e
    wait "${child_pid}"
    exit_code=$?
    set -e

    trap - TERM INT
    return "${exit_code}"
}

echo "=================================================="
echo "    启动 RecordLab C++ 控制中心"
echo "=================================================="

[[ -x "${BIN_FILE}" ]] || die "找不到可执行文件 ${BIN_FILE}，请先运行 ./build.sh"

if [[ "${SKIP_CLEANUP}" != "1" ]]; then
    cleanup_stale_runtime
else
    log "按请求跳过残留进程清理。"
fi

prepare_environment

echo "--------------------------------------------------"
launch_app
EXIT_CODE=$?

echo "--------------------------------------------------"
log "RecordLabC 退出，exit code=${EXIT_CODE}"
exit "${EXIT_CODE}"
