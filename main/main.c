#include <stdio.h>
#include "driver/i2c.h"

#define I2C_PORT    I2C_NUM_0
#define SDA_PIN     GPIO_NUM_8
#define SCL_PIN     GPIO_NUM_9
#define SCD41_ADDR 0x62

void i2c_init(void) {
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

esp_err_t scd41_write_command(uint16_t cmd) {
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

uint8_t crc8(const uint8_t *data, int len) {
    uint8_t crc = 0xFF;

    while(len--) {
        crc ^= *data++;

        for(int i=0;i<8;i++) {
            if(crc & 0x80) {
                crc = (crc << 1) ^ 0x31;
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

void app_main(void)
{
    uint8_t buffer[9];

    i2c_init();

    //Start periodic measurement
    ESP_ERROR_CHECK(scd41_write_command(0x21B1));
    vTaskDelay(pdMS_TO_TICKS(5000));

    //Send the Read Measurement command
    ESP_ERROR_CHECK(scd41_write_command(0xEC05));
    vTaskDelay(pdMS_TO_TICKS(10));

    //Read 9 bytes
    ESP_ERROR_CHECK(i2c_master_read_from_device(I2C_PORT, SCD41_ADDR, buffer, sizeof(buffer), pdMS_TO_TICKS(1000)));

    //Check CRC for CO
    if(crc8(buffer, 2) != buffer[2]) {
        printf("CO2 CRC ERROR\n");
    }

    //Check CRC for temperature
    if(crc8(buffer, 5) != buffer[5]) {
        printf("Temperature CRC ERROR\n");
    }

    //Check CRC for humidity
    if(crc8(buffer, 8) != buffer[8]) {
        printf("Humidity CRC ERROR\n");
    }

    //////Get data from sensor
    //CO2
    uint16_t co2 = (buffer[0]<<8) | buffer[1];

    //Temperature
    uint16_t raw_temp = (buffer[3]<<8) | buffer[4];
    float temp = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);

    //Humidity
    uint16_t raw_humidity = (buffer[6]<<8) | buffer[7];
    float humidity = 100.0f * ((float)raw_humidity / 65535.0f);

    //Print data
    printf("CO2 = %u ppm\n", co2);
    printf("Temp = %.2f C\n", temp);
    printf("Humidity = %.2f %%\n", humidity);

    vTaskDelay(pdMS_TO_TICKS(5000));
}
