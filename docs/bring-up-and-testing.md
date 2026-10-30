# Devreye alma ve test notları

Sistem adım adım, her aşamada bir öncekinin doğrulandığı şekilde ayağa kaldırıldı.

## Test sırası

| Adım | Kurulum | Doğrulanan |
|---|---|---|
| 1 | 2× NUCLEO, ST ping-pong örneği | Radyo, RF switch, TCXO çalışıyor |
| 2 | Node + gateway (`GATEWAY_LOCAL_TABLE=1`) | Konum paketi, seq/kayıp sayımı, tablo yayını, Zybo'suz uçtan uca |
| 3 | `VIRTUAL_NODES=3` | Tek kartla çok node; çakışma ve jitter davranışı |
| 4 | Gateway ↔ Zybo UART | Çerçeve formatı, CRC8, whitelist filtresi |
| 5 | Node + ESP32 + telefon | BLE notify, tablonun haritada görünmesi |
| 6 | Telefon → komut | 0xC1 tekrar yayını, iki node'da da görünmesi; `GW_FORWARD_CMD=1` ile admin GUI'ye ulaşması |
| 7 | Quectel KG200Z | Farklı modülde aynı firmware ile çalışma |

Güncel kurulum: **2 gerçek node + 1 gateway + Zybo**. GPS modülleri henüz eklenmedi; node'lar sabit test koordinatı yayınlıyor, telefon kendi GPS'ini kullanıyor.

## Karşılaşılan sorunlar ve çözümleri

| Belirti | Sebep | Çözüm |
|---|---|---|
| Node log'unda tablo var ama telefonda node görünmüyor | `NODE_UART_BRIDGE=0` iken node tabloyu ESP32'ye göndermiyor | Köprü bağlı node'da `NODE_UART_BRIDGE=1` |
| Komutlar iki node'a da ulaşıyor ama admin GUI'de yok | Gateway'de `GW_FORWARD_CMD=0` | Gateway'i `GW_FORWARD_CMD=1` ile derle |
| Zybo'dan gelen 0xC1 gateway'de "geçersiz paket" | Gateway yalnızca 0xD1 tabloyu bekliyordu | V1.7.0: gateway Zybo'dan 0xC1'i kabul edip ayrı tekrar tamponuyla 3× yayınlıyor |
| Bridge açıkken node uykuya geçince UART kopuyor | USART1 STOP2'de çalışmıyor | Bridge veya gateway modunda STOP2 derleme zamanında kapatılıyor (`sys_conf.h`) |
| KG200Z'de TX log'u var ama gateway duymuyor (olası) | Modülde antene yalnızca HP PA çıkışı bağlı olabilir | `TX_OUTPUT_POWER` > 15 dBm (HP PA seçilir) ya da `RBI_CONF_RFO_HP` |
| IDE "launch" hatası | Run'a basarken bir `.h` dosyası seçiliydi | Proje seçiliyken Run |
| iOS'ta ESP32 bulunamıyor (olası) | iOS cihazı MAC adresiyle değil ad/UUID ile bulur | Tarama servis UUID'si `0xABF0` veya ada göre |

## Yapılacaklar

- [ ] GPS modüllerini node'lara eklemek (gerçek konum, `fix=1`)
- [ ] Telefonun GPS konumunu BLE → node üzerinden yayınlama seçeneği
- [ ] Menzil testi (powerbank ile sahada)
- [ ] ~30 node için duty cycle ve tablo periyodu hesabı
- [ ] Mesh / gateway'den herkese dağıtım (mentörün sonraki aşama planı)
