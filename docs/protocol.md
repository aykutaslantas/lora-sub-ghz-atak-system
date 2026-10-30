# Protokol ve paket formatları

Tüm çok baytlı alanlar **little-endian**. Koordinatlar `derece × 1e7` olarak `int32` taşınır (ör. 40.9830600 → `409830600`).

## 1. Adres alanı (16 bit)

| Adres | Anlamı |
|---|---|
| `0x0000` | Telefon (yalnızca BLE/UART üzerinde, havada kullanılmaz) |
| `0x0001 … 0xFFEF` | Node'lar. Varsayılan olarak çip UID'sinden üretilir |
| `0xFFF0 … 0xFFFD` | Ayrılmış |
| `0xFFFE` | Admin (Zybo) |
| `0xFFFF` | Herkes (broadcast) |

Node adresi, `NODE_ADDR` elle verilmezse şöyle hesaplanır:

```c
uint32_t uid = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();
myAddr = (uint16_t)(uid ^ (uid >> 16));
```

Sonuç 0 ise 1 yapılır, ayrılmış aralığa düşerse `0x1000` çıkarılır.

## 2. LoRa (hava) paketleri

Radyo ayarları: **869.525 MHz**, LoRa **SF7**, **BW 125 kHz**, CR **4/5**, preamble 8, değişken uzunluk. Paket türü **ilk bayt ve uzunluktan** ayırt edilir.

### 2.1 Konum paketi: `pos_packet_t` (12 B, node → herkes)

| Offset | Alan | Tip | Açıklama |
|---|---|---|---|
| 0 | `addr` | u16 | Gönderen node |
| 2 | `seq` | u8 | Her pakette +1, kayıp tespiti için |
| 3 | `lat` | i32 | Enlem × 1e7 |
| 7 | `lon` | i32 | Boylam × 1e7 |
| 11 | `flags` | u8 | bit0 = GPS fix var |

Periyot `TX_PERIOD_MS = 5000`, üzerine `[0, TX_JITTER_MS=1000)` ms rastgele gecikme eklenir (çakışma önleme).

### 2.2 Konum tablosu: `0xD1` (gateway → herkes)

```
[0xD1][N][kayıt 1 (12 B)] … [kayıt N]      boy = 2 + 12·N,  1 ≤ N ≤ 21
```

Her kayıt bir `pos_packet_t`. N = 21 iken toplam 254 bayt, LoRa'nın 255 bayt sınırına sığar.

### 2.3 Kontrol paketleri (15 B)

| Offset | Alan | Tip |
|---|---|---|
| 0 | `type` | u8: `0xC1`, `0xC2` veya `0xC3` |
| 1 | `src` | u16 |
| 3 | `dst` | u16 (`0xFFFF` = herkes) |
| 5 | `msg_id` | u8, gönderen başına sayaç |
| 6..14 | türe göre | aşağıda |

**`0xC1` İşaret / komut** (telefon → node → gateway → herkes)

| Offset | Alan | Açıklama |
|---|---|---|
| 6 | `kind` | 1 ilgi noktası, 2 tehlike, 3 toplanma, 4 buraya gel, 5 burada bekle, 6 bölgeyi kontrol et, **7 geri dön** (yalnızca admin → tek node) |
| 7 | `lat` | i32 |
| 11 | `lon` | i32 |

**`0xC2` Silme isteği** (node → gateway → **yalnızca Zybo**, havada tekrar yayınlanmaz)

`[6]=0`, `[7..8]` = silinecek işaretin `src`'si, `[9]` = silinecek işaretin `msg_id`'si.

**`0xC3` Admin kararı** (Zybo → gateway → 3× yayın → tüm node'lar → telefonlar)

`src = 0xFFFE`, `[6]` = karar (1 onay, 2 red), `[7..8]` = hedef `src`, `[9]` = hedef `msg_id`, `[10]` = isteğin `msg_id`'si.

**Tekrar yayın ve eleme:** Gateway her yeni `0xC1`/`0xC3`'ü `CMD_RELAY_COUNT = 3` kez, aralarda `400 ms + [0, 200) ms` boşlukla yayınlar. Alıcılar `(type, src, msg_id)` üçlüsünü 16 elemanlı bir halka tamponda tutar ve tekrarları atar.

## 3. UART çerçevesi (node ↔ ESP32, gateway ↔ Zybo)

Tüm UART bağlantıları aynı çerçeveyi kullanır:

```
[0xAA][0x55][LEN][PAYLOAD … (LEN bayt)][CRC8]
CRC8: poly 0x07, init 0x00, LEN + PAYLOAD üzerinden (sync baytları hariç)
```

Alıcı tarafı byte byte çalışan bir durum makinesi (`SYNC0 → SYNC1 → LEN → DATA → CRC`). `AA AA 55` gibi kaymalarda senkronu kaybetmeden toparlanır, CRC hatalı çerçeveyi atar.

| Yön | Payload | LEN | Toplam |
|---|---|---|---|
| Gateway → Zybo, konum | `pos_packet_t` + RSSI (i8) + SNR (i8) | `0x0E` (14) | 18 B |
| Gateway → Zybo, komut (`GW_FORWARD_CMD=1`) | 15 B kontrol paketi + RSSI + SNR | `0x11` (17) | 21 B |
| Zybo → Gateway, tablo | `0xD1` tablo (2 + 12·N) | 2 + 12·N | |
| Zybo → Gateway, komut | 15 B `0xC1` / `0xC3` | `0x0F` (15) | 19 B |
| Node → ESP32 | `0xD1` tablo veya 15 B kontrol paketi | | |
| ESP32 → Node | Telefondan gelen 15 B `0xC1` / `0xC2` | `0x0F` | 19 B |

Örnek (Zybo → gateway, admin'den herkese "tehlike" işareti; `src=0xFFFE`, `dst=0xFFFF`, `msg_id=0x21`, `kind=2`):
`AA 55 0F C1 FE FF FF FF 21 02 E1 C8 6D 18 9E D8 32 11 C4`

## 4. BLE (ESP32 ↔ telefon)

| | Değer |
|---|---|
| Cihaz adı | `LoRaNode-BLE` |
| Servis UUID (16 bit) | `0xABF0` |
| Karakteristik UUID | `0xABF1` (notify + write) |

ESP32, node'dan gelen çerçevenin CRC'sini kontrol eder ve **yalnızca payload'u** (sync, LEN ve CRC olmadan) abone olan tüm telefonlara notify ile gönderir. Telefondan yazılan 15 baytlık kontrol paketini çerçeveleyip node'a iletir.
