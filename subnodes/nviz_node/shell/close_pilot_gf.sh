#!/bin/bash

# Load common configuration
SHELL_PATH=$(dirname "$0")
source "$SHELL_PATH/glasses_config.sh"

CalibFilePre="/usrdata/online_calibration"

# Target path passed as an argument
TARGET_PATH=$1


#echo "scp libNRTracker3D.so"
#sshpass -p "$PASSWORD" scp $SSH_PARAM -r $SHELL_PATH/libNRTracker3D.so $GLASSES_IP:/usrdata/pilot/bin/libNRTracker3D.so
## Check if the scp command was successful
#if [ $? -eq 0 ]; then
#    echo "libNRTracker3D.so transferred successfully"
#else
#    echo "Error occurred during libNRTracker3D.so transfer"
#    exit 1
#fi
#
#echo "sync"
#sshpass -p "$PASSWORD" ssh $SSH_PARAM -T $GLASSES_IP << EOF
#  sync
#EOF


# scp init shell to x1
echo "scp shell close pilot autorun"
echo "sshpass -p \"$PASSWORD\" scp $SSH_PARAM -r $SHELL_PATH/close_pilot_shell $GLASSES_IP:/etc/services/pilot/run"
sshpass -p "$PASSWORD" scp $SSH_PARAM -r $SHELL_PATH/close_pilot_shell $GLASSES_IP:/etc/services/pilot/run

# Check if the scp command was successful
if [ $? -eq 0 ]; then
    echo "Shell script transferred successfully"
else
    echo "Error occurred during shell script transfer"
    exit 1
fi


echo "add run permissions"
sshpass -p "$PASSWORD" ssh $SSH_PARAM -T $GLASSES_IP << EOF
  chmod +x /etc/services/pilot/run
EOF

# close pilot
echo "killpilt"
sshpass -p "$PASSWORD" ssh $SSH_PARAM -T $GLASSES_IP << EOF
    # Define the process path to be terminated
    process_path="/usrdata/pilot/bin/pilot"

    # Use ps and grep to find the process PID
    pids=\$(ps aux | grep "\$process_path" | grep -v grep | awk '{print \$1}')
    echo "pid '\$pids'"

    # Check if the process was found
    if [ -z "\$pids" ]; then
      echo "No process found for '\$process_path'"
      exit 1
    fi

    # Output the found PID and terminate the process
    echo "Found process '\$process_path' with PID: \$pids"
    for pid in \$pids; do
        kill -9 \$pid
        echo "Killed process with PID: \$pid"
    done
EOF

echo "sync"
sshpass -p "$PASSWORD" ssh $SSH_PARAM -T $GLASSES_IP << EOF
  sync
EOF


if [ $? -eq 0 ]; then
    echo "$CalibFilePre transferred successfully to $TARGET_PATH"
else
    echo "Error occurred during file transfer"
fi


echo "reboot"
sshpass -p "$PASSWORD" ssh $SSH_PARAM -T $GLASSES_IP << EOF
  reboot
EOF
