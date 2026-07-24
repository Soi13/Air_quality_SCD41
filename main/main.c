#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

#define I2C_PORT    I2C_NUM_0
#define SDA_PIN     GPIO_NUM_6
#define SCL_PIN     GPIO_NUM_7
#define I2C_FREQ        100000
#define SCD41_ADDR 0x62

#define WIFI_SSID "Soi13"
#define WIFI_PASS ""

// MQTT Server/Broker credentials
#define MQTT_BROKER_URI "mqtt://192.168.1.64"
#define MQTT_USER "mqtt_user"
#define MQTT_PASSWORD ""
#define SCD41_IP "homeassistant/sensor/SCD41_IP"
#define SCD41_CO2 "homeassistant/sensor/SCD41_CO2"
#define SCD41_TEMPERATURE "homeassistant/sensor/SCD41_TEMPERATURE"
#define SCD41_HUMIDITY "homeassistant/sensor/SCD41_HUMIDITY"

static const char *TAG = "SCD41";
char ip[16];

// Wifi event handler for displaying parameters of connection
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying to reconnect. The reason is %d ", event->reason);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Wi-Fi connected");
        ESP_LOGI(TAG, "IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Subnet Mask: " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
    }
}

// Initializing Wifi connection
static void wifi_init(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    ESP_ERROR_CHECK(esp_wifi_start());
    //ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(40)); //This method specifically for ESP32-C3, otherwise it will not connect to WiFi.
}

//Initializing MQTT
static esp_mqtt_client_handle_t client = NULL;

static void mqtt_app(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASSWORD,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);
}

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

esp_err_t scd41_write_command_with_u16_parameter(uint16_t cmd, uint16_t value) {
    uint8_t tx[5];

    tx[0] = (cmd >> 8) & 0xFF;
    tx[1] = cmd & 0xFF;

    tx[2] = (value >> 8) & 0xFF;
    tx[3] = value & 0xFF;

    tx[4] = crc8(&tx[2], 2);

    return i2c_master_transmit(scd41, tx, sizeof(tx), 1000);
}

esp_err_t scd41_set_sensor_altitude(uint16_t altitude) {
    if (altitude > 3000) {
        ESP_LOGE(TAG, "Altitude can't be more than 3000m.");
        return ESP_ERR_INVALID_ARG;
    }

    return scd41_write_command_with_u16_parameter(0x2427, altitude);
}

void app_main(void)
{
    uint8_t buffer[9];
    uint16_t co2, raw_temp, raw_humidity;
    char val[16];

    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();
    vTaskDelay(pdMS_TO_TICKS(5000));
    mqtt_app();
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_ERROR_CHECK(i2c_init());
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG,"Checking sensor...");

    esp_err_t err = probe_sensor();

    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "Sensor not found: %s", esp_err_to_name(err));

        while(1)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG,"Sensor detected!");

    ESP_ERROR_CHECK(scd41_write_command(0x3f86)); //Before start measurement we need send command for Stop periodic measurement
    vTaskDelay(pdMS_TO_TICKS(500)); //Wait exactly 500ms (this is requirement)
    ESP_LOGI(TAG,"Setting altitude...");
    ESP_ERROR_CHECK(scd41_set_sensor_altitude(76)); //Set altitude of location for sensor. 76 meters is average altitude of San Jose above sea level.
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG,"Starting periodic measurements...");
    ESP_ERROR_CHECK(scd41_write_command(0x21B1)); //Now Start periodic measurement

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000)); //Wait exactly 5000ms (this is requirement)
        ESP_ERROR_CHECK(scd41_write_command(0xec05)); //Now begin read measurement
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
        printf("IP: %s\n", ip);
        snprintf(val, sizeof(val), "%s", ip); //Convert data to character buffer
        esp_mqtt_client_publish(client, SCD41_IP, val, 0, 1, 0);

        printf("CO2 = %u ppm\n", co2);
        snprintf(val, sizeof(val), "%u", co2); //Convert data to character buffer
        esp_mqtt_client_publish(client, SCD41_CO2, val, 0, 1, 0);

        printf("Temp = %.2f C\n", temp);
        snprintf(val, sizeof(val), "%.2f", temp); //Convert data to character buffer
        esp_mqtt_client_publish(client, SCD41_TEMPERATURE, val, 0, 1, 0);

        printf("Humidity = %.2f %%\n", humidity);
        snprintf(val, sizeof(val), "%.2f", humidity); //Convert data to character buffer
        esp_mqtt_client_publish(client, SCD41_HUMIDITY, val, 0, 1, 0);

        printf("----------------------------\n");
    }
}
