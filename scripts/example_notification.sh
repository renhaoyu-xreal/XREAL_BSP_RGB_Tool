#!/bin/bash
# 示例脚本：显示桌面通知

MESSAGE="${1:-Hello from LocalhostAgent!}"
TITLE="${2:-Notification}"

# 使用notify-send显示通知
notify-send "$TITLE" "$MESSAGE"

echo "Notification sent: $TITLE - $MESSAGE"
