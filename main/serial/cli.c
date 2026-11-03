#include "serial/cli.h"

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
} cmd_entry_t;

static void cli_sp_off(const char *line)
{
    float val = 0.0f;
    if (sscanf(line, "sp off %f", &val) == 1)
    {
        (void)app_settings_set_level_offset_db(val);
        app_settings_t cfg;
        app_settings_get(&cfg);
        ESP_LOGI(TAG, "[sp] level_offset_db=%.1f", cfg.level_offset_db);
    }
}

static void cli_sp_view(const char *line)
{
    int mode = -1;
    if (sscanf(line, "sp view %d", &mode) == 1)
    {
        (void)app_settings_set_view_mode((uint8_t)mode);
        app_settings_t cfg;
        app_settings_get(&cfg);
        ESP_LOGI(TAG, "[sp] view=%u (0=full 1=if-centred)", (unsigned)cfg.view_mode);
    }
}

static void cli_sp_range(const char *line)
{
    float floor_db = 0.0f, ceil_db = 0.0f;
    if (sscanf(line, "sp range %f %f", &floor_db, &ceil_db) == 2)
    {
        (void)app_settings_set_wf_range(floor_db, ceil_db);
        app_settings_t cfg;
        app_settings_get(&cfg);
        ESP_LOGI(TAG, "[sp] range=%.1f..%.1f dB", cfg.wf_floor_db, cfg.wf_ceil_db);
    }
    else
    {
        ESP_LOGI(TAG, "[sp] usage: sp range <floor_dB> <ceil_dB>");
    }
}

static void cli_sp_cwbfo(const char *line)
{
    int hz = 0;
    if (sscanf(line, "sp cwbfo %d", &hz) == 1)
    {
        (void)app_settings_set_cw_pitch_hz((uint16_t)hz);
        app_settings_t cfg;
        app_settings_get(&cfg);
        ESP_LOGI(TAG, "[sp] cwbfo=%u Hz", (unsigned)cfg.cw_pitch_hz);
    }
    else
    {
        ESP_LOGI(TAG, "[sp] usage: sp cwbfo <400..1000>");
    }
}

static void cli_sp_palette(const char *line)
{
    int pal = 0;
    if (sscanf(line, "sp palette %d", &pal) == 1)
    {
        (void)app_settings_set_wf_palette((uint8_t)pal);
        app_settings_t cfg;
        app_settings_get(&cfg);
        ESP_LOGI(TAG, "[sp] palette=%u (0=turbo 1=gray 2=inferno 3=viridis 4=magma 5=plasma 6=sdr_bluehot)",
                 (unsigned)cfg.wf_palette);
    }
    else
    {
        ESP_LOGI(TAG, "[sp] usage: sp palette <0..6>");
    }
}

static void cli_sp_speed(const char *line)
{
    int level = 0;
    if (sscanf(line, "sp speed %d", &level) == 1)
    {
        (void)app_settings_set_speed_level((uint8_t)level);
        app_settings_t cfg;
        app_settings_get(&cfg);
        ESP_LOGI(TAG, "[sp] speed=%u/%u (%.1f lines/s, %.1f s of history)",
                 (unsigned)cfg.speed_level, (unsigned)spectrum_speed_level_count(),
                 spectrum_speed_lps_from_level(cfg.speed_level),
                 (float)(76 - 25) / spectrum_speed_lps_from_level(cfg.speed_level));
    }
    else
    {
        ESP_LOGI(TAG, "[sp] usage: sp speed <1..%u> (1 = slowest, %u = fastest)",
                 (unsigned)spectrum_speed_level_count(),
                 (unsigned)spectrum_speed_level_count());
    }
}

static void cli_ui_lang(const char *line)
{
    char arg[16] = {0};
    if (sscanf(line, "ui lang %15s", arg) == 1)
    {
        uint8_t lang = 0;
        if (!ui_lang_parse(arg, &lang))
        {
            ESP_LOGI(TAG, "[ui] usage: ui lang <en|pl>");
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

static const char *const s_spec_mode_name[] = {"trace", "trace+peak", "peak", "off"};

static void cli_sp_mode(const char *line)
{
    char arg[16] = {0};
    if (sscanf(line, "sp mode %15s", arg) == 1)
    {
        int mode = -1;
        for (int i = 0; i < 4; i++)
        {
            if (strcmp(arg, s_spec_mode_name[i]) == 0)
                mode = i;
        }
        if (mode < 0 && sscanf(arg, "%d", &mode) != 1)
            mode = -1;

        if (mode >= 0 && mode <= 3)
        {
            (void)app_settings_set_spec_mode((uint8_t)mode);
            app_settings_t cfg;
            app_settings_get(&cfg);
            ESP_LOGI(TAG, "[sp] mode=%s", s_spec_mode_name[cfg.spec_mode]);
            return;
        }
        ESP_LOGI(TAG, "[sp] usage: sp mode <trace|trace+peak|peak|off>");
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
             st.level_period_rows / 93.75f * 1000.0f);
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
        ESP_LOGI(TAG, "[sp] usage: sp log <tag> <none|error|warn|info|debug|verbose>");
        return;
    }

    esp_log_level_t level = ESP_LOG_INFO;
    if (strcmp(lvl, "none") == 0)
        level = ESP_LOG_NONE;
    else if (strcmp(lvl, "error") == 0)
        level = ESP_LOG_ERROR;
    else if (strcmp(lvl, "warn") == 0)
        level = ESP_LOG_WARN;
    else if (strcmp(lvl, "debug") == 0)
        level = ESP_LOG_DEBUG;
    else if (strcmp(lvl, "verbose") == 0)
        level = ESP_LOG_VERBOSE;

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
    ESP_LOGI(TAG, "[sp] now: off=%.1f view=%u range=%.1f..%.1f cwbfo=%u pal=%u speed=%u/%u mode=%s fft=%s",
             cfg.level_offset_db, (unsigned)cfg.view_mode, cfg.wf_floor_db, cfg.wf_ceil_db,
             (unsigned)cfg.cw_pitch_hz, (unsigned)cfg.wf_palette,
             (unsigned)cfg.speed_level, (unsigned)spectrum_speed_level_count(),
             s_spec_mode_name[cfg.spec_mode < 4 ? cfg.spec_mode : 0],
             spectrum_get_fft_impl() ? "aes3" : "ansi");
    ESP_LOGI(TAG, "[sp] usage: sp off <dB> | sp view <0|1> | sp range <floor> <ceil> | sp cwbfo <400..1000> | "
                  "sp palette <0..6> | sp speed <1..N> | sp mode <trace|trace+peak|peak|off> | sp stats [reset] | "
                  "sp fft <ansi|aes3> | sp ffttest | sp iqdump | sp shot | sp log <tag> <level> | sp save | sp reset");
}

static const cmd_entry_t s_sp_cmds[] = {
    {.prefix = "ui lang", .handler = cli_ui_lang},
    {.prefix = "sp stats reset", .handler = cli_sp_stats_reset},
    {.prefix = "sp stats", .handler = cli_sp_stats},
    {.prefix = "sp save", .handler = cli_sp_save},
    {.prefix = "sp reset", .handler = cli_sp_reset},
    {.prefix = "sp off ", .handler = cli_sp_off},
    {.prefix = "sp view ", .handler = cli_sp_view},
    {.prefix = "sp range ", .handler = cli_sp_range},
    {.prefix = "sp cwbfo ", .handler = cli_sp_cwbfo},
    {.prefix = "sp palette ", .handler = cli_sp_palette},
    {.prefix = "sp speed ", .handler = cli_sp_speed},
    {.prefix = "sp mode", .handler = cli_sp_mode},
    {.prefix = "sp ffttest", .handler = cli_sp_fft_test},
    {.prefix = "sp fft ", .handler = cli_sp_fft},
    {.prefix = "sp iqdump", .handler = cli_sp_iqdump},
    {.prefix = "sp shot", .handler = cli_sp_shot},
    {.prefix = "sp level", .handler = cli_sp_level},
    {.prefix = "sp log ", .handler = cli_sp_log},
};

static void handle_cli_line(const char *line)
{
    for (size_t i = 0; i < sizeof(s_sp_cmds) / sizeof(s_sp_cmds[0]); i++)
    {
        size_t n = strlen(s_sp_cmds[i].prefix);
        if (strncmp(line, s_sp_cmds[i].prefix, n) == 0)
        {
            s_sp_cmds[i].handler(line);
            return;
        }
    }

    if (strncmp(line, "sp", 2) == 0)
    {
        cli_sp_status(line);
    }
}

static void monitor_cli_task(void *arg)
{
    (void)arg;
    uint8_t rx[64];
    char line[CLI_BUF_SZ];
    size_t line_len = 0;

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
                if (line_len > 0)
                {
                    line[line_len] = '\0';
                    handle_cli_line(line);
                    line_len = 0;
                }
                continue;
            }
            if (line_len + 1 < sizeof(line))
            {
                line[line_len++] = c;
            }
            else
            {
                line_len = 0;
            }
        }
    }
}

void serial_cli_start(void)
{
    /* TX ring buffer: without it, large writes spin on the FIFO instead of
     * blocking (the `sp shot` dump is ~86 KB). */
    uart_driver_install(CLI_UART_NUM, 1024, 4096, 0, NULL, 0);
    xTaskCreate(monitor_cli_task, "mon_cli", 4096, NULL, 1, NULL);
}
