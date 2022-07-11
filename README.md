# Выключатель на основе ESP-NOW для ESP8266/ESP8285
Выключатель на основе ESP-NOW для ESP8266/ESP8285. Альтернативная прошивка для Tuya/SmartLife WiFi выключателей.

## Функции:

1. Никакого WiFi и сторонних серверов. Всё работает исключительно локально.
2. Прошивка может использоваться на многих выключателях Tuya/SmartLife.
3. Сохранение в памяти последнего состояния при выключении питания. Переход в последнее состояние при включении питания.
4. При подключении к шлюзу периодическая передача своего состояния доступности (Keep Alive) и статуса (ON/OFF). 
5. Возможность ручного управления с помощью кнопки (опционально).
6. Поддержка одного дополнительного внешнего цифрового датчика (DS18B20) (опционально).
7. Управление осуществляется через [ESP-NOW шлюз](https://github.com/aZholtikov/ESP-NOW-MQTT_Gateway) посредством Home Assistant через MQTT брокер.
  
## Примечание:

1. Работает на основе библиотеки [ZHNetwork](https://github.com/aZholtikov/ZHNetwork) и протокола передачи данных [ZH Smart Home Protocol](https://github.com/aZholtikov/ZH-Smart-Home-Protocol).
2. Для работы в сети необходимо наличие [ESP-NOW - MQTT Gateway](https://github.com/aZholtikov/ESP-NOW-MQTT_Gateway).
3. Для включения режима обновления прошивки необходимо послать команду "update" в корневой топик устройства (пример - "homeassistant/espnow_switch/E8DB849CA148"). Устройство перейдет в режим обновления (подробности в [API](https://github.com/aZholtikov/ZHNetwork/blob/master/src/ZHNetwork.h) библиотеки [ZHNetwork](https://github.com/aZholtikov/ZHNetwork)). Аналогично для перезагрузки послать команду "restart".
4. При возникновении вопросов/пожеланий/замечаний пишите на github@zh.com.ru

## Внимание!

Для использования этой прошивки на Tuya/SmartLife выключателях WiFi модуль должен быть заменен на ESP8266 совместимый (при необходимости).

## Пример полной конфигурации для Home Assistant:

    switch:
    - platform: mqtt
      name: "NAME"
      state_topic: "homeassistant/espnow_switch/E8DB849CA148/switch/state"
      value_template: "{{ value_json.state }}"
      command_topic: "homeassistant/espnow_switch/E8DB849CA148/switch/set"
      json_attributes_topic: "homeassistant/espnow_switch/E8DB849CA148/attributes"
      availability:
        - topic: "homeassistant/espnow_switch/E8DB849CA148/status"
      payload_on: "ON"
      payload_off: "OFF"
      optimistic: false
      qos: 2
      retain: true
    
    sensor:
    - platform: mqtt
      name: "NAME"
      device_class: "temperature"
      unit_of_measurement: "°C"
      state_topic: "homeassistant/espnow_sensor/E8DB849CA148"
      value_template: "{{ value_json.temperature }}"
      expire_after: 450
      force_update: true
      qos: 2

## Версии:

1. v1.0 Начальная версия.