#include "ArduinoJson.h"
#include "ArduinoOTA.h"
#include "ESPAsyncWebServer.h" // https://github.com/aZholtikov/Async-Web-Server
#include "LittleFS.h"
#include "Ticker.h"
#include "DallasTemperature.h"
#include "DHTesp.h"
#include "ZHNetwork.h"
#include "ZHConfig.h"

void onBroadcastReceiving(const char *data, const uint8_t *sender);
void onUnicastReceiving(const char *data, const uint8_t *sender);
void onConfirmReceiving(const uint8_t *target, const uint16_t id, const bool status);

void loadConfig(void);
void saveConfig(void);
void setupWebServer(void);

void buttonInterrupt(void);
void switchingRelay(void);

void sendAttributesMessage(const uint8_t type = ENST_NONE);
void sendKeepAliveMessage(void);
void sendConfigMessage(const uint8_t type = ENST_NONE);
void sendStatusMessage(const uint8_t type = ENST_NONE);

typedef struct
{
  uint16_t id{0};
  char message[200]{0};
} espnow_message_t;

std::vector<espnow_message_t> espnowMessage;

const String firmware{"1.32"};

String espnowNetName{"DEFAULT"};

uint8_t workMode{0};

String deviceName = "ESP-NOW switch " + String(ESP.getChipId(), HEX);

bool relayStatus{false};
uint8_t relayPin{0};
uint8_t relayPinType{1};
uint8_t buttonPin{0};
uint8_t buttonPinType{0};
uint8_t extButtonPin{0};
uint8_t extButtonPinType{0};
uint8_t ledPin{0};
uint8_t ledPinType{0};

uint8_t sensorPin{0};
uint8_t sensorType{0};

bool wasMqttAvailable{false};

uint8_t gatewayMAC[6]{0};

const String payloadOn{"ON"};
const String payloadOff{"OFF"};

ZHNetwork myNet;
AsyncWebServer webServer(80);

OneWire oneWire;
DallasTemperature ds18b20(&oneWire);

DHTesp dht;

Ticker buttonInterruptTimer;

Ticker gatewayAvailabilityCheckTimer;
bool isGatewayAvailable{false};
void gatewayAvailabilityCheckTimerCallback(void);

Ticker apModeHideTimer;
void apModeHideTimerCallback(void);

Ticker attributesMessageTimer;
bool attributesMessageTimerSemaphore{true};
void attributesMessageTimerCallback(void);

Ticker keepAliveMessageTimer;
bool keepAliveMessageTimerSemaphore{true};
void keepAliveMessageTimerCallback(void);

Ticker statusMessageTimer;
bool statusMessageTimerSemaphore{true};
void statusMessageTimerCallback(void);

void setup()
{
  LittleFS.begin();

  loadConfig();

  if (sensorPin)
  {
    if (sensorType == ENST_DS18B20)
      oneWire.begin(sensorPin);
    if (sensorType == ENST_DHT11 || sensorType == ENST_DHT22)
      dht.setup(sensorPin, DHTesp::AUTO_DETECT);
  }
  if (relayPin)
  {
    pinMode(relayPin, OUTPUT);
    if (workMode)
      digitalWrite(relayPin, relayPinType ? !relayStatus : relayStatus);
    else
      digitalWrite(relayPin, relayPinType ? relayStatus : !relayStatus);
  }
  if (ledPin)
  {
    pinMode(ledPin, OUTPUT);
    digitalWrite(ledPin, ledPinType ? relayStatus : !relayStatus);
  }
  if (buttonPin)
    attachInterrupt(buttonPin, buttonInterrupt, buttonPinType ? RISING : FALLING);
  if (extButtonPin)
    attachInterrupt(extButtonPin, buttonInterrupt, extButtonPinType ? RISING : FALLING);

  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  myNet.begin(espnowNetName.c_str());
  // myNet.setCryptKey("VERY_LONG_CRYPT_KEY"); // If encryption is used, the key must be set same of all another ESP-NOW devices in network.

  myNet.setOnBroadcastReceivingCallback(onBroadcastReceiving);
  myNet.setOnUnicastReceivingCallback(onUnicastReceiving);
  myNet.setOnConfirmReceivingCallback(onConfirmReceiving);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(("ESP-NOW switch " + String(ESP.getChipId(), HEX)).c_str(), "12345678", 1, 0);
  apModeHideTimer.once(300, apModeHideTimerCallback);

  setupWebServer();

  ArduinoOTA.begin();

  attributesMessageTimer.attach(60, attributesMessageTimerCallback);
  keepAliveMessageTimer.attach(10, keepAliveMessageTimerCallback);
  statusMessageTimer.attach(300, statusMessageTimerCallback);
}

void loop()
{
  if (attributesMessageTimerSemaphore)
  {
    sendAttributesMessage();
    if (sensorPin)
      sendAttributesMessage(sensorType);
  }
  if (keepAliveMessageTimerSemaphore)
    sendKeepAliveMessage();
  if (statusMessageTimerSemaphore)
  {
    sendStatusMessage();
    if (sensorPin)
      sendStatusMessage(sensorType);
  }
  myNet.maintenance();
  ArduinoOTA.handle();
}

void onBroadcastReceiving(const char *data, const uint8_t *sender)
{
  esp_now_payload_data_t incomingData;
  memcpy(&incomingData, data, sizeof(esp_now_payload_data_t));
  if (incomingData.deviceType != ENDT_GATEWAY)
    return;
  if (myNet.macToString(gatewayMAC) != myNet.macToString(sender) && incomingData.payloadsType == ENPT_KEEP_ALIVE)
    memcpy(&gatewayMAC, sender, 6);
  if (myNet.macToString(gatewayMAC) == myNet.macToString(sender) && incomingData.payloadsType == ENPT_KEEP_ALIVE)
  {
    isGatewayAvailable = true;
    StaticJsonDocument<sizeof(esp_now_payload_data_t::message)> json;
    deserializeJson(json, incomingData.message);
    bool temp = json["MQTT"] == "online" ? true : false;
    if (wasMqttAvailable != temp)
    {
      wasMqttAvailable = temp;
      if (temp)
      {
        sendConfigMessage();
        if (sensorPin)
          sendConfigMessage(sensorType);
        sendAttributesMessage();
        if (sensorPin)
          sendAttributesMessage(sensorType);
        sendStatusMessage();
        if (sensorPin)
          sendStatusMessage(sensorType);
      }
    }
    gatewayAvailabilityCheckTimer.once(15, gatewayAvailabilityCheckTimerCallback);
  }
}

void onUnicastReceiving(const char *data, const uint8_t *sender)
{
  esp_now_payload_data_t incomingData;
  memcpy(&incomingData, data, sizeof(esp_now_payload_data_t));
  if (incomingData.deviceType != ENDT_GATEWAY || myNet.macToString(gatewayMAC) != myNet.macToString(sender))
    return;
  StaticJsonDocument<sizeof(esp_now_payload_data_t::message)> json;
  if (incomingData.payloadsType == ENPT_SET)
  {
    deserializeJson(json, incomingData.message);
    relayStatus = json["set"] == payloadOn ? true : false;
    if (relayPin)
    {
      if (workMode)
        digitalWrite(relayPin, relayPinType ? !relayStatus : relayStatus);
      else
        digitalWrite(relayPin, relayPinType ? relayStatus : !relayStatus);
    }
    if (ledPin)
    {
      if (workMode)
        digitalWrite(ledPin, ledPinType ? !relayStatus : relayStatus);
      else
        digitalWrite(ledPin, ledPinType ? relayStatus : !relayStatus);
    }
    saveConfig();
    sendStatusMessage();
  }
  if (incomingData.payloadsType == ENPT_UPDATE)
  {
    WiFi.softAP(("ESP-NOW switch " + String(ESP.getChipId(), HEX)).c_str(), "12345678", 1, 0);
    webServer.begin();
    apModeHideTimer.once(300, apModeHideTimerCallback);
  }
  if (incomingData.payloadsType == ENPT_RESTART)
    ESP.restart();
}

void onConfirmReceiving(const uint8_t *target, const uint16_t id, const bool status)
{
  for (uint16_t i{0}; i < espnowMessage.size(); ++i)
  {
    espnow_message_t message = espnowMessage[i];
    if (message.id == id)
    {
      if (status)
        espnowMessage.erase(espnowMessage.begin() + i);
      else
      {
        message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);
        espnowMessage.at(i) = message;
      }
    }
  }
}

void loadConfig()
{
  if (!LittleFS.exists("/config.json"))
    saveConfig();
  File file = LittleFS.open("/config.json", "r");
  String jsonFile = file.readString();
  StaticJsonDocument<512> json;
  deserializeJson(json, jsonFile);
  espnowNetName = json["espnowNetName"].as<String>();
  deviceName = json["deviceName"].as<String>();
  relayStatus = json["relayStatus"];
  relayPin = json["relayPin"];
  relayPinType = json["relayPinType"];
  buttonPin = json["buttonPin"];
  buttonPinType = json["buttonPinType"];
  extButtonPin = json["extButtonPin"];
  extButtonPinType = json["extButtonPinType"];
  ledPin = json["ledPin"];
  ledPinType = json["ledPinType"];
  sensorPin = json["sensorPin"];
  sensorType = json["sensorType"];
  workMode = json["workMode"];
  file.close();
}

void saveConfig()
{
  StaticJsonDocument<512> json;
  json["firmware"] = firmware;
  json["espnowNetName"] = espnowNetName;
  json["deviceName"] = deviceName;
  json["relayStatus"] = relayStatus;
  json["relayPin"] = relayPin;
  json["relayPinType"] = relayPinType;
  json["buttonPin"] = buttonPin;
  json["buttonPinType"] = buttonPinType;
  json["extButtonPin"] = extButtonPin;
  json["extButtonPinType"] = extButtonPinType;
  json["ledPin"] = ledPin;
  json["ledPinType"] = ledPinType;
  json["sensorPin"] = sensorPin;
  json["sensorType"] = sensorType;
  json["workMode"] = workMode;
  json["system"] = "empty";
  File file = LittleFS.open("/config.json", "w");
  serializeJsonPretty(json, file);
  file.close();
}

void setupWebServer()
{
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(LittleFS, "/index.htm"); });

  webServer.on("/setting", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        relayPin = request->getParam("relayPin")->value().toInt();
        relayPinType = request->getParam("relayPinType")->value().toInt();
        buttonPin = request->getParam("buttonPin")->value().toInt();
        buttonPinType = request->getParam("buttonPinType")->value().toInt();
        extButtonPin = request->getParam("extButtonPin")->value().toInt();
        extButtonPinType = request->getParam("extButtonPinType")->value().toInt();
        ledPin = request->getParam("ledPin")->value().toInt();
        ledPinType = request->getParam("ledPinType")->value().toInt();
        sensorPin = request->getParam("sensorPin")->value().toInt();
        sensorType = request->getParam("sensorType")->value().toInt();
        workMode = request->getParam("workMode")->value().toInt();
        deviceName = request->getParam("deviceName")->value();
        espnowNetName = request->getParam("espnowNetName")->value();
        request->send(200);
        saveConfig(); });

  webServer.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        request->send(200);
        ESP.restart(); });

  webServer.onNotFound([](AsyncWebServerRequest *request)
                       { 
        if (LittleFS.exists(request->url()))
        request->send(LittleFS, request->url());
        else
        {
        request->send(404, "text/plain", "File Not Found");
        } });

  webServer.begin();
}

void IRAM_ATTR buttonInterrupt()
{
  ETS_GPIO_INTR_DISABLE();
  buttonInterruptTimer.once_ms(500, switchingRelay); // For prevent contact chatter.
}

void switchingRelay()
{
  relayStatus = !relayStatus;
  if (relayPin)
  {
    if (workMode)
      digitalWrite(relayPin, relayPinType ? !relayStatus : relayStatus);
    else
      digitalWrite(relayPin, relayPinType ? relayStatus : !relayStatus);
  }
  if (ledPin)
  {
    if (workMode)
      digitalWrite(ledPin, ledPinType ? !relayStatus : relayStatus);
    else
      digitalWrite(ledPin, ledPinType ? relayStatus : !relayStatus);
  }
  saveConfig();
  sendStatusMessage();
  ETS_GPIO_INTR_ENABLE();
}

void sendAttributesMessage(const uint8_t type)
{
  if (!isGatewayAvailable)
    return;
  attributesMessageTimerSemaphore = false;
  uint32_t secs = millis() / 1000;
  uint32_t mins = secs / 60;
  uint32_t hours = mins / 60;
  uint32_t days = hours / 24;
  esp_now_payload_data_t outgoingData{ENDT_SWITCH, ENPT_ATTRIBUTES};
  espnow_message_t message;
  StaticJsonDocument<sizeof(esp_now_payload_data_t::message)> json;
  if (type == ENST_NONE)
    json["Type"] = "ESP-NOW switch";
  else
  {
    outgoingData.deviceType = ENDT_SENSOR;
    json["Type"] = getValueName(esp_now_sensor_type_t(type));
  }
  json["MCU"] = "ESP8266";
  json["MAC"] = myNet.getNodeMac();
  json["Firmware"] = firmware;
  json["Library"] = myNet.getFirmwareVersion();
  json["Uptime"] = "Days:" + String(days) + " Hours:" + String(hours - (days * 24)) + " Mins:" + String(mins - (hours * 60));
  serializeJsonPretty(json, outgoingData.message);
  memcpy(&message.message, &outgoingData, sizeof(esp_now_payload_data_t));
  message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);

  espnowMessage.push_back(message);
}

void sendKeepAliveMessage()
{
  if (!isGatewayAvailable)
    return;
  keepAliveMessageTimerSemaphore = false;
  esp_now_payload_data_t outgoingData{ENDT_SWITCH, ENPT_KEEP_ALIVE};
  espnow_message_t message;
  memcpy(&message.message, &outgoingData, sizeof(esp_now_payload_data_t));
  message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);

  espnowMessage.push_back(message);
}

void sendConfigMessage(const uint8_t type)
{
  if (!isGatewayAvailable)
    return;
  esp_now_payload_data_t outgoingData{ENDT_SWITCH, ENPT_CONFIG};
  espnow_message_t message;
  StaticJsonDocument<sizeof(esp_now_payload_data_t::message)> json;
  if (type == ENST_NONE)
  {
    json["name"] = deviceName;
    json["unit"] = 1;
    json["type"] = HACT_SWITCH;    // ha_component_type_t
    json["class"] = HASWDC_SWITCH; // ha_switch_device_class_t
    json["template"] = "state";    // value_template
    json["payload_on"] = payloadOn;
    json["payload_off"] = payloadOff;
  }
  if (type == ENST_DS18B20 || type == ENST_DHT11 || type == ENST_DHT22)
  {
    outgoingData.deviceType = ENDT_SENSOR;
    json["name"] = deviceName + " temperature";
    json["unit"] = 2;
    json["type"] = HACT_SENSOR;        // ha_component_type_t
    json["class"] = HASDC_TEMPERATURE; // ha_sensor_device_class_t
    json["template"] = "temperature";  // value_template
    json["meas"] = "°C";               // unit_of_measurement
    json["time"] = 900;                // expire_after
  }
  serializeJsonPretty(json, outgoingData.message);
  memcpy(&message.message, &outgoingData, sizeof(esp_now_payload_data_t));
  message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);

  espnowMessage.push_back(message);

  if (type == ENST_DHT11 || type == ENST_DHT22)
  {
    outgoingData.deviceType = ENDT_SENSOR;
    json["name"] = deviceName + " humidity";
    json["unit"] = 3;
    json["type"] = HACT_SENSOR;     // ha_component_type_t
    json["class"] = HASDC_HUMIDITY; // ha_sensor_device_class_t
    json["template"] = "humidity";  // value_template
    json["meas"] = "%";             // unit_of_measurement
    json["time"] = 900;             // expire_after

    serializeJsonPretty(json, outgoingData.message);
    memcpy(&message.message, &outgoingData, sizeof(esp_now_payload_data_t));
    message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);

    espnowMessage.push_back(message);
  }
}

void sendStatusMessage(const uint8_t type)
{
  if (!isGatewayAvailable)
    return;
  statusMessageTimerSemaphore = false;
  esp_now_payload_data_t outgoingData{ENDT_SWITCH, ENPT_STATE};
  espnow_message_t message;
  StaticJsonDocument<sizeof(esp_now_payload_data_t::message)> json;
  if (type == ENST_NONE)
    json["state"] = relayStatus ? payloadOn : payloadOff;
  if (type == ENST_DS18B20)
  {
    outgoingData.deviceType = ENDT_SENSOR;
    ds18b20.requestTemperatures();
    json["temperature"] = int8_t(ds18b20.getTempCByIndex(0));
  }
  if (type == ENST_DHT11 || type == ENST_DHT22)
  {
    outgoingData.deviceType = ENDT_SENSOR;
    json["temperature"] = int8_t(dht.getTemperature());
    json["humidity"] = int8_t(dht.getHumidity());
  }
  serializeJsonPretty(json, outgoingData.message);
  memcpy(&message.message, &outgoingData, sizeof(esp_now_payload_data_t));
  message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);

  espnowMessage.push_back(message);
}

void gatewayAvailabilityCheckTimerCallback()
{
  isGatewayAvailable = false;
  memset(&gatewayMAC, 0, 6);
  espnowMessage.clear();
}

void apModeHideTimerCallback()
{
  WiFi.softAP(("ESP-NOW switch " + String(ESP.getChipId(), HEX)).c_str(), "12345678", 1, 1);
  webServer.end();
}

void attributesMessageTimerCallback()
{
  attributesMessageTimerSemaphore = true;
}

void keepAliveMessageTimerCallback()
{
  keepAliveMessageTimerSemaphore = true;
}

void statusMessageTimerCallback()
{
  statusMessageTimerSemaphore = true;
}