# ESP32 BLE köprüsü

STM32WL'de BLE olmadığı için node ile telefon arasına konan küçük köprü. Espressif'in NimBLE **BLE SPP server** örneği üzerine yazıldı (orijinal örnek açıklaması: [README.md](README.md)). Uygulama kodu: [`main/main.c`](main/main.c). ESP32-C3 ve ESP32-S3'te aynı kod çalışır.

## Görevi

- **Node → telefon:** UART'tan gelen `AA 55 LEN … CRC8` çerçevesini çözer, CRC'yi kontrol eder, payload'u (tablo, komutlar, node durumu, katılma durumu) **notify** ile bağlı telefonlara yollar.
- **Telefon → node:** Karakteristiğe yazılan paketi çerçeveleyip node'a yollar. Kabul edilen tipler: `0xC1` işaret, `0xC2` silme isteği, `0xC4` konumum, `0xC5` OKUNDU, `0xC9` katılma, `0xCC` takımdan ayrılma.

Telefon–node arasındaki trafik düz; güvenli zarfı node ekler ve çözer.

## Bağlantı

| ESP32 | STM32WL | Not |
|---|---|---|
| GPIO4 (UART1 RX) | PB6 (USART1 TX; NUCLEO'da D1) | Node → ESP32 |
| GPIO5 (UART1 TX) | PB7 (USART1 RX; NUCLEO'da D0) | ESP32 → node |
| GND | GND | Ortak toprak; iki taraf da 3.3 V |

UART: **115200 8N1**.

## BLE

| | |
|---|---|
| Cihaz adı | `LoRaNode-BLE` |
| Servis | `0xABF0` |
| Karakteristik | `0xABF1` (notify + write) |

İki köprü yan yana çalışacaksa birinin adını değiştirin (`main.c` içinde `ble_svc_gap_device_name_set`) ve o telefondaki uygulamada `kDeviceName`'i aynı yapın; yoksa telefonlar birbirinin köprüsüne bağlanabilir.

## Derleme

```bash
idf.py set-target esp32c3      # ya da esp32s3
idf.py build flash monitor
```

ESP-IDF v6.1 ile derlendi (`nimble_peripheral_utils` bileşeni IDF örneklerinden geliyor; ESP-IDF terminalinden derleyin). `sdkconfig` hedefe göre `sdkconfig.defaults`'tan üretilir.
