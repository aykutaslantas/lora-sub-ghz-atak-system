// "Bul" ekrani (yatay): solda buyuk ok, sagda mesafe ve bilgiler.
// Hedef ya bir node (canli guncellenir) ya da haritadaki sabit bir nokta.
//
// Ok neye gore?
//  - Yururken: GPS'in hareket yonune gore (ok yukari = tam onunde).
//  - Dururken: kuzeye gore (telefonun ust kenarini kuzeye cevir).

import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:latlong2/latlong.dart';

import 'location_service.dart';
import 'table_parser.dart';

class FindPage extends StatelessWidget {
  final String title;
  final int? addr; // node hedefi
  final ValueNotifier<List<NodeRecord>>? nodes;
  final LatLng? point; // sabit nokta hedefi

  const FindPage.node(
      {super.key, required int this.addr, required ValueNotifier<List<NodeRecord>> this.nodes})
      : title = '',
        point = null;

  const FindPage.point({super.key, required this.title, required LatLng this.point})
      : addr = null,
        nodes = null;

  @override
  Widget build(BuildContext context) {
    final loc = LocationService.instance;
    final listen = <Listenable>[loc.current, loc.status];
    if (nodes != null) listen.add(nodes!);

    return Scaffold(
      appBar: AppBar(title: const Text('Bul'), toolbarHeight: 44),
      body: AnimatedBuilder(
        animation: Listenable.merge(listen),
        builder: (context, _) {
          // Hedefi coz
          LatLng? target = point;
          String name = title;
          final extra = <String>[];
          if (addr != null) {
            NodeRecord? n;
            for (final r in nodes!.value) {
              if (r.addr == addr) n = r;
            }
            if (n == null) {
              return const Center(child: Text('Bu node artık tabloda yok'));
            }
            target = LatLng(n.lat, n.lon);
            name = n.name;
            extra.add(n.hasFix ? 'Node konumu: GPS' : 'Node konumu: sahte (fix yok)');
            extra.add('seq ${n.seq}');
          }

          final me = loc.current.value;
          if (me == null || target == null) {
            return Center(
              child: Text(
                '${loc.status.value}\nKonum gelince ok görünecek.',
                textAlign: TextAlign.center,
              ),
            );
          }

          final d = distanceM(me.point, target);
          final brg = bearingDeg(me.point, target);
          final ref = me.headingValid ? me.heading! : 0.0;
          final rel = (brg - ref + 360) % 360;
          final veryClose = me.accuracy > 0 && d <= me.accuracy;
          final grey = TextStyle(color: Colors.grey.shade500);

          return Row(
            children: [
              Expanded(
                child: Center(
                  child: veryClose
                      ? const Icon(Icons.adjust, size: 180, color: Colors.green)
                      : Transform.rotate(
                          angle: rel * math.pi / 180,
                          child: Icon(Icons.navigation,
                              size: 200,
                              color: Theme.of(context).colorScheme.primary),
                        ),
                ),
              ),
              Expanded(
                child: Padding(
                  padding: const EdgeInsets.all(16),
                  child: Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(name,
                          style: const TextStyle(
                              fontSize: 22,
                              fontWeight: FontWeight.bold,
                              fontFamily: 'monospace')),
                      Text(fmtDistance(d),
                          style: const TextStyle(
                              fontSize: 48, fontWeight: FontWeight.bold)),
                      Text('${compassName(brg)} (${brg.round()}°)',
                          style: const TextStyle(fontSize: 18)),
                      const SizedBox(height: 8),
                      if (veryClose)
                        const Text('Çok yakın: konum hatası içindesin',
                            style: TextStyle(color: Colors.green)),
                      Text(
                        me.headingValid
                            ? 'Ok: yürüdüğün yöne göre'
                            : 'Ok: kuzeye göre (telefonun üstü kuzeye)',
                        style: grey,
                      ),
                      Text(
                        me.manual
                            ? 'Senin konumun: TEST'
                            : 'Senin konumun: GPS ±${me.accuracy.round()} m',
                        style: grey,
                      ),
                      for (final e in extra) Text(e, style: grey),
                    ],
                  ),
                ),
              ),
            ],
          );
        },
      ),
    );
  }
}
