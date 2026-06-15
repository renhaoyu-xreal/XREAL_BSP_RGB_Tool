#!/bin/bash

# Load common configuration
SHELL_PATH=$(dirname "$0")
source "$SHELL_PATH/glasses_config.sh"

CalibFilePre="/usrdata/online_calibration"

# Target path passed as an argument
TARGET_PATH=$1

echo "scp normal shell"
echo "sshpass -p \"$PASSWORD\" scp $SSH_PARAM -r $SHELL_PATH/normal_shell $GLASSES_IP:/etc/services/pilot/run"
sshpass -p "$PASSWORD" scp $SSH_PARAM -r $SHELL_PATH/normal_shell $GLASSES_IP:/etc/services/pilot/run

echo "add run permissions"
sshpass -p "$PASSWORD" ssh $SSH_PARAM -T $GLASSES_IP << EOF
  chmod +x /etc/services/pilot/run
EOF


echo "sync"
sshpass -p "$PASSWORD" ssh $SSH_PARAM -T $GLASSES_IP << EOF
  sync
EOF

echo "reboot"
sshpass -p "$PASSWORD" ssh $SSH_PARAM -T $GLASSES_IP << EOF
  reboot
EOF



