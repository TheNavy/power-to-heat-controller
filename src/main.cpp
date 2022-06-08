/*  Power to Heat Controller by Richard Haase
 *  Controls a TA EHS-R Heating Element via PWM
 *  0 - 3000W
 *  PWM-Settings: 400Hz-4kHz / 9-13V / 10-90% Starts @ 12% = 45W
 *  Input via MQTT
 *  (c) Richard Haase 2022
 */

// Additional Librarys
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <PubSubClient.h>
#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include "ESPAsyncWebServer.h"
#include "SPIFFS.h"
#include "config.h"

//I/O
const int PWMPin = 16;  // GPIO16

// WiFi -> defined in config.h
// const char* ssid     = "your-ssid";
// const char* password = "your-password";

// MQTT -> defined in config.h
// const char* mqtt_server = "YOUR_MQTT_BROKER_IP_ADDRESS";

WiFiClient espClient;
PubSubClient client(espClient);
long lastMsg = 0;
char msg[50];
int value = 0;

// 1-Wire
#define ONE_WIRE_BUS 15 // GPIO15 1-WireBus
float temperature_sensor1 = 0;
int temperature_sensor1_int = 0;
// Setup a oneWire instance to communicate with a OneWire device
OneWire oneWire(ONE_WIRE_BUS);
// Pass our oneWire reference to Dallas Temperature sensor 
DallasTemperature sensors(&oneWire);

DeviceAddress sensor1 = { 0x28, 0xFF, 0x77, 0x62, 0x40, 0x17, 0x4, 0x31 }; // SensorAddress

// PWM-Settings
const int PWMFreq = 1000; // 1 KHz
const int PWMChannel = 0;
const int PWMResolution = 10; // 10 Bit = 1024 Steps
const int PWMmin = 102; // 10%
const int PWMmax = 921; // 90%
int dutyCycle = 102;

// Create AsyncWebServer object on port 80
AsyncWebServer server(80);

// Additional
float available_power = 0; // Available power in Watts
float heater_power = 0; // Heater Level
bool Overheat = false; // Overheat Protection
int manualDutyCycle = -1; // Manual Control

// Replaces html-placeholder with state value
String processor(const String& var){
  String tempValue;
  if(var == "STATE"){
    if(manualDutyCycle == 100){
      tempValue = "100%";
    }
    else if(manualDutyCycle == 50){
      tempValue = "50%";
    }
    else if(manualDutyCycle == 0){
      tempValue = "0%";
    }
    else{
      tempValue = "DISABLED";
    }
    return tempValue;
  }
  if(var == "WATTS"){
    tempValue = available_power;
    return tempValue;
  }
  if(var == "LEVEL"){
    tempValue = heater_power;
    return tempValue;
  }
  return String();
}

void setup_wifi() {
  delay(10);
  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

void setup_http() {
    // Route for root / web page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });
  
  // Route to load style.css file
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/style.css", "text/css");
  });

  // Route to set Heater to 100%
  server.on("/100", HTTP_GET, [](AsyncWebServerRequest *request){
    manualDutyCycle = 100;    
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });

  // Route to set Heater to 50%
  server.on("/50", HTTP_GET, [](AsyncWebServerRequest *request){
    manualDutyCycle = 50;   
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });
  
  // Route to set Heater to 0%
  server.on("/off", HTTP_GET, [](AsyncWebServerRequest *request){
    manualDutyCycle = 0;    
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });

  // Route to set Manual-Control OFF
  server.on("/dis", HTTP_GET, [](AsyncWebServerRequest *request){
    manualDutyCycle = -1;  
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });

  // Start server
  server.begin();
}

void setup() {  
  Serial.begin(115200);  // Starts Serial Connection
  Serial.print("Booting.....");

  // Initialize SPIFFS
  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  // Setup the WiFi
  setup_wifi();

  // Initialize OTA-Update
  ArduinoOTA.begin(WiFi.localIP(), "Arduino", "password", InternalStorage);

  // Initialize PWM
  ledcSetup(PWMChannel, PWMFreq, PWMResolution);
  ledcAttachPin(PWMPin, PWMChannel);

  // Start the MQTT-Server
  client.setServer(mqtt_server, 1883);

  // Start the WebServer
  setup_http();
}

void callback(char* topic, byte* message, unsigned int length) {
  String messageTemp;
  
  for (int i = 0; i < length; i++) {
    messageTemp += (char)message[i];
  }
  if (String(topic) == "heating/water/pvheating") {
    available_power = messageTemp.toFloat();
  }
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect("ESP8266Client")) {
      Serial.println("connected");
      // Subscribe
      client.subscribe("heating/water/pvheating");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void set_heater(int tempPower) {
  float tempLvl = (100/3000) *tempPower;
  int PWMrange = PWMmax - PWMmin;
  heater_power = ((PWMrange / 100) * tempLvl) + PWMmin;
  dutyCycle = round(heater_power);

  /*
   * OVERHEAT PROTECTION!!!
   * Turns Heater off @ 75°C and back on @ 70°C
   */
  if (Overheat) {
    dutyCycle = PWMmin;
  }

  if (dutyCycle > PWMmax) {
    dutyCycle = PWMmax;
  }
  
  ledcWrite(PWMChannel, dutyCycle); // Sets the PWM-Duty-Cycle!
}

void set_heater_manual(int tempPower) {
  int PWMrange = PWMmax - PWMmin;
  heater_power = ((PWMrange / 100) * tempPower) + PWMmin;
  dutyCycle = round(heater_power);
  /*
   * OVERHEAT PROTECTION!!!
   * Turns Heater off @ 75°C and back on @ 70°C
   */
  if (Overheat) {
    dutyCycle = PWMmin;
  }

  if (dutyCycle > PWMmax) {
    dutyCycle = PWMmax;
  }
  
  ledcWrite(PWMChannel, dutyCycle); // Sets the PWM-Duty-Cycle!
}

void loop() {
 ArduinoOTA.poll(); //OTA-Updates

  if (!client.connected()) { //Checks MQTT-Connection
    reconnect();
  }
  client.loop();

  // Checks the Temperature-Sensor every 10 Seconds
  long now = millis();
  if (now - lastMsg > 10000) {
    lastMsg = now;

    // Get Temperature in Celsius
    sensors.requestTemperatures();
    temperature_sensor1 = sensors.getTempC(sensor1);
    temperature_sensor1_int = sensors.getTempC(sensor1);
    // Convert the value to a char array
    char tempString_1[8];
    dtostrf(temperature_sensor1, 1, 2, tempString_1);
    Serial.print("Sensor 1(*C): ");
    Serial.println(tempString_1);
    client.publish("heating/water/ww_temp", tempString_1);
    Serial.print("Heater-Values: Watts: ");
    Serial.print(available_power);
    Serial.print(" Duty-Cycle: ");
    Serial.println(dutyCycle);
    /*
     * OVERHEAT PROTECTION!!!
     * Turns Heater off @ 75°C and back on @ 70°C
     */
    if (temperature_sensor1_int >= 75 && Overheat == false) {
      Overheat = true;
      Serial.println("Overheat Protection Triggered");
    } else if (temperature_sensor1_int < 70 && Overheat == true) {
      Overheat = false;
      Serial.println("Overheat Protection Disabled");
    }    
  }

  if(manualDutyCycle >= 0){
    set_heater_manual(manualDutyCycle);
  }else{
    set_heater(available_power);
  }
  
}