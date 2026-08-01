#ifndef GPUTEMPS_SENSOR_H
#define GPUTEMPS_SENSOR_H

#include "mmio.h"
#include "monitor.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <nvml.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

struct pci_dev;
typedef struct SensorProfile SensorProfile;

typedef enum {
    GDDR7_TOPOLOGY_UNKNOWN,
    GDDR7_TOPOLOGY_STANDARD,
    GDDR7_TOPOLOGY_CLAMSHELL
} Gddr7Topology;

typedef struct {
    const SensorProfile *profile;
    MmioRegion junction_mmio;
    MmioRegion vram_mmio;
    Gddr7Topology gddr7_topology;
} GpuSensorLayout;

void sensor_init(GpuSensorLayout *sensors);
void sensor_setup(GpuSensorLayout *sensors,
    const struct pci_dev *pci_device, int memory_fd, long page_size);
void sensor_sample(const GpuSensorLayout *sensors, nvmlDevice_t nvml,
    GpuReading *reading);
bool sensor_junction_mapped(const GpuSensorLayout *sensors);
bool sensor_vram_mapped(const GpuSensorLayout *sensors);
void sensor_cleanup(GpuSensorLayout *sensors);

#endif
