#include "sensor.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include <pci/pci.h>

#define MILLIDEGREES_PER_C 1000

#define PCI_DEVICE_ID_RTX_5090 0x2B85u
#define PCI_DEVICE_ID_RTX_5080 0x2B82u
#define PCI_DEVICE_ID_RTX_5070_TI 0x2C05u
#define PCI_DEVICE_ID_RTX_5070 0x2C02u
#define PCI_DEVICE_ID_RTX_5060_TI 0x2D04u

#define GDDR6_JUNCTION_THERM_OFFSET 0x0002046Cu
#define GDDR6_VRAM_ADC_OFFSET 0x0000E2A8u
#define GDDR6_JUNCTION_SHIFT 8u
#define GDDR6_JUNCTION_MASK 0xFFu
#define GDDR6_VRAM_ADC_MASK 0xFFFu
#define GDDR6_VRAM_ADC_DIVISOR 32u
#define GDDR6_TEMP_LIMIT_C 0x7Fu

#define BLACKWELL_THERM_OFFSET 0x00AD0A90u
#define BLACKWELL_THERM_CHANNEL_COUNT 4u
#define BLACKWELL_THERM_STRIDE 0x4u
#define BLACKWELL_THERM_HW_MAX_OFFSET 0x10u
#define BLACKWELL_THERM_HW_AVG_OFFSET 0x14u
#define BLACKWELL_THERM_SPAN \
    (BLACKWELL_THERM_HW_AVG_OFFSET + sizeof(uint32_t))
#define BLACKWELL_THERM_VALUE_MASK 0xFFFFu
#define BLACKWELL_THERM_SCALE 256u
#define BLACKWELL_THERM_MAX_C 150u

#define GDDR7_DQR_OFFSET 0x009024C0u
#define GDDR7_DQR_VALID_OFFSET 0x10u
#define GDDR7_DQR_STRIDE 0x4000u
#define GDDR7_DQR_SUBREGISTER_COUNT 4u
#define GDDR7_DQR_VALID_SHIFT 24u
#define GDDR7_MAX_FBPA_COUNT 16u
#define GDDR7_STANDARD_FBPA_COUNT 16u
#define GDDR7_CLAMSHELL_FBPA_COUNT 8u
#define GDDR7_STANDARD_SUBPARTITION_COUNT 2u
#define GDDR7_DQR_SPAN \
    ((GDDR7_MAX_FBPA_COUNT - 1u) * GDDR7_DQR_STRIDE + \
     GDDR7_DQR_VALID_OFFSET + sizeof(uint32_t))

#define GDDR7_STRAP_BROADCAST_OFFSET 0x009A0200u
#define GDDR7_STRAP_X16_BIT 22u

#define GDDR7_TEMP_CODE_SHIFT 16u
#define GDDR7_TEMP_CODE_MASK 0xFFu
#define GDDR7_ZERO_C_CODE 20u
#define GDDR7_CODE_MIN 21u
#define GDDR7_CODE_MAX 80u
#define GDDR7_MILLIDEGREES_PER_CODE 2000

#define GDDR7_POISON_MASK 0xFFFF0000u
#define GDDR7_POISON_VALUE 0xBADF0000u

_Static_assert(BLACKWELL_THERM_CHANNEL_COUNT <=
    GPU_MAX_JUNCTION_CHANNELS,
    "junction channel capacity is too small");

_Static_assert(BLACKWELL_THERM_SPAN == 0x18u,
    "unexpected Blackwell thermal span");

_Static_assert(GDDR7_DQR_SPAN == 0x0003C014u,
    "unexpected GDDR7 DQR span");

_Static_assert(GDDR7_STANDARD_FBPA_COUNT *
    GDDR7_STANDARD_SUBPARTITION_COUNT <= GPU_MAX_VRAM_SOURCES,
    "standard GDDR7 source capacity is too small");

_Static_assert(GDDR7_CLAMSHELL_FBPA_COUNT *
    GDDR7_DQR_SUBREGISTER_COUNT <= GPU_MAX_VRAM_SOURCES,
    "clamshell GDDR7 source capacity is too small");

_Static_assert(GDDR7_MAX_FBPA_COUNT *
    GDDR7_DQR_SUBREGISTER_COUNT <= GPU_MAX_VRAM_SOURCES,
    "ungrouped GDDR7 source capacity is too small");

typedef enum {
    JUNCTION_THERM_BYTE,
    JUNCTION_BLACKWELL_THERMAL
} JunctionMethod;

typedef enum {
    VRAM_GDDR6_ADC,
    VRAM_GDDR7_DQR
} VramMethod;

struct SensorProfile {
    JunctionMethod junction_method;
    VramMethod vram_method;
};

typedef struct {
    uint16_t device_id;
    const SensorProfile *profile;
} DeviceProfile;

static const SensorProfile gddr6_profile = {
    .junction_method = JUNCTION_THERM_BYTE,
    .vram_method = VRAM_GDDR6_ADC
};

static const SensorProfile gddr7_profile = {
    .junction_method = JUNCTION_BLACKWELL_THERMAL,
    .vram_method = VRAM_GDDR7_DQR
};

static const DeviceProfile device_profiles[] = {
    { PCI_DEVICE_ID_RTX_5090, &gddr7_profile },
    { PCI_DEVICE_ID_RTX_5080, &gddr7_profile },
    { PCI_DEVICE_ID_RTX_5070_TI, &gddr7_profile },
    { PCI_DEVICE_ID_RTX_5070, &gddr7_profile },
    { PCI_DEVICE_ID_RTX_5060_TI, &gddr7_profile },
};

static const SensorProfile *select_sensor_profile(uint16_t device_id)
{
    size_t count = sizeof(device_profiles) / sizeof(device_profiles[0]);

    for (size_t i = 0; i < count; i++) {
        if (device_profiles[i].device_id == device_id)
            return device_profiles[i].profile;
    }

    return &gddr6_profile;
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

static Temperature hottest_temperature(Temperature first,
    Temperature second)
{
    if (!first.valid)
        return second;

    if (!second.valid)
        return first;

    return first.millidegrees >= second.millidegrees ? first : second;
}

static bool is_gddr7_poison(uint32_t raw)
{
    return (raw & GDDR7_POISON_MASK) == GDDR7_POISON_VALUE;
}

static bool gddr7_status_readable(uint32_t status)
{
    return status != UINT32_MAX && !is_gddr7_poison(status);
}

static Temperature decode_gddr6_junction(uint32_t raw)
{
    uint32_t celsius;

    if (raw == UINT32_MAX)
        return invalid_temperature();

    celsius = (raw >> GDDR6_JUNCTION_SHIFT) & GDDR6_JUNCTION_MASK;

    if (celsius >= GDDR6_TEMP_LIMIT_C)
        return invalid_temperature();

    return valid_temperature((int)celsius * MILLIDEGREES_PER_C);
}

static Temperature decode_gddr6_vram(uint32_t raw)
{
    uint32_t celsius;

    if (raw == UINT32_MAX)
        return invalid_temperature();

    celsius = (raw & GDDR6_VRAM_ADC_MASK) / GDDR6_VRAM_ADC_DIVISOR;

    if (celsius >= GDDR6_TEMP_LIMIT_C)
        return invalid_temperature();

    return valid_temperature((int)celsius * MILLIDEGREES_PER_C);
}

static Temperature decode_blackwell_temperature(uint32_t raw)
{
    uint32_t fixed;
    int millidegrees;

    if (raw == UINT32_MAX)
        return invalid_temperature();

    fixed = raw & BLACKWELL_THERM_VALUE_MASK;

    if (!fixed ||
        fixed > BLACKWELL_THERM_MAX_C * BLACKWELL_THERM_SCALE) {
        return invalid_temperature();
    }

    millidegrees = (int)((fixed * MILLIDEGREES_PER_C +
        BLACKWELL_THERM_SCALE / 2u) / BLACKWELL_THERM_SCALE);

    return valid_temperature(millidegrees);
}

static Temperature decode_gddr7_vram(uint32_t raw)
{
    unsigned int code;
    int millidegrees;

    if (raw == UINT32_MAX || is_gddr7_poison(raw))
        return invalid_temperature();

    code = (raw >> GDDR7_TEMP_CODE_SHIFT) & GDDR7_TEMP_CODE_MASK;

    if (code < GDDR7_CODE_MIN || code > GDDR7_CODE_MAX)
        return invalid_temperature();

    millidegrees = ((int)code - (int)GDDR7_ZERO_C_CODE) *
        GDDR7_MILLIDEGREES_PER_CODE;

    return valid_temperature(millidegrees);
}

static Temperature sample_core_temperature(nvmlDevice_t nvml)
{
    unsigned int celsius;
    nvmlReturn_t result;

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    result = nvmlDeviceGetTemperature(
        nvml, NVML_TEMPERATURE_GPU, &celsius);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

    if (result != NVML_SUCCESS ||
        celsius > (unsigned int)(INT_MAX / MILLIDEGREES_PER_C)) {
        return invalid_temperature();
    }

    return valid_temperature((int)celsius * MILLIDEGREES_PER_C);
}

static void sample_gddr6_junction(const GpuSensorLayout *sensors,
    JunctionReading *junction)
{
    uint32_t raw;

    if (!sensors->junction_mmio.regs)
        return;

    raw = mmio_read32(&sensors->junction_mmio, 0);
    junction->hotspot = decode_gddr6_junction(raw);
}

static void sample_blackwell_junction(const GpuSensorLayout *sensors,
    JunctionReading *junction)
{
    uint32_t raw;

    junction->channel_count = BLACKWELL_THERM_CHANNEL_COUNT;

    if (!sensors->junction_mmio.regs)
        return;

    for (unsigned int channel = 0;
        channel < BLACKWELL_THERM_CHANNEL_COUNT; channel++) {
        size_t offset = (size_t)channel * BLACKWELL_THERM_STRIDE;
        Temperature temperature;

        raw = mmio_read32(&sensors->junction_mmio, offset);
        temperature = decode_blackwell_temperature(raw);
        junction->channels[channel] = temperature;
        junction->hotspot = hottest_temperature(
            junction->hotspot, temperature);
    }

    raw = mmio_read32(&sensors->junction_mmio,
        BLACKWELL_THERM_HW_MAX_OFFSET);
    junction->hardware_max = decode_blackwell_temperature(raw);

    raw = mmio_read32(&sensors->junction_mmio,
        BLACKWELL_THERM_HW_AVG_OFFSET);
    junction->hardware_average = decode_blackwell_temperature(raw);
    junction->hotspot = hottest_temperature(
        junction->hotspot, junction->hardware_max);
}

static void sample_gddr6_vram(const GpuSensorLayout *sensors,
    VramReading *vram)
{
    uint32_t raw;

    if (!sensors->vram_mmio.regs)
        return;

    raw = mmio_read32(&sensors->vram_mmio, 0);
    vram->hottest = decode_gddr6_vram(raw);
}

static Temperature sample_gddr7_component(const MmioRegion *region,
    size_t fbpa_offset, uint32_t status, unsigned int subregister)
{
    uint32_t valid_mask;
    size_t offset;

    if (subregister >= GDDR7_DQR_SUBREGISTER_COUNT)
        return invalid_temperature();

    valid_mask = 1u << (GDDR7_DQR_VALID_SHIFT + subregister);

    if (!(status & valid_mask))
        return invalid_temperature();

    offset = fbpa_offset + (size_t)subregister * sizeof(uint32_t);
    return decode_gddr7_vram(mmio_read32(region, offset));
}

static void record_vram_source(VramReading *vram,
    unsigned int source_index, Temperature temperature)
{
    if (source_index >= GPU_MAX_VRAM_SOURCES)
        return;

    vram->sources[source_index] = temperature;
    vram->hottest = hottest_temperature(vram->hottest, temperature);
}

static void sample_gddr7_standard(const GpuSensorLayout *sensors,
    VramReading *vram)
{
    const MmioRegion *region = &sensors->vram_mmio;

    vram->source_layout = GPU_VRAM_SOURCE_LAYOUT_STANDARD;
    vram->source_slot_count = GDDR7_STANDARD_FBPA_COUNT *
        GDDR7_STANDARD_SUBPARTITION_COUNT;

    if (!region->regs)
        return;

    for (unsigned int fbpa = 0; fbpa < GDDR7_STANDARD_FBPA_COUNT;
        fbpa++) {
        size_t fbpa_offset = (size_t)fbpa * GDDR7_DQR_STRIDE;
        size_t status_offset = fbpa_offset + GDDR7_DQR_VALID_OFFSET;
        unsigned int source_base;
        uint32_t status = mmio_read32(region, status_offset);

        if (!gddr7_status_readable(status))
            continue;

        source_base = fbpa * GDDR7_STANDARD_SUBPARTITION_COUNT;

        for (unsigned int subpartition = 0;
            subpartition < GDDR7_STANDARD_SUBPARTITION_COUNT;
            subpartition++) {
            unsigned int paired = subpartition +
                GDDR7_STANDARD_SUBPARTITION_COUNT;
            unsigned int source_index = source_base + subpartition;
            Temperature first = sample_gddr7_component(
                region, fbpa_offset, status, subpartition);
            Temperature second = sample_gddr7_component(
                region, fbpa_offset, status, paired);
            Temperature temperature =
                hottest_temperature(first, second);

            record_vram_source(vram, source_index, temperature);
        }
    }
}

static void sample_gddr7_independent(const GpuSensorLayout *sensors,
    VramReading *vram, unsigned int fbpa_count,
    GpuVramSourceLayout source_layout)
{
    const MmioRegion *region = &sensors->vram_mmio;

    vram->source_layout = source_layout;
    vram->source_slot_count =
        fbpa_count * GDDR7_DQR_SUBREGISTER_COUNT;

    if (!region->regs)
        return;

    for (unsigned int fbpa = 0; fbpa < fbpa_count; fbpa++) {
        size_t fbpa_offset = (size_t)fbpa * GDDR7_DQR_STRIDE;
        size_t status_offset = fbpa_offset + GDDR7_DQR_VALID_OFFSET;
        unsigned int source_base;
        uint32_t status = mmio_read32(region, status_offset);

        if (!gddr7_status_readable(status))
            continue;

        source_base = fbpa * GDDR7_DQR_SUBREGISTER_COUNT;

        for (unsigned int subregister = 0;
            subregister < GDDR7_DQR_SUBREGISTER_COUNT;
            subregister++) {
            unsigned int source_index = source_base + subregister;
            Temperature temperature = sample_gddr7_component(
                region, fbpa_offset, status, subregister);

            record_vram_source(vram, source_index, temperature);
        }
    }
}

static void sample_gddr7_vram(const GpuSensorLayout *sensors,
    VramReading *vram)
{
    switch (sensors->gddr7_topology) {
    case GDDR7_TOPOLOGY_STANDARD:
        sample_gddr7_standard(sensors, vram);
        break;

    case GDDR7_TOPOLOGY_CLAMSHELL:
        sample_gddr7_independent(sensors, vram,
            GDDR7_CLAMSHELL_FBPA_COUNT,
            GPU_VRAM_SOURCE_LAYOUT_CLAMSHELL);
        break;

    case GDDR7_TOPOLOGY_UNKNOWN:
    default:
        sample_gddr7_independent(sensors, vram,
            GDDR7_MAX_FBPA_COUNT,
            GPU_VRAM_SOURCE_LAYOUT_UNGROUPED);
        break;
    }
}

static void map_junction_sensor(GpuSensorLayout *sensors,
    const struct pci_dev *pci_device, int memory_fd, long page_size)
{
    uint32_t offset;
    size_t span;

    switch (sensors->profile->junction_method) {
    case JUNCTION_THERM_BYTE:
        offset = GDDR6_JUNCTION_THERM_OFFSET;
        span = sizeof(uint32_t);
        break;

    case JUNCTION_BLACKWELL_THERMAL:
        offset = BLACKWELL_THERM_OFFSET;
        span = BLACKWELL_THERM_SPAN;
        break;

    default:
        return;
    }

    mmio_map_bar0(&sensors->junction_mmio, memory_fd, pci_device,
        offset, span, page_size);
}

static void map_vram_sensor(GpuSensorLayout *sensors,
    const struct pci_dev *pci_device, int memory_fd, long page_size)
{
    uint32_t offset;
    size_t span;

    switch (sensors->profile->vram_method) {
    case VRAM_GDDR6_ADC:
        offset = GDDR6_VRAM_ADC_OFFSET;
        span = sizeof(uint32_t);
        break;

    case VRAM_GDDR7_DQR:
        offset = GDDR7_DQR_OFFSET;
        span = GDDR7_DQR_SPAN;
        break;

    default:
        return;
    }

    mmio_map_bar0(&sensors->vram_mmio, memory_fd, pci_device,
        offset, span, page_size);
}

static Gddr7Topology detect_gddr7_topology(
    const struct pci_dev *pci_device, int memory_fd, long page_size)
{
    MmioRegion strap_mmio = {0};
    uint32_t raw;

    if (!mmio_map_bar0(&strap_mmio, memory_fd, pci_device,
            GDDR7_STRAP_BROADCAST_OFFSET, sizeof(uint32_t),
            page_size)) {
        return GDDR7_TOPOLOGY_UNKNOWN;
    }

    raw = mmio_read32(&strap_mmio, 0);
    mmio_unmap(&strap_mmio);

    if (!raw || raw == UINT32_MAX || is_gddr7_poison(raw))
        return GDDR7_TOPOLOGY_UNKNOWN;

    return raw & (1u << GDDR7_STRAP_X16_BIT)
        ? GDDR7_TOPOLOGY_CLAMSHELL
        : GDDR7_TOPOLOGY_STANDARD;
}

void sensor_init(GpuSensorLayout *sensors)
{
    if (!sensors)
        return;

    memset(sensors, 0, sizeof(*sensors));
    sensors->profile = &gddr6_profile;
}

void sensor_setup(GpuSensorLayout *sensors,
    const struct pci_dev *pci_device, int memory_fd, long page_size)
{
    if (!sensors || !pci_device)
        return;

    sensors->profile = select_sensor_profile(pci_device->device_id);
    map_junction_sensor(sensors, pci_device, memory_fd, page_size);
    map_vram_sensor(sensors, pci_device, memory_fd, page_size);

    if (sensors->profile->vram_method == VRAM_GDDR7_DQR) {
        sensors->gddr7_topology = detect_gddr7_topology(
            pci_device, memory_fd, page_size);
    }
}

void sensor_sample(const GpuSensorLayout *sensors, nvmlDevice_t nvml,
    GpuReading *reading)
{
    if (!sensors || !reading)
        return;

    reading->core = sample_core_temperature(nvml);

    switch (sensors->profile->junction_method) {
    case JUNCTION_THERM_BYTE:
        sample_gddr6_junction(sensors, &reading->junction);
        break;

    case JUNCTION_BLACKWELL_THERMAL:
        sample_blackwell_junction(sensors, &reading->junction);
        break;

    default:
        break;
    }

    switch (sensors->profile->vram_method) {
    case VRAM_GDDR6_ADC:
        sample_gddr6_vram(sensors, &reading->vram);
        break;

    case VRAM_GDDR7_DQR:
        sample_gddr7_vram(sensors, &reading->vram);
        break;

    default:
        break;
    }
}

bool sensor_junction_mapped(const GpuSensorLayout *sensors)
{
    return sensors && sensors->junction_mmio.regs;
}

bool sensor_vram_mapped(const GpuSensorLayout *sensors)
{
    return sensors && sensors->vram_mmio.regs;
}

void sensor_cleanup(GpuSensorLayout *sensors)
{
    if (!sensors)
        return;

    mmio_unmap(&sensors->junction_mmio);
    mmio_unmap(&sensors->vram_mmio);
}
