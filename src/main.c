#define _GNU_SOURCE

#include "sensors.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>

#define BUFFER_SIZE 16384

#define GPU_TEMP_WARN 70
#define GPU_TEMP_DANGER 85
#define JUNCTION_TEMP_WARN 80
#define JUNCTION_TEMP_DANGER 95
#define VRAM_TEMP_WARN 80
#define VRAM_TEMP_DANGER 95

#define CUDA_VISIBLE_DEVICES_ENV "CUDA_VISIBLE_DEVICES"
#define NVIDIA_VISIBLE_DEVICES_ENV "NVIDIA_VISIBLE_DEVICES"

#define SEPARATOR "\xE2\x94\x82"
#define CURSOR_HIDE "\x1B[?25l"
#define CURSOR_SHOW "\x1B[?25h"
#define COLOR_RESET "\x1B[0m"
#define COLOR_GREEN "\x1B[32m"
#define COLOR_YELLOW "\x1B[33m"
#define COLOR_RED "\x1B[31m"

typedef enum {
    FORMAT_TABLE,
    FORMAT_JSON
} OutputFormat;

typedef enum {
    MODE_CONTINUOUS,
    MODE_ONCE
} OutputMode;

typedef enum {
    SELECT_NONE,
    SELECT_DEVICE,
    SELECT_VISIBLE
} SelectionMode;

typedef struct {
    Monitor *monitor;
    unsigned int monitored_count;
    unsigned int index_width;
    unsigned int name_width;
    int tick;
    int refresh_ms;
    OutputMode output_mode;
    OutputFormat output_format;
    SelectionMode selection_mode;
    const char *device_selector;
    const char *visible_devices;
    char output_buffer[BUFFER_SIZE];
    size_t buffer_pos;
} Context;

static volatile sig_atomic_t running = 1;
static struct termios original_terminal;

static unsigned int max_uint(unsigned int a, unsigned int b)
{
    return a > b ? a : b;
}

static void init_context_defaults(Context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->output_format = FORMAT_TABLE;
    ctx->output_mode = MODE_CONTINUOUS;
    ctx->refresh_ms = 1000;
    ctx->index_width = 1;
    ctx->name_width = 3;
}

static void cleanup_context(Context *ctx)
{
    if (!ctx)
        return;

    monitor_destroy(ctx->monitor);
    ctx->monitor = NULL;
}

static void signal_handler(int signum)
{
    (void)signum;
    running = 0;
}

static void restore_cursor(void)
{
    fputs(CURSOR_SHOW, stdout);
    fflush(stdout);
}

static void reset_terminal(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &original_terminal);
}

static int setup_terminal(void)
{
    struct termios terminal;

    if (tcgetattr(STDIN_FILENO, &original_terminal) < 0)
        return -1;

    atexit(reset_terminal);

    terminal = original_terminal;
    terminal.c_lflag &= ~(ICANON | ECHO);
    terminal.c_cc[VMIN] = 0;
    terminal.c_cc[VTIME] = 0;

    return tcsetattr(STDIN_FILENO, TCSANOW, &terminal) < 0 ? -1 : 0;
}

static void buffer_append(Context *ctx, const char *format, ...)
{
    int remaining;
    int written;
    va_list args;

    if (ctx->buffer_pos >= BUFFER_SIZE - 1)
        return;

    remaining = (int)(BUFFER_SIZE - ctx->buffer_pos);

    va_start(args, format);
    written = vsnprintf(
        ctx->output_buffer + ctx->buffer_pos,
        (size_t)remaining,
        format,
        args
    );
    va_end(args);

    if (written < 0)
        return;

    if (written >= remaining)
        ctx->buffer_pos = BUFFER_SIZE - 1;
    else
        ctx->buffer_pos += (size_t)written;
}

static int temperature_celsius(Temperature temperature)
{
    return temperature.millidegrees / 1000;
}

static const char *get_temp_color(int temperature, int warning, int danger)
{
    if (temperature >= danger)
        return COLOR_RED;

    if (temperature >= warning)
        return COLOR_YELLOW;

    return COLOR_GREEN;
}

static unsigned int digits_uint(unsigned int value)
{
    unsigned int digits = 1;

    while (value >= 10) {
        value /= 10;
        digits++;
    }

    return digits;
}

static void append_spaces(Context *ctx, unsigned int count)
{
    for (unsigned int i = 0; i < count; i++)
        buffer_append(ctx, " ");
}

static void append_padded(Context *ctx, const char *text, unsigned int width)
{
    unsigned int length = (unsigned int)strlen(text);

    buffer_append(ctx, "%s", text);

    if (length < width)
        append_spaces(ctx, width - length);
}

static void append_name_cell(Context *ctx, const char *name)
{
    buffer_append(ctx, " ");
    append_padded(ctx, name, ctx->name_width);
    buffer_append(ctx, " ");
}

static void append_temp_header_cell(Context *ctx, const char *name)
{
    buffer_append(ctx, "  %s  ", name);
}

static void append_temp_cell(
    Context *ctx,
    Temperature temperature,
    int warning,
    int danger
)
{
    int celsius;

    if (!temperature.valid) {
        buffer_append(ctx, "  N/A   ");
        return;
    }

    celsius = temperature_celsius(temperature);

    buffer_append(
        ctx,
        " %s%3d°C%s  ",
        get_temp_color(celsius, warning, danger),
        celsius,
        COLOR_RESET
    );
}

static void append_json_temp(
    Context *ctx,
    const char *name,
    Temperature temperature
)
{
    if (temperature.valid) {
        buffer_append(
            ctx,
            "\"%s\":%d",
            name,
            temperature_celsius(temperature)
        );
    } else {
        buffer_append(ctx, "\"%s\":null", name);
    }
}

static void update_column_widths(Context *ctx)
{
    unsigned int max_index = 0;

    ctx->name_width = 3;

    for (unsigned int i = 0; i < ctx->monitored_count; i++) {
        const GpuReading *gpu = monitor_gpu(ctx->monitor, i);

        if (!gpu)
            continue;

        max_index = gpu->index;
        ctx->name_width = max_uint(
            ctx->name_width,
            (unsigned int)strlen(gpu->short_name)
        );
    }

    ctx->index_width = digits_uint(max_index);
}

static int init_monitoring(Context *ctx)
{
    MonitorOptions options = {
        .selection_mode = MONITOR_SELECT_ALL,
        .selectors = NULL
    };

    if (ctx->selection_mode == SELECT_DEVICE) {
        options.selection_mode = MONITOR_SELECT_DEVICE;
        options.selectors = ctx->device_selector;
    } else if (ctx->selection_mode == SELECT_VISIBLE) {
        options.selection_mode = MONITOR_SELECT_VISIBLE;
        options.selectors = ctx->visible_devices;
    }

    if (monitor_init(&ctx->monitor, &options) < 0)
        return -1;

    ctx->monitored_count = monitor_gpu_count(ctx->monitor);
    update_column_widths(ctx);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);

    return 0;
}

static void append_table_header(Context *ctx)
{
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

static void append_table_row(Context *ctx, const GpuReading *gpu)
{
    buffer_append(
        ctx,
        "%*u %s",
        (int)ctx->index_width,
        gpu->index,
        SEPARATOR
    );

    append_name_cell(ctx, gpu->short_name);

    buffer_append(ctx, "%s", SEPARATOR);
    append_temp_cell(ctx, gpu->core, GPU_TEMP_WARN, GPU_TEMP_DANGER);

    buffer_append(ctx, "%s", SEPARATOR);
    append_temp_cell(
        ctx,
        gpu->junction.hotspot,
        JUNCTION_TEMP_WARN,
        JUNCTION_TEMP_DANGER
    );

    buffer_append(ctx, "%s", SEPARATOR);
    append_temp_cell(
        ctx,
        gpu->vram.hottest,
        VRAM_TEMP_WARN,
        VRAM_TEMP_DANGER
    );

    buffer_append(ctx, "%s\n", SEPARATOR);
}

static void monitor_temperatures_table(Context *ctx)
{
    int row_count = 0;

    ctx->buffer_pos = 0;
    ctx->tick = !ctx->tick;

    append_table_header(ctx);
    monitor_sample(ctx->monitor);

    for (unsigned int i = 0; i < ctx->monitored_count; i++) {
        const GpuReading *gpu = monitor_gpu(ctx->monitor, i);

        if (!gpu)
            continue;

        append_table_row(ctx, gpu);
        row_count++;
    }

    if (ctx->output_mode == MODE_CONTINUOUS)
        buffer_append(ctx, "\033[%dA", row_count + 2);
    else
        buffer_append(ctx, "\n");

    printf("%s", ctx->output_buffer);
    fflush(stdout);
}

static void monitor_temperatures_json(Context *ctx)
{
    struct timeval time;
    int printed = 0;

    ctx->buffer_pos = 0;
    gettimeofday(&time, NULL);

    buffer_append(
        ctx,
        "{\"timestamp\":%lld,\"gpus\":[",
        (long long)time.tv_sec * 1000 + time.tv_usec / 1000
    );

    monitor_sample(ctx->monitor);

    for (unsigned int i = 0; i < ctx->monitored_count; i++) {
        const GpuReading *gpu = monitor_gpu(ctx->monitor, i);

        if (!gpu)
            continue;

        if (printed++ > 0)
            buffer_append(ctx, ",");

        buffer_append(ctx, "{\"index\":%u,", gpu->index);
        append_json_temp(ctx, "core", gpu->core);
        buffer_append(ctx, ",");
        append_json_temp(ctx, "junction", gpu->junction.hotspot);
        buffer_append(ctx, ",");
        append_json_temp(ctx, "vram", gpu->vram.hottest);
        buffer_append(ctx, "}");
    }

    buffer_append(ctx, "]}");
    printf("%s\n", ctx->output_buffer);
    fflush(stdout);
}

static int handle_input(int duration_ms)
{
    struct timeval timeout;
    fd_set descriptors;
    char input;

    if (duration_ms <= 0)
        return 0;

    timeout.tv_sec = duration_ms / 1000;
    timeout.tv_usec = (duration_ms % 1000) * 1000;

    if (!isatty(STDIN_FILENO)) {
        select(0, NULL, NULL, NULL, &timeout);
        return 0;
    }

    FD_ZERO(&descriptors);
    FD_SET(STDIN_FILENO, &descriptors);

    if (select(
            STDIN_FILENO + 1,
            &descriptors,
            NULL,
            NULL,
            &timeout
        ) > 0) {
        return read(STDIN_FILENO, &input, 1) > 0;
    }

    return 0;
}

static void finish_table(Context *ctx)
{
    printf("\033[%uB\n", ctx->monitored_count + 2);
    fflush(stdout);
}

static void run_table_loop(Context *ctx)
{
    while (running) {
        monitor_temperatures_table(ctx);

        if (handle_input(ctx->refresh_ms))
            break;
    }

    finish_table(ctx);
}

static void run_json_loop(Context *ctx)
{
    while (running) {
        monitor_temperatures_json(ctx);

        if (handle_input(ctx->refresh_ms))
            break;
    }
}

static int parse_int_arg(const char *text, int *out)
{
    char *end;
    long value;

    if (!text || *text == '\0')
        return -1;

    errno = 0;
    value = strtol(text, &end, 10);

    if (errno ||
        end == text ||
        *end != '\0' ||
        value < 0 ||
        value > INT_MAX) {
        return -1;
    }

    *out = (int)value;
    return 0;
}

static void print_usage(const char *program)
{
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
        "  CUDA_VISIBLE_DEVICES selects GPUs when --device is not set\n"
        "  NVIDIA_VISIBLE_DEVICES is used when CUDA_VISIBLE_DEVICES is unset\n\n"
        "Examples:\n"
        "  %s                   Display GPU temperature table\n"
        "  %s --device 0        Monitor only GPU 0\n"
        "  %s --device 0,2      Monitor GPUs 0 and 2\n"
        "  %s --refresh-ms 100  Refresh 10 times per second\n",
        program,
        program,
        program,
        program,
        program
    );
}

static void apply_visible_devices_environment(Context *ctx)
{
    const char *selectors;

    if (ctx->selection_mode != SELECT_NONE)
        return;

    selectors = getenv(CUDA_VISIBLE_DEVICES_ENV);

    if (!selectors)
        selectors = getenv(NVIDIA_VISIBLE_DEVICES_ENV);

    if (!selectors)
        return;

    ctx->visible_devices = *selectors ? selectors : "none";
    ctx->selection_mode = SELECT_VISIBLE;
}

static int parse_arguments(int argc, char *argv[], Context *ctx)
{
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

    apply_visible_devices_environment(ctx);
    return 0;
}

int main(int argc, char *argv[])
{
    Context context;

    init_context_defaults(&context);

    if (parse_arguments(argc, argv, &context) < 0)
        return 1;

    if (init_monitoring(&context) < 0) {
        cleanup_context(&context);
        return 1;
    }

    if (context.output_format == FORMAT_TABLE &&
        context.output_mode == MODE_CONTINUOUS) {
        if (setup_terminal() < 0) {
            cleanup_context(&context);
            return 1;
        }

        fputs(CURSOR_HIDE, stdout);
        fflush(stdout);
        atexit(restore_cursor);
    }

    if (context.output_format == FORMAT_JSON) {
        if (context.output_mode == MODE_ONCE)
            monitor_temperatures_json(&context);
        else
            run_json_loop(&context);
    } else {
        if (context.output_mode == MODE_ONCE)
            monitor_temperatures_table(&context);
        else
            run_table_loop(&context);
    }

    cleanup_context(&context);
    return 0;
}
