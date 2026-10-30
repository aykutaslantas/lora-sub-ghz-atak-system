# STM32WL node / gateway firmware

ST'nin `SubGHz_Phy_PingPong` örneği temel alınarak yazılmış; node ve gateway rolleri **aynı kod tabanında**, derleme bayraklarıyla seçilir. Sürüm: **V1.7.0** (`SubGHz_Phy/App/app_version.h`).

## Hedef kartlar

- **NUCLEO-WL55JC1** (STM32WL55, yalnızca Cortex-M4 çekirdeği kullanılıyor)
- **Quectel KG200Z** (STM32WLE5). Proje WL55 için üretildi; WLE5'in M4 tarafı aynı olduğu için doğrudan çalışır. RF switch (PC3/PC4/PC5), TCXO (PB0, 1.7 V), DCDC ve PA ayarları NUCLEO ile aynı.

## Önemli dosyalar

| Dosya | İçerik |
|---|---|
| `Core/Inc/app_mode.h` | **Rol ve özellik bayrakları** (tek yapılandırma noktası, CubeMX üzerine yazmaz) |
| `SubGHz_Phy/App/subghz_phy_app.h` | Paket yapıları, radyo parametreleri, protokol sabitleri |
| `SubGHz_Phy/App/subghz_phy_app.c` | Uygulama mantığı: TX zamanlama, RX ayrıştırma, tablo, komut tekrar yayını, UART çerçeve ayrıştırıcı |
| `Core/Src/usart.c`, `Core/Src/stm32wlxx_it.c` | USART1 (PB6 TX / PB7 RX) kesme tabanlı alım |
| `Core/Inc/sys_conf.h` | Log seviyesi, debugger, STOP2 düşük güç ayarı |
| `SubGHz_Phy/Target/radio_board_if.h` | RF switch, TCXO, DCDC, PA (LP/HP) seçimi |
| `SubGHz_Phy_PingPong.ioc` | CubeMX pin ve saat yapılandırması |

## Rol seçimi (`Core/Inc/app_mode.h`)

| Senaryo | `GATEWAY_MODE` | `GATEWAY_LOCAL_TABLE` | `NODE_UART_BRIDGE` | `GW_FORWARD_CMD` |
|---|---|---|---|---|
| Node, pil dostu (STOP2) | 0 | – | 0 | – |
| Node + ESP32/telefon | 0 | – | 1 | – |
| Gateway, Zybo ile | 1 | 0 | – | 1 |
| Gateway, Zybo'suz test | 1 | 1 | – | 0 |

Test için `VIRTUAL_NODES` (1–8) tek kartın birden çok adresle yayın yapmasını sağlar; ek adreslerin Zybo whitelist'ine eklenmesi gerekir.

## Derleme ve yükleme

1. `Drivers/`, `Middlewares/`, `Utilities/` ve `STM32CubeIDE/Application/User/` (startup, syscalls) bu repoda yok (ST'nin dağıttığı kütüphaneler). `SubGHz_Phy_PingPong.ioc` dosyasını STM32CubeMX'te açıp **Generate Code** ile oluştur ya da STM32CubeWL paketindeki aynı örnekten kopyala. Kullanıcı kodu `USER CODE` blokları ve `app_mode.h` içinde olduğu için üzerine yazılmaz.
2. STM32CubeIDE'de `STM32CubeIDE/` klasörünü proje olarak içe aktar → Build.
3. ST-LINK ile Run. Birden fazla kart bağlıysa yalnızca hedef kartın ST-LINK'i takılı olsun.
4. Log: VCOM portu, **115200 baud**. Açılışta `NODE_ADDR=0x....` ve ardından `[TX] addr=… seq=…` satırları görünür.

## Log örnekleri

```
P2P KONUM NODE
APPLICATION_VERSION: V1.7.0
[TX] addr=0x157C seq= 12 lat=40.9830600 lon=28.8536100 fix=0
[RX] addr=0x0F53 seq= 40 lat=… lon=… fix=0 rssi=-41 snr=9 alinan=40 kayip=0
[TABLO] 2 node (rssi=-38 snr=10):
[BLE] komut -> ESP32
```

![Node log'u](../../docs/images/node-log.png)
