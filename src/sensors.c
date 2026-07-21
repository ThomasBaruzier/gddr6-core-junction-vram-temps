#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include "sensors.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <nvml.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <pci/pci.h>

#define MAX_GPUS 64u
#define MEM_PATH "/dev/mem"

#define NVIDIA_VENDOR_ID 0x10DEu
#define PCI_DEVICE_ID_SHIFT 16u
#define PCI_BAR_IO_SPACE 0x1u

#define PCI_DEVICE_ID_RTX_5090 0x2B85u
#define PCI_DEVICE_ID_RTX_5070_TI 0x2C05u

#define JUNCTION_THERM_BYTE_OFFSET 0x0002046Cu
#define VRAM_GDDR6_ADC_OFFSET 0x0000E2A8u

#define JUNCTION_BYTE_SHIFT 8u
#define JUNCTION_BYTE_MASK 0xFFu
#define VRAM_ADC_MASK 0xFFFu
#define VRAM_ADC_DIVISOR 32u
#define TEMP_VALID_LIMIT 0x7Fu

#define BLACKWELL_BJT_BASE 0x00AD0A90u
#define BLACKWELL_BJT_COUNT 6u
#define BLACKWELL_BJT_STRIDE 0x4u
#define BLACKWELL_BJT_VALID_BIT 0x40000000u
#define BLACKWELL_BJT_VALUE_MASK 0xFFFFu
#define BLACKWELL_BJT_SPAN \
    ((BLACKWELL_BJT_COUNT - 1u) * BLACKWELL_BJT_STRIDE + \
     sizeof(uint32_t))

#define GDDR7_DQR_BASE 0x009024C0u
#define GDDR7_DQR_VALID_OFFSET 0x10u
#define GDDR7_DQR_STRIDE 0x4000u
#define GDDR7_DQR_MAX_SLOTS 16u
#define GDDR7_DQR_SPAN \
    ((GDDR7_DQR_MAX_SLOTS - 1u) * GDDR7_DQR_STRIDE + \
     GDDR7_DQR_VALID_OFFSET + sizeof(uint32_t))

#ifndef NVML_DEVICE_NAME_BUFFER_SIZE
#define NVML_DEVICE_NAME_BUFFER_SIZE 64
#endif

#ifndef NVML_DEVICE_UUID_BUFFER_SIZE
#define NVML_DEVICE_UUID_BUFFER_SIZE 80
#endif

_Static_assert(
    BLACKWELL_BJT_SPAN == 0x18u,
    "unexpected Blackwell BJT span"
);

_Static_assert(
    GDDR7_DQR_SPAN == 0x0003C014u,
    "unexpected GDDR7 DQR span"
);

typedef enum {
    JUNCTION_THERM_BYTE,
    JUNCTION_BLACKWELL_BJT
} JunctionMethod;

typedef enum {
    VRAM_NONE,
    VRAM_GDDR6_ADC,
    VRAM_GDDR7_DQR
} VramMethod;

typedef struct {
    void *mapping;
    size_t mapping_len;
    volatile uint8_t *regs;
} MmioRegion;

typedef struct {
    uint16_t device_id;
    JunctionMethod junction_method;
    VramMethod vram_method;
} DeviceProfile;

typedef struct {
    nvmlDevice_t nvml;
    nvmlPciInfo_t pci_info;
    char short_name[NVML_DEVICE_NAME_BUFFER_SIZE];
    char uuid[NVML_DEVICE_UUID_BUFFER_SIZE];
    GpuReading reading;
    MmioRegion junction_mmio;
    MmioRegion vram_mmio;
    JunctionMethod junction_method;
    VramMethod vram_method;
    bool present;
    bool selected;
    bool has_pci;
} GpuDevice;

struct Monitor {
    GpuDevice *devices;
    unsigned int device_count;
    unsigned int selected_count;
    bool nvml_initialized;
};

static const DeviceProfile device_profiles[] = {
    {
        .device_id = PCI_DEVICE_ID_RTX_5090,
        .junction_method = JUNCTION_BLACKWELL_BJT,
        .vram_method = VRAM_GDDR7_DQR
    },
    {
        .device_id = PCI_DEVICE_ID_RTX_5070_TI,
        .junction_method = JUNCTION_BLACKWELL_BJT,
        .vram_method = VRAM_NONE
    }
};

static bool starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static void copy_string(char *destination, size_t size, const char *source)
{
    if (size == 0)
        return;

    snprintf(destination, size, "%s", source ? source : "");
}

static void replace_suffix(
    char *text,
    size_t size,
    const char *suffix,
    const char *replacement
)
{
    size_t length = strlen(text);
    size_t suffix_length = strlen(suffix);
    size_t replacement_length = strlen(replacement);

    if (length < suffix_length)
        return;

    if (strcmp(text + length - suffix_length, suffix) != 0)
        return;

    if (length - suffix_length + replacement_length >= size)
        return;

    snprintf(
        text + length - suffix_length,
        size - (length - suffix_length),
        "%s",
        replacement
    );
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

static const DeviceProfile *find_device_profile(uint16_t device_id)
{
    size_t count = sizeof(device_profiles) / sizeof(device_profiles[0]);

    for (size_t i = 0; i < count; i++) {
        if (device_profiles[i].device_id == device_id)
            return &device_profiles[i];
    }

    return NULL;
}

static void unmap_mmio(MmioRegion *region)
{
    if (!region)
        return;

    if (region->mapping)
        munmap(region->mapping, region->mapping_len);

    memset(region, 0, sizeof(*region));
}

static bool map_mmio(
    int memory_fd,
    struct pci_dev *device,
    uint32_t offset,
    size_t span,
    long page_size,
    MmioRegion *output
)
{
    pciaddr_t bar;
    pciaddr_t bar_size;
    pciaddr_t physical;
    pciaddr_t mapping_base;
    pciaddr_t page;
    pciaddr_t maximum_address;
    size_t page_offset;
    size_t required;
    size_t mapping_length;
    off_t file_offset;
    void *mapping;

    memset(output, 0, sizeof(*output));

    if (span == 0 || page_size <= 0)
        return false;

    if (device->base_addr[0] == 0 || device->size[0] == 0)
        return false;

    if (device->base_addr[0] & PCI_BAR_IO_SPACE)
        return false;

    bar_size = device->size[0];

    if ((pciaddr_t)offset > bar_size)
        return false;

    if ((uintmax_t)span >
        (uintmax_t)(bar_size - (pciaddr_t)offset)) {
        return false;
    }

    bar = device->base_addr[0] & ~(pciaddr_t)0xFu;
    maximum_address = ~(pciaddr_t)0;

    if ((pciaddr_t)offset > maximum_address - bar)
        return false;

    physical = bar + (pciaddr_t)offset;
    page = (pciaddr_t)page_size;
    mapping_base = physical - physical % page;
    page_offset = (size_t)(physical - mapping_base);

    if (span > SIZE_MAX - page_offset)
        return false;

    required = page_offset + span;

    if (required > SIZE_MAX - ((size_t)page_size - 1u))
        return false;

    mapping_length =
        (required + (size_t)page_size - 1u) /
        (size_t)page_size *
        (size_t)page_size;

    file_offset = (off_t)mapping_base;

    if (file_offset < 0 || (pciaddr_t)file_offset != mapping_base)
        return false;

    mapping = mmap(
        NULL,
        mapping_length,
        PROT_READ,
        MAP_SHARED,
        memory_fd,
        file_offset
    );

    if (mapping == MAP_FAILED)
        return false;

    output->mapping = mapping;
    output->mapping_len = mapping_length;
    output->regs = (volatile uint8_t *)mapping + page_offset;

    return true;
}

static uint32_t read_mmio32(const MmioRegion *region, size_t offset)
{
    volatile const uint32_t *address =
        (volatile const uint32_t *)(region->regs + offset);

    return *address;
}

static Temperature invalid_temperature(void)
{
    return (Temperature){0};
}

static Temperature sample_therm_byte(const MmioRegion *region)
{
    uint32_t raw;
    uint32_t value;

    if (!region->regs)
        return invalid_temperature();

    raw = read_mmio32(region, 0);

    if (raw == UINT32_MAX)
        return invalid_temperature();

    value =
        (raw >> JUNCTION_BYTE_SHIFT) &
        JUNCTION_BYTE_MASK;

    if (value >= TEMP_VALID_LIMIT)
        return invalid_temperature();

    return (Temperature){
        .value = (int)value,
        .valid = true
    };
}

static Temperature sample_blackwell_bjt(const MmioRegion *region)
{
    uint32_t hottest = 0;
    bool found = false;

    if (!region->regs)
        return invalid_temperature();

    for (unsigned int channel = 0;
         channel < BLACKWELL_BJT_COUNT;
         channel++) {
        uint32_t raw = read_mmio32(
            region,
            (size_t)channel * BLACKWELL_BJT_STRIDE
        );
        uint32_t value;

        if (raw == UINT32_MAX || !(raw & BLACKWELL_BJT_VALID_BIT))
            continue;

        value = raw & BLACKWELL_BJT_VALUE_MASK;

        if (!found || value > hottest) {
            hottest = value;
            found = true;
        }
    }

    if (!found)
        return invalid_temperature();

    return (Temperature){
        .value = (int)(hottest >> 8),
        .valid = true
    };
}

static Temperature sample_gddr6_adc(const MmioRegion *region)
{
    uint32_t raw;
    uint32_t value;

    if (!region->regs)
        return invalid_temperature();

    raw = read_mmio32(region, 0);

    if (raw == UINT32_MAX)
        return invalid_temperature();

    value = (raw & VRAM_ADC_MASK) / VRAM_ADC_DIVISOR;

    if (value >= TEMP_VALID_LIMIT)
        return invalid_temperature();

    return (Temperature){
        .value = (int)value,
        .valid = true
    };
}

static Temperature decode_gddr7(uint32_t validity, uint32_t data)
{
    unsigned int code;

    if (validity == UINT32_MAX || data == UINT32_MAX)
        return invalid_temperature();

    if (((validity >> 24) & 0x0Fu) != 0x0Fu)
        return invalid_temperature();

    if ((data & 0xFFFF0000u) == 0xBADF0000u)
        return invalid_temperature();

    code = (data >> 16) & 0xFFu;

    if (code > 80u)
        return invalid_temperature();

    return (Temperature){
        .value = (int)code * 2 - 40,
        .valid = true
    };
}

static Temperature sample_gddr7(const MmioRegion *region)
{
    Temperature hottest = {0};

    if (!region->regs)
        return hottest;

    for (unsigned int slot = 0;
         slot < GDDR7_DQR_MAX_SLOTS;
         slot++) {
        size_t offset = (size_t)slot * GDDR7_DQR_STRIDE;
        uint32_t validity = read_mmio32(
            region,
            offset + GDDR7_DQR_VALID_OFFSET
        );
        uint32_t data = read_mmio32(region, offset);
        Temperature current = decode_gddr7(validity, data);

        if (!current.valid)
            continue;

        if (!hottest.valid || current.value > hottest.value)
            hottest = current;
    }

    return hottest;
}

static void sample_core(GpuDevice *gpu)
{
    unsigned int value;
    nvmlReturn_t result;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    result = nvmlDeviceGetTemperature(
        gpu->nvml,
        NVML_TEMPERATURE_GPU,
        &value
    );
#pragma GCC diagnostic pop

    gpu->reading.core.valid = result == NVML_SUCCESS;
    gpu->reading.core.value =
        gpu->reading.core.valid ? (int)value : 0;
}

static void sample_gpu(GpuDevice *gpu)
{
    sample_core(gpu);

    switch (gpu->junction_method) {
    case JUNCTION_THERM_BYTE:
        gpu->reading.junction =
            sample_therm_byte(&gpu->junction_mmio);
        break;

    case JUNCTION_BLACKWELL_BJT:
        gpu->reading.junction =
            sample_blackwell_bjt(&gpu->junction_mmio);
        break;

    default:
        gpu->reading.junction = invalid_temperature();
        break;
    }

    switch (gpu->vram_method) {
    case VRAM_GDDR6_ADC:
        gpu->reading.vram =
            sample_gddr6_adc(&gpu->vram_mmio);
        break;

    case VRAM_GDDR7_DQR:
        gpu->reading.vram = sample_gddr7(&gpu->vram_mmio);
        break;

    case VRAM_NONE:
    default:
        gpu->reading.vram = invalid_temperature();
        break;
    }
}

static bool parse_unsigned(const char *text, unsigned int *output)
{
    char *end;
    unsigned long value;

    if (!text || *text == '\0')
        return false;

    errno = 0;
    value = strtoul(text, &end, 10);

    if (errno ||
        end == text ||
        *end != '\0' ||
        value > UINT_MAX) {
        return false;
    }

    *output = (unsigned int)value;
    return true;
}

static bool parse_bdf(
    const char *text,
    unsigned int *domain,
    unsigned int *bus,
    unsigned int *device,
    unsigned int *function
)
{
    unsigned int parsed_domain;
    unsigned int parsed_bus;
    unsigned int parsed_device;
    unsigned int parsed_function;
    char tail;

    if (sscanf(
            text,
            "%x:%x:%x.%x%c",
            &parsed_domain,
            &parsed_bus,
            &parsed_device,
            &parsed_function,
            &tail
        ) == 4) {
        if (parsed_bus <= 0xFFu &&
            parsed_device <= 0x1Fu &&
            parsed_function <= 7u) {
            *domain = parsed_domain;
            *bus = parsed_bus;
            *device = parsed_device;
            *function = parsed_function;
            return true;
        }
    }

    parsed_domain = 0;

    if (sscanf(
            text,
            "%x:%x.%x%c",
            &parsed_bus,
            &parsed_device,
            &parsed_function,
            &tail
        ) == 3) {
        if (parsed_bus <= 0xFFu &&
            parsed_device <= 0x1Fu &&
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

static bool selector_matches_gpu(
    const char *selector,
    unsigned int index,
    const GpuDevice *gpu
)
{
    unsigned int parsed_index;
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int function;

    if (!gpu->present)
        return false;

    if (parse_unsigned(selector, &parsed_index))
        return parsed_index == index;

    if (gpu->uuid[0] != '\0' &&
        strcmp(selector, gpu->uuid) == 0) {
        return true;
    }

    if (parse_bdf(
            selector,
            &domain,
            &bus,
            &device,
            &function
        )) {
        return gpu->has_pci &&
            domain == gpu->pci_info.domain &&
            bus == gpu->pci_info.bus &&
            device == gpu->pci_info.device &&
            function == 0;
    }

    return false;
}

static int apply_selector(
    Monitor *monitor,
    const char *selector,
    bool required
)
{
    bool matched = false;

    for (unsigned int i = 0; i < monitor->device_count; i++) {
        GpuDevice *gpu = &monitor->devices[i];

        if (!selector_matches_gpu(selector, i, gpu))
            continue;

        if (!gpu->selected) {
            gpu->selected = true;
            monitor->selected_count++;
        }

        matched = true;
    }

    if (required && !matched) {
        fprintf(stderr, "Invalid device selector: %s\n", selector);
        return -1;
    }

    return 0;
}

static void select_all_present(Monitor *monitor)
{
    monitor->selected_count = 0;

    for (unsigned int i = 0; i < monitor->device_count; i++) {
        GpuDevice *gpu = &monitor->devices[i];

        gpu->selected = gpu->present;

        if (gpu->selected)
            monitor->selected_count++;
    }
}

static int apply_selector_list(
    Monitor *monitor,
    const char *selectors,
    bool required
)
{
    char *copy;
    char *token;
    char *save = NULL;

    if (!selectors || *selectors == '\0') {
        if (required) {
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

    for (token = strtok_r(copy, ",", &save);
         token;
         token = strtok_r(NULL, ",", &save)) {
        char *end;

        while (isspace((unsigned char)*token))
            token++;

        end = token + strlen(token);

        while (end > token && isspace((unsigned char)end[-1]))
            *--end = '\0';

        if (*token != '\0' &&
            apply_selector(monitor, token, required) < 0) {
            free(copy);
            return -1;
        }
    }

    free(copy);
    return 0;
}

static int apply_selection(
    Monitor *monitor,
    const MonitorOptions *options
)
{
    switch (options->selection_mode) {
    case MONITOR_SELECT_DEVICE:
        return apply_selector_list(
            monitor,
            options->selectors,
            true
        );

    case MONITOR_SELECT_VISIBLE:
        return apply_selector_list(
            monitor,
            options->selectors,
            false
        );

    case MONITOR_SELECT_ALL:
    default:
        select_all_present(monitor);
        return 0;
    }
}

static struct pci_dev *match_pci_device(
    struct pci_access *access,
    const nvmlPciInfo_t *pci_info
)
{
    uint32_t target = pci_info->pciDeviceId;

    for (struct pci_dev *device = access->devices;
         device;
         device = device->next) {
        uint32_t identifier;

        pci_fill_info(
            device,
            PCI_FILL_IDENT |
            PCI_FILL_BASES |
            PCI_FILL_SIZES
        );

        if (device->vendor_id != NVIDIA_VENDOR_ID)
            continue;

        identifier =
            ((uint32_t)device->device_id << PCI_DEVICE_ID_SHIFT) |
            device->vendor_id;

        if (identifier == target &&
            (unsigned int)device->domain == pci_info->domain &&
            device->bus == pci_info->bus &&
            device->dev == pci_info->device) {
            return device;
        }
    }

    return NULL;
}

static void setup_junction_mmio(
    GpuDevice *gpu,
    struct pci_dev *device,
    int memory_fd,
    long page_size
)
{
    uint32_t offset;
    size_t span;

    switch (gpu->junction_method) {
    case JUNCTION_THERM_BYTE:
        offset = JUNCTION_THERM_BYTE_OFFSET;
        span = sizeof(uint32_t);
        break;

    case JUNCTION_BLACKWELL_BJT:
        offset = BLACKWELL_BJT_BASE;
        span = BLACKWELL_BJT_SPAN;
        break;

    default:
        return;
    }

    map_mmio(
        memory_fd,
        device,
        offset,
        span,
        page_size,
        &gpu->junction_mmio
    );
}

static void setup_vram_mmio(
    GpuDevice *gpu,
    struct pci_dev *device,
    int memory_fd,
    long page_size
)
{
    uint32_t offset;
    size_t span;

    switch (gpu->vram_method) {
    case VRAM_GDDR6_ADC:
        offset = VRAM_GDDR6_ADC_OFFSET;
        span = sizeof(uint32_t);
        break;

    case VRAM_GDDR7_DQR:
        offset = GDDR7_DQR_BASE;
        span = GDDR7_DQR_SPAN;
        break;

    case VRAM_NONE:
    default:
        return;
    }

    map_mmio(
        memory_fd,
        device,
        offset,
        span,
        page_size,
        &gpu->vram_mmio
    );
}

static void setup_gpu_mmio(
    GpuDevice *gpu,
    struct pci_access *access,
    int memory_fd,
    long page_size
)
{
    struct pci_dev *device;
    const DeviceProfile *profile;

    gpu->junction_method = JUNCTION_THERM_BYTE;
    gpu->vram_method = VRAM_GDDR6_ADC;

    if (!gpu->has_pci)
        return;

    device = match_pci_device(access, &gpu->pci_info);

    if (!device)
        return;

    profile = find_device_profile(device->device_id);

    if (profile) {
        gpu->junction_method = profile->junction_method;
        gpu->vram_method = profile->vram_method;
    }

    setup_junction_mmio(
        gpu,
        device,
        memory_fd,
        page_size
    );
    setup_vram_mmio(
        gpu,
        device,
        memory_fd,
        page_size
    );
}

static void log_sensor_warnings(
    unsigned int index,
    const GpuDevice *gpu
)
{
    if (!gpu->junction_mmio.regs) {
        fprintf(
            stderr,
            "gputemps: GPU %u: junction sensor unavailable\n",
            index
        );
    }

    if (gpu->vram_method != VRAM_NONE &&
        !gpu->vram_mmio.regs) {
        fprintf(
            stderr,
            "gputemps: GPU %u: VRAM sensor unavailable\n",
            index
        );
    }
}

int monitor_init(Monitor **output, const MonitorOptions *options)
{
    Monitor *monitor = NULL;
    struct pci_access *pci_access = NULL;
    int memory_fd = -1;
    long page_size;
    unsigned int count = 0;
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

    memory_fd = open(MEM_PATH, O_RDONLY | O_SYNC);

    if (memory_fd < 0) {
        perror("open " MEM_PATH);
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
        fprintf(
            stderr,
            "nvmlInit failed: %s\n",
            nvmlErrorString(result)
        );
        goto fail;
    }

    monitor->nvml_initialized = true;

    if (nvmlDeviceGetCount(&count) != NVML_SUCCESS || count == 0) {
        fprintf(stderr, "No NVIDIA GPUs found.\n");
        goto fail;
    }

    if (count > MAX_GPUS)
        count = MAX_GPUS;

    monitor->device_count = count;
    monitor->devices = calloc(
        monitor->device_count,
        sizeof(*monitor->devices)
    );

    if (!monitor->devices) {
        perror("calloc");
        goto fail;
    }

    for (unsigned int i = 0; i < monitor->device_count; i++) {
        GpuDevice *gpu = &monitor->devices[i];
        char name[NVML_DEVICE_NAME_BUFFER_SIZE];

        gpu->reading.index = i;
        gpu->reading.short_name = gpu->short_name;

        if (nvmlDeviceGetHandleByIndex(
                i,
                &gpu->nvml
            ) != NVML_SUCCESS) {
            continue;
        }

        gpu->present = true;

        if (nvmlDeviceGetName(
                gpu->nvml,
                name,
                sizeof(name)
            ) != NVML_SUCCESS) {
            snprintf(name, sizeof(name), "GPU %u", i);
        }

        if (nvmlDeviceGetUUID(
                gpu->nvml,
                gpu->uuid,
                sizeof(gpu->uuid)
            ) != NVML_SUCCESS) {
            gpu->uuid[0] = '\0';
        }

        if (nvmlDeviceGetPciInfo(
                gpu->nvml,
                &gpu->pci_info
            ) == NVML_SUCCESS) {
            gpu->has_pci = true;
        }

        make_short_name(name, gpu->short_name, sizeof(gpu->short_name));
    }

    if (apply_selection(monitor, options) < 0)
        goto fail;

    if (monitor->selected_count == 0) {
        fprintf(stderr, "No GPUs selected.\n");
        goto fail;
    }

    for (unsigned int i = 0; i < monitor->device_count; i++) {
        GpuDevice *gpu = &monitor->devices[i];

        if (!gpu->selected)
            continue;

        setup_gpu_mmio(
            gpu,
            pci_access,
            memory_fd,
            page_size
        );
        log_sensor_warnings(i, gpu);
    }

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

    for (unsigned int i = 0; i < monitor->device_count; i++) {
        GpuDevice *gpu = &monitor->devices[i];

        if (gpu->selected)
            sample_gpu(gpu);
    }
}

unsigned int monitor_gpu_count(const Monitor *monitor)
{
    return monitor ? monitor->selected_count : 0;
}

const GpuReading *monitor_gpu(
    const Monitor *monitor,
    unsigned int position
)
{
    unsigned int selected_position = 0;

    if (!monitor || position >= monitor->selected_count)
        return NULL;

    for (unsigned int i = 0; i < monitor->device_count; i++) {
        const GpuDevice *gpu = &monitor->devices[i];

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

    if (monitor->devices) {
        for (unsigned int i = 0; i < monitor->device_count; i++) {
            unmap_mmio(&monitor->devices[i].junction_mmio);
            unmap_mmio(&monitor->devices[i].vram_mmio);
        }

        free(monitor->devices);
    }

    if (monitor->nvml_initialized)
        nvmlShutdown();

    free(monitor);
}
