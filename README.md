***Current & Previous versions can be found in Releases***

**Introduction**

This is a custom controller I have designed to interface with ESP-32 Projects. PCB layout file & images are viewable.
The board is designed as a wireless controller that communicates via ESP-NOW with other ESP-32 microcontrollers; The code is viewable, although it is not finished as of writing this. 

**Board**
The board is equipped with 4 12mm push buttons, an analog 2 axis stick + button breakout board, a KY-040 rotary encoder + button breakout board (totaling 6), and an OLED SSD1306 display to receive data back from recipient ESP-32 (optional). 
It is designed for the 38 pin ESP-32D, although it works with the 38 pin ESP-32U as well for extra range. The board can be powered by either the USB port on the ESP-32, an external power source, or with the built in battery module on the board. This includes a LiPo Battery (1000 mAh), a TP-4056 battery module for battery 
management, and a MT3608 boost converter to boost the 3.7V LiPo to the 5V required by the ESP-32. As of writing, a prototype and one revision have been made to the board layout. The board, as of the 1st revision, measures 130 x 125 mm.

**Code**
To build proper hardware to interface with this board, a few things are of note. The board uses the custom MAC address 2:1:1:1:1:1, and its recipient controller should have the MAC address of 16:16:16:16:16:16. This can be configured in the main.c file with the variables "controller_address" and "peer_address1" variables. Future functionality to possibly have multiple selectable MAC addresses during board operation is on the list for future changes.

Receiving devices need the following structs defined to decrypt incoming packets

```
typedef struct {
    gpio_num_t pin;
    bool btndown; 
    uint32_t ms_trig; // time stamp of input
} BtnInputPacket;

typedef struct {
    int Xrange;
    int Yrange;
} StickPacket;

typedef struct {
    int direction;
} EncoderPacket;

typedef struct {
    uint8_t packet_type; // 0: Button, 1: Joystick, 2: Encoder
    union {
        BtnInputPacket Button;
        StickPacket JoyStick;
        EncoderPacket Encoder;
    } packet_select;
} SendPacket;
```

With this, the packet_type will contain the general packet class, and packet_select will be one of 3 packets, populated with the input data shown in the 3 elementary structs above.
Button and encoder inputs are debounced before being sent over, and ms_trig allows for optional measurement of hold timing on the receiver end.
Standard ESP-NOW signatures are required on the receiver to receive inputs, including NVS flash init, esp now init, and WiFi init.
