#include <stdio.h>
#include "driver/i2c.h"

#define I2C_PORT    I2C_NUM_0
#define SDA_PIN     GPIO_NUM_8
#define SCL_PIN     GPIO_NUM_9
#define SCD41_ADDR 0x62

void i2c_init(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SDA_PIN,
        .scl_io_num = SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, cfg.mode, 0, 0, 0));
}

esp_err_t scd41_write_command(uint16_t cmd)
{
    uint8_t data[2];

    data[0] = cmd >> 8;
    data[1] = cmd & 0xFF;

    return i2c_master_write_to_device(
            I2C_PORT,
            SCD41_ADDR,
            data,
            sizeof(data),
            pdMS_TO_TICKS(1000));
}

void app_main(void)
{

}
