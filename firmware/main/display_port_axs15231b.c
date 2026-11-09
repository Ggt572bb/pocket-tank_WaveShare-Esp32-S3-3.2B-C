/* Waveshare ESP32-S3-Touch-LCD-3.5B-C. Pinout and panel sequence from
 * waveshareteam/ESP32-S3-Touch-LCD-3.5B (Apache-2.0; see vendor notice).
 * Send every frame from native row zero: AXS15231B QSPI cannot address
 * arbitrary rows. Transfer one internal DMA stripe at a time. */
#include "display_port.h"
#include "tank.h"
#include "lcd35b_geometry.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_axs15231b.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd35b_init.inc"

#define ROWS 16
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_ex, s_pmu;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_done;
static uint16_t *s_stripe;
static uint8_t s_brightness = 153;
static bool s_inverted, s_ready, s_lit, s_asleep;
static uint8_t s_ex_out;
static const char *TAG = "lcd35b";

static void wr(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t v) {
    uint8_t b[] = {reg, v}; ESP_ERROR_CHECK(i2c_master_transmit(dev, b, 2, 100));
}
static uint8_t rd(i2c_master_dev_handle_t dev, uint8_t reg) {
    uint8_t v; ESP_ERROR_CHECK(i2c_master_transmit_receive(dev, &reg, 1, &v, 1, 100)); return v;
}
static i2c_master_dev_handle_t device(uint8_t address) {
    i2c_master_dev_handle_t dev;
    i2c_device_config_t c = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = address, .scl_speed_hz = 400000};
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &c, &dev)); return dev;
}
i2c_master_bus_handle_t board_i2c_bus(void) { return s_bus; }
bool board_is_v2(void) { return false; }
bool board_is_round(void) { return false; }
bool board_is_175(void) { return false; }
bool board_is_watch(void) { return false; }
bool board_is_square(void) { return false; }
bool board_has_expander(void) { return true; }
bool board_port_probed(void) { return s_ready; }
const char *board_port_hw(void) { return s_ready ? "lcd35b" : NULL; }
int board_pwr_sense_pin(void) { return -1; } /* PWR is EXIO6, PMIC IRQ polling handles it */
void board_sd_select(bool on) { (void)on; } /* this board uses SDMMC, no SPI CS */

static bool IRAM_ATTR transferred(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *arg) {
    BaseType_t woke = pdFALSE; xSemaphoreGiveFromISR(s_done, &woke); return woke == pdTRUE;
}
static void backlight(uint8_t level) {
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, level * 1023u / 255u));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
}
bool display_port_init(void) {
    i2c_master_bus_config_t bc = {.i2c_port = I2C_NUM_0, .sda_io_num = 8, .scl_io_num = 7,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &s_bus));
    s_pmu = device(0x34);
    /* Preserve factory rail voltages and enables. Keep audio analog at 3.3V
       (ALDO1) and DCDC1 in PWM, as the vendor's flicker fix specifies.
       Disable the factory application's PMIC watchdog: our task WDT owns it. */
    wr(s_pmu, 0x18, rd(s_pmu, 0x18) & ~(1u << 0));
    wr(s_pmu, 0x81, rd(s_pmu, 0x81) | (1u << 2));
    wr(s_pmu, 0x92, (rd(s_pmu, 0x92) & 0xe0) | 28); /* ALDO1: 500 + 28*100 mV */
    wr(s_pmu, 0x90, rd(s_pmu, 0x90) | 1u);
    s_ex = device(0x20);
    s_ex_out = rd(s_ex, 1);
    wr(s_ex, 1, s_ex_out & ~2u);
    wr(s_ex, 3, (rd(s_ex, 3) | (1u << 6)) & ~2u);
    vTaskDelay(pdMS_TO_TICKS(100));
    s_ex_out |= 2u; wr(s_ex, 1, s_ex_out);
    vTaskDelay(pdMS_TO_TICKS(200));
    ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t light = {.gpio_num = 6, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0};
    ESP_ERROR_CHECK(ledc_channel_config(&light));
    s_done = xSemaphoreCreateBinary();
    s_stripe = heap_caps_malloc(LCD35B_W * ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_done || !s_stripe) { ESP_LOGE(TAG, "no LCD DMA memory"); return false; }
    spi_bus_config_t bus = {.sclk_io_num = 5, .data0_io_num = 1, .data1_io_num = 2,
        .data2_io_num = 3, .data3_io_num = 4, .max_transfer_sz = LCD35B_W * ROWS * 2};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t ic = AXS15231B_PANEL_IO_QSPI_CONFIG(12, transferred, NULL);
    ic.pclk_hz = 40 * 1000 * 1000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ic, &io));
    axs15231b_vendor_config_t vendor = {.init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]), .flags.use_qspi_interface = 1};
    esp_lcd_panel_dev_config_t pc = {.reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16, .vendor_config = &vendor};
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(io, &pc, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* esp_lcd_axs15231b 1.0.0 implements this argument as "off", unlike
       ESP-IDF's API name. Keep this dependency pinned: false = DISPON. */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, false));
    s_ready = true;
    ESP_LOGI(TAG, "320x480 AXS15231B: landscape fit, original %dx%d tank", TANK_W, TANK_H);
    return true;
}
void display_port_flush(const uint16_t *fb) {
    if (!s_ready || s_asleep) return;
    for (int y = 0; y < LCD35B_H; y += ROWS) {
        for (int j = 0; j < ROWS; j++) for (int x = 0; x < LCD35B_W; x++) {
            int tx, ty; uint16_t c = 0;
            if (lcd35b_point(x, y+j, &tx, &ty)) {
                if (s_inverted) { tx = TANK_W-1-tx; ty = TANK_H-1-ty; }
                c = fb[ty * TANK_W + tx]; /* PT_FB_SWAP: already in panel byte order */
            }
            s_stripe[j * LCD35B_W + x] = c;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD35B_W, y+ROWS, s_stripe));
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(1000)) != pdTRUE) ESP_ERROR_CHECK(ESP_ERR_TIMEOUT);
    }
    if (!s_lit) { s_lit = true; backlight(s_brightness); }
}
void display_port_set_brightness(uint8_t v) { s_brightness = v; if (s_lit && !s_asleep) backlight(v); }
uint8_t display_port_brightness(void) { return s_brightness; }
void display_port_sleep(void) { backlight(0); s_asleep = true; ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true)); }
void display_port_wake(void) { s_asleep = false; s_lit = false; ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, false)); }
void display_port_set_inverted(bool v) { s_inverted = v; }
void display_port_set_quarter(int q) { s_inverted = (q & 2) != 0; }
bool display_port_quarters(void) { return false; }
void display_port_side_tune(int m, int x, int y) { (void)m; (void)x; (void)y; }
void display_port_side_info(int *m, int *x, int *y, bool *on) { *m = *x = *y = 0; *on = false; }
void display_port_probe(bool on) { (void)on; }
void display_port_set_madctl(uint8_t v) { (void)v; }
uint8_t display_port_madctl(void) { return 0; }
void display_port_frame_origin(int *x, int *y) { *x = *y = 0; }
void display_port_set_view(int v) { (void)v; }
int display_port_view(void) { return DISPLAY_VIEW_FIT; }
void display_port_panel_to_tank(int x, int y, float *tx, float *ty) {
    int a = -1, b = -1; lcd35b_point(x, y, &a, &b); *tx = a; *ty = b;
}
void display_port_deep_sleep_pins(bool on) { (void)on; }
void display_port_deep_sleep_bus(void) { }
void display_port_deep_standby(void) { display_port_sleep(); }
void display_port_flush_prof(int64_t *w, int64_t *s) { *w = *s = 0; }
bool display_port_flush_async(const uint16_t *fb, void (*done)(const uint16_t *, void *), void *arg) { return false; }
void display_port_flush_wait(const uint16_t *fb) { }
void display_port_flush_async_prof(int64_t *us, int *n, int64_t *copy) { *us = *copy = 0; *n = 0; }
