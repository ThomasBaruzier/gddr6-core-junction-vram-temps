#ifndef GPU_TEMP_SENSORS_H
#define GPU_TEMP_SENSORS_H

#include <stdbool.h>

typedef struct {
    int value;
    bool valid;
} Temperature;

typedef struct {
    unsigned int index;
    const char *short_name;
    Temperature core;
    Temperature junction;
    Temperature vram;
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

int monitor_init(Monitor **out, const MonitorOptions *options);
void monitor_sample(Monitor *monitor);
unsigned int monitor_gpu_count(const Monitor *monitor);
const GpuReading *monitor_gpu(const Monitor *monitor, unsigned int position);
void monitor_destroy(Monitor *monitor);

#endif
