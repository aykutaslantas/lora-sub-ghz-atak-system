# STM32WL node / gateway firmware

ST'nin `SubGHz_Phy_PingPong` örneği üzerine yazıldı; node ve gateway aynı kod tabanında, derleme bayraklarıyla seçilir. Sürüm: **V1.17.0** (`SubGHz_Phy/App/app_version.h`).

## Hedef kartlar

- **NUCLEO-WL55JC1** (STM32WL55, sadece Cortex-M4 çekirdeği)
- **Quectel KG200Z** (STM32WLE5). Aynı binary çalışır; antene sadece HP PA bağlı olduğu için güç 17 dBm.

## Önemli dosyalar

| Dosya | İçerik |
|---|---|
| `Core/Inc/app_mode.h` | **Rol ve özellik bayrakları** (tek yapılandırma noktası) |
| `SubGHz_Phy/App/subghz_phy_app.h` | Paket yapıları, protokol sabitleri |
| `SubGHz_Phy/App/subghz_phy_app.c` | Uygulama: TX zamanlama, RX, tablo, komut/ACK, JOIN, mesh, güvenli zarf, UART çerçeve çözücü |
| `SubGHz_Phy/App/lora_secure.h` | AES-128 ve AES-128-CCM (yazılım, header-only) |
| `SubGHz_Phy/App/net_key.example.h` | Ağ anahtarı şablonu; `net_key.h` olarak kopyalanır (git'e girmez) |
| `Core/Inc/sys_conf.h` | Log seviyesi, debugger, STOP2 düşük güç |
| `STM32CubeIDE/STM32WL55JCIX_FLASH.ld` | Flash'ın son iki sayfası kod dışı: güvenlik sayacı (sayfa 126) ve JOIN kimliği (sayfa 127) |
| `tests/` | PC'de çalışan testler (`./run_tests.sh`) |

## Kart başına ayarlar (`Core/Inc/app_mode.h`)

| Kart | `GATEWAY_MODE` | `NODE_UART_BRIDGE` | `JOIN_ENABLED` | `SECURE_ENABLED` | `SECURE_ONLY` |
|---|---|---|---|---|---|
| Gateway (Zybo'ya bağlı) | 1 | – | – | 1 | 1 |
| Node + ESP32 + telefon | 0 | 1 | 1 | 1 | 1 |

Geçiş ya da test için: `SECURE_ONLY 0` düz paketleri de kabul eder, `SECURE_ENABLED 0` tamamen düz çalışır. `TEST_FAR_NODE` ve `TEST_GW_IGNORE_DIRECT` masada mesh testi içindir; sahada 0 olmalı.

## Derleme ve yükleme

1. `Drivers/`, `Middlewares/`, `Utilities/` ve `STM32CubeIDE/Application/User/` (startup, syscalls) repoda yok. `SubGHz_Phy_PingPong.ioc`'u STM32CubeMX'te açıp **Generate Code** ile üret ya da STM32CubeWL paketindeki aynı örnekten kopyala. Kullanıcı kodu `USER CODE` blokları ve `app_mode.h` içinde olduğu için üzerine yazılmaz.
2. `SubGHz_Phy/App/net_key.example.h` → `net_key.h` olarak kopyala ve anahtarı değiştir. Zybo'daki anahtarla aynı olmalı.
3. STM32CubeIDE'de `STM32CubeIDE/` klasörünü içe aktar → **Clean** → Build.
4. ST-LINK ile Run. Birden fazla ST-LINK takılıysa Debug ayarında doğru seri numarasını seç.
5. Log: ST-LINK sanal COM portu, **115200 baud**.

İlk açılışta flash'taki kimlik ve sayaç sayfaları boşsa node kimliksiz başlar, telefondan katılmayı bekler. Yeniden yüklemede kimlik ve sayaç korunur; tam silme (full chip erase) ikisini de siler.

## Log örneği

```
APPLICATION_VERSION: V1.17.0
JOIN: kayitli kimlik takim=1 numara=1 adres=0x8101
SECURE: acik (sadece guvenli), dev_id=0xA3D2EEB3, sayac=513
MODE=NODE
NODE_ADDR=0x8101
[TX] addr=0x8101 seq= 12 lat=40.9856843 lon=28.8317307
[RX] addr=0x8102 seq= 30 lat=... fix=1 rssi=-40 snr=13 alinan=29 kayip=1
[TABLO] 2 node (rssi=-24 snr=13):
[ONAY] admin son konumumu aldi: seq=12 (son yollanan 12)
```

## Testler

```bash
cd tests && ./run_tests.sh
```
Sadece `gcc` gerekir. Firmware'in uygulama dosyası sahte HAL/radyo/zamanlayıcı ile derlenir; mesh, JOIN, güvenlik ve gateway senaryoları çalıştırılır.
