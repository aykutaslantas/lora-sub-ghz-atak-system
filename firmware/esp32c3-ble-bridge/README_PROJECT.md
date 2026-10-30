# ESP32-C3 BLE köprüsü

STM32WL'de BLE olmadığı için node ile telefon arasına konan küçük bir köprü. Espressif'in NimBLE **BLE SPP server** örneği üzerine yazıldı (orijinal örnek açıklaması: [README.md](README.md)). Asıl uygulama kodu: [`main/main.c`](main/main.c).

## Görevi

- **Node → telefon:** UART'tan gelen `AA 55 LEN … CRC8` çerçevesini ayrıştırır, CRC'yi kontrol eder, payload'u (0xD1 tablo, 0xC1 komut, 0xC3 karar) **notify** ile bağlı tüm telefonlara gönderir.
- **Telefon → node:** Karakteristiğe yazılan 15 baytlık kontrol paketini (0xC1 / 0xC2) çerçeveleyip node'a yollar.

## Bağlantı

| ESP32-C3 | NUCLEO-WL55 | Not |
|---|---|---|
| GPIO4 (UART1 RX) | D1 / PB6 (USART1 TX) | Node → ESP32 |
| GPIO5 (UART1 TX) | D0 / PB7 (USART1 RX) | ESP32 → node |
| GND | GND | Ortak toprak; iki taraf da 3.3 V |

UART: **115200 8N1**.

## BLE

| | |
|---|---|
| Cihaz adı | `LoRaNode-BLE` |
| Servis | `0xABF0` |
| Karakteristik | `0xABF1` (notify + write) |

## Derleme

```bash
idf.py set-target esp32c3
idf.py build flash monitor
```

ESP-IDF v5.x ve NimBLE gerekir (`sdkconfig.defaults` NimBLE'ı açar).
