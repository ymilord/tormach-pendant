// -------------------------------------------------------------------------------------------------------
// Teensy LC based USB pendant for Tormach PathPilot
// R&D by Steve Richardson (steve.richardson@makeitlabs.com) - December 2016
//
// adds:
//   - physical CYCLE START, FEED HOLD, STOP, M01 BREAK buttons
//   - physical MAXVEL rotary encoder for better control of speeds when proving new G-Code
//   - LED feedback for state of FEED HOLD, M01 BREAK, CYCLE START
//   - stack light support for up to 4 lights (BLUE/GREEN/YELLOW/RED) - e.g. error, cycle start, etc.
//   - RFID reader interface for access control
//
// to be useful, requires a number of changes to PathPilot itself, covered elsewhere in this repository
//
// 
// builds on Arduino 1.8.5 and Teensyduino 1.40 with a modification to boards.txt
// also requires Timer library from https://github.com/JChristensen/Timer to be installed in Arduino libraries folder
//
// you must add a new type of USB endpoint called USB_EVERYTHING in the Teensy boards.txt file
// e.g. C:\Program Files (x86)\Arduino\hardware\teensy\avr\boards.txt
//
// This file will be read-only and you will need administrator access to edit/overwrite it.
// Restart the Arduino IDE once you've made this change and double check Tools->USB Type and make sure
// it shows "All The Things" (or whatever you named it)
//
//    teensyLC.menu.usb.flightsim=Flight Sim Controls
//    teensyLC.menu.usb.flightsim.build.usbtype=USB_FLIGHTSIM
//    teensyLC.menu.usb.flightsim.fake_serial=teensy_gateway
// >> teensyLC.menu.usb.rawhid=All The Things
// >> teensyLC.menu.usb.rawhid.build.usbtype=USB_EVERYTHING
//    teensyLC.menu.usb.disable=No USB
//    teensyLC.menu.usb.disable.build.usbtype=USB_DISABLED
//
// -------------------------------------------------------------------------------------------------------

#define ENCODER_USE_INTERRUPTS
#define ENCODER_OPTIMIZE_INTERRUPTS
#include <Encoder.h>

#include "Timer.h"
#include "usb_desc.h"
#include <Bounce.h>

#define PIN_BEACON_BLUE 0
#define PIN_BEACON_GREEN 1
#define PIN_BEACON_AMBER 2
#define PIN_BEACON_RED 3

#define PIN_BTN_START 4
#define PIN_BTN_STOP 6
#define PIN_BTN_FEED 7
#define PIN_BTN_M1 8

#define PIN_ENCODER_A 11
#define PIN_ENCODER_B 12

#define PIN_LED_START 5
#define PIN_LED_FEED 16
#define PIN_LED_M1 17
#define PIN_LED_RFID 21


Encoder encoder_maxvel(PIN_ENCODER_A, PIN_ENCODER_B);

enum indicator_t {
  ALERT_BEEPER,
  INDICATOR_BEACON_GREEN,
  INDICATOR_BEACON_AMBER,
  INDICATOR_BEACON_RED,
  INDICATOR_LED_START,
  INDICATOR_LED_FEED,
  INDICATOR_LED_M1,
  INDICATOR_LED_RFID,
  INDICATOR_COUNT
};

enum indicator_INDICATOR_t {
  INDICATOR_OFF,
  INDICATOR_ON,
  INDICATOR_BLINK,
  INDICATOR_PULSE
};

struct s_indicator {
  int event_id;
  int io_pin;
  bool active_level;
  int state;
};

Bounce button_start = Bounce(PIN_BTN_START, 10);
Bounce button_stop = Bounce(PIN_BTN_STOP, 10);
Bounce button_feed = Bounce(PIN_BTN_FEED, 10);
Bounce button_m1 = Bounce(PIN_BTN_M1, 10);

Timer t;
Timer alert_t;
struct s_indicator g_indicators[INDICATOR_COUNT];

byte buffer[64];

void setup() {
  Serial.begin(9600);
  Serial2.begin(9600);
  
  pinMode(PIN_BTN_START, INPUT_PULLUP);
  pinMode(PIN_BTN_STOP, INPUT_PULLUP);
  pinMode(PIN_BTN_FEED, INPUT_PULLUP);
  pinMode(PIN_BTN_M1, INPUT_PULLUP);


  init_indicator(ALERT_BEEPER, PIN_BEACON_BLUE, LOW);
  init_indicator(INDICATOR_BEACON_GREEN, PIN_BEACON_GREEN, LOW);
  init_indicator(INDICATOR_BEACON_AMBER, PIN_BEACON_AMBER, LOW);
  init_indicator(INDICATOR_BEACON_RED, PIN_BEACON_RED, LOW);
  
  init_indicator(INDICATOR_LED_START, PIN_LED_START, LOW);
  init_indicator(INDICATOR_LED_FEED, PIN_LED_FEED, LOW);
  init_indicator(INDICATOR_LED_M1, PIN_LED_M1, LOW);
  init_indicator(INDICATOR_LED_RFID, PIN_LED_RFID, LOW);

  encoder_maxvel.write(1);
}


void init_indicator(int idx, int io_pin, bool active_level)
{
    g_indicators[idx].event_id = -1;
    g_indicators[idx].io_pin = io_pin;
    g_indicators[idx].active_level = active_level;
    g_indicators[idx].state = INDICATOR_OFF;
    
    pinMode(io_pin, OUTPUT);
    digitalWrite(io_pin, ~active_level);
}

void set_indicator(int idx, int state, int period=0)
{
  s_indicator& i = g_indicators[idx];
    
  switch (state) {
    case INDICATOR_OFF:
      if (i.state != INDICATOR_OFF) {
        if (i.event_id != -1) {
          t.stop(i.event_id);
          i.event_id = -1;
        }
        digitalWrite(i.io_pin, ~(i.active_level));
        i.state = INDICATOR_OFF;
      }
      break;
    case INDICATOR_ON:
      if (i.state != INDICATOR_ON) {
        if (i.event_id != -1) {
          t.stop(i.event_id);
          i.event_id = -1;
        }
        digitalWrite(i.io_pin, i.active_level);
        i.state = INDICATOR_ON;
      }
      break;

    case INDICATOR_BLINK:
      if (i.state != INDICATOR_BLINK && i.event_id == -1) {
        i.event_id = t.oscillate(i.io_pin, period, i.active_level);
        i.state = INDICATOR_BLINK;
      }
      break;
    case INDICATOR_PULSE:
      if (i.event_id == -1) {
        i.event_id = t.pulseImmediate(i.io_pin, period, i.active_level);
        i.state = INDICATOR_PULSE;
      }
      break;
  }
}


#define RFID_STATE_WAIT_STX 0
#define RFID_STATE_GET_BYTES 1
#define RFID_STATE_GET_CKSUM 2
#define RFID_STATE_WAIT_ETX 3

void rfid_poll()
{
  byte b;
  int n;
  static byte rfid_state = RFID_STATE_WAIT_STX;
  static byte rfid_byte_count = 0;
  static byte rfid_buffer[10];
  static byte rfid_cksum[2];

  if (Serial.available()) {
    b = Serial.read();
    switch (b) {
      case '2':
          set_indicator(INDICATOR_LED_RFID, INDICATOR_BLINK, 500);
          break;
      case '1':
          set_indicator(INDICATOR_LED_RFID, INDICATOR_ON);
          break;
      case '0':
          set_indicator(INDICATOR_LED_RFID, INDICATOR_OFF);
          break;
    }
  }

  if (Serial2.available()) {
      b = Serial2.read();

      switch (rfid_state) {
        case RFID_STATE_WAIT_STX:
          if (b == 0x02) {
            rfid_state = RFID_STATE_GET_BYTES;
            rfid_byte_count = 10;
          }
          break;
        case RFID_STATE_GET_BYTES:
          rfid_buffer[10 - rfid_byte_count] = b;
          rfid_byte_count--;
          if (rfid_byte_count == 0) {
            rfid_state = RFID_STATE_GET_CKSUM;
            rfid_byte_count = 2;
          }
          break;          

        case RFID_STATE_GET_CKSUM:
          rfid_cksum[2 - rfid_byte_count] = b;
          rfid_byte_count--;
          if (rfid_byte_count == 0) {
            rfid_state = RFID_STATE_WAIT_ETX;
          }
          break;

        case RFID_STATE_WAIT_ETX:
          if (b == 0x03) {
            // got ETX
            Serial.print(0x02, BYTE); // send STX
            for (n=0; n<10; n++) {
              Serial.print(rfid_buffer[n], BYTE); // send 10 hex digits
            }
            Serial.print(0x03, BYTE); // send ETX
          }
          rfid_state = RFID_STATE_WAIT_STX;        
          break;
      }
  }
}

void button_poll()
{
  button_start.update();
  button_stop.update();
  button_feed.update();
  button_m1.update();

  // start button is active high (NC switch)
  if (button_start.risingEdge()) {
    Keyboard.set_modifier(MODIFIERKEY_ALT);
    Keyboard.send_now();

    Keyboard.set_key1(KEY_R);
    Keyboard.send_now();

    Keyboard.set_key1(0);
    Keyboard.send_now();

    Keyboard.set_modifier(0);
    Keyboard.send_now();
  }

  if (button_stop.fallingEdge()) {
    Keyboard.set_modifier(0);
    Keyboard.set_key1(KEY_ESC);
    Keyboard.send_now();

    Keyboard.set_modifier(0);
    Keyboard.set_key1(0);
    Keyboard.send_now();
  }

  if (button_feed.fallingEdge()) {
    Keyboard.set_modifier(0);
    Keyboard.set_key1(KEY_SPACE);
    Keyboard.send_now();

    Keyboard.set_modifier(0);
    Keyboard.set_key1(0);
    Keyboard.send_now();
  }
  
  if (button_m1.fallingEdge()) {
    Keyboard.set_modifier(MODIFIERKEY_ALT);
    Keyboard.send_now();

    Keyboard.set_key1(KEY_K);
    Keyboard.send_now();

    Keyboard.set_key1(0);
    Keyboard.send_now();

    Keyboard.set_modifier(0);
    Keyboard.send_now();
  }
}

void alert_off()
{
  // turn off alert
  set_indicator(ALERT_BEEPER, INDICATOR_OFF);
}

void rawhid_poll()
{
  static unsigned int packetCount = 0;
  static unsigned int run_state = -1;
  static unsigned int last_run_state = -1;
  int pathpilot_maxvel = 0;
  int maxvel = 0;
  byte maxvel_lut[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 25, 50, 75, 100};
  int n;

  n = RawHID.recv(buffer, 0); // 0 timeout = do not wait
  if (n == 64) {
    // state-run
    if (buffer[0] == 0x01) {
      set_indicator(INDICATOR_BEACON_GREEN, INDICATOR_ON);
      set_indicator(INDICATOR_LED_START, INDICATOR_ON);
      run_state = 1;
    } else if (buffer[0] == 0x02) {
      set_indicator(INDICATOR_BEACON_GREEN, INDICATOR_BLINK, 500);
      set_indicator(INDICATOR_LED_START, INDICATOR_BLINK, 500);
      run_state = 2;
    } else {
      set_indicator(INDICATOR_BEACON_GREEN, INDICATOR_OFF);
      set_indicator(INDICATOR_LED_START, INDICATOR_OFF);
      run_state = 0;
    }

    if ( last_run_state != 1 && run_state == 1 ) {
      // turn on alert for 5 seconds if going from paused or stopped to running
      set_indicator(ALERT_BEEPER, INDICATOR_ON);
      alert_t.after(5000, alert_off);      
    }

    last_run_state = run_state;
    
    // state-alarm
    if (buffer[1] == 0x01) {
      set_indicator(INDICATOR_BEACON_RED, INDICATOR_ON);
    } else if (buffer[1] == 0x02) {
      set_indicator(INDICATOR_BEACON_RED, INDICATOR_BLINK, 250);
    } else {
      set_indicator(INDICATOR_BEACON_RED, INDICATOR_OFF);
    }

    // state-maxvel-override
    pathpilot_maxvel = (int) buffer[4];

    // state-m1
    if (buffer[5] == 0x01) {
      set_indicator(INDICATOR_LED_M1, INDICATOR_ON);
    } else {
      set_indicator(INDICATOR_LED_M1, INDICATOR_OFF);
    }

    // state-feed-hold
    if (buffer[6] == 0x01) {
      set_indicator(INDICATOR_LED_FEED, INDICATOR_ON);
    } else {
      set_indicator(INDICATOR_LED_FEED, INDICATOR_OFF);
    }

    // send packet in response to the poll
    for (int i=0; i<63; i++) {
      buffer[i] = 0x00;
    }

    buffer[0] = 0x42;
    buffer[1] = 0x42;
    
    // maxvel knob
    maxvel = encoder_maxvel.read();
    if (maxvel < 0) {
      encoder_maxvel.write(0);
      maxvel = 0;
    } if (maxvel > 15) {
      encoder_maxvel.write(15);
      maxvel = 15;
    }
    
    int new_maxvel = maxvel_lut[maxvel];
    buffer[2] = highByte(new_maxvel);
    buffer[3] = lowByte(new_maxvel);
        
    if (new_maxvel == 0) {
      set_indicator(INDICATOR_BEACON_AMBER, INDICATOR_BLINK, 100);
    } else if (new_maxvel == 100) {
      set_indicator(INDICATOR_BEACON_AMBER, INDICATOR_OFF);
    } else {
      set_indicator(INDICATOR_BEACON_AMBER, INDICATOR_ON);
    }

    // and put a count of packets sent at the end
    buffer[62] = highByte(packetCount);
    buffer[63] = lowByte(packetCount);
   
    if(RawHID.send(buffer, 100) > 0) {
      packetCount = packetCount + 1;
    }
  }
}

void loop() 
{
  
  t.update();
  alert_t.update();
  rfid_poll();  
  button_poll();
  rawhid_poll();
 
}
