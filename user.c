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
#include <time.h>

#include "myring_uapi.h"

/* Get current timestamp as string */
static void get_timestamp_str(char *buf, size_t buf_size) {
  struct timespec ts;
  struct tm *tm_info;
  clock_gettime(CLOCK_REALTIME, &ts);
  tm_info = localtime(&ts.tv_sec);
  snprintf(buf, buf_size, "%02d:%02d:%02d.%03ld",
           tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
           ts.tv_nsec / 1000000);
}

/* Debug logging with timestamp and file:line info */
#define DEBUG_LOG(fmt, ...) do { \
    char ts_buf[16]; \
    get_timestamp_str(ts_buf, sizeof(ts_buf)); \
    printf("[%s %s:%d] " fmt, ts_buf, __FILE__, __LINE__, ##__VA_ARGS__); \
} while(0)

#define ERROR_LOG(fmt, ...) do { \
    char ts_buf[16]; \
    get_timestamp_str(ts_buf, sizeof(ts_buf)); \
    fprintf(stderr, "[%s %s:%d] ERROR: " fmt, ts_buf, __FILE__, __LINE__, ##__VA_ARGS__); \
} while(0)

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* Choose a mapping size. Must match kernel's allocation:
   total = PAGE_SIZE + (1ull << ring_order). We don't know ring_order here,
   so we'll probe via /proc or just map a generous upper bound. For demo,
   map  (PAGE_SIZE + 4MB) by default; adjust via env if needed. */
#define DEFAULT_RING_SIZE   (1ull << 20) /* 1MB */
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
  DEBUG_LOG("ioctl works, ring_order=%u, rate_hz=%u\n", test_cfg.ring_order, test_cfg.rate_hz);

  /* mmap */
  void *map = mmap(NULL, DEFAULT_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) { perror("mmap"); return 1; }

  struct myring_ctrl *ctrl = (struct myring_ctrl *)map;
  uint8_t *data = (uint8_t*)map + PAGE_SIZE;
  uint64_t size = ctrl->size; /* provided by kernel */

  DEBUG_LOG("mapped ctrl@%p data@%p size=%" PRIu64 " bytes\n", (void*)ctrl, (void*)data, size);


  /* epoll on eventfd */
  int ep = epoll_create1(EPOLL_CLOEXEC);
  if (ep < 0) { perror("epoll_create1"); return 1; }
  struct epoll_event ev = { .events = EPOLLIN, .data.fd = efd };
  if (epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) != 0) { perror("epoll_ctl"); return 1; }

  uint64_t total_packets = 0, total_drops = 0;
  uint64_t total_bytes = 0;
  struct timespec start_time, current_time;
  clock_gettime(CLOCK_MONOTONIC, &start_time);

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
        total_bytes += rh->len;
        
        /* Detailed packet consumption diagnostics */
        DEBUG_LOG("[CONSUME] Packet #%" PRIu64 ": ts=%" PRIu64 " len=%" PRIu32 "\n",
               total_packets, rh->ts_ns, rh->len);
        DEBUG_LOG("[CONSUME] Ring state: head=%" PRIu64 " tail=%" PRIu64 " used=%" PRIu64 "\n",
               head, tail, head - tail);
        DEBUG_LOG("[CONSUME] Record position: tail_offset=%" PRIu64 " record_len=%" PRIu64 "\n",
               tail & (size - 1), reclen);
        DEBUG_LOG("[CONSUME] Memory: ring_size=%" PRIu64 " wrap=%s\n",
               size, (first < reclen) ? "YES" : "NO");
        
        /* Show first 16 bytes of payload for diagnostics */
        if (rh->len >= 8) {
          uint64_t *payload_u64 = (uint64_t*)payload;
          DEBUG_LOG("[CONSUME] Payload first 8 bytes: 0x%016" PRIx64 "\n", *payload_u64);
        }
        
        /* Show detailed hexdump for first few packets */
        if (total_packets <= 5) {
          DEBUG_LOG("[CONSUME] Full hexdump for packet #%" PRIu64 ":\n", total_packets);
          hexdump(payload, rh->len, rh->len); /* Show full packet for first 5 */
        } else {
          hexdump(payload, rh->len, 32); /* Truncated for others */
        }
        
        /* Print progress every 10 packets */
        if (total_packets % 10 == 0) {
          clock_gettime(CLOCK_MONOTONIC, &current_time);
          double elapsed = (current_time.tv_sec - start_time.tv_sec) + 
                          (current_time.tv_nsec - start_time.tv_nsec) / 1e9;
          double rate_pps = elapsed > 0 ? total_packets / elapsed : 0;
          double rate_bps = elapsed > 0 ? total_bytes / elapsed : 0;
          
          printf("\n=== PROGRESS ===\n");
          printf("Packets: %" PRIu64 ", Bytes: %" PRIu64 " (%.2f KB, %.2f MB)\n", 
                 total_packets, total_bytes, total_bytes / 1024.0, total_bytes / (1024.0 * 1024.0));
          printf("Elapsed: %.2fs, Rate: %.1f pps, %.2f KB/s\n", 
                 elapsed, rate_pps, rate_bps / 1024.0);
          printf("Drops: %" PRIu64 "\n", total_drops);
          printf("Ring utilization: %.1f%% (%" PRIu64 "/%" PRIu64 ")\n", 
                 size > 0 ? (100.0 * (head - tail)) / size : 0.0, head - tail, size);
          printf("================\n\n");
        }
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
      uint64_t old_tail = tail;
      
      DEBUG_LOG("[ADVANCE] Advancing tail: %" PRIu64 " -> %" PRIu64 " (delta=%" PRIu64 ")\n",
             old_tail, new_tail, reclen);
      
      struct myring_advance adv = { .new_tail = new_tail };
      if (ioctl(fd, MYRING_IOC_ADVANCE_TAIL, &adv) != 0) { 
        ERROR_LOG("ADVANCE_TAIL ioctl failed: %s (errno=%d)\n", strerror(errno), errno);
        ERROR_LOG("Failed to advance tail from %" PRIu64 " to %" PRIu64 "\n", old_tail, new_tail);
        perror("ADVANCE_TAIL"); 
        break; 
      }
      
      DEBUG_LOG("[ADVANCE] Tail successfully advanced, record consumed\n");


      /* optional: stop early demonstration */
      if (total_packets >= 100) { 
        DEBUG_LOG("stopping after %"PRIu64" packets\n", total_packets);
        break;
      }
    }
    
    /* show final stats */
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    double total_elapsed = (current_time.tv_sec - start_time.tv_sec) + 
                          (current_time.tv_nsec - start_time.tv_nsec) / 1e9;
    
    struct myring_stats stats;
    if (ioctl(fd, MYRING_IOC_GET_STATS, &stats) == 0) {
      DEBUG_LOG("\nFinal stats: head=%"PRIu64" tail=%"PRIu64" records=%"PRIu64" drops=%"PRIu64" bytes=%"PRIu64"\n",
             stats.head, stats.tail, stats.records, stats.drops, stats.bytes);
    }
    
    printf("\n=== FINAL SUMMARY ===\n");
    printf("Total Runtime: %.2f seconds\n", total_elapsed);
    printf("Packets Processed: %" PRIu64 "\n", total_packets);
    printf("Bytes Processed: %" PRIu64 " (%.2f KB, %.2f MB)\n", 
           total_bytes, total_bytes / 1024.0, total_bytes / (1024.0 * 1024.0));
    if (total_elapsed > 0) {
      printf("Average Rate: %.1f packets/sec, %.2f KB/sec\n", 
             total_packets / total_elapsed, (total_bytes / 1024.0) / total_elapsed);
    }
    printf("Total Drops: %" PRIu64 "\n", total_drops);
    printf("====================\n");
    break;
  }
  
  munmap(map, DEFAULT_MAP_SIZE);
  close(efd);
  close(fd);
  return 0;
}
