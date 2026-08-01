#define _GNU_SOURCE

#include "monitor.h"
#include "sensor.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pci/pci.h>

#define MAX_GPUS 64u
#define DEVMEM_PATH "/dev/mem"

#define NVIDIA_PCI_VENDOR_ID 0x10DEu
#define PCI_DEVICE_ID_SHIFT 16u

#ifndef NVML_DEVICE_NAME_BUFFER_SIZE
#define NVML_DEVICE_NAME_BUFFER_SIZE 64
#endif

#ifndef NVML_DEVICE_UUID_BUFFER_SIZE
#define NVML_DEVICE_UUID_BUFFER_SIZE 80
#endif

typedef struct {
    nvmlDevice_t nvml;
    nvmlPciInfo_t pci_info;
    char short_name[NVML_DEVICE_NAME_BUFFER_SIZE];
    char uuid[NVML_DEVICE_UUID_BUFFER_SIZE];
    GpuSensorLayout sensors;
    GpuReading reading;
    unsigned int index;
    bool present;
    bool selected;
    bool has_pci;
} GpuDevice;

struct Monitor {
    GpuDevice *gpus;
    unsigned int gpu_count;
    unsigned int selected_count;
    bool nvml_initialized;
};

static bool starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static void copy_string(char *destination, size_t size,
    const char *source)
{
    if (!size)
        return;

    snprintf(destination, size, "%s", source ? source : "");
}

static void replace_suffix(char *text, size_t size,
    const char *suffix, const char *replacement)
{
    size_t length = strlen(text);
    size_t suffix_length = strlen(suffix);
    size_t replacement_length = strlen(replacement);
    size_t prefix_length;

    if (length < suffix_length)
        return;

    if (strcmp(text + length - suffix_length, suffix) != 0)
        return;

    prefix_length = length - suffix_length;

    if (prefix_length >= size ||
        replacement_length >= size - prefix_length) {
        return;
    }

    snprintf(text + prefix_length, size - prefix_length,
        "%s", replacement);
}

static void make_short_name(const char *name, char *output, size_t size)
{
    const char *start = name;

    if (starts_with(start, "NVIDIA "))
        start += strlen("NVIDIA ");

    if (starts_with(start, "GeForce "))
        start += strlen("GeForce ");

    copy_string(output, size, start);
    replace_suffix(output, size, " SUPER", " S");
}

static bool parse_gpu_index(const char *text, unsigned int *output)
{
    char *end;
    unsigned long value;

    if (!text || !*text)
        return false;

    errno = 0;
    value = strtoul(text, &end, 10);

    if (errno || end == text || *end || value > UINT_MAX)
        return false;

    *output = (unsigned int)value;
    return true;
}

static bool parse_pci_bdf(const char *text, unsigned int *domain,
    unsigned int *bus, unsigned int *device, unsigned int *function)
{
    unsigned int parsed_domain;
    unsigned int parsed_bus;
    unsigned int parsed_device;
    unsigned int parsed_function;
    char tail;

    if (sscanf(text, "%x:%x:%x.%x%c", &parsed_domain, &parsed_bus,
            &parsed_device, &parsed_function, &tail) == 4) {
        if (parsed_bus <= 0xFFu && parsed_device <= 0x1Fu &&
            parsed_function <= 7u) {
            *domain = parsed_domain;
            *bus = parsed_bus;
            *device = parsed_device;
            *function = parsed_function;
            return true;
        }
    }

    parsed_domain = 0;

    if (sscanf(text, "%x:%x.%x%c", &parsed_bus, &parsed_device,
            &parsed_function, &tail) == 3) {
        if (parsed_bus <= 0xFFu && parsed_device <= 0x1Fu &&
            parsed_function <= 7u) {
            *domain = parsed_domain;
            *bus = parsed_bus;
            *device = parsed_device;
            *function = parsed_function;
            return true;
        }
    }

    return false;
}

static bool selector_matches_gpu(const char *selector,
    const GpuDevice *gpu)
{
    unsigned int parsed_index;
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int function;

    if (!gpu->present)
        return false;

    if (parse_gpu_index(selector, &parsed_index))
        return parsed_index == gpu->index;

    if (gpu->uuid[0] && strcmp(selector, gpu->uuid) == 0)
        return true;

    if (!parse_pci_bdf(selector, &domain, &bus, &device, &function))
        return false;

    if (!gpu->has_pci)
        return false;

    return domain == gpu->pci_info.domain &&
        bus == gpu->pci_info.bus &&
        device == gpu->pci_info.device &&
        function == 0;
}

static int apply_selector(Monitor *monitor, const char *selector,
    bool require_match)
{
    bool matched = false;

    for (unsigned int i = 0; i < monitor->gpu_count; i++) {
        GpuDevice *gpu = &monitor->gpus[i];

        if (!selector_matches_gpu(selector, gpu))
            continue;

        if (!gpu->selected) {
            gpu->selected = true;
            monitor->selected_count++;
        }

        matched = true;
    }

    if (require_match && !matched) {
        fprintf(stderr, "Invalid device selector: %s\n", selector);
        return -1;
    }

    return 0;
}

static void select_all_present(Monitor *monitor)
{
    monitor->selected_count = 0;

    for (unsigned int i = 0; i < monitor->gpu_count; i++) {
        GpuDevice *gpu = &monitor->gpus[i];

        gpu->selected = gpu->present;

        if (gpu->selected)
            monitor->selected_count++;
    }
}

static int apply_selector_list(Monitor *monitor, const char *selectors,
    bool require_match)
{
    char *copy;
    char *token;
    char *saveptr = NULL;

    if (!selectors || !*selectors) {
        if (require_match) {
            fprintf(stderr, "Invalid device selector: \n");
            return -1;
        }

        select_all_present(monitor);
        return 0;
    }

    if (strcmp(selectors, "all") == 0) {
        select_all_present(monitor);
        return 0;
    }

    if (strcmp(selectors, "none") == 0 ||
        strcmp(selectors, "void") == 0) {
        monitor->selected_count = 0;
        return 0;
    }

    copy = strdup(selectors);

    if (!copy) {
        perror("strdup");
        return -1;
    }

    for (token = strtok_r(copy, ",", &saveptr);
        token;
        token = strtok_r(NULL, ",", &saveptr)) {
        char *end;

        while (isspace((unsigned char)*token))
            token++;

        end = token + strlen(token);

        while (end > token && isspace((unsigned char)end[-1]))
            *--end = '\0';

        if (*token &&
            apply_selector(monitor, token, require_match) < 0) {
            free(copy);
            return -1;
        }
    }

    free(copy);
    return 0;
}

static int apply_selection(Monitor *monitor,
    const MonitorOptions *options)
{
    switch (options->selection_mode) {
    case MONITOR_SELECT_DEVICE:
        return apply_selector_list(monitor, options->selectors, true);

    case MONITOR_SELECT_VISIBLE:
        return apply_selector_list(monitor, options->selectors, false);

    case MONITOR_SELECT_ALL:
    default:
        select_all_present(monitor);
        return 0;
    }
}

static void discover_gpu(GpuDevice *gpu, unsigned int index)
{
    char name[NVML_DEVICE_NAME_BUFFER_SIZE];
    nvmlReturn_t result;

    gpu->index = index;
    gpu->reading.index = index;
    gpu->reading.short_name = gpu->short_name;
    sensor_init(&gpu->sensors);

    result = nvmlDeviceGetHandleByIndex(index, &gpu->nvml);

    if (result != NVML_SUCCESS)
        return;

    gpu->present = true;

    result = nvmlDeviceGetName(gpu->nvml, name, sizeof(name));

    if (result != NVML_SUCCESS)
        snprintf(name, sizeof(name), "GPU %u", index);

    result = nvmlDeviceGetUUID(gpu->nvml, gpu->uuid,
        sizeof(gpu->uuid));

    if (result != NVML_SUCCESS)
        gpu->uuid[0] = '\0';

    result = nvmlDeviceGetPciInfo(gpu->nvml, &gpu->pci_info);

    if (result == NVML_SUCCESS)
        gpu->has_pci = true;

    make_short_name(name, gpu->short_name, sizeof(gpu->short_name));
}

static int discover_gpus(Monitor *monitor)
{
    unsigned int gpu_count = 0;

    if (nvmlDeviceGetCount(&gpu_count) != NVML_SUCCESS ||
        !gpu_count) {
        fprintf(stderr, "No NVIDIA GPUs found.\n");
        return -1;
    }

    if (gpu_count > MAX_GPUS)
        gpu_count = MAX_GPUS;

    monitor->gpu_count = gpu_count;
    monitor->gpus = calloc(gpu_count, sizeof(*monitor->gpus));

    if (!monitor->gpus) {
        perror("calloc");
        return -1;
    }

    for (unsigned int i = 0; i < gpu_count; i++)
        discover_gpu(&monitor->gpus[i], i);

    return 0;
}

static struct pci_dev *find_pci_device(struct pci_access *pci_access,
    const nvmlPciInfo_t *pci_info)
{
    uint32_t target_id = pci_info->pciDeviceId;

    for (struct pci_dev *candidate = pci_access->devices;
        candidate;
        candidate = candidate->next) {
        uint32_t candidate_id;

        pci_fill_info(candidate,
            PCI_FILL_IDENT | PCI_FILL_BASES | PCI_FILL_SIZES);

        if (candidate->vendor_id != NVIDIA_PCI_VENDOR_ID)
            continue;

        candidate_id =
            ((uint32_t)candidate->device_id << PCI_DEVICE_ID_SHIFT) |
            candidate->vendor_id;

        if (candidate_id == target_id &&
            (unsigned int)candidate->domain == pci_info->domain &&
            candidate->bus == pci_info->bus &&
            candidate->dev == pci_info->device) {
            return candidate;
        }
    }

    return NULL;
}

static void warn_unavailable_sensors(const GpuDevice *gpu)
{
    if (!sensor_junction_mapped(&gpu->sensors)) {
        fprintf(stderr,
            "gputemps: GPU %u: junction sensor unavailable\n",
            gpu->index);
    }

    if (!sensor_vram_mapped(&gpu->sensors)) {
        fprintf(stderr,
            "gputemps: GPU %u: VRAM sensor unavailable\n",
            gpu->index);
    }
}

static void setup_selected_gpus(Monitor *monitor,
    struct pci_access *pci_access, int memory_fd, long page_size)
{
    for (unsigned int i = 0; i < monitor->gpu_count; i++) {
        GpuDevice *gpu = &monitor->gpus[i];
        struct pci_dev *pci_device = NULL;

        if (!gpu->selected)
            continue;

        if (gpu->has_pci)
            pci_device = find_pci_device(pci_access, &gpu->pci_info);

        if (pci_device) {
            sensor_setup(&gpu->sensors, pci_device, memory_fd,
                page_size);
        }

        warn_unavailable_sensors(gpu);
    }
}

static void sample_gpu(GpuDevice *gpu)
{
    GpuReading reading = {
        .index = gpu->index,
        .short_name = gpu->short_name
    };

    sensor_sample(&gpu->sensors, gpu->nvml, &reading);
    gpu->reading = reading;
}

int monitor_init(Monitor **output, const MonitorOptions *options)
{
    Monitor *monitor = NULL;
    struct pci_access *pci_access = NULL;
    int memory_fd = -1;
    long page_size;
    nvmlReturn_t result;

    if (!output || !options)
        return -1;

    *output = NULL;

    if (geteuid() != 0) {
        fprintf(stderr, "This program requires root privileges.\n");
        return -1;
    }

    page_size = sysconf(_SC_PAGE_SIZE);

    if (page_size <= 0)
        return -1;

    monitor = calloc(1, sizeof(*monitor));

    if (!monitor) {
        perror("calloc");
        return -1;
    }

    memory_fd = open(DEVMEM_PATH, O_RDONLY | O_SYNC);

    if (memory_fd < 0) {
        perror("open " DEVMEM_PATH);
        goto fail;
    }

    pci_access = pci_alloc();

    if (!pci_access) {
        fprintf(stderr, "pci_alloc failed\n");
        goto fail;
    }

    pci_init(pci_access);
    pci_scan_bus(pci_access);

    result = nvmlInit();

    if (result != NVML_SUCCESS) {
        fprintf(stderr, "nvmlInit failed: %s\n",
            nvmlErrorString(result));
        goto fail;
    }

    monitor->nvml_initialized = true;

    if (discover_gpus(monitor) < 0)
        goto fail;

    if (apply_selection(monitor, options) < 0)
        goto fail;

    if (!monitor->selected_count) {
        fprintf(stderr, "No GPUs selected.\n");
        goto fail;
    }

    setup_selected_gpus(monitor, pci_access, memory_fd, page_size);

    pci_cleanup(pci_access);
    close(memory_fd);

    *output = monitor;
    return 0;

fail:
    if (pci_access)
        pci_cleanup(pci_access);

    if (memory_fd >= 0)
        close(memory_fd);

    monitor_destroy(monitor);
    return -1;
}

void monitor_sample(Monitor *monitor)
{
    if (!monitor)
        return;

    for (unsigned int i = 0; i < monitor->gpu_count; i++) {
        GpuDevice *gpu = &monitor->gpus[i];

        if (gpu->selected)
            sample_gpu(gpu);
    }
}

unsigned int monitor_gpu_count(const Monitor *monitor)
{
    return monitor ? monitor->selected_count : 0;
}

const GpuReading *monitor_gpu(const Monitor *monitor,
    unsigned int position)
{
    unsigned int selected_position = 0;

    if (!monitor || position >= monitor->selected_count)
        return NULL;

    for (unsigned int i = 0; i < monitor->gpu_count; i++) {
        const GpuDevice *gpu = &monitor->gpus[i];

        if (!gpu->selected)
            continue;

        if (selected_position == position)
            return &gpu->reading;

        selected_position++;
    }

    return NULL;
}

void monitor_destroy(Monitor *monitor)
{
    if (!monitor)
        return;

    if (monitor->gpus) {
        for (unsigned int i = 0; i < monitor->gpu_count; i++)
            sensor_cleanup(&monitor->gpus[i].sensors);

        free(monitor->gpus);
    }

    if (monitor->nvml_initialized)
        nvmlShutdown();

    free(monitor);
}
