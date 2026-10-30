# lora_tracker: offline taktik harita uygulaması

ESP32 BLE köprüsüne bağlanıp node'dan gelen "kim nerede" tablosunu haritada gösteren, ATAK'a benzer yatay arayüzlü Flutter uygulaması. **İnternet gerektirmez.**

## Özellikler

- Node'ları offline haritada gösterme (0xD1 tablo), son görülme ve GPS fix durumu
- Uzun basınca açılan **radyal menü** ile işaret koyma: ilgi noktası, tehlike, toplanma, buraya gel, burada bekle, bölgeyi kontrol et
- İşaret silme isteği gönderme (0xC2), admin kararıyla haritadan kaldırma (0xC3)
- Admin'den gelen **"GERİ DÖN"** komutunda uyarı, hedefe kesikli çizgi, mesafe/yön şeridi ve **Bul** ekranı
- Telefonun kendi GPS'i ile konum (geolocator), testte haritadan elle konum seçme

## Dosyalar

| Dosya | İçerik |
|---|---|
| `lib/main.dart` | BLE tarama/bağlantı, gelen paketlerin yönlendirilmesi |
| `lib/table_parser.dart` | 0xD1 tablo çözümleyici |
| `lib/cmd_packet.dart` | 0xC1 / 0xC2 / 0xC3 paket kodlama-çözme |
| `lib/map_view.dart` | flutter_map + mbtiles harita, node ve işaret katmanları |
| `lib/marks.dart` | İşaret modeli ve yönetimi |
| `lib/radial_menu.dart` | İşaret seçme menüsü |
| `lib/find_page.dart` | Hedefe yön/mesafe gösteren Bul ekranı |
| `lib/location_service.dart` | Telefon GPS'i |

## Çalıştırma

1. Offline haritayı `assets/map.mbtiles` olarak ekle (repoda yok, ~15 MB). Örneğin OpenStreetMap verisinden ilgili bölge için raster mbtiles üretilebilir.
2. Repoda yalnızca uygulama kodu ve Android manifest'i var. Platform iskelet dosyalarını üretmek için klasörde bir kez `flutter create --platforms=android,ios .` çalıştır (mevcut dosyaların üzerine yazmaz).
3. `flutter pub get`
4. `flutter run` (Android). iOS projesi mevcut, derlemek için macOS/Xcode gerekir.

İzinler: Bluetooth tarama/bağlanma ve konum (AndroidManifest ve Info.plist içinde tanımlı).

## Ekran görüntüleri

![Harita](../../docs/images/app-map.png)
![Radyal menü](../../docs/images/app-radial-menu.png)
![Geri dön uyarısı](../../docs/images/app-go-back.png)
