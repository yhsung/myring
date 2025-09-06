#ifndef _MYRING_UAPI_H_
#define _MYRING_UAPI_H_

#include <linux/types.h>

/* IOCTLs */
#define MYRING_IOC_MAGIC    'r'
#define MYRING_IOC_SET_WM        _IOW(MYRING_IOC_MAGIC, 1, struct myring_watermarks)
#define MYRING_IOC_SET_EVENTFD    _IOW(MYRING_IOC_MAGIC, 2, int)
#define MYRING_IOC_GET_STATS      _IOR(MYRING_IOC_MAGIC, 3, struct myring_stats)
#define MYRING_IOC_ADVANCE_TAIL   _IOW(MYRING_IOC_MAGIC, 4, struct myring_advance)
#define MYRING_IOC_RESET           _IO(MYRING_IOC_MAGIC, 5)
#define MYRING_IOC_GET_CONFIG     _IOR(MYRING_IOC_MAGIC, 6, struct myring_config)
#define MYRING_IOC_SET_RATE       _IOW(MYRING_IOC_MAGIC, 7, __u32)

/* Record types */
#define REC_TYPE_PKT   1
#define REC_TYPE_DROP  0xFFFF

/* Flags */
#define CTRL_FLAG_DROPPING   (1u << 0)

struct myring_watermarks {
  __u32 hi_pct;  /* e.g., 50 */
  __u32 lo_pct;  /* e.g., 30 */
};

struct myring_advance {
  __u64 new_tail;
};

struct myring_stats {
  __u64 head;
  __u64 tail;
  __u64 drops;
  __u64 records;
  __u64 bytes;
  __u64 last_hi_cross_ns;
  __u64 last_lo_cross_ns;
};

struct myring_config {
  __u32 ring_order;    /* log2 of ring data size in bytes */
  __u32 rate_hz;       /* synthetic producer rate in Hz */
  __u64 ring_size;     /* actual ring size in bytes (1 << ring_order) */
};

/* control page, first PAGE_SIZE bytes of the mapping */
struct myring_ctrl {
  volatile __u64 head;   /* kernel producer writes */
  volatile __u64 tail;   /* user consumer writes   */
  __u64 size;            /* ring data size in bytes (data region only) */
  __u32 hi_pct;
  __u32 lo_pct;
  __u32 flags;           /* CTRL_FLAG_* */
  __u32 _pad;
  __u64 drop_start_ns;
  __u64 lost_in_drop;
} __attribute__((packed));

/* record header (in ring data) */
struct myring_rec_hdr {
  __u16 type;
  __u16 flags;
  __u32 len;
  __u64 ts_ns;
} __attribute__((packed));

/* drop payload */
struct myring_rec_drop {
  __u32 lost;
  __u64 start_ns;
  __u64 end_ns;
} __attribute__((packed));

#endif /* _MYRING_UAPI_H_ */
