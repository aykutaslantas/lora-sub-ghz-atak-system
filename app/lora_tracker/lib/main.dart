// LoRa Tracker: LoRaNode-BLE (ESP32-C3) ile BLE uzerinden baglanir,
// node'un aldigi "kim nerede" tablosunu listeler.
//
// Akis: tara -> "LoRaNode-BLE" bulununca baglan (MTU 247) -> servis 0xABF0 /
// karakteristik 0xABF1 -> notify ac -> gelen her tabloyu coz ve goster.
// Baglanti koparsa birkac saniye sonra kendiliginden yeniden arar.

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:latlong2/latlong.dart';

import 'find_page.dart';
import 'join_page.dart';
import 'cmd_packet.dart';
import 'location_service.dart';
import 'marks.dart';
import 'map_view.dart';
import 'table_parser.dart';
import 'teams.dart';

const String kDeviceName = 'LoRaNode-BLE';
final Guid kServiceUuid = Guid('abf0');
final Guid kCharUuid = Guid('abf1');

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  // ATAK gibi: yatay ve tam ekran
  SystemChrome.setPreferredOrientations([
    DeviceOrientation.landscapeLeft,
    DeviceOrientation.landscapeRight,
  ]);
  SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky);
  runApp(const MyApp());
}

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'LoRa Tracker',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
            seedColor: Colors.teal, brightness: Brightness.dark),
        useMaterial3: true,
      ),
      home: const TrackerPage(),
    );
  }
}

enum LinkState { idle, scanning, connecting, connected, error }

class TrackerPage extends StatefulWidget {
  const TrackerPage({super.key});

  @override
  State<TrackerPage> createState() => _TrackerPageState();
}

class _TrackerPageState extends State<TrackerPage> {
  LinkState _state = LinkState.idle;
  String _status = 'Başlatılıyor...';
  BluetoothDevice? _device;
  BluetoothCharacteristic? _chr; // 0xABF1: notify ile tablo gelir, write ile komut gider
  int _mtu = 0;
  List<NodeRecord> _nodes = [];
  DateTime? _lastTable;
  int _tableCount = 0;
  String _lastHex = '';
  // "Bul" ekrani acikken de guncel tabloyu gorsun diye
  final ValueNotifier<List<NodeRecord>> _nodesVN = ValueNotifier([]);

  StreamSubscription<List<ScanResult>>? _scanSub;
  StreamSubscription<BluetoothConnectionState>? _connSub;
  StreamSubscription<List<int>>? _valueSub;
  StreamSubscription<BluetoothAdapterState>? _adapterSub;
  Timer? _ticker; // "x sn once" yazisini her saniye tazeler
  Timer? _retry;
  bool _disposed = false;
  bool _goBackDialogOpen = false;

  // ---- 0xC4: telefonun konumunu node'un konumu yap ----
  bool _sharePos = false;
  DateTime? _lastPosSent;
  static const Duration kPosMinGap = Duration(seconds: 5); // en sik
  static const Duration kPosRefresh = Duration(seconds: 15); // hareket yoksa da

  // ---- Dolayli onay (0xC6): son konumum admin'e ulasti mi ----
  NodeStatus? _nodeStatus;
  DateTime? _nodeStatusTime;
  // OKUNDU ACK'i yollanan komutlar (ayni komuta iki kez yollama)
  final Set<String> _seenAcked = {};

  // ---- JOIN v1 (0xCB): node'un takim kimligi ----
  final ValueNotifier<JoinStatus?> _joinVN = ValueNotifier(null);
  // Kullanicinin yolladigi ama node'un henuz almadigi parametreler
  // (node o sirada yeniden baslarsa "parametre bekliyorum" der, tekrar yollariz)
  ({int team, int num, int pin})? _pendingJoin;
  bool _joinPageOpen = false;
  bool _joinPromptShown = false;

  @override
  void initState() {
    super.initState();
    _ticker = Timer.periodic(const Duration(seconds: 1), (_) {
      if (!mounted) return;
      setState(() {});
      // Admin 60 sn icinde cevap vermediyse silme istegini geri al
      for (final m in MarkStore.instance.expirePending(DateTime.now())) {
        _snack('Admin cevap vermedi: "${m.kind.label}" silinmedi, tekrar deneyebilirsin');
      }
      // Admin "geri don" hatirlatmasi kesildiyse (alana donduk) hedefi kaldir
      if (MarkStore.instance.expireGoBack(DateTime.now())) {
        _snack('ADMIN: geri dön hatırlatması bitti, hedef kaldırıldı');
      }
      // Konum paylasimi acikken hareket olmasa da node'a tazele (node 60 sn'de birakir)
      _maybeSendPos(refresh: true);
    });
    _adapterSub = FlutterBluePlus.adapterState.listen((s) {
      if (s == BluetoothAdapterState.on) {
        if (_state == LinkState.idle || _state == LinkState.error) {
          _startScan();
        }
      } else if (s == BluetoothAdapterState.off) {
        _setStatus(LinkState.error, "Bluetooth kapalı. Telefonda Bluetooth'u açın.");
      }
    });
    // Telefonun GPS'i. BLE izin penceresiyle cakismasin diye biraz sonra.
    Future<void>.delayed(
        const Duration(seconds: 2), LocationService.instance.start);
    // Konum degisince (paylasim aciksa) node'a yolla
    LocationService.instance.current.addListener(_onMyLocation);
  }

  // ---- 0xC4: konum paylasimi ----
  void _onMyLocation() => _maybeSendPos();

  /// Paylasim aciksa ve zamani geldiyse konumu node'a yolla.
  ///  - konum degisti (listener): en fazla 5 sn'de bir
  ///  - refresh (her saniyelik ticker): degismese de 15 sn'de bir
  ///  - force: hemen (acildiginda / yeniden baglanildiginda)
  Future<void> _maybeSendPos({bool force = false, bool refresh = false}) async {
    if (!_sharePos || _state != LinkState.connected) return;
    final me = LocationService.instance.current.value;
    if (me == null) return;
    final now = DateTime.now();
    final last = _lastPosSent;
    if (!force && last != null &&
        now.difference(last) < (refresh ? kPosRefresh : kPosMinGap)) {
      return;
    }
    _lastPosSent = now;
    await _writeBle(encodeSetPos(me.point));
  }

  Future<void> _toggleSharePos() async {
    if (_sharePos) {
      _sharePos = false;
      final ok = await _writeBle(encodeSetPos(null));
      _snack(ok
          ? 'Konum paylaşımı kapandı: node sahte konuma döndü'
          : 'Konum paylaşımı kapandı (BLE bağlı değil, node 60 sn içinde kendisi bırakır)');
    } else {
      if (LocationService.instance.current.value == null) {
        _snack('Önce konumun lazım: ${LocationService.instance.status.value}');
        return;
      }
      _sharePos = true;
      _lastPosSent = null;
      await _maybeSendPos(force: true);
      _snack(_state == LinkState.connected
          ? 'Node artık bu telefonun konumunu yayınlıyor (fix=1)'
          : 'Konum paylaşımı açık, BLE bağlanınca gönderilecek');
    }
    if (mounted) setState(() {});
  }

  void _openFind(NodeRecord n) {
    Navigator.of(context).push(MaterialPageRoute<void>(
      builder: (_) => FindPage.node(addr: n.addr, nodes: _nodesVN),
    ));
  }

  void _openFindPoint(String title, LatLng p) {
    Navigator.of(context).push(MaterialPageRoute<void>(
      builder: (_) => FindPage.point(title: title, point: p),
    ));
  }

  void _snack(String msg) {
    if (!mounted) return;
    ScaffoldMessenger.of(context)
      ..hideCurrentSnackBar()
      ..showSnackBar(SnackBar(
          content: Text(msg), duration: const Duration(seconds: 3)));
  }

  // ---- Faz 2: isaret / komutu aga gonder ----
  Future<void> _placeMark(MarkKind kind, LatLng p, int? target) async {
    final store = MarkStore.instance;
    final msgId = store.newMsgId();
    final m = store.add(kind, p, target, msgId: msgId);
    final pkt = CmdPacket(
      src: 0, // node kendi adresini yazar
      dst: target ?? cmdDstAll,
      msgId: msgId,
      kind: kind,
      latE7: toE7(p.latitude),
      lonE7: toE7(p.longitude),
    );
    final ok = await _writeBle(pkt.encode());
    store.setState(m.id, ok ? MarkState.sent : MarkState.failed);
    _snack(ok
        ? '${kind.label} → ${m.targetText} gönderildi, ağdan onay bekleniyor'
        : 'BLE bağlı değil: ${kind.label} sadece bu telefonda kaldı');
  }

  Future<bool> _writeBle(List<int> data) async {
    final c = _chr;
    if (c == null || _state != LinkState.connected) return false;
    try {
      await c.write(data, withoutResponse: false, timeout: 5);
      return true;
    } catch (_) {
      return false;
    }
  }

  // ---- Silme: aga ulasmis isaret icin admin'e istek ----
  Future<void> _deleteMark(MapMark m) async {
    final store = MarkStore.instance;
    if (!store.canDeleteOnNetwork(m)) {
      // Aga hic ulasmamis (✗ / ✓ bekliyor): sadece bu telefonda var
      store.remove(m.id);
      _snack('"${m.kind.label}" ağa ulaşmamıştı, sadece bu telefondan silindi');
      return;
    }
    final reqId = store.newMsgId();
    final ok = await _writeBle(encodeDelRequest(
        msgId: reqId, targetSrc: store.netSrcOf(m)!, targetMsgId: m.msgId!));
    if (!ok) {
      _snack('BLE bağlı değil: silme isteği gönderilemedi');
      return;
    }
    store.markDeletePending(m, reqId);
    _snack('Silme isteği admin\'e gönderildi, onay bekleniyor');
  }

  // ---- Silme: admin karari geldi (0xC3) ----
  void _onDecision(AdminDecision d) {
    final (res, m) = MarkStore.instance.onDecision(d);
    final name = m == null ? 'işaret' : '"${m.kind.label}"';
    switch (res) {
      case DecisionResult.duplicate:
      case DecisionResult.deletedUnknown:
        return;
      case DecisionResult.deleted:
        final mine = d.dst != cmdDstAll &&
            d.dst == MarkStore.instance.myNodeAddr;
        _snack(mine
            ? 'Admin onayladı: $name herkesten silindi'
            : 'Admin $name işaretini sildi');
      case DecisionResult.rejectedMine:
        _snack('Admin silme isteğini reddetti: $name yerinde kaldı');
      case DecisionResult.rejectedOther:
        return;
    }
  }

  // ---- Faz 2: agdan komut paketi geldi ----
  void _onCmd(CmdPacket p) {
    // Sadece son tabloda olan node'lardan (Zybo whitelist'inden gecmis) ya da
    // ayrilmis adreslerden (0xFFF0+, or. admin) gelen komutlari kabul et
    final (res, m) = MarkStore.instance.onNetwork(p,
        accept: (src) => src >= 0xFFF0 || _nodes.any((n) => n.addr == src));
    if (res == NetResult.rejected) {
      debugPrint('Komut yok sayildi: ${nodeName(p.src)} tabloda yok');
      return;
    }
    if (m == null) return; // tekrar
    if (res == NetResult.confirmed) {
      _snack('✓✓ Ağa ulaştı: ${m.kind.label} → ${m.targetText} '
          '(node ${nodeName(p.src)})');
      return;
    }
    // Baskasindan yeni isaret / komut. Admin'den gelen HER sey (isaret dahil) uyari
    // penceresi acar: Tamam / Bul'a basilinca admin'e OKUNDU gider.
    if ((m.kind.isCommand && MarkStore.instance.isForMe(m)) || m.src == addrAdmin) {
      _showCmdAlert(m);
    } else {
      _snack('${m.fromText}: ${m.kind.label}'
          '${m.kind.isCommand ? ' → ${m.targetText}' : ''}');
    }
  }

  // ---- Admin: bolgeye geri don (0xC1, tur 7) ----
  void _onGoBack(GoBackCmd c) {
    if (!MarkStore.instance.onGoBack(c)) return; // tekrar ya da baska node'a
    HapticFeedback.heavyImpact();
    if (_goBackDialogOpen) {
      // Pencere zaten acik: hedef guncellendi, ikinci pencere acma
      _snack('ADMIN: bölgeye geri dön (hedef güncellendi)');
      return;
    }
    _showGoBackAlert();
  }

  void _showGoBackAlert() {
    final g = MarkStore.instance.goBack.value;
    if (g == null || !mounted) return;
    final me = LocationService.instance.current.value;
    final range = me == null
        ? ''
        : '\n${fmtDistance(distanceM(me.point, g.point))} · '
            '${compassName(bearingDeg(me.point, g.point))}';
    _goBackDialogOpen = true;
    showDialog<void>(
      context: context,
      builder: (ctx) => AlertDialog(
        icon: const Icon(Icons.u_turn_left, color: Colors.deepOrange, size: 40),
        title: const Text('ADMIN: Bölgeye geri dön'),
        content: Text('Görev alanının dışındasın. '
            'Haritadaki turuncu hedefe dön.$range'),
        actions: [
          TextButton(
              onPressed: () {
                Navigator.pop(ctx);
                _sendSeen(addrAdmin, g.msgId);
              },
              child: const Text('Tamam')),
          FilledButton.icon(
            icon: const Icon(Icons.navigation),
            label: const Text('Bul'),
            onPressed: () {
              Navigator.pop(ctx);
              _sendSeen(addrAdmin, g.msgId);
              _openFindPoint('ADMIN: Bölgeye geri dön', g.point);
            },
          ),
        ],
      ),
    ).whenComplete(() => _goBackDialogOpen = false);
  }

  // ---- 0xC5 OKUNDU: kullanici admin komutunu gordu ----
  Future<void> _sendSeen(int src, int msgId) async {
    final key = '$src:$msgId';
    if (_seenAcked.contains(key)) return;
    final ok = await _writeBle(encodeSeenAck(targetSrc: src, targetMsgId: msgId));
    if (ok) _seenAcked.add(key);
  }

  // ---- 0xC6: node durumu (son konumum admin'e ulasti mi) ----
  void _onNodeStatus(NodeStatus s) {
    MarkStore.instance.myNodeAddr = s.addr; // telefonun bagli oldugu node artik kesin
    if (!mounted) return;
    setState(() {
      _nodeStatus = s;
      _nodeStatusTime = DateTime.now();
    });
  }

  // ---- JOIN v1: node'dan katilma durumu (0xCB) ----
  void _onJoinStatus(JoinStatus s) {
    final prev = _joinVN.value;
    _joinVN.value = s;
    switch (s.state) {
      case JoinState.joined:
        MarkStore.instance.myNodeAddr = s.addr;
        _pendingJoin = null;
        if (prev != null && prev.state != JoinState.joined) {
          HapticFeedback.mediumImpact();
          _snack('Admin onayladı: artık ${nodeName(s.addr)} olarak görünüyorsun');
        }
      case JoinState.failed:
        _pendingJoin = null;
        if (prev == null ||
            prev.state != JoinState.failed ||
            prev.reqSeq != s.reqSeq) {
          _snack('Katılma reddedildi: ${s.resultText}');
        }
      case JoinState.needParams:
        final wasMember = prev != null &&
            (prev.state == JoinState.joined || prev.state == JoinState.leaving);
        if (wasMember && s.result == 5) {
          // Admin takimdan cikardi
          _pendingJoin = null;
          HapticFeedback.heavyImpact();
          _showKickedAlert(prev!.addr);
          break;
        }
        if (wasMember && s.result == 6) {
          _pendingJoin = null;
          _snack('Takımdan ayrıldın (${nodeName(prev!.addr)})');
          break;
        }
        // Yolladigimiz parametreler node'a ulasmadiysa (node resetlendi vs.) tekrar yolla
        final p = _pendingJoin;
        if (p != null) {
          _writeBle(encodeJoinParams(team: p.team, num: p.num, pin: p.pin));
        } else if (!_joinPromptShown && !_joinPageOpen) {
          // Kimliksiz node: katilma ekranini bir kez kendiliginden ac
          _joinPromptShown = true;
          _openJoin();
        }
      case JoinState.waiting:
        _pendingJoin = null; // node aldi
      case JoinState.leaving:
        break;
      case JoinState.disabled:
        break;
    }
    if (mounted) setState(() {});
  }

  Future<bool> _sendJoin(int team, int num, int pin) async {
    _pendingJoin = (team: team, num: num, pin: pin);
    return _writeBle(encodeJoinParams(team: team, num: num, pin: pin));
  }

  Future<bool> _sendLeave() async {
    _pendingJoin = null;
    _joinPromptShown = true; // ayrilan kullanici zaten bu ekranda
    return _writeBle(encodeLeave());
  }

  void _showKickedAlert(int oldAddr) {
    if (!mounted) return;
    showDialog<void>(
      context: context,
      builder: (ctx) => AlertDialog(
        icon: const Icon(Icons.person_remove, color: Colors.redAccent, size: 40),
        title: const Text('ADMIN seni takımdan çıkardı'),
        content: Text('${nodeName(oldAddr)} kimliği silindi, node konum yayınlamıyor. '
            'Tekrar katılmak için admin onayı gerekir.'),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(ctx), child: const Text('Tamam')),
          FilledButton(
            onPressed: () {
              Navigator.pop(ctx);
              _openJoin();
            },
            child: const Text('Tekrar katıl'),
          ),
        ],
      ),
    );
  }

  void _openJoin() {
    if (_joinPageOpen || !mounted) return;
    _joinPageOpen = true;
    Navigator.of(context)
        .push(MaterialPageRoute<void>(
          builder: (_) => JoinPage(
              status: _joinVN, onJoin: _sendJoin, onLeave: _sendLeave),
        ))
        .whenComplete(() => _joinPageOpen = false);
  }

  void _showCmdAlert(MapMark m) {
    final me = LocationService.instance.current.value;
    final range = me == null
        ? ''
        : '\n${fmtDistance(distanceM(me.point, m.point))} · '
            '${compassName(bearingDeg(me.point, m.point))}';
    showDialog<void>(
      context: context,
      builder: (ctx) => AlertDialog(
        icon: Icon(m.kind.icon, color: m.kind.color, size: 40),
        title: Text('${m.fromText}: ${m.kind.label}'),
        content: Text(
            'Hedef: ${m.target == null ? 'Herkes' : 'Sen'} · ${m.timeText}$range'),
        actions: [
          TextButton(
              onPressed: () {
                Navigator.pop(ctx);
                if (m.src == addrAdmin && m.msgId != null) _sendSeen(addrAdmin, m.msgId!);
              },
              child: const Text('Tamam')),
          FilledButton.icon(
            icon: const Icon(Icons.navigation),
            label: const Text('Bul'),
            onPressed: () {
              Navigator.pop(ctx);
              if (m.src == addrAdmin && m.msgId != null) _sendSeen(addrAdmin, m.msgId!);
              _openFindPoint('${m.fromText}: ${m.kind.label}', m.point);
            },
          ),
        ],
      ),
    );
  }

  @override
  void dispose() {
    _disposed = true;
    LocationService.instance.current.removeListener(_onMyLocation);
    _nodesVN.dispose();
    _joinVN.dispose();
    _ticker?.cancel();
    _retry?.cancel();
    _scanSub?.cancel();
    _connSub?.cancel();
    _valueSub?.cancel();
    _adapterSub?.cancel();
    FlutterBluePlus.stopScan();
    _device?.disconnect();
    super.dispose();
  }

  void _setStatus(LinkState s, String msg) {
    if (!mounted) return;
    setState(() {
      _state = s;
      _status = msg;
    });
  }

  bool _sameUuid(Guid a, Guid b) =>
      a.str128.toLowerCase() == b.str128.toLowerCase();

  // ---- 1) Tarama ----
  Future<void> _startScan() async {
    if (_disposed) return;
    _retry?.cancel();
    _setStatus(LinkState.scanning, '$kDeviceName aranıyor...');

    await _scanSub?.cancel();
    _scanSub = FlutterBluePlus.scanResults.listen(
      (results) {
        for (final r in results) {
          final adv = r.advertisementData.advName;
          final name = adv.isNotEmpty ? adv : r.device.platformName;
          if (name == kDeviceName && _state == LinkState.scanning) {
            _connect(r.device);
            break;
          }
        }
      },
      onError: (Object e) => _setStatus(LinkState.error, 'Tarama hatası: $e'),
    );

    try {
      await FlutterBluePlus.startScan(
        withNames: [kDeviceName],
        timeout: const Duration(seconds: 15),
      );
    } catch (e) {
      _setStatus(LinkState.error, 'Tarama başlatılamadı: $e');
      _scheduleRetry();
      return;
    }

    // Sure doldu ve hala bulunamadiysa biraz sonra tekrar dene
    await FlutterBluePlus.isScanning.where((s) => s == false).first;
    if (_state == LinkState.scanning) {
      _setStatus(LinkState.error, '$kDeviceName bulunamadı. ESP32 açık mı?');
      _scheduleRetry();
    }
  }

  void _scheduleRetry() {
    _retry?.cancel();
    _retry = Timer(const Duration(seconds: 3), () {
      if (!_disposed &&
          (_state == LinkState.error || _state == LinkState.idle)) {
        _startScan();
      }
    });
  }

  // ---- 2) Baglanti, MTU, servis, notify ----
  Future<void> _connect(BluetoothDevice d) async {
    _setStatus(LinkState.connecting, 'Bağlanılıyor: ${d.remoteId}');
    await FlutterBluePlus.stopScan();
    await _scanSub?.cancel();
    _scanSub = null;
    _device = d;

    await _connSub?.cancel();
    _connSub = d.connectionState.listen((s) {
      if (s == BluetoothConnectionState.disconnected &&
          _state == LinkState.connected) {
        _valueSub?.cancel();
        _valueSub = null;
        _chr = null;
        _setStatus(LinkState.error, 'Bağlantı koptu, yeniden aranıyor...');
        _scheduleRetry();
      }
    });

    try {
      // 38 byte'lik tablo icin varsayilan MTU (23 -> 20 B veri) yetmez.
      // License.nonprofit: egitim / ogrenci projesi kullanimi.
      await d.connect(
        license: License.nonprofit,
        mtu: 247,
        timeout: const Duration(seconds: 15),
      );
      _mtu = d.mtuNow;

      final services = await d.discoverServices();
      BluetoothCharacteristic? chr;
      for (final s in services) {
        if (_sameUuid(s.uuid, kServiceUuid)) {
          for (final c in s.characteristics) {
            if (_sameUuid(c.uuid, kCharUuid)) chr = c;
          }
        }
      }
      if (chr == null) {
        throw Exception('0xABF0 / 0xABF1 servisi bulunamadı');
      }

      await _valueSub?.cancel();
      _valueSub = chr.onValueReceived.listen(_onData);
      _chr = chr;
      await chr.setNotifyValue(true);
      _setStatus(
          LinkState.connected, 'Bağlı · MTU $_mtu · tablo bekleniyor (~10 sn)');
      // Konum paylasimi aciksa yeniden baglaninca hemen yolla
      _lastPosSent = null;
      _maybeSendPos(force: true);
    } catch (e) {
      _setStatus(LinkState.error, 'Bağlantı hatası: $e');
      try {
        await d.disconnect();
      } catch (_) {}
      _scheduleRetry();
    }
  }

  // ---- 3) Gelen veri ----
  void _onData(List<int> data) {
    // 8 byte 0xCB = katilma durumu (JOIN v1)
    final js = JoinStatus.decode(data);
    if (js != null) {
      _lastHex = toHex(data);
      _onJoinStatus(js);
      return;
    }
    // 8 byte 0xC6 = node durumu (dolayli onay)
    final st = NodeStatus.decode(data);
    if (st != null) {
      _lastHex = toHex(data);
      _onNodeStatus(st);
      return;
    }
    // Silme: 15 byte 0xC3 = admin karari
    final dec = AdminDecision.decode(data);
    if (dec != null) {
      _lastHex = toHex(data);
      _onDecision(dec);
      return;
    }
    // Admin: 15 byte 0xC1, tur 7 = bolgeye geri don (normal isaret degil)
    final gb = GoBackCmd.decode(data);
    if (gb != null) {
      _lastHex = toHex(data);
      _onGoBack(gb);
      return;
    }
    // Faz 2: 15 byte 0xC1 = isaret / komut paketi
    final cmd = CmdPacket.decode(data);
    if (cmd != null) {
      _lastHex = toHex(data);
      _onCmd(cmd);
      return;
    }
    // Tanimadigimiz bir 0xC1 turu (ileride eklenenler): sessizce yok say
    if (data.length == cmdLen && data[0] == cmdType) {
      _lastHex = toHex(data);
      debugPrint('Bilinmeyen komut turu ${data[6]} yok sayildi');
      return;
    }
    final recs = parseTable(data);
    if (!mounted) return;
    setState(() {
      _lastHex = toHex(data);
      if (recs != null) {
        recs.sort((a, b) => a.addr.compareTo(b.addr));
        _nodes = recs;
        _nodesVN.value = List.of(recs);
        _lastTable = DateTime.now();
        _tableCount++;
        _status = 'Bağlı · MTU $_mtu · $_tableCount tablo alındı';
      } else {
        _status =
            'Bağlı · tanınmayan veri (${data.length} B), MTU küçük olabilir';
      }
    });
  }

  Future<void> _manualReconnect() async {
    _retry?.cancel();
    _state = LinkState.idle;
    final d = _device;
    if (d != null) {
      try {
        await d.disconnect();
      } catch (_) {}
    }
    await _valueSub?.cancel();
    _valueSub = null;
    _chr = null;
    _startScan();
  }

  String _age(DateTime? t) {
    if (t == null) return '—';
    final s = DateTime.now().difference(t).inSeconds;
    if (s < 60) return '$s sn önce';
    return '${s ~/ 60} dk ${s % 60} sn önce';
  }

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final Color stateColor = switch (_state) {
      LinkState.connected => Colors.green,
      LinkState.scanning || LinkState.connecting => Colors.orange,
      LinkState.error => Colors.red,
      LinkState.idle => Colors.grey,
    };
    final stale = _lastTable != null &&
        DateTime.now().difference(_lastTable!).inSeconds > 30;

    return Scaffold(
      body: SafeArea(
        child: Column(
          children: [
            _statusBar(cs, stateColor, stale),
            Expanded(
              child: MapView(
                nodes: _nodes,
                stale: stale,
                onFindNode: _openFind,
                onFindPoint: _openFindPoint,
                onPlace: _placeMark,
                onDelete: _deleteMark,
              ),
            ),
          ],
        ),
      ),
    );
  }

  // Dolayli onay gostergesi: son konumum admin'in tablosunda mi?
  //  yesil  = var, en fazla 2 paket geride
  //  turkuaz= var, ama komsu node uzerinden aktarimla (mesh)
  //  sari   = var ama geride (paket kaybi)
  //  kirmizi= tabloda yok (whitelist / menzil)
  //  gri    = 30 sn'dir durum gelmedi (tablo gelmiyor)
  Widget _adminAckChip(TextStyle small) {
    final s = _nodeStatus;
    final t = _nodeStatusTime;
    if (s == null || t == null) {
      return Text('Admin: —', style: small.copyWith(color: Colors.grey));
    }
    final age = DateTime.now().difference(t).inSeconds;
    late final Color c;
    late final String txt;
    if (age > 30) {
      c = Colors.grey;
      txt = 'Admin: ? (${age}sn)';
    } else if (!s.inTable) {
      c = Colors.redAccent;
      txt = 'Admin: yok';
    } else if ((s.tableFlags & 0x04) != 0) {
      // bit2: konumum admin'e komsu node uzerinden (mesh aktarimiyla) ulasiyor
      c = Colors.cyanAccent;
      txt = 'Admin ✓ aktarımla ${age}sn';
    } else if (s.lag <= 2) {
      c = Colors.lightGreenAccent;
      txt = 'Admin ✓ ${age}sn';
    } else {
      c = Colors.amber;
      txt = 'Admin ✓ ${s.lag} geride';
    }
    return Row(mainAxisSize: MainAxisSize.min, children: [
      Icon(Icons.cell_tower, size: 16, color: c),
      const SizedBox(width: 4),
      Text(txt, style: small.copyWith(color: c)),
    ]);
  }

  // Takim kimligi: ALPHA-2 (takim renginde) / KATIL / bekliyor / red.
  // Basinca katilma ekrani acilir. JOIN kapali node'da gorunmez.
  Widget _joinChip(TextStyle small) {
    final s = _joinVN.value;
    if (s == null || s.state == JoinState.disabled) {
      return const SizedBox.shrink();
    }
    final (Color c, IconData ic, String txt) = switch (s.state) {
      JoinState.joined => (teamColor(s.team), Icons.verified, nodeName(s.addr)),
      JoinState.waiting => (Colors.amber, Icons.hourglass_top, 'Onay bekleniyor'),
      JoinState.failed => (Colors.redAccent, Icons.error, 'Katılma reddedildi'),
      JoinState.leaving => (Colors.amber, Icons.logout, 'Ayrılınıyor'),
      _ => (Colors.orange, Icons.person_add, 'KATIL'),
    };
    return InkWell(
      onTap: _openJoin,
      child: Row(mainAxisSize: MainAxisSize.min, children: [
        Icon(ic, size: 16, color: c),
        const SizedBox(width: 4),
        Text(txt,
            style: small.copyWith(color: c, fontWeight: FontWeight.bold)),
      ]),
    );
  }

  // Ust ince durum cubugu (ATAK'taki gibi): BLE | GPS | node sayisi | saat | konum
  Widget _statusBar(ColorScheme cs, Color stateColor, bool stale) {
    String two(int v) => v.toString().padLeft(2, '0');
    final now = DateTime.now();
    const small = TextStyle(fontSize: 12);
    return Container(
      height: 32,
      color: Colors.black,
      padding: const EdgeInsets.symmetric(horizontal: 8),
      child: Row(
        children: [
          Icon(Icons.bluetooth, size: 16, color: stateColor),
          const SizedBox(width: 4),
          Flexible(
            child: Text(_status,
                style: small, maxLines: 1, overflow: TextOverflow.ellipsis),
          ),
          const SizedBox(width: 12),
          Icon(Icons.table_rows,
              size: 16, color: stale ? Colors.red : Colors.grey),
          const SizedBox(width: 4),
          Text('${_nodes.length} node · ${_age(_lastTable)}', style: small),
          const SizedBox(width: 12),
          const Icon(Icons.gps_fixed, size: 16, color: Colors.grey),
          const SizedBox(width: 4),
          ValueListenableBuilder<MyLocation?>(
            valueListenable: LocationService.instance.current,
            builder: (context, me, _) => Text(
              me == null
                  ? LocationService.instance.status.value
                  : '${me.manual ? 'TEST ' : ''}'
                      '${me.point.latitude.toStringAsFixed(5)} '
                      '${me.point.longitude.toStringAsFixed(5)}'
                      '${me.manual ? '' : ' ±${me.accuracy.round()}m'}',
              style: small.copyWith(fontFamily: 'monospace'),
            ),
          ),
          const SizedBox(width: 12),
          _joinChip(small),
          const SizedBox(width: 12),
          _adminAckChip(small),
          const Spacer(),
          // Konum paylasimi: acikken node bu telefonun konumunu yayinlar
          IconButton(
            icon: Icon(_sharePos ? Icons.share_location : Icons.location_disabled,
                size: 18, color: _sharePos ? Colors.lightGreenAccent : Colors.grey),
            tooltip: _sharePos
                ? 'Node konumu = bu telefon (kapat)'
                : 'Node konumu olarak bu telefonu kullan',
            visualDensity: VisualDensity.compact,
            onPressed: _toggleSharePos,
          ),
          Text('${two(now.hour)}:${two(now.minute)}',
              style: small.copyWith(fontWeight: FontWeight.bold)),
          IconButton(
            icon: const Icon(Icons.refresh, size: 18),
            tooltip: 'Yeniden bağlan',
            visualDensity: VisualDensity.compact,
            onPressed: _manualReconnect,
          ),
        ],
      ),
    );
  }
}
