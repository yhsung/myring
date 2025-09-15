#!/usr/bin/env bash
# QEMU ARM64 VM launcher for myring kernel module development
#
# This script launches a Debian ARM64 virtual machine with:
# - High performance configuration (8 cores, 12GB RAM)
# - Bridged networking for direct network access
# - Shared filesystem for code development
# - UEFI boot support
#
# Usage: ./run.sh
# Requirements: QEMU with HVF acceleration (macOS)

set -euo pipefail

# =============================================================================
# Configuration paths
# =============================================================================
LAB="${HOME}/qemu-linux-lab"           # Base lab directory
DISK="${LAB}/debian-aarch64.qcow2"     # VM disk image
SHARE="${LAB}/hostshare"               # Shared directory for code/data
CODE="${LAB}/edk2-code.fd"             # UEFI firmware code
VARS="${LAB}/edk2-vars.fd"             # UEFI variables storage

# =============================================================================
# Pre-flight checks
# =============================================================================
[[ -f "${DISK}" ]] || { echo "Disk not found: ${DISK}"; exit 1; }
[[ -f "${CODE}" ]] || { echo "UEFI code not found: ${CODE}"; exit 1; }
[[ -f "${VARS}" ]] || { echo "UEFI vars not found: ${VARS}"; exit 1; }

# =============================================================================
# Network Configuration
# =============================================================================
OS=$(uname)
if [[ "${OS}" == "Darwin" ]]; then
    IFACE=$(route -n get default | grep 'interface:' | awk '{print $2}')
elif [[ "${OS}" == "Linux" ]]; then
    IFACE=$(ip route | grep default | awk '{print $5}')
else
    echo "Unsupported OS: ${OS}"
    exit 1
fi

[[ -n "${IFACE}" ]] || { echo "Could not determine default network interface."; exit 1; }

# =============================================================================
# QEMU configuration
# =============================================================================
QEMU=/opt/homebrew/bin/qemu-system-aarch64
exec $QEMU \
    `# === Machine Configuration ===` \
    -machine virt,gic-version=3,accel=hvf \
    -cpu cortex-a72 \
    -smp 8 \
    -m 12G \
    `# === UEFI Firmware ===` \
    -drive if=pflash,format=raw,readonly=on,file="${CODE}" \
    -drive if=pflash,format=raw,file="${VARS}" \
    `# === Storage ===` \
    -drive if=none,file="${DISK}",format=qcow2,id=hd0 \
    -device virtio-blk-pci,drive=hd0,bootindex=1 \
    `# === Networking (Bridged Mode) ===` \
    -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 \
    -netdev vmnet-bridged,id=net0,ifname=${IFACE} \
    `# === Shared Filesystem ===` \
    -virtfs local,path="${SHARE}",security_model=mapped-xattr,mount_tag=hostshare \
    `# === Logging ===` \
    -serial mon:stdio -nographic
    #-serial tcp::5555,server,nowait
    #-monitor none -nographic
    #-S -s \
    #-gdb tcp::1234
    #-kernel Image \
    #-append "root=UUID=2cd55bd5-9cb6-4fee-b811-bb0aedaacdc9 kgdboc=ttyAMA0,115200 kgdbwait nokaslr"
    # === Debugging ===



# =============================================================================
# Alternative NAT networking configuration (commented out)
# Use this for port forwarding when bridged networking is not available
# =============================================================================
# -device virtio-net-pci,netdev=n1 \
# -netdev user,id=n1,hostfwd=tcp::2222-:22,hostfwd=udp::5555-:5555,hostfwd=tcp::8080-:80,hostfwd=tcp::5432-:5432,hostfwd=tcp::3306-:3306 \
#
# Port forwarding mapping:
# - SSH:        localhost:2222 -> VM:22
# - HTTP:       localhost:8080 -> VM:80
# - PostgreSQL: localhost:5432 -> VM:5432
# - MySQL:      localhost:3306 -> VM:3306
# - Custom UDP: localhost:5555 -> VM:5555