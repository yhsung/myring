#ifndef _MYRING_UAPI_H_
#define _MYRING_UAPI_H_

#include <stdint.h>

/* IOCTLs */
#define MYRING_IOC_MAGIC    'r'
#define MYRING_IOC_SET_WM        _IOW(MYRING_IOC_MAGIC, 1, struct myring_watermarks)
#define MYRING_IOC_SET_EVENTFD    _IOW(MYRING_IOC_MAGIC, 2, int)
#define MYRING_IOC_GET_STATS      _IOR(MYRING_IOC_MAGIC, 3, struct myring_stats)
#define MYRING_IOC_ADVANCE_TAIL   _IOW(MYRING_IOC_MAGIC, 4, struct myring_advance)
#define MYRING_IOC_RESET           _IO(MYRING_IOC_MAGIC, 5)

/* Record types */
#define REC_TYPE_PKT   1
#define REC_TYPE_DROP  0xFFFF

/* Flags */
#define CTRL_FLAG_DROPPING   (1u << 0)

struct myring_watermarks {
  uint32_t hi_pct;  /* e.g., 50 */
  uint32_t lo_pct;  /* e.g., 30 */
};

struct myring_advance {
  uint64_t new_tail;
};

struct myring_stats {
  uint64_t head;
  uint64_t tail;
  uint64_t drops;
  uint64_t records;
  uint64_t bytes;
  uint64_t last_hi_cross_ns;
  uint64_t last_lo_cross_ns;
};

/* control page, first PAGE_SIZE bytes of the mapping */
struct myring_ctrl {
  volatile uint64_t head;   /* kernel producer writes */
  volatile uint64_t tail;   /* user consumer writes   */
  uint64_t size;            /* ring data size in bytes (data region only) */
  uint32_t hi_pct;
  uint32_t lo_pct;
  uint32_t flags;           /* CTRL_FLAG_* */
  uint32_t _pad;
  uint64_t drop_start_ns;
  uint64_t lost_in_drop;
} __attribute__((packed));

/* record header (in ring data) */
struct myring_rec_hdr {
  uint16_t type;
  uint16_t flags;
  uint32_t len;
  uint64_t ts_ns;
} __attribute__((packed));

/* drop payload */
struct myring_rec_drop {
  uint32_t lost;
  uint64_t start_ns;
  uint64_t end_ns;
} __attribute__((packed));

#endif /* _MYRING_UAPI_H_ */
