# myring

A minimal Linux kernel module + user-space program demo for a **lockless SPSC ring buffer**
with shared memory via `mmap`, **eventfd** notifications (high/low watermarks with hysteresis),
and a **drop indicator** record when the buffer overflows.

- Kernel module: `myring.c` (misc device `/dev/myring`)
- UAPI header: `myring_uapi.h`
- User app: `user.c` (epoll + eventfd + mmap consumer)
- Kbuild: `Makefile`
- License: MIT

Tested on **Alpine Linux 3.22 (aarch64)** under **QEMU (Apple Silicon / HVF)**.

---

## Quick Start (Build & Run inside the guest)

```sh
# In Alpine guest after installation
apk update
apk add --no-cache alpine-sdk build-base linux-virt-dev iperf3 netcat-openbsd

# (optional) mount host share if you put the project there
modprobe 9p 9pnet 9pnet_virtio || true
mkdir -p /mnt/hostshare
mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/hostshare

# build kernel module
make
sudo insmod myring.ko

# build and run user app
gcc -O2 -Wall -o user user.c
./user
```

The user app will wait on an `eventfd` and consume records from the ring. The kernel module
includes a **synthetic producer** (default ~2 kHz of ~256B records). You can later switch
to a netfilter hook path to ingest real packets.

---

## QEMU on Apple Silicon: Install & Run Alpine 3.22.1 (aarch64)

> These instructions assume macOS with Homebrew QEMU. We use **HVF** acceleration,
> **UEFI pflash** (EDK2), **virtio** devices, **SCSI CD-ROM** with `bootindex`, and
> a **9p share** for easy file exchange.

### 0) Prepare folders & files on the Mac (host)

```sh
# install qemu
brew install qemu

# workspace
mkdir -p ~/qemu-linux-lab/hostshare
cd ~/qemu-linux-lab

# disk image
qemu-img create -f qcow2 alpine-aarch64.qcow2 20G

# download ISO (place it here with exact name)
#   alpine-standard-3.22.1-aarch64.iso
# verify:
ls -lh alpine-standard-3.22.1-aarch64.iso

# copy UEFI firmware blobs into the lab (code=RO, vars=RW)
cp /opt/homebrew/share/qemu/edk2-aarch64-code.fd ./edk2-code.fd
cp /opt/homebrew/share/qemu/edk2-arm-vars.fd      ./edk2-vars.fd
```

### 1) First boot (from ISO) — **installer**

> Use **serial console** so you can always see output in the terminal.

Create `install-iso.sh` with the following content:

```sh
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

# create disk if needed
[[ -f "${DISK}" ]] || qemu-img create -f qcow2 "${DISK}" 20G

exec qemu-system-aarch64   -accel hvf   -machine virt,gic-version=3   -cpu cortex-a72   -smp 4 -m 4096     -drive if=pflash,format=raw,readonly=on,file="${CODE}"   -drive if=pflash,format=raw,file="${VARS}"     -device virtio-scsi-pci,id=scsi0   -drive if=none,media=cdrom,format=raw,file="${ISO}",id=cd0   -device scsi-cd,drive=cd0,bus=scsi0.0,bootindex=1     -drive if=none,file="${DISK}",format=qcow2,id=hd0   -device virtio-blk-pci,drive=hd0,bootindex=2     -device virtio-net-pci,netdev=n1   -netdev user,id=n1,hostfwd=tcp::2222-:22,hostfwd=udp::5555-:5555   -virtfs local,path="${SHARE}",security_model=none,mount_tag=hostshare     -nographic -serial mon:stdio
```

Make it executable and run it:

```sh
chmod +x install-iso.sh
./install-iso.sh
```

You should see the Alpine boot menu (`boot:`). **Press Enter** to boot the default
kernel (or type `linux-virt` then Enter). You will land in a root shell.

Run the interactive installer:

```sh
setup-alpine
```

Suggested answers (you can adjust):
- Keyboard: default (`us`)
- Hostname: `alpine`
- Network: interface `eth0` → DHCP (`no` for manual) → IPv6 `no` (or yes if you want)
- root password: set one
- Timezone: `Asia/Taipei` (or as desired)
- NTP: `chrony`
- SSH server: `openssh` (recommended)
- Mirrors: pick a close mirror (e.g., dl-cdn.alpinelinux.org)
- **Kernel**: choose **`linux-virt`** (optimized for VMs)
- Disk: choose `vda`, mode `sys` (will wipe that disk)

When it says “Installation is complete”, power off:
```sh
poweroff
```

### 2) Subsequent boots — run from disk

Create `run.sh`:

```sh
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
```

Run it after installation to boot from disk:
```sh
chmod +x run.sh
./run.sh
```

Now you can SSH from the host macOS into the guest:
```sh
ssh -p 2222 root@127.0.0.1
```

---

## After installing Alpine (inside the guest)

```sh
apk update
apk add --no-cache alpine-sdk build-base linux-virt-dev iperf3 netcat-openbsd

# mount 9p share if needed
modprobe 9p 9pnet 9pnet_virtio || true
mkdir -p /mnt/hostshare
mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/hostshare

# go to your project (if placed in the host share)
cd /mnt/hostshare/myring

make
sudo insmod myring.ko

gcc -O2 -Wall -o user user.c
./user
```

Optional quick traffic (host → guest via UDP 5555):
```sh
# in guest, open a UDP sink
nc -ul 5555 >/dev/null &

# on mac host, send many UDP packets (uses the user-mode net forward)
python3 - <<'PY'
import socket, time
s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
addr=('127.0.0.1', 5555)
payload=b'x'*1200
for i in range(20000):
    s.sendto(payload, addr)
PY
```

---

## Troubleshooting

- **UEFI shell / no boot from ISO**: Ensure ISO path is correct and exist; our script
  uses `scsi-cd` with `bootindex=1` so UEFI picks it. Try re-running `install-iso.sh`.
- **“HVF does not support providing Virtualization extensions…”**: We **do not**
  set `virtualization=on`. Make sure `-machine virt,gic-version=3` (no `virtualization=on`).
- **Install shows boot menu only**: Press **Enter** (default boot). You’ll get a root shell.
  Then run `setup-alpine`.
- **Could not open ' '**: A path variable is empty. Double-check ISO and pflash file paths.
- **`command not found: -cpu` in your script**: Missing trailing `\` on the previous line.
- **HVF quirks**: If you suspect HVF issues, try software emulation:
  ```sh
  # replace -accel hvf with:
  -accel tcg,thread=multi
  ```

---

## Project layout

```
myring/
├── Makefile
├── LICENSE
├── README.md         ← you are here
├── myring.c          ← kernel module (miscdev + mmap ring + eventfd + drop)
├── myring_uapi.h     ← shared UAPI
└── user.c            ← user-space consumer
```

## License
MIT
