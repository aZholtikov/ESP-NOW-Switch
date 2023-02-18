# Tested on

1. MOES 1CH 10A. Built on Tuya WiFi module WA2 (WB2S) (BK7231T chip). Replacement required. Performed replacement with ESP-02S (analogue of TYWE2S but with 2Mb flash). [Photo](https://github.com/aZholtikov/ESP-NOW-Switch/tree/main/hardware/MOES_1CH_10A).

```text
    Relay GPIO          GPIO12      HIGH
    Led GPIO            GPIO04      LOW
    Button GPIO         GPIO13      RISING
```

2. MINI 1CH 16A. Built on Tuya WiFi module WB2S (BK7231T chip). Replacement required. Performed replacement with ESP-02S (analogue of TYWE2S but with 2Mb flash). [Photo](https://github.com/aZholtikov/ESP-NOW-Switch/tree/main/hardware/MINI_1CH_16A).

```text
    Relay GPIO          GPIO13      HIGH
    Led GPIO            GPIO04      LOW
    Button GPIO         GPIO03      RISING
    Ext button GPIO     GPIO14      FALLING
```

3. TH 1CH 16A + SENSOR. Built on ITEAD WiFi module PSF-B85 (ESP8285 chip). Replacement not required. [Photo](https://github.com/aZholtikov/ESP-NOW-Switch/tree/main/hardware/TH_1CH_16A). Attention! Because the button is connected to GPIO00 and the firmware does not work with GPIO00 required connect GPIO00 to GPIO04 on the module.

```text
    Relay GPIO          GPIO12      HIGH
    Button GPIO         GPIO04      RISING
    Ext sensor GPIO     GPIO14      DS18B20
```

4. LIGHT E27 SOCKET (Coming soon)
