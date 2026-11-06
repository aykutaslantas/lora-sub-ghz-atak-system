# Protokol ve paket formatları

Firmware V1.16.0. Tüm çok baytlı alanlar **little-endian**. Koordinatlar `derece × 1e7` olarak `int32` (ör. 40.9830600 → `409830600`).

Radyo: **869.525 MHz**, LoRa **SF7**, **BW 125 kHz**, CR 4/5, preamble 8, değişken uzunluk, en fazla 255 B. Paket türü ilk bayt ve uzunluktan ayırt edilir. `SECURE_ENABLED=1` iken havadaki her paket bölüm 6'daki zarfla sarılır; içteki paketler aşağıdakilerle aynıdır.

## 1. Adresler (16 bit)

| Adres | Anlamı |
|---|---|
| `0x0000` | Telefon / henüz kimliği olmayan node (havada kaynak olarak sadece A1'de) |
| `0x0001 … 0x7FFF` | Kartın UID'sinden türetilen adres (JOIN kapalı node) |
| `0x8000 \| (takım << 8) \| numara` | Takıma katılmış node. Takım 1 = ALPHA … 26 = ZULU, numara 1–99. Ör. `0x8102` = ALPHA-2 |
| `0xFF00 \| takım` | İleride takım adresleri için ayrılmış |
| `0xFFFE` | Admin (Zybo) |
| `0xFFFF` | Herkes |

**dev_id:** Kartın 96 bit UID'sinin CRC32'si (IEEE). Kimlik ve güvenlik zarfında kullanılır; Zybo'nun dev_id'si `0xFFFFFFFE`.

## 2. Konum ve tablo

### 2.1 Konum paketi `pos_packet_t` (12 B, node → herkes)

| Offset | Alan | Tip | Açıklama |
|---|---|---|---|
| 0 | `addr` | u16 | Gönderen |
| 2 | `seq` | u8 | Her pakette +1 (kayıp sayımı) |
| 3 | `lat` | i32 | Enlem × 1e7 |
| 7 | `lon` | i32 | Boylam × 1e7 |
| 11 | `flags` | u8 | bit0 GPS fix, bit1 "beni aktarın" (mesh), bit2 aktarımla ulaştı (gateway/Zybo koyar) |

Periyot 5000 ms + [0, 1000) ms rastgele. Konum telefondan gelir (`0xC4`); 60 sn gelmezse node sahte konuma döner (`fix=0`).

### 2.2 Konum tablosu `0xD1` (Zybo → herkes)

```
[0xD1][N][N × 12 B kayıt]     boy = 2 + 12·N
```
Her kayıt bir `pos_packet_t`. Düz modda N ≤ 21; güvenli modda UART sınırı yüzünden N ≤ 19 (aktarılırsa 18).

## 3. Kontrol paketleri (15 B)

Ortak başlık: `[0] tip [1..2] src [3..4] dst [5] msg_id`, gerisi tipe göre.

| Tip | Yön | İçerik |
|---|---|---|
| `0xC1` işaret / komut | telefon → node → herkes; admin → node(lar) | `[6]` tür: 1 ilgi noktası, 2 tehlike, 3 toplanma, 4 buraya gel, 5 bekle, 6 kontrol et, 7 geri dön (sadece admin). `[7..10]` lat, `[11..14]` lon |
| `0xC2` silme isteği | node → sadece Zybo | `[7..8]` hedef src, `[9]` hedef msg_id |
| `0xC3` admin kararı | Zybo → herkes | `src=0xFFFE`, `[6]` 1 onay / 2 red, `[7..8]` hedef src, `[9]` hedef msg_id, `[10]` isteğin msg_id'si |
| `0xC5` ACK | node → sadece Zybo | `[5]` node'un ACK sayacı, `[6]` 1 ULAŞTI / 2 OKUNDU, `[7..8]` onaylanan komutun src'si, `[9]` onaylanan msg_id |

- **ULAŞTI:** Node, kendisine özel admin komutunu aldığı an yollar. **OKUNDU:** Telefonda kullanıcı uyarıda Tamam/Bul'a basınca.
- Her ACK, rastgele aralıklarla 3 kez gider (her seferinde yeni sayaç). Zybo `(src, onaylanan msg_id, tür)` ile eler; ACK gelmezse komutu aynı msg_id ile tekrar yollar.
- Alıcılar `(tip, src, msg_id)` ile tekrarları eler.

## 4. JOIN: takıma katılma, atma, ayrılma

| Tip | Boy | Yön | Alanlar |
|---|---|---|---|
| `0xA1` katılma isteği | 17 | node → Zybo | `[1..2]` 0x0000, `[3..4]` 0xFFFE, `[5]` req_seq, `[6..9]` dev_id, `[10]` takım, `[11]` numara (0 = admin seçsin), `[12..13]` PIN, `[14..15]` fw sürümü, `[16]` pil (0xFF) |
| `0xA2` katılma cevabı | 15 | Zybo → node | `[1..2]` 0xFFFE, `[3..4]` 0xFFFF, `[5]` msg_id, `[6..9]` dev_id, `[10..11]` verilen adres, `[12]` takım, `[13]` numara, `[14]` sonuç: 0 tamam, 1 yanlış PIN, 2 red, 3 takım dolu, 4 numara dolu (`[13]` = önerilen) |
| `0xA3` atma / ayrılma onayı | 15 | Zybo → node | `[3..4]` node adresi, `[5]` msg_id, `[6..9]` dev_id, `[10]` sebep: 1 admin attı, 2 ayrılma onaylandı |
| `0xA4` ayrılma isteği | 15 | node → Zybo | `[1..2]` node adresi, `[3..4]` 0xFFFE, `[5]` msg_id, `[6..9]` dev_id |

- Kimliği olmayan node konum yollamaz, sadece 10–15 sn'de bir A1 yollar.
- Onay gelince node adresi flash'a yazar (son sayfa) ve yeni adresinden ULAŞTI yollar.
- Atılan node mevcut adresinden ULAŞTI yollar, sonra kimliğini siler. Ayrılmak isteyen node A4'ü 5 sn arayla en fazla 3 kez yollar, onay gelince ya da denemeler bitince kimliğini siler.

## 5. Tek sekme mesh

Node 25 sn boyunca tabloda kendini **doğrudan** ulaşmış olarak görmezse konum paketine "beni aktarın" bayrağını (bit1) koyar. Son 25 sn içinde admin'e doğrudan ulaşmış bir komşu, 100–600 ms rastgele bekleyip paketi aktarır; başka bir komşunun aynı paketi aktardığını duyarsa kendi aktarımını iptal eder.

| Tip | Düz boy | Yön | Düzen |
|---|---|---|---|
| `0xE1` aktarılmış konum | 17 | aktaran → gateway | `[0]` E1, `[1..2]` aktaran, `[3]` aktaranın ölçtüğü RSSI, `[4]` SNR, `[5..]` uzak node'un paketi |
| `0xE2` aktarılmış uplink kontrol | 20 | aktaran → gateway | aynı başlık, `[5..]` uzak node'un 15 B kontrol paketi (C1/C2/C5/A4) |
| `0xE3` aktarılmış downlink | 3 + N | aktaran → uzak node | `[0]` E3, `[1..2]` aktaran, `[3..]` tablo ya da admin paketi |

Aktaran, uzak node'u 30 sn boyunca "müşteri" sayar ve ona gelen tabloları/komutları E3 ile iletir. Aktarılan paket tekrar aktarılmaz.

## 6. Güvenli zarf (Faz 1, `SECURE_ENABLED=1`)

```
[0x51][dev_id 4][sayaç 4][şifreli iç paket N][MIC 4]        boy = N + 13
```

| Alan | Açıklama |
|---|---|
| Algoritma | AES-128-CCM, MIC 4 B, tek ortak ağ anahtarı (`net_key.h`, git'e girmez) |
| Nonce (13 B) | `dev_id(4) \| sayaç(4) \| dir(1) \| 00 00 00 00`; dir 0 = node yollar, 1 = Zybo yollar |
| AAD | Zarfın ilk 9 baytı (açık ama korunuyor) |
| Sayaç | Gönderen başına her pakette +1. Flash'ta ayrı bir sayfaya 256 pakette bir yazılır (sıradaki boş yuvaya), açılışta son kayıt + 256 ile devam eder |
| Replay | Alıcı önce MIC'e, sonra sayaca bakar: gönderen başına son sayaç + 32'lik pencere; görülmüş ya da 32'den eski sayaç atılır |

**İç içe aktarım:** Aktaran node uzak node'un güvenli paketini **açmadan** taşır:
```
E1 / E2 iç = [E1|E2][aktaran 2][RSSI][SNR][uzak node'un güvenli paketi, aynen]   → havada 43 / 46 B
E3 iç      = [E3][aktaran 2][Zybo'nun güvenli paketi, aynen]                       → kontrol için 44 B
```
Aynı paket hem doğrudan hem aktarımla gelirse içteki sayaç aynı olduğu için ikinci kopya tekrar diye atılır.

**Gateway sadece boru:** Paketleri açmaz, anahtarı bilmez. 0x51 ile başlayan ve en az 25 B olan paketleri olduğu gibi Zybo'ya verir; Zybo'nun hazırladığı güvenli paketleri istenen sayıda yayınlar. Node'ların işaretlerini Zybo doğrulayıp yeniden yayınlar.

**Test vektörü** (anahtar `00 01 02 … 0F`, dev_id `0x12345678`, sayaç 1, dir 0):
```
iç paket : 01 81 07 D0 5A 71 18 C0 C0 45 11 01        (konum 0x8101, seq 7, fix)
nonce    : 78 56 34 12 01 00 00 00 00 00 00 00 00
zarf     : 51 78 56 34 12 01 00 00 00 5B 0D 40 6A AE 8D 1E FF CD E1 0F 18 B6 2D 73 27
           (başlık 9 + şifreli 12 + MIC B6 2D 73 27)
UART     : AA 55 1B <zarf> C4 08 90                     (gateway → Zybo, RSSI −60, SNR 8)
```

**Geçiş:** `SECURE_ONLY=0` düz ve güvenli paketlerin ikisini de kabul eder (0x51 ile başlayıp MIC'i tutan paket güvenli, diğerleri düz sayılır); `SECURE_ONLY=1` sadece güvenli paketleri kabul eder.

## 7. UART çerçevesi (node ↔ ESP32, gateway ↔ Zybo)

```
[0xAA][0x55][LEN][PAYLOAD (LEN bayt)][CRC8]
CRC8: poly 0x07, init 0x00, LEN + PAYLOAD üzerinden
```
Alıcı bayt bayt çalışan bir durum makinesi (`SYNC0 → SYNC1 → LEN → DATA → CRC`); CRC'si tutmayan çerçeveyi atar, sonraki senkronu arar.

### Gateway → Zybo

Payload = havadan duyulan paket + RSSI (i8) + SNR (i8), yani LEN = boy + 2.

| Paket | Düz LEN | Güvenli LEN |
|---|---|---|
| Konum | 14 | 27 |
| Kontrol (C1/C2/C5/A4) | 17 | 30 |
| A1 | 19 | 32 |
| E1 (aktarılmış konum) | 19 | 45 |
| E2 (aktarılmış kontrol) | düz modda gateway açıp içteki 15 B'yi 17 ile verir | 48 |

### Zybo → gateway

| Mod | Payload |
|---|---|
| Düz | Tablo `0xD1` ya da 15 B `0xC1/0xC3/0xA2/0xA3`; gateway admin paketlerini 3 kez yayınlar |
| Güvenli | `[tx_repeat][güvenli paket]`, LEN = 1 + boy. Gateway paketi aynen `tx_repeat` kez (tablo 1, diğerleri 3), 400–600 ms arayla yayınlar; 4 paketlik kuyruk tutar |

### Node ↔ ESP32 ↔ telefon (yerel, LoRa'ya çıkmaz)

| Tip | Yön | İçerik |
|---|---|---|
| `0xD1`, `0xC1`, `0xC3` | node → telefon | Ağdan gelen tablo ve paketler (düz hâlleriyle) |
| `0xC1`, `0xC2`, `0xC5` | telefon → node | İşaret, silme isteği, OKUNDU (src'yi node doldurur) |
| `0xC4` konumumu ayarla (15 B) | telefon → node | `[1..4]` lat, `[5..8]` lon, `[9]` 1 ayarla / 0 sahte konuma dön |
| `0xC6` node durumu (8 B) | node → telefon | `[1..2]` adres, `[3]` son seq, `[4]` tabloda var mı, `[5]` tablodaki seq, `[6]` tablodaki flags |
| `0xC9` katılma parametreleri (15 B) | telefon → node | `[1]` takım, `[2]` numara, `[3..4]` PIN |
| `0xCB` katılma durumu (8 B) | node → telefon | `[1]` durum (0 kapalı, 1 parametre bekliyor, 2 onay bekliyor, 3 katıldı, 4 red, 5 ayrılıyor), `[2]` sonuç (A2'dekiler + 5 atıldı, 6 ayrıldı), `[3..4]` adres, `[5]` takım, `[6]` numara, `[7]` req_seq |
| `0xCC` takımdan ayrıl (15 B) | telefon → node | — |

## 8. BLE (ESP32 ↔ telefon)

| | |
|---|---|
| Cihaz adı | `LoRaNode-BLE` (iki köprü yan yana çalışacaksa birini değiştirin, uygulamadaki `kDeviceName` ile aynı olmalı) |
| Servis | `0xABF0` |
| Karakteristik | `0xABF1` (notify + write) |

ESP32, node'dan gelen çerçevenin CRC'sini kontrol eder ve sadece payload'u notify ile yollar; telefondan yazılan paketi çerçeveleyip node'a iletir. iPhone'da MTU genelde 185 B olduğu için tek notify'a en fazla 15 kayıtlık tablo sığar.
