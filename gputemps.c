#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <nvml.h>
#include <pci/pci.h>

#define BUFFER_SIZE  16384
#define MAX_GPUS     64
#define MEM_PATH     "/dev/mem"
#define REG_SIZE     sizeof(uint32_t)

#define NVIDIA_VENDOR_ID  0x10DEu
#define PCI_DEVID_SHIFT   16
#define PCI_BAR_IO_SPACE  0x1u

#define GPU_TEMP_WARN         70u
#define GPU_TEMP_DANGER       85u
#define JUNCTION_TEMP_WARN    80u
#define JUNCTION_TEMP_DANGER  95u
#define VRAM_TEMP_WARN        80u
#define VRAM_TEMP_DANGER      95u

#define OFFSET_HOTSPOT_DEFAULT    0x0002046Cu
#define OFFSET_HOTSPOT_BLACKWELL  0x00AD046Cu
#define OFFSET_VRAM_DEFAULT       0x0000E2A8u

#define HOTSPOT_BYTE_SHIFT  8
#define HOTSPOT_BYTE_MASK   0xFFu
#define VRAM_ADC_MASK       0xFFFu
#define VRAM_ADC_DIVISOR    32u
#define TEMP_VALID_LIMIT    0x7Fu

#define VISIBLE_DEVICES_ENV  "NVIDIA_VISIBLE_DEVICES"

#define SEPARATOR    "\xE2\x94\x82"
#define CURSOR_HIDE  "\x1B[?25l"
#define CURSOR_SHOW  "\x1B[?25h"
#define COLOR_RESET  "\x1B[0m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_YELLOW "\x1B[33m"
#define COLOR_RED    "\x1B[31m"

#define NVML_ARCH_BLACKWELL_VALUE  10u

#if defined(NVML_API_VERSION) && NVML_API_VERSION >= 11
#define HAVE_NVML_ARCH_API  1
#else
#define HAVE_NVML_ARCH_API  0
#endif

#ifndef NVML_DEVICE_NAME_BUFFER_SIZE
#define NVML_DEVICE_NAME_BUFFER_SIZE  64
#endif

#ifndef NVML_DEVICE_UUID_BUFFER_SIZE
#define NVML_DEVICE_UUID_BUFFER_SIZE  80
#endif

typedef enum { FORMAT_TABLE, FORMAT_JSON } OutputFormat;
typedef enum { MODE_CONTINUOUS, MODE_ONCE  } OutputMode;
typedef enum { SELECT_NONE, SELECT_DEVICE, SELECT_VISIBLE } SelectionMode;

typedef struct {
    uint32_t value;
    bool valid;
} Temperature;

typedef struct {
    bool known;
    unsigned int value;
    bool is_blackwell;
} GpuArchitecture;

typedef struct {
    nvmlDevice_t device;
    nvmlPciInfo_t pci_info;
    char name[NVML_DEVICE_NAME_BUFFER_SIZE];
    char short_name[NVML_DEVICE_NAME_BUFFER_SIZE];
    char uuid[NVML_DEVICE_UUID_BUFFER_SIZE];
    Temperature gpu_temp;
    Temperature junction_temp;
    Temperature vram_temp;
    void *hotspot_map;
    volatile uint32_t *hotspot_ptr;
    void *vram_map;
    volatile uint32_t *vram_ptr;
    bool present;
    bool has_pci;
    bool selected;
    GpuArchitecture arch;
} GpuDevice;

typedef struct {
    unsigned int device_count;
    unsigned int monitored_count;
    unsigned int index_width;
    unsigned int name_width;
    int mem_fd;
    int tick;
    int initialized;
    int refresh_ms;
    OutputMode output_mode;
    OutputFormat output_format;
    SelectionMode selection_mode;
    const char *device_selector;
    const char *visible_devices;
    struct pci_access *pacc;
    GpuDevice *gpus;
    char output_buffer[BUFFER_SIZE];
    size_t buffer_pos;
} Context;

static volatile sig_atomic_t running = 1;
static struct termios orig_termios;
static long page_size;


static inline off_t page_base_of(pciaddr_t addr) {
    return (off_t)(addr & ~((pciaddr_t)page_size - 1));
}

static inline uint32_t page_offset_of(pciaddr_t addr) {
    return (uint32_t)(addr & ((pciaddr_t)page_size - 1));
}

static inline unsigned int max_uint(unsigned int a, unsigned int b) {
    return a > b ? a : b;
}


static void log_msg(const char *level, const char *fmt, ...) {
    va_list args;

    fprintf(stderr, "%s: ", level);

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
}

static void init_context_defaults(Context *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->output_format = FORMAT_TABLE;
    ctx->output_mode = MODE_CONTINUOUS;
    ctx->refresh_ms = 1000;
    ctx->mem_fd = -1;
    ctx->index_width = 1;
    ctx->name_width = 3;
}

static void cleanup_context(Context *ctx) {
    if (!ctx) return;

    if (ctx->gpus) {
        for (unsigned int i = 0; i < ctx->device_count; i++) {
            GpuDevice *gpu = &ctx->gpus[i];

            if (gpu->hotspot_map && gpu->hotspot_map != MAP_FAILED)
                munmap(gpu->hotspot_map, page_size);

            if (gpu->vram_map && gpu->vram_map != MAP_FAILED &&
                gpu->vram_map != gpu->hotspot_map)
                munmap(gpu->vram_map, page_size);
        }

        free(ctx->gpus);
        ctx->gpus = NULL;
    }

    if (ctx->mem_fd >= 0) {
        close(ctx->mem_fd);
        ctx->mem_fd = -1;
    }

    if (ctx->pacc) {
        pci_cleanup(ctx->pacc);
        ctx->pacc = NULL;
    }

    if (ctx->initialized) {
        nvmlShutdown();
        ctx->initialized = 0;
    }
}


static void signal_handler(int signum) {
    (void)signum;
    running = 0;
}

static void restore_cursor(void) {
    printf(CURSOR_SHOW);
    fflush(stdout);
}

static void reset_terminal(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

static int setup_terminal(void) {
    struct termios t;

    if (tcgetattr(STDIN_FILENO, &orig_termios) < 0)
        return -1;

    atexit(reset_terminal);

    t = orig_termios;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;

    return tcsetattr(STDIN_FILENO, TCSANOW, &t) < 0 ? -1 : 0;
}


static void buffer_append(Context *ctx, const char *fmt, ...) {
    if (ctx->buffer_pos >= BUFFER_SIZE - 1)
        return;

    int remaining = (int)(BUFFER_SIZE - ctx->buffer_pos);
    va_list args;

    va_start(args, fmt);
    int n = vsnprintf(
        ctx->output_buffer + ctx->buffer_pos,
        remaining,
        fmt,
        args
    );
    va_end(args);

    if (n < 0)
        return;

    if (n >= remaining)
        ctx->buffer_pos = BUFFER_SIZE - 1;
    else
        ctx->buffer_pos += n;
}

static const char *get_temp_color(
    uint32_t temp,
    uint32_t warn,
    uint32_t danger
) {
    if (temp >= danger) return COLOR_RED;
    if (temp >= warn) return COLOR_YELLOW;
    return COLOR_GREEN;
}

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static unsigned int digits_uint(unsigned int n) {
    unsigned int digits = 1;

    while (n >= 10) {
        n /= 10;
        digits++;
    }

    return digits;
}

static void append_spaces(Context *ctx, unsigned int count) {
    for (unsigned int i = 0; i < count; i++)
        buffer_append(ctx, " ");
}

static void copy_string(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0)
        return;

    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void replace_suffix(
    char *s,
    size_t s_size,
    const char *suffix,
    const char *replacement
) {
    size_t len = strlen(s);
    size_t suffix_len = strlen(suffix);
    size_t replacement_len = strlen(replacement);

    if (len < suffix_len)
        return;

    if (strcmp(s + len - suffix_len, suffix) != 0)
        return;

    if (len - suffix_len + replacement_len >= s_size)
        return;

    snprintf(s + len - suffix_len,
        s_size - (len - suffix_len),
        "%s",
        replacement);
}

static void make_short_name(const char *name, char *out, size_t out_size) {
    const char *s = name;

    if (starts_with(s, "NVIDIA "))
        s += strlen("NVIDIA ");

    if (starts_with(s, "GeForce "))
        s += strlen("GeForce ");

    copy_string(out, out_size, s);
    replace_suffix(out, out_size, " SUPER", " S");
}

static void append_padded(Context *ctx, const char *s, unsigned int width) {
    unsigned int len = (unsigned int)strlen(s);

    buffer_append(ctx, "%s", s);

    if (len < width)
        append_spaces(ctx, width - len);
}

static void append_name_cell(Context *ctx, const char *name) {
    buffer_append(ctx, " ");
    append_padded(ctx, name, ctx->name_width);
    buffer_append(ctx, " ");
}

static void append_temp_header_cell(Context *ctx, const char *name) {
    buffer_append(ctx, "  %s  ", name);
}

static void append_temp_cell(
    Context *ctx,
    Temperature temp,
    uint32_t warn,
    uint32_t danger
) {
    if (!temp.valid) {
        buffer_append(ctx, "  N/A   ");
        return;
    }

    buffer_append(
        ctx,
        " %s%3u°C%s  ",
        get_temp_color(temp.value, warn, danger),
        temp.value,
        COLOR_RESET
    );
}

static void append_json_temp(Context *ctx, const char *name, Temperature temp) {
    if (temp.valid)
        buffer_append(ctx, "\"%s\":%u", name, temp.value);
    else
        buffer_append(ctx, "\"%s\":null", name);
}


static bool temp_valid(uint32_t temp) {
    return temp < TEMP_VALID_LIMIT;
}

static GpuArchitecture get_gpu_architecture(nvmlDevice_t device) {
    GpuArchitecture arch = {0};

#if HAVE_NVML_ARCH_API
    nvmlDeviceArchitecture_t nvml_arch;

    if (nvmlDeviceGetArchitecture(device, &nvml_arch) == NVML_SUCCESS) {
        arch.known = true;
        arch.value = (unsigned int)nvml_arch;
        arch.is_blackwell = arch.value == NVML_ARCH_BLACKWELL_VALUE;
    }
#else
    (void)device;
#endif

    return arch;
}

static bool arch_uses_default_mmio(const GpuArchitecture *arch) {
    return arch &&
        arch->known &&
        !arch->is_blackwell &&
        arch->value > 0 &&
        arch->value < NVML_ARCH_BLACKWELL_VALUE;
}

static void determine_offsets(
    const GpuArchitecture *arch,
    uint32_t *hotspot_off,
    bool *has_hotspot,
    uint32_t *vram_off,
    bool *has_vram
) {
    *hotspot_off = 0;
    *vram_off = 0;
    *has_hotspot = false;
    *has_vram = false;

    if (!arch || !arch->known)
        return;

    if (arch->is_blackwell) {
        *hotspot_off = OFFSET_HOTSPOT_BLACKWELL;
        *has_hotspot = true;
        return;
    }

    if (arch_uses_default_mmio(arch)) {
        *hotspot_off = OFFSET_HOTSPOT_DEFAULT;
        *vram_off = OFFSET_VRAM_DEFAULT;
        *has_hotspot = true;
        *has_vram = true;
    }
}

static void map_register(
    int mem_fd,
    struct pci_dev *dev,
    uint32_t offset,
    void **map_out,
    volatile uint32_t **ptr_out
) {
    *map_out = MAP_FAILED;
    *ptr_out = NULL;

    if (offset == 0)
        return;

    if (dev->base_addr[0] == 0 || dev->size[0] < REG_SIZE)
        return;

    if (dev->base_addr[0] & PCI_BAR_IO_SPACE)
        return;

    if ((pciaddr_t)offset > dev->size[0] - REG_SIZE)
        return;

    pciaddr_t bar0 = dev->base_addr[0] & ~(pciaddr_t)0xF;
    pciaddr_t phys = bar0 + offset;
    uint32_t page_off = page_offset_of(phys);

    if ((uint64_t)page_off + REG_SIZE > (uint64_t)page_size)
        return;

    *map_out = mmap(
        NULL,
        page_size,
        PROT_READ,
        MAP_SHARED,
        mem_fd,
        page_base_of(phys)
    );

    if (*map_out != MAP_FAILED) {
        *ptr_out = (volatile uint32_t *)(
            (char *)*map_out + page_off
        );
    }
}

static struct pci_dev *match_pci_dev(Context *ctx, nvmlPciInfo_t *nvml_pci) {
    uint32_t target = nvml_pci->pciDeviceId;

    for (struct pci_dev *dev = ctx->pacc->devices; dev; dev = dev->next) {
        pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES | PCI_FILL_SIZES);

        if (dev->vendor_id != NVIDIA_VENDOR_ID)
            continue;

        uint32_t dev_id =
            ((uint32_t)dev->device_id << PCI_DEVID_SHIFT) | dev->vendor_id;

        if (dev_id == target &&
            (unsigned int)dev->domain == nvml_pci->domain &&
            dev->bus == nvml_pci->bus &&
            dev->dev == nvml_pci->device)
            return dev;
    }

    return NULL;
}

static void setup_gpu_memory(Context *ctx, GpuDevice *gpu) {
    uint32_t hotspot_off, vram_off;
    bool has_hotspot, has_vram;

    if (!gpu->has_pci)
        return;

    struct pci_dev *dev = match_pci_dev(ctx, &gpu->pci_info);
    if (!dev)
        return;

    determine_offsets(
        &gpu->arch,
        &hotspot_off,
        &has_hotspot,
        &vram_off,
        &has_vram
    );

    if (has_hotspot) {
        map_register(
            ctx->mem_fd,
            dev,
            hotspot_off,
            &gpu->hotspot_map,
            &gpu->hotspot_ptr
        );
    }

    if (!has_vram) {
        gpu->vram_map = MAP_FAILED;
        gpu->vram_ptr = NULL;
        return;
    }

    if (has_hotspot && gpu->hotspot_map != MAP_FAILED) {
        pciaddr_t bar0 = dev->base_addr[0] & ~(pciaddr_t)0xF;
        off_t hotspot_page = page_base_of(bar0 + hotspot_off);
        off_t vram_page = page_base_of(bar0 + vram_off);

        if (hotspot_page == vram_page) {
            gpu->vram_map = gpu->hotspot_map;
            gpu->vram_ptr = (volatile uint32_t *)(
                (char *)gpu->vram_map + page_offset_of(bar0 + vram_off)
            );
            return;
        }
    }

    map_register(
        ctx->mem_fd,
        dev,
        vram_off,
        &gpu->vram_map,
        &gpu->vram_ptr
    );
}

static void log_sensor_status(unsigned int idx, const GpuDevice *gpu) {
    if (!gpu->arch.known) {
#if HAVE_NVML_ARCH_API
        log_msg("warn", "GPU %u: NVML arch unknown; junction/VRAM off", idx);
#endif
        return;
    }

    if (gpu->arch.is_blackwell) {
        log_msg(
            gpu->hotspot_ptr ? "note" : "warn",
            "GPU %u Blackwell: hotspot %s, VRAM off",
            idx,
            gpu->hotspot_ptr ? "on" : "off"
        );
        return;
    }

    if (!arch_uses_default_mmio(&gpu->arch)) {
        log_msg(
            "warn",
            "GPU %u: unsupported NVML arch=%u; junction/VRAM off",
            idx,
            gpu->arch.value
        );
    }
}


static bool parse_uint(const char *s, unsigned int *out) {
    char *end;
    unsigned long n;

    if (!s || *s == '\0')
        return false;

    errno = 0;
    n = strtoul(s, &end, 10);

    if (errno || end == s || *end != '\0' || n > UINT_MAX)
        return false;

    *out = (unsigned int)n;
    return true;
}

static bool parse_bdf(
    const char *s,
    unsigned int *domain,
    unsigned int *bus,
    unsigned int *device,
    unsigned int *function
) {
    unsigned int d, b, dev, fn;
    char tail;

    if (sscanf(s, "%x:%x:%x.%x%c", &d, &b, &dev, &fn, &tail) == 4) {
        if (b <= 0xFFu && dev <= 0x1Fu && fn <= 7u) {
            *domain = d;
            *bus = b;
            *device = dev;
            *function = fn;
            return true;
        }
    }

    d = 0;
    if (sscanf(s, "%x:%x.%x%c", &b, &dev, &fn, &tail) == 3) {
        if (b <= 0xFFu && dev <= 0x1Fu && fn <= 7u) {
            *domain = d;
            *bus = b;
            *device = dev;
            *function = fn;
            return true;
        }
    }

    return false;
}

static bool selector_matches_gpu(
    const char *selector,
    unsigned int idx,
    GpuDevice *gpu
) {
    unsigned int n;
    unsigned int domain, bus, device, function;

    if (!gpu->present)
        return false;

    if (parse_uint(selector, &n))
        return n == idx;

    if (gpu->uuid[0] && strcmp(selector, gpu->uuid) == 0)
        return true;

    if (parse_bdf(selector, &domain, &bus, &device, &function)) {
        return gpu->has_pci &&
            domain == gpu->pci_info.domain &&
            bus == gpu->pci_info.bus &&
            device == gpu->pci_info.device &&
            function == 0;
    }

    return false;
}

static int apply_selector(Context *ctx, const char *selector, bool required) {
    bool matched = false;

    for (unsigned int i = 0; i < ctx->device_count; i++) {
        if (!selector_matches_gpu(selector, i, &ctx->gpus[i]))
            continue;

        if (!ctx->gpus[i].selected) {
            ctx->gpus[i].selected = true;
            ctx->monitored_count++;
        }

        matched = true;
    }

    if (required && !matched) {
        fprintf(stderr, "Invalid device selector: %s\n", selector);
        return -1;
    }

    return 0;
}

static void select_all_present(Context *ctx) {
    ctx->monitored_count = 0;

    for (unsigned int i = 0; i < ctx->device_count; i++) {
        ctx->gpus[i].selected = ctx->gpus[i].present;

        if (ctx->gpus[i].selected)
            ctx->monitored_count++;
    }
}

static int apply_selector_list(
    Context *ctx,
    const char *selectors,
    bool required
) {
    char *copy, *token, *saveptr = NULL;

    if (!selectors || *selectors == '\0') {
        if (required) {
            fprintf(stderr, "Invalid device selector: \n");
            return -1;
        }

        select_all_present(ctx);
        return 0;
    }

    if (strcmp(selectors, "all") == 0) {
        select_all_present(ctx);
        return 0;
    }

    if (strcmp(selectors, "none") == 0 || strcmp(selectors, "void") == 0) {
        ctx->monitored_count = 0;
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
        while (isspace((unsigned char)*token))
            token++;

        char *end = token + strlen(token);
        while (end > token && isspace((unsigned char)end[-1]))
            *--end = '\0';

        if (*token != '\0' && apply_selector(ctx, token, required) < 0) {
            free(copy);
            return -1;
        }
    }

    free(copy);
    return 0;
}

static int apply_selection(Context *ctx) {
    if (ctx->selection_mode == SELECT_DEVICE)
        return apply_selector_list(ctx, ctx->device_selector, true);

    if (ctx->selection_mode == SELECT_VISIBLE)
        return apply_selector_list(ctx, ctx->visible_devices, false);

    select_all_present(ctx);
    return 0;
}

static void update_column_widths(Context *ctx) {
    unsigned int max_idx = 0;

    ctx->name_width = 3;

    for (unsigned int i = 0; i < ctx->device_count; i++) {
        if (!ctx->gpus[i].selected)
            continue;

        max_idx = i;
        ctx->name_width = max_uint(
            ctx->name_width,
            (unsigned int)strlen(ctx->gpus[i].short_name)
        );
    }

    ctx->index_width = digits_uint(max_idx);
}

static int init_monitoring(Context *ctx) {
    if (geteuid() != 0) {
        fprintf(stderr, "This program requires root privileges.\n");
        return -1;
    }

#if !HAVE_NVML_ARCH_API
    log_msg("warn", "NVML arch detection unavailable; junction/VRAM off");
#endif

    ctx->mem_fd = open(MEM_PATH, O_RDONLY | O_SYNC);
    if (ctx->mem_fd < 0) {
        perror("open " MEM_PATH);
        return -1;
    }

    ctx->pacc = pci_alloc();
    if (!ctx->pacc) {
        fprintf(stderr, "pci_alloc failed\n");
        return -1;
    }

    pci_init(ctx->pacc);
    pci_scan_bus(ctx->pacc);

    nvmlReturn_t r = nvmlInit();
    if (r != NVML_SUCCESS) {
        fprintf(stderr, "nvmlInit failed: %s\n", nvmlErrorString(r));
        return -1;
    }

    ctx->initialized = 1;

    if (nvmlDeviceGetCount(&ctx->device_count) != NVML_SUCCESS ||
        ctx->device_count == 0) {
        fprintf(stderr, "No NVIDIA GPUs found.\n");
        return -1;
    }

    if (ctx->device_count > MAX_GPUS)
        ctx->device_count = MAX_GPUS;

    ctx->gpus = calloc(ctx->device_count, sizeof(*ctx->gpus));
    if (!ctx->gpus) {
        perror("calloc");
        return -1;
    }

    for (unsigned int i = 0; i < ctx->device_count; i++) {
        GpuDevice *gpu = &ctx->gpus[i];

        if (nvmlDeviceGetHandleByIndex(i, &gpu->device) != NVML_SUCCESS)
            continue;

        gpu->present = true;
        gpu->arch = get_gpu_architecture(gpu->device);

        if (nvmlDeviceGetName(gpu->device, gpu->name, sizeof(gpu->name))
            != NVML_SUCCESS)
            snprintf(gpu->name, sizeof(gpu->name), "GPU %u", i);

        if (nvmlDeviceGetUUID(gpu->device, gpu->uuid, sizeof(gpu->uuid))
            != NVML_SUCCESS)
            gpu->uuid[0] = '\0';

        if (nvmlDeviceGetPciInfo(gpu->device, &gpu->pci_info) == NVML_SUCCESS)
            gpu->has_pci = true;

        make_short_name(gpu->name, gpu->short_name, sizeof(gpu->short_name));
    }

    if (apply_selection(ctx) < 0)
        return -1;

    if (ctx->monitored_count == 0) {
        fprintf(stderr, "No GPUs selected.\n");
        return -1;
    }

    for (unsigned int i = 0; i < ctx->device_count; i++) {
        if (!ctx->gpus[i].selected)
            continue;

        setup_gpu_memory(ctx, &ctx->gpus[i]);
        log_sensor_status(i, &ctx->gpus[i]);
    }

    update_column_widths(ctx);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);

    return 0;
}


static void update_gpu_temps(GpuDevice *gpu) {
    uint32_t value;
    nvmlReturn_t r;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    r = nvmlDeviceGetTemperature(
        gpu->device,
        NVML_TEMPERATURE_GPU,
        &value
    );
#pragma GCC diagnostic pop

    gpu->gpu_temp.valid = r == NVML_SUCCESS;
    gpu->gpu_temp.value = gpu->gpu_temp.valid ? value : 0;

    if (gpu->hotspot_ptr) {
        value = (*gpu->hotspot_ptr >> HOTSPOT_BYTE_SHIFT) & HOTSPOT_BYTE_MASK;
        gpu->junction_temp.valid = temp_valid(value);
        gpu->junction_temp.value = gpu->junction_temp.valid ? value : 0;
    } else {
        gpu->junction_temp.valid = false;
        gpu->junction_temp.value = 0;
    }

    if (gpu->vram_ptr) {
        value = (*gpu->vram_ptr & VRAM_ADC_MASK) / VRAM_ADC_DIVISOR;
        gpu->vram_temp.valid = temp_valid(value);
        gpu->vram_temp.value = gpu->vram_temp.valid ? value : 0;
    } else {
        gpu->vram_temp.valid = false;
        gpu->vram_temp.value = 0;
    }
}

static void append_table_header(Context *ctx) {
    buffer_append(
        ctx,
        "\n%*s %s",
        (int)ctx->index_width,
        ctx->tick ? "" : "*",
        SEPARATOR
    );

    append_name_cell(ctx, "GPU");

    buffer_append(ctx, "%s", SEPARATOR);
    append_temp_header_cell(ctx, "CORE");

    buffer_append(ctx, "%s", SEPARATOR);
    append_temp_header_cell(ctx, "JUNC");

    buffer_append(ctx, "%s", SEPARATOR);
    append_temp_header_cell(ctx, "VRAM");

    buffer_append(ctx, "%s\n", SEPARATOR);
}

static void append_table_row(
    Context *ctx,
    unsigned int idx,
    GpuDevice *gpu
) {
    buffer_append(ctx, "%*u %s", (int)ctx->index_width, idx, SEPARATOR);
    append_name_cell(ctx, gpu->short_name);

    buffer_append(ctx, "%s", SEPARATOR);
    append_temp_cell(ctx, gpu->gpu_temp, GPU_TEMP_WARN, GPU_TEMP_DANGER);

    buffer_append(ctx, "%s", SEPARATOR);
    append_temp_cell(
        ctx,
        gpu->junction_temp,
        JUNCTION_TEMP_WARN,
        JUNCTION_TEMP_DANGER
    );

    buffer_append(ctx, "%s", SEPARATOR);
    append_temp_cell(ctx, gpu->vram_temp, VRAM_TEMP_WARN, VRAM_TEMP_DANGER);

    buffer_append(ctx, "%s\n", SEPARATOR);
}

static void monitor_temperatures_table(Context *ctx) {
    int row_count = 0;

    ctx->buffer_pos = 0;
    ctx->tick = !ctx->tick;

    append_table_header(ctx);

    for (unsigned int i = 0; i < ctx->device_count; i++) {
        if (!ctx->gpus[i].selected)
            continue;

        update_gpu_temps(&ctx->gpus[i]);
        append_table_row(ctx, i, &ctx->gpus[i]);
        row_count++;
    }

    if (ctx->output_mode == MODE_CONTINUOUS)
        buffer_append(ctx, "\033[%dA", row_count + 2);
    else
        buffer_append(ctx, "\n");

    printf("%s", ctx->output_buffer);
    fflush(stdout);
}

static void monitor_temperatures_json(Context *ctx) {
    struct timeval tv;
    int printed = 0;

    ctx->buffer_pos = 0;
    gettimeofday(&tv, NULL);

    buffer_append(
        ctx,
        "{\"timestamp\":%lld,\"gpus\":[",
        (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000
    );

    for (unsigned int i = 0; i < ctx->device_count; i++) {
        if (!ctx->gpus[i].selected)
            continue;

        GpuDevice *gpu = &ctx->gpus[i];
        update_gpu_temps(gpu);

        if (printed++ > 0)
            buffer_append(ctx, ",");

        buffer_append(ctx, "{\"index\":%u,", i);
        append_json_temp(ctx, "core", gpu->gpu_temp);
        buffer_append(ctx, ",");
        append_json_temp(ctx, "junction", gpu->junction_temp);
        buffer_append(ctx, ",");
        append_json_temp(ctx, "vram", gpu->vram_temp);
        buffer_append(ctx, "}");
    }

    buffer_append(ctx, "]}");
    printf("%s\n", ctx->output_buffer);
    fflush(stdout);
}


static int handle_input(int duration_ms) {
    if (duration_ms <= 0)
        return 0;

    struct timeval tv = {
        .tv_sec = duration_ms / 1000,
        .tv_usec = (duration_ms % 1000) * 1000
    };

    if (!isatty(STDIN_FILENO)) {
        select(0, NULL, NULL, NULL, &tv);
        return 0;
    }

    fd_set fds;
    char c;

    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0)
        return read(STDIN_FILENO, &c, 1) > 0;

    return 0;
}

static void finish_table(Context *ctx) {
    printf("\033[%uB\n", ctx->monitored_count + 2);
    fflush(stdout);
}

static void run_table_loop(Context *ctx) {
    while (running) {
        monitor_temperatures_table(ctx);

        if (handle_input(ctx->refresh_ms))
            break;
    }

    finish_table(ctx);
}

static void run_json_loop(Context *ctx) {
    while (running) {
        monitor_temperatures_json(ctx);

        if (handle_input(ctx->refresh_ms))
            break;
    }
}


static int parse_int_arg(const char *s, int *out) {
    char *end;
    long n;

    if (!s || *s == '\0')
        return -1;

    errno = 0;
    n = strtol(s, &end, 10);

    if (errno || end == s || *end != '\0' || n < 0 || n > INT_MAX)
        return -1;

    *out = (int)n;
    return 0;
}

static void print_usage(const char *prog) {
    fprintf(
        stderr,
        "Usage: %s [OPTIONS]\n\n"
        "Options:\n"
        "  --device <list>    Monitor selected devices: N, UUID, or BDF\n"
        "  --json             Output in JSON format\n"
        "  --once             Output once and exit\n"
        "  --refresh-ms <ms>  Polling interval in ms, minimum 50, default 1000\n"
        "  --help             Show this help and exit\n\n"
        "Environment:\n"
        "  NVIDIA_VISIBLE_DEVICES limits monitored GPUs when --device is not set\n\n"
        "Examples:\n"
        "  %s                   Display GPU temperature table\n"
        "  %s --device 0        Monitor only GPU 0\n"
        "  %s --device 0,2      Monitor GPUs 0 and 2\n"
        "  %s --refresh-ms 100  Refresh 10 times per second\n",
        prog,
        prog,
        prog,
        prog,
        prog
    );
}

static int parse_arguments(int argc, char *argv[], Context *ctx) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            ctx->output_format = FORMAT_JSON;

        } else if (strcmp(argv[i], "--once") == 0) {
            ctx->output_mode = MODE_ONCE;

        } else if (strcmp(argv[i], "--device") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --device requires a selector.\n");
                return -1;
            }

            ctx->selection_mode = SELECT_DEVICE;
            ctx->device_selector = argv[++i];

        } else if (strcmp(argv[i], "--refresh-ms") == 0) {
            if (i + 1 >= argc ||
                parse_int_arg(argv[++i], &ctx->refresh_ms) != 0 ||
                ctx->refresh_ms < 50) {
                fprintf(
                    stderr,
                    "Error: --refresh-ms requires a value >= 50.\n"
                );
                return -1;
            }

        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);

        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return -1;
        }
    }

    ctx->visible_devices = getenv(VISIBLE_DEVICES_ENV);

    if (ctx->selection_mode == SELECT_NONE &&
        ctx->visible_devices &&
        *ctx->visible_devices)
        ctx->selection_mode = SELECT_VISIBLE;

    return 0;
}


int main(int argc, char *argv[]) {
    Context ctx;

    page_size = sysconf(_SC_PAGE_SIZE);
    if (page_size <= 0)
        return 1;

    init_context_defaults(&ctx);

    if (parse_arguments(argc, argv, &ctx) < 0)
        return 1;

    if (init_monitoring(&ctx) < 0) {
        cleanup_context(&ctx);
        return 1;
    }

    if (ctx.output_format == FORMAT_TABLE &&
        ctx.output_mode == MODE_CONTINUOUS) {
        if (setup_terminal() < 0) {
            cleanup_context(&ctx);
            return 1;
        }

        printf(CURSOR_HIDE);
        fflush(stdout);
        atexit(restore_cursor);
    }

    if (ctx.output_format == FORMAT_JSON) {
        if (ctx.output_mode == MODE_ONCE)
            monitor_temperatures_json(&ctx);
        else
            run_json_loop(&ctx);
    } else {
        if (ctx.output_mode == MODE_ONCE)
            monitor_temperatures_table(&ctx);
        else
            run_table_loop(&ctx);
    }

    cleanup_context(&ctx);
    return 0;
}
