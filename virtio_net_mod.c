/*
 * Loadable virtio-net (legacy PCI, I/O BAR0) for Cact kmod.
 * Lives under source/Virtio-net-for-Cact (outside kernel tree).
 * Build: make  (KERN_ROOT defaults to ../CactKernel-x86_32), or from kernel: make virtio-module
 * Load (root): modload virtio_net.cctk  — uses cact_pci_* manifest below;
 *   or modload PATH 0x1AF4 0x1000 to override.
 */

#include <stddef.h>
#include <stdint.h>
#include "virtio_net.h"
#include "pci_enum.h"
#include "net.h"

/* ── kernel entry points (resolved via ksym at load time) ───────── */
extern void kprint(char* s);
extern void kprint_hex(uint32_t v);
extern void* kmalloc_aligned(uint32_t size, uint32_t align);
extern void kfree_aligned(void* p);
extern uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
extern void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                        uint32_t val);
extern uint8_t  port_byte_in(uint16_t port);
extern void     port_byte_out(uint16_t port, uint8_t val);
extern void     port_word_out(uint16_t port, uint16_t val);
extern uint32_t port_long_in(uint16_t port);
extern void     port_long_out(uint16_t port, uint32_t val);
extern void     net_register_driver(net_driver_t* drv);
extern void     net_unregister_driver(net_driver_t* drv);
extern void     net_receive(skb_t* skb);
extern void*    memset(void* s, int c, size_t n);
extern void     irq_register_handler(unsigned int irq, void (*handler)(void));
extern void     net_driver_irq_wake(void);
/* log_level_t: LOG_OK=0 LOG_WARN=1 LOG_ERROR=2 LOG_FAIL=3 (kernel.h) */
extern void     klog(int level, const char* message);
extern skb_t*   skb_alloc(void);
extern void     skb_free(skb_t* skb);
extern uint8_t* skb_push(skb_t* skb, uint16_t len);
extern uint8_t* skb_data(skb_t* skb);
extern uint16_t skb_len(skb_t* skb);

/* Manifest: kernel pci_peek_module_manifest() reads these from the .cctk (no VID/DID on cmdline). */
const uint16_t cact_pci_vendor_id = 0x1AF4;
/* Zero ends list; first DID present on the PCI bus is preferred for binding. */
const uint16_t cact_pci_device_ids[] = {0x1000, 0x1041, 0};
/* Optional: restrict by class (not set — match by VID/DID only). */
/* const uint8_t cact_pci_class = 0x02; */
/* const uint8_t cact_pci_subclass = 0x00; */

static uint16_t io_base;
static uint8_t  vnet_bus, vnet_dev, vnet_fn;
static uint32_t vnet_saved_pci_cmd_dw;
static int      virtio_net_ready;
static uint8_t  vnet_irq_line;
static int      vnet_irq_armed;

typedef struct {
    virtq_desc_t*  desc;
    virtq_avail_t* avail;
    virtq_used_t*  used;
    uint16_t       size;
    uint16_t       last_used_idx;
    uint16_t       free_head;
    uint16_t       num_free;
    skb_t*         rx_skbs[VIRTQ_MAX_SIZE];
} virtqueue_t;

static virtqueue_t vq_rx;
static virtqueue_t vq_tx;

#define KLOG_OK    0
#define KLOG_WARN  1
#define KLOG_ERROR 2
#define KLOG_FAIL  3

static uint32_t align_up(uint32_t addr, uint32_t align) {
    return (addr + align - 1) & ~(align - 1);
}

static void virtq_alloc(virtqueue_t* vq, uint16_t size) {
    vq->size = size;
    vq->last_used_idx = 0;
    vq->free_head     = 0;
    vq->num_free      = size;

    uint32_t desc_sz  = sizeof(virtq_desc_t) * size;
    uint32_t avail_sz = sizeof(uint16_t) * (3 + size);
    uint32_t used_sz  = sizeof(uint16_t) * 3 + sizeof(virtq_used_elem_t) * size;

    uint32_t avail_off = desc_sz;
    uint32_t used_off  = align_up(avail_off + avail_sz, 4096);
    uint32_t total     = used_off + used_sz;

    uint8_t* mem = (uint8_t*)kmalloc_aligned(total, 4096);
    vq->desc  = (virtq_desc_t*)mem;
    vq->avail = (virtq_avail_t*)(mem + avail_off);
    vq->used  = (virtq_used_t*)(mem + used_off);

    for (int i = 0; i < size - 1; i++) {
        vq->desc[i].flags = VIRTQ_DESC_F_NEXT;
        vq->desc[i].next  = i + 1;
    }
    vq->desc[size - 1].flags = 0;
    vq->desc[size - 1].next  = 0xFFFF;
}

static void virtq_activate(virtqueue_t* vq, uint16_t queue_idx) {
    port_word_out(io_base + VIRTIO_PCI_QUEUE_SELECT, queue_idx);
    uint32_t pfn = (uint32_t)(uintptr_t)vq->desc / 4096;
    port_long_out(io_base + VIRTIO_PCI_QUEUE_PFN, pfn);
}

static void virtq_deactivate(uint16_t queue_idx) {
    port_word_out(io_base + VIRTIO_PCI_QUEUE_SELECT, queue_idx);
    port_long_out(io_base + VIRTIO_PCI_QUEUE_PFN, 0);
}

static void pic_mask_line(unsigned irq) {
    if (irq >= 16) return;
    uint16_t port = irq < 8 ? 0x21u : 0xA1u;
    uint8_t  line = irq < 8 ? (uint8_t)irq : (uint8_t)(irq - 8);
    uint8_t  m    = port_byte_in(port);
    m |= (uint8_t)(1u << line);
    port_byte_out(port, m);
}

static void pic_unmask_line(unsigned irq) {
    if (irq >= 16) return;
    uint16_t port = irq < 8 ? 0x21u : 0xA1u;
    uint8_t  line = irq < 8 ? (uint8_t)irq : (uint8_t)(irq - 8);
    uint8_t  m    = port_byte_in(port);
    m &= (uint8_t)~(1u << line);
    port_byte_out(port, m);
}

static void virtio_net_free_rx_skbs(void) {
    for (int i = 0; i < VIRTQ_MAX_SIZE; i++) {
        if (vq_rx.rx_skbs[i]) {
            skb_free(vq_rx.rx_skbs[i]);
            vq_rx.rx_skbs[i] = NULL;
        }
    }
}

static uint16_t desc_alloc(virtqueue_t* vq) {
    if (vq->num_free == 0) return 0xFFFF;
    uint16_t idx  = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->num_free--;
    return idx;
}

static void desc_free(virtqueue_t* vq, uint16_t idx) {
    vq->desc[idx].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[idx].next  = vq->free_head;
    vq->free_head       = idx;
    vq->num_free++;
}

static void rx_fill(void) {
    while (vq_rx.num_free >= 2) {
        skb_t* skb = skb_alloc();
        if (!skb) break;

        uint16_t d0 = desc_alloc(&vq_rx);
        uint16_t d1 = desc_alloc(&vq_rx);

        virtio_net_hdr_t* hdr = (virtio_net_hdr_t*)skb->data;
        skb->data_offset = sizeof(virtio_net_hdr_t);

        vq_rx.desc[d0].addr  = (uint32_t)(uintptr_t)hdr;
        vq_rx.desc[d0].len   = sizeof(virtio_net_hdr_t);
        vq_rx.desc[d0].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
        vq_rx.desc[d0].next  = d1;

        vq_rx.desc[d1].addr  = (uint32_t)(uintptr_t)(skb->data + skb->data_offset);
        vq_rx.desc[d1].len   = SKB_MAX_SIZE - sizeof(virtio_net_hdr_t);
        vq_rx.desc[d1].flags = VIRTQ_DESC_F_WRITE;
        vq_rx.desc[d1].next  = 0;

        vq_rx.rx_skbs[d0] = skb;

        uint16_t avail_idx = vq_rx.avail->idx % vq_rx.size;
        vq_rx.avail->ring[avail_idx] = d0;
        __asm__ __volatile__("" ::: "memory");
        vq_rx.avail->idx++;
    }
    port_word_out(io_base + VIRTIO_PCI_QUEUE_NOTIFY, VIRTIO_NET_QUEUE_RX);
}

static int virtio_net_send(skb_t* skb) {
    if (!skb || vq_tx.num_free < 2) return -1;

    virtio_net_hdr_t* hdr = (virtio_net_hdr_t*)skb_push(skb, sizeof(virtio_net_hdr_t));
    hdr->flags       = 0;
    hdr->gso_type    = VIRTIO_NET_HDR_GSO_NONE;
    hdr->hdr_len     = 0;
    hdr->gso_size    = 0;
    hdr->csum_start  = 0;
    hdr->csum_offset = 0;

    uint16_t d0 = desc_alloc(&vq_tx);
    vq_tx.desc[d0].addr  = (uint32_t)(uintptr_t)skb_data(skb);
    vq_tx.desc[d0].len   = skb_len(skb);
    vq_tx.desc[d0].flags = 0;
    vq_tx.desc[d0].next  = 0;

    uint16_t avail_idx = vq_tx.avail->idx % vq_tx.size;
    vq_tx.avail->ring[avail_idx] = d0;
    __asm__ __volatile__("" ::: "memory");
    vq_tx.avail->idx++;

    port_word_out(io_base + VIRTIO_PCI_QUEUE_NOTIFY, VIRTIO_NET_QUEUE_TX);
    return 0;
}

static void virtio_net_drain_rx(void) {
    while (vq_rx.last_used_idx != vq_rx.used->idx) {
        uint16_t used_idx = vq_rx.last_used_idx % vq_rx.size;
        virtq_used_elem_t* elem = &vq_rx.used->ring[used_idx];

        uint16_t d0  = (uint16_t)elem->id;
        uint32_t len = elem->len;
        vq_rx.last_used_idx++;

        skb_t* skb = vq_rx.rx_skbs[d0];
        if (skb) {
            skb->total_len = (uint16_t)(len - sizeof(virtio_net_hdr_t));
            vq_rx.rx_skbs[d0] = NULL;

            uint16_t d1 = vq_rx.desc[d0].next;
            desc_free(&vq_rx, d1);
            desc_free(&vq_rx, d0);

            net_receive(skb);
        }
    }

    while (vq_tx.last_used_idx != vq_tx.used->idx) {
        uint16_t used_idx = vq_tx.last_used_idx % vq_tx.size;
        uint16_t d0 = (uint16_t)vq_tx.used->ring[used_idx].id;
        vq_tx.last_used_idx++;
        desc_free(&vq_tx, d0);
    }

    rx_fill();
}

static void virtio_net_poll(void) { virtio_net_drain_rx(); }

static void virtio_net_irq_handler(void) {
    if (!virtio_net_ready || io_base == 0) return;
    port_byte_in(io_base + VIRTIO_PCI_ISR);
    net_driver_irq_wake();
}

static void virtio_net_get_mac(mac_addr_t* out) {
    for (int i = 0; i < 6; i++)
        out->b[i] = port_byte_in(io_base + VIRTIO_PCI_NET_MAC + i);
}

static net_driver_t virtio_driver = {
    .name    = "virtio-net (legacy)",
    .send    = virtio_net_send,
    .poll    = virtio_net_poll,
    .get_mac = virtio_net_get_mac,
};

static void virtio_detach(void) {
    int had_any = virtio_net_ready || io_base != 0 || vq_rx.desc != NULL ||
                  vq_tx.desc != NULL || vnet_irq_armed;

    if (vnet_irq_armed && vnet_irq_line < 16) {
        irq_register_handler(vnet_irq_line, NULL);
        pic_mask_line(vnet_irq_line);
        vnet_irq_armed = 0;
    }

    if (virtio_net_ready) {
        net_unregister_driver(&virtio_driver);
        virtio_net_ready = 0;
    }

    if (io_base != 0) {
        virtq_deactivate(VIRTIO_NET_QUEUE_RX);
        virtq_deactivate(VIRTIO_NET_QUEUE_TX);
        port_byte_out(io_base + VIRTIO_PCI_STATUS, 0);
    }

    if (vnet_bus || vnet_dev || vnet_fn)
        pci_write32(vnet_bus, vnet_dev, vnet_fn, 0x04, vnet_saved_pci_cmd_dw);

    virtio_net_free_rx_skbs();

    if (vq_rx.desc) {
        kfree_aligned(vq_rx.desc);
        memset(&vq_rx, 0, sizeof(vq_rx));
    }
    if (vq_tx.desc) {
        kfree_aligned(vq_tx.desc);
        memset(&vq_tx, 0, sizeof(vq_tx));
    }

    io_base = 0;
    vnet_bus = vnet_dev = vnet_fn = 0;
    vnet_saved_pci_cmd_dw = 0;

    if (had_any)
        klog(KLOG_OK, "virtio-net (kmod): unloaded");
}

int pci_driver_probe(pci_device_t* dev) {
    if (!dev) return -1;

    virtio_detach();

    kprint((char*)"[virtio_net mod] probe bus=");
    kprint_hex(dev->bus);
    kprint((char*)" dev=");
    kprint_hex(dev->dev);
    kprint((char*)" fn=");
    kprint_hex(dev->fn);
    kprint((char*)"\n");

    if (!dev->bars[0].is_io || dev->bars[0].base == 0) {
        klog(KLOG_FAIL,
             "virtio-net (kmod): BAR0 not I/O — need legacy virtio (e.g. virtio-net-pci,disable-modern=on)");
        return -1;
    }

    io_base = (uint16_t)dev->bars[0].base;
    vnet_bus = dev->bus;
    vnet_dev = dev->dev;
    vnet_fn  = dev->fn;

    vnet_saved_pci_cmd_dw = pci_read32(vnet_bus, vnet_dev, vnet_fn, 0x04);
    uint16_t cmd = (uint16_t)(vnet_saved_pci_cmd_dw & 0xFFFFu);
    pci_write32(vnet_bus, vnet_dev, vnet_fn, 0x04, cmd | 0x05);

    port_byte_out(io_base + VIRTIO_PCI_STATUS, 0);
    port_byte_out(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    port_byte_out(io_base + VIRTIO_PCI_STATUS,
                  VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    uint32_t host_feat = port_long_in(io_base + VIRTIO_PCI_HOST_FEATURES);
    uint32_t our_feat  = host_feat & (VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS);
    port_long_out(io_base + VIRTIO_PCI_GUEST_FEATURES, our_feat);

    virtq_alloc(&vq_rx, VIRTQ_MAX_SIZE);
    virtq_alloc(&vq_tx, VIRTQ_MAX_SIZE);
    if (!vq_rx.desc || !vq_tx.desc) {
        klog(KLOG_FAIL, "virtio-net (kmod): virtqueue allocation failed");
        virtio_detach();
        return -1;
    }

    virtq_activate(&vq_rx, VIRTIO_NET_QUEUE_RX);
    virtq_activate(&vq_tx, VIRTIO_NET_QUEUE_TX);
    port_byte_out(io_base + VIRTIO_PCI_STATUS,
                  VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    virtio_net_get_mac(&virtio_driver.mac);
    net_register_driver(&virtio_driver);
    rx_fill();

    vnet_irq_line = dev->irq_line;
    if (vnet_irq_line < 16) {
        irq_register_handler(vnet_irq_line, virtio_net_irq_handler);
        pic_unmask_line(vnet_irq_line);
        vnet_irq_armed = 1;
    } else {
        vnet_irq_armed = 0;
        klog(KLOG_WARN, "virtio-net (kmod): no valid PCI IRQ line — RX via poll only");
    }

    virtio_net_ready = 1;

    kprint((char*)"\n*** virtio-net (kmod): SUCCESS — NIC online ***\n");
    kprint((char*)"    MAC ");
    for (int i = 0; i < 6; i++) {
        if (i) kprint((char*)":");
        kprint_hex(virtio_driver.mac.b[i]);
    }
    kprint((char*)"\n    PCI IRQ line ");
    kprint_hex(vnet_irq_line);
    kprint((char*)"\n");

    klog(KLOG_OK, "virtio-net (kmod): driver attached — network stack ready");
    return 0;
}

void pci_driver_remove(pci_device_t* dev) {
    (void)dev;
    virtio_detach();
}
