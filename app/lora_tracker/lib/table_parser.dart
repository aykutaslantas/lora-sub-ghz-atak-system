// LoRa node'un BLE ile yolladigi "kim nerede" tablosunu cozer.
//
// Paket (STM32 ve Zybo tarafiyla ayni):
//   [0xD1][N][kayit 1 (12 B)] ... [kayit N]     boy = 2 + 12*N
// Kayit = pos_packet_t, little-endian:
//   [0..1] addr u16, [2] seq u8, [3..6] lat i32 (derece*1e7),
//   [7..10] lon i32 (derece*1e7), [11] flags u8 (bit0 = GPS fix)

import 'dart:typed_data';

import 'teams.dart';

const int tableType = 0xD1;
const int recordSize = 12;

class NodeRecord {
  final int addr;
  final int seq;
  final int latE7;
  final int lonE7;
  final int flags;

  const NodeRecord({
    required this.addr,
    required this.seq,
    required this.latE7,
    required this.lonE7,
    required this.flags,
  });

  double get lat => latE7 / 1e7;
  double get lon => lonE7 / 1e7;
  bool get hasFix => (flags & 0x01) != 0;

  /// "0x157C" seklinde adres
  String get addrHex =>
      '0x${addr.toRadixString(16).toUpperCase().padLeft(4, '0')}';

  /// Ekranda gosterilen isim: ALPHA-2 (takimli) ya da 0x157C (UID)
  String get name => nodeName(addr);

  /// Yuvarlama hatasi olmadan "41.0101060" seklinde koordinat
  static String formatE7(int v) {
    final neg = v < 0;
    final a = v.abs();
    final whole = a ~/ 10000000;
    final frac = (a % 10000000).toString().padLeft(7, '0');
    return '${neg ? '-' : ''}$whole.$frac';
  }

  String get latText => formatE7(latE7);
  String get lonText => formatE7(lonE7);
}

/// Gecerli bir tablo paketiyse kayitlari dondurur, degilse null.
List<NodeRecord>? parseTable(List<int> data) {
  if (data.length < 2) return null;
  if (data[0] != tableType) return null;
  final n = data[1];
  if (n < 1 || data.length != 2 + recordSize * n) return null;

  final bytes = ByteData.sublistView(Uint8List.fromList(data));
  final out = <NodeRecord>[];
  for (var i = 0; i < n; i++) {
    final o = 2 + i * recordSize;
    out.add(NodeRecord(
      addr: bytes.getUint16(o, Endian.little),
      seq: bytes.getUint8(o + 2),
      latE7: bytes.getInt32(o + 3, Endian.little),
      lonE7: bytes.getInt32(o + 7, Endian.little),
      flags: bytes.getUint8(o + 11),
    ));
  }
  return out;
}

/// Hata ayiklama icin "D1-03-7C-15-..." seklinde hex
String toHex(List<int> data) => data
    .map((b) => b.toRadixString(16).toUpperCase().padLeft(2, '0'))
    .join('-');
