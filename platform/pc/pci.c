#include <kernel.h>
#include <apic.h>
#include <pci.h>
#include <io.h>

//#define PCI_PLATFORM_DEBUG
#ifdef PCI_PLATFORM_DEBUG
#define pci_plat_debug rprintf
#else
#define pci_plat_debug(...) do { } while(0)
#endif

#define CONF1_ADDR_PORT    0x0cf8
#define CONF1_DATA_PORT    0x0cfc

#define CONF1_ENABLE       0x80000000ul
#define CONF1_ENABLE_CHK   0x80000000ul
#define CONF1_ENABLE_MSK   0x7f000000ul
#define CONF1_ENABLE_CHK1  0xff000001ul
#define CONF1_ENABLE_MSK1  0x80000001ul
#define CONF1_ENABLE_RES1  0x80000000ul

#define CONF2_ENABLE_PORT  0x0cf8
#define CONF2_FORWARD_PORT 0x0cfa

#define CONF2_ENABLE_CHK   0x0e
#define CONF2_ENABLE_RES   0x0e

/* enable configuration space accesses and return data port address */
static int pci_cfgenable(pci_dev dev, int reg, int bytes)
{
    pci_plat_debug("%s: dev %p, dev->bus %d, reg %d, bytes %d\n",
                   __func__, dev, dev->bus, reg, bytes);
    int dataport = 0;

    if (dev->bus <= PCI_BUSMAX && dev->slot <= PCI_SLOTMAX && dev->function <= PCI_FUNCMAX &&
        (unsigned)reg <= PCI_REGMAX && bytes != 3 &&
        (unsigned)bytes <= 4 && (reg & (bytes - 1)) == 0) {
        out32(CONF1_ADDR_PORT, (1U << 31) | (dev->bus << 16) | (dev->slot << 11)
              | (dev->function << 8) | (reg & ~0x03));
        dataport = CONF1_DATA_PORT + (reg & 0x03);
    }
    return (dataport);
}

u32 pci_cfgread(pci_dev dev, int reg, int bytes)
{
    pci_plat_debug("%s: dev %p, dev->bus %d, reg %d, bytes %d\n",
                   __func__, dev, dev->bus, reg, bytes);
    u32 data = -1;
    int port;

    u64 flags = irq_disable_save();
    port = pci_cfgenable(dev, reg, bytes);
    if (port != 0) {
        switch (bytes) {
        case 1:
            data = in8(port);
            break;
        case 2:
            data = in16(port);
            break;
        case 4:
            data = in32(port);
            break;
        }
    }
    irq_restore(flags);
    return (data);
}

void pci_cfgwrite(pci_dev dev, int reg, int bytes, u32 source)
{
    pci_plat_debug("%s: dev %p, dev->bus %d, reg %d, bytes %d, source 0x%x\n",
                   __func__, dev, dev->bus, reg, bytes, source);
    int port;

    u64 flags = irq_disable_save();
    port = pci_cfgenable(dev, reg, bytes);
    if (port != 0) {
        switch (bytes) {
        case 1:
            out8(port, source);
            break;
        case 2:
            out16(port, source);
            break;
        case 4:
            out32(port, source);
            break;
        }
    }
    irq_restore(flags);
}

/* Seeing bad effects if an interrupt is caught after port I/O; disable irqs as precaution */
u8 pci_bar_read_1(struct pci_bar *b, u64 offset)
{
    pci_plat_debug("%s: pci_bar %p, offset 0x%lx\n", __func__, b, offset);
    u64 flags = irq_disable_save();
    u8 v = b->type == PCI_BAR_MEMORY ? *(u8 *) (b->vaddr + offset) : in8(b->addr + offset);
    irq_restore(flags);
    return v;
}

void pci_bar_write_1(struct pci_bar *b, u64 offset, u8 val)
{
    pci_plat_debug("%s: pci_bar %p, offset 0x%lx, val 0x%x\n", __func__, b, offset, val);
    u64 flags = irq_disable_save();
    if (b->type == PCI_BAR_MEMORY)
        *(u8 *) (b->vaddr + offset) = val;
    else
        out8(b->addr + offset, val);
    irq_restore(flags);
}

u16 pci_bar_read_2(struct pci_bar *b, u64 offset)
{
    pci_plat_debug("%s: pci_bar %p, offset 0x%lx\n", __func__, b, offset);
    u64 flags = irq_disable_save();
    u16 v = b->type == PCI_BAR_MEMORY ? *(u16 *) (b->vaddr + offset) : in16(b->addr + offset);
    irq_restore(flags);
    return v;
}

void pci_bar_write_2(struct pci_bar *b, u64 offset, u16 val)
{
    pci_plat_debug("%s: pci_bar %p, offset 0x%lx, val 0x%x\n", __func__, b, offset, val);
    u64 flags = irq_disable_save();
    if (b->type == PCI_BAR_MEMORY)
        *(u16 *) (b->vaddr + offset) = val;
    else
        out16(b->addr + offset, val);
    irq_restore(flags);
}

u32 pci_bar_read_4(struct pci_bar *b, u64 offset)
{
    pci_plat_debug("%s: pci_bar %p, offset 0x%lx\n", __func__, b, offset);
    u64 flags = irq_disable_save();
    u32 v = b->type == PCI_BAR_MEMORY ? *(u32 *) (b->vaddr + offset) : in32(b->addr + offset);
    irq_restore(flags);
    return v;
}

void pci_bar_write_4(struct pci_bar *b, u64 offset, u32 val)
{
    pci_plat_debug("%s: pci_bar %p, offset 0x%lx, val 0x%x\n", __func__, b, offset, val);
    u64 flags = irq_disable_save();
    if (b->type == PCI_BAR_MEMORY)
        *(u32 *) (b->vaddr + offset) = val;
    else
        out32(b->addr + offset, val);
    irq_restore(flags);
}

u64 pci_bar_read_8(struct pci_bar *b, u64 offset)
{
    u64 flags = irq_disable_save();
    u64 v = b->type == PCI_BAR_MEMORY ? *(u64 *) (b->vaddr + offset) : in64(b->addr + offset);
    irq_restore(flags);
    return v;
}

void pci_bar_write_8(struct pci_bar *b, u64 offset, u64 val)
{
    u64 flags = irq_disable_save();
    if (b->type == PCI_BAR_MEMORY)
        *(u64 *) (b->vaddr + offset) = val;
    else
        out64(b->addr + offset, val);
    irq_restore(flags);
}

void pci_setup_non_msi_irq(pci_dev dev, thunk h, const char *name)
{
    pci_plat_debug("%s: h %F, name %s\n", __func__, h, name);

    /* For maximum portability, the GSI should be retrieved via the ACPI _PRT method. */
    unsigned int gsi = pci_cfgread(dev, PCIR_INTERRUPT_LINE, 1);

    ioapic_register_int(gsi, h, name);
}

void pci_platform_init_bar(pci_dev dev, int bar)
{
    pci_plat_debug("%s: dev %p, %d:%d:%d, bar %d\n", __func__, dev,
                   dev->bus, dev->slot, dev->function, bar);
    u64 base = pci_cfgread(dev, PCIR_BAR(bar), 4);
    boolean is_io = (base & PCI_BAR_B_TYPE_MASK) == PCI_BAR_IOPORT;
    if (base & (is_io ? ~PCI_BAR_B_IOPORT_MASK : ~PCI_BAR_B_MEMORY_MASK))
        return; /* BAR configured by BIOS */
    if (is_io) {
        msg_err("I/O port resource allocation not supported (%d:%d:%d, bar %d)\n",
                dev->bus, dev->slot, dev->function, bar);
        return;
    }
    id_heap iomem = pci_bus_get_iomem(dev->bus);
    if (!iomem) {
        msg_err("I/O memory heap not available for bus %d\n", dev->bus);
        return;
    }
    base = id_heap_alloc_subrange(iomem,
        pci_bar_size(dev, PCI_BAR_MEMORY, base & PCI_BAR_B_MEMORY_MASK, bar), 0, U64_FROM_BIT(32));
    if (base != INVALID_PHYSICAL) {
        pci_plat_debug("   allocated base 0x%x\n", base);
        pci_cfgwrite(dev, PCIR_BAR(bar), 4, base);
    } else {
        msg_err("failed to allocate I/O memory (%d:%d:%d, bar %d)\n",
                dev->bus, dev->slot, dev->function, bar);
    }
}

u64 pci_platform_allocate_msi(pci_dev dev, thunk h, const char *name, u32 *address, u32 *data)
{
    u64 v = allocate_interrupt();
    if (v == INVALID_PHYSICAL)
        return v;
    register_interrupt(v, h, name);
    msi_format(address, data, v);
    return v;
}

void pci_platform_deallocate_msi(pci_dev dev, u64 v)
{
    unregister_interrupt(v);
    deallocate_interrupt(v);
}

boolean pci_platform_has_msi(void)
{
    return true;
}

