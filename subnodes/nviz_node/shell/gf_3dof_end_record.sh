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
sshpass -p "$PASSWORD" scp $SSH_PARAM -r $GLASSES_IP:$CalibFilePre $TARGET_PATH


if [ $? -eq 0 ]; then
    echo "online_calibration transferred successfully"
else
    echo "Error occurred during online_calibration transfer"
    exit 1
fi


echo "scp 3dof_param"
sshpass -p "$PASSWORD" scp $SSH_PARAM -r $GLASSES_IP:/usrdata/3dof_param $TARGET_PATH

echo "copy 3dof_param file"
# 搜索所有的 .txt 文件，排除 id.txt
find $TARGET_PATH/3dof_param -type f -name "*.txt" ! -name "id.txt" | while read -r file; do
    # 获取文件名和路径
    dir=$(dirname "$file")
    base=$(basename "$file")
    
    # 删除文件名中最后一个下划线到扩展名前面的字符
    new_base=$(echo "$base" | sed 's/\(.*\)_.*\.txt/\1.txt/')
    
    # 移动文件到新文件名
    cp "$file" "$dir/$new_base"
done



echo "delete glasses_config.json and scp glasses_config.json"
rm $TARGET_PATH/glasses_config.json $TARGET_PATH/glass_config.json

echo "scp glasses_config.json"
sshpass -p "$PASSWORD" scp $SSH_PARAM -r $GLASSES_IP:/factory/glasses_config.json $TARGET_PATH

echo "cp glasses_config_file"
cp $TARGET_PATH/glasses_config.json $TARGET_PATH/glass_config.json

echo "scp log"
sshpass -p "$PASSWORD" scp $SSH_PARAM -r $GLASSES_IP:/usrdata/log $TARGET_PATH


