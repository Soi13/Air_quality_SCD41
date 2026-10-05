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
#define SSD1309_ADDR 0x3C

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_PAGES 8

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
char room[2][15] = {"LIZA'S ROOM:", "DARIA'S ROOM"};

const char* air_q(uint16_t co2) {
    const char* air_quality[] = {"(EXELLENT)", "(GOOD)", "(MODERATE)", "(UNHEALTHY)", "(DANGEROUS)"};

    if (co2 >= 400 && co2 <= 600) {
        return air_quality[0];
    } else if (co2 >= 601 && co2 <= 800) {
        return air_quality[1];
    } else if (co2 >= 801 && co2 <= 1100) {
        return air_quality[2];
    } else if (co2 >= 1101 && co2 <= 2500) {
        return air_quality[3];
    } else if (co2 >= 2501 && co2 <= 5000) {
        return air_quality[4];
    }

   return 0;
}


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
i2c_master_dev_handle_t oled;


// OLED frame buffer (1028 * 64 / 8 = 1024 bytes)
static uint8_t oled_buffer[OLED_WIDTH * OLED_PAGES];

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

    //Config for SCD41
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SCD41_ADDR,
        .scl_speed_hz = I2C_FREQ,
    };

    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &scd41));

    //Config for OLED display
    i2c_device_config_t oled_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SSD1309_ADDR,
        .scl_speed_hz = I2C_FREQ,
    };

    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &oled_cfg, &oled));

    return ESP_OK;
}

//SCD41
esp_err_t probe_sensor(void) {
    return i2c_master_probe(bus_handle, SCD41_ADDR, 100);
}

//OLED
esp_err_t probe_oled(void) {
    return i2c_master_probe(bus_handle, SSD1309_ADDR, 100);
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

static esp_err_t oled_command(uint8_t command) {
    uint8_t data[2];

    // 0x00 = following byte is command
    data[0] = 0x00;
    data[1] = command;

    return i2c_master_transmit(oled, data, sizeof(data), 1000);
}


static esp_err_t oled_data(const uint8_t *data, size_t length) {
    // One control byte + maximum 128 data bytes
    uint8_t tx[129];

    while (length > 0) {
        size_t chunk = length;

        if (chunk > 128) {
            chunk = 128;
        }

        // 0x40 means following bytes are display data
        tx[0] = 0x40;

        memcpy(&tx[1], data, chunk);

        esp_err_t err = i2c_master_transmit(oled, tx, chunk + 1, 1000);

        if (err != ESP_OK) {
            return err;
        }

        data += chunk;
        length -= chunk;
    }

    return ESP_OK;
}

// Initialize OLED
static void oled_init(void) {
    vTaskDelay(pdMS_TO_TICKS(100));

    // Display OFF
    ESP_ERROR_CHECK(oled_command(0xAE));

    // Clock divide ratio / oscillator frequency
    ESP_ERROR_CHECK(oled_command(0xD5));
    ESP_ERROR_CHECK(oled_command(0x80));

    // Multiplex ratio
    ESP_ERROR_CHECK(oled_command(0xA8));
    ESP_ERROR_CHECK(oled_command(0x3F));

    // Display offset
    ESP_ERROR_CHECK(oled_command(0xD3));
    ESP_ERROR_CHECK(oled_command(0x00));

    // Start line
    ESP_ERROR_CHECK( oled_command(0x40));

    // Memory addressing mode
    ESP_ERROR_CHECK(oled_command(0x20));

    // Horizontal addressing
    ESP_ERROR_CHECK(oled_command(0x00));

    // Segment remap
    ESP_ERROR_CHECK(oled_command(0xA1));

    // COM scan direction
    ESP_ERROR_CHECK(oled_command(0xC8));

    // COM pin configuration
    ESP_ERROR_CHECK(oled_command(0xDA));
    ESP_ERROR_CHECK(oled_command(0x12));

    // Contrast
    ESP_ERROR_CHECK(oled_command(0x81));
    ESP_ERROR_CHECK(oled_command(0xCF));

    // Precharge
    ESP_ERROR_CHECK(oled_command(0xD9));
    ESP_ERROR_CHECK(oled_command(0xF1));

    // VCOMH
    ESP_ERROR_CHECK(oled_command(0xDB));
    ESP_ERROR_CHECK(oled_command(0x40));

    // Entire display follows RAM
    ESP_ERROR_CHECK(oled_command(0xA4));

    // Normal display
    ESP_ERROR_CHECK(oled_command(0xA6));

    // Display ON
    ESP_ERROR_CHECK(oled_command(0xAF));
}

//Clear buffer
static void oled_clear(void) {
    memset(oled_buffer, 0, sizeof(oled_buffer));
}

//Update
static void oled_update(void) {
    // Column address
    ESP_ERROR_CHECK(oled_command(0x21));
    ESP_ERROR_CHECK(oled_command(0));
    ESP_ERROR_CHECK(oled_command(127));

    // Page address
    ESP_ERROR_CHECK(oled_command(0x22));
    ESP_ERROR_CHECK(oled_command(0));
    ESP_ERROR_CHECK(oled_command(7));

    ESP_ERROR_CHECK(oled_data(oled_buffer, sizeof(oled_buffer))
    );
}

static void oled_pixel(int x, int y, bool state) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }

    int page = y / 8;
    int bit =  y % 8;
    int index = page * OLED_WIDTH + x;

    if (state) {
        oled_buffer[index] |= (1 << bit);
    } else {
        oled_buffer[index] &= ~(1 << bit);
    }
}

// 5X7 digits
static const uint8_t font_digits[10][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}  // 9
};

//Font
static uint8_t get_font_column(char c, int column) {
    if (c >= '0' && c <= '9') {
        return font_digits[c - '0'][column];
    }

    // A
    if (c == 'A') {
        static const uint8_t f[5] = {0x7E, 0x11, 0x11, 0x11, 0x7E};
        return f[column];
    }

    // C
    if (c == 'C') {
        static const uint8_t f[5] = {0x3E, 0x41, 0x41, 0x41, 0x22};
        return f[column];
    }

    // D
    if (c == 'D') {
        static const uint8_t f[5] = {0x7F, 0x41, 0x41, 0x22, 0x1C};
        return f[column];
    }

    // E
    if (c == 'E') {
        static const uint8_t f[5] = {0x7F, 0x49, 0x49, 0x49, 0x41};
        return f[column];
    }

    // G
    if (c == 'G') {
        static const uint8_t f[5] = {0x3E, 0x41, 0x49, 0x49, 0x7A};
        return f[column];
    }

    // H
    if (c == 'H') {
        static const uint8_t f[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
        return f[column];
    }

    // I
    if (c == 'I') {
        static const uint8_t f[5] = { 0x00, 0x41, 0x7F, 0x41, 0x00};
        return f[column];
    }

    // L
    if (c == 'L') {
        static const uint8_t f[5] = {0x7F, 0x40, 0x40, 0x40, 0x40};
        return f[column];
    }

    // M
    if (c == 'M') {
        static const uint8_t f[5] = {0x7F, 0x02, 0x0C, 0x02, 0x7F};
        return f[column];
    }

    // N
    if (c == 'N') {
        static const uint8_t f[5] = {0x7F, 0x02, 0x0C, 0x10, 0x7F};
        return f[column];
    }

    // O
    if (c == 'O') {
        static const uint8_t f[5] = {0x3E, 0x41, 0x41, 0x41, 0x3E};
        return f[column];
    }

    // P
    if (c == 'P') {
        static const uint8_t f[5] = {0x7F, 0x09, 0x09, 0x09, 0x06};
        return f[column];
    }

    // R
    if (c == 'R') {
        static const uint8_t f[5] = {0x7F, 0x09, 0x19, 0x29, 0x46};
        return f[column];
    }

    // S
    if (c == 'S') {
        static const uint8_t f[5] = {0x46, 0x49, 0x49, 0x49, 0x31};
        return f[column];
    }

    // T
    if (c == 'T') {
        static const uint8_t f[5] = {0x01, 0x01, 0x7F, 0x01, 0x01};
        return f[column];
    }

    // U
    if (c == 'U') {
        static const uint8_t f[5] = {0x3F, 0x40, 0x40, 0x40, 0x3F};
        return f[column];
    }

    // Y
    if (c == 'Y') {
        static const uint8_t f[5] = {0x03, 0x04, 0x78, 0x04, 0x03};
        return f[column];
    }

    // X
    if (c == 'X') {
        static const uint8_t f[5] = {0x63, 0x14, 0x08, 0x14, 0x63};
        return f[column];
    }

    // Z
    if (c == 'Z') {
        static const uint8_t f[5] = {0x61, 0x51, 0x49, 0x45, 0x43};
        return f[column];
    }

    // '
    if (c == '\'') {
        static const uint8_t f[5] = {0x00, 0x03, 0x07, 0x00, 0x00};
        return f[column];
    }

    // colon
    if (c == ':') {
        static const uint8_t f[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
        return f[column];
    }

    // decimal point
    if (c == '.') {
        static const uint8_t f[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
        return f[column];
    }

    // minus
    if (c == '-') {
        static const uint8_t f[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
        return f[column];
    }

    // percent
    if (c == '%') {
        static const uint8_t f[5] = {0x62, 0x64, 0x08, 0x13, 0x23};
        return f[column];
    }

    // (
    if (c == '(') {
        static const uint8_t f[5] = {0x00, 0x1C, 0x22, 0x41, 0x00};
        return f[column];
    }

    // )
    if (c == ')') {
        static const uint8_t f[5] = {0x00, 0x41, 0x22, 0x1C, 0x00};
        return f[column];
    }

    // Space / unsupported
    return 0x00;
}

//Draw character
static void oled_char(int x, int y, char c) {
    for (int column = 0; column < 5; column++) {
        uint8_t line = get_font_column(c, column);

        for (int row = 0; row < 7; row++) {
            if (line & (1 << row)) {
                oled_pixel(x + column, y + row, true);
            }
        }
    }
}

//Draw text
static void oled_text(int x, int y, const char *text) {
    while (*text) {
        oled_char(x, y, *text);
        x += 6;
        text++;
    }
}

//Display sensor information
static void display_sensor_data(uint16_t co2, float temp, float humidity) {
    char value[32];

    oled_clear();

    //Room name
    oled_text(2, 4, room[0]);

    //CO2
    oled_text(2, 25, "CO2:");
    snprintf(value, sizeof(value), "%u", co2);
    oled_text(38, 25, value);
    //oled_text(68, 25, "PPM");
    oled_text(64, 25, air_q(co2));

    //Temperature
    oled_text(2, 40, "TEMP:");
    snprintf(value, sizeof(value), "%.1f C", temp);
    oled_text(38, 40, value);

    //Humidity
    oled_text(2, 55, "RH:");
    snprintf(value, sizeof(value), "%.1f %%", humidity);
    oled_text(38, 55, value);

    oled_update();
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
    ESP_LOGI(TAG, "Checking OLED at address 0x%02X...", SSD1309_ADDR);

    err = probe_oled();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OLED not found at 0x%02X: %s", SSD1309_ADDR, esp_err_to_name(err));
        ESP_LOGE(TAG, "If your OLED is connected correctly, try OLED_ADDR 0x3D.");

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGI(TAG, "OLED detected!");

    oled_init();
    oled_clear();
    oled_text(20, 20, "SCD41");
    oled_text(20, 36, "START");
    oled_update();

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
        i2c_master_receive(scd41, buffer, sizeof(buffer), 1000);

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

        // Display data on OLED
        display_sensor_data(co2, temp, humidity);

        //Print data
        //printf("IP: %s\n", ip);
        snprintf(val, sizeof(val), "%s", ip); //Convert data to character buffer
        esp_mqtt_client_publish(client, SCD41_IP, val, 0, 1, 0);

        //printf("CO2 = %u ppm\n", co2);
        snprintf(val, sizeof(val), "%u", co2); //Convert data to character buffer
        esp_mqtt_client_publish(client, SCD41_CO2, val, 0, 1, 0);

        //printf("Temp = %.2f C\n", temp);
        snprintf(val, sizeof(val), "%.2f", temp); //Convert data to character buffer
        esp_mqtt_client_publish(client, SCD41_TEMPERATURE, val, 0, 1, 0);

        //printf("Humidity = %.2f %%\n", humidity);
        snprintf(val, sizeof(val), "%.2f", humidity); //Convert data to character buffer
        esp_mqtt_client_publish(client, SCD41_HUMIDITY, val, 0, 1, 0);

        //printf("----------------------------\n");
    }
}
