#!/bin/bash

set -euo pipefail

SHELL_PATH=$(dirname "$0")
source "$SHELL_PATH/glasses_config.sh"

if [ "$#" -lt 6 ]; then
    echo "Usage: $0 <host> <port> <username> <password> <remote_path> <local_path>" >&2
    exit 2
fi

HOST="$1"
PORT="$2"
USERNAME="$3"
PASSWORD="$4"
REMOTE_PATH="$5"
LOCAL_PATH="$6"

SSH_TARGET="${USERNAME}@${HOST}"
mkdir -p "$(dirname "$LOCAL_PATH")"

sshpass -p "$PASSWORD" scp -O "${SSH_COMMON_OPTIONS[@]}" -P "$PORT" "${SSH_TARGET}:${REMOTE_PATH}" "$LOCAL_PATH"

# Some devices expose the raw file with mode 0000; normalize the local copy so
# the current user can inspect and post-process it.
chmod u+rw "$LOCAL_PATH"
stat -c '%s' "$LOCAL_PATH"
