#define _FILE_OFFSET_BITS 64

#include "mmio.h"

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <pci/pci.h>

#define PCI_BAR_IO_FLAG 0x1u
#define PCI_BAR_FLAGS_MASK 0xFu

typedef struct {
    off_t file_offset;
    size_t mapping_size;
    size_t register_offset;
} MmioPlan;

static bool resolve_bar0_range(const struct pci_dev *pci_device,
    uint32_t offset, size_t span, pciaddr_t *physical)
{
    pciaddr_t bar;
    pciaddr_t bar_size;
    pciaddr_t max_address;

    if (!pci_device->base_addr[0] || !pci_device->size[0])
        return false;

    if (pci_device->base_addr[0] & PCI_BAR_IO_FLAG)
        return false;

    bar_size = pci_device->size[0];

    if ((pciaddr_t)offset > bar_size)
        return false;

    if ((uintmax_t)span >
        (uintmax_t)(bar_size - (pciaddr_t)offset)) {
        return false;
    }

    bar = pci_device->base_addr[0] & ~(pciaddr_t)PCI_BAR_FLAGS_MASK;
    max_address = ~(pciaddr_t)0;

    if ((pciaddr_t)offset > max_address - bar)
        return false;

    *physical = bar + (pciaddr_t)offset;
    return true;
}

static bool plan_mapping(pciaddr_t physical, size_t span,
    long page_size, MmioPlan *plan)
{
    pciaddr_t page;
    pciaddr_t max_address;
    pciaddr_t mapping_base;
    uintmax_t page_value;
    size_t page_bytes;
    size_t register_offset;
    size_t required_size;
    size_t mapping_size;
    off_t file_offset;

    if (page_size <= 0)
        return false;

    page_value = (uintmax_t)page_size;
    max_address = ~(pciaddr_t)0;

    if (page_value > (uintmax_t)SIZE_MAX ||
        page_value > (uintmax_t)max_address) {
        return false;
    }

    page = (pciaddr_t)page_value;
    page_bytes = (size_t)page_value;
    mapping_base = physical - physical % page;
    register_offset = (size_t)(physical - mapping_base);

    if (span > SIZE_MAX - register_offset)
        return false;

    required_size = register_offset + span;

    if (required_size > SIZE_MAX - (page_bytes - 1u))
        return false;

    mapping_size = (required_size + page_bytes - 1u) /
        page_bytes * page_bytes;
    file_offset = (off_t)mapping_base;

    if (file_offset < 0 || (pciaddr_t)file_offset != mapping_base)
        return false;

    plan->file_offset = file_offset;
    plan->mapping_size = mapping_size;
    plan->register_offset = register_offset;
    return true;
}

bool mmio_map_bar0(MmioRegion *region, int memory_fd,
    const struct pci_dev *pci_device, uint32_t offset,
    size_t span, long page_size)
{
    pciaddr_t physical;
    MmioPlan plan;
    void *mapping;

    if (!region)
        return false;

    memset(region, 0, sizeof(*region));

    if (!pci_device || !span)
        return false;

    if (!resolve_bar0_range(pci_device, offset, span, &physical))
        return false;

    if (!plan_mapping(physical, span, page_size, &plan))
        return false;

    mapping = mmap(NULL, plan.mapping_size, PROT_READ, MAP_SHARED,
        memory_fd, plan.file_offset);

    if (mapping == MAP_FAILED)
        return false;

    region->mapping = mapping;
    region->mapping_size = plan.mapping_size;
    region->regs = (volatile uint8_t *)mapping + plan.register_offset;
    return true;
}

uint32_t mmio_read32(const MmioRegion *region, size_t offset)
{
    volatile const uint32_t *address;

    address = (volatile const uint32_t *)(region->regs + offset);
    return *address;
}

void mmio_unmap(MmioRegion *region)
{
    if (!region)
        return;

    if (region->mapping)
        munmap(region->mapping, region->mapping_size);

    memset(region, 0, sizeof(*region));
}
