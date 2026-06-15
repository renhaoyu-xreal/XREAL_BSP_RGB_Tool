#!/bin/bash

# Docker Build Script for Echo Message System
# Uses gcc7.5 in Docker container

set -e

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

PROJECT_DIR="$(pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
DOCKER_BUILD_DIR="/app/build"

# Docker image with gcc7.5
DOCKER_IMAGE="gcc:7.5"

# Create temporary Dockerfile if needed
TEMP_DOCKERFILE="${PROJECT_DIR}/Dockerfile.temp"

cat > "${TEMP_DOCKERFILE}" << EOF
FROM ${DOCKER_IMAGE}

# Install dependencies using archived repositories since Debian Buster is EOL
RUN echo "deb http://archive.debian.org/debian buster main contrib non-free" > /etc/apt/sources.list && \ 
    echo "deb http://archive.debian.org/debian buster-updates main contrib non-free" >> /etc/apt/sources.list && \ 
    echo "deb http://archive.debian.org/debian-security buster/updates main contrib non-free" >> /etc/apt/sources.list && \ 
    # Update with flags to handle archived repository issues
    apt-get update -o Acquire::Check-Valid-Until=false -o Acquire::AllowInsecureRepositories=true && \ 
    # Install packages
    apt-get install -y --allow-unauthenticated \
    pkg-config \
    libzmq3-dev \
    cmake \
    git \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app
EOF

echo -e "${YELLOW}Building Docker image with gcc7.5...${NC}"
sudo docker build -t echo_build:gcc7.5 -f "${TEMP_DOCKERFILE}" .

# Clean up temporary Dockerfile
rm "${TEMP_DOCKERFILE}"

echo -e "${YELLOW}Compiling project inside Docker container...${NC}"

# Run Docker container to compile the project
sudo docker run --rm \
    -v "${PROJECT_DIR}:/app" \
    echo_build:gcc7.5 \
    bash -c "cd /app && rm -rf build && mkdir -p build && cd build && cmake .. && make -j$(nproc)"

# Check if build directory exists and has content
echo -e "${YELLOW}Checking build directory contents...${NC}"
ls -la "${BUILD_DIR}"

# Since we're using volume mounting, the compiled files are already in the host's build directory
echo -e "${YELLOW}Compiled files are already available in the host's build directory due to volume mounting...${NC}"

echo -e "${GREEN}✓ Docker build completed successfully!${NC}"
echo -e "${GREEN}✓ Compiled files copied to host build directory.${NC}"
echo ""
echo -e "${YELLOW}Build directory contents:${NC}"
ls -la "${BUILD_DIR}"