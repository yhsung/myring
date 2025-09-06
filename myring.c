// SPDX-License-Identifier: GPL-2.0
// myring: miscdev + mmap SPSC ring + eventfd notify + watermarks + drop indicator
// Debian / aarch64 friendly
//
// Build: make
// Load : insmod myring.ko [rate_hz=2000 ring_order=22]
// Device: /dev/myring

#include <linux/module.h>
#include <linux/version.h>

/* Kernel version compatibility */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,3,0)
#define COMPAT_VM_FLAGS_SET(vma, flags) vm_flags_set(vma, flags)
#else
#define COMPAT_VM_FLAGS_SET(vma, flags) do { (vma)->vm_flags |= (flags); } while(0)
#endif
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
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>

// #define USE_NETFILTER 1
#ifdef USE_NETFILTER
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#endif

#include "myring_uapi.h"

#define DRV_NAME "myring"

/* Module params */
static unsigned int ring_order = 20; /* ring data size = 1<<order bytes (default 1MB) */
module_param(ring_order, uint, 0444);
MODULE_PARM_DESC(ring_order, "log2 of ring data bytes (default 20 -> 1MB)");

static unsigned int rate_hz = 2000; /* synthetic producer rate */
module_param(rate_hz, uint, 0644);
MODULE_PARM_DESC(rate_hz, "synthetic producer rate in Hz (default 2000)");

/* Device state */
struct myring_dev {
  struct miscdevice misc;
  struct myring_ctrl *ctrl;   /* first PAGE_SIZE */
  void *vmem;                 /* DMA coherent block (ctrl + data) */
  dma_addr_t dma_handle;      /* DMA physical address */
  size_t vmem_len;
  void *data;                 /* start of ring data region */
  uint64_t size;              /* ring data bytes (power-of-two) */
  struct device *dev;         /* device for DMA allocation */
  bool use_free_pages;        /* true if allocated with __get_free_pages */

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
  uint64_t seq_number;        /* monotonic sequence number for packets */

#ifdef USE_NETFILTER
  struct nf_hook_ops nfops;
#endif
};

static struct myring_dev __attribute__((aligned(PAGE_SIZE))) _this_dev;

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
  uint64_t head_before = smp_load_acquire(&c->head);
  uint64_t tail_before = smp_load_acquire(&c->tail);
  uint64_t used_before = head_before - tail_before;
  uint64_t free_before = c->size - used_before;
  
  struct myring_rec_hdr hdr = {
    .type = REC_TYPE_PKT,
    .flags = 0,
    .len = len,
    .ts_ns = ktime_get_ns(),
  };
  uint64_t pos;
  uint64_t need = sizeof(hdr) + len;

  printk(KERN_DEBUG "myring_push_packet: len=%u, need=%llu, free=%llu, head=%llu, tail=%llu\n",
         len, need, free_before, head_before, tail_before);

  if (!myring_reserve(c, need, &pos)) {
    printk(KERN_WARNING "myring_push_packet: FULL - need=%llu > free=%llu, dropping packet\n",
           need, free_before);
    myring_on_full(c);
    d->drops++;
    return;
  }

  printk(KERN_DEBUG "myring_push_packet: reserved pos=%llu, writing packet\n", pos);

  /* If we were dropping, emit the drop record first */
  myring_flush_drop_record(d);

  myring_write_bytes(d, pos, &hdr, sizeof(hdr));
  myring_write_bytes(d, pos + sizeof(hdr), payload, len);
  rb_commit_head(c, pos + need);

  d->records++;
  d->bytes += need;
  
  uint64_t head_after = pos + need;
  printk(KERN_DEBUG "myring_push_packet: SUCCESS - head updated %llu->%llu, records=%llu, bytes=%llu\n",
         head_before, head_after, d->records, d->bytes);

  myring_maybe_notify(d);
}

/* Synthetic producer work */
static void myring_prod_fn(struct work_struct *w)
{
  struct myring_dev *d = container_of(to_delayed_work(w), struct myring_dev, prod_work);
  if (d->stopping) return;

  /* Generate monotonic pattern payload */
  uint8_t buf[256];
  uint64_t *payload_u64 = (uint64_t*)buf;
  
  /* Header: timestamp + sequence number */
  payload_u64[0] = ktime_get_ns();      /* timestamp */
  payload_u64[1] = ++d->seq_number;     /* monotonic sequence */
  
  /* Generate predictable pattern based on sequence number */
  for (int i = 2; i < sizeof(buf) / sizeof(uint64_t); i++) {
    payload_u64[i] = d->seq_number * 0x123456789ABCDEF0ULL + i;
  }
  
  /* Fill remaining bytes with sequence-based pattern */
  for (int i = (sizeof(buf) / sizeof(uint64_t)) * sizeof(uint64_t); i < sizeof(buf); i++) {
    buf[i] = (uint8_t)(d->seq_number + i);
  }
  
  printk(KERN_DEBUG "myring_prod_fn: generating packet #%llu, timestamp=%llu\n",
         d->seq_number, payload_u64[0]);
  
  myring_push_packet(d, buf, sizeof(buf));

  if (!d->stopping) {
    unsigned long interval_ms = rate_hz ? max(1u, 1000u / rate_hz) : 1u;
    schedule_delayed_work(&d->prod_work, msecs_to_jiffies(interval_ms));
  }
}

/* File ops */

static int myring_open(struct inode *ino, struct file *f)
{
  printk(KERN_INFO "myring: device opened, vmem=%p, vmem_len=%zu\n", _this_dev.vmem, _this_dev.vmem_len);
  f->private_data = &_this_dev;
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
    case MYRING_IOC_GET_CONFIG: {
      struct myring_config cfg = {
        .ring_order = ring_order,
        .rate_hz = rate_hz,
        .ring_size = d->size,
      };
      if (copy_to_user((void __user *)arg, &cfg, sizeof(cfg))) ret = -EFAULT;
      break;
    }
    case MYRING_IOC_SET_RATE: {
      uint32_t new_rate;
      if (copy_from_user(&new_rate, (void __user *)arg, sizeof(new_rate))) { ret = -EFAULT; break; }
      if (new_rate == 0 || new_rate > 100000) { ret = -EINVAL; break; }
      rate_hz = new_rate;
      /* The new rate will take effect on the next work scheduling cycle */
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
  int ret;

  printk(KERN_INFO "myring: mmap called - requested len=%zu, vmem_len=%zu, vmem=%p\n", 
         len, d->vmem_len, d->vmem);
  printk(KERN_INFO "myring: PAGE_SIZE=%lu (kernel)\n", PAGE_SIZE);
  
  /* Dump VMA details */
  printk(KERN_INFO "myring: VMA dump:\n");
  printk(KERN_INFO "  vm_start=0x%lx, vm_end=0x%lx (len=%zu)\n", 
         vma->vm_start, vma->vm_end, len);
  printk(KERN_INFO "  vm_flags=0x%lx, vm_pgoff=%lu\n", 
         vma->vm_flags, vma->vm_pgoff);
  printk(KERN_INFO "  vm_file=%p, vm_private_data=%p\n",
         vma->vm_file, vma->vm_private_data);

  if (!d->vmem) {
    printk(KERN_ERR "myring: mmap failed - vmem is NULL\n");
    return -ENOMEM;
  }

  if (len > d->vmem_len) {
    printk(KERN_ERR "myring: mmap failed - len %zu > vmem_len %zu\n", len, d->vmem_len);
    return -EINVAL;
  }

  //printk(KERN_INFO "myring: setting VM flags\n");
  //COMPAT_VM_FLAGS_SET(vma, VM_DONTEXPAND | VM_DONTDUMP);

  
  /* Dump memory layout before remap */
  printk(KERN_INFO "myring: Memory layout:\n");
  printk(KERN_INFO "  PAGE_SIZE=%lu bytes\n", PAGE_SIZE);
  printk(KERN_INFO "  d->vmem=%p (vmalloc'd)\n", d->vmem);
  printk(KERN_INFO "  d->ctrl=%p (should be d->vmem)\n", d->ctrl);
  printk(KERN_INFO "  d->data=%p (should be d->vmem + PAGE_SIZE=%p)\n", d->data, d->vmem + PAGE_SIZE);
  printk(KERN_INFO "  d->size=%zu (ring data size)\n", d->size);
  printk(KERN_INFO "  d->vmem_len=%zu (total allocation = PAGE_SIZE + ring_data)\n", d->vmem_len);
  printk(KERN_INFO "  Expected: vmem_len=%zu + %lu = %zu\n", d->size, PAGE_SIZE, d->size + PAGE_SIZE);
  
  /* DMA coherent memory is always valid for CPU access and DMA */
  printk(KERN_INFO "myring: DMA coherent memory allocated\n");
  printk(KERN_INFO "myring: Virtual address: %p, Physical/DMA address: 0x%llx\n", 
         d->vmem, (unsigned long long)d->dma_handle);
  
  if (d->dma_handle != 0) {
    printk(KERN_INFO "myring: calling remap_pfn_range for physically contiguous memory\n");
    ret = remap_pfn_range(vma, vma->vm_start, 
                          virt_to_phys(d->vmem) >> PAGE_SHIFT,
                          len, vma->vm_page_prot);
  } else {
    printk(KERN_INFO "myring: calling remap_vmalloc_range for vmalloc memory\n");
    ret = remap_vmalloc_range(vma, d->vmem, 0);
  }
  
  printk(KERN_INFO "myring: remap_vmalloc_range returned %d\n", ret);
  if (ret != 0) {
    printk(KERN_ERR "myring: remap_vmalloc_range FAILED with %d\n", ret);
    switch(ret) {
      case -EINVAL:
        printk(KERN_ERR "  -EINVAL: Invalid argument (bad vma or vmem)\n");
        break;
      case -ENOMEM:
        printk(KERN_ERR "  -ENOMEM: Out of memory\n");
        break;
      default:
        printk(KERN_ERR "  Unknown error code\n");
        break;
    }
  }
  
  printk(KERN_INFO "myring: mmap %s, ret=%d\n", ret == 0 ? "SUCCESS" : "FAILED", ret);
  return ret;
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

  printk(KERN_INFO "myring: initializing module, ring_order=%u, data_sz=%zu, total=%zu\n", 
         ring_order, data_sz, total);

  memset(&_this_dev, 0, sizeof(_this_dev));
  init_waitqueue_head(&_this_dev.wq);
  mutex_init(&_this_dev.ioctl_mu);

  /* Try multiple allocation strategies for physically contiguous memory */
  _this_dev.dev = NULL;
  _this_dev.use_free_pages = false;
  _this_dev.vmem = NULL;
  
  unsigned int order = get_order(total);
  printk(KERN_INFO "myring: trying to allocate %zu bytes (order %u)\n", total, order);
  
  /* Strategy 1: __get_free_pages with reduced flags */
  if (order <= 10) { /* Only try for reasonable sizes */
    unsigned long page = __get_free_pages(GFP_KERNEL, order);
    if (page) {
      _this_dev.vmem = (void*)page;
      _this_dev.dma_handle = virt_to_phys(_this_dev.vmem);
      _this_dev.use_free_pages = true;
      memset(_this_dev.vmem, 0, total);
      printk(KERN_INFO "myring: __get_free_pages succeeded, vmem=%p, phys_addr=0x%llx, order=%u\n", 
             _this_dev.vmem, (unsigned long long)_this_dev.dma_handle, order);
    }
  }
  
  /* Strategy 2: Try DMA coherent allocation */
  if (!_this_dev.vmem) {
    _this_dev.vmem = dma_alloc_coherent(NULL, total, &_this_dev.dma_handle, GFP_KERNEL);
    if (_this_dev.vmem) {
      _this_dev.use_free_pages = false;
      printk(KERN_INFO "myring: dma_alloc_coherent succeeded, vmem=%p, dma_handle=0x%llx\n", 
             _this_dev.vmem, (unsigned long long)_this_dev.dma_handle);
    }
  }
  
  /* Strategy 3: Fallback to vmalloc (not physically contiguous but works) */
  if (!_this_dev.vmem) {
    printk(KERN_WARNING "myring: contiguous allocation failed, falling back to vmalloc\n");
    printk(KERN_WARNING "myring: WARNING - memory will NOT be physically contiguous for DMA\n");
    _this_dev.vmem = vzalloc(total);
    if (_this_dev.vmem) {
      _this_dev.dma_handle = 0; /* Invalid for DMA */
      _this_dev.use_free_pages = false;
      printk(KERN_INFO "myring: vmalloc succeeded, vmem=%p (NOT DMA-suitable)\n", _this_dev.vmem);
    }
  }
  
  if (!_this_dev.vmem) {
    printk(KERN_ERR "myring: all allocation strategies failed\n");
    return -ENOMEM;
  }
  _this_dev.vmem_len = total;
  _this_dev.ctrl = (struct myring_ctrl *)_this_dev.vmem;
  _this_dev.data = (uint8_t*)_this_dev.vmem + PAGE_SIZE;
  _this_dev.size = data_sz;
  printk(KERN_INFO "myring: _this_dev.vmem_len=%zu, _this_dev.size=%zu, data_sz=%zu\n", _this_dev.vmem_len, _this_dev.size, data_sz);
  printk(KERN_INFO "myring: Pointer layout: vmem=%p, ctrl=%p, data=%p\n", _this_dev.vmem, _this_dev.ctrl, _this_dev.data);
  printk(KERN_INFO "myring: Pointer arithmetic: vmem + PAGE_SIZE(%lu) = %p (should equal data)\n", 
         PAGE_SIZE, ((uint8_t*)_this_dev.vmem) + PAGE_SIZE);
  printk(KERN_INFO "myring: Data pointer offset: %ld bytes from vmem\n", 
         (long)((uint8_t*)_this_dev.data - (uint8_t*)_this_dev.vmem));

  _this_dev.ctrl->head = 0;
  _this_dev.ctrl->tail = 0;
  _this_dev.ctrl->size = data_sz;
  printk(KERN_INFO "myring: initialized ctrl->size=%llu (should be data_sz=%zu)\n", _this_dev.ctrl->size, data_sz);
  _this_dev.ctrl->hi_pct = 50;
  _this_dev.ctrl->lo_pct = 30;
  _this_dev.ctrl->flags = 0;

  _this_dev.misc.minor = MISC_DYNAMIC_MINOR;
  _this_dev.misc.name = DRV_NAME;
  _this_dev.misc.fops = &myring_fops;
  _this_dev.misc.mode = 0666;

  printk(KERN_INFO "myring: registering misc device, name=%s\n", _this_dev.misc.name);
  ret = misc_register(&_this_dev.misc);
  if (ret) {
    printk(KERN_ERR "myring: misc_register failed, ret=%d\n", ret);
    if (_this_dev.vmem) {
      if (_this_dev.use_free_pages) {
        unsigned int order = get_order(_this_dev.vmem_len);
        free_pages((unsigned long)_this_dev.vmem, order);
      } else if (_this_dev.dma_handle != 0) {
        dma_free_coherent(_this_dev.dev, _this_dev.vmem_len, _this_dev.vmem, _this_dev.dma_handle);
      } else {
        vfree(_this_dev.vmem);
      }
    }
    return ret;
  }
  printk(KERN_INFO "myring: misc device registered successfully\n");

  /* start synthetic producer */
  INIT_DELAYED_WORK(&_this_dev.prod_work, myring_prod_fn);
  _this_dev.stopping = false;
  _this_dev.seq_number = 0;  /* Initialize sequence counter */
  schedule_delayed_work(&_this_dev.prod_work, msecs_to_jiffies(100));

  pr_info(DRV_NAME ": loaded, ring=%zu bytes, dev=/dev/%s\n", data_sz, _this_dev.misc.name);
  return 0;
}

static void __exit myring_exit(void)
{
  _this_dev.stopping = true;
  cancel_delayed_work_sync(&_this_dev.prod_work);

  if (_this_dev.evt) {
    eventfd_ctx_put(_this_dev.evt);
    _this_dev.evt = NULL;
  }
  misc_deregister(&_this_dev.misc);
  if (_this_dev.vmem) {
    if (_this_dev.use_free_pages) {
      unsigned int order = get_order(_this_dev.vmem_len);
      free_pages((unsigned long)_this_dev.vmem, order);
    } else if (_this_dev.dma_handle != 0) {
      dma_free_coherent(_this_dev.dev, _this_dev.vmem_len, _this_dev.vmem, _this_dev.dma_handle);
    } else {
      vfree(_this_dev.vmem);
    }
  }
  pr_info(DRV_NAME ": unloaded\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ChatGPT");
MODULE_DESCRIPTION("SPSC ring + eventfd + mmap + watermarks + drop indicator");
module_init(myring_init);
module_exit(myring_exit);
