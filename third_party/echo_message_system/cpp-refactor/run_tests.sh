#!/bin/bash

# Echo Message System Test Script

set -e

echo "╔════════════════════════════════════════╗"
echo "║  Echo Message System - Test Runner    ║"
echo "╚════════════════════════════════════════╝"
echo ""

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Build directory
BUILD_DIR="build"

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${RED}Error: Build directory '$BUILD_DIR' does not exist${NC}"
    echo "Please run docker_build.sh first to compile the project"
    exit 1
fi

cd "$BUILD_DIR"

# Check if required binaries exist
required_binaries=("echo_master" "param_server" "test_pubsub" "test_service" "test_parameter" "test_all")
missing_binaries=()

for binary in "${required_binaries[@]}"; do
    if [ ! -f "$binary" ]; then
        missing_binaries+=($binary)
    fi

done

if [ ${#missing_binaries[@]} -gt 0 ]; then
    echo -e "${RED}Error: Missing required binaries in build directory:${NC}"
    echo "${missing_binaries[@]}"
    echo "Please run docker_build.sh first to compile the project"
    exit 1
fi

echo -e "${GREEN}✓ All required binaries found in build directory${NC}"
echo ""

# Start Master in background
echo -e "${YELLOW}Starting Master server...${NC}"
./echo_master &
MASTER_PID=$!
echo "Master PID: $MASTER_PID"
sleep 1

# Start Parameter Server in background
echo -e "${YELLOW}Starting Parameter Server...${NC}"
./param_server &
PARAM_PID=$!
echo "Parameter Server PID: $PARAM_PID"
sleep 1

# Function to kill master and parameter server on exit
cleanup() {
    echo ""
    echo -e "${YELLOW}Stopping services...${NC}"
    kill $MASTER_PID 2>/dev/null || true
    kill $PARAM_PID 2>/dev/null || true
    wait $MASTER_PID 2>/dev/null || true
    wait $PARAM_PID 2>/dev/null || true
    echo -e "${GREEN}✓ Cleanup complete${NC}"
}

trap cleanup EXIT

# Run tests
echo ""
echo "═══════════════════════════════════════"
echo "Test 1: Pub/Sub"
echo "═══════════════════════════════════════"
if ./test_pubsub; then
    echo -e "${GREEN}✓ Pub/Sub test PASSED${NC}"
else
    echo -e "${RED}✗ Pub/Sub test FAILED${NC}"
    exit 1
fi

echo ""
echo "═══════════════════════════════════════"
echo "Test 3: Service"
echo "═══════════════════════════════════════"
if ./test_service; then
    echo -e "${GREEN}✓ Service test PASSED${NC}"
else
    echo -e "${RED}✗ Service test FAILED${NC}"
    exit 1
fi

echo ""
echo "═══════════════════════════════════════"
echo "Test 4: Parameter Server"
echo "═══════════════════════════════════════"
if ./test_parameter; then
    echo -e "${GREEN}✓ Parameter Server test PASSED${NC}"
else
    echo -e "${RED}✗ Parameter Server test FAILED${NC}"
    exit 1
fi

echo ""
echo "═══════════════════════════════════════"
echo "Test 5: Comprehensive Tests"
echo "═══════════════════════════════════════"
if ./test_all; then
    echo -e "${GREEN}✓ All tests PASSED${NC}"
else
    echo -e "${RED}✗ Some tests FAILED${NC}"
    exit 1
fi

echo ""
echo "╔════════════════════════════════════════╗"
echo "║     All Tests Completed Successfully   ║"
echo "╚════════════════════════════════════════╝"