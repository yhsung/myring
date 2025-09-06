#!/usr/bin/env bash
set -euo pipefail

LAB="${HOME}/qemu-linux-lab"
DISK="${LAB}/alpine-aarch64.qcow2"
SHARE="${LAB}/hostshare"
CODE="${LAB}/edk2-code.fd"
VARS="${LAB}/edk2-vars.fd"

[[ -f "${DISK}" ]] || { echo "Disk not found: ${DISK}"; exit 1; }
[[ -f "${CODE}" ]] || { echo "UEFI code not found: ${CODE}"; exit 1; }
[[ -f "${VARS}" ]] || { echo "UEFI vars not found: ${VARS}"; exit 1; }

exec qemu-system-aarch64   -accel hvf   -machine virt,gic-version=3   -cpu cortex-a72   -smp 4 -m 4096     -drive if=pflash,format=raw,readonly=on,file="${CODE}"   -drive if=pflash,format=raw,file="${VARS}"     -drive if=none,file="${DISK}",format=qcow2,id=hd0   -device virtio-blk-pci,drive=hd0,bootindex=1     -device virtio-net-pci,netdev=n1   -netdev user,id=n1,hostfwd=tcp::2222-:22,hostfwd=udp::5555-:5555   -virtfs local,path="${SHARE}",security_model=none,mount_tag=hostshare     -nographic -serial mon:stdio
