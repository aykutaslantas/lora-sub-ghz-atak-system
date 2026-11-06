// ATAK tarzi taktik harita ekrani (yatay):
//   solda arac cubugu | ortada tam ekran harita + nisangah | sagda node paneli
//
// - Harita: assets/map.mbtiles (Bakirkoy, offline). Ilk acilista uygulama
//   klasorune kopyalanir (flutter_map_mbtiles assets'ten okuyamiyor).
// - Uzun bas: halka menu -> isaret / komut / mesafe / test konumu.
// - Arac cubugu "R&B" (Range & Bearing): benden nisangaha canli mesafe-yon.

import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:flutter_map_mbtiles/flutter_map_mbtiles.dart';
import 'package:latlong2/latlong.dart';
import 'package:path_provider/path_provider.dart';

import 'location_service.dart';
import 'marks.dart';
import 'radial_menu.dart';
import 'table_parser.dart';
import 'teams.dart';

const String kMapAsset = 'assets/map.mbtiles';

// Maperitive'de verdigimiz alan: set-geo-bounds 28.79,40.95,28.89,41.00
final LatLngBounds kMapBounds = LatLngBounds(
  const LatLng(40.95, 28.79), // guney-bati
  const LatLng(41.00, 28.89), // kuzey-dogu
);
const LatLng kMapCenter = LatLng(40.98306, 28.85361); // Bakirkoy merkezi
const double kMinZoom = 12;
const double kMaxNativeZoom = 17; // dosyada olan en yakin zoom
const double kMaxZoom = 18; // 17'nin karolari buyutulerek gosterilir

/// Asset'teki .mbtiles dosyasini uygulama klasorune kopyalar, yolunu dondurur.
Future<String> prepareMapFile() async {
  final dir = await getApplicationDocumentsDirectory();
  final file = File('${dir.path}/map.mbtiles');
  final data = await rootBundle.load(kMapAsset);
  // Dosya yoksa ya da harita degistiyse (boyut farkli) yeniden yaz
  if (!await file.exists() || await file.length() != data.lengthInBytes) {
    await file.writeAsBytes(
      data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes),
      flush: true,
    );
  }
  return file.path;
}

String fmtLatLng(LatLng p) =>
    '${p.latitude.toStringAsFixed(5)}  ${p.longitude.toStringAsFixed(5)}';

// Halka menudeki secenekler
enum _Act { poi, hazard, rally, come, hold, check, measure, testLoc }

const Map<_Act, MarkKind> _actKind = {
  _Act.poi: MarkKind.poi,
  _Act.hazard: MarkKind.hazard,
  _Act.rally: MarkKind.rally,
  _Act.come: MarkKind.cmdCome,
  _Act.hold: MarkKind.cmdHold,
  _Act.check: MarkKind.cmdCheck,
};

class MapView extends StatefulWidget {
  final List<NodeRecord> nodes;
  final bool stale; // son tablo 30 sn'den eskiyse node'lar gri
  final void Function(NodeRecord) onFindNode;
  final void Function(String title, LatLng point) onFindPoint;
  // Isaret / komut koyuldu: kaydet ve aga gonder (main.dart yapar)
  final void Function(MarkKind kind, LatLng point, int? target) onPlace;
  // Sil: aga ulasmis isaretler icin admin'e silme istegi (main.dart yapar)
  final void Function(MapMark m) onDelete;

  const MapView({
    super.key,
    required this.nodes,
    required this.stale,
    required this.onFindNode,
    required this.onFindPoint,
    required this.onPlace,
    required this.onDelete,
  });

  @override
  State<MapView> createState() => _MapViewState();
}

class _MapViewState extends State<MapView> {
  final MapController _map = MapController();
  final ValueNotifier<LatLng> _center = ValueNotifier(kMapCenter);
  final GlobalKey _mapKey = GlobalKey(); // nisangahin ekrandaki yeri icin
  MbTilesTileProvider? _tiles;
  String? _error;
  bool _didAutoFit = false;
  bool _panelOpen = true;
  bool _rbLive = false; // R&B: benden nisangaha
  LatLng? _measurePt; // menuden secilen sabit olcum noktasi

  @override
  void initState() {
    super.initState();
    _open();
  }

  @override
  void dispose() {
    _center.dispose();
    super.dispose();
  }

  Future<void> _open() async {
    try {
      final path = await prepareMapFile();
      if (!mounted) return;
      setState(() => _tiles = MbTilesTileProvider.fromPath(path: path));
    } catch (e) {
      if (mounted) setState(() => _error = 'Harita açılamadı: $e');
    }
  }

  @override
  void didUpdateWidget(MapView old) {
    super.didUpdateWidget(old);
    // Ilk tablo gelince node'lari bir kere ekrana sigdir
    if (!_didAutoFit && _tiles != null && widget.nodes.isNotEmpty) {
      _didAutoFit = true;
      WidgetsBinding.instance.addPostFrameCallback((_) => _fitAll());
    }
  }

  // Not: _tiles'i TileLayer dispose edildiginde kendisi kapatir.

  // ---------------- Arac cubugu islemleri ----------------

  void _fitAll() {
    if (widget.nodes.isEmpty) {
      _map.move(kMapCenter, 14);
      return;
    }
    final pts = widget.nodes.map((n) => LatLng(n.lat, n.lon)).toList();
    if (pts.length == 1) {
      _map.move(pts.first, 16);
      return;
    }
    _map.fitCamera(CameraFit.coordinates(
      coordinates: pts,
      padding: const EdgeInsets.all(60),
      maxZoom: kMaxNativeZoom,
    ));
  }

  void _goToMe() {
    final me = LocationService.instance.current.value;
    if (me == null) {
      _snack(LocationService.instance.status.value);
      return;
    }
    if (!kMapBounds.contains(me.point)) {
      _snack('Konumun harita dışında (Bakırköy\'e '
          '${fmtDistance(distanceM(me.point, kMapCenter))}). '
          'Uzun bas → "Test konumum".');
      return;
    }
    _map.move(me.point, 16);
  }

  void _snack(String msg) {
    ScaffoldMessenger.of(context)
      ..hideCurrentSnackBar()
      ..showSnackBar(SnackBar(
          content: Text(msg), duration: const Duration(seconds: 3)));
  }

  // ---------------- Uzun basma: halka menu ----------------

  // Uzun basma nereye olursa olsun islem NISANGAHTAKI noktaya yapilir.
  void _onLongPress(TapPosition tp, LatLng _) => _openMenuAtCrosshair();

  Future<void> _openMenuAtCrosshair() async {
    if (_tiles == null) return; // harita henuz acilmadi
    final p = _map.camera.center; // nisangah = haritanin tam ortasi
    // Menuyu nisangahin ekrandaki yerinde ac
    final box = _mapKey.currentContext?.findRenderObject() as RenderBox?;
    final pos = box == null
        ? Offset.zero
        : box.localToGlobal(box.size.center(Offset.zero));
    final act = await showRadialMenu<_Act>(
      context: context,
      position: pos,
      title: fmtLatLng(p),
      items: [
        for (final a in [_Act.poi, _Act.hazard, _Act.rally])
          RadialItem(
              value: a,
              icon: _actKind[a]!.icon,
              label: _actKind[a]!.label,
              color: _actKind[a]!.color),
        for (final a in [_Act.come, _Act.hold, _Act.check])
          RadialItem(
              value: a,
              icon: _actKind[a]!.icon,
              label: _actKind[a]!.label,
              color: _actKind[a]!.color),
        const RadialItem(
            value: _Act.measure,
            icon: Icons.straighten,
            label: 'Mesafe / yön',
            color: Colors.blueGrey),
        const RadialItem(
            value: _Act.testLoc,
            icon: Icons.person_pin_circle,
            label: 'Test konumum',
            color: Colors.purple),
      ],
    );
    if (act == null || !mounted) return;

    switch (act) {
      case _Act.measure:
        setState(() => _measurePt = p);
        if (LocationService.instance.current.value == null) {
          _snack('Mesafe için önce konumun lazım');
        }
        return;
      case _Act.testLoc:
        LocationService.instance.setManual(p);
        setState(() {});
        _snack('Test konumu ayarlandı. Gerçek GPS için sol üstteki ✕');
        return;
      default:
        break;
    }

    final kind = _actKind[act]!;
    int? target; // null = herkes
    if (kind.isCommand) {
      final picked = await _pickTarget(kind);
      if (picked == null || !mounted) return; // vazgecti
      target = picked < 0 ? null : picked;
    }
    widget.onPlace(kind, p, target);
  }

  /// Komutun hedefi: -1 = herkes, >=0 = node adresi, null = vazgecti
  Future<int?> _pickTarget(MarkKind kind) {
    return showDialog<int>(
      context: context,
      builder: (ctx) => SimpleDialog(
        title: Text('"${kind.label}" kime?'),
        children: [
          SimpleDialogOption(
            onPressed: () => Navigator.pop(ctx, -1),
            child: const Row(children: [
              Icon(Icons.groups),
              SizedBox(width: 12),
              Text('Herkes'),
            ]),
          ),
          for (final n in widget.nodes)
            SimpleDialogOption(
              onPressed: () => Navigator.pop(ctx, n.addr),
              child: Row(children: [
                const Icon(Icons.person),
                const SizedBox(width: 12),
                Text(n.name, style: const TextStyle(fontFamily: 'monospace')),
              ]),
            ),
        ],
      ),
    );
  }

  // ---------------- Detay pencereleri ----------------

  String _rangeText(LatLng target) {
    final me = LocationService.instance.current.value;
    if (me == null) return '';
    return '${fmtDistance(distanceM(me.point, target))} · '
        '${compassName(bearingDeg(me.point, target))}';
  }

  void _showNode(NodeRecord n) {
    final target = LatLng(n.lat, n.lon);
    final range = _rangeText(target);
    showModalBottomSheet<void>(
      context: context,
      builder: (ctx) => _sheet(
        ctx,
        icon: Icons.location_on,
        color: widget.stale ? Colors.grey : nodeColor(n.addr),
        title: n.name,
        lines: [
          if (n.name != n.addrHex) 'Adres ${n.addrHex}',
          '${n.latText}, ${n.lonText}',
          'seq ${n.seq} · ${n.hasFix ? 'GPS fix var' : 'GPS fix yok (sahte konum)'}',
          if (widget.stale) 'Tablo güncel değil (30 sn+)',
          if (range.isNotEmpty) 'Uzaklık: $range',
        ],
        actions: [
          FilledButton.icon(
            icon: const Icon(Icons.navigation),
            label: const Text('Bul'),
            onPressed: () {
              Navigator.pop(ctx);
              widget.onFindNode(n);
            },
          ),
        ],
      ),
    );
  }

  void _showMark(MapMark m) {
    final range = _rangeText(m.point);
    showModalBottomSheet<void>(
      context: context,
      builder: (ctx) => _sheet(
        ctx,
        icon: m.kind.icon,
        color: m.kind.color,
        title: m.kind.label,
        lines: [
          fmtLatLng(m.point),
          'Gönderen: ${m.fromText} · ${m.kind.isCommand ? 'Hedef' : 'Paylaşım'}: ${m.targetText} · ${m.timeText}',
          'Durum: ${m.state.text}',
          if (range.isNotEmpty) 'Uzaklık: $range',
        ],
        actions: [
          OutlinedButton.icon(
            icon: const Icon(Icons.delete_outline),
            label: Text(m.state == MarkState.deletePending
                ? 'Onay bekleniyor'
                : 'Sil'),
            onPressed: m.state == MarkState.deletePending
                ? null
                : () {
                    Navigator.pop(ctx);
                    widget.onDelete(m);
                  },
          ),
          FilledButton.icon(
            icon: const Icon(Icons.navigation),
            label: const Text('Bul'),
            onPressed: () {
              Navigator.pop(ctx);
              widget.onFindPoint(m.kind.label, m.point);
            },
          ),
        ],
      ),
    );
  }

  Widget _sheet(BuildContext ctx,
      {required IconData icon,
      required Color color,
      required String title,
      required List<String> lines,
      required List<Widget> actions}) {
    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(20, 12, 20, 12),
        child: Row(
          children: [
            Icon(icon, color: color, size: 40),
            const SizedBox(width: 16),
            Expanded(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(title,
                      style: const TextStyle(
                          fontSize: 18, fontWeight: FontWeight.bold)),
                  for (final l in lines) Text(l),
                ],
              ),
            ),
            for (final a in actions)
              Padding(padding: const EdgeInsets.only(left: 8), child: a),
          ],
        ),
      ),
    );
  }

  void _showMarkList() {
    showModalBottomSheet<void>(
      context: context,
      builder: (ctx) => ValueListenableBuilder<List<MapMark>>(
        valueListenable: MarkStore.instance.marks,
        builder: (ctx, marks, _) => SafeArea(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              ListTile(
                title: Text('İşaretler ve komutlar (${marks.length})'),
                trailing: TextButton(
                  // Sadece bu telefonun ekranini temizler (agdan silmez)
                  onPressed: marks.isEmpty ? null : MarkStore.instance.clear,
                  child: const Text('Ekrandan temizle'),
                ),
              ),
              if (marks.isEmpty)
                const Padding(
                  padding: EdgeInsets.all(16),
                  child: Text('Henüz yok. Haritaya uzun bas.'),
                ),
              Flexible(
                child: ListView(
                  shrinkWrap: true,
                  children: [
                    for (final m in marks.reversed)
                      ListTile(
                        dense: true,
                        leading: Icon(m.kind.icon, color: m.kind.color),
                        title: Text(
                            '${m.fromText}: ${m.kind.label} → ${m.targetText}${m.state.badge}'),
                        subtitle: Text(
                            '${m.timeText} · ${m.state.text} · ${fmtLatLng(m.point)}'),
                        onTap: () {
                          Navigator.pop(ctx);
                          _map.move(m.point, 16);
                        },
                        trailing: IconButton(
                          icon: const Icon(Icons.delete_outline),
                          onPressed: m.state == MarkState.deletePending
                              ? null
                              : () {
                                  Navigator.pop(ctx);
                                  widget.onDelete(m);
                                },
                        ),
                      ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  // ---------------- Harita katmanlari ----------------

  Marker _nodeMarker(NodeRecord n) {
    final color = widget.stale ? Colors.grey : nodeColor(n.addr);
    return Marker(
      point: LatLng(n.lat, n.lon),
      width: 90,
      height: 60,
      alignment: Alignment.topCenter, // ignenin ucu tam konumda
      child: GestureDetector(
        onTap: () => _showNode(n),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            _label(n.name, color),
            Icon(Icons.location_on, color: color, size: 34),
          ],
        ),
      ),
    );
  }

  Marker _markMarker(MapMark m) {
    return Marker(
      point: m.point,
      width: 170,
      height: 58,
      alignment: Alignment.topCenter,
      child: GestureDetector(
        onTap: () => _showMark(m),
        // Silme onayi beklerken soluk gorunur
        child: Opacity(
          opacity: m.state == MarkState.deletePending ? 0.45 : 1,
          child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            _label(
                '${m.mine ? '' : '${m.fromText}: '}'
                '${m.kind.isCommand ? '${m.kind.label} → ${m.targetText}' : m.kind.label}'
                '${m.state.badge}',
                m.kind.color),
            Icon(m.kind.icon, color: m.kind.color, size: 30),
          ],
        ),
        ),
      ),
    );
  }

  Marker _goBackMarker(GoBackTarget g) {
    return Marker(
      point: g.point,
      width: 120,
      height: 58,
      alignment: Alignment.topCenter,
      child: GestureDetector(
        onTap: () => widget.onFindPoint('ADMIN: Bölgeye geri dön', g.point),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            _label('ADMIN: GERİ DÖN', Colors.deepOrange),
            const Icon(Icons.flag_circle, color: Colors.deepOrange, size: 30),
          ],
        ),
      ),
    );
  }

  Widget _label(String text, Color border) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 1),
        decoration: BoxDecoration(
          color: Colors.black.withValues(alpha: 0.75),
          borderRadius: BorderRadius.circular(5),
          border: Border.all(color: border),
        ),
        child: Text(text,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: const TextStyle(
                color: Colors.white, fontSize: 10, fontWeight: FontWeight.w600)),
      );

  Widget _meMarkerChild(MyLocation me) {
    final color = me.manual ? Colors.purple : Colors.lightBlueAccent;
    if (me.headingValid) {
      // Yururken: hareket yonunu gosteren ok
      return Transform.rotate(
        angle: me.heading! * 3.141592653589793 / 180,
        child: Icon(Icons.navigation, color: color, size: 30,
            shadows: const [Shadow(blurRadius: 4)]),
      );
    }
    return Container(
      decoration: BoxDecoration(
        color: color,
        shape: BoxShape.circle,
        border: Border.all(color: Colors.white, width: 3),
        boxShadow: const [BoxShadow(blurRadius: 4, color: Colors.black38)],
      ),
    );
  }

  /// Olcum hedefi: menuden secilen nokta, yoksa R&B aciksa nisangah
  LatLng? _measureTarget() => _measurePt ?? (_rbLive ? _center.value : null);

  // ---------------- Ekran ----------------

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Row(
      children: [
        _toolbar(cs),
        Expanded(child: _mapArea(cs)),
        if (_panelOpen) _nodePanel(cs),
      ],
    );
  }

  Widget _toolbar(ColorScheme cs) {
    Widget btn(IconData icon, String tip, VoidCallback onTap,
            {bool active = false}) =>
        IconButton(
          icon: Icon(icon),
          tooltip: tip,
          color: active ? cs.primary : cs.onSurface,
          style: active
              ? IconButton.styleFrom(backgroundColor: cs.primaryContainer)
              : null,
          onPressed: onTap,
        );

    return Container(
      width: 52,
      color: cs.surfaceContainer,
      child: SingleChildScrollView(
        child: Column(
          children: [
            const SizedBox(height: 4),
            btn(Icons.add_location_alt, 'Nişangaha işaret / komut',
                _openMenuAtCrosshair),
            btn(Icons.my_location, 'Konumuma git', _goToMe),
            btn(Icons.center_focus_strong, 'Node\'ları göster', _fitAll),
            btn(Icons.straighten, 'R&B: benden nişangaha', () {
              setState(() {
                _rbLive = !_rbLive;
                if (!_rbLive) _measurePt = null;
              });
            }, active: _rbLive),
            btn(Icons.layers, 'İşaret listesi', _showMarkList),
            btn(Icons.view_sidebar, 'Node paneli',
                () => setState(() => _panelOpen = !_panelOpen),
                active: _panelOpen),
          ],
        ),
      ),
    );
  }

  Widget _mapArea(ColorScheme cs) {
    if (_error != null) {
      return Center(child: Text(_error!, textAlign: TextAlign.center));
    }
    final tiles = _tiles;
    if (tiles == null) {
      return const Center(child: CircularProgressIndicator());
    }
    final measureListen =
        Listenable.merge([_center, LocationService.instance.current]);

    return Stack(
      key: _mapKey,
      children: [
        FlutterMap(
          mapController: _map,
          options: MapOptions(
            initialCenter: kMapCenter,
            initialZoom: 14,
            minZoom: kMinZoom,
            maxZoom: kMaxZoom,
            // Merkez Bakirkoy kutusunun disina cikamaz (harita orada bitiyor)
            cameraConstraint: CameraConstraint.containCenter(bounds: kMapBounds),
            interactionOptions: const InteractionOptions(
              flags: InteractiveFlag.all & ~InteractiveFlag.rotate,
            ),
            onLongPress: _onLongPress,
            onPositionChanged: (camera, hasGesture) {
              // build sirasinda gelebilir; bir sonraki kareye ertele
              final c = camera.center;
              WidgetsBinding.instance.addPostFrameCallback((_) {
                if (mounted) _center.value = c;
              });
            },
          ),
          children: [
            TileLayer(
              tileProvider: tiles,
              maxNativeZoom: kMaxNativeZoom.toInt(),
            ),
            // Olcum cizgisi (benden hedefe, kesikli)
            AnimatedBuilder(
              animation: measureListen,
              builder: (context, _) {
                final me = LocationService.instance.current.value;
                final t = _measureTarget();
                if (me == null || t == null) return const SizedBox.shrink();
                return PolylineLayer(polylines: [
                  Polyline(
                    points: [me.point, t],
                    color: Colors.yellowAccent,
                    strokeWidth: 3,
                    pattern: StrokePattern.dashed(segments: const [12, 8]),
                  ),
                ]);
              },
            ),
            // Dogruluk dairesi
            ValueListenableBuilder<MyLocation?>(
              valueListenable: LocationService.instance.current,
              builder: (context, me, _) {
                if (me == null || me.accuracy <= 0) {
                  return const SizedBox.shrink();
                }
                return CircleLayer(circles: [
                  CircleMarker(
                    point: me.point,
                    radius: me.accuracy,
                    useRadiusInMeter: true,
                    color: Colors.lightBlue.withValues(alpha: 0.15),
                    borderColor: Colors.lightBlue.withValues(alpha: 0.5),
                    borderStrokeWidth: 1,
                  ),
                ]);
              },
            ),
            // Isaretler ve komutlar
            ValueListenableBuilder<List<MapMark>>(
              valueListenable: MarkStore.instance.marks,
              builder: (context, marks, _) =>
                  MarkerLayer(markers: marks.map(_markMarker).toList()),
            ),
            // Admin: bolgeye geri don hedefi + benden hedefe cizgi
            AnimatedBuilder(
              animation: Listenable.merge(
                  [MarkStore.instance.goBack, LocationService.instance.current]),
              builder: (context, _) {
                final g = MarkStore.instance.goBack.value;
                final me = LocationService.instance.current.value;
                if (g == null || me == null) return const SizedBox.shrink();
                return PolylineLayer(polylines: [
                  Polyline(
                    points: [me.point, g.point],
                    color: Colors.deepOrange,
                    strokeWidth: 4,
                    pattern: StrokePattern.dashed(segments: const [14, 8]),
                  ),
                ]);
              },
            ),
            ValueListenableBuilder<GoBackTarget?>(
              valueListenable: MarkStore.instance.goBack,
              builder: (context, g, _) => g == null
                  ? const SizedBox.shrink()
                  : MarkerLayer(markers: [_goBackMarker(g)]),
            ),
            // Node'lar
            MarkerLayer(markers: widget.nodes.map(_nodeMarker).toList()),
            // Ben
            ValueListenableBuilder<MyLocation?>(
              valueListenable: LocationService.instance.current,
              builder: (context, me, _) {
                if (me == null) return const SizedBox.shrink();
                return MarkerLayer(markers: [
                  Marker(
                      point: me.point,
                      width: 30,
                      height: 30,
                      child: _meMarkerChild(me)),
                ]);
              },
            ),
            Scalebar(alignment: Alignment.bottomLeft),
            const SimpleAttributionWidget(
              source: Text('© OpenStreetMap contributors'),
            ),
          ],
        ),

        // Nisangah (haritanin tam ortasi)
        const IgnorePointer(
          child: Center(
            child: Icon(Icons.add, size: 30, color: Colors.black87),
          ),
        ),

        // Sol ust: test konumu varsa kaldirma dugmesi
        if (LocationService.instance.hasManual)
          Positioned(
            left: 8,
            top: 8,
            child: ActionChip(
              avatar: const Icon(Icons.close, size: 18),
              label: const Text('Test konumu'),
              onPressed: () {
                LocationService.instance.clearManual();
                setState(() {});
              },
            ),
          ),

        // Ust orta: admin "geri don" uyarisi (dokun = Bul, ✕ = kaldir)
        Positioned(
          left: 0,
          right: 0,
          top: 8,
          child: Center(
            child: AnimatedBuilder(
              animation: Listenable.merge(
                  [MarkStore.instance.goBack, LocationService.instance.current]),
              builder: (context, _) {
                final g = MarkStore.instance.goBack.value;
                if (g == null) return const SizedBox.shrink();
                final range = _rangeText(g.point);
                return InputChip(
                  backgroundColor: Colors.deepOrange,
                  avatar: const Icon(Icons.u_turn_left, color: Colors.white),
                  label: Text(
                      'ADMIN: GERİ DÖN${range.isEmpty ? '' : ' · $range'}',
                      style: const TextStyle(
                          color: Colors.white, fontWeight: FontWeight.bold)),
                  deleteIconColor: Colors.white,
                  onDeleted: MarkStore.instance.clearGoBack,
                  onPressed: () =>
                      widget.onFindPoint('ADMIN: Bölgeye geri dön', g.point),
                );
              },
            ),
          ),
        ),

        // Alt orta: nisangah koordinati + olcum sonucu
        Positioned(
          left: 0,
          right: 0,
          bottom: 22,
          child: Center(
            child: AnimatedBuilder(
              animation: measureListen,
              builder: (context, _) {
                final me = LocationService.instance.current.value;
                final t = _measureTarget();
                var text = '⌖ ${fmtLatLng(_center.value)}';
                if (t != null && me != null) {
                  final d = distanceM(me.point, t);
                  final b = bearingDeg(me.point, t);
                  text += '   R&B ${fmtDistance(d)} · ${b.round()}° ${compassName(b)}';
                }
                return Container(
                  padding:
                      const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                  decoration: BoxDecoration(
                    color: Colors.black.withValues(alpha: 0.7),
                    borderRadius: BorderRadius.circular(6),
                  ),
                  child: Text(text,
                      style: const TextStyle(
                          color: Colors.white,
                          fontSize: 12,
                          fontFamily: 'monospace')),
                );
              },
            ),
          ),
        ),
      ],
    );
  }

  Widget _nodePanel(ColorScheme cs) {
    return Container(
      width: 200,
      color: cs.surfaceContainer,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 8, 12, 4),
            child: Text('NODE\'LAR (${widget.nodes.length})',
                style: TextStyle(
                    fontWeight: FontWeight.bold,
                    letterSpacing: 1,
                    color: widget.stale ? Colors.grey : cs.primary)),
          ),
          const Divider(height: 1),
          Expanded(
            child: widget.nodes.isEmpty
                ? const Center(child: Text('Tablo bekleniyor'))
                : ListView(
                    children: [
                      for (final n in widget.nodes)
                        ListTile(
                          dense: true,
                          contentPadding:
                              const EdgeInsets.only(left: 12, right: 0),
                          leading: Icon(Icons.location_on,
                              color: widget.stale ? Colors.grey : nodeColor(n.addr)),
                          minLeadingWidth: 20,
                          title: Text(n.name,
                              style: const TextStyle(
                                  fontFamily: 'monospace',
                                  fontWeight: FontWeight.w600)),
                          subtitle: Text(
                            _rangeText(LatLng(n.lat, n.lon)).isEmpty
                                ? 'seq ${n.seq}'
                                : _rangeText(LatLng(n.lat, n.lon)),
                          ),
                          onTap: () => _map.move(LatLng(n.lat, n.lon), 16),
                          trailing: IconButton(
                            icon: const Icon(Icons.navigation, size: 20),
                            tooltip: 'Bul',
                            onPressed: () => widget.onFindNode(n),
                          ),
                        ),
                    ],
                  ),
          ),
        ],
      ),
    );
  }
}
