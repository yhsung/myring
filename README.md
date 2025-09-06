# myring

A minimal Linux kernel module + user-space program demo for a **lockless SPSC ring buffer**
with shared memory via `mmap`, **eventfd** notifications (high/low watermarks with hysteresis),
and a **drop indicator** record when the buffer overflows.

- Kernel module: `myring.c` (misc device `/dev/myring`)
- UAPI header: `myring_uapi.h`
- User app: `user.c` (epoll + eventfd + mmap consumer)
- Kbuild: `Makefile`
- License: Dual (GPL-2.0 kernel module, MIT userspace)

Tested on **Debian (aarch64)** under **QEMU (Apple Silicon / HVF)**.

---

## Quick Start (Build & Run inside the guest)

```sh
# In Debian guest after installation
apt update
apt install -y build-essential linux-headers-$(uname -r) iperf3 netcat-openbsd

# (optional) mount host share if you put the project there
modprobe 9p 9pnet 9pnet_virtio || true
mkdir -p /mnt/hostshare
mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/hostshare

# build kernel module
make
sudo insmod build/myring.ko

# build and run user app
make user
./build/user
```

The user app will wait on an `eventfd` and consume records from the ring. The kernel module
includes a **synthetic producer** (default ~2 kHz of ~256B records). You can later switch
to a netfilter hook path to ingest real packets.

---

## Cross-compilation on macOS (Apple Silicon)

You can cross-compile the user-space application on macOS for the Debian guest. The kernel module must be built inside the guest with the target kernel headers.

### Install aarch64 cross-compiler

```sh
# Install via Homebrew
brew install aarch64-elf-gcc

# Or use a full Linux toolchain (alternative)
brew install x86_64-elf-gcc  # if you need x86_64 later
```

### Cross-compile user application

```sh
# Build user app for aarch64-linux (static linking recommended)
aarch64-elf-gcc -static -O2 -Wall -o user-aarch64 user.c

# Copy to host share (accessible from guest)
mkdir -p ~/qemu-linux-lab/hostshare/myring
cp build/user-aarch64 ~/qemu-linux-lab/hostshare/myring/

# Alternative: use musl cross-compiler for better Linux compatibility
# brew install filosottile/musl-cross/musl-cross
# musl-cross-aarch64-linux-gnu-gcc -static -O2 -Wall -o user user.c
```

### Build workflow

1. **On macOS host**: Cross-compile user application
2. **In Debian guest**: Build kernel module with target headers
3. **Share via 9p**: Use the host share to transfer binaries

```sh
# On macOS (in project directory)
make user-cross         # creates build/user-aarch64 binary
cp build/user-aarch64 ~/qemu-linux-lab/hostshare/myring/

# In Debian guest (after mounting hostshare)
cd /mnt/hostshare/myring
make                    # builds kernel module to build/myring.ko
sudo insmod build/myring.ko
./user-aarch64         # run cross-compiled binary
```

---

## QEMU on Apple Silicon: Install & Run Debian (aarch64)

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
qemu-img create -f qcow2 debian-aarch64.qcow2 20G

# download ISO (place it here with exact name)
#   debian-12.8.0-arm64-netinst.iso
# verify:
ls -lh debian-12.8.0-arm64-netinst.iso

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
ISO="${LAB}/debian-12.8.0-arm64-netinst.iso"
DISK="${LAB}/debian-aarch64.qcow2"
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

You should see the Debian boot menu. **Press Enter** to boot the default
kernel. The installer will start automatically.

Follow the interactive installer:

- Language: English
- Country: Select your country
- Keyboard: default
- Hostname: `debian` (or preferred name)
- Domain: (can leave empty)
- Root password: set one
- User account: create a regular user
- Timezone: select appropriate timezone
- Partitioning: Guided - use entire disk (will use `vda`)
- Software selection: Standard system utilities (you can add SSH server)
- GRUB bootloader: install to `/dev/vda`

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
DISK="${LAB}/debian-aarch64.qcow2"
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

## After installing Debian (inside the guest)

```sh
apt update
apt install -y build-essential linux-headers-$(uname -r) iperf3 netcat-openbsd

# mount 9p share (one-time)
modprobe 9p 9pnet 9pnet_virtio || true
mkdir -p /mnt/hostshare
mount -t 9p -o trans=virtio,version=9p2000.L,rw,uid=$(id -u),gid=$(id -g),uname=$(whoami),access=client hostshare /mnt/hostshare

# OR set up auto-mount at boot (recommended)
# See "Auto-mount hostshare" section below

# go to your project (if placed in the host share)
cd /mnt/hostshare/myring

make
sudo insmod build/myring.ko

make user
./build/user
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

## Auto-mount hostshare at boot

To automatically mount the 9p hostshare every time the Debian guest boots:

### Method 1: Using /etc/fstab (recommended)

```bash
# 1. Load 9p modules at boot
echo '9p' | sudo tee -a /etc/modules
echo '9pnet' | sudo tee -a /etc/modules
echo '9pnet_virtio' | sudo tee -a /etc/modules

# 2. Create mount point
sudo mkdir -p /mnt/hostshare

# 3. Add to fstab with proper permissions
echo "hostshare /mnt/hostshare 9p trans=virtio,version=9p2000.L,rw,_netdev,uid=$(id -u),gid=$(id -g),uname=$(whoami),access=client 0 0" | sudo tee -a /etc/fstab

# 4. Test the mount
sudo mount -a
ls /mnt/hostshare
```

### Method 2: Using systemd service (alternative)

```bash
# 1. Load modules (same as above)
echo '9p' | sudo tee -a /etc/modules
echo '9pnet' | sudo tee -a /etc/modules
echo '9pnet_virtio' | sudo tee -a /etc/modules

# 2. Create systemd service
sudo tee /etc/systemd/system/mount-hostshare.service << 'EOF'
[Unit]
Description=Mount QEMU hostshare via 9p
After=network.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStartPre=/bin/mkdir -p /mnt/hostshare
ExecStart=/bin/bash -c 'mount -t 9p -o trans=virtio,version=9p2000.L,rw,uid=$(id -u),gid=$(id -g),uname=$(whoami),access=client hostshare /mnt/hostshare'
ExecStop=/bin/umount /mnt/hostshare

[Install]
WantedBy=multi-user.target
EOF

# 3. Enable and start the service
sudo systemctl enable mount-hostshare.service
sudo systemctl start mount-hostshare.service

# 4. Check status
sudo systemctl status mount-hostshare.service
ls /mnt/hostshare
```

### Verify auto-mount

After setting up either method, reboot and check:

```bash
sudo reboot
# After reboot:
ls /mnt/hostshare
mount | grep hostshare
```

---

## Troubleshooting

### Network Issues in Debian Guest

If you get "Network is unreachable" during or after Debian installation:

**During Installation:**
- Select "Configure network automatically" when prompted
- If auto-config fails, use manual configuration:
  - IP: `10.0.2.15`
  - Netmask: `255.255.255.0`
  - Gateway: `10.0.2.2`
  - DNS: `8.8.8.8`

**After Installation (if network still broken):**
```bash
# Check interface status
ip a
ip route

# Fix manually (temporary)
sudo ip link set enp0s1 up
sudo ip addr add 10.0.2.15/24 dev enp0s1
sudo ip route add default via 10.0.2.2
echo "nameserver 8.8.8.8" | sudo tee /etc/resolv.conf

# Test connectivity
ping 8.8.8.8
```

**Permanent fix with NetworkManager:**
```bash
sudo systemctl enable NetworkManager
sudo systemctl start NetworkManager
sudo nmcli device connect enp0s1
```

### Hostshare Permission Issues

If you can't modify files in `/mnt/hostshare`:

**Quick fix (remount with correct permissions):**
```bash
sudo umount /mnt/hostshare
mount -t 9p -o trans=virtio,version=9p2000.L,rw,uid=$(id -u),gid=$(id -g),uname=$(whoami),access=client hostshare /mnt/hostshare
```

**For persistent fix:** Update your fstab or systemd service with the permission options shown above.

**Alternative: Use security_model=mapped-xattr in QEMU scripts:**
Add to your QEMU command:
```bash
-virtfs local,path="${SHARE}",security_model=mapped-xattr,mount_tag=hostshare
```

### Other Issues

- **UEFI shell / no boot from ISO**: Ensure ISO path is correct and exist; our script
  uses `scsi-cd` with `bootindex=1` so UEFI picks it. Try re-running `install-iso.sh`.
- **"HVF does not support providing Virtualization extensions…"**: We **do not**
  set `virtualization=on`. Make sure `-machine virt,gic-version=3` (no `virtualization=on`).
- **Install shows boot menu only**: Press **Enter** (default boot). You’ll get a root shell.
  Then run the Debian installer.
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

**Dual License:**
- **Kernel module** (`myring.c`): GPL-2.0 (required for GPL-only kernel symbols)
- **Userspace components** (`user.c`, `myring_uapi.h`, scripts): MIT

See `LICENSE` file for full terms.
