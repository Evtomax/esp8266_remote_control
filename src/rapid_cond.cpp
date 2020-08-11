#include "rapid_cond.h"



byte startCode[] = {
  B00100011, B11001011, B00100110, B00000010, B00000000, B00000000, B00000000, 
  B00000000, B00000000, B00000000, B00000000, B00000000, B00000000, B00100101
};

byte settingsCode[] = {
  B00100011, B11001011, B00100110, B00000001, B00000000, B00100100, B00000111, 
  B00001000, B00000101, B00000000, B00000000, B00000000, B10000000, B00000000
};


state currentState = {1, MODE_COOL, 27, FAN_HIGH, 0};
const state fanState = {1, MODE_FAN, 27, FAN_HIGH, 0};
const state fullHeatState = {1, MODE_HEAT, 31, FAN_HIGH, 0}; 
const state nightCoolState = {1, MODE_COOL, 18, FAN_HIGH, 0};

int irLedPin = 0;

void setIrLedPin(int pin){
    irLedPin = pin;
    pinMode(irLedPin, OUTPUT);
    digitalWrite(irLedPin, LOW);
}

void impulse(int count) {
  // T= ~ 26uS on ESP8266 Generic Board, real freq is 37687Hz
  // duration = 26uS * count
  for(byte i=0; i<count; i++){
    digitalWrite(irLedPin, HIGH);
    delayMicroseconds(15);
    digitalWrite(irLedPin, LOW);
    delayMicroseconds(8);
  }
}

inline void beginSend(){
  impulse(119); // ~3099 uS
  delayMicroseconds(1604);
}

inline void bitBegin(){
  impulse(19); // ~505 uS
}

inline void send1(){
  bitBegin();
  delayMicroseconds(1089);
}

inline void send0(){
  bitBegin();
  delayMicroseconds(318);
}

inline void endSend(){
  impulse(19); // ~505 uS
}


void sendCode(byte code[]){
  beginSend();
  for(byte i=0; i<14; i++){
    byte chunk = code[i];
    for(byte j=0; j<8; j++){
      if (chunk & 0B00000001){
        send1();
      }else{
        send0();
      }
      chunk = chunk>>1;
    }
  }
  endSend();
}

void sendSettings(){

  if (currentState.power){ // проверено
    settingsCode[5]|=0B00000100;
  }else{
    settingsCode[5]&=0B11111011;
  }

  settingsCode[6]&=0B11110000; // проверено
  settingsCode[6]|=currentState.mode;

  settingsCode[7]&=0B11110000; // проверено
  settingsCode[7]|=15-(currentState.temperature-16);

  settingsCode[8]&=0B11111000;
  settingsCode[8]|=currentState.fan;

  settingsCode[8]&=0B11000111; // проверено
  if (currentState.swing){
    settingsCode[8]|=0B00111000;
  }

  //settingsCode[8]|=0B00100000; // Проверено 101, 110, 011, 001, 010, 100 Секретов нет :-(

  byte crc = 0;
  for(byte i=0; i<13; i++){
    crc+=settingsCode[i];
  }
  settingsCode[13] = crc;
  
  sendCode(startCode);
  delay(70);
  sendCode(settingsCode);
}

bool swingDownInProgress = 0;
unsigned long startSwingMillis;
unsigned long stopSwingMillis;
const unsigned long swingDelayDuration = 4000;
const unsigned long swingDuration = 10800;
void swingDownProgress(){
  if (swingDownInProgress){
    unsigned long curMillis = millis();
    if (curMillis>stopSwingMillis){
      if (currentState.swing){
        currentState.swing = 0;
        sendSettings();
        swingDownInProgress = 0;
      }
    }else{
      if (curMillis>startSwingMillis){
        if (!currentState.swing){
          currentState.swing = 1;
          sendSettings();
        }
      }
    }
  }
}

void swingDown(){
  
}