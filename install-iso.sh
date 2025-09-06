#!/usr/bin/env bash
set -euo pipefail

LAB="${HOME}/qemu-linux-lab"
ISO="${LAB}/alpine-standard-3.22.1-aarch64.iso"
DISK="${LAB}/alpine-aarch64.qcow2"
SHARE="${LAB}/hostshare"
CODE="${LAB}/edk2-code.fd"
VARS="${LAB}/edk2-vars.fd"

command -v qemu-system-aarch64 >/dev/null || { echo "qemu not found"; exit 1; }
[[ -f "${ISO}" ]]  || { echo "ISO not found: ${ISO}"; exit 1; }
[[ -f "${CODE}" ]] || { echo "UEFI code missing: ${CODE}"; exit 1; }
[[ -f "${VARS}" ]] || { echo "UEFI vars missing: ${VARS}"; exit 1; }
mkdir -p "${SHARE}"

[[ -f "${DISK}" ]] || qemu-img create -f qcow2 "${DISK}" 20G

exec qemu-system-aarch64   -accel hvf   -machine virt,gic-version=3   -cpu cortex-a72   -smp 4 -m 4096     -drive if=pflash,format=raw,readonly=on,file="${CODE}"   -drive if=pflash,format=raw,file="${VARS}"     -device virtio-scsi-pci,id=scsi0   -drive if=none,media=cdrom,format=raw,file="${ISO}",id=cd0   -device scsi-cd,drive=cd0,bus=scsi0.0,bootindex=1     -drive if=none,file="${DISK}",format=qcow2,id=hd0   -device virtio-blk-pci,drive=hd0,bootindex=2     -device virtio-net-pci,netdev=n1   -netdev user,id=n1,hostfwd=tcp::2222-:22,hostfwd=udp::5555-:5555   -virtfs local,path="${SHARE}",security_model=none,mount_tag=hostshare     -nographic -serial mon:stdio
