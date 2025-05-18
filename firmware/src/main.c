#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "ssd1306.h"
#include "hardware/i2c.h"
#include "pico/cyw43_arch.h"
#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"

// Wi-Fi Credentials
#define WIFI_SSID "brisa-4370576"
#define WIFI_PASSWORD "mmy6opmr"

// MQTT Configuration
#define MQTT_BROKER "52.28.107.34"
#define MQTT_TOPIC_INFRACTION "volume/infraction"
#define MQTT_TOPIC_CLEAR "volume/clear"

#define LED_R_PIN 13
#define LED_G_PIN 11
#define BTN_A_PIN 5
#define BTN_B_PIN 6

#define I2C_SDA_PIN 14
#define I2C_SCL_PIN 15

uint8_t ssd[ssd1306_buffer_length];
struct render_area frame_area;

// Global Variables
int send_message_infraction = 0;
int send_message_clear = 0;
volatile uint64_t last_press_time_a = 0;
volatile uint64_t last_press_time_b = 0;

// MQTT Client
mqtt_client_t *mqtt_client;

static const struct mqtt_connect_client_info_t mqtt_client_info = {
    .client_id = "PicoWLuanMenezes", // Client ID (required)
    .client_user = NULL,             // Username (optional, NULL if not used)
    .client_pass = NULL,             // Password (optional, NULL if not used)
    .keep_alive = 60,                // Keep-alive interval in seconds
    .will_topic = NULL,              // Last will topic (optional)
    .will_msg = NULL,                // Last will message (optional)
    .will_retain = 0,                // Last will retain flag
    .will_qos = 0                    // Last will QoS level
};

// Function to send a message via MQTT
void mqtt_send_message(mqtt_client_t *client, const char *mqtt_topic, const char *message)
{
    char formatted_message[256];
    snprintf(formatted_message, sizeof(formatted_message), "pico: %s", message);

    err_t result = mqtt_publish(client, mqtt_topic, formatted_message, strlen(formatted_message), 0, 0, NULL, NULL);
    if (result == ERR_OK)
    {
        printf("Message published: %s\n", formatted_message);
        gpio_put(LED_R_PIN, 1); // Turn on red LED
        sleep_ms(500);
        gpio_put(LED_R_PIN, 0); // Turn off red LED
    }
    else
    {
        printf("Failed to publish message. Error: %d\n", result);
    }
}

// MQTT Connection Callback
void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status)
{
    if (status == MQTT_CONNECT_ACCEPTED)
    {
        printf("MQTT connected successfully.\n");

        // Call the send_message function with the desired message
        mqtt_send_message(client, MQTT_TOPIC_CLEAR, "Hello from Raspberry Pi Pico W!");

        gpio_put(LED_G_PIN, 1); // Turn on green LED
        gpio_put(LED_R_PIN, 0); // Turn off red LED
    }
    else
    {
        printf("MQTT connection failed with status: %d\n", status);
        gpio_put(LED_G_PIN, 0); // Turn off green LED
        gpio_put(LED_R_PIN, 1); // Turn on red LED
    }
}

// Initialize MQTT
void init_mqtt()
{
    ip_addr_t broker_ip;
    mqtt_client = mqtt_client_new();
    if (!mqtt_client)
    {
        printf("Failed to create MQTT client.\n");
        gpio_put(LED_G_PIN, 0); // Turn off green LED
        gpio_put(LED_R_PIN, 1); // Turn on red LED
        return;
    }

    if (!ip4addr_aton(MQTT_BROKER, &broker_ip))
    {
        printf("Failed to resolve broker IP address: %s\n", MQTT_BROKER);
        gpio_put(LED_G_PIN, 0); // Turn off green LED
        gpio_put(LED_R_PIN, 1); // Turn on red LED
        return;
    }

    err_t err = mqtt_client_connect(mqtt_client, &broker_ip, MQTT_PORT, mqtt_connection_cb, NULL, &mqtt_client_info);

    if (err != ERR_OK)
    {
        printf("MQTT connection failed with error code: %d\n", err);
        gpio_put(LED_G_PIN, 0); // Turn off green LED
        gpio_put(LED_R_PIN, 1); // Turn on red LED
        return;
    }

    printf("Connecting to MQTT broker at %s:%d...\n", MQTT_BROKER, MQTT_PORT);
}

// Connect to Wi-Fi
void connect_to_wifi()
{
    printf("Connecting to Wi-Fi...\n");
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 10000))
    {
        printf("Failed to connect to Wi-Fi.\n");
        gpio_put(LED_G_PIN, 0); // Turn off green LED
        gpio_put(LED_R_PIN, 1); // Turn on red LED
        while (1)
            sleep_ms(1000);
    }
    printf("Connected to Wi-Fi.\n");
    gpio_put(LED_G_PIN, 1); // Turn on green LED
    gpio_put(LED_R_PIN, 0); // Turn off red LED
}

// inicializar o OLED
void init_oled()
{
    i2c_init(i2c1, ssd1306_i2c_clock * 1000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    ssd1306_init();

    frame_area.start_column = 0;
    frame_area.end_column = ssd1306_width - 1;
    frame_area.start_page = 0;
    frame_area.end_page = ssd1306_n_pages - 1;

    calculate_render_area_buffer_length(&frame_area);

    memset(ssd, 0, ssd1306_buffer_length);
    render_on_display(ssd, &frame_area);
}

void display_message(char *message)
{
    // Constants for the display
    const int max_chars_per_line = ssd1306_width / 8.5; // Assuming each character is ~8.5px wide
    const int max_lines = ssd1306_n_pages;              // Number of lines (pages) on the display

    // Buffer to store one line of text
    char line_buffer[max_chars_per_line + 1];

    // Clear the display buffer
    memset(ssd, 0, ssd1306_buffer_length);

    // Split the message into lines and render each
    for (int line = 0; line < max_lines && *message != '\0'; ++line)
    {
        // Copy up to `max_chars_per_line` characters to the line buffer
        strncpy(line_buffer, message, max_chars_per_line);
        line_buffer[max_chars_per_line] = '\0'; // Ensure null-termination

        // Draw the current line
        ssd1306_draw_string(ssd, 5, line * 8, line_buffer); // Y-offset: line * 8px

        // Move the pointer in the message to the next chunk
        message += max_chars_per_line;
    }

    // Render the updated buffer on the display
    render_on_display(ssd, &frame_area);
}

void gpio_callback(uint gpio, uint32_t events)
{
    uint64_t current_time = to_ms_since_boot(get_absolute_time());

    if (gpio == BTN_A_PIN && current_time - last_press_time_a > 200 && send_message_infraction == 0)
    { // Debounce Button A
        last_press_time_a = current_time;
        send_message_infraction = 1;
    }

    if (gpio == BTN_B_PIN && current_time - last_press_time_b > 200 && send_message_clear == 0)
    { // Debounce Button B
        last_press_time_b = current_time;
        send_message_clear = 1;
    }
}

int main()
{
    stdio_init_all();

    // Initialize Leds and Buttons
    gpio_init(LED_R_PIN);
    gpio_set_dir(LED_R_PIN, GPIO_OUT);
    gpio_init(LED_G_PIN);
    gpio_set_dir(LED_G_PIN, GPIO_OUT);

    gpio_init(BTN_A_PIN);
    gpio_set_dir(BTN_A_PIN, GPIO_IN);
    gpio_pull_up(BTN_A_PIN);
    gpio_init(BTN_B_PIN);
    gpio_set_dir(BTN_B_PIN, GPIO_IN);
    gpio_pull_up(BTN_B_PIN);

    // Set Shared GPIO Interrupt
    gpio_set_irq_enabled_with_callback(BTN_A_PIN, GPIO_IRQ_EDGE_FALL, true, &gpio_callback);
    gpio_set_irq_enabled(BTN_B_PIN, GPIO_IRQ_EDGE_FALL, true);

    init_oled();

    if (cyw43_arch_init())
    {
        printf("Failed to initialize CYW43.\n");
        gpio_put(LED_G_PIN, 0); // Turn off green LED
        gpio_put(LED_R_PIN, 1); // Turn on red LED
        return 1;
    }
    cyw43_arch_enable_sta_mode();

    connect_to_wifi();

    init_mqtt();

    while (true)
    {
        // Poll the Wi-Fi chip
        cyw43_arch_poll();

        if (send_message_infraction)
        {
            send_message_infraction = 0;
            mqtt_send_message(mqtt_client, MQTT_TOPIC_INFRACTION, "Infraction detected!");
            display_message("Infraction detected!");
        }

        if (send_message_clear)
        {
            send_message_clear = 0;
            mqtt_send_message(mqtt_client, MQTT_TOPIC_CLEAR, "Clear!");
            display_message("Clear!");
        }

        // Reduce CPU usage
        sleep_ms(10);
    }
}
