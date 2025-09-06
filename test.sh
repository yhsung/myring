#!/usr/bin/env bash
# myring module test script
# Builds, loads module, runs user program, and shows kernel logs

set -e  # Exit on error

echo "=== MyRing Module Test Script ==="
echo "$(date): Starting test sequence"

# Build kernel module and user program
echo "[1/6] Building kernel module..."
make clean
make

echo "[2/6] Building user program..."
make user

# Check if module is already loaded and remove it
echo "[3/6] Checking for existing module..."
if lsmod | grep -q myring; then
    echo "  Removing existing myring module"
    sudo rmmod myring || echo "  Warning: Failed to remove module (may not be loaded)"
else
    echo "  No existing myring module found"
fi

# Load the new module
echo "[4/6] Loading myring module..."
sudo insmod build/myring.ko
echo "  Module loaded successfully"

# Check if device exists, create if needed
echo "[5/6] Checking device node..."
if [ ! -c /dev/myring ]; then
    echo "  /dev/myring not found, checking for dynamic device creation..."
    sleep 1
    if [ ! -c /dev/myring ]; then
        echo "  Device node not auto-created, manual creation may be needed"
        echo "  Check 'dmesg | grep myring' for device major number"
    fi
fi

# Run the user program
echo "[6/6] Running user program..."
echo "----------------------------------------"
build/user
echo "----------------------------------------"

echo "\n=== Kernel Messages (last 20 lines) ==="
dmesg | grep myring | tail -n 20

echo "\n=== Test Complete ==="
echo "$(date): Test sequence finished"