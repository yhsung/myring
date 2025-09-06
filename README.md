# myring

A minimal Linux kernel module + user-space program demo for a **lockless SPSC ring buffer**
with:

- `mmap` shared buffer (kernel ⇄ user)
- `eventfd` notification on high/low watermarks
- **drop indicator** records when buffer overflows
- Synthetic packet producer in kernel (can later be replaced with netfilter hook)

Tested on **Alpine Linux 3.22 (aarch64)** under **QEMU on Apple Silicon**.

## Build & Run

### Inside Alpine guest
```sh
# install toolchain and headers
apk add --no-cache alpine-sdk build-base linux-virt-dev

# build kernel module
make
sudo insmod myring.ko

# build and run user app
gcc -O2 -Wall -o user user.c
./user
```

The device will appear at `/dev/myring`.

### QEMU setup
See [README from earlier instructions](../) for full QEMU + Alpine setup with HVF and 9p share.

## License
MIT
