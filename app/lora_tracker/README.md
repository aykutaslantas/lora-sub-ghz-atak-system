# lora_tracker: offline taktik harita uygulaması

ESP32 BLE köprüsüne bağlanıp node'dan gelen "kim nerede" tablosunu haritada gösteren, ATAK'a benzer yatay arayüzlü Flutter uygulaması. **İnternet gerektirmez.** Android ve iOS.

## Özellikler

- Node'ları offline haritada **takım renkleri ve adlarıyla** gösterme (ör. ALPHA-2); takımsız node'lar kırmızı ve hex adresle
- **Takım / Kimlik** ekranı: takım, numara (0 = admin seçsin), PIN ile katılma; durum (onay bekleniyor, red sebebi, önerilen numara), takımdan ayrılma, admin atınca uyarı
- **Konum paylaşımı:** telefonun GPS'i node'un konumu olarak yayınlanır (`0xC4`)
- **Admin ✓** çipi: node'un son konumu admin'in tablosunda mı (doğrudan / aktarımla / yok)
- Radyal menüyle işaret ve komut: ilgi noktası, tehlike, toplanma, buraya gel, burada bekle, bölgeyi kontrol et; ✓ gönderildi, ✓✓ ağa ulaştı
- Admin komutlarında uyarı penceresi; Tamam / Bul ile OKUNDU onayı
- İşaret silme isteği, admin kararıyla kaldırma; **Bul** ekranında hedefe yön ve mesafe

## Dosyalar

| Dosya | İçerik |
|---|---|
| `lib/main.dart` | BLE tarama/bağlantı, gelen paketlerin yönlendirilmesi, durum çubuğu |
| `lib/cmd_packet.dart` | Kontrol, konum, ACK, node durumu ve katılma paketleri |
| `lib/table_parser.dart` | `0xD1` tablo çözücü |
| `lib/teams.dart` | Takım adları ve renkleri, adres → ad (ALPHA-2) |
| `lib/join_page.dart` | Takım / Kimlik ekranı |
| `lib/map_view.dart` | flutter_map + mbtiles harita, node ve işaret katmanları |
| `lib/marks.dart`, `lib/radial_menu.dart` | İşaret modeli ve seçme menüsü |
| `lib/find_page.dart` | Bul ekranı |
| `lib/location_service.dart` | Telefon GPS'i, testte elle konum |

## Çalıştırma (Android)

1. Offline haritayı `assets/map.mbtiles` olarak ekle (repoda yok, ~15 MB).
2. Android iskelet dosyaları eksikse klasörde bir kez `flutter create --platforms=android .` (mevcut dosyaların üzerine yazmaz).
3. `flutter pub get` → `flutter run`.

## iPhone (Mac olmadan)

1. Repoda **Actions → iOS unsigned IPA → Run workflow**. Derleme için harita dosyasının da repoda olması gerekir (`.gitignore`'daki `assets/*.mbtiles` satırı yüzünden şu an yok).
2. Bitince artifact'tan `lora_tracker-unsigned.ipa`'yı indir.
3. Windows'ta **Sideloadly** (iTunes ve iCloud apple.com'dan kurulu olmalı) ile kendi Apple ID'nle iPhone'a yükle.
4. iPhone'da Ayarlar → Genel → VPN ve Cihaz Yönetimi → profile güven; iOS 16+ için Gizlilik ve Güvenlik → Geliştirici Modu.

Ücretsiz Apple ID ile yükleme 7 günde bir yenilenmeli (aynı `.ipa` tekrar yüklenir). Private repoda macOS dakikaları sınırlı (ücretsiz planda ayda ~190 dakika, bir derleme ~10–15 dakika).

İzinler: Bluetooth ve konum (Android `AndroidManifest.xml`, iOS `Info.plist`). iPhone'da uygulama arka plana atılınca BLE durur; ön planda tutun.
