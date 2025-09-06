// SPDX-License-Identifier: MIT
// myring: miscdev + mmap SPSC ring + eventfd notify + watermarks + drop indicator
// Alpine 3.22 / aarch64 friendly
//
// Build: make
// Load : insmod myring.ko [rate_hz=2000 ring_order=22]
// Device: /dev/myring

#include <linux/module.h>
#include <linux/version.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/eventfd.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>
#include <linux/smp.h>
#include <linux/mutex.h>

// #define USE_NETFILTER 1
#ifdef USE_NETFILTER
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#endif

#include "myring_uapi.h"

#define DRV_NAME "myring"

/* Module params */
static unsigned int ring_order = 22; /* ring data size = 1<<order bytes (default 4MB) */
module_param(ring_order, uint, 0444);
MODULE_PARM_DESC(ring_order, "log2 of ring data bytes (default 22 -> 4MB)");

static unsigned int rate_hz = 2000; /* synthetic producer rate */
module_param(rate_hz, uint, 0644);
MODULE_PARM_DESC(rate_hz, "synthetic producer rate in Hz (default 2000)");

/* Device state */
struct myring_dev {
  struct miscdevice misc;
  struct myring_ctrl *ctrl;   /* first PAGE_SIZE */
  void *vmem;                 /* vmalloc'd block (ctrl + data) */
  size_t vmem_len;
  void *data;                 /* start of ring data region */
  uint64_t size;              /* ring data bytes (power-of-two) */

  struct eventfd_ctx *evt;
  bool above_hi;
  wait_queue_head_t wq;
  struct mutex ioctl_mu;

  /* stats */
  uint64_t records;
  uint64_t bytes;
  uint64_t drops;

  /* synthetic producer */
  struct delayed_work prod_work;
  bool stopping;

#ifdef USE_NETFILTER
  struct nf_hook_ops nfops;
#endif
};

static struct myring_dev g;

/* Helpers */
static inline uint64_t rb_used(struct myring_ctrl *c)
{
  uint64_t head = smp_load_acquire(&c->head);
  uint64_t tail = smp_load_acquire(&c->tail);
  return head - tail;
}

static inline uint64_t rb_free(struct myring_ctrl *c)
{
  return c->size - rb_used(c);
}

static inline void rb_commit_head(struct myring_ctrl *c, uint64_t new_head)
{
  smp_store_release(&c->head, new_head);
}

static inline uint32_t rb_pct(uint64_t used, uint64_t size)
{
  if (!size) return 0;
  return (uint32_t)((used * 100) / size);
}

static void myring_signal(struct myring_dev *d)
{
  if (d->evt) eventfd_signal(d->evt, 1);
  wake_up_interruptible(&d->wq);
}

static void myring_maybe_notify(struct myring_dev *d)
{
  struct myring_ctrl *c = d->ctrl;
  uint64_t used = rb_used(c);
  uint32_t pct  = rb_pct(used, c->size);

  if (!d->above_hi && pct >= c->hi_pct) {
    d->above_hi = true;
    // record timestamp
    // not exposed directly; GET_STATS reads ctrl's last_* stamped here
    // but simpler: keep in d? We'll keep only in stats when asked.
    myring_signal(d);
  } else if (d->above_hi && pct <= c->lo_pct) {
    d->above_hi = false;
  }
}

static bool myring_reserve(struct myring_ctrl *c, uint64_t need, uint64_t *pos_out)
{
  uint64_t head = smp_load_acquire(&c->head);
  uint64_t tail = smp_load_acquire(&c->tail);
  uint64_t free = c->size - (head - tail);
  if (free < need) return false;
  *pos_out = head;
  return true;
}

static void myring_write_bytes(struct myring_dev *d, uint64_t pos, const void *src, uint64_t len)
{
  uint64_t mask = d->size - 1; /* size is power-of-two */
  uint64_t off = pos & mask;
  uint64_t first = min_t(uint64_t, len, d->size - off);
  memcpy(d->data + off, src, first);
  if (len > first) memcpy(d->data, src + first, len - first);
}

static void myring_on_full(struct myring_ctrl *c)
{
  if (!(c->flags & CTRL_FLAG_DROPPING)) {
    c->flags |= CTRL_FLAG_DROPPING;
    c->drop_start_ns = ktime_get_ns();
    c->lost_in_drop = 0;
  }
  c->lost_in_drop++;
}

static void myring_flush_drop_record(struct myring_dev *d)
{
  struct myring_ctrl *c = d->ctrl;
  if (!(c->flags & CTRL_FLAG_DROPPING)) return;

  struct myring_rec_hdr hdr = {
    .type = REC_TYPE_DROP,
    .flags = 0,
    .len = sizeof(struct myring_rec_drop),
    .ts_ns = ktime_get_ns(),
  };
  struct myring_rec_drop drop = {
    .lost = (uint32_t)c->lost_in_drop,
    .start_ns = c->drop_start_ns,
    .end_ns = ktime_get_ns(),
  };
  uint64_t pos;
  uint64_t need = sizeof(hdr) + sizeof(drop);

  if (myring_reserve(c, need, &pos)) {
    myring_write_bytes(d, pos, &hdr, sizeof(hdr));
    myring_write_bytes(d, pos + sizeof(hdr), &drop, sizeof(drop));
    rb_commit_head(c, pos + need);
    c->flags &= ~CTRL_FLAG_DROPPING;
    d->records++;
    d->bytes += need;
  }
}

/* Push a "packet" record into the ring (payload=payload,len) */
static void myring_push_packet(struct myring_dev *d, const void *payload, uint32_t len)
{
  struct myring_ctrl *c = d->ctrl;
  struct myring_rec_hdr hdr = {
    .type = REC_TYPE_PKT,
    .flags = 0,
    .len = len,
    .ts_ns = ktime_get_ns(),
  };
  uint64_t pos;
  uint64_t need = sizeof(hdr) + len;

  if (!myring_reserve(c, need, &pos)) {
    myring_on_full(c);
    d->drops++;
    return;
  }

  /* If we were dropping, emit the drop record first */
  myring_flush_drop_record(d);

  myring_write_bytes(d, pos, &hdr, sizeof(hdr));
  myring_write_bytes(d, pos + sizeof(hdr), payload, len);
  rb_commit_head(c, pos + need);

  d->records++;
  d->bytes += need;

  myring_maybe_notify(d);
}

/* Synthetic producer work */
static void myring_prod_fn(struct work_struct *w)
{
  struct myring_dev *d = container_of(to_delayed_work(w), struct myring_dev, prod_work);
  if (d->stopping) return;

  /* fabricate a 256B payload */
  uint8_t buf[256];
  *(uint64_t*)buf = ktime_get_ns();
  memset(buf + 8, 0xAB, sizeof(buf) - 8);
  myring_push_packet(d, buf, sizeof(buf));

  if (!d->stopping) {
    unsigned long interval_ms = rate_hz ? max(1u, 1000u / rate_hz) : 1u;
    schedule_delayed_work(&d->prod_work, msecs_to_jiffies(interval_ms));
  }
}

/* File ops */

static int myring_open(struct inode *ino, struct file *f)
{
  f->private_data = &g;
  return 0;
}

static int myring_release(struct inode *ino, struct file *f)
{
  return 0;
}

static long myring_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
  struct myring_dev *d = f->private_data;
  int ret = 0;

  if (_IOC_TYPE(cmd) != MYRING_IOC_MAGIC) return -ENOTTY;

  mutex_lock(&d->ioctl_mu);
  switch (cmd) {
    case MYRING_IOC_SET_WM: {
      struct myring_watermarks wm;
      if (copy_from_user(&wm, (void __user *)arg, sizeof(wm))) { ret = -EFAULT; break; }
      if (wm.hi_pct > 100 || wm.lo_pct > 100 || wm.lo_pct > wm.hi_pct) { ret = -EINVAL; break; }
      d->ctrl->hi_pct = wm.hi_pct;
      d->ctrl->lo_pct = wm.lo_pct;
      break;
    }
    case MYRING_IOC_SET_EVENTFD: {
      int efd;
      if (copy_from_user(&efd, (void __user *)arg, sizeof(efd))) { ret = -EFAULT; break; }
      if (d->evt) { eventfd_ctx_put(d->evt); d->evt = NULL; }
      if (efd >= 0) {
        d->evt = eventfd_ctx_fdget(efd);
        if (IS_ERR(d->evt)) { ret = PTR_ERR(d->evt); d->evt = NULL; }
      }
      break;
    }
    case MYRING_IOC_GET_STATS: {
      struct myring_stats st = {
        .head = smp_load_acquire(&d->ctrl->head),
        .tail = smp_load_acquire(&d->ctrl->tail),
        .drops = d->drops,
        .records = d->records,
        .bytes = d->bytes,
        .last_hi_cross_ns = 0,
        .last_lo_cross_ns = 0,
      };
      if (copy_to_user((void __user *)arg, &st, sizeof(st))) ret = -EFAULT;
      break;
    }
    case MYRING_IOC_ADVANCE_TAIL: {
      struct myring_advance adv;
      if (copy_from_user(&adv, (void __user *)arg, sizeof(adv))) { ret = -EFAULT; break; }
      /* allow user to advance up to head */
      uint64_t head = smp_load_acquire(&d->ctrl->head);
      uint64_t tail = smp_load_acquire(&d->ctrl->tail);
      if (adv.new_tail > head) { ret = -EINVAL; break; }
      if (adv.new_tail < tail) { ret = -EINVAL; break; }
      smp_store_release(&d->ctrl->tail, adv.new_tail);
      myring_maybe_notify(d); /* may drop below lo% */
      break;
    }
    case MYRING_IOC_RESET: {
      d->drops = d->records = d->bytes = 0;
      d->above_hi = false;
      d->ctrl->head = 0;
      d->ctrl->tail = 0;
      d->ctrl->flags = 0;
      d->ctrl->drop_start_ns = 0;
      d->ctrl->lost_in_drop = 0;
      break;
    }
    default:
      ret = -ENOTTY;
  }
  mutex_unlock(&d->ioctl_mu);
  return ret;
}

static __poll_t myring_poll(struct file *f, poll_table *wait)
{
  struct myring_dev *d = f->private_data;
  poll_wait(f, &d->wq, wait);

  if (rb_pct(rb_used(d->ctrl), d->ctrl->size) >= d->ctrl->hi_pct)
    return EPOLLIN | EPOLLRDNORM;
  return 0;
}

static int myring_mmap(struct file *f, struct vm_area_struct *vma)
{
  struct myring_dev *d = f->private_data;
  size_t len = vma->vm_end - vma->vm_start;

  if (len > d->vmem_len) return -EINVAL;

  vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
  return remap_vmalloc_range(vma, d->vmem, 0);
}

static const struct file_operations myring_fops = {
  .owner          = THIS_MODULE,
  .open           = myring_open,
  .release        = myring_release,
  .unlocked_ioctl = myring_ioctl,
#ifdef CONFIG_COMPAT
  .compat_ioctl   = myring_ioctl,
#endif
  .poll           = myring_poll,
  .mmap           = myring_mmap,
};

/* Init & Exit */

static int __init myring_init(void)
{
  int ret;
  size_t data_sz = 1ull << ring_order;
  size_t total = PAGE_SIZE + data_sz;

  memset(&g, 0, sizeof(g));
  init_waitqueue_head(&g.wq);
  mutex_init(&g.ioctl_mu);

  g.vmem = vzalloc(total);
  if (!g.vmem) return -ENOMEM;
  g.vmem_len = total;
  g.ctrl = (struct myring_ctrl *)g.vmem;
  g.data = g.vmem + PAGE_SIZE;
  g.size = data_sz;

  g.ctrl->head = 0;
  g.ctrl->tail = 0;
  g.ctrl->size = data_sz;
  g.ctrl->hi_pct = 50;
  g.ctrl->lo_pct = 30;
  g.ctrl->flags = 0;

  g.misc.minor = MISC_DYNAMIC_MINOR;
  g.misc.name = DRV_NAME;
  g.misc.fops = &myring_fops;
  g.misc.mode = 0666;

  ret = misc_register(&g.misc);
  if (ret) {
    vfree(g.vmem);
    return ret;
  }

  /* start synthetic producer */
  INIT_DELAYED_WORK(&g.prod_work, myring_prod_fn);
  g.stopping = false;
  schedule_delayed_work(&g.prod_work, msecs_to_jiffies(100));

  pr_info(DRV_NAME ": loaded, ring=%zu bytes, dev=/dev/%s\n", data_sz, g.misc.name);
  return 0;
}

static void __exit myring_exit(void)
{
  g.stopping = true;
  cancel_delayed_work_sync(&g.prod_work);

  if (g.evt) {
    eventfd_ctx_put(g.evt);
    g.evt = NULL;
  }
  misc_deregister(&g.misc);
  if (g.vmem) vfree(g.vmem);
  pr_info(DRV_NAME ": unloaded\n");
}

MODULE_LICENSE("MIT");
MODULE_AUTHOR("ChatGPT");
MODULE_DESCRIPTION("SPSC ring + eventfd + mmap + watermarks + drop indicator");
module_init(myring_init);
module_exit(myring_exit);
