#!/bin/bash

set -euo pipefail

SHELL_PATH=$(dirname "$0")
source "$SHELL_PATH/glasses_config.sh"

log() {
    printf '[capture_raw_frame] %s\n' "$*" >&2
}

if [ "$#" -lt 12 ]; then
    echo "Usage: $0 <host> <port> <username> <password> <local_cameratest> <remote_cameratest> <remote_raw_path> <remote_frame_path> <remote_timestamp_path> <cleanup_before_capture> <timeout_s> <poll_interval_s> [raw_resolution] [raw_exposure_mode] [raw_exposure_value] [raw_gain]" >&2
    exit 2
fi

HOST="$1"
PORT="$2"
USERNAME="$3"
PASSWORD="$4"
LOCAL_CAMERATEST_PATH="$5"
REMOTE_CAMERATEST_PATH="$6"
REMOTE_RAW_PATH="$7"
REMOTE_FRAME_PATH="$8"
REMOTE_TIMESTAMP_PATH="$9"
CLEANUP_BEFORE_CAPTURE="${10}"
TIMEOUT_S="${11}"
POLL_INTERVAL_S="${12}"
RAW_RESOLUTION="${13:-8}"
RAW_EXPOSURE_MODE="${14:-0}"
RAW_EXPOSURE_VALUE="${15:-1}"
RAW_GAIN="${16:-1}"

if [ ! -f "$LOCAL_CAMERATEST_PATH" ]; then
    echo "Missing local cameratest: $LOCAL_CAMERATEST_PATH" >&2
    exit 1
fi

SSH_TARGET="${USERNAME}@${HOST}"
SSH_CMD=(
    sshpass -p "$PASSWORD"
    ssh
    "${SSH_COMMON_OPTIONS[@]}"
    -p "$PORT"
    -T
    "$SSH_TARGET"
)
SCP_CMD=(
    sshpass -p "$PASSWORD"
    scp
    -O
    "${SSH_COMMON_OPTIONS[@]}"
    -P "$PORT"
)

log "checking remote cameratest: $REMOTE_CAMERATEST_PATH"
if ! "${SSH_CMD[@]}" sh -s -- "$REMOTE_CAMERATEST_PATH" <<'EOF'
set -eu
remote_cameratest="$1"
[ -f "$remote_cameratest" ]
EOF
then
    log "remote cameratest missing, preparing upload"
    if ! "${SSH_CMD[@]}" sh -s -- "$REMOTE_CAMERATEST_PATH" <<'EOF'
set -eu
remote_cameratest="$1"
mkdir -p "$(dirname "$remote_cameratest")"
EOF
    then
        echo "Failed to prepare remote cameratest directory: $REMOTE_CAMERATEST_PATH" >&2
        exit 1
    fi

    if ! "${SCP_CMD[@]}" "$LOCAL_CAMERATEST_PATH" "${SSH_TARGET}:${REMOTE_CAMERATEST_PATH}"; then
        echo "Failed to upload cameratest to $REMOTE_CAMERATEST_PATH" >&2
        exit 1
    fi
    log "remote cameratest uploaded"
fi

log "executing remote cameratest via ssh"
if ! "${SSH_CMD[@]}" sh -s -- "$REMOTE_CAMERATEST_PATH" "$REMOTE_RAW_PATH" "$REMOTE_FRAME_PATH" "$REMOTE_TIMESTAMP_PATH" "$CLEANUP_BEFORE_CAPTURE" "$RAW_RESOLUTION" "$RAW_EXPOSURE_MODE" "$RAW_EXPOSURE_VALUE" "$RAW_GAIN" <<'EOF'
set -eu
remote_cameratest="$1"
remote_raw_path="$2"
remote_frame_path="$3"
remote_timestamp_path="$4"
cleanup_before_capture="$5"
raw_resolution="$6"
raw_exposure_mode="$7"
raw_exposure_value="$8"
raw_gain="$9"

mkdir -p "$(dirname "$remote_raw_path")"
mkdir -p "$(dirname "$remote_frame_path")"
mkdir -p "$(dirname "$remote_timestamp_path")"
chmod +x "$remote_cameratest"
printf '[capture_raw_frame remote] prepared remote paths\n' >&2

if [ "$cleanup_before_capture" = "1" ]; then
    rm -f -- "$remote_raw_path" "$remote_frame_path" "$remote_timestamp_path"
    printf '[capture_raw_frame remote] cleaned previous outputs\n' >&2
fi

printf '[capture_raw_frame remote] launching cameratest: %s %s %s %s %s %s %s %s 4\n' \
    "$remote_cameratest" "$raw_resolution" "$raw_exposure_mode" "$raw_exposure_value" "$raw_gain" \
    "$remote_raw_path" "$remote_frame_path" "$remote_timestamp_path" >&2
"$remote_cameratest" "$raw_resolution" "$raw_exposure_mode" "$raw_exposure_value" "$raw_gain" "$remote_raw_path" "$remote_frame_path" "$remote_timestamp_path" 4
printf '[capture_raw_frame remote] cameratest exited\n' >&2

for output_path in "$remote_raw_path" "$remote_frame_path" "$remote_timestamp_path"; do
    if [ -f "$output_path" ]; then
        output_size="$(wc -c < "$output_path" | tr -d '[:space:]')"
        printf '[capture_raw_frame remote] output ready: %s (%s bytes)\n' "$output_path" "${output_size:-0}" >&2
    else
        printf '[capture_raw_frame remote] output missing: %s\n' "$output_path" >&2
    fi
done
EOF
then
    echo "cameratest execution failed" >&2
    exit 1
fi
log "remote cameratest command returned"

ATTEMPTS=$(python3 - "$TIMEOUT_S" "$POLL_INTERVAL_S" <<'PY'
import math
import sys

timeout_s = max(float(sys.argv[1]), 0.0)
poll_s = max(float(sys.argv[2]), 0.01)
print(max(1, int(math.ceil(timeout_s / poll_s))))
PY
)
SIZE_OUTPUT="$("${SSH_CMD[@]}" sh -s -- "$REMOTE_RAW_PATH" "$ATTEMPTS" "$POLL_INTERVAL_S" <<'EOF'
set -eu
remote_raw_path="$1"
attempts="$2"
poll_interval_s="$3"

i=0
while [ "$i" -lt "$attempts" ]; do
    if [ -f "$remote_raw_path" ]; then
        size="$(wc -c < "$remote_raw_path" | tr -d '[:space:]')"
        if [ -n "$size" ] && [ "$size" -gt 0 ] 2>/dev/null; then
            printf '%s\n' "$size"
            exit 0
        fi
    fi

    i=$((i + 1))
    sleep "$poll_interval_s"
done

echo "__MISSING__"
EOF
)"

SIZE_OUTPUT="$(printf '%s' "$SIZE_OUTPUT" | tr -d '\r' | tail -n 1 | xargs || true)"
if [[ -n "$SIZE_OUTPUT" && "$SIZE_OUTPUT" != "__MISSING__" && "$SIZE_OUTPUT" =~ ^[0-9]+$ && "$SIZE_OUTPUT" -gt 0 ]]; then
    printf '%s\n' "$SIZE_OUTPUT"
    exit 0
fi

echo "Raw file not ready within ${TIMEOUT_S}s after cameratest completed: ${REMOTE_RAW_PATH}" >&2
exit 1
