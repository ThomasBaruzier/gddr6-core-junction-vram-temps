#define _GNU_SOURCE

#include "monitor.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#define OUTPUT_BUFFER_SIZE 16384

#define DEFAULT_REFRESH_MS 1000
#define MIN_REFRESH_MS 50
#define MILLIDEGREES_PER_C 1000

#define CORE_WARN_C 70
#define CORE_DANGER_C 85
#define JUNCTION_WARN_C 80
#define JUNCTION_DANGER_C 95
#define VRAM_WARN_C 80
#define VRAM_DANGER_C 95

#define CUDA_VISIBLE_DEVICES_ENV "CUDA_VISIBLE_DEVICES"
#define NVIDIA_VISIBLE_DEVICES_ENV "NVIDIA_VISIBLE_DEVICES"

#define TABLE_SEPARATOR "\xE2\x94\x82"
#define CURSOR_HIDE "\x1B[?25l"
#define CURSOR_SHOW "\x1B[?25h"
#define COLOR_RESET "\x1B[0m"
#define COLOR_GREEN "\x1B[32m"
#define COLOR_YELLOW "\x1B[33m"
#define COLOR_RED "\x1B[31m"

typedef enum {
    OUTPUT_TABLE,
    OUTPUT_JSON
} OutputFormat;

typedef enum {
    RUN_CONTINUOUS,
    RUN_ONCE
} RunMode;

typedef struct {
    Monitor *monitor;
    MonitorOptions monitor_options;
    unsigned int gpu_count;
    unsigned int index_width;
    unsigned int name_width;
    bool show_marker;
    int refresh_ms;
    RunMode run_mode;
    OutputFormat output_format;
    char output[OUTPUT_BUFFER_SIZE];
    size_t output_length;
} Context;

static volatile sig_atomic_t running = 1;
static struct termios original_terminal;

static unsigned int max_uint(unsigned int a, unsigned int b)
{
    return a > b ? a : b;
}

static void init_context(Context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->monitor_options.selection_mode = MONITOR_SELECT_ALL;
    ctx->output_format = OUTPUT_TABLE;
    ctx->run_mode = RUN_CONTINUOUS;
    ctx->refresh_ms = DEFAULT_REFRESH_MS;
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

static void handle_stop_signal(int signum)
{
    (void)signum;
    running = 0;
}

static void restore_cursor(void)
{
    fputs(CURSOR_SHOW, stdout);
    fflush(stdout);
}

static void restore_terminal(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &original_terminal);
}

static int setup_terminal(void)
{
    struct termios terminal;

    if (tcgetattr(STDIN_FILENO, &original_terminal) < 0)
        return -1;

    atexit(restore_terminal);

    terminal = original_terminal;
    terminal.c_lflag &= ~(ICANON | ECHO);
    terminal.c_cc[VMIN] = 0;
    terminal.c_cc[VTIME] = 0;

    return tcsetattr(STDIN_FILENO, TCSANOW, &terminal) < 0 ? -1 : 0;
}

static void append_output(Context *ctx, const char *format, ...)
{
    char *destination;
    size_t available;
    int written;
    va_list args;

    if (ctx->output_length >= sizeof(ctx->output) - 1u)
        return;

    destination = ctx->output + ctx->output_length;
    available = sizeof(ctx->output) - ctx->output_length;

    va_start(args, format);
    written = vsnprintf(destination, available, format, args);
    va_end(args);

    if (written < 0)
        return;

    if ((size_t)written >= available)
        ctx->output_length = sizeof(ctx->output) - 1u;
    else
        ctx->output_length += (size_t)written;
}

static int temperature_celsius(Temperature temperature)
{
    return temperature.millidegrees / MILLIDEGREES_PER_C;
}

static const char *temperature_color(int celsius, int warn_c,
    int danger_c)
{
    if (celsius >= danger_c)
        return COLOR_RED;

    if (celsius >= warn_c)
        return COLOR_YELLOW;

    return COLOR_GREEN;
}

static unsigned int decimal_width(unsigned int value)
{
    unsigned int width = 1;

    while (value >= 10) {
        value /= 10;
        width++;
    }

    return width;
}

static void append_spaces(Context *ctx, unsigned int count)
{
    for (unsigned int i = 0; i < count; i++)
        append_output(ctx, " ");
}

static void append_padded(Context *ctx, const char *text,
    unsigned int width)
{
    unsigned int length = (unsigned int)strlen(text);

    append_output(ctx, "%s", text);

    if (length < width)
        append_spaces(ctx, width - length);
}

static void append_name_cell(Context *ctx, const char *name)
{
    append_output(ctx, " ");
    append_padded(ctx, name, ctx->name_width);
    append_output(ctx, " ");
}

static void append_temperature_header(Context *ctx, const char *name)
{
    append_output(ctx, "  %s  ", name);
}

static void append_temperature_cell(Context *ctx,
    Temperature temperature, int warn_c, int danger_c)
{
    const char *color;
    int celsius;

    if (!temperature.valid) {
        append_output(ctx, "  N/A   ");
        return;
    }

    celsius = temperature_celsius(temperature);
    color = temperature_color(celsius, warn_c, danger_c);
    append_output(ctx, " %s%3d°C%s  ", color, celsius, COLOR_RESET);
}

static void append_json_temperature(Context *ctx, const char *name,
    Temperature temperature)
{
    if (!temperature.valid) {
        append_output(ctx, "\"%s\":null", name);
        return;
    }

    append_output(ctx, "\"%s\":%d", name,
        temperature_celsius(temperature));
}

static void update_column_widths(Context *ctx)
{
    unsigned int max_index = 0;

    ctx->name_width = 3;

    for (unsigned int i = 0; i < ctx->gpu_count; i++) {
        const GpuReading *gpu = monitor_gpu(ctx->monitor, i);
        unsigned int name_width;

        if (!gpu)
            continue;

        name_width = (unsigned int)strlen(gpu->short_name);
        max_index = max_uint(max_index, gpu->index);
        ctx->name_width = max_uint(ctx->name_width, name_width);
    }

    ctx->index_width = decimal_width(max_index);
}

static int init_monitoring(Context *ctx)
{
    if (monitor_init(&ctx->monitor, &ctx->monitor_options) < 0)
        return -1;

    ctx->gpu_count = monitor_gpu_count(ctx->monitor);
    update_column_widths(ctx);

    signal(SIGINT, handle_stop_signal);
    signal(SIGTERM, handle_stop_signal);
    signal(SIGHUP, handle_stop_signal);

    return 0;
}

static void append_table_header(Context *ctx)
{
    const char *marker = ctx->show_marker ? "*" : "";

    append_output(ctx, "\n%*s %s", (int)ctx->index_width,
        marker, TABLE_SEPARATOR);
    append_name_cell(ctx, "GPU");

    append_output(ctx, "%s", TABLE_SEPARATOR);
    append_temperature_header(ctx, "CORE");

    append_output(ctx, "%s", TABLE_SEPARATOR);
    append_temperature_header(ctx, "JUNC");

    append_output(ctx, "%s", TABLE_SEPARATOR);
    append_temperature_header(ctx, "VRAM");

    append_output(ctx, "%s\n", TABLE_SEPARATOR);
}

static void append_table_row(Context *ctx, const GpuReading *gpu)
{
    append_output(ctx, "%*u %s", (int)ctx->index_width,
        gpu->index, TABLE_SEPARATOR);
    append_name_cell(ctx, gpu->short_name);

    append_output(ctx, "%s", TABLE_SEPARATOR);
    append_temperature_cell(ctx, gpu->core,
        CORE_WARN_C, CORE_DANGER_C);

    append_output(ctx, "%s", TABLE_SEPARATOR);
    append_temperature_cell(ctx, gpu->junction.hotspot,
        JUNCTION_WARN_C, JUNCTION_DANGER_C);

    append_output(ctx, "%s", TABLE_SEPARATOR);
    append_temperature_cell(ctx, gpu->vram.hottest,
        VRAM_WARN_C, VRAM_DANGER_C);

    append_output(ctx, "%s\n", TABLE_SEPARATOR);
}

static void print_table_sample(Context *ctx)
{
    int row_count = 0;

    ctx->output_length = 0;
    append_table_header(ctx);
    ctx->show_marker = !ctx->show_marker;
    monitor_sample(ctx->monitor);

    for (unsigned int i = 0; i < ctx->gpu_count; i++) {
        const GpuReading *gpu = monitor_gpu(ctx->monitor, i);

        if (!gpu)
            continue;

        append_table_row(ctx, gpu);
        row_count++;
    }

    if (ctx->run_mode == RUN_CONTINUOUS)
        append_output(ctx, "\033[%dA", row_count + 2);
    else
        append_output(ctx, "\n");

    printf("%s", ctx->output);
    fflush(stdout);
}

static void print_json_sample(Context *ctx)
{
    struct timeval now;
    long long timestamp;
    unsigned int emitted = 0;

    ctx->output_length = 0;
    gettimeofday(&now, NULL);
    timestamp = (long long)now.tv_sec * 1000 + now.tv_usec / 1000;

    append_output(ctx, "{\"timestamp\":%lld,\"gpus\":[", timestamp);
    monitor_sample(ctx->monitor);

    for (unsigned int i = 0; i < ctx->gpu_count; i++) {
        const GpuReading *gpu = monitor_gpu(ctx->monitor, i);

        if (!gpu)
            continue;

        if (emitted++ > 0)
            append_output(ctx, ",");

        append_output(ctx, "{\"index\":%u,", gpu->index);
        append_json_temperature(ctx, "core", gpu->core);
        append_output(ctx, ",");
        append_json_temperature(ctx, "junction",
            gpu->junction.hotspot);
        append_output(ctx, ",");
        append_json_temperature(ctx, "vram", gpu->vram.hottest);
        append_output(ctx, "}");
    }

    append_output(ctx, "]}");
    printf("%s\n", ctx->output);
    fflush(stdout);
}

static bool wait_for_input(int duration_ms)
{
    struct timeval timeout;
    fd_set read_fds;
    char key;
    int ready;

    if (duration_ms <= 0)
        return false;

    timeout.tv_sec = duration_ms / 1000;
    timeout.tv_usec = (duration_ms % 1000) * 1000;

    if (!isatty(STDIN_FILENO)) {
        select(0, NULL, NULL, NULL, &timeout);
        return false;
    }

    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);

    ready = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &timeout);

    if (ready > 0)
        return read(STDIN_FILENO, &key, 1) > 0;

    return false;
}

static void finish_table(Context *ctx)
{
    printf("\033[%uB\n", ctx->gpu_count + 2);
    fflush(stdout);
}

static void run_table(Context *ctx)
{
    while (running) {
        print_table_sample(ctx);

        if (wait_for_input(ctx->refresh_ms))
            break;
    }

    finish_table(ctx);
}

static void run_json(Context *ctx)
{
    while (running) {
        print_json_sample(ctx);

        if (wait_for_input(ctx->refresh_ms))
            break;
    }
}

static int parse_nonnegative_int(const char *text, int *output)
{
    char *end;
    long value;

    if (!text || !*text)
        return -1;

    errno = 0;
    value = strtol(text, &end, 10);

    if (errno || end == text || *end || value < 0 ||
        value > INT_MAX) {
        return -1;
    }

    *output = (int)value;
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
        "  --refresh-ms <ms>  Polling interval in ms, "
        "minimum %d, default %d\n"
        "  --help             Show this help and exit\n\n"
        "Environment:\n"
        "  CUDA_VISIBLE_DEVICES selects GPUs when --device is not set\n"
        "  NVIDIA_VISIBLE_DEVICES is used when "
        "CUDA_VISIBLE_DEVICES is unset\n\n"
        "Examples:\n"
        "  %s                   Display GPU temperature table\n"
        "  %s --device 0        Monitor only GPU 0\n"
        "  %s --device 0,2      Monitor GPUs 0 and 2\n"
        "  %s --refresh-ms 100  Refresh 10 times per second\n",
        program, MIN_REFRESH_MS, DEFAULT_REFRESH_MS,
        program, program, program, program
    );
}

static void apply_visible_devices_env(Context *ctx)
{
    const char *selectors;

    if (ctx->monitor_options.selection_mode != MONITOR_SELECT_ALL)
        return;

    selectors = getenv(CUDA_VISIBLE_DEVICES_ENV);

    if (!selectors)
        selectors = getenv(NVIDIA_VISIBLE_DEVICES_ENV);

    if (!selectors)
        return;

    ctx->monitor_options.selection_mode = MONITOR_SELECT_VISIBLE;
    ctx->monitor_options.selectors = *selectors ? selectors : "none";
}

static int parse_options(int argc, char *argv[], Context *ctx)
{
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--json") == 0) {
            ctx->output_format = OUTPUT_JSON;
        } else if (strcmp(arg, "--once") == 0) {
            ctx->run_mode = RUN_ONCE;
        } else if (strcmp(arg, "--device") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr,
                    "Error: --device requires a selector.\n");
                return -1;
            }

            ctx->monitor_options.selection_mode =
                MONITOR_SELECT_DEVICE;
            ctx->monitor_options.selectors = argv[++i];
        } else if (strcmp(arg, "--refresh-ms") == 0) {
            if (i + 1 >= argc ||
                parse_nonnegative_int(argv[++i],
                    &ctx->refresh_ms) != 0 ||
                ctx->refresh_ms < MIN_REFRESH_MS) {
                fprintf(stderr,
                    "Error: --refresh-ms requires a value >= %d.\n",
                    MIN_REFRESH_MS);
                return -1;
            }
        } else if (strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Unknown argument: %s\n", arg);
            print_usage(argv[0]);
            return -1;
        }
    }

    apply_visible_devices_env(ctx);
    return 0;
}

int main(int argc, char *argv[])
{
    Context context;

    init_context(&context);

    if (parse_options(argc, argv, &context) < 0)
        return 1;

    if (init_monitoring(&context) < 0) {
        cleanup_context(&context);
        return 1;
    }

    if (context.output_format == OUTPUT_TABLE &&
        context.run_mode == RUN_CONTINUOUS) {
        if (setup_terminal() < 0) {
            cleanup_context(&context);
            return 1;
        }

        fputs(CURSOR_HIDE, stdout);
        fflush(stdout);
        atexit(restore_cursor);
    }

    if (context.output_format == OUTPUT_JSON) {
        if (context.run_mode == RUN_ONCE)
            print_json_sample(&context);
        else
            run_json(&context);
    } else {
        if (context.run_mode == RUN_ONCE)
            print_table_sample(&context);
        else
            run_table(&context);
    }

    cleanup_context(&context);
    return 0;
}
