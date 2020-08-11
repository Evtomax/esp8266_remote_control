#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <WiFiClient.h>

#include <PubSubClient.h>

#include "LittleFS.h"

#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson  

#include "rapid_cond.h"

const int configPin = D7;
const int irLedPin = D6;

//WiFiManager wifiManager;
//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

char mqtt_server[40] = "";
char mqtt_login[40] = "";
char mqtt_password[40] = "";

WiFiManagerParameter custom_mqtt_server("mqtt_server", "mqtt server", mqtt_server, 40);
WiFiManagerParameter custom_mqtt_login("mqtt_login", "mqtt login", mqtt_login, 40);
WiFiManagerParameter custom_mqtt_password("mqtt_password", "mqtt password", mqtt_password, 40, " type=password");

void mqttCallback(char* topic, byte* payload, unsigned int length);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

ESP8266WebServer server(80);

int rpi1_lux = 0;
int nightLightLuxThr = 5;

const int nightLightDuration = 10*60*1000;

class Switch
{
  public:
    Switch(int pin, bool inverse = false)
    {
      Switch::pin = pin;
      Switch::inverse = inverse;
      pinMode(pin, OUTPUT);
      if (Switch::inverse)
      {
        digitalWrite(pin, HIGH);      
      }
      else
      {
        digitalWrite(pin, LOW);   
      }
    }
    bool toggle()
    {
      bool newPinState = !digitalRead(pin);
      digitalWrite(pin, newPinState);
      return newPinState;
    }
    void on()
    {
      if (Switch::inverse)
      {
        digitalWrite(pin, LOW);      
      }
      else
      {
        digitalWrite(pin, HIGH);   
      }
    }
    void off()
    {
      if (Switch::inverse)
      {
        digitalWrite(pin, HIGH);      
      }
      else
      {
        digitalWrite(pin, LOW);   
      }
    }    
  protected:
    int pin;
    bool inverse;
};

class TimerSwitch : public Switch
{
  public:
    TimerSwitch(int pin, bool inverse = false) : Switch(pin, inverse)
    {     
    }

    void tick(){
      if (smoothOnStart)
      {
        if ((millis() - start_ms) < smooth_ms){
          analogWrite(pin, 1024*(millis() - start_ms)/smooth_ms);
        }
        else{
          smoothOnStart = false;
          on();
        }
      }
      else if (tempOnStart){
        if (millis() - start_ms > temp_ms){
          off();
          tempOnStart = false;
          smoothOffStart = false;
        }
        else if (smoothOffStart){
          if (digitalRead(pin) == HIGH){
            if ((millis() - start_ms) > (temp_ms - smooth_ms)){
              analogWrite(pin, 1024*((temp_ms - (millis() - start_ms)))/smooth_ms);
            }
          }
        }
      }
    }

    void tempOn(unsigned long on_ms){
      if (digitalRead(pin) == LOW){
        start_ms = millis();
        temp_ms = on_ms;
        on();
        tempOnStart = true;
      }
    }

    void smoothOn(unsigned long smoothMs = 1000){
      if (digitalRead(pin) == LOW){
        if (!smoothOnStart){
          smoothOnStart = true;
          start_ms = millis();
          smooth_ms = smoothMs;
        }
      }
    }

    void tempSmoothOn(unsigned long tempMs, unsigned long smoothMs = 1000){
      if (digitalRead(pin) == LOW){
        smoothOn(smoothMs);
        temp_ms = tempMs;
        tempOnStart = true;  
        smoothOffStart = true;   
      }
      else{       
        if (!smoothOnStart){
          temp_ms = tempMs;
          start_ms = millis();
        }
      }
    }


  private:
    unsigned long start_ms = 0;
    unsigned long temp_ms = 0;
    unsigned long smooth_ms = 0;
    bool tempOnStart = false;
    bool smoothOnStart = false;
    bool smoothOffStart = false;
  
};

Switch led(LED_BUILTIN, true);
TimerSwitch nightLight(D1);



void handleRoot() {
//  digitalWrite(led, 1);
  server.send(200, "text/plain", "hello from esp8266!");
//  digitalWrite(led, 0);
}

void handleRapidCondNightCool() {
  currentState = nightCoolState;
  sendSettings();
  server.send(200, "text/html", "NightCool");
}

void handleRapidCond22() {
  currentState.temperature = 22;
  sendSettings();
  server.send(200, "text/html", "22");
}

void handleRapidCondOn() {
  currentState.power = 1;
  sendSettings();
  server.send(200, "text/html", "on");
}

void handleRapidCondOff() {
  currentState.power = 0;
  sendSettings();
  server.send(200, "text/html", "off");
}

void handleNotFound() {
  //digitalWrite(led, 1);
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
 // digitalWrite(led, 0);
}

inline void configPortalHandle() {
  if ( digitalRead(configPin) == LOW ) {
    server.close();
    server.stop();
    WiFiManager wifiManager;
    wifiManager.addParameter(&custom_mqtt_server);
    wifiManager.addParameter(&custom_mqtt_login);
    wifiManager.addParameter(&custom_mqtt_password);
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    if (!wifiManager.startConfigPortal("OnDemandAP")) {
      Serial.println("failed to connect and hit timeout");
      delay(3000);
      //reset and try again, or maybe put it to deep sleep
      ESP.reset();
      delay(5000);
    }    

    Serial.println("connected...yeey :)");

  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_login, custom_mqtt_login.getValue());
  strcpy(mqtt_password, custom_mqtt_password.getValue());

  }
}

void setup(){
  Serial.begin(115200);
  Serial.println("Booting");

  //read configuration from FS json
  Serial.println("mounting FS...");

  if (LittleFS.begin()) {
    Serial.println("mounted file system");
    if (LittleFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = LittleFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        //DynamicJsonBuffer jsonBuffer;
        //JsonObject& json = jsonBuffer.parseObject(buf.get());
        //json.printTo(Serial);
        DynamicJsonDocument doc(1024);
        auto desResult = deserializeJson(doc, buf.get());
        //if (json.success()) {
        if (desResult == DeserializationError::Ok){
          Serial.println("\nparsed json");

          //strcpy(mqtt_server, json["mqtt_server"]);
          //strcpy(mqtt_password, json["mqtt_password"]);
          if (doc.containsKey("mqtt_server")){
            strcpy(mqtt_server, doc["mqtt_server"]);
          }
          if (doc.containsKey("mqtt_login")){
            strcpy(mqtt_login, doc["mqtt_login"]);
          }
          if (doc.containsKey("mqtt_password")){
            strcpy(mqtt_password, doc["mqtt_password"]);
          }

        } else {
          Serial.println("failed to load json config");
        }
        configFile.close();
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }

  WiFi.mode(WIFI_STA);

  pinMode(configPin, INPUT);

  //wifiManager.autoConnect();

  // Port defaults to 8266
  // ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  //ArduinoOTA.setHostname("esp8266_switch1");

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();

  server.on("/", handleRoot);
  
  server.on("/inline", []() {
    server.send(200, "text/plain", "this works as well");
  });  

  server.on("/ledtoggle", []() {
    led.toggle();
    server.send(200, "text/plain", "ok");
  });

  server.on("/d1toggle", []() {
    nightLight.toggle();
    server.send(200, "text/plain", "ok");
  });

  server.on("/condOn", handleRapidCondOn);  
  server.on("/condOff", handleRapidCondOff);  
  server.on("/condNightCool", handleRapidCondNightCool); 
  server.on("/cond22", handleRapidCond22);

  server.on("/gif", []() {
    static const uint8_t gif[] PROGMEM = {
      0x47, 0x49, 0x46, 0x38, 0x37, 0x61, 0x10, 0x00, 0x10, 0x00, 0x80, 0x01,
      0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x2c, 0x00, 0x00, 0x00, 0x00,
      0x10, 0x00, 0x10, 0x00, 0x00, 0x02, 0x19, 0x8c, 0x8f, 0xa9, 0xcb, 0x9d,
      0x00, 0x5f, 0x74, 0xb4, 0x56, 0xb0, 0xb0, 0xd2, 0xf2, 0x35, 0x1e, 0x4c,
      0x0c, 0x24, 0x5a, 0xe6, 0x89, 0xa6, 0x4d, 0x01, 0x00, 0x3b
    };
    char gif_colored[sizeof(gif)];
    memcpy_P(gif_colored, gif, sizeof(gif));
    // Set the background to a random set of colors
    gif_colored[16] = millis() % 256;
    gif_colored[17] = millis() % 256;
    gif_colored[18] = millis() % 256;
    server.send(200, "image/gif", gif_colored, sizeof(gif_colored));
  });

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started");  

  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);

  //pinMode(irLedPin, OUTPUT);
  //digitalWrite(irLedPin, HIGH);
  setIrLedPin(irLedPin);

}

void mqttReconnect() {
  // Loop until we're reconnected
  while (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Create a random client ID
    String clientId = "ESP8266Client-remote";
    //clientId += String(random(0xffff), HEX);
    // Attempt to connect
    if (mqttClient.connect(clientId.c_str(), mqtt_login, mqtt_password)) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      //mqttClient.publish("outTopic", "hello world");
      // ... and resubscribe
      mqttClient.subscribe("/remote/light");
      mqttClient.subscribe("/remote/led");
      mqttClient.subscribe("/all_lights");
      mqttClient.subscribe("/rpi1_sensors/motion");
      mqttClient.subscribe("/rpi1_sensors/lux");
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

bool payloadCompare(byte* payload, unsigned int length, const char* str){
  if (length != strlen(str)){
    return false;
  }
  for (unsigned int i=0; i<length; ++i){
    if (payload[i] != str[i]){
      return false;
    }
  }
  return true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // handle message arrived
  //const char* light_topic = "/remote/light";
  char buf[10];
  Serial.println(topic);
  for (int i = 0; i<length; ++i){
    Serial.write(payload[i]);
  }
  Serial.println();
  Switch* sw = nullptr;
  if (strcmp(topic, "/remote/light") == 0){
    sw = &nightLight;
  }
  else if (strcmp(topic, "/remote/led") == 0){
    sw = &led;
  }
  else if (strcmp(topic, "/all_lights") == 0){
    if (payloadCompare(payload, length, "ON"))
    {
      led.on();
      nightLight.on();
      mqttClient.publish("/remote/light", "ON");
      mqttClient.publish("/remote/led", "ON");
    }
    else if (payloadCompare(payload, length, "OFF"))
    {
      led.off();
      nightLight.off();
      mqttClient.publish("/remote/light", "OFF");
      mqttClient.publish("/remote/led", "OFF");
    }    
  }
  else if (strcmp(topic, "/rpi1_sensors/motion") == 0){
    if (payloadCompare(payload, length, "DETECTED")){
      if (rpi1_lux < nightLightLuxThr){
        nightLight.tempSmoothOn(nightLightDuration, 2000);
      }
    }
  }
  else if (strcmp(topic, "/rpi1_sensors/lux") == 0){
    if (length < (sizeof(buf))){
      for (unsigned int i=0; i<length; ++i){
        buf[i] = payload[i];
        rpi1_lux = atoi(buf);
      }
      buf[length] = 0;
    }
  }

  if (sw != nullptr){
    if (payloadCompare(payload, length, "ON"))
    {
      sw->on();
    }
    else if (payloadCompare(payload, length, "OFF"))
    {
      sw->off();
    }
  }
}



void loop() {

  configPortalHandle();
  
  ArduinoOTA.handle();
  server.handleClient();

  if (!mqttClient.connected()) {
    mqttReconnect();
  }
  mqttClient.loop();

  nightLight.tick();

    //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    //DynamicJsonBuffer jsonBuffer;
    //JsonObject& json = jsonBuffer.createObject();
    DynamicJsonDocument doc(1024);

    //json["mqtt_server"] = mqtt_server;
    //json["mqtt_password"] = mqtt_password;
    doc["mqtt_server"] = mqtt_server;
    doc["mqtt_login"] = mqtt_login;
    doc["mqtt_password"] = mqtt_password;

    File configFile = LittleFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    //json.printTo(Serial);
    //json.printTo(configFile);
    serializeJson(doc, Serial);
    serializeJson(doc, configFile);
    configFile.close();
    shouldSaveConfig = false;
    //end save
  }

}