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

static void cmd_sp_clear(const char *line)
{
    (void)line;
    spectrum_clear();
    ESP_LOGI(TAG, "[sp] cleared");
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

static const cmd_entry_t s_sp_cmds[] = {
    {.prefix = "sp off ", .handler = cmd_sp_off},
    {.prefix = "sp clear", .handler = cmd_sp_clear},
    {.prefix = "sp view ", .handler = cmd_sp_view},
    {.prefix = "sp range ", .handler = cmd_sp_range},
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
        ESP_LOGI(TAG, "[sp] usage: sp off <dB> | sp clear | sp view <0|1> | sp range <floor> <ceil>");
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
