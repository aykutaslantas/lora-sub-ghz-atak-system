// Haritaya konan isaretler ve komutlar (ATAK'taki "nokta birak" mantigi).
//
// Faz 2: koydugun isaret/komut LoRa ile aga gider (0xC1 paketi, bkz. cmd_packet.dart),
// baskalarininki aga gelince haritaya duser. Uygulama kapaninca liste silinir.

import 'dart:math';

import 'package:flutter/material.dart';
import 'package:latlong2/latlong.dart';

import 'cmd_packet.dart';
import 'teams.dart';

enum MarkKind { poi, hazard, rally, cmdCome, cmdHold, cmdCheck }

extension MarkKindInfo on MarkKind {
  String get label {
    switch (this) {
      case MarkKind.poi:
        return 'İlgi noktası';
      case MarkKind.hazard:
        return 'Tehlike';
      case MarkKind.rally:
        return 'Toplanma noktası';
      case MarkKind.cmdCome:
        return 'Buraya gel';
      case MarkKind.cmdHold:
        return 'Burada bekle';
      case MarkKind.cmdCheck:
        return 'Bölgeyi kontrol et';
    }
  }

  IconData get icon {
    switch (this) {
      case MarkKind.poi:
        return Icons.place;
      case MarkKind.hazard:
        return Icons.warning_amber_rounded;
      case MarkKind.rally:
        return Icons.flag;
      case MarkKind.cmdCome:
        return Icons.directions_walk;
      case MarkKind.cmdHold:
        return Icons.pan_tool;
      case MarkKind.cmdCheck:
        return Icons.travel_explore;
    }
  }

  Color get color {
    switch (this) {
      case MarkKind.poi:
        return Colors.blue;
      case MarkKind.hazard:
        return Colors.red;
      case MarkKind.rally:
        return Colors.green;
      case MarkKind.cmdCome:
      case MarkKind.cmdHold:
      case MarkKind.cmdCheck:
        return Colors.orange;
    }
  }

  bool get isCommand =>
      this == MarkKind.cmdCome ||
      this == MarkKind.cmdHold ||
      this == MarkKind.cmdCheck;
}

/// Isaretin ag durumu
enum MarkState {
  sending, // BLE'ye yaziliyor
  sent, // ESP32'ye gitti, agdan donus bekleniyor
  confirmed, // gateway'in tekrar yayini geri geldi = aga ulasti
  failed, // BLE bagli degil / yazma hatasi -> sadece bu telefonda
  remote, // baska bir node'dan geldi
  deletePending, // silme istegi admin'e gitti, karar bekleniyor
}

extension MarkStateInfo on MarkState {
  String get text {
    switch (this) {
      case MarkState.sending:
        return 'gönderiliyor';
      case MarkState.sent:
        return 'gönderildi, ağdan onay bekleniyor';
      case MarkState.confirmed:
        return 'ağa ulaştı ✓✓';
      case MarkState.failed:
        return 'gönderilemedi (sadece bu telefonda)';
      case MarkState.remote:
        return 'ağdan geldi';
      case MarkState.deletePending:
        return 'silme onayı bekleniyor (admin)';
    }
  }

  /// Etiketin sonuna eklenen kisa isaret
  String get badge {
    switch (this) {
      case MarkState.sending:
        return ' …';
      case MarkState.sent:
        return ' ✓';
      case MarkState.confirmed:
        return ' ✓✓';
      case MarkState.failed:
        return ' ✗';
      case MarkState.remote:
        return '';
      case MarkState.deletePending:
        return ' ⏳';
    }
  }
}

class MapMark {
  final int id;
  final MarkKind kind;
  final LatLng point;
  final int? target; // null = herkes, yoksa node adresi
  final DateTime time;
  final int? src; // null = bu telefon, yoksa gonderen node
  final int? msgId;
  MarkState state;
  // Silme istegi suruyorsa: onceki durum, istek no, zaman asimi
  MarkState? stateBeforeDelete;
  int? delReqId;
  DateTime? delDeadline;

  MapMark({
    required this.id,
    required this.kind,
    required this.point,
    required this.target,
    required this.time,
    this.src,
    this.msgId,
    required this.state,
  });

  bool get mine => src == null;

  String get targetText => target == null ? 'Herkes' : nodeName(target!);
  String get fromText => src == null ? 'Ben' : nodeName(src!);

  String get timeText {
    String two(int v) => v.toString().padLeft(2, '0');
    return '${two(time.hour)}:${two(time.minute)}:${two(time.second)}';
  }
}

/// Agdan gelen paketin sonucu
enum NetResult { duplicate, confirmed, added, rejected }

/// Admin kararinin bu telefondaki sonucu
enum DecisionResult {
  duplicate, // ayni karar tekrar geldi
  deleted, // onay: isaret silindi
  deletedUnknown, // onay geldi ama bu telefonda o isaret yok
  rejectedMine, // benim istegim reddedildi, isaret geri geldi
  rejectedOther, // baskasinin istegi reddedildi
}

/// Admin bu sure icinde cevap vermezse istek dusurulur
const Duration kDeleteTimeout = Duration(seconds: 60);

/// Admin'in "bolgeye geri don" hedefi (en son gelen gecerli)
class GoBackTarget {
  final LatLng point;
  final int msgId;
  final DateTime time;

  const GoBackTarget(
      {required this.point, required this.msgId, required this.time});
}

/// Zybo hatirlatmayi 60 sn'de bir tekrarlar. Bu kadar sure yeni hatirlatma
/// gelmezse node alana donmus sayilir ve hedef haritadan kalkar.
const Duration kGoBackTimeout = Duration(seconds: 150);

class MarkStore {
  MarkStore._();
  static final MarkStore instance = MarkStore._();

  final ValueNotifier<List<MapMark>> marks = ValueNotifier([]);
  int _nextId = 1;
  int _nextMsgId = Random().nextInt(256);
  // Gorulen paket anahtarlari -> ilk gorulme zamani. msg_id 8 bit oldugu icin
  // 256 mesajdan sonra tekrar kullanilir; bu yuzden sadece kDedupWindow icinde
  // ayni anahtar "tekrar" sayilir, sonrasi yeni mesajdir.
  final Map<String, DateTime> _seen = {};
  static const Duration kDedupWindow = Duration(seconds: 60);

  /// Anahtar pencere icinde goruldu mu? Gorulmediyse kaydeder ve false doner.
  bool _isDuplicate(String key, [DateTime? now]) {
    final t = now ?? DateTime.now();
    _seen.removeWhere((_, at) => t.difference(at) > kDedupWindow);
    if (_seen.containsKey(key)) return true;
    _seen[key] = t;
    return false;
  }

  /// Bu telefonun bagli oldugu node (kendi komutumuz agdan geri donunce ogrenilir)
  int? myNodeAddr;

  // ---------------- Admin: bolgeye geri don ----------------

  /// Gecerli geri donus hedefi (null = yok). Yenisi gelince eskisinin yerine gecer.
  final ValueNotifier<GoBackTarget?> goBack = ValueNotifier(null);

  /// GERI DON paketi geldi. Yeni ve bu telefona aitse hedefi gunceller, true doner.
  bool onGoBack(GoBackCmd c, {DateTime? now}) {
    final t = now ?? DateTime.now();
    if (_isDuplicate('gb:${c.src}:${c.msgId}', t)) return false;
    // Node zaten sadece kendi adresine gelenleri iletir; adresimizi biliyorsak
    // yine de kontrol et
    if (myNodeAddr != null && c.dst != myNodeAddr) return false;
    goBack.value = GoBackTarget(point: c.point, msgId: c.msgId, time: t);
    return true;
  }

  void clearGoBack() => goBack.value = null;

  /// Hatirlatma kesildiyse (node alana dondu) hedefi kaldir. Kaldirdiysa true.
  bool expireGoBack(DateTime now) {
    final g = goBack.value;
    if (g != null && now.difference(g.time) > kGoBackTimeout) {
      goBack.value = null;
      return true;
    }
    return false;
  }

  int newMsgId() {
    final id = _nextMsgId;
    _nextMsgId = (_nextMsgId + 1) & 0xFF;
    return id;
  }

  MapMark add(MarkKind kind, LatLng point, int? target,
      {int? msgId, int? src, MarkState state = MarkState.sending}) {
    final m = MapMark(
      id: _nextId++,
      kind: kind,
      point: point,
      target: target,
      time: DateTime.now(),
      src: src,
      msgId: msgId,
      state: state,
    );
    marks.value = [...marks.value, m];
    return m;
  }

  void setState(int id, MarkState s) {
    for (final m in marks.value) {
      if (m.id == id) m.state = s;
    }
    marks.value = List.of(marks.value); // dinleyenler yeniden cizsin
  }

  void remove(int id) =>
      marks.value = marks.value.where((m) => m.id != id).toList();

  void clear() => marks.value = [];

  // ---------------- Silme (admin onayli) ----------------

  /// Isaretin agdaki anahtari: (src, msgId). Bizim isaretlerde src = bagli node.
  int? netSrcOf(MapMark m) => m.src ?? myNodeAddr;

  /// Agdan silinebilir mi? (aga ulasmis ve anahtari belli)
  bool canDeleteOnNetwork(MapMark m) =>
      m.msgId != null &&
      netSrcOf(m) != null &&
      (m.state == MarkState.confirmed || m.state == MarkState.remote);

  void markDeletePending(MapMark m, int reqId) {
    m.stateBeforeDelete = m.state;
    m.delReqId = reqId;
    m.delDeadline = DateTime.now().add(kDeleteTimeout);
    setState(m.id, MarkState.deletePending);
  }

  void _restore(MapMark m) {
    final prev = m.stateBeforeDelete ?? MarkState.remote;
    m.stateBeforeDelete = null;
    m.delReqId = null;
    m.delDeadline = null;
    setState(m.id, prev);
  }

  /// Zamani dolan silme isteklerini geri al, geri alinanlari dondur
  List<MapMark> expirePending(DateTime now) {
    final out = <MapMark>[];
    for (final m in List.of(marks.value)) {
      if (m.state == MarkState.deletePending &&
          m.delDeadline != null &&
          now.isAfter(m.delDeadline!)) {
        _restore(m);
        out.add(m);
      }
    }
    return out;
  }

  /// Admin karari geldi (0xC3)
  /// Gec gelen onay da (telefon zaman asimina ugradiktan sonra) isareti siler;
  /// onay HERKES icin gecerlidir (dst baska bir isteyen olsa bile).
  (DecisionResult, MapMark?) onDecision(AdminDecision d, {DateTime? now}) {
    final key = 'adm:${d.msgId}:${d.targetSrc}:${d.targetMsgId}';
    if (_isDuplicate(key, now)) return (DecisionResult.duplicate, null);

    MapMark? m;
    for (final x in marks.value) {
      if (x.msgId == d.targetMsgId && netSrcOf(x) == d.targetSrc) m = x;
    }
    if (d.approved) {
      if (m == null) return (DecisionResult.deletedUnknown, null);
      remove(m.id);
      return (DecisionResult.deleted, m);
    }
    // Red: sadece bizim bekleyen istegimizse geri al
    if (m != null &&
        m.state == MarkState.deletePending &&
        m.delReqId == d.reqMsgId &&
        (myNodeAddr == null || d.dst == myNodeAddr)) {
      _restore(m);
      return (DecisionResult.rejectedMine, m);
    }
    return (DecisionResult.rejectedOther, m);
  }

  /// Agdan komut paketi geldi.
  ///  - Daha once goruldu -> duplicate
  ///  - Bizim gonderdigimiz geri dondu -> confirmed (isaret guncellenir)
  ///  - Baskasindan yeni -> added (haritaya eklenir)
  ///  - Gonderen [accept] testinden gecmedi -> rejected (or. whitelist disi node)
  (NetResult, MapMark?) onNetwork(CmdPacket p, {bool Function(int src)? accept}) {
    final key = '${p.src}:${p.msgId}';
    if (_isDuplicate(key)) return (NetResult.duplicate, null);

    // Bizim gonderdigimiz mi? (ayni msgId + tur + konum, hala onay bekliyor)
    for (final m in marks.value) {
      if (m.mine &&
          m.msgId == p.msgId &&
          m.kind == p.kind &&
          (m.state == MarkState.sent || m.state == MarkState.sending) &&
          toE7(m.point.latitude) == p.latE7 &&
          toE7(m.point.longitude) == p.lonE7) {
        myNodeAddr = p.src;
        setState(m.id, MarkState.confirmed);
        return (NetResult.confirmed, m);
      }
    }

    if (accept != null && !accept(p.src)) return (NetResult.rejected, null);

    final m = add(p.kind, p.point, p.toAll ? null : p.dst,
        msgId: p.msgId, src: p.src, state: MarkState.remote);
    return (NetResult.added, m);
  }

  /// Bu komut bana mi? (herkese ya da benim node'uma)
  bool isForMe(MapMark m) =>
      m.target == null || (myNodeAddr != null && m.target == myNodeAddr);
}
