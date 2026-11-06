# Devreye alma ve test notları

Sistem adım adım ayağa kaldırıldı; her aşamada bir öncekinin doğrulandığından emin olundu.

## Test sırası

| Adım | Kurulum | Doğrulanan |
|---|---|---|
| 1 | 2× NUCLEO, ST ping-pong örneği | Radyo, RF switch, TCXO |
| 2 | Node + gateway (`GATEWAY_LOCAL_TABLE=1`) | Konum paketi, kayıp sayımı, tablo yayını, Zybo'suz uçtan uca |
| 3 | `VIRTUAL_NODES=3` | Tek kartla çok node, çakışma ve jitter |
| 4 | Gateway ↔ Zybo UART | Çerçeve formatı, CRC8, whitelist |
| 5 | Node + ESP32 + telefon | BLE, tablonun haritada görünmesi |
| 6 | İşaret ve komutlar | Tekrar yayın, ACK (ULAŞTI / OKUNDU), silme istekleri |
| 7 | Masada mesh (`TEST_FAR_NODE`) | Aktarım iki yönde, bayrağın salınmaması |
| 8 | JOIN | Katılma, red sebepleri, atma, ayrılma, kimliğin yeniden açılışta geri gelmesi |
| 9 | iPhone + ESP32-S3 + KG200Z | İkinci telefon, iOS derlemesi |
| 10 | Güvenlik geçişi | Düz → güvenli uplink → güvenli downlink → sadece güvenli |
| 11 | Bina içi menzil yürüyüşü | Kalın duvarlar arkasında bağlantı, RSSI/SNR |

Güncel kurulum: **2 node (NUCLEO + KG200Z) + 1 gateway + Zybo**, iki telefon (Android, iPhone). GPS ayrı modülden değil telefondan geliyor.

## PC testleri

Firmware'in uygulama mantığı (`subghz_phy_app.c`) PC'de sahte HAL, radyo ve zamanlayıcıyla derlenip test edilir:

```bash
cd firmware/stm32wl-node-gateway/tests
./run_tests.sh
```

Kapsananlar: AES-128 (FIPS-197) ve CCM test vektörleri, mesh aktarımı (iki yön, bastırma, salınım), gateway yönlendirmesi, JOIN/kick/leave, güvenli zarf, sayaç saklama, replay penceresi, sahte/bozuk paketlerin reddi, gateway kuyruğu.

## Ölçümler

**Bina içi yürüyüş (V1.15, düz mod):** Gateway ve bir node sabit, diğer node elde kalın duvarların arkasına götürüldü.
- Gateway sinyali en kötü noktada **−95 dBm, SNR +9 dB**; 37 tablonun 35'i geldi, yollanan her konum admin'e ulaştı. SF7 hassasiyeti (~−124 dBm) düşünülünce ~30 dB pay vardı.
- Diğer node'un sinyali aynı noktada ~20 dB daha zayıftı (−107…−109 dBm, SNR −8 dB, SF7 sınırında); o bölgede paketlerin bir kısmı kayboldu. Sebep olarak anten/konum incelenecek.
- Kaybolan iki tablo menzil değil çakışma kaynaklıydı: biri iki vericinin aynı anda yayını, diğeri node'un yayın yaparken tabloyu kaçırması (half-duplex).

**Güvenlik geçişi (V1.16):** Zybo sayaçlarında güvenli paketler doğrulandı, replay 0. Node'lar Zybo'nun güvenli tablosunu ve komutlarını çözdü; mesh aktarımı iç içe zarfla çalıştı.

## Karşılaşılan sorunlar ve çözümleri

| Belirti | Sebep | Çözüm |
|---|---|---|
| Node log'unda tablo var ama telefonda node yok | `NODE_UART_BRIDGE=0` | Köprü bağlı node'da `NODE_UART_BRIDGE=1` |
| Bridge açıkken node uykuya geçince UART kopuyor | USART1 STOP2'de çalışmıyor | Bridge/gateway modunda STOP2 kapatılıyor (`sys_conf.h`) |
| Kart uyurken ST-LINK bağlanamıyor | STOP2'de SWD kapalı | `DEBUGGER_ENABLED 1`, gerekirse BOOT0 ile kurtarma |
| KG200Z'de TX var ama gateway duymuyor | Modülde antene sadece HP PA bağlı | `TX_OUTPUT_POWER` 17 dBm (HP PA) |
| ACK'ler kayboluyor | Node'ların ACK'leri ve gateway tekrarları çakışıyor | ACK 3×, rastgele aralıklarla; aktarıcıda downlink tekrar denemesi |
| Zybo yeniden başlayınca node'lar tablodan düşüyor | Whitelist RAM'de | Zybo tarafında üye listesi SD kartta tutuluyor |
| `Failed to start GDB server` | Yanlış ST-LINK seçili ya da kablo ters | Debug ayarında doğru ST-LINK S/N'i; kırmızı şerit = 1. pin |
| İki telefon birbirinin köprüsüne bağlanabiliyor | İki ESP32 aynı BLE adıyla yayın yapıyor | Bir köprünün adını ve o telefondaki `kDeviceName`'i değiştir |
| dev_id log'da `0x%lX` görünüyor | Hafif printf `%l`'yi tanımıyor | `%08X` + `(unsigned int)` |

## Yapılacaklar

- [ ] Açık alanda gerçek menzil testi (görüş hattı, mesafe – RSSI – kayıp grafiği)
- [ ] Gateway'i duymayan gerçek bir node ile mesh testi
- [ ] Güvenlik Faz 2: node başına anahtar, grup anahtarı, atılan node'da anahtar yenileme
- [ ] Ölçek: 20+ node için tablo bölme, uyarlamalı konum periyodu
- [ ] Uygulama: bağlantı kopunca açık uyarı, tekil BLE adı / köprü seçme
- [ ] Sahadan önce test anahtarını ve masa test modlarını kaldırmak
