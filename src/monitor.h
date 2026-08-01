#ifndef GPUTEMPS_MONITOR_H
#define GPUTEMPS_MONITOR_H

#include <stdbool.h>

#define GPU_MAX_JUNCTION_CHANNELS 4u
#define GPU_MAX_VRAM_SOURCES 64u

typedef struct {
    int millidegrees;
    bool valid;
} Temperature;

typedef struct {
    Temperature hotspot;
    Temperature hardware_max;
    Temperature hardware_average;
    Temperature channels[GPU_MAX_JUNCTION_CHANNELS];
    unsigned int channel_count;
} JunctionReading;

typedef enum {
    GPU_VRAM_SOURCE_LAYOUT_NONE,
    GPU_VRAM_SOURCE_LAYOUT_STANDARD,
    GPU_VRAM_SOURCE_LAYOUT_CLAMSHELL,
    GPU_VRAM_SOURCE_LAYOUT_UNGROUPED
} GpuVramSourceLayout;

typedef struct {
    Temperature hottest;
    Temperature sources[GPU_MAX_VRAM_SOURCES];
    unsigned int source_slot_count;
    GpuVramSourceLayout source_layout;
} VramReading;

typedef struct {
    unsigned int index;
    const char *short_name;
    Temperature core;
    JunctionReading junction;
    VramReading vram;
} GpuReading;

typedef enum {
    MONITOR_SELECT_ALL,
    MONITOR_SELECT_DEVICE,
    MONITOR_SELECT_VISIBLE
} MonitorSelectionMode;

typedef struct {
    MonitorSelectionMode selection_mode;
    const char *selectors;
} MonitorOptions;

typedef struct Monitor Monitor;

int monitor_init(Monitor **output, const MonitorOptions *options);
void monitor_sample(Monitor *monitor);
unsigned int monitor_gpu_count(const Monitor *monitor);
const GpuReading *monitor_gpu(const Monitor *monitor,
    unsigned int position);
void monitor_destroy(Monitor *monitor);

#endif
