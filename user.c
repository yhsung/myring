// SPDX-License-Identifier: MIT
// user-space consumer for myring
// - opens /dev/myring, sets watermarks, registers eventfd
// - mmaps ctrl+data, waits on epoll(eventfd), consumes records, advances tail

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <inttypes.h>

#include "myring_uapi.h"

/* Debug logging with file:line info */
#define DEBUG_LOG(fmt, ...) \
    printf("[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)

#define ERROR_LOG(fmt, ...) \
    fprintf(stderr, "[%s:%d] ERROR: " fmt, __FILE__, __LINE__, ##__VA_ARGS__)

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* Choose a mapping size. Must match kernel's allocation:
   total = PAGE_SIZE + (1ull << ring_order). We don't know ring_order here,
   so we'll probe via /proc or just map a generous upper bound. For demo,
   map  (PAGE_SIZE + 4MB) by default; adjust via env if needed. */
#define DEFAULT_RING_SIZE   (1ull << 22) /* 4MB */
#define DEFAULT_MAP_SIZE    (PAGE_SIZE + DEFAULT_RING_SIZE)

static inline uint64_t load_acquire_u64(volatile uint64_t *p) {
  __sync_synchronize();
  return *p;
}
static inline void store_release_u64(volatile uint64_t *p, uint64_t v) {
  *p = v;
  __sync_synchronize();
}

/* Safe load for potentially unaligned packed struct members */
static inline uint64_t load_acquire_u64_packed(const volatile void *p) {
  __sync_synchronize();
  uint64_t val;
  memcpy(&val, (const void*)p, sizeof(val));
  return val;
}

static void hexdump(const void *buf, size_t len, size_t max)
{
  const unsigned char *p = (const unsigned char*)buf;
  size_t n = len < max ? len : max;
  for (size_t i=0;i<n;i++) {
    if (i && (i%16)==0) fprintf(stdout, "\n");
    fprintf(stdout, "%02x ", p[i]);
  }
  if (n < len) fprintf(stdout, "...");
  fprintf(stdout, "\n");
}

int main(int argc, char **argv)
{
  const char *dev = "/dev/myring";

  DEBUG_LOG("open device %s\n", dev);
  
  /* Check if device exists first */
  struct stat st;
  if (stat(dev, &st) != 0) {
    ERROR_LOG("Device %s does not exist: %s\n", dev, strerror(errno));
    ERROR_LOG("1) Load module: sudo insmod build/myring.ko\n");
    ERROR_LOG("2) Check dmesg for major number\n");
    ERROR_LOG("3) Create device: sudo mknod %s c <major> 0\n", dev);
    ERROR_LOG("4) Set permissions: sudo chmod 666 %s\n", dev);
    return 1;
  }
  
  DEBUG_LOG("device exists, checking properties...\n");
  DEBUG_LOG("Device major:minor = %d:%d, mode = 0%o\n", 
         (int)major(st.st_rdev), (int)minor(st.st_rdev), st.st_mode & 0777);
  
  DEBUG_LOG("attempting to open...\n");
  int fd = open(dev, O_RDWR | O_NONBLOCK);
  if (fd < 0) { 
    ERROR_LOG("open %s failed: %s (errno=%d)\n", dev, strerror(errno), errno);
    ERROR_LOG("Check device permissions: ls -la %s\n", dev);
    return 1; 
  }
  DEBUG_LOG("device opened successfully (fd=%d)\n", fd);

  /* get current configuration */
  DEBUG_LOG("get configuration\n");
  struct myring_config cfg;
  if (ioctl(fd, MYRING_IOC_GET_CONFIG, &cfg) == 0) {
    DEBUG_LOG("ring_order=%u (ring_size=%" PRIu64 " bytes, %.1fMB)\n", 
           cfg.ring_order, cfg.ring_size, cfg.ring_size / (1024.0 * 1024.0));
    DEBUG_LOG("rate_hz=%u Hz\n", cfg.rate_hz);
  } else {
    perror("GET_CONFIG");
  }

  /* watermarks */
  DEBUG_LOG("set watermark\n");
  struct myring_watermarks wm = { .hi_pct = 50, .lo_pct = 30 };
  if (ioctl(fd, MYRING_IOC_SET_WM, &wm) != 0) { perror("IOCTL_SET_WM"); }

  /* optionally change the rate */
  if (argc > 1) {
    uint32_t new_rate = (uint32_t)atoi(argv[1]);
    if (new_rate > 0) {
      DEBUG_LOG("setting new rate to %u Hz\n", new_rate);
      if (ioctl(fd, MYRING_IOC_SET_RATE, &new_rate) != 0) {
        perror("SET_RATE");
      } else {
        DEBUG_LOG("rate changed successfully\n");
      }
    }
  }

  /* eventfd */
  DEBUG_LOG("create eventfd\n");
  int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (efd < 0) { perror("eventfd"); return 1; }
  if (ioctl(fd, MYRING_IOC_SET_EVENTFD, &efd) != 0) { perror("IOCTL_SET_EVENTFD"); }


  /* Try a simple ioctl first to see if the device is working */
  DEBUG_LOG("testing device with GET_CONFIG ioctl...\n");
  struct myring_config test_cfg;
  int ioctl_ret = ioctl(fd, MYRING_IOC_GET_CONFIG, &test_cfg);
  if (ioctl_ret != 0) {
    ERROR_LOG("GET_CONFIG ioctl failed: %s (errno=%d)\n", strerror(errno), errno);
    ERROR_LOG("This suggests the kernel module is not properly loaded or compatible\n");
    ERROR_LOG("Check: dmesg | grep myring\n");
    return 1;
  }

  /* mmap */
  void *map = mmap(NULL, DEFAULT_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) { perror("mmap"); return 1; }

  struct myring_ctrl *ctrl = (struct myring_ctrl *)map;
  uint8_t *data = (uint8_t*)map + PAGE_SIZE;
  uint64_t size = ctrl->size; /* provided by kernel */

  DEBUG_LOG("ioctl works, ring_order=%u, rate_hz=%u\n", test_cfg.ring_order, test_cfg.rate_hz);
  DEBUG_LOG("mapped ctrl@%p data@%p size=%" PRIu64 " bytes\n", (void*)ctrl, (void*)data, size);


  /* epoll on eventfd */
  int ep = epoll_create1(EPOLL_CLOEXEC);
  if (ep < 0) { perror("epoll_create1"); return 1; }
  struct epoll_event ev = { .events = EPOLLIN, .data.fd = efd };
  if (epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) != 0) { perror("epoll_ctl"); return 1; }

  uint64_t total_packets = 0, total_drops = 0;

  for (;;) {
    struct epoll_event out;
    int n = epoll_wait(ep, &out, 1, -1);
    if (n < 0) {
      if (errno == EINTR) continue;
      perror("epoll_wait");
      break;
    }
    /* drain eventfd */
    uint64_t tick;
    if (read(efd, &tick, sizeof(tick)) < 0 && errno != EAGAIN) perror("read eventfd");

    /* consume records until tail == head or we drop below lo% */
    for (;;) {
      uint64_t head = load_acquire_u64_packed(&ctrl->head);
      uint64_t tail = load_acquire_u64_packed(&ctrl->tail);
      if (tail == head) break;

      /* data region is a power-of-two ring */
      uint64_t mask = size - 1;
      uint64_t off = tail & mask;
      /* peek header (may wrap) */
      struct myring_rec_hdr hdr;
      size_t first = (size - off) < sizeof(hdr) ? (size - off) : sizeof(hdr);
      memcpy(&hdr, data + off, first);
      if (first < sizeof(hdr)) {
        memcpy(((uint8_t*)&hdr)+first, data, sizeof(hdr) - first);
      }
      uint64_t reclen = sizeof(hdr) + hdr.len;

      /* now copy the whole record to a temp buffer and parse */
      uint8_t *tmp = malloc(reclen);
      if (!tmp) { fprintf(stderr, "oom\n"); exit(1); }
      first = (size - off) < reclen ? (size - off) : reclen;
      memcpy(tmp, data + off, first);
      if (first < reclen) memcpy(tmp + first, data, reclen - first);

      /* parse */
      struct myring_rec_hdr *rh = (struct myring_rec_hdr*)tmp;
      uint8_t *payload = tmp + sizeof(*rh);

      if (rh->type == REC_TYPE_PKT) {
        total_packets++;
        DEBUG_LOG("[pkt] ts=%" PRIu64 " len=%" PRIu32 "  head=%" PRIu64 " tail=%" PRIu64 "\n",
               rh->ts_ns, rh->len, head, tail);
        hexdump(payload, rh->len, 32);
      } else if (rh->type == REC_TYPE_DROP) {
        struct myring_rec_drop *dr = (struct myring_rec_drop*)payload;
        total_drops += dr->lost;
        DEBUG_LOG("** DROP ** lost=%" PRIu32 "  start=%" PRIu64 " end=%" PRIu64 "  (total lost=%" PRIu64 ")\n",
               dr->lost, dr->start_ns, dr->end_ns, total_drops);
      } else {
        DEBUG_LOG("[unknown type=0x%x] len=%" PRIu32 "\n", rh->type, rh->len);
      }

      free(tmp);

      /* advance tail */
      uint64_t new_tail = tail + reclen;
      struct myring_advance adv = { .new_tail = new_tail };
      if (ioctl(fd, MYRING_IOC_ADVANCE_TAIL, &adv) != 0) { perror("ADVANCE_TAIL"); break; }


      /* optional: stop early demonstration */
      if (total_packets >= 10) { 
        DEBUG_LOG("stopping after %"PRIu64" packets\n", total_packets);
        break;
      }
    }
    
    /* show final stats */
    struct myring_stats stats;
    if (ioctl(fd, MYRING_IOC_GET_STATS, &stats) == 0) {
      DEBUG_LOG("\nFinal stats: head=%"PRIu64" tail=%"PRIu64" records=%"PRIu64" drops=%"PRIu64" bytes=%"PRIu64"\n",
             stats.head, stats.tail, stats.records, stats.drops, stats.bytes);
    }
    break;
  }
  
  munmap(map, DEFAULT_MAP_SIZE);
  close(efd);
  close(fd);
  return 0;
}
