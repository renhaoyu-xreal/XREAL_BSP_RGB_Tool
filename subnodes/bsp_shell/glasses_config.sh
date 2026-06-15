#!/bin/bash

# Common SSH options shared by BSP shell helpers.
SSH_COMMON_OPTIONS=(
    -o StrictHostKeyChecking=no
    -o UserKnownHostsFile=/dev/null
    -o LogLevel=ERROR
    -o HostKeyAlgorithms=+ssh-rsa
    -o PubkeyAcceptedKeyTypes=+ssh-rsa
    -o PubkeyAcceptedAlgorithms=+ssh-rsa
)
