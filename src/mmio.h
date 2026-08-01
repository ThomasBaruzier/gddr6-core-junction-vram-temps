#ifndef GPUTEMPS_MMIO_H
#define GPUTEMPS_MMIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pci_dev;

typedef struct {
    void *mapping;
    size_t mapping_size;
    volatile uint8_t *regs;
} MmioRegion;

bool mmio_map_bar0(MmioRegion *region, int memory_fd,
    const struct pci_dev *pci_device, uint32_t offset,
    size_t span, long page_size);
uint32_t mmio_read32(const MmioRegion *region, size_t offset);
void mmio_unmap(MmioRegion *region);

#endif
