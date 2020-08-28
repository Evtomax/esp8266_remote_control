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

//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

char cond_topic[] = "cond";

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
        DynamicJsonDocument doc(1024);
        auto desResult = deserializeJson(doc, buf.get());
        if (desResult == DeserializationError::Ok){
          Serial.println("\nparsed json");

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

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started");  

  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);

  setIrLedPin(irLedPin);

}

void ledSignal(){
  led.on();
  delay(10);
  led.off();
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

      String condMqttRoot = cond_topic;
      mqttClient.subscribe((condMqttRoot + "/temp").c_str());
      mqttClient.subscribe((condMqttRoot + "/power").c_str());
      mqttClient.subscribe((condMqttRoot + "/mode").c_str());
      mqttClient.subscribe((condMqttRoot + "/swing").c_str());
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
      }
      buf[length] = 0;
      rpi1_lux = atoi(buf);
    }
  }
  else if (strcmp(topic ,((String)cond_topic + "/temp").c_str()) == 0){
    if (length < (sizeof(buf))){
      for (unsigned int i=0; i<length; ++i){
        buf[i] = payload[i];     
      }
      buf[length] = 0;
      currentState.temperature = atoi(buf);
      sendSettings();
      ledSignal();
    }   
  }
  else if (strcmp(topic ,((String)cond_topic + "/power").c_str()) == 0){
    if (payloadCompare(payload, length, "ON"))
    {
      currentState.power = 1;
      sendSettings();
    }
    else if (payloadCompare(payload, length, "OFF"))
    {
      currentState.power = 0;
      sendSettings();
    }   
    ledSignal();
  }
  else if (strcmp(topic ,((String)cond_topic + "/mode").c_str()) == 0){
    if (payloadCompare(payload, length, "COOL")){
      currentState.mode = MODE_COOL;
    }
    else if (payloadCompare(payload, length, "FAN")){
      currentState.mode = MODE_FAN;
    }
    else if (payloadCompare(payload, length, "HEAT")){
      currentState.mode = MODE_HEAT;
    }
    sendSettings();
    ledSignal();
  }
  else if (strcmp(topic ,((String)cond_topic + "/swing").c_str()) == 0){
    if (payloadCompare(payload, length, "ON"))
    {
      currentState.swing = 1;
      sendSettings();
    }
    else if (payloadCompare(payload, length, "OFF"))
    {
      currentState.swing = 0;
      sendSettings();
    }   
    ledSignal();
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
    DynamicJsonDocument doc(1024);

    doc["mqtt_server"] = mqtt_server;
    doc["mqtt_login"] = mqtt_login;
    doc["mqtt_password"] = mqtt_password;

    File configFile = LittleFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    serializeJson(doc, Serial);
    serializeJson(doc, configFile);
    configFile.close();
    shouldSaveConfig = false;
    //end save
  }

}