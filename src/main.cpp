#include "ArduinoJson.h"
#include "ArduinoOTA.h"
#include "ESPAsyncWebServer.h"
#include "Ticker.h"
#include "ZHNetwork.h"
#include "ZHConfig.h"

void onBroadcastReceiving(const char *data, const uint8_t *sender);
void onUnicastReceiving(const char *data, const uint8_t *sender);
void onConfirmReceiving(const uint8_t *target, const bool status);

void loadConfig(void);
void saveConfig(void);
void setupWebServer(void);

void buttonInterrupt(void);
void switchingRelay(void);

void sendAttributesMessage(void);
void sendKeepAliveMessage(void);
void sendConfigMessage(void);
void sendStatusMessage(void);

const String firmware{"1.11"};

String espnowNetName{"DEFAULT"};

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

bool wasMqttAvailable{false};

uint8_t gatewayMAC[6]{0};

const String payloadOn{"ON"};
const String payloadOff{"OFF"};

ZHNetwork myNet;
AsyncWebServer webServer(80);

Ticker buttonInterruptTimer;

Ticker gatewayAvailabilityCheckTimer;
bool isGatewayAvailable{false};
void gatewayAvailabilityCheckTimerCallback(void);

Ticker apModeHideTimer;
void apModeHideTimerCallback(void);

Ticker attributesMessageTimer;
bool attributesMessageTimerSemaphore{true};
void attributesMessageTimerCallback(void);
Ticker attributesMessageResendTimer;
bool attributesMessageResendTimerSemaphore{false};

Ticker keepAliveMessageTimer;
bool keepAliveMessageTimerSemaphore{true};
void keepAliveMessageTimerCallback(void);
Ticker keepAliveMessageResendTimer;
bool keepAliveMessageResendTimerSemaphore{false};

Ticker configMessageResendTimer;
bool configMessageResendTimerSemaphore{false};

Ticker statusMessageTimer;
bool statusMessageTimerSemaphore{true};
void statusMessageTimerCallback(void);
Ticker statusMessageResendTimer;
bool statusMessageResendTimerSemaphore{false};

void setup()
{
  SPIFFS.begin();

  loadConfig();

  if (relayPin)
  {
    pinMode(relayPin, OUTPUT);
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
    sendAttributesMessage();
  if (keepAliveMessageTimerSemaphore)
    sendKeepAliveMessage();
  if (statusMessageTimerSemaphore)
    sendStatusMessage();
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
    memcpy(gatewayMAC, sender, 6);
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
        sendConfigMessage();
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
      digitalWrite(relayPin, relayPinType ? relayStatus : !relayStatus);
    if (ledPin)
      digitalWrite(ledPin, ledPinType ? relayStatus : !relayStatus);
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

void onConfirmReceiving(const uint8_t *target, const bool status)
{
  if (status)
  {
    if (attributesMessageResendTimerSemaphore)
    {
      attributesMessageResendTimerSemaphore = false;
      attributesMessageResendTimer.detach();
    }
    if (keepAliveMessageResendTimerSemaphore)
    {
      keepAliveMessageResendTimerSemaphore = false;
      keepAliveMessageResendTimer.detach();
    }
    if (configMessageResendTimerSemaphore)
    {
      configMessageResendTimerSemaphore = false;
      configMessageResendTimer.detach();
    }
    if (statusMessageResendTimerSemaphore)
    {
      statusMessageResendTimerSemaphore = false;
      statusMessageResendTimer.detach();
    }
  }
}

void loadConfig()
{
  if (!SPIFFS.exists("/config.json"))
    saveConfig();
  File file = SPIFFS.open("/config.json", "r");
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
  json["system"] = "empty";
  File file = SPIFFS.open("/config.json", "w");
  serializeJsonPretty(json, file);
  file.close();
}

void setupWebServer()
{
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(SPIFFS, "/index.htm"); });

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
        if (SPIFFS.exists(request->url()))
        request->send(SPIFFS, request->url());
        else
        {
        request->send(404, "text/plain", "File Not Found");
        } });

  webServer.begin();
}

void IRAM_ATTR buttonInterrupt()
{
  ETS_GPIO_INTR_DISABLE();
  buttonInterruptTimer.once_ms(100, switchingRelay); // For prevent contact chatter.
}

void switchingRelay()
{
  relayStatus = !relayStatus;
  if (relayPin)
    digitalWrite(relayPin, relayPinType ? relayStatus : !relayStatus);
  if (ledPin)
    digitalWrite(ledPin, ledPinType ? relayStatus : !relayStatus);
  saveConfig();
  sendStatusMessage();
  ETS_GPIO_INTR_ENABLE();
}

void sendAttributesMessage()
{
  if (!isGatewayAvailable)
    return;
  attributesMessageTimerSemaphore = false;
  uint32_t secs = millis() / 1000;
  uint32_t mins = secs / 60;
  uint32_t hours = mins / 60;
  uint32_t days = hours / 24;
  esp_now_payload_data_t outgoingData{ENDT_SWITCH, ENPT_ATTRIBUTES};
  StaticJsonDocument<sizeof(esp_now_payload_data_t::message)> json;
  json["Type"] = "ESP-NOW switch";
  json["MCU"] = "ESP8266";
  json["MAC"] = myNet.getNodeMac();
  json["Firmware"] = firmware;
  json["Library"] = myNet.getFirmwareVersion();
  json["Uptime"] = "Days:" + String(days) + " Hours:" + String(hours - (days * 24)) + " Mins:" + String(mins - (hours * 60));
  char buffer[sizeof(esp_now_payload_data_t::message)]{0};
  serializeJsonPretty(json, buffer);
  memcpy(outgoingData.message, buffer, sizeof(esp_now_payload_data_t::message));
  char temp[sizeof(esp_now_payload_data_t)]{0};
  memcpy(&temp, &outgoingData, sizeof(esp_now_payload_data_t));
  myNet.sendUnicastMessage(temp, gatewayMAC, true);

  attributesMessageResendTimerSemaphore = true;
  attributesMessageResendTimer.once(1, sendAttributesMessage);
}

void sendKeepAliveMessage()
{
  if (!isGatewayAvailable)
    return;
  keepAliveMessageTimerSemaphore = false;
  esp_now_payload_data_t outgoingData{ENDT_SWITCH, ENPT_KEEP_ALIVE};
  char temp[sizeof(esp_now_payload_data_t)]{0};
  memcpy(&temp, &outgoingData, sizeof(esp_now_payload_data_t));
  myNet.sendUnicastMessage(temp, gatewayMAC, true);

  keepAliveMessageResendTimerSemaphore = true;
  keepAliveMessageResendTimer.once(1, sendKeepAliveMessage);
}

void sendConfigMessage()
{
  if (!isGatewayAvailable)
    return;
  esp_now_payload_data_t outgoingData{ENDT_SWITCH, ENPT_CONFIG};
  StaticJsonDocument<sizeof(esp_now_payload_data_t::message)> json;
  json["name"] = deviceName;
  json["unit"] = 1;
  json["type"] = HACT_SWITCH;
  json["class"] = HASWDC_SWITCH;
  json["payload_on"] = payloadOn;
  json["payload_off"] = payloadOff;
  char buffer[sizeof(esp_now_payload_data_t::message)]{0};
  serializeJsonPretty(json, buffer);
  memcpy(outgoingData.message, buffer, sizeof(esp_now_payload_data_t::message));
  char temp[sizeof(esp_now_payload_data_t)]{0};
  memcpy(&temp, &outgoingData, sizeof(esp_now_payload_data_t));
  myNet.sendUnicastMessage(temp, gatewayMAC, true);

  configMessageResendTimerSemaphore = true;
  configMessageResendTimer.once(1, sendConfigMessage);
}

void sendStatusMessage()
{
  if (!isGatewayAvailable)
    return;
  statusMessageTimerSemaphore = false;
  esp_now_payload_data_t outgoingData{ENDT_SWITCH, ENPT_STATE};
  StaticJsonDocument<sizeof(esp_now_payload_data_t::message)> json;
  json["state"] = relayStatus ? payloadOn : payloadOff;
  char buffer[sizeof(esp_now_payload_data_t::message)]{0};
  serializeJsonPretty(json, buffer);
  memcpy(&outgoingData.message, &buffer, sizeof(esp_now_payload_data_t::message));
  char temp[sizeof(esp_now_payload_data_t)]{0};
  memcpy(&temp, &outgoingData, sizeof(esp_now_payload_data_t));
  myNet.sendUnicastMessage(temp, gatewayMAC, true);

  statusMessageResendTimerSemaphore = true;
  statusMessageResendTimer.once(1, sendStatusMessage);
}

void gatewayAvailabilityCheckTimerCallback()
{
  isGatewayAvailable = false;
  memset(gatewayMAC, 0, 6);
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