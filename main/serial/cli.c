#include "serial/cli.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "app/settings.h"

#define CLI_UART_NUM UART_NUM_0
#define CLI_BUF_SZ 128

static const char *TAG = "serial_cli";

typedef void (*cmd_handler_t)(const char *line);

typedef struct
{
    const char *prefix;
    cmd_handler_t handler;
} cmd_entry_t;

static void cmd_sp_off(const char *line)
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

static void cmd_sp_view(const char *line)
{
    int mode = -1;
    if (sscanf(line, "sp view %d", &mode) == 1)
    {
        (void)app_settings_set_view_mode((uint8_t)mode);
        app_settings_t cfg;
        app_settings_get(&cfg);
        ESP_LOGI(TAG, "[sp] view=%u (0=full 1=pos-only)", (unsigned)cfg.view_mode);
    }
}

static void cmd_sp_range(const char *line)
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

static void cmd_sp_cwbfo(const char *line)
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

static void cmd_sp_gamma(const char *line)
{
    float gamma = 1.0f;
    if (sscanf(line, "sp gamma %f", &gamma) == 1)
    {
        (void)app_settings_set_wf_gamma(gamma);
        app_settings_t cfg;
        app_settings_get(&cfg);
        ESP_LOGI(TAG, "[sp] gamma=%.2f", cfg.wf_gamma);
    }
    else
    {
        ESP_LOGI(TAG, "[sp] usage: sp gamma <0.5..2.5>");
    }
}

static void cmd_sp_palette(const char *line)
{
    int pal = 0;
    if (sscanf(line, "sp palette %d", &pal) == 1)
    {
        (void)app_settings_set_wf_palette((uint8_t)pal);
        app_settings_t cfg;
        app_settings_get(&cfg);
        ESP_LOGI(TAG, "[sp] palette=%u (0=turbo 1=gray 2=inferno 3=viridis 4=magma 5=plasma 6=sdr_bluehot)", (unsigned)cfg.wf_palette);
    }
    else
    {
        ESP_LOGI(TAG, "[sp] usage: sp palette <0..6>");
    }
}

static void cmd_sp_alpha(const char *line)
{
    float a = 0.25f;
    if (sscanf(line, "sp alpha %f", &a) == 1)
    {
        (void)app_settings_set_spec_alpha(a);
        app_settings_t cfg;
        app_settings_get(&cfg);
        ESP_LOGI(TAG, "[sp] alpha=%.2f", cfg.spec_alpha);
    }
    else
    {
        ESP_LOGI(TAG, "[sp] usage: sp alpha <0.01..1.0>");
    }
}

static void cmd_sp_wfcondense(const char *line)
{
    float f = 2.0f;
    if (sscanf(line, "sp wfcondense %f", &f) == 1)
    {
        (void)app_settings_set_wf_condense(f);
        app_settings_t cfg;
        app_settings_get(&cfg);
        ESP_LOGI(TAG, "[sp] wfcondense=%.2f", cfg.wf_condense);
    }
    else
    {
        ESP_LOGI(TAG, "[sp] usage: sp wfcondense <1.0..3.0>");
    }
}

static void cmd_sp_wfbeta(const char *line)
{
    float b = 0.0f;
    if (sscanf(line, "sp wfbeta %f", &b) == 1)
    {
        (void)app_settings_set_wf_temporal_beta(b);
        app_settings_t cfg;
        app_settings_get(&cfg);
        ESP_LOGI(TAG, "[sp] wfbeta=%.2f", cfg.wf_temporal_beta);
    }
    else
    {
        ESP_LOGI(TAG, "[sp] usage: sp wfbeta <0.0..0.5>");
    }
}

static void cmd_sp_wfbilinear(const char *line)
{
    int en = 0;
    if (sscanf(line, "sp wfbilinear %d", &en) == 1)
    {
        (void)app_settings_set_wf_bilinear(en != 0);
        app_settings_t cfg;
        app_settings_get(&cfg);
        ESP_LOGI(TAG, "[sp] wfbilinear=%u", cfg.wf_bilinear ? 1u : 0u);
    }
    else
    {
        ESP_LOGI(TAG, "[sp] usage: sp wfbilinear <0|1>");
    }
}

static void cmd_sp_save(const char *line)
{
    (void)line;
    esp_err_t err = app_settings_flush_now();
    ESP_LOGI(TAG, "[sp] save: %s", esp_err_to_name(err));
}

static void cmd_sp_reset(const char *line)
{
    (void)line;
    esp_err_t err = app_settings_reset_to_defaults(true);
    ESP_LOGI(TAG, "[sp] reset: %s", esp_err_to_name(err));
}

static const cmd_entry_t s_sp_cmds[] = {
    {.prefix = "sp save", .handler = cmd_sp_save},
    {.prefix = "sp reset", .handler = cmd_sp_reset},
    {.prefix = "sp off ", .handler = cmd_sp_off},
    {.prefix = "sp view ", .handler = cmd_sp_view},
    {.prefix = "sp range ", .handler = cmd_sp_range},
    {.prefix = "sp cwbfo ", .handler = cmd_sp_cwbfo},
    {.prefix = "sp gamma ", .handler = cmd_sp_gamma},
    {.prefix = "sp palette ", .handler = cmd_sp_palette},
    {.prefix = "sp alpha ", .handler = cmd_sp_alpha},
    {.prefix = "sp wfcondense ", .handler = cmd_sp_wfcondense},
    {.prefix = "sp wfbeta ", .handler = cmd_sp_wfbeta},
    {.prefix = "sp wfbilinear ", .handler = cmd_sp_wfbilinear},
};

static void handle_cli_line(const char *line)
{
    size_t i;

    for (i = 0; i < sizeof(s_sp_cmds) / sizeof(s_sp_cmds[0]); i++)
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
        app_settings_t cfg;
        app_settings_get(&cfg);
        ESP_LOGI(TAG, "[sp] now: off=%.1f view=%u range=%.1f..%.1f cwbfo=%u gamma=%.2f pal=%u alpha=%.2f wfcondense=%.2f wfbeta=%.2f wfbilinear=%u",
                 cfg.level_offset_db, (unsigned)cfg.view_mode, cfg.wf_floor_db, cfg.wf_ceil_db, (unsigned)cfg.cw_pitch_hz,
                 cfg.wf_gamma, (unsigned)cfg.wf_palette, cfg.spec_alpha, cfg.wf_condense, cfg.wf_temporal_beta, cfg.wf_bilinear ? 1 : 0);
        ESP_LOGI(TAG, "[sp] usage: sp off <dB> | sp view <0|1> | sp range <floor> <ceil> | sp cwbfo <400..1000> | sp gamma <0.5..2.5> | sp palette <0..6> | sp alpha <0.01..1.0> | sp wfcondense <1.0..3.0> | sp wfbeta <0.0..0.5> | sp wfbilinear <0|1> | sp save | sp reset");
    }
}

static void monitor_cli_task(void *arg)
{
    (void)arg;
    uint8_t rx[64];
    char line[CLI_BUF_SZ];
    size_t line_len = 0;

    ESP_LOGI(TAG, "UART CLI ready on UART0. Commands: sp ...");
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
    uart_driver_install(CLI_UART_NUM, 1024, 0, 0, NULL, 0);
    xTaskCreate(monitor_cli_task, "mon_cli", 4096, NULL, 1, NULL);
}
