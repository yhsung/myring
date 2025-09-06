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
#include <inttypes.h>

#include "myring_uapi.h"

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
  uint64_t map_len = DEFAULT_MAP_SIZE;
  if (getenv("MYRING_MAP_LEN")) {
    map_len = strtoull(getenv("MYRING_MAP_LEN"), NULL, 0);
  }

  int fd = open(dev, O_RDWR | O_NONBLOCK);
  if (fd < 0) { perror("open /dev/myring"); return 1; }

  /* watermarks */
  struct myring_watermarks wm = { .hi_pct = 50, .lo_pct = 30 };
  if (ioctl(fd, MYRING_IOC_SET_WM, &wm) != 0) { perror("IOCTL_SET_WM"); }

  /* eventfd */
  int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (efd < 0) { perror("eventfd"); return 1; }
  if (ioctl(fd, MYRING_IOC_SET_EVENTFD, &efd) != 0) { perror("IOCTL_SET_EVENTFD"); }

  /* mmap */
  void *map = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) { perror("mmap"); return 1; }

  struct myring_ctrl *ctrl = (struct myring_ctrl *)map;
  uint8_t *data = (uint8_t*)map + PAGE_SIZE;
  uint64_t size = ctrl->size; /* provided by kernel */

  printf("mapped ctrl@%p data@%p size=%" PRIu64 " bytes\n", (void*)ctrl, (void*)data, size);

  /* epoll on eventfd */
  int ep = epoll_create1(EPOLL_CLOEXEC);
  if (ep < 0) { perror("epoll_create1"); return 1; }
  struct epoll_event ev = { .events = EPOLLIN, .data.fd = efd };
  if (epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) != 0) { perror("epoll_ctl"); return 1; }

  uint64_t last_tail = 0;
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
      uint64_t head = load_acquire_u64(&ctrl->head);
      uint64_t tail = load_acquire_u64(&ctrl->tail);
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
        printf("[pkt] ts=%" PRIu64 " len=%u  head=%" PRIu64 " tail=%" PRIu64 "\n",
               rh->ts_ns, rh->len, head, tail);
        hexdump(payload, rh->len, 32);
      } else if (rh->type == REC_TYPE_DROP) {
        struct myring_rec_drop *dr = (struct myring_rec_drop*)payload;
        total_drops += dr->lost;
        printf("** DROP ** lost=%u  start=%" PRIu64 " end=%" PRIu64 "  (total lost=%" PRIu64 ")\n",
               dr->lost, dr->start_ns, dr->end_ns, total_drops);
      } else {
        printf("[unknown type=0x%x] len=%u\n", rh->type, rh->len);
      }

      free(tmp);

      /* advance tail */
      uint64_t new_tail = tail + reclen;
      struct myring_advance adv = { .new_tail = new_tail };
      if (ioctl(fd, MYRING_IOC_ADVANCE_TAIL, &adv) != 0) { perror("ADVANCE_TAIL"); break; }

      last_tail = new_tail;

      /* optional: stop early demonstration */
      // if (total_packets >= 10) { printf("done\n"); goto out; }
    }
  }

out:
  munmap(map, map_len);
  close(efd);
  close(fd);
  return 0;
}
