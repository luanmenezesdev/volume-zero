#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "ssd1306.h"
#include "hardware/i2c.h"
#include "hardware/adc.h"
#include "hardware/pio.h"
#include "ws2818b.pio.h"
#include "pico/cyw43_arch.h"
#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"

// App Mode
typedef enum
{
    MODE_LISTENING,
    MODE_CHALLENGE
} app_mode_t;

volatile app_mode_t current_mode = MODE_LISTENING;

// Led Matrix Configuration
#define WS_PIN 7
#define MAX_LEVELS 10
#define MATRIX_SIDE 5
#define MATRIX_LEDS 25
#define MID_IDX 12
uint8_t global_brightness = 32;

uint8_t level = 1;
static uint64_t last_reset_ms = 0;
bool led_on[MATRIX_LEDS];
uint8_t cursor_row = 2;
uint8_t cursor_col = 2;
uint8_t activate_led = 0;

// Wi-Fi Credentials
// #define WIFI_SSID "brisa-4370576"
// #define WIFI_PASSWORD "mmy6opmr"
#define WIFI_SSID "Redmi Note 12"
#define WIFI_PASSWORD "luanteste"

// MQTT Configuration
#define MQTT_BROKER "52.28.107.34"
#define MQTT_TOPIC_INFRACTION "volume/infraction"
#define MQTT_TOPIC_CLEAR "volume/clear"

// Microphone Configuration
#define INFRACTION_SEND_COOLDOWN_MS 15000
#define MIC_THRESHOLD 2100
#define REQUIRED_PEAKS 5
#define WINDOW_MS 10000
#define SILENCE_RESET_MS 5000
#define MIC_ADC_CHANNEL 2
#define MIC_ADC_PIN (26 + MIC_ADC_CHANNEL)

//  Keeps track of the peaks
uint64_t peak_times[REQUIRED_PEAKS];
uint8_t peak_index = 0;
uint8_t peak_count = 0;

uint64_t last_peak_time = 0;

// LED and Button Pins
#define LED_R_PIN 13
#define LED_G_PIN 11
#define BTN_B_PIN 6

// I2C Pins
#define I2C_SDA_PIN 14
#define I2C_SCL_PIN 15

uint8_t ssd[ssd1306_buffer_length];
struct render_area frame_area;

volatile uint64_t last_press_time_b = 0;
volatile uint64_t last_mic_infraction_send_time = 0;

// Joystick Configuration
#define JOY_ADC_MAX 4095 // 12-bit ADC
#define JOY_CENTER (JOY_ADC_MAX / 2)
#define JOY_DEAD_ZONE 600 // ± dead band around centre
#define JOY_REPEAT_MS 200 // cursor repeat rate
#define JOY_Y_ADC_CHANNEL 0
#define JOY_X_ADC_CHANNEL 1
#define JOY_X_ADC_PIN (26 + JOY_X_ADC_CHANNEL)
#define JOY_Y_ADC_PIN (26 + JOY_Y_ADC_CHANNEL)

// Cursor position helpers
static inline uint8_t rc_to_idx(uint8_t row, uint8_t col)
{
    /* a matrixa real tem a primeira fileira no topo,
       mas o seu cabeamento começa no rodapé (row 4). */
    uint8_t hw_row = MATRIX_SIDE - 1 - row; // espelha verticalmente

    if (hw_row & 1)
    {
        /* fileiras ímpares (contando 0-based) vão da esquerda→direita */
        return hw_row * MATRIX_SIDE + col;
    }
    else
    {
        /* fileiras pares vão da direita→esquerda */
        return hw_row * MATRIX_SIDE + (MATRIX_SIDE - 1 - col);
    }
}

static uint8_t wrap5(int v) { return (v + MATRIX_SIDE) % MATRIX_SIDE; }
struct pixel_t
{
    uint8_t G, R, B; // Três valores de 8-bits compõem um pixel.
};
typedef struct pixel_t pixel_t;
typedef pixel_t npLED_t; // Mudança de nome de "struct pixel_t" para "npLED_t" por clareza.

// Declaração do buffer de pixels que formam a matriz.
npLED_t leds[MATRIX_LEDS];

// Variáveis para uso da máquina PIO.
PIO np_pio;
uint sm;

static inline uint8_t scale(uint8_t v, uint8_t factor)
{
    return (uint16_t)v * factor / 255;
}

/**
 * Escreve os dados do buffer nos LEDs.
 */
void npWrite()
{
    // Escreve cada dado de 8-bits dos pixels em sequência no buffer da máquina PIO.
    for (uint i = 0; i < MATRIX_LEDS; ++i)
    {
        pio_sm_put_blocking(np_pio, sm, leds[i].G);
        pio_sm_put_blocking(np_pio, sm, leds[i].R);
        pio_sm_put_blocking(np_pio, sm, leds[i].B);
    }
    sleep_us(100); // Espera 100us, sinal de RESET do datasheet.
}

// Inicializa a máquina PIO para controle da matriz de LEDs.
void npInit(uint pin)
{

    // Cria programa PIO.
    uint offset = pio_add_program(pio0, &ws2818b_program);
    np_pio = pio0;

    // Toma posse de uma máquina PIO.
    sm = pio_claim_unused_sm(np_pio, false);
    if (sm < 0)
    {
        np_pio = pio1;
        sm = pio_claim_unused_sm(np_pio, true); // Se nenhuma máquina estiver livre, panic!
    }

    // Inicia programa na máquina PIO obtida.
    ws2818b_program_init(np_pio, sm, offset, pin, 800000.f);

    // Limpa buffer de pixels.
    for (uint i = 0; i < MATRIX_LEDS; ++i)
    {
        leds[i].R = 0;
        leds[i].G = 0;
        leds[i].B = 0;
    }
    npWrite();
}

// Atribui uma cor RGB a um LED.
void npSetLED(const uint index, const uint8_t r, const uint8_t g, const uint8_t b)
{
    leds[index].R = scale(r, global_brightness);
    leds[index].G = scale(g, global_brightness);
    leds[index].B = scale(b, global_brightness);
}

// Limpa o buffer de pixels.
void npClear()
{
    for (uint i = 0; i < MATRIX_LEDS; ++i)
        npSetLED(i, 0, 0, 0);
}

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

// Matrix helpers
void draw_matrix(void)
{
    for (int r = 0; r < MATRIX_SIDE; ++r)
        for (int c = 0; c < MATRIX_SIDE; ++c)
        {
            uint8_t idx = rc_to_idx(r, c);

            if (r == cursor_row && c == cursor_col)
                npSetLED(idx, 255, 0, 0); // vermelho
            else if (led_on[idx])
                npSetLED(idx, 255, 255, 255); // branco
            else
                npSetLED(idx, 0, 0, 0); // apagado
        }
    npWrite();
}

static bool all_leds_on(void)
{
    for (int i = 0; i < MATRIX_LEDS; ++i)
        if (!led_on[i])
            return false;
    return true;
}

static void clear_matrix(void)
{
    npClear();
    npWrite();
}

// Function to start a new level in the game
void start_level(uint8_t lvl)
{
    memset(led_on, true, sizeof led_on);
    led_on[MID_IDX] = true;

    uint8_t to_turn_off = (2 * lvl - 1);
    if (to_turn_off > MATRIX_LEDS - 1)
        to_turn_off = MATRIX_LEDS - 1;

    // randomly blank LEDs (never the centre)
    while (to_turn_off)
    {
        uint8_t r = rand() % MATRIX_LEDS;
        if (r == MID_IDX || !led_on[r])
            continue;
        led_on[r] = false;
        --to_turn_off;
    }

    cursor_row = 2;
    cursor_col = 2;
    draw_matrix();
}

// Function to activate the cursor
void try_activate_square()
{
    if (!led_on[rc_to_idx(cursor_row, cursor_col)])
    {
        led_on[rc_to_idx(cursor_row, cursor_col)] = true;
        draw_matrix();
    }
}

// Function to check if the number of peaks in the last 10 seconds is sufficient
bool peaks_in_window(uint64_t now_ms)
{
    if (peak_count < REQUIRED_PEAKS)
        return false;
    // The index of the oldest peak in the circular buffer
    uint8_t oldest = (peak_index + (REQUIRED_PEAKS - peak_count)) % REQUIRED_PEAKS;
    return (now_ms - peak_times[oldest]) <= WINDOW_MS;
}

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

void display_message(char *lines[], int line_count)
{
    // Limpa o buffer
    memset(ssd, 0, ssd1306_buffer_length);

    // Desenha cada linha
    for (int i = 0; i < line_count; i++)
    {
        if (lines[i] != NULL)
        {
            ssd1306_draw_string(ssd, 5, i * 8, lines[i]);
        }
    }

    render_on_display(ssd, &frame_area);
}

void gpio_callback(uint gpio, uint32_t events)
{
    uint64_t current_time = to_ms_since_boot(get_absolute_time());

    if (gpio == BTN_B_PIN && current_time - last_press_time_b > 200)
    { // Debounce Button B
        last_press_time_b = current_time;
        if (current_mode == MODE_CHALLENGE)
        {
            activate_led = 1;
        }
    }
}

/* ----------------------------------------------------
 *  Reads joystick and moves the blue cursor if needed
 * --------------------------------------------------*/
void handle_joystick(void)
{
    static uint64_t last_move_ms = 0;
    uint64_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms - last_move_ms < JOY_REPEAT_MS)
        return;

    /* ler joystick ------------------------------------------------ */
    adc_select_input(JOY_X_ADC_CHANNEL);
    uint16_t adc_x = adc_read();
    adc_select_input(JOY_Y_ADC_CHANNEL);
    uint16_t adc_y = adc_read();

    int dx = 0, dy = 0;
    if (adc_x > JOY_CENTER + JOY_DEAD_ZONE)
        dx = +1;
    else if (adc_x < JOY_CENTER - JOY_DEAD_ZONE)
        dx = -1;

    if (adc_y > JOY_CENTER + JOY_DEAD_ZONE)
        dy = -1; // stick desce → cursor desce
    else if (adc_y < JOY_CENTER - JOY_DEAD_ZONE)
        dy = +1; // stick sobe  → cursor sobe

    if (!dx && !dy)
        return;

    /* calcular novo row/col --------------------------------------- */
    cursor_row = wrap5(cursor_row + dy);
    cursor_col = wrap5(cursor_col + dx);

    last_move_ms = now_ms;
    draw_matrix();
}

void listening_mode()
{
    uint64_t now_ms = to_ms_since_boot(get_absolute_time());
    adc_select_input(MIC_ADC_CHANNEL);
    uint16_t adc_value = adc_read();

    printf("ADC Value: %d\n", adc_value);
    if (adc_value > MIC_THRESHOLD)
    {
        last_peak_time = now_ms;

        peak_times[peak_index] = now_ms;
        peak_index = (peak_index + 1) % REQUIRED_PEAKS;
        if (peak_count < REQUIRED_PEAKS)
            peak_count++;

        if (peaks_in_window(now_ms) && (now_ms - last_mic_infraction_send_time) > INFRACTION_SEND_COOLDOWN_MS)
        {
            mqtt_send_message(mqtt_client, MQTT_TOPIC_INFRACTION, "Infraction detected!");
            display_message((char *[]){
                                "infraction",
                                "detected",
                                "",
                                "complete the",
                                "challenge"},
                            5);
            last_mic_infraction_send_time = now_ms;
            current_mode = MODE_CHALLENGE;
            start_level(level);
            peak_count = 0;
        }
    }
    else if (peak_count > 0 && (now_ms - last_peak_time) > SILENCE_RESET_MS)
    {
        peak_count = 0;
    }
}

void challenge_mode()
{
    uint64_t now_ms = to_ms_since_boot(get_absolute_time());

    printf("Challenge Mode: LED %d\n", rc_to_idx(cursor_row, cursor_col));

    if (activate_led)
    {
        activate_led = 0;
        try_activate_square();
    }

    if (all_leds_on())
    {
        if (now_ms - last_reset_ms > 60 * 60 * 1000) // 1 hour
        {
            level = 1;
            last_reset_ms = now_ms;
        }
        else if (level < MAX_LEVELS)
        {
            ++level;
        }

        display_message((char *[]){
                            "Listening Mode",
                        },
                        1);
        mqtt_send_message(mqtt_client, MQTT_TOPIC_CLEAR, "Clear!");
        current_mode = MODE_LISTENING;
        clear_matrix();
    }
}

int main()
{
    stdio_usb_init();
    npInit(WS_PIN);

    srand((unsigned)time_us_64());

    // Initialize Leds and Buttons
    gpio_init(LED_R_PIN);
    gpio_set_dir(LED_R_PIN, GPIO_OUT);
    gpio_init(LED_G_PIN);
    gpio_set_dir(LED_G_PIN, GPIO_OUT);

    gpio_init(BTN_B_PIN);
    gpio_set_dir(BTN_B_PIN, GPIO_IN);
    gpio_pull_up(BTN_B_PIN);

    gpio_set_irq_enabled_with_callback(BTN_B_PIN, GPIO_IRQ_EDGE_FALL, true, &gpio_callback);

    // Initialize ADC
    adc_gpio_init(MIC_ADC_PIN);
    adc_gpio_init(JOY_X_ADC_PIN);
    adc_gpio_init(JOY_Y_ADC_PIN);
    adc_init();
    adc_select_input(MIC_ADC_CHANNEL);

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

    display_message((char *[]){
                        "Listening Mode",
                    },
                    1);

    while (true)
    {
        // Poll the Wi-Fi chip
        cyw43_arch_poll();

        switch (current_mode)
        {

        case MODE_LISTENING:
            listening_mode();
            break;

        case MODE_CHALLENGE:
            handle_joystick();
            challenge_mode();
            break;
        }

        // Reduce CPU usage
        sleep_ms(10);
    }
}
