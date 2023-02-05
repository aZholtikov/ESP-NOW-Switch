# ESP-NOW switch for ESP8266

ESP-NOW based switch for ESP8266. Alternate firmware for Tuya/SmartLife/eWeLink WiFi switches.

## Features

1. After turn on (or after rebooting) creates an access point named "ESP-NOW switch XXXXXXXXXXXX" with password "12345678" (IP 192.168.4.1). Access point will be shown during 5 minutes. The rest of the time access point is a hidden.
2. Periodically transmission of system information (every 60 seconds), availability status (every 10 seconds) and state status (every 300 seconds) to the gateway.
3. Saves the last state when the power is turned off. Goes to the last state when the power is turned on.
4. Automatically adds switch configuration to Home Assistan via MQTT discovery as a switch.
5. Possibility firmware update over OTA (if is allows the size of the flash memory).
6. Web interface for settings.
7. Optionally support one external one wire digital climate sensor (DS18B20, DHT11 or DHT22) with automatically added sensor configuration to Home Assistan via MQTT discovery as a sensor. Periodically transmission sensor status (every 300 seconds) to the gateway.
  
## Notes

1. ESP-NOW mesh network based on the library [ZHNetwork](https://github.com/aZholtikov/ZHNetwork).
2. Regardless of the status of connection to gateway the device perform ESP-NOW node function.
3. For show the access point for setting or firmware update, send the command "update" to the device's root topic (example - "homeassistant/espnow_switch/E8DB849CA148"). Access point will be shown during 5 minutes. Similarly, for restart send the command "restart".

## Tested on

See [here](https://github.com/aZholtikov/ESP-NOW-Switch/tree/main/hardware).

## Attention

1. A gateway is required. For details see [ESP-NOW Gateway](https://github.com/aZholtikov/ESP-NOW-Gateway).
2. ESP-NOW network name must be set same of all another ESP-NOW devices in network.
3. If encryption is used, the key must be set same of all another ESP-NOW devices in network.
4. Upload the "data" folder (with web interface) into the filesystem before flashing.
5. For using this firmware on Tuya/SmartLife/eWeLink WiFi switches, the WiFi module must be replaced with an ESP8266 compatible module (if necessary).
