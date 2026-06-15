#!/bin/bash

# Load common configuration
SHELL_PATH=$(dirname "$0")
source "$SHELL_PATH/glasses_config.sh"

CalibFilePre="/usrdata/online_calibration"

# Target path passed as an argument
TARGET_PATH=$1

# Create target directory if it doesn't exist
mkdir -p "$TARGET_PATH"

SHELL_PATH=$(dirname "$0")

# scp online_calibration to target dir 
echo "delete online_calibration file"
rm -rf $TARGET_PATH/online_calibration/

echo "scp online_calibration_file"
sshpass -p "$PASSWORD" scp $SSH_PARAM -r $GLASSES_IP:$CalibFilePre $TARGET_PATH/

echo "sshpass -p \"$PASSWORD\" scp $SSH_PARAM -r $GLASSES_IP:$CalibFilePre $TARGET_PATH"

# cp online_calibration file to online_calibration_pre
echo "cp online_calibration to online_calibration_pre"
mv  $TARGET_PATH/online_calibration/gyro_bias_drift_backup.json $TARGET_PATH/online_calibration/gyro_bias_drift_backup_pre.json 
mv  $TARGET_PATH/online_calibration/gyro_bias_drift_normal.json $TARGET_PATH/online_calibration/gyro_bias_drift_normal_pre.json 
rm -rf $TARGET_PATH/online_calibration_pre/
mv  $TARGET_PATH/online_calibration $TARGET_PATH/online_calibration_pre


#run pilot
#echo "rm 3dof_param"
#sshpass -p "$PASSWORD" ssh -T $GLASSES_IP << EOF
#   rm -r /usrdata/3dof_param 
#   sync
#EOF



#run pilot
echo "run pilot"
sshpass -p "$PASSWORD" ssh $SSH_PARAM -T $GLASSES_IP << EOF
    cd /usrdata/pilot/bin/
    ./pilot &
    sleep 5 
    echo "Checking if pilot process is running..."
EOF
#
# Check if the pilot process was started successfully
if [ $? -eq 0 ]; then
    echo "Pilot process started successfully"
else
    echo "Error occurred during pilot process start"
    exit 1
fi





