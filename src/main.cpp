#include "ArduinoJson.h"
#include "ArduinoOTA.h"
#include "ESPAsyncWebServer.h" // https://github.com/aZholtikov/Async-Web-Server
#include "LittleFS.h"
#include "EEPROM.h"
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
void loadStatus(void);
void saveStatus(void);
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

struct deviceConfig
{
  String espnowNetName{"DEFAULT"};
  uint8_t workMode{0};
  String deviceName = "ESP-NOW switch " + String(ESP.getChipId(), HEX);
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
} config;

std::vector<espnow_message_t> espnowMessage;

const String firmware{"1.42"};

bool relayStatus{false};

bool wasMqttAvailable{false};

uint8_t gatewayMAC[6]{0};

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
  loadStatus();

  if (config.sensorPin)
  {
    if (config.sensorType == ENST_DS18B20)
      oneWire.begin(config.sensorPin);
    if (config.sensorType == ENST_DHT11 || config.sensorType == ENST_DHT22)
      dht.setup(config.sensorPin, DHTesp::AUTO_DETECT);
  }
  if (config.relayPin)
  {
    pinMode(config.relayPin, OUTPUT);
    if (config.workMode)
      digitalWrite(config.relayPin, config.relayPinType ? !relayStatus : relayStatus);
    else
      digitalWrite(config.relayPin, config.relayPinType ? relayStatus : !relayStatus);
  }
  if (config.ledPin)
  {
    pinMode(config.ledPin, OUTPUT);
    digitalWrite(config.ledPin, config.ledPinType ? relayStatus : !relayStatus);
  }
  if (config.buttonPin)
    attachInterrupt(config.buttonPin, buttonInterrupt, config.buttonPinType ? RISING : FALLING);
  if (config.extButtonPin)
    attachInterrupt(config.extButtonPin, buttonInterrupt, config.extButtonPinType ? RISING : FALLING);

  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  myNet.begin(config.espnowNetName.c_str());
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
    if (config.sensorPin)
      sendAttributesMessage(config.sensorType);
  }
  if (keepAliveMessageTimerSemaphore)
    sendKeepAliveMessage();
  if (statusMessageTimerSemaphore)
  {
    sendStatusMessage();
    if (config.sensorPin)
      sendStatusMessage(config.sensorType);
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
    DynamicJsonDocument json(sizeof(esp_now_payload_data_t::message));
    deserializeJson(json, incomingData.message);
    bool temp = json["MQTT"] == "online" ? true : false;
    if (wasMqttAvailable != temp)
    {
      wasMqttAvailable = temp;
      if (temp)
      {
        sendConfigMessage();
        if (config.sensorPin)
          sendConfigMessage(config.sensorType);
        sendAttributesMessage();
        if (config.sensorPin)
          sendAttributesMessage(config.sensorType);
        sendStatusMessage();
        if (config.sensorPin)
          sendStatusMessage(config.sensorType);
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
  DynamicJsonDocument json(sizeof(esp_now_payload_data_t::message));
  if (incomingData.payloadsType == ENPT_SET)
  {
    deserializeJson(json, incomingData.message);
    relayStatus = json["set"] == "ON" ? true : false;
    if (config.relayPin)
    {
      if (config.workMode)
        digitalWrite(config.relayPin, config.relayPinType ? !relayStatus : relayStatus);
      else
        digitalWrite(config.relayPin, config.relayPinType ? relayStatus : !relayStatus);
    }
    if (config.ledPin)
    {
      if (config.workMode)
        digitalWrite(config.ledPin, config.ledPinType ? !relayStatus : relayStatus);
      else
        digitalWrite(config.ledPin, config.ledPinType ? relayStatus : !relayStatus);
    }
    saveStatus();
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
  ETS_GPIO_INTR_DISABLE();
  EEPROM.begin(4096);
  if (EEPROM.read(4095) == 254)
  {
    EEPROM.get(0, config);
    EEPROM.end();
  }
  else
  {
    EEPROM.end();
    saveConfig();
  }
  delay(50);
  ETS_GPIO_INTR_ENABLE();
}

void saveConfig()
{
  ETS_GPIO_INTR_DISABLE();
  EEPROM.begin(4096);
  EEPROM.write(4095, 254);
  EEPROM.put(0, config);
  EEPROM.end();
  delay(50);
  ETS_GPIO_INTR_ENABLE();
}

void loadStatus(void)
{
  ETS_GPIO_INTR_DISABLE();
  if (!LittleFS.exists("/status.json"))
    saveStatus();
  File file = LittleFS.open("/status.json", "r");
  String jsonFile = file.readString();
  DynamicJsonDocument json(64); // To calculate the buffer size uses https://arduinojson.org/v6/assistant.
  deserializeJson(json, jsonFile);
  relayStatus = json["status"];
  file.close();
  delay(50);
  ETS_GPIO_INTR_ENABLE();
}

void saveStatus(void)
{
  ETS_GPIO_INTR_DISABLE();
  DynamicJsonDocument json(48); // To calculate the buffer size uses https://arduinojson.org/v6/assistant.
  json["status"] = relayStatus;
  json["system"] = "empty";
  File file = LittleFS.open("/status.json", "w");
  serializeJsonPretty(json, file);
  file.close();
  delay(50);
  ETS_GPIO_INTR_ENABLE();
}

void setupWebServer()
{
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(LittleFS, "/index.htm"); });

  webServer.on("/function.js", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(LittleFS, "/function.js"); });

  webServer.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(LittleFS, "/style.css"); });

  webServer.on("/setting", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        config.relayPin = request->getParam("relayPin")->value().toInt();
        config.relayPinType = request->getParam("relayPinType")->value().toInt();
        config.buttonPin = request->getParam("buttonPin")->value().toInt();
        config.buttonPinType = request->getParam("buttonPinType")->value().toInt();
        config.extButtonPin = request->getParam("extButtonPin")->value().toInt();
        config.extButtonPinType = request->getParam("extButtonPinType")->value().toInt();
        config.ledPin = request->getParam("ledPin")->value().toInt();
        config.ledPinType = request->getParam("ledPinType")->value().toInt();
        config.sensorPin = request->getParam("sensorPin")->value().toInt();
        config.sensorType = request->getParam("sensorType")->value().toInt();
        config.workMode = request->getParam("workMode")->value().toInt();
        config.deviceName = request->getParam("deviceName")->value();
        config.espnowNetName = request->getParam("espnowNetName")->value();
        request->send(200);
        saveConfig(); });

  webServer.on("/config", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        String configJson;
        DynamicJsonDocument json(384); // To calculate the buffer size uses https://arduinojson.org/v6/assistant.
        json["firmware"] = firmware;
        json["espnowNetName"] = config.espnowNetName;
        json["deviceName"] = config.deviceName;
        json["relayPin"] = config.relayPin;
        json["relayPinType"] = config.relayPinType;
        json["buttonPin"] = config.buttonPin;
        json["buttonPinType"] = config.buttonPinType;
        json["extButtonPin"] = config.extButtonPin;
        json["extButtonPinType"] = config.extButtonPinType;
        json["ledPin"] = config.ledPin;
        json["ledPinType"] = config.ledPinType;
        json["sensorPin"] = config.sensorPin;
        json["sensorType"] = config.sensorType;
        json["workMode"] = config.workMode;
        serializeJsonPretty(json, configJson);
        request->send(200, "application/json", configJson); });

  webServer.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request)
               {request->send(200);
        ESP.restart(); });

  webServer.onNotFound([](AsyncWebServerRequest *request)
                       { request->send(404, "text/plain", "File Not Found"); });

  webServer.begin();
}

void IRAM_ATTR buttonInterrupt()
{
  ETS_GPIO_INTR_DISABLE();
  buttonInterruptTimer.once_ms(500, switchingRelay); // For prevent contact chatter.
}

void switchingRelay()
{
  ETS_GPIO_INTR_ENABLE();
  relayStatus = !relayStatus;
  if (config.relayPin)
  {
    if (config.workMode)
      digitalWrite(config.relayPin, config.relayPinType ? !relayStatus : relayStatus);
    else
      digitalWrite(config.relayPin, config.relayPinType ? relayStatus : !relayStatus);
  }
  if (config.ledPin)
  {
    if (config.workMode)
      digitalWrite(config.ledPin, config.ledPinType ? !relayStatus : relayStatus);
    else
      digitalWrite(config.ledPin, config.ledPinType ? relayStatus : !relayStatus);
  }
  saveStatus();
  sendStatusMessage();
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
  DynamicJsonDocument json(sizeof(esp_now_payload_data_t::message));
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
  DynamicJsonDocument json(sizeof(esp_now_payload_data_t::message));
  if (type == ENST_NONE)
  {
    json[MCMT_DEVICE_NAME] = config.deviceName;
    json[MCMT_DEVICE_UNIT] = 1;
    json[MCMT_COMPONENT_TYPE] = HACT_SWITCH;
    json[MCMT_DEVICE_CLASS] = HASWDC_SWITCH;
    json[MCMT_VALUE_TEMPLATE] = "state";
  }
  if (type == ENST_DS18B20 || type == ENST_DHT11 || type == ENST_DHT22)
  {
    outgoingData.deviceType = ENDT_SENSOR;
    json[MCMT_DEVICE_NAME] = config.deviceName + " temperature";
    json[MCMT_DEVICE_UNIT] = 2;
    json[MCMT_COMPONENT_TYPE] = HACT_SENSOR;
    json[MCMT_DEVICE_CLASS] = HASDC_TEMPERATURE;
    json[MCMT_VALUE_TEMPLATE] = "temperature";
    json[MCMT_UNIT_OF_MEASUREMENT] = "°C";
    json[MCMT_EXPIRE_AFTER] = 900;
  }
  serializeJsonPretty(json, outgoingData.message);
  memcpy(&message.message, &outgoingData, sizeof(esp_now_payload_data_t));
  message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);

  espnowMessage.push_back(message);

  if (type == ENST_DHT11 || type == ENST_DHT22)
  {
    outgoingData.deviceType = ENDT_SENSOR;
    json[MCMT_DEVICE_NAME] = config.deviceName + " humidity";
    json[MCMT_DEVICE_UNIT] = 3;
    json[MCMT_COMPONENT_TYPE] = HACT_SENSOR;
    json[MCMT_DEVICE_CLASS] = HASDC_HUMIDITY;
    json[MCMT_VALUE_TEMPLATE] = "humidity";
    json[MCMT_UNIT_OF_MEASUREMENT] = "%";
    json[MCMT_EXPIRE_AFTER] = 900;

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
  DynamicJsonDocument json(sizeof(esp_now_payload_data_t::message));
  if (type == ENST_NONE)
    json["state"] = relayStatus ? "ON" : "OFF";
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