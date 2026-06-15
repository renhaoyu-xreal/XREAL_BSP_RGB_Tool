#!/bin/bash

# Load common configuration
SHELL_PATH=$(dirname "$0")
source "$SHELL_PATH/glasses_config.sh"

echo "reboot"
sshpass -p "$PASSWORD" ssh $SSH_PARAM -T $GLASSES_IP << EOF
  reboot
EOF


