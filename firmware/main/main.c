#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"
#include <string.h>
#include "ssd1306.h"

// Hardware Pin Configurations
#define JOYSTICK_SW_PIN GPIO_NUM_32
#define ENCODER_SW_PIN  GPIO_NUM_27
#define ENCODER_CLK_PIN GPIO_NUM_25 
#define ENCODER_DT_PIN GPIO_NUM_26
#define BUTTON1_PIN GPIO_NUM_16 // Btn A
#define BUTTON2_PIN GPIO_NUM_17 // Btn B
#define BUTTON3_PIN GPIO_NUM_18 // Btn X
#define BUTTON4_PIN GPIO_NUM_19 // Btn Y
#define ANALOGX_PIN ADC1_CHANNEL_6
#define ANALOGY_PIN ADC1_CHANNEL_7
#define OLED_SCK_PIN GPIO_NUM_13
#define OLED_SDA_PIN GPIO_NUM_33
// Joystick configs
#define JOYSTICK_POLL_RATE_MS 25
#define JOYSTICK_CENTER 2048
#define JOYSTICK_DEADZONE 50
// Cooldown for button presses
#define ISR_CD 8000ULL // 5 ms
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// Declare Queues
QueueHandle_t SendQueue;
QueueHandle_t ReceiveQueue; 
QueueHandle_t ButtonQueue;
QueueHandle_t EncoderQueue;
// Declare Task handles
TaskHandle_t SendHandle = NULL;
TaskHandle_t ReadHandle = NULL;
TaskHandle_t DisplayHandle = NULL;
TaskHandle_t ReceiveHandle = NULL;

// Controller and receiver mac address
const uint8_t peer_address1[ESP_NOW_ETH_ALEN] = {0x10,0x10,0x10,0x10,0x10,0x10}; // 16:16:16:16:16:16
const uint8_t controller_address[ESP_NOW_ETH_ALEN] = {0x02,0x01,0x01,0x01,0x01,0x01}; //2:1:1:1:1:1

// Timer for each individual GPIO pin for button cooldown
static volatile uint64_t isr_time_read[GPIO_NUM_MAX] = {0};

uint64_t espn_send_wd_timer = 0;

//Display
SSD1306_t OledScreen;


// Left / Right twist for enum
enum Direction { // ENCODER 
    LEFT = -1,
    RIGHT = 1,
};

enum Mode { // Mode of controller used by display
    CONNECTING,
    DIAGNOSTIC,
    CONNECTED,
};


typedef struct {
    char title[10];
    char text1[18];
    char text2[18];
    char text3[18];
} DisplayPacket;

// Entire packet for button press
typedef struct {
    gpio_num_t pin; // Pin #
    bool btndown; // T/F for whether button is down
    uint32_t ms_trig; // Timestamp of when info was sent to determine presses vs holds
} BtnInputPacket;

// Values for Analog stick
typedef struct {
    int Xrange; // X value
    int Yrange; // Y value
} StickInputPacket;

// Only needs direction from enum
typedef struct {
    int direction; // left (0) or right (1)
} EncInputPacket;

// Master packet that can be populated with one of the above
typedef struct {
    uint8_t packet_type; // 0: Button 1: Joystick 2: Encoder
    union { // Select one type of packet here
        BtnInputPacket Button;
        StickInputPacket JoyStick;
        EncInputPacket Encoder;
    } packet_select;
} SendPacket;

// Call to initialize a button in main
void init_button(gpio_num_t pin) {
    ESP_ERROR_CHECK(gpio_set_direction(pin, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY));
    ESP_ERROR_CHECK(gpio_set_intr_type(pin, GPIO_INTR_ANYEDGE));
}

// Filter the Joystick values and zero it
int Filter_JS(int RAW_JS) {
    int JS_ZERO = RAW_JS - JOYSTICK_CENTER;
    if (abs(JS_ZERO) <= JOYSTICK_DEADZONE) {
        JS_ZERO = 0;
    } else {
        JS_ZERO = JS_ZERO;
    }
    return JS_ZERO;
}

// Btn ISR sends the activated pin to the BtnQueue if its outside cooldown to prevent bouncing
void static IRAM_ATTR Btn_ISR(void* params) {
    uint16_t pin_num = (uint16_t)(uintptr_t)params;
    uint64_t time_read_active = esp_timer_get_time();
    if (time_read_active - isr_time_read[pin_num] >= ISR_CD) {
        isr_time_read[pin_num] = time_read_active;
        xQueueSendFromISR(ButtonQueue,&pin_num, NULL);
    }
}

// Encoder ISR sends to Encoder Queue, no cooldown
void static IRAM_ATTR Enc_ISR(void* params) {
    int DT = gpio_get_level(ENCODER_DT_PIN); // DT = 1 means the encoder moved right
    uint64_t time_read_active = esp_timer_get_time();
    if (time_read_active - isr_time_read[DT] >= ISR_CD) {
        isr_time_read[DT] = time_read_active;
        xQueueSendFromISR(EncoderQueue, &DT, NULL); // send to encoder queue
    }
}

// Processes all inputs and sends to the send queue
void ReadInputs(void *pvParameters) {
    (void)pvParameters;
    uint16_t pin_receive; // integer holding pin #
    uint8_t enc_receive; // Integer holding encoder direction
    // initialize packets for input
    // timer for joystick poll
    int64_t LAST_JS_POLL = 0;

    // ADC config
    ESP_ERROR_CHECK(adc1_config_width(ADC_WIDTH_BIT_12));
    ESP_ERROR_CHECK(adc1_config_channel_atten(ANALOGX_PIN, ADC_ATTEN_DB_12));
    ESP_ERROR_CHECK(adc1_config_channel_atten(ANALOGY_PIN, ADC_ATTEN_DB_12));


    while (1) 
    {
    while (xQueueReceive(ButtonQueue,&pin_receive,0) == pdTRUE) { // will drain queue, populates pin receive from button queue
        uint64_t time_read = esp_timer_get_time(); // time read for button packet
        int level = gpio_get_level((gpio_num_t)pin_receive); // level read for button packet

        // Fill out Send Packet
        SendPacket SendBtnPckt; 
        SendBtnPckt.packet_type = 0;
        SendBtnPckt.packet_select.Button.pin = (gpio_num_t)pin_receive;
        SendBtnPckt.packet_select.Button.ms_trig = (uint32_t)(time_read / 1000);
        SendBtnPckt.packet_select.Button.btndown = (level == 0);
        xQueueSend(SendQueue, &SendBtnPckt, pdMS_TO_TICKS(100));
    }
    while (xQueueReceive(EncoderQueue, &enc_receive, 0) == pdTRUE) {
        SendPacket SendEncPckt;
        SendEncPckt.packet_type = 2; // Packet type 2 is encoder
        SendEncPckt.packet_select.Encoder.direction = enc_receive;
        xQueueSend(SendQueue, &SendEncPckt, pdMS_TO_TICKS(100));
    }
    volatile int JSX_RAW = adc1_get_raw(ANALOGX_PIN);
    volatile int JSY_RAW = adc1_get_raw(ANALOGY_PIN);
    // milli counter for when to poll joystick
    int JS_POLL_TIMER = (esp_timer_get_time()) / 1000; // Convert microseconds to milliseconds
    if (JS_POLL_TIMER - LAST_JS_POLL >= JOYSTICK_POLL_RATE_MS) { 
        LAST_JS_POLL = JS_POLL_TIMER;
        JSX_RAW = adc1_get_raw(ANALOGX_PIN); // Read values
        JSY_RAW = adc1_get_raw(ANALOGY_PIN);
        int JSX_FILTERED = Filter_JS(JSX_RAW); // Filter values
        int JSY_FILTERED = Filter_JS(JSY_RAW);
        SendPacket SendPckt;
        SendPckt.packet_type = 1;
        SendPckt.packet_select.JoyStick.Xrange = JSX_FILTERED;
        SendPckt.packet_select.JoyStick.Yrange = JSY_FILTERED;
          xQueueSend(SendQueue, &SendPckt, pdMS_TO_TICKS(100)); //Send Packet
        }
    vTaskDelay(pdMS_TO_TICKS(10));
    } 
    // ENCODER
}

void SendInputs(void *pvParameters) { // Send inputs over ESP-NOW
    // Connect to ESP-NOW
    while(1){
    SendPacket FinalPacket;
    if (xQueueReceive(SendQueue, &FinalPacket, pdMS_TO_TICKS(10)) == pdTRUE) {
        esp_now_send(peer_address1, &FinalPacket, sizeof(FinalPacket));
        vTaskDelay(10); // just sit there diggin in yo butt twin for now
    }
    }
}
// Checks to see that packets are being picked up by another receiver to update controller status 
void ControllerSendIntegrity(const uint8_t *mac_addr, esp_now_send_status_t status) { 
    if (status == ESP_NOW_SEND_SUCCESS) {}
}

static void ReceiveDisplayText(const uint8_t *mac_addr, const uint8_t *data, int data_len) { // Esp NOW receive parameter signature
    DisplayPacket TextPacket;
    memcpy(&TextPacket, data, sizeof(DisplayPacket));
    xQueueSend(ReceiveQueue, &TextPacket, 0); // No delay inside WiFi stack task
}

void ReceiveData(void *pvParameters) { 
    while(1){

        vTaskDelay(100);
    }
}

void DisplayData(void *pvParameters) {
    DisplayPacket DisplayedText;
        while(1){
        xQueueReceive(ReceiveQueue, &DisplayedText, pdMS_TO_TICKS(100));
        vTaskDelay(10);
    }
}


void app_main(void) {

// NVS flash initialization for wifi radio
esp_err_t ret = nvs_flash_init();
if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
}
ESP_ERROR_CHECK(ret);

// Init the buttons
    init_button(BUTTON1_PIN);
    init_button(BUTTON2_PIN);
    init_button(BUTTON3_PIN);
    init_button(BUTTON4_PIN);
    init_button(ENCODER_SW_PIN);
    init_button(JOYSTICK_SW_PIN);
// Define Queues
SendQueue = xQueueCreate(25, sizeof(SendPacket));
ReceiveQueue = xQueueCreate(25, sizeof(char*)); // Pass pointer to string for dynamic memory size allocating
ButtonQueue = xQueueCreate(25,sizeof(uint16_t));
EncoderQueue = xQueueCreate(25, sizeof(int)); // DT value gets passed as int and read after being pulled from queue

//Install ISR and config GPIO pins
ESP_ERROR_CHECK(gpio_install_isr_service(0));
ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON1_PIN, Btn_ISR, (void*)(uintptr_t) BUTTON1_PIN));
ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON2_PIN, Btn_ISR, (void*)(uintptr_t) BUTTON2_PIN));
ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON3_PIN, Btn_ISR, (void*)(uintptr_t) BUTTON3_PIN));
ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON4_PIN, Btn_ISR, (void*)(uintptr_t) BUTTON4_PIN));
ESP_ERROR_CHECK(gpio_isr_handler_add(JOYSTICK_SW_PIN, Btn_ISR, (void*)(uintptr_t) JOYSTICK_SW_PIN));
ESP_ERROR_CHECK(gpio_isr_handler_add(ENCODER_SW_PIN, Btn_ISR, (void*)(uintptr_t) ENCODER_SW_PIN));

ESP_ERROR_CHECK(gpio_isr_handler_add(ENCODER_CLK_PIN, Enc_ISR, NULL));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_AP, controller_address));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_now_init());

esp_now_peer_info_t peer1 = {0};
memcpy(peer1.peer_addr, peer_address1, 6);
peer1.channel = 0;
peer1.ifidx = WIFI_IF_AP;
peer1.encrypt = false;
ESP_ERROR_CHECK(esp_now_add_peer(&peer1));

int ControllerMode = CONNECTING;

ESP_ERROR_CHECK(esp_now_register_recv_cb(ReceiveDisplayText)); // Pins Receive function to start pulling
ESP_ERROR_CHECK(esp_now_register_send_cb(ControllerSendIntegrity)); // 

ssd1306_init(&OledScreen, SCREEN_WIDTH, SCREEN_HEIGHT, I2C_NUM_1);
// Create individual tasks on cores
xTaskCreatePinnedToCore(
    ReadInputs,   // Function name
    "Read Input", // Text name
    3072,         // Stack size
    NULL,         // pvParameters (Passed here)  
    2,            // Priority
    &ReadHandle,         // Task handle
    0             // Pinned Core
);
xTaskCreatePinnedToCore(
    SendInputs,   // Function name
    "Send Input", // Text name
    3072,         // Stack size
    NULL,         // pvParameters (Passed here)  
    2,            // Priority
    &SendHandle,         // Task handle
    0             // Pinned Core
);
xTaskCreatePinnedToCore(
    ReceiveData,   // Function name
    "Receive Data", // Text name
    3072,         // Stack size
    NULL,         // pvParameters (Passed here)  
    2,            // Priority
    &ReceiveHandle,         // Task handle
    1             // Pinned Core
);
xTaskCreatePinnedToCore(
    DisplayData,   // Function name
    "Display Data", // Text name
    4096,         // Stack size
    NULL,         // pvParameters (Passed here)  
    1,            // Priority
    &DisplayHandle,         // Task handle
    1             // Pinned Core
);

}
