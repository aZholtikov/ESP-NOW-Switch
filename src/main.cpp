#include "ArduinoJson.h"
#include "LittleFS.h"
#include "Ticker.h"
#include "ZHNetwork.h"
#include "ZHSmartHomeProtocol.h"

//***********************КОНФИГУРАЦИЯ***********************//
// Раскомментируйте только один если необходимо управление кнопкой:
//#define MANUAL_SWITCHING_POSEDGE // High нажата / Low отжата.
//#define MANUAL_SWITCHING_NEGEDGE // Low нажата / High отжата.

// Раскомментируйте если необходимо использовать внешний датчик:
//#define DS18B20

// Укажите соответствующие GPIO:
#define RELAY_PIN 16
//#define LED_PIN 4 // Закомментируйте если не используется.
#if defined(MANUAL_SWITCHING_POSEDGE) || defined(MANUAL_SWITCHING_NEGEDGE)
#define BUTTON_PIN 13
#endif
#if defined(DS18B20)
#define SENSOR_PIN 2
#endif

const char *myNetName{"SMART"}; // Укажите имя сети ESP-NOW.
//***********************************************************//

#if defined(DS18B20)
#include "OneWire.h"
#include "DallasTemperature.h"
#endif

void onBroadcastReceiving(const char *data, const byte *sender);
void onUnicastReceiving(const char *data, const byte *sender);
void loadStatus(void);
void saveStatus(void);
void restart(void);
void buttonInterrupt(void);
void manualRelaySwitching(void);
void attributesMessage(void);
void keepAliveMessage(void);
void statusMessage(void);
void sensorMessage(void);

const String firmware{"1.0"};
bool relayStatus{false};
bool sensorMessageSemaphore{false};
byte gatewayMAC[6]{0};

ZHNetwork myNet;
#if defined(DS18B20)
OneWire oneWire(SENSOR_PIN);
DallasTemperature ds18b20Sensor(&oneWire);
Ticker sensorMessageTimer;
#endif
#if defined(MANUAL_SWITCHING_POSEDGE) || defined(MANUAL_SWITCHING_NEGEDGE)
Ticker buttonInterruptTimer;
#endif
Ticker attributesMessageTimer;
Ticker keepAliveMessageTimer;
Ticker statusMessageTimer;
Ticker restartTimer;

void setup()
{
#if defined(MANUAL_SWITCHING_POSEDGE) || defined(MANUAL_SWITCHING_NEGEDGE)
  ETS_GPIO_INTR_DISABLE();
  ETS_GPIO_INTR_ATTACH(buttonInterrupt, NULL);
#if defined(MANUAL_SWITCHING_POSEDGE) && defined(BUTTON_PIN)
  gpio_pin_intr_state_set(GPIO_ID_PIN(BUTTON_PIN), GPIO_PIN_INTR_POSEDGE);
#endif
#if defined(MANUAL_SWITCHING_NEGEDGE) && defined(BUTTON_PIN)
  gpio_pin_intr_state_set(GPIO_ID_PIN(BUTTON_PIN), GPIO_PIN_INTR_NEGEDGE);
#endif
  ETS_GPIO_INTR_ENABLE();
#endif
  pinMode(RELAY_PIN, OUTPUT);
#if defined(LED_PIN)
  pinMode(LED_PIN, OUTPUT);
#endif
  LittleFS.begin();
  loadStatus();
  digitalWrite(RELAY_PIN, relayStatus);
#if defined(LED_PIN)
  digitalWrite(LED_PIN, !relayStatus);
#endif
  myNet.begin(myNetName);
  myNet.setOnBroadcastReceivingCallback(onBroadcastReceiving);
  myNet.setOnUnicastReceivingCallback(onUnicastReceiving);
}

void loop()
{
#if defined(DS18B20)
  if (sensorMessageSemaphore)
  {
    sensorMessageSemaphore = false;
    PayloadsData outgoingData{SENSOR, STATE};
    StaticJsonDocument<sizeof(outgoingData.message)> json;
    ds18b20Sensor.requestTemperatures();
    json["temperature"] = int(ds18b20Sensor.getTempCByIndex(0));
    char buffer[sizeof(outgoingData.message)];
    serializeJsonPretty(json, buffer);
    os_memcpy(outgoingData.message, buffer, sizeof(outgoingData.message));
    char temp[sizeof(PayloadsData)];
    os_memcpy(temp, &outgoingData, sizeof(PayloadsData));
    myNet.sendUnicastMessage(temp, gatewayMAC);
  }
#endif
  myNet.maintenance();
}

void onBroadcastReceiving(const char *data, const uint8_t *sender)
{
  PayloadsData incomingData;
  os_memcpy(&incomingData, data, sizeof(PayloadsData));
  if (incomingData.deviceType != GATEWAY || myNet.macToString(gatewayMAC) == myNet.macToString(sender))
    return;
  if (incomingData.payloadsType == KEEP_ALIVE)
  {
    os_memcpy(gatewayMAC, sender, 6);
    attributesMessage();
    keepAliveMessage();
    statusMessage();
    attributesMessageTimer.attach(3600, attributesMessage);
    keepAliveMessageTimer.attach(60, keepAliveMessage);
    statusMessageTimer.attach(300, statusMessage);
#if defined(DS18B20)
    sensorMessage();
    sensorMessageTimer.attach(300, sensorMessage);
#endif
  }
}

void onUnicastReceiving(const char *data, const uint8_t *sender)
{
  PayloadsData incomingData;
  os_memcpy(&incomingData, data, sizeof(PayloadsData));
  if (incomingData.deviceType != GATEWAY || myNet.macToString(gatewayMAC) != myNet.macToString(sender))
    return;
  StaticJsonDocument<sizeof(incomingData.message)> json;
  if (incomingData.payloadsType == SET)
  {
    deserializeJson(json, incomingData.message);
    relayStatus = json["set"] == "ON" ? true : false;
    digitalWrite(RELAY_PIN, relayStatus);
#if defined(LED_PIN)
    digitalWrite(LED_PIN, !relayStatus);
#endif
    saveStatus();
    statusMessage();
  }
  if (incomingData.payloadsType == UPDATE)
  {
#if defined(MANUAL_SWITCHING_POSEDGE) || defined(MANUAL_SWITCHING_NEGEDGE)
    ETS_GPIO_INTR_DISABLE();
#endif
    digitalWrite(RELAY_PIN, LOW);
#if defined(LED_PIN)
    digitalWrite(LED_PIN, HIGH);
#endif
    myNet.update();
    attributesMessageTimer.detach();
    keepAliveMessageTimer.detach();
    statusMessageTimer.detach();
#if defined(DS18B20)
    sensorMessageTimer.detach();
#endif
    restartTimer.once(300, restart);
  }
  if (incomingData.payloadsType == RESTART)
    restart();
}

void loadStatus()
{
  if (!LittleFS.exists("/status.json"))
  {
    saveStatus();
    return;
  }
  File file = LittleFS.open("/status.json", "r");
  String jsonFile = file.readString();
  StaticJsonDocument<124> json;
  deserializeJson(json, jsonFile);
  relayStatus = json["status"];
  file.close();
}

void saveStatus()
{
  StaticJsonDocument<124> json;
  json["status"] = relayStatus;
  File file = LittleFS.open("/status.json", "w");
  serializeJsonPretty(json, file);
  file.close();
}

void restart()
{
  ESP.restart();
}

#if defined(MANUAL_SWITCHING_POSEDGE) || defined(MANUAL_SWITCHING_NEGEDGE) || defined(BUTTON_PIN)
void buttonInterrupt()
{
  ETS_GPIO_INTR_DISABLE();
  buttonInterruptTimer.once(1, manualRelaySwitching);
}

void manualRelaySwitching()
{
  uint32_t gpioStatus = GPIO_REG_READ(GPIO_STATUS_ADDRESS);
  GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, gpioStatus);
  relayStatus = !relayStatus;
  digitalWrite(RELAY_PIN, relayStatus);
#if defined(LED_PIN)
  digitalWrite(LED_PIN, !relayStatus);
#endif
  saveStatus();
  statusMessage();
  ETS_GPIO_INTR_ENABLE();
}
#endif

void attributesMessage()
{
  PayloadsData outgoingData{SWITCH, ATTRIBUTES};
  StaticJsonDocument<sizeof(outgoingData.message)> json;
#if defined(TYPE8266)
  json["MCU"] = "ESP8266";
#endif
#if defined(TYPE8285)
  json["MCU"] = "ESP8285";
#endif
  json["MAC"] = myNet.getNodeMac();
  json["Firmware"] = firmware;
  json["Library"] = myNet.getFirmwareVersion();
  char buffer[sizeof(outgoingData.message)];
  serializeJsonPretty(json, buffer);
  os_memcpy(outgoingData.message, buffer, sizeof(outgoingData.message));
  char temp[sizeof(PayloadsData)];
  os_memcpy(temp, &outgoingData, sizeof(PayloadsData));
  myNet.sendUnicastMessage(temp, gatewayMAC);
}

void keepAliveMessage()
{
  PayloadsData outgoingData{SWITCH, KEEP_ALIVE};
  char temp[sizeof(PayloadsData)];
  os_memcpy(temp, &outgoingData, sizeof(PayloadsData));
  myNet.sendUnicastMessage(temp, gatewayMAC);
}

void statusMessage()
{
  PayloadsData outgoingData{SWITCH, STATE};
  StaticJsonDocument<sizeof(outgoingData.message)> json;
  json["state"] = relayStatus ? "ON" : "OFF";
  char buffer[sizeof(outgoingData.message)];
  serializeJsonPretty(json, buffer);
  os_memcpy(outgoingData.message, buffer, sizeof(outgoingData.message));
  char temp[sizeof(PayloadsData)];
  os_memcpy(temp, &outgoingData, sizeof(PayloadsData));
  myNet.sendUnicastMessage(temp, gatewayMAC);
}

#if defined(DS18B20)
void sensorMessage()
{
  sensorMessageSemaphore = true;
}
#endif