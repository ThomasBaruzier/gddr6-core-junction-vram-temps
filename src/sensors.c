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
#define PCI_DEVICE_ID_RTX_5080 0x2B82u
#define PCI_DEVICE_ID_RTX_5070_TI 0x2C05u
#define PCI_DEVICE_ID_RTX_5070 0x2C02u

#define JUNCTION_THERM_BYTE_OFFSET 0x0002046Cu
#define VRAM_GDDR6_ADC_OFFSET 0x0000E2A8u

#define JUNCTION_BYTE_SHIFT 8u
#define JUNCTION_BYTE_MASK 0xFFu
#define VRAM_ADC_MASK 0xFFFu
#define VRAM_ADC_DIVISOR 32u
#define GDDR6_TEMP_VALID_LIMIT 0x7Fu

#define BLACKWELL_THERM_BASE 0x00AD0A90u
#define BLACKWELL_THERM_CHANNEL_COUNT 4u
#define BLACKWELL_THERM_STRIDE 0x4u
#define BLACKWELL_THERM_HW_MAX_OFFSET 0x10u
#define BLACKWELL_THERM_HW_AVG_OFFSET 0x14u
#define BLACKWELL_THERM_SPAN \
    (BLACKWELL_THERM_HW_AVG_OFFSET + sizeof(uint32_t))
#define BLACKWELL_THERM_VALUE_MASK 0xFFFFu
#define BLACKWELL_THERM_SCALE 256u
#define BLACKWELL_THERM_MAX_C 150u

#define GDDR7_DQR_BASE 0x009024C0u
#define GDDR7_DQR_VALID_OFFSET 0x10u
#define GDDR7_DQR_STRIDE 0x4000u
#define GDDR7_DQR_SUBREGISTER_COUNT 4u
#define GDDR7_DQR_VALID_SHIFT 24u
#define GDDR7_MAX_FBPA_COUNT 16u
#define GDDR7_STANDARD_FBPA_COUNT 16u
#define GDDR7_CLAMSHELL_FBPA_COUNT 8u
#define GDDR7_DQR_SPAN \
    ((GDDR7_MAX_FBPA_COUNT - 1u) * GDDR7_DQR_STRIDE + \
     GDDR7_DQR_VALID_OFFSET + sizeof(uint32_t))
#define GDDR7_STRAP_BROADCAST 0x009A0200u
#define GDDR7_STRAP_X16_BIT 22u
#define GDDR7_CODE_MIN 21u
#define GDDR7_CODE_MAX 80u
#define GDDR7_POISON_MASK 0xFFFF0000u
#define GDDR7_POISON_VALUE 0xBADF0000u

#ifndef NVML_DEVICE_NAME_BUFFER_SIZE
#define NVML_DEVICE_NAME_BUFFER_SIZE 64
#endif

#ifndef NVML_DEVICE_UUID_BUFFER_SIZE
#define NVML_DEVICE_UUID_BUFFER_SIZE 80
#endif

_Static_assert(
    BLACKWELL_THERM_CHANNEL_COUNT <= GPU_MAX_JUNCTION_CHANNELS,
    "junction channel capacity is too small"
);

_Static_assert(
    BLACKWELL_THERM_SPAN == 0x18u,
    "unexpected Blackwell thermal span"
);

_Static_assert(
    GDDR7_DQR_SPAN == 0x0003C014u,
    "unexpected GDDR7 DQR span"
);

_Static_assert(
    GDDR7_STANDARD_FBPA_COUNT * 2u <= GPU_MAX_VRAM_SOURCES,
    "standard GDDR7 source capacity is too small"
);

_Static_assert(
    GDDR7_CLAMSHELL_FBPA_COUNT *
        GDDR7_DQR_SUBREGISTER_COUNT <= GPU_MAX_VRAM_SOURCES,
    "clamshell GDDR7 source capacity is too small"
);

_Static_assert(
    GDDR7_MAX_FBPA_COUNT *
        GDDR7_DQR_SUBREGISTER_COUNT <= GPU_MAX_VRAM_SOURCES,
    "ungrouped GDDR7 source capacity is too small"
);

typedef enum {
    JUNCTION_THERM_BYTE,
    JUNCTION_BLACKWELL_THERMAL
} JunctionMethod;

typedef enum {
    VRAM_GDDR6_ADC,
    VRAM_GDDR7_DQR
} VramMethod;

typedef enum {
    GDDR7_TOPOLOGY_UNKNOWN,
    GDDR7_TOPOLOGY_STANDARD,
    GDDR7_TOPOLOGY_CLAMSHELL
} Gddr7Topology;

typedef struct {
    void *mapping;
    size_t mapping_len;
    volatile uint8_t *regs;
} MmioRegion;

typedef struct {
    JunctionMethod junction_method;
    VramMethod vram_method;
} SensorProfile;

typedef struct {
    uint16_t device_id;
    const SensorProfile *profile;
} DeviceProfile;

typedef struct {
    const SensorProfile *profile;
    MmioRegion junction_mmio;
    MmioRegion vram_mmio;
    Gddr7Topology gddr7_topology;
} GpuSensorLayout;

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
    GpuDevice *devices;
    unsigned int device_count;
    unsigned int selected_count;
    bool nvml_initialized;
};

static const SensorProfile gddr6_profile = {
    .junction_method = JUNCTION_THERM_BYTE,
    .vram_method = VRAM_GDDR6_ADC
};

static const SensorProfile gddr7_profile = {
    .junction_method = JUNCTION_BLACKWELL_THERMAL,
    .vram_method = VRAM_GDDR7_DQR
};

static const DeviceProfile device_profiles[] = {
    {
        .device_id = PCI_DEVICE_ID_RTX_5090,
        .profile = &gddr7_profile
    },
    {
        .device_id = PCI_DEVICE_ID_RTX_5080,
        .profile = &gddr7_profile
    },
    {
        .device_id = PCI_DEVICE_ID_RTX_5070_TI,
        .profile = &gddr7_profile
    },
    {
        .device_id = PCI_DEVICE_ID_RTX_5070,
        .profile = &gddr7_profile
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

static const SensorProfile *select_sensor_profile(uint16_t device_id)
{
    size_t count = sizeof(device_profiles) / sizeof(device_profiles[0]);

    for (size_t i = 0; i < count; i++) {
        if (device_profiles[i].device_id == device_id)
            return device_profiles[i].profile;
    }

    return &gddr6_profile;
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

static Temperature valid_temperature(int millidegrees)
{
    return (Temperature){
        .millidegrees = millidegrees,
        .valid = true
    };
}

static Temperature hottest_temperature(
    Temperature first,
    Temperature second
)
{
    if (!first.valid)
        return second;

    if (!second.valid)
        return first;

    return first.millidegrees >= second.millidegrees
        ? first
        : second;
}

static bool is_gddr7_poison(uint32_t raw)
{
    return (raw & GDDR7_POISON_MASK) == GDDR7_POISON_VALUE;
}

static Temperature decode_therm_byte(uint32_t raw)
{
    uint32_t value;

    if (raw == UINT32_MAX)
        return invalid_temperature();

    value = (raw >> JUNCTION_BYTE_SHIFT) & JUNCTION_BYTE_MASK;

    if (value >= GDDR6_TEMP_VALID_LIMIT)
        return invalid_temperature();

    return valid_temperature((int)value * 1000);
}

static Temperature decode_gddr6_adc(uint32_t raw)
{
    uint32_t value;

    if (raw == UINT32_MAX)
        return invalid_temperature();

    value = (raw & VRAM_ADC_MASK) / VRAM_ADC_DIVISOR;

    if (value >= GDDR6_TEMP_VALID_LIMIT)
        return invalid_temperature();

    return valid_temperature((int)value * 1000);
}

static Temperature decode_blackwell_temperature(uint32_t raw)
{
    uint32_t fixed;
    int millidegrees;

    if (raw == UINT32_MAX)
        return invalid_temperature();

    fixed = raw & BLACKWELL_THERM_VALUE_MASK;

    if (fixed == 0 ||
        fixed > BLACKWELL_THERM_MAX_C * BLACKWELL_THERM_SCALE) {
        return invalid_temperature();
    }

    millidegrees = (int)(
        (fixed * 1000u + BLACKWELL_THERM_SCALE / 2u) /
        BLACKWELL_THERM_SCALE
    );

    return valid_temperature(millidegrees);
}

static Temperature decode_gddr7_data(uint32_t raw)
{
    unsigned int code;

    if (raw == UINT32_MAX || is_gddr7_poison(raw))
        return invalid_temperature();

    code = (raw >> 16) & 0xFFu;

    if (code < GDDR7_CODE_MIN || code > GDDR7_CODE_MAX)
        return invalid_temperature();

    return valid_temperature(((int)code - 20) * 2000);
}

static Temperature sample_core(const GpuDevice *gpu)
{
    unsigned int value;
    nvmlReturn_t result;

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    result = nvmlDeviceGetTemperature(
        gpu->nvml,
        NVML_TEMPERATURE_GPU,
        &value
    );
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

    if (result != NVML_SUCCESS ||
        value > (unsigned int)(INT_MAX / 1000)) {
        return invalid_temperature();
    }

    return valid_temperature((int)value * 1000);
}

static void sample_gddr6_junction(
    const GpuDevice *gpu,
    JunctionReading *junction
)
{
    if (!gpu->sensors.junction_mmio.regs)
        return;

    junction->hotspot = decode_therm_byte(
        read_mmio32(&gpu->sensors.junction_mmio, 0)
    );
}

static void sample_blackwell_junction(
    const GpuDevice *gpu,
    JunctionReading *junction
)
{
    junction->channel_count = BLACKWELL_THERM_CHANNEL_COUNT;

    if (!gpu->sensors.junction_mmio.regs)
        return;

    for (unsigned int channel = 0;
         channel < BLACKWELL_THERM_CHANNEL_COUNT;
         channel++) {
        Temperature temperature = decode_blackwell_temperature(
            read_mmio32(
                &gpu->sensors.junction_mmio,
                (size_t)channel * BLACKWELL_THERM_STRIDE
            )
        );

        junction->channels[channel] = temperature;
        junction->hotspot = hottest_temperature(
            junction->hotspot,
            temperature
        );
    }

    junction->hardware_max = decode_blackwell_temperature(
        read_mmio32(
            &gpu->sensors.junction_mmio,
            BLACKWELL_THERM_HW_MAX_OFFSET
        )
    );
    junction->hardware_average = decode_blackwell_temperature(
        read_mmio32(
            &gpu->sensors.junction_mmio,
            BLACKWELL_THERM_HW_AVG_OFFSET
        )
    );
    junction->hotspot = hottest_temperature(
        junction->hotspot,
        junction->hardware_max
    );
}

static void sample_gddr6_vram(
    const GpuDevice *gpu,
    VramReading *vram
)
{
    if (!gpu->sensors.vram_mmio.regs)
        return;

    vram->hottest = decode_gddr6_adc(
        read_mmio32(&gpu->sensors.vram_mmio, 0)
    );
}

static bool valid_gddr7_validity(uint32_t raw)
{
    return raw != UINT32_MAX && !is_gddr7_poison(raw);
}

static Temperature sample_gddr7_component(
    const MmioRegion *region,
    size_t base,
    uint32_t validity,
    unsigned int subregister
)
{
    uint32_t mask;

    if (subregister >= GDDR7_DQR_SUBREGISTER_COUNT)
        return invalid_temperature();

    mask = 1u << (GDDR7_DQR_VALID_SHIFT + subregister);

    if (!(validity & mask))
        return invalid_temperature();

    return decode_gddr7_data(
        read_mmio32(
            region,
            base + (size_t)subregister * sizeof(uint32_t)
        )
    );
}

static void include_vram_source(
    VramReading *vram,
    unsigned int source,
    Temperature temperature
)
{
    if (source >= GPU_MAX_VRAM_SOURCES)
        return;

    vram->sources[source] = temperature;
    vram->hottest = hottest_temperature(
        vram->hottest,
        temperature
    );
}

static void sample_gddr7_standard(
    const GpuDevice *gpu,
    VramReading *vram
)
{
    const MmioRegion *region = &gpu->sensors.vram_mmio;

    vram->source_layout = GPU_VRAM_SOURCE_LAYOUT_STANDARD;
    vram->source_slot_count = GDDR7_STANDARD_FBPA_COUNT * 2u;

    if (!region->regs)
        return;

    for (unsigned int fbpa = 0;
         fbpa < GDDR7_STANDARD_FBPA_COUNT;
         fbpa++) {
        size_t base = (size_t)fbpa * GDDR7_DQR_STRIDE;
        uint32_t validity = read_mmio32(
            region,
            base + GDDR7_DQR_VALID_OFFSET
        );

        if (!valid_gddr7_validity(validity))
            continue;

        for (unsigned int subpartition = 0;
             subpartition < 2u;
             subpartition++) {
            Temperature first = sample_gddr7_component(
                region,
                base,
                validity,
                subpartition
            );
            Temperature second = sample_gddr7_component(
                region,
                base,
                validity,
                subpartition + 2u
            );

            include_vram_source(
                vram,
                fbpa * 2u + subpartition,
                hottest_temperature(first, second)
            );
        }
    }
}

static void sample_gddr7_independent(
    const GpuDevice *gpu,
    VramReading *vram,
    unsigned int fbpa_count,
    GpuVramSourceLayout source_layout
)
{
    const MmioRegion *region = &gpu->sensors.vram_mmio;

    vram->source_layout = source_layout;
    vram->source_slot_count =
        fbpa_count * GDDR7_DQR_SUBREGISTER_COUNT;

    if (!region->regs)
        return;

    for (unsigned int fbpa = 0; fbpa < fbpa_count; fbpa++) {
        size_t base = (size_t)fbpa * GDDR7_DQR_STRIDE;
        uint32_t validity = read_mmio32(
            region,
            base + GDDR7_DQR_VALID_OFFSET
        );

        if (!valid_gddr7_validity(validity))
            continue;

        for (unsigned int subregister = 0;
             subregister < GDDR7_DQR_SUBREGISTER_COUNT;
             subregister++) {
            include_vram_source(
                vram,
                fbpa * GDDR7_DQR_SUBREGISTER_COUNT + subregister,
                sample_gddr7_component(
                    region,
                    base,
                    validity,
                    subregister
                )
            );
        }
    }
}

static void sample_gddr7_vram(
    const GpuDevice *gpu,
    VramReading *vram
)
{
    switch (gpu->sensors.gddr7_topology) {
    case GDDR7_TOPOLOGY_STANDARD:
        sample_gddr7_standard(gpu, vram);
        break;

    case GDDR7_TOPOLOGY_CLAMSHELL:
        sample_gddr7_independent(
            gpu,
            vram,
            GDDR7_CLAMSHELL_FBPA_COUNT,
            GPU_VRAM_SOURCE_LAYOUT_CLAMSHELL
        );
        break;

    case GDDR7_TOPOLOGY_UNKNOWN:
    default:
        sample_gddr7_independent(
            gpu,
            vram,
            GDDR7_MAX_FBPA_COUNT,
            GPU_VRAM_SOURCE_LAYOUT_UNGROUPED
        );
        break;
    }
}

static void sample_gpu(GpuDevice *gpu)
{
    GpuReading reading = {
        .index = gpu->index,
        .short_name = gpu->short_name
    };

    reading.core = sample_core(gpu);

    switch (gpu->sensors.profile->junction_method) {
    case JUNCTION_THERM_BYTE:
        sample_gddr6_junction(gpu, &reading.junction);
        break;

    case JUNCTION_BLACKWELL_THERMAL:
        sample_blackwell_junction(gpu, &reading.junction);
        break;

    default:
        break;
    }

    switch (gpu->sensors.profile->vram_method) {
    case VRAM_GDDR6_ADC:
        sample_gddr6_vram(gpu, &reading.vram);
        break;

    case VRAM_GDDR7_DQR:
        sample_gddr7_vram(gpu, &reading.vram);
        break;

    default:
        break;
    }

    gpu->reading = reading;
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
    GpuSensorLayout *sensors,
    struct pci_dev *device,
    int memory_fd,
    long page_size
)
{
    uint32_t offset;
    size_t span;

    switch (sensors->profile->junction_method) {
    case JUNCTION_THERM_BYTE:
        offset = JUNCTION_THERM_BYTE_OFFSET;
        span = sizeof(uint32_t);
        break;

    case JUNCTION_BLACKWELL_THERMAL:
        offset = BLACKWELL_THERM_BASE;
        span = BLACKWELL_THERM_SPAN;
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
        &sensors->junction_mmio
    );
}

static void setup_vram_mmio(
    GpuSensorLayout *sensors,
    struct pci_dev *device,
    int memory_fd,
    long page_size
)
{
    uint32_t offset;
    size_t span;

    switch (sensors->profile->vram_method) {
    case VRAM_GDDR6_ADC:
        offset = VRAM_GDDR6_ADC_OFFSET;
        span = sizeof(uint32_t);
        break;

    case VRAM_GDDR7_DQR:
        offset = GDDR7_DQR_BASE;
        span = GDDR7_DQR_SPAN;
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
        &sensors->vram_mmio
    );
}

static Gddr7Topology detect_gddr7_topology(
    struct pci_dev *device,
    int memory_fd,
    long page_size
)
{
    MmioRegion strap_region;
    uint32_t raw;

    if (!map_mmio(
            memory_fd,
            device,
            GDDR7_STRAP_BROADCAST,
            sizeof(uint32_t),
            page_size,
            &strap_region
        )) {
        return GDDR7_TOPOLOGY_UNKNOWN;
    }

    raw = read_mmio32(&strap_region, 0);
    unmap_mmio(&strap_region);

    if (raw == 0 || raw == UINT32_MAX || is_gddr7_poison(raw))
        return GDDR7_TOPOLOGY_UNKNOWN;

    return raw & (1u << GDDR7_STRAP_X16_BIT)
        ? GDDR7_TOPOLOGY_CLAMSHELL
        : GDDR7_TOPOLOGY_STANDARD;
}

static void setup_gpu_mmio(
    GpuDevice *gpu,
    struct pci_access *access,
    int memory_fd,
    long page_size
)
{
    struct pci_dev *device;

    if (!gpu->has_pci)
        return;

    device = match_pci_device(access, &gpu->pci_info);

    if (!device)
        return;

    gpu->sensors.profile = select_sensor_profile(device->device_id);

    setup_junction_mmio(
        &gpu->sensors,
        device,
        memory_fd,
        page_size
    );
    setup_vram_mmio(
        &gpu->sensors,
        device,
        memory_fd,
        page_size
    );

    if (gpu->sensors.profile->vram_method == VRAM_GDDR7_DQR) {
        gpu->sensors.gddr7_topology = detect_gddr7_topology(
            device,
            memory_fd,
            page_size
        );
    }
}

static void log_sensor_warnings(
    unsigned int index,
    const GpuDevice *gpu
)
{
    if (!gpu->sensors.junction_mmio.regs) {
        fprintf(
            stderr,
            "gputemps: GPU %u: junction sensor unavailable\n",
            index
        );
    }

    if (!gpu->sensors.vram_mmio.regs) {
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

        gpu->index = i;
        gpu->reading.index = i;
        gpu->reading.short_name = gpu->short_name;
        gpu->sensors.profile = &gddr6_profile;

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
            unmap_mmio(
                &monitor->devices[i].sensors.junction_mmio
            );
            unmap_mmio(
                &monitor->devices[i].sensors.vram_mmio
            );
        }

        free(monitor->devices);
    }

    if (monitor->nvml_initialized)
        nvmlShutdown();

    free(monitor);
}
