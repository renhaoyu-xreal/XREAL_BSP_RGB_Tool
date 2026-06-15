#!/bin/bash
# Glasses X1 Configuration
# This file contains common configuration used by all shell scripts

GLASSES_IP=root@169.254.2.1
PORT=22
PASSWORD="xreal2017"
SSH_PARAM="-o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedKeyTypes=+ssh-rsa -o BindAddress=169.254.2.10"
