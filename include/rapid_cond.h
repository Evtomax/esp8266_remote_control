#include <Arduino.h>

#define MODE_FEEL 0B1000
#define MODE_COOL 0B0011
#define MODE_DRY  0B0010
#define MODE_FAN  0B0111
#define MODE_HEAT 0B0001

#define FAN_LOW  0B010
#define FAN_MID  0B011
#define FAN_HIGH 0B101
#define FAN_AUTO 0B000

struct state     
{                  
  bool power;
  byte mode;
  byte temperature;
  byte fan;
  bool swing;
};  

extern state currentState;
extern const state fanState;
extern const state fullHeatState;
extern const state nightCoolState;

void setIrLedPin(int pin);

void sendSettings();

