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

// ---------------- 0xC4: KONUMUMU AYARLA (telefon -> SADECE bagli node) ----------------
// LoRa'ya hic cikmaz. Node bu konumu sahte konum yerine fix=1 ile yayinlar.
// 60 sn yenisi gelmezse node sahte konuma doner, o yuzden periyodik yollanir.
//   [0] 0xC4  [1..4] lat i32 x1e7  [5..8] lon i32 x1e7  [9] 1=ayarla / 0=kapat

const int cmdTypeSetPos = 0xC4;

/// p = null -> paylasimi kapat (node sahte konuma doner)
List<int> encodeSetPos(LatLng? p) {
  final b = ByteData(cmdLen);
  b.setUint8(0, cmdTypeSetPos);
  if (p != null) {
    b.setInt32(1, toE7(p.latitude), Endian.little);
    b.setInt32(5, toE7(p.longitude), Endian.little);
    b.setUint8(9, 1);
  }
  return b.buffer.asUint8List();
}

// ---------------- 0xC5: KOMUT ACK (OKUNDU) ----------------
// Kullanici admin komutunun uyarisinda Tamam / Bul'a basinca telefon yollar:
// telefon -> ESP32 -> node -> LoRa -> gateway -> Zybo. Node adresini ve sayaci doldurur.
// (ULASTI ACK'ini node kendisi yollar, telefonun isi yok.)
//   [0] 0xC5 [1..2] src (node doldurur) [3..4] 0xFFFE [5] sayac (node) [6] tur
//   [7..8] onaylanan komutun src [9] onaylanan komutun msgId [10..14] 0

const int cmdTypeAck = 0xC5;
const int ackKindDelivered = 1;
const int ackKindSeen = 2;

List<int> encodeSeenAck({required int targetSrc, required int targetMsgId}) {
  final b = ByteData(cmdLen);
  b.setUint8(0, cmdTypeAck);
  b.setUint16(3, addrAdmin, Endian.little);
  b.setUint8(6, ackKindSeen);
  b.setUint16(7, targetSrc, Endian.little);
  b.setUint8(9, targetMsgId & 0xFF);
  return b.buffer.asUint8List();
}

// ---------------- 0xC6: NODE DURUMU (node -> telefon, dolayli onay) ----------------
// Node her tablodan (10 sn) sonra yollar: Zybo'nun tablosunda kendi kaydi var mi?
// Varsa son konumu admin'e ulasmis demektir. LoRa'ya cikmaz.
//   [0] 0xC6 [1..2] node adresi [3] son yollanan seq [4] tabloda var mi
//   [5] tablodaki seq [6] tablodaki flags [7] 0

const int cmdTypeNodeStatus = 0xC6;
const int nodeStatusLen = 8;

class NodeStatus {
  final int addr;
  final int lastTxSeq;
  final bool inTable;
  final int tableSeq;
  final int tableFlags;

  const NodeStatus({
    required this.addr,
    required this.lastTxSeq,
    required this.inTable,
    required this.tableSeq,
    required this.tableFlags,
  });

  /// Admin'deki kayit, son yolladigimizdan kac paket geride (0 = en son paket ulasti)
  int get lag => (lastTxSeq - tableSeq) & 0xFF;

  static NodeStatus? decode(List<int> d) {
    if (d.length != nodeStatusLen || d[0] != cmdTypeNodeStatus) return null;
    return NodeStatus(
      addr: d[1] | (d[2] << 8),
      lastTxSeq: d[3],
      inTable: d[4] == 1,
      tableSeq: d[5],
      tableFlags: d[6],
    );
  }
}

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

// ---------------- JOIN v1: takima katilma (telefon <-> SADECE bagli node) ----------------
// LoRa'ya bunlar cikmaz; node admin'le 0xA1 / 0xA2 ile konusur.
//   0xC9 KATILMA PARAMETRELERI (15 B): [1] takim 1..26 [2] numara 0..99 (0 = admin secsin)
//        [3..4] PIN u16 LE (0..9999) [5..14] 0
//   0xCC TAKIMDAN AYRIL (15 B): [1..14] 0. Node admin'e 0xA4 yollar, onay gelince
//        (ya da 3 deneme bitince) kimligi siler.
//   0xCB KATILMA DURUMU (node -> telefon, 8 B): [1] durum [2] sonuc [3..4] adres
//        [5] takim [6] numara (sonuc 4'te onerilen numara) [7] istek sayaci
//        sonuc: 0 ok, 1 yanlis PIN, 2 red, 3 takim dolu, 4 numara dolu,
//               5 admin takimdan cikardi, 6 takimdan ayrildin

const int cmdTypeJoinParams = 0xC9;
const int cmdTypeLeave = 0xCC;
const int cmdTypeJoinStatus = 0xCB;
const int joinStatusLen = 8;

List<int> encodeJoinParams(
    {required int team, required int num, required int pin}) {
  final b = ByteData(cmdLen);
  b.setUint8(0, cmdTypeJoinParams);
  b.setUint8(1, team);
  b.setUint8(2, num);
  b.setUint16(3, pin, Endian.little);
  return b.buffer.asUint8List();
}

List<int> encodeLeave() {
  final b = ByteData(cmdLen);
  b.setUint8(0, cmdTypeLeave);
  return b.buffer.asUint8List();
}

enum JoinState { disabled, needParams, waiting, joined, failed, leaving }

class JoinStatus {
  final JoinState state;
  final int result; // 0 ok, 1 PIN, 2 red, 3 takim dolu, 4 numara dolu, 5 atildi, 6 ayrildi
  final int addr;
  final int team;
  final int num;
  final int reqSeq;

  const JoinStatus({
    required this.state,
    required this.result,
    required this.addr,
    required this.team,
    required this.num,
    required this.reqSeq,
  });

  String get resultText => switch (result) {
        1 => 'PIN yanlış',
        2 => 'Admin reddetti',
        3 => 'Takım dolu',
        4 => 'Numara dolu (önerilen: $num)',
        5 => 'Admin seni takımdan çıkardı',
        6 => 'Takımdan ayrıldın',
        _ => '',
      };

  static JoinStatus? decode(List<int> d) {
    if (d.length != joinStatusLen || d[0] != cmdTypeJoinStatus) return null;
    if (d[1] >= JoinState.values.length) return null;
    return JoinStatus(
      state: JoinState.values[d[1]],
      result: d[2],
      addr: d[3] | (d[4] << 8),
      team: d[5],
      num: d[6],
      reqSeq: d[7],
    );
  }
}
