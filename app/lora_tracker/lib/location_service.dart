// Telefonun kendi konumu (GPS, internetsiz calisir) + mesafe / yon hesaplari.
//
// Evden test icin: haritaya uzun basinca "test konumu" ayarlanir ve GPS'in
// yerine o kullanilir (setManual). "Kaldir" ile gercek GPS'e donulur.

import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:geolocator/geolocator.dart';
import 'package:latlong2/latlong.dart';

class MyLocation {
  final LatLng point;
  final double accuracy; // metre (yaricap)
  final double? heading; // derece, sadece yururken anlamli
  final double speed; // m/s
  final bool manual; // true = haritadan elle verilen test konumu

  const MyLocation({
    required this.point,
    required this.accuracy,
    this.heading,
    this.speed = 0,
    this.manual = false,
  });

  /// Yururken (>= ~3 km/sa) GPS yonu guvenilir olur
  bool get headingValid => !manual && heading != null && speed >= 0.8;
}

class LocationService {
  LocationService._();
  static final LocationService instance = LocationService._();

  /// Kullanilan konum (test konumu varsa o, yoksa GPS). null = henuz yok.
  final ValueNotifier<MyLocation?> current = ValueNotifier(null);

  /// Kullaniciya gosterilecek durum yazisi
  final ValueNotifier<String> status = ValueNotifier('Konum bekleniyor...');

  StreamSubscription<Position>? _sub;
  MyLocation? _gps;
  MyLocation? _manual;
  bool _started = false;

  Future<void> start() async {
    if (_started) return;
    _started = true;

    if (!await Geolocator.isLocationServiceEnabled()) {
      status.value = 'Telefonda konum (GPS) kapalı';
    }

    // BLE izni ayni anda soruluyorsa Android "izin istegi suruyor" hatasi
    // verebilir; birkac kez bekleyip tekrar dene.
    LocationPermission perm = await Geolocator.checkPermission();
    for (var i = 0; i < 5 && perm == LocationPermission.denied; i++) {
      try {
        perm = await Geolocator.requestPermission();
      } on PermissionRequestInProgressException {
        await Future<void>.delayed(const Duration(seconds: 2));
      }
    }
    if (perm == LocationPermission.denied ||
        perm == LocationPermission.deniedForever) {
      status.value = 'Konum izni verilmedi';
      _started = false; // sonra tekrar denenebilsin
      return;
    }

    final LocationSettings settings = Platform.isAndroid
        ? AndroidSettings(
            accuracy: LocationAccuracy.best,
            distanceFilter: 1,
            intervalDuration: const Duration(seconds: 1),
          )
        : const LocationSettings(
            accuracy: LocationAccuracy.best,
            distanceFilter: 1,
          );

    status.value = 'GPS konumu aranıyor...';
    _sub = Geolocator.getPositionStream(locationSettings: settings).listen(
      (p) {
        _gps = MyLocation(
          point: LatLng(p.latitude, p.longitude),
          accuracy: p.accuracy,
          heading: p.heading,
          speed: p.speed,
        );
        status.value = 'GPS ±${p.accuracy.round()} m';
        _publish();
      },
      onError: (Object e) => status.value = 'Konum hatası: $e',
    );
  }

  void setManual(LatLng p) {
    _manual = MyLocation(point: p, accuracy: 0, manual: true);
    _publish();
  }

  void clearManual() {
    _manual = null;
    _publish();
  }

  bool get hasManual => _manual != null;

  void _publish() => current.value = _manual ?? _gps;

  void dispose() {
    _sub?.cancel();
  }
}

// ---- Mesafe / yon yardimcilari ----

const Distance _dist = Distance(roundResult: false);

/// Iki nokta arasi mesafe (metre)
double distanceM(LatLng a, LatLng b) => _dist.as(LengthUnit.Meter, a, b);

/// a'dan b'ye yon, kuzeyden saat yonunde 0..360 derece
double bearingDeg(LatLng a, LatLng b) => (_dist.bearing(a, b) + 360) % 360;

String fmtDistance(double m) {
  if (m < 1000) return '${m.round()} m';
  return '${(m / 1000).toStringAsFixed(m < 10000 ? 2 : 1)} km';
}

const List<String> _dirNames = [
  'Kuzey', 'Kuzeydoğu', 'Doğu', 'Güneydoğu',
  'Güney', 'Güneybatı', 'Batı', 'Kuzeybatı',
];

/// 0..360 dereceyi "Kuzeydogu" gibi yaziya cevirir
String compassName(double deg) => _dirNames[((deg + 22.5) ~/ 45) % 8];
