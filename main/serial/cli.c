#include "serial/cli.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "app/settings.h"
#include "spectrum/spectrum.h"
#include "ui/i18n.h"

#define CLI_UART_NUM UART_NUM_0
#define CLI_BUF_SZ 128

static const char *TAG = "serial_cli";

typedef void (*cmd_handler_t)(const char *line);

typedef struct
{
    const char *prefix;
    cmd_handler_t handler;
    const char *help;
    int field; /* app_field_id_t whose range is appended, -1 for none */
} cmd_entry_t;

#define CLI_NO_FIELD (-1)

static void cli_usage(const char *prefix);
static void cli_print_usage(void);

/* A value outside the descriptor range (including NaN) is rejected rather
 * than silently clamped, so `sp view 257` cannot become view 1. */
static bool cli_field_in_range(app_field_id_t id, float value)
{
    return app_settings_field_clamp(id, value) == value;
}

static bool cli_apply_field(app_field_id_t id, float value)
{
    if (!cli_field_in_range(id, value))
        return false;
    return app_settings_field_set(id, value) == ESP_OK;
}

static void cli_sp_off(const char *line)
{
    float value = 0.0f;
    if (sscanf(line, "sp off %f", &value) != 1 || !cli_apply_field(APP_FIELD_LEVEL_OFF, value))
    {
        cli_usage("sp off");
        return;
    }

    app_settings_t cfg;
    app_settings_get(&cfg);
    ESP_LOGI(TAG, "[sp] level_offset_db=%.1f", cfg.level_offset_db);
}

static void cli_sp_view(const char *line)
{
    long mode = 0;
    if (sscanf(line, "sp view %ld", &mode) != 1 || !cli_apply_field(APP_FIELD_VIEW_MODE, (float)mode))
    {
        cli_usage("sp view");
        return;
    }

    app_settings_t cfg;
    app_settings_get(&cfg);
    ESP_LOGI(TAG, "[sp] view=%u (0=full 1=if-centred)", (unsigned)cfg.view_mode);
}

static void cli_sp_range(const char *line)
{
    float floor_db = 0.0f, ceil_db = 0.0f;
    if (sscanf(line, "sp range %f %f", &floor_db, &ceil_db) != 2 ||
        !cli_field_in_range(APP_FIELD_WF_FLOOR, floor_db) ||
        !cli_field_in_range(APP_FIELD_WF_CEIL, ceil_db) ||
        floor_db >= ceil_db)
    {
        cli_usage("sp range");
        return;
    }

    (void)app_settings_set_wf_range(floor_db, ceil_db);
    app_settings_t cfg;
    app_settings_get(&cfg);
    ESP_LOGI(TAG, "[sp] range=%.1f..%.1f dB", cfg.wf_floor_db, cfg.wf_ceil_db);
}

static void cli_sp_cwbfo(const char *line)
{
    long hz = 0;
    if (sscanf(line, "sp cwbfo %ld", &hz) != 1 || !cli_apply_field(APP_FIELD_CW_PITCH, (float)hz))
    {
        cli_usage("sp cwbfo");
        return;
    }

    app_settings_t cfg;
    app_settings_get(&cfg);
    ESP_LOGI(TAG, "[sp] cwbfo=%u Hz", (unsigned)cfg.cw_pitch_hz);
}

static void cli_sp_palette(const char *line)
{
    long pal = 0;
    if (sscanf(line, "sp palette %ld", &pal) != 1 || !cli_apply_field(APP_FIELD_PALETTE, (float)pal))
    {
        cli_usage("sp palette");
        return;
    }

    app_settings_t cfg;
    app_settings_get(&cfg);
    ESP_LOGI(TAG, "[sp] palette=%u (0=turbo 1=gray 2=inferno 3=viridis 4=magma 5=plasma 6=sdr_bluehot)",
             (unsigned)cfg.wf_palette);
}

static void cli_sp_gamma(const char *line)
{
    float gamma = 0.0f;
    if (sscanf(line, "sp gamma %f", &gamma) != 1 || !cli_apply_field(APP_FIELD_GAMMA, gamma))
    {
        cli_usage("sp gamma");
        return;
    }

    app_settings_t cfg;
    app_settings_get(&cfg);
    ESP_LOGI(TAG, "[sp] gamma=%.2f", cfg.wf_gamma);
}

static void cli_sp_speed(const char *line)
{
    long level = 0;
    if (sscanf(line, "sp speed %ld", &level) != 1 || !cli_apply_field(APP_FIELD_SPEED, (float)level))
    {
        cli_usage("sp speed");
        return;
    }

    app_settings_t cfg;
    app_settings_get(&cfg);
    ESP_LOGI(TAG, "[sp] speed=%u/%u (%.1f lines/s, %.1f s of history)",
             (unsigned)cfg.speed_level, (unsigned)spectrum_speed_level_count(),
             spectrum_speed_lps_from_level(cfg.speed_level),
             spectrum_history_seconds());
}

static void cli_ui_lang(const char *line)
{
    char arg[16] = {0};
    if (sscanf(line, "ui lang %15s", arg) == 1)
    {
        uint8_t lang = 0;
        if (!ui_lang_parse(arg, &lang))
        {
            cli_usage("ui lang");
            return;
        }
        esp_err_t err = app_settings_set_language(lang);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "[ui] language update failed: %s", esp_err_to_name(err));
            return;
        }
    }

    app_settings_t cfg;
    app_settings_get(&cfg);
    ESP_LOGI(TAG, "[ui] lang=%s", ui_lang_code((ui_lang_t)cfg.language));
}

enum
{
    SPEC_MODE_COUNT = 4,
};

static const char *const s_spec_mode_name[SPEC_MODE_COUNT] = {"trace", "trace+peak", "peak", "off"};

static void cli_sp_mode(const char *line)
{
    char arg[16] = {0};
    if (sscanf(line, "sp mode %15s", arg) == 1)
    {
        int mode = -1;
        for (int i = 0; i < SPEC_MODE_COUNT; i++)
        {
            if (strcmp(arg, s_spec_mode_name[i]) == 0)
                mode = i;
        }
        if (mode < 0 && sscanf(arg, "%d", &mode) != 1)
            mode = -1;

        if (mode >= 0 && mode < SPEC_MODE_COUNT)
        {
            (void)app_settings_field_set(APP_FIELD_SPEC_MODE, (float)mode);
            app_settings_t cfg;
            app_settings_get(&cfg);
            ESP_LOGI(TAG, "[sp] mode=%s", s_spec_mode_name[cfg.spec_mode]);
            return;
        }
        cli_usage("sp mode");
        return;
    }

    app_settings_t cfg;
    app_settings_get(&cfg);
    ESP_LOGI(TAG, "[sp] mode=%s", s_spec_mode_name[cfg.spec_mode]);
}

static void cli_sp_stats(const char *line)
{
    spectrum_stats_t st;
    spectrum_get_stats(&st);

    uint64_t expected = (uint64_t)st.sample_freq * st.frame_bytes * st.run_us / 1000000ULL;
    int deficits_x100 = 0;
    if (expected > 0)
    {
        uint64_t deficit = expected > st.bytes_read ? expected - st.bytes_read : 0;
        deficits_x100 = (int)((deficit * 10000ULL) / expected);
    }

    float fft_rate = spectrum_fft_rate();
    float period_ms = fft_rate > 0.0f ? st.level_period_rows / fft_rate * 1000.0f : 0.0f;

    ESP_LOGI(TAG, "[sp] stats: fft=%lu lines=%lu (%lu fft/line) draws=%lu skipped=%lu",
             (unsigned long)st.fft_frames, (unsigned long)st.lines,
             (unsigned long)st.line_group, (unsigned long)st.draws,
             (unsigned long)st.draws_skipped);
    ESP_LOGI(TAG, "[sp] audio: read=%lluB in %lluB expected (%d.%02d%% missing), calls=%lu, %lums audio / %lums wall",
             (unsigned long long)st.bytes_read, (unsigned long long)expected,
             deficits_x100 / 100, deficits_x100 % 100,
             (unsigned long)st.read_calls,
             (unsigned long)(st.audio_us / 1000), (unsigned long)(st.run_us / 1000));
    ESP_LOGI(TAG, "[sp] draw: last=%luus avg=%luus max=%luus, lock wait max=%luus",
             (unsigned long)st.draw_us_last, (unsigned long)st.draw_us_avg,
             (unsigned long)st.draw_us_max, (unsigned long)st.lock_wait_us_max);
    ESP_LOGI(TAG, "[sp] level: min=%.1f max=%.1f std=%.2f dB, ripple %.2f dB @%.0f fft-frames (%.1f ms)",
             st.level_min_db, st.level_max_db, st.level_std_db,
             st.level_period_db, st.level_period_rows,
             period_ms);
    ESP_LOGI(TAG, "[sp] health: internal=%luB (largest %luB) psram=%luB stack_free=%luB",
             (unsigned long)st.heap_internal_free,
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned long)st.heap_psram_free, (unsigned long)st.stack_free_words);
    (void)line;
}

static void cli_sp_stats_reset(const char *line)
{
    spectrum_reset_stats();
    ESP_LOGI(TAG, "[sp] stats reset");
    (void)line;
}

static void cli_sp_fft(const char *line)
{
    char impl[8] = {0};
    if (sscanf(line, "sp fft %7s", impl) == 1)
    {
        if (strcmp(impl, "ansi") != 0 && strcmp(impl, "aes3") != 0)
        {
            cli_usage("sp fft");
            return;
        }
        uint8_t want = (strcmp(impl, "aes3") == 0) ? 1 : 0;
        esp_err_t err = spectrum_set_fft_impl(want);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "[sp] fft %s unavailable: %s", impl, esp_err_to_name(err));
        }
    }
    else
    {
        ESP_LOGI(TAG, "[sp] fft=%s (sp fft <ansi|aes3>, then sp ffttest)",
                 spectrum_get_fft_impl() ? "aes3" : "ansi");
    }
}

static void cli_sp_fft_test(const char *line)
{
    esp_err_t err = spectrum_fft_selftest();
    ESP_LOGI(TAG, "[sp] ffttest: %s", esp_err_to_name(err));
    (void)line;
}

/* Raw I/Q capture: 512 frames of up to 8 bytes each, plus the 284 column dB
 * values the firmware derives from the same block. Buffers live in .bss so the
 * CLI task stack stays small. */
#define IQ_MAX_FRAMES 512
#define IQ_MAX_FRAME_BYTES 8
static uint8_t s_iq_raw[IQ_MAX_FRAMES * IQ_MAX_FRAME_BYTES];
static int16_t s_iq_db[SPECTRUM_IQ_COLUMNS];

static void cli_sp_iqdump(const char *line)
{
    (void)line;
    if (!spectrum_capture_iq_block())
    {
        ESP_LOGW(TAG, "[sp] iqdump: spectrum not running");
        return;
    }

    spectrum_iq_block_info_t info = {0};
    uint32_t frames = 0;
    for (int i = 0; i < 100; i++)
    {
        frames = spectrum_get_iq_block(&info, s_iq_raw, sizeof(s_iq_raw), s_iq_db, SPECTRUM_IQ_COLUMNS);
        if (frames > 0)
            break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (frames == 0)
    {
        ESP_LOGW(TAG, "[sp] iqdump: no block captured");
        return;
    }

    ESP_LOGI(TAG, "IQ %lu %lu %u %u", (unsigned long)frames, (unsigned long)info.frame_bytes,
             (unsigned)info.fft_size, (unsigned)info.view_mode);

    char buf[200];
    size_t len = 0;
    buf[0] = '\0';
    for (uint32_t i = 0; i < frames * info.frame_bytes; i++)
    {
        len += (size_t)snprintf(buf + len, sizeof(buf) - len, "%02X", s_iq_raw[i]);
        if (len >= 160)
        {
            ESP_LOGI(TAG, "IQDATA %s", buf);
            len = 0;
            buf[0] = '\0';
        }
    }
    if (len > 0)
        ESP_LOGI(TAG, "IQDATA %s", buf);

    len = 0;
    buf[0] = '\0';
    for (int i = 0; i < SPECTRUM_IQ_COLUMNS; i++)
    {
        len += (size_t)snprintf(buf + len, sizeof(buf) - len, "%d ", s_iq_db[i]);
        if (len >= 160)
        {
            ESP_LOGI(TAG, "DB %s", buf);
            len = 0;
            buf[0] = '\0';
        }
    }
    if (len > 0)
        ESP_LOGI(TAG, "DB %s", buf);
}

/* Dump the exact canvas we push to the panel: 284x76 RGB565 as hex, so a PC
 * can reconstruct the image without a camera in the loop. */
static void cli_sp_shot(const char *line)
{
    (void)line;
    if (!spectrum_is_running())
    {
        ESP_LOGW(TAG, "[sp] shot: spectrum not running");
        return;
    }

    const uint32_t pixels = SPECTRUM_IQ_COLUMNS * SPECTRUM_DISPLAY_HEIGHT;
    /* The internal heap is fragmented by the canvas/ring allocations, so a
     * 43 KB contiguous block is not guaranteed there: PSRAM is fine for a
     * one-shot diagnostic copy. */
    uint16_t *buf = heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf)
        buf = heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf)
    {
        ESP_LOGW(TAG, "[sp] shot: no memory (largest internal %uB)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return;
    }
    if (!spectrum_copy_canvas(buf, pixels))
    {
        heap_caps_free(buf);
        ESP_LOGW(TAG, "[sp] shot: lock failed");
        return;
    }

    spectrum_stats_t st;
    spectrum_get_stats(&st);
    ESP_LOGI(TAG, "SHOT %u %u RGB565 fft=%s lines=%lu/%lu draws=%lu",
             (unsigned)SPECTRUM_IQ_COLUMNS, (unsigned)SPECTRUM_DISPLAY_HEIGHT,
             spectrum_get_fft_impl() ? "aes3" : "ansi",
             (unsigned long)st.lines, (unsigned long)st.fft_frames,
             (unsigned long)st.draws);

    const uint8_t *raw = (const uint8_t *)buf;
    char out[512];
    size_t len = 0;
    unsigned flushes = 0;
    out[0] = '\0';
    for (uint32_t i = 0; i < pixels * 2u; i++)
    {
        len += (size_t)snprintf(out + len, sizeof(out) - len, "%02X", raw[i]);
        if (len >= sizeof(out) - 3)
        {
            ESP_LOGI(TAG, "SHOTDATA %s", out);
            len = 0;
            out[0] = '\0';
            /* 86 KB at 115200 takes ~10 s. Yielding every 8th line keeps the
             * idle task alive (a sub-tick delay would be a no-op at 100 Hz). */
            if (++flushes % 8 == 0)
                vTaskDelay(1);
        }
    }
    if (len > 0)
        ESP_LOGI(TAG, "SHOTDATA %s", out);
    ESP_LOGI(TAG, "SHOTEND");

    heap_caps_free(buf);
}

/* Per-line mean level history: timestamp, level. Lets us look at the actual
 * waveform instead of a single DFT peak. */
static float s_hist_db[SPECTRUM_LEVEL_HIST_MAX];
static uint32_t s_hist_ms[SPECTRUM_LEVEL_HIST_MAX];

static void cli_sp_level(const char *line)
{
    (void)line;
    uint32_t n = spectrum_get_level_history(s_hist_db, s_hist_ms, SPECTRUM_LEVEL_HIST_MAX);
    if (n == 0)
    {
        ESP_LOGW(TAG, "[sp] level: history unavailable");
        return;
    }

    char buf[192];
    size_t len = 0;
    buf[0] = '\0';
    for (uint32_t i = 0; i < n; i++)
    {
        len += (size_t)snprintf(buf + len, sizeof(buf) - len, "%lu:%.2f ", (unsigned long)s_hist_ms[i],
                                (double)s_hist_db[i]);
        if (len >= sizeof(buf) - 24)
        {
            ESP_LOGI(TAG, "LEVEL %s", buf);
            len = 0;
            buf[0] = '\0';
        }
    }
    if (len > 0)
        ESP_LOGI(TAG, "LEVEL %s", buf);
    ESP_LOGI(TAG, "LEVELEND");
}

static void cli_sp_log(const char *line)
{
    char tag[32] = {0};
    char lvl[16] = {0};
    if (sscanf(line, "sp log %31s %15s", tag, lvl) != 2)
    {
        cli_usage("sp log");
        return;
    }

    esp_log_level_t level;
    if (strcmp(lvl, "none") == 0)
        level = ESP_LOG_NONE;
    else if (strcmp(lvl, "error") == 0)
        level = ESP_LOG_ERROR;
    else if (strcmp(lvl, "warn") == 0)
        level = ESP_LOG_WARN;
    else if (strcmp(lvl, "info") == 0)
        level = ESP_LOG_INFO;
    else if (strcmp(lvl, "debug") == 0)
        level = ESP_LOG_DEBUG;
    else if (strcmp(lvl, "verbose") == 0)
        level = ESP_LOG_VERBOSE;
    else
    {
        cli_usage("sp log");
        return;
    }

    esp_log_level_set(tag, level);
    ESP_LOGI(TAG, "[sp] log %s=%s", tag, lvl);
}

static void cli_sp_save(const char *line)
{
    (void)line;
    esp_err_t err = app_settings_flush_now();
    ESP_LOGI(TAG, "[sp] save: %s", esp_err_to_name(err));
}

static void cli_sp_reset(const char *line)
{
    (void)line;
    esp_err_t err = app_settings_reset_to_defaults(true);
    ESP_LOGI(TAG, "[sp] reset: %s", esp_err_to_name(err));
}

static void cli_sp_status(const char *line)
{
    (void)line;
    app_settings_t cfg;
    app_settings_get(&cfg);
    ESP_LOGI(TAG, "[sp] now: off=%.1f view=%u range=%.1f..%.1f gamma=%.2f cwbfo=%u pal=%u speed=%u/%u mode=%s fft=%s",
             cfg.level_offset_db, (unsigned)cfg.view_mode, cfg.wf_floor_db, cfg.wf_ceil_db,
             cfg.wf_gamma, (unsigned)cfg.cw_pitch_hz, (unsigned)cfg.wf_palette,
             (unsigned)cfg.speed_level, (unsigned)spectrum_speed_level_count(),
             s_spec_mode_name[cfg.spec_mode < SPEC_MODE_COUNT ? cfg.spec_mode : 0],
             spectrum_get_fft_impl() ? "aes3" : "ansi");
}

static const cmd_entry_t s_sp_cmds[] = {
    {.prefix = "ui lang", .handler = cli_ui_lang, .help = "ui lang <en|pl>", .field = CLI_NO_FIELD},
    {.prefix = "sp status", .handler = cli_sp_status, .help = "sp status", .field = CLI_NO_FIELD},
    {.prefix = "sp stats reset", .handler = cli_sp_stats_reset, .help = "sp stats reset", .field = CLI_NO_FIELD},
    {.prefix = "sp stats", .handler = cli_sp_stats, .help = "sp stats", .field = CLI_NO_FIELD},
    {.prefix = "sp save", .handler = cli_sp_save, .help = "sp save", .field = CLI_NO_FIELD},
    {.prefix = "sp reset", .handler = cli_sp_reset, .help = "sp reset", .field = CLI_NO_FIELD},
    {.prefix = "sp off", .handler = cli_sp_off, .help = "sp off <dB>", .field = APP_FIELD_LEVEL_OFF},
    {.prefix = "sp view", .handler = cli_sp_view, .help = "sp view <mode>", .field = APP_FIELD_VIEW_MODE},
    {.prefix = "sp range", .handler = cli_sp_range, .help = "sp range <floor> <ceil> dB", .field = CLI_NO_FIELD},
    {.prefix = "sp cwbfo", .handler = cli_sp_cwbfo, .help = "sp cwbfo <Hz>", .field = APP_FIELD_CW_PITCH},
    {.prefix = "sp palette", .handler = cli_sp_palette, .help = "sp palette <index>", .field = APP_FIELD_PALETTE},
    {.prefix = "sp gamma", .handler = cli_sp_gamma, .help = "sp gamma <value>", .field = APP_FIELD_GAMMA},
    {.prefix = "sp speed", .handler = cli_sp_speed, .help = "sp speed <level>", .field = APP_FIELD_SPEED},
    {.prefix = "sp mode", .handler = cli_sp_mode, .help = "sp mode <trace|trace+peak|peak|off>", .field = CLI_NO_FIELD},
    {.prefix = "sp ffttest", .handler = cli_sp_fft_test, .help = "sp ffttest", .field = CLI_NO_FIELD},
    {.prefix = "sp fft", .handler = cli_sp_fft, .help = "sp fft <ansi|aes3>", .field = CLI_NO_FIELD},
    {.prefix = "sp iqdump", .handler = cli_sp_iqdump, .help = "sp iqdump", .field = CLI_NO_FIELD},
    {.prefix = "sp shot", .handler = cli_sp_shot, .help = "sp shot", .field = CLI_NO_FIELD},
    {.prefix = "sp level", .handler = cli_sp_level, .help = "sp level", .field = CLI_NO_FIELD},
    {.prefix = "sp log", .handler = cli_sp_log, .help = "sp log <tag> <none|error|warn|info|debug|verbose>", .field = CLI_NO_FIELD},
};

#define CLI_CMD_COUNT (sizeof(s_sp_cmds) / sizeof(s_sp_cmds[0]))

static const char *cli_usage_tag(const char *prefix)
{
    return strncmp(prefix, "ui", 2) == 0 ? "[ui]" : "[sp]";
}

static void cli_log_help(const cmd_entry_t *cmd)
{
    if (cmd->field >= 0)
    {
        app_field_id_t id = (app_field_id_t)cmd->field;
        const app_field_spec_t *spec = app_settings_field_spec(id);
        float min = app_settings_field_clamp(id, -1.0e9f);
        float max = app_settings_field_clamp(id, 1.0e9f);
        if (spec && spec->step > 0.0f && spec->step < 1.0f)
            ESP_LOGI(TAG, "%s usage: %s (%.2f..%.2f)",
                     cli_usage_tag(cmd->prefix), cmd->help, min, max);
        else
            ESP_LOGI(TAG, "%s usage: %s (%.0f..%.0f)",
                     cli_usage_tag(cmd->prefix), cmd->help, min, max);
        return;
    }
    ESP_LOGI(TAG, "%s usage: %s", cli_usage_tag(cmd->prefix), cmd->help);
}

static void cli_usage(const char *prefix)
{
    for (size_t i = 0; i < CLI_CMD_COUNT; i++)
    {
        if (strcmp(s_sp_cmds[i].prefix, prefix) == 0)
        {
            cli_log_help(&s_sp_cmds[i]);
            return;
        }
    }
}

static void cli_print_usage(void)
{
    for (size_t i = 0; i < CLI_CMD_COUNT; i++)
        cli_log_help(&s_sp_cmds[i]);
}

static void handle_cli_line(const char *line)
{
    for (size_t i = 0; i < CLI_CMD_COUNT; i++)
    {
        size_t n = strlen(s_sp_cmds[i].prefix);
        if (strncmp(line, s_sp_cmds[i].prefix, n) == 0 && (line[n] == '\0' || line[n] == ' '))
        {
            s_sp_cmds[i].handler(line);
            return;
        }
    }

    if (strcmp(line, "sp") == 0)
    {
        cli_sp_status(line);
        cli_print_usage();
        return;
    }

    if (strncmp(line, "sp ", 3) == 0)
    {
        ESP_LOGW(TAG, "[sp] unknown command: %s", line);
        cli_print_usage();
    }
}

static void monitor_cli_task(void *arg)
{
    (void)arg;
    uint8_t rx[64];
    char line[CLI_BUF_SZ];
    size_t line_len = 0;
    bool discard = false;

    ESP_LOGI(TAG, "UART CLI ready on UART0. Commands: sp ... | ui lang <en|pl>");
    while (true)
    {
        int n = uart_read_bytes(CLI_UART_NUM, rx, sizeof(rx), pdMS_TO_TICKS(50));
        if (n <= 0)
        {
            continue;
        }

        for (int i = 0; i < n; i++)
        {
            char c = (char)rx[i];
            if (c == '\r' || c == '\n')
            {
                if (!discard && line_len > 0)
                {
                    line[line_len] = '\0';
                    handle_cli_line(line);
                }
                line_len = 0;
                discard = false;
                continue;
            }
            if (discard)
                continue;
            if (line_len + 1 < sizeof(line))
                line[line_len++] = c;
            else
            {
                /* Restarting mid-line would run a command assembled from the
                 * tail; drop the rest of this line instead. */
                line_len = 0;
                discard = true;
            }
        }
    }
}

void serial_cli_start(void)
{
    /* TX ring buffer: without it, large writes spin on the FIFO instead of
     * blocking (the `sp shot` dump is ~86 KB). */
    esp_err_t err = uart_driver_install(CLI_UART_NUM, 1024, 4096, 0, NULL, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "uart driver install failed: %s", esp_err_to_name(err));
        return;
    }

    if (xTaskCreate(monitor_cli_task, "mon_cli", 4096, NULL, 1, NULL) != pdTRUE)
    {
        ESP_LOGE(TAG, "failed to create CLI task");
    }
}
