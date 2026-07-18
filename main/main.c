#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
//#include "driver/i2c.h"

#define I2C_PORT    I2C_NUM_0
#define SDA_PIN     GPIO_NUM_6
#define SCL_PIN     GPIO_NUM_7
#define I2C_FREQ        100000
#define SCD41_ADDR 0x62

static const char *TAG = "SCD41";

i2c_master_bus_handle_t bus_handle;
i2c_master_dev_handle_t scd41;

esp_err_t i2c_init(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = SDA_PIN,
        .scl_io_num = SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SCD41_ADDR,
        .scl_speed_hz = I2C_FREQ,
    };

    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &scd41));

    return ESP_OK;
}

esp_err_t probe_sensor(void) {
    return i2c_master_probe(bus_handle, SCD41_ADDR, 100);
}

esp_err_t scd41_write_command(uint16_t cmd) {
    uint8_t data[2];

    data[0] = cmd >> 8;
    data[1] = cmd & 0xFF;

    return i2c_master_transmit(scd41, data, sizeof(data), 1000);
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
    uint16_t co2, raw_temp, raw_humidity;

    ESP_ERROR_CHECK(i2c_init());
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG,"Checking sensor...");

    esp_err_t err = probe_sensor();

    if(err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Sensor not found: %s",
                 esp_err_to_name(err));

        while(1)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG,"Sensor detected!");

    err = scd41_write_command(0x3f86); //Before start measurement we need send command for Stop periodic measurement
    vTaskDelay(pdMS_TO_TICKS(500)); //Wait exactly 500ms (this is requirement)
    err = scd41_write_command(0x21B1); //Now Start periodic measurement

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000)); //Wait exactly 5000ms (this is requirement)
        err = scd41_write_command(0xec05); //Now begin read measurement
        vTaskDelay(pdMS_TO_TICKS(1));

        //ESP_LOGI(TAG, "Start measurement: %s", esp_err_to_name(err));

        //Read 9 bytes
        i2c_master_receive(scd41, buffer, 9, 1000);

        //Check CRC for CO
        if(crc8(&buffer[0], 2) != buffer[2]) {
            ESP_LOGE(TAG, "CO2 CRC failed");
        }

        //Check CRC for temperature
        if(crc8(&buffer[3], 2) != buffer[5]) {
            ESP_LOGE(TAG, "Temperature CRC failed");
        }

        //Check CRC for humidity
        if(crc8(&buffer[6], 2) != buffer[8]) {
            ESP_LOGE(TAG, "Humidity CRC failed");
        }

        //////Get data from sensor
        //CO2
        co2 = (buffer[0]<<8) | buffer[1];

        //Temperature
        raw_temp = (buffer[3]<<8) | buffer[4];
        float temp = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);

        //Humidity
        raw_humidity = (buffer[6]<<8) | buffer[7];
        float humidity = 100.0f * ((float)raw_humidity / 65535.0f);

        //Print data
        printf("CO2 = %u ppm\n", co2);
        printf("Temp = %.2f C\n", temp);
        printf("Humidity = %.2f %%\n", humidity);
        printf("----------------------------\n");
    }
}
