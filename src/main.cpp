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

//I/O
const int PWMPin = 16;  // GPIO16

// WiFi
const char* ssid     = "your-ssid";
const char* password = "your-password";

// MQTT
const char* mqtt_server = "YOUR_MQTT_BROKER_IP_ADDRESS";

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

// Additional
float available_power = 0; // Available power in Watts
float heater_power = 0; // Heater Level
bool Overheat = false; // Overheat Protection

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

void setup() {  
  Serial.begin(115200);  // Starts Serial Connection
  Serial.print("Booting.....");

  setup_wifi(); // Setup the WiFi

  ArduinoOTA.begin(WiFi.localIP(), "Arduino", "password", InternalStorage); // Setup the OTA Update

  // PWM-Init
  ledcSetup(PWMChannel, PWMFreq, PWMResolution);
  ledcAttachPin(PWMPin, PWMChannel);

  client.setServer(mqtt_server, 1883);
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

  set_heater(available_power);
}
