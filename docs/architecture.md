# Mimari

## Gereksinimler

- Sistem **tamamen yerel** çalışmalı: internet, bulut ya da hücresel şebeke yok.
- Kişi sayısı sabit değil (hedef ~30 node).
- Admin istasyonu tanınmayan cihazları **donanım seviyesinde** filtrelemeli.
- Saha kullanıcısı telefonundan herkesi haritada görebilmeli, işaret koyabilmeli, admin'den komut alabilmeli.

## Bileşenler

```mermaid
flowchart TB
    subgraph Node["Node (kişi başına)"]
        direction LR
        APP["Flutter app<br/>harita, işaretler"] -- BLE --> ESP["ESP32-C3<br/>BLE GATT ↔ UART"]
        ESP -- "USART1<br/>PB6/PB7" --> STM["STM32WL55<br/>SubGHz radyo"]
    end
    STM -. "LoRa 869.525 MHz" .-> GW["Gateway<br/>(aynı firmware, GATEWAY_MODE=1)"]
    GW -- "USART1" --> PL
    subgraph Zybo["Zybo Z7-20"]
        PL["PL<br/>UART RX → parser → whitelist (BRAM)<br/>konum tablosu → downlink TX"] -- AXI --> PS["PS / PetaLinux<br/>admin GUI (HDMI)"]
    end
```

## Uplink: konum

```mermaid
sequenceDiagram
    participant N as Node
    participant G as Gateway
    participant Z as Zybo PL
    loop her 5 s + jitter
        N->>G: LoRa pos_packet_t (12 B)
        G->>Z: UART AA 55 0E … RSSI SNR CRC8 (18 B)
        Z->>Z: CRC kontrol, adres whitelist'te mi?
    end
```

Gateway'de **adres filtrelemesi yok**. Hangi node'un kabul edileceğine Zybo'nun PL tarafı karar verir; böylece whitelist tek yerde, donanımda tutulur ve Linux'a sadece onaylı veriler ulaşır.

## Downlink: konum tablosu

```mermaid
sequenceDiagram
    participant Z as Zybo
    participant G as Gateway
    participant N as Node
    participant E as ESP32
    participant P as Telefon
    Z->>G: UART AA 55 LEN [D1 N kayıtlar] CRC8
    G->>N: LoRa 0xD1 tablo (2 + 12·N B)
    N->>E: UART AA 55 LEN [D1 …] CRC8
    E->>P: BLE notify [D1 …]
    P->>P: haritadaki node'ları güncelle
```

## Komutlar ve işaretler

```mermaid
sequenceDiagram
    participant P as Telefon
    participant N as Node
    participant G as Gateway
    participant X as Diğer node'lar
    participant Z as Zybo
    P->>N: BLE write 0xC1 (via ESP32)
    N->>G: LoRa 0xC1
    G-->>Z: UART (GW_FORWARD_CMD=1 ise)
    G->>X: 3× tekrar yayın
    G->>N: (kendi kopyası = "ağa ulaştı" onayı)
    X->>X: (src,msg_id) tekrar eleme → telefona ilet
```

- **0xC2 silme isteği** havada tekrar yayınlanmaz, yalnızca Zybo'ya gider.
- **0xC3 admin kararı** gateway tarafından 3× yayınlanır, telefonlar ilgili işareti haritadan kaldırır.
- **Kind 7 "geri dön"** yalnızca admin'den tek bir node'a gönderilir; diğer node'lar bunu kendi telefonlarına iletmez.

## Firmware modları

| Bayrak | Değer | Etki |
|---|---|---|
| `GATEWAY_MODE` | 0 / 1 | Node mu gateway mi |
| `VIRTUAL_NODES` | 1–8 | Test için tek kartın birden çok node gibi yayın yapması |
| `GATEWAY_LOCAL_TABLE` | 0 / 1 | Zybo yokken gateway tabloyu kendisi tutar ve 10 s'de bir yayınlar |
| `NODE_UART_BRIDGE` | 0 / 1 | Node'un tabloyu ESP32'ye iletmesi (açıkken STOP2 kapanır) |
| `GW_FORWARD_CMD` | 0 / 1 | Gateway'in komutları Zybo'ya iletmesi |

Bu bayraklar sayesinde sistem parça parça test edilebildi: önce iki NUCLEO arasında ping-pong, sonra Zybo'suz yerel tablo, sonra sanal node'larla çok node'lu senaryo, en son gerçek Zybo ile.

## Tasarım kararları

| Karar | Gerekçe |
|---|---|
| LoRaWAN yerine ham LoRa (SubGHz_Phy) | Ağ sunucusu ve internet gerektirmeden kendi protokolümüz; düşük gecikme |
| 869.4–869.65 MHz alt bandı | Avrupa SRD kuralları altında en yüksek izinli güç (500 mW e.r.p.) ve %10 duty cycle |
| SF7 / BW125 | Kısa yayın süresi: 12 B konum paketi ≈ 41 ms, 15 B komut ≈ 46 ms. En büyük tablo (21 kayıt, 254 B) ≈ 400 ms, bu yüzden tablo periyodu duty cycle hesabında dikkate alınmalı |
| TX jitter | Aynı anda açılan node'ların sürekli çakışmasını önler |
| Whitelist PL'de | Filtre gecikmesi sabit ve yazılımdan bağımsız; yetkisiz veri Linux'a hiç ulaşmaz |
| ESP32 köprüsü | STM32WL'de BLE yok; telefonla en basit kablosuz bağlantı |
| Offline mbtiles | Sahada internet olmadan harita |
| ATAK yerine kendi uygulama | Basit, ihtiyaca özel; mentör geri bildirimiyle ATAK'a benzer yatay arayüz |
