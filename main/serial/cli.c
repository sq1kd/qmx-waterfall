#include "serial/cli.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "spectrum/spectrum.h"

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
        spectrum_set_level_offset_db(val);
        ESP_LOGI(TAG, "[sp] level_offset_db=%.1f", val);
    }
}

static void cmd_sp_view(const char *line)
{
    int mode = -1;
    if (sscanf(line, "sp view %d", &mode) == 1)
    {
        if (mode < 0)
            mode = 0;
        if (mode > 1)
            mode = 1;
        spectrum_set_view_mode((uint8_t)mode);
        ESP_LOGI(TAG, "[sp] view=%d (0=full 1=pos-only)", mode);
    }
}

static void cmd_sp_range(const char *line)
{
    float floor_db = 0.0f, ceil_db = 0.0f;
    if (sscanf(line, "sp range %f %f", &floor_db, &ceil_db) == 2)
    {
        spectrum_set_wf_range(floor_db, ceil_db);
        ESP_LOGI(TAG, "[sp] range=%.1f..%.1f dB", floor_db, ceil_db);
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
        if (hz < 400)
            hz = 400;
        if (hz > 1000)
            hz = 1000;
        spectrum_set_cw_pitch_hz((uint16_t)hz);
        ESP_LOGI(TAG, "[sp] cwbfo=%d Hz", hz);
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
        spectrum_set_wf_gamma(gamma);
        ESP_LOGI(TAG, "[sp] gamma=%.2f", gamma);
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
        if (pal < 0)
            pal = 0;
        if (pal > 5)
            pal = 5;
        spectrum_set_wf_palette((uint8_t)pal);
        ESP_LOGI(TAG, "[sp] palette=%d (0=turbo 1=gray 2=inferno 3=viridis 4=magma 5=plasma)", pal);
    }
    else
    {
        ESP_LOGI(TAG, "[sp] usage: sp palette <0..5>");
    }
}

static void cmd_sp_alpha(const char *line)
{
    float a = 0.25f;
    if (sscanf(line, "sp alpha %f", &a) == 1)
    {
        spectrum_set_spec_alpha(a);
        ESP_LOGI(TAG, "[sp] alpha=%.2f", a);
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
        spectrum_set_wf_condense(f);
        ESP_LOGI(TAG, "[sp] wfcondense=%.2f", f);
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
        spectrum_set_wf_temporal_beta(b);
        ESP_LOGI(TAG, "[sp] wfbeta=%.2f", b);
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
        if (en < 0)
            en = 0;
        if (en > 1)
            en = 1;
        spectrum_set_wf_bilinear(en != 0);
        ESP_LOGI(TAG, "[sp] wfbilinear=%d", en);
    }
    else
    {
        ESP_LOGI(TAG, "[sp] usage: sp wfbilinear <0|1>");
    }
}

static const cmd_entry_t s_sp_cmds[] = {
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
        float off = 0.0f;
        float wf_floor = 0.0f;
        float wf_ceil = 0.0f;
        float gamma = 1.0f;
        float alpha = 0.25f;
        float wfcondense = 2.0f;
        float wfbeta = 0.0f;
        uint8_t view = 0;
        uint8_t palette = 4;
        uint16_t cwbfo = 700;
        bool wfbilinear = true;
        spectrum_get_runtime(&off, &view, &wf_floor, &wf_ceil, &cwbfo,
                             &gamma, &palette, &alpha, &wfcondense, &wfbeta, &wfbilinear);
        ESP_LOGI(TAG, "[sp] now: off=%.1f view=%u range=%.1f..%.1f cwbfo=%u gamma=%.2f pal=%u alpha=%.2f wfcondense=%.2f wfbeta=%.2f wfbilinear=%u",
                 off, (unsigned)view, wf_floor, wf_ceil, (unsigned)cwbfo,
                 gamma, (unsigned)palette, alpha, wfcondense, wfbeta, wfbilinear ? 1 : 0);
        ESP_LOGI(TAG, "[sp] usage: sp off <dB> | sp view <0|1> | sp range <floor> <ceil> | sp cwbfo <400..1000> | sp gamma <0.5..2.5> | sp palette <0..5> | sp alpha <0.01..1.0> | sp wfcondense <1.0..3.0> | sp wfbeta <0.0..0.5> | sp wfbilinear <0|1>");
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
