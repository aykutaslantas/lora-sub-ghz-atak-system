// Faz 2: isaret / komut paketi (STM32 cmd_packet_t ile ayni, 15 byte, little-endian)
//
//   [0]    type   0xC1
//   [1..2] src    u16  gonderen node (telefon 0 yollar, node kendi adresini yazar)
//   [3..4] dst    u16  hedef node, 0xFFFF = herkes
//   [5]    msgId  u8   gonderen basina sayac
//   [6]    kind   u8   1 ilgi, 2 tehlike, 3 toplanma, 4 buraya gel, 5 bekle, 6 kontrol,
//                      7 BOLGEYE GERI DON (sadece admin 0xFFFE -> tek node, bkz. GoBackCmd)
//   [7..10]  lat  i32  derece * 1e7
//   [11..14] lon  i32  derece * 1e7

import 'dart:typed_data';

import 'package:latlong2/latlong.dart';

import 'marks.dart';

const int cmdType = 0xC1;
const int cmdLen = 15;
const int cmdDstAll = 0xFFFF;

/// MarkKind <-> paketteki tur numarasi (1..6)
int kindToCode(MarkKind k) => k.index + 1;
MarkKind? kindFromCode(int c) =>
    (c >= 1 && c <= MarkKind.values.length) ? MarkKind.values[c - 1] : null;

class CmdPacket {
  final int src;
  final int dst; // 0xFFFF = herkes
  final int msgId;
  final MarkKind kind;
  final int latE7;
  final int lonE7;

  const CmdPacket({
    required this.src,
    required this.dst,
    required this.msgId,
    required this.kind,
    required this.latE7,
    required this.lonE7,
  });

  LatLng get point => LatLng(latE7 / 1e7, lonE7 / 1e7);
  bool get toAll => dst == cmdDstAll;

  static String hex16(int v) =>
      '0x${v.toRadixString(16).toUpperCase().padLeft(4, '0')}';

  List<int> encode() {
    final b = ByteData(cmdLen);
    b.setUint8(0, cmdType);
    b.setUint16(1, src, Endian.little);
    b.setUint16(3, dst, Endian.little);
    b.setUint8(5, msgId & 0xFF);
    b.setUint8(6, kindToCode(kind));
    b.setInt32(7, latE7, Endian.little);
    b.setInt32(11, lonE7, Endian.little);
    return b.buffer.asUint8List();
  }

  /// Gecerli bir komut paketiyse coz, degilse null
  static CmdPacket? decode(List<int> data) {
    if (data.length != cmdLen || data[0] != cmdType) return null;
    final b = ByteData.sublistView(Uint8List.fromList(data));
    final kind = kindFromCode(b.getUint8(6));
    if (kind == null) return null;
    return CmdPacket(
      src: b.getUint16(1, Endian.little),
      dst: b.getUint16(3, Endian.little),
      msgId: b.getUint8(5),
      kind: kind,
      latE7: b.getInt32(7, Endian.little),
      lonE7: b.getInt32(11, Endian.little),
    );
  }
}

int toE7(double deg) => (deg * 1e7).round();

// ---------------- Admin: BOLGEYE GERI DON (0xC1, tur 7) ----------------
// Zybo, admin'in cizdigi alanin disina cikan node'a 60 sn'de bir yollar
// (her seferinde yeni msgId). lat/lon = donulecek nokta (alanin ~20 m icinde).
// Haritaya normal isaret olarak konmaz; telefon bunu OLUSTURAMAZ (MarkKind'da yok).

const int cmdKindGoBack = 7;

class GoBackCmd {
  final int src; // her zaman 0xFFFE (admin)
  final int dst; // hedef node
  final int msgId;
  final int latE7;
  final int lonE7;

  const GoBackCmd({
    required this.src,
    required this.dst,
    required this.msgId,
    required this.latE7,
    required this.lonE7,
  });

  LatLng get point => LatLng(latE7 / 1e7, lonE7 / 1e7);

  /// Admin'den gelen gecerli bir GERI DON paketiyse coz, degilse null
  static GoBackCmd? decode(List<int> data) {
    if (data.length != cmdLen ||
        data[0] != cmdType ||
        data[6] != cmdKindGoBack) {
      return null;
    }
    final b = ByteData.sublistView(Uint8List.fromList(data));
    final src = b.getUint16(1, Endian.little);
    if (src != addrAdmin) return null; // sadece admin yollayabilir
    return GoBackCmd(
      src: src,
      dst: b.getUint16(3, Endian.little),
      msgId: b.getUint8(5),
      latE7: b.getInt32(7, Endian.little),
      lonE7: b.getInt32(11, Endian.little),
    );
  }
}

// ---------------- Silme (admin onayli) ----------------
// Ayni 15 byte duzen, sadece tip farkli. Isaret (src, msgId) ile tanimlanir.
//   0xC2 SILME ISTEGI : telefon -> node -> gateway -> SADECE Zybo
//        [1..2] src (node doldurur) [3..4] dst=0xFFFE [5] istek msgId [6] 0
//        [7..8] hedef src  [9] hedef msgId  [10..14] 0
//   0xC3 ADMIN KARARI : Zybo -> gateway -> 3x yayin -> herkes
//        [1..2] src=0xFFFE [3..4] dst=isteyen (ya da 0xFFFF) [5] admin sayaci
//        [6] karar 1=onay 2=red  [7..8] hedef src  [9] hedef msgId  [10] istek msgId

const int cmdTypeDelReq = 0xC2;
const int cmdTypeDelAck = 0xC3;
const int addrAdmin = 0xFFFE;

List<int> encodeDelRequest(
    {required int msgId, required int targetSrc, required int targetMsgId}) {
  final b = ByteData(cmdLen);
  b.setUint8(0, cmdTypeDelReq);
  b.setUint16(1, 0, Endian.little); // node kendi adresini yazar
  b.setUint16(3, addrAdmin, Endian.little);
  b.setUint8(5, msgId & 0xFF);
  b.setUint16(7, targetSrc, Endian.little);
  b.setUint8(9, targetMsgId & 0xFF);
  return b.buffer.asUint8List();
}

class AdminDecision {
  final int msgId; // admin sayaci (tekrar elemek icin)
  final int dst; // isteyen node ya da 0xFFFF (admin kendisi sildi)
  final bool approved;
  final int targetSrc;
  final int targetMsgId;
  final int reqMsgId;

  const AdminDecision({
    required this.msgId,
    required this.dst,
    required this.approved,
    required this.targetSrc,
    required this.targetMsgId,
    required this.reqMsgId,
  });

  static AdminDecision? decode(List<int> d) {
    if (d.length != cmdLen || d[0] != cmdTypeDelAck) return null;
    if (d[6] != 1 && d[6] != 2) return null;
    return AdminDecision(
      msgId: d[5],
      dst: d[3] | (d[4] << 8),
      approved: d[6] == 1,
      targetSrc: d[7] | (d[8] << 8),
      targetMsgId: d[9],
      reqMsgId: d[10],
    );
  }
}
