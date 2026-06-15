#!/bin/bash

# Load common configuration
SHELL_PATH=$(dirname "$0")
source "$SHELL_PATH/glasses_config.sh"

#scp sdk_global.json to x1 
echo "scp sdk_global.json"
sshpass -p "$PASSWORD" scp $SSH_PARAM -r $SHELL_PATH/sdk_global_send_data_3dof.json $GLASSES_IP:/usrdata/pilot/conf/sdk_global.json

# Check if the scp command was successful
if [ $? -ne 0 ]; then
    echo "Error occurred during sdk_global.json transfer"
    exit 1
fi

echo "run pilot"
sshpass -p "$PASSWORD" ssh $SSH_PARAM -T $GLASSES_IP << EOF
    cd /usrdata/pilot/bin/
    ./pilot &
    sleep 5
    echo "Checking if pilot process is running..."
EOF

# Check if the pilot process was started successfully
if [ $? -eq 0 ]; then
    echo "Pilot process started successfully"
else
    echo "Error occurred during pilot process start"
    exit 1
fi
