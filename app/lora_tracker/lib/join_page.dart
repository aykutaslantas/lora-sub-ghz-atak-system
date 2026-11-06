// Takima katilma ekrani (JOIN v1).
// Telefon takim / numara / PIN'i bagli node'a yollar (0xC9); node admin'den
// adres ister (0xA1), admin onaylayinca ALPHA-2 gibi bir kimlikle calisir.
// Durum node'dan 0xCB ile gelir ve bu ekranda canli gosterilir.

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'cmd_packet.dart';
import 'teams.dart';

class JoinPage extends StatefulWidget {
  final ValueNotifier<JoinStatus?> status;
  final Future<bool> Function(int team, int num, int pin) onJoin;
  final Future<bool> Function() onLeave;

  const JoinPage({
    super.key,
    required this.status,
    required this.onJoin,
    required this.onLeave,
  });

  @override
  State<JoinPage> createState() => _JoinPageState();
}

class _JoinPageState extends State<JoinPage> {
  int _team = 1;
  final _num = TextEditingController(text: '0');
  final _pin = TextEditingController();
  bool _hidePin = true;
  int _lastSuggestSeq = -1;

  @override
  void initState() {
    super.initState();
    final s = widget.status.value;
    if (s != null && s.team >= 1 && s.team <= teamMax) {
      _team = s.team;
      _num.text = '${s.num}';
    }
    widget.status.addListener(_onStatus);
  }

  @override
  void dispose() {
    widget.status.removeListener(_onStatus);
    _num.dispose();
    _pin.dispose();
    super.dispose();
  }

  // "Numara dolu" gelince admin'in onerdigi numarayi kutuya yaz
  void _onStatus() {
    final s = widget.status.value;
    if (s != null &&
        s.state == JoinState.failed &&
        s.result == 4 &&
        s.reqSeq != _lastSuggestSeq) {
      _lastSuggestSeq = s.reqSeq;
      _num.text = '${s.num}';
    }
    if (mounted) setState(() {});
  }

  void _snack(String msg) {
    ScaffoldMessenger.of(context)
      ..hideCurrentSnackBar()
      ..showSnackBar(SnackBar(content: Text(msg)));
  }

  Future<void> _join() async {
    final n = int.tryParse(_num.text.trim());
    final p = int.tryParse(_pin.text.trim());
    if (n == null || n < 0 || n > teamNumMax) {
      _snack('Numara 0-99 arası olmalı (0 = admin seçsin)');
      return;
    }
    if (p == null || p < 0 || p > 9999) {
      _snack('PIN 0-9999 arası bir sayı olmalı');
      return;
    }
    final ok = await widget.onJoin(_team, n, p);
    if (!mounted) return;
    _snack(ok
        ? 'Gönderildi: ${teamName(_team)}${n == 0 ? '' : '-$n'}, admin onayı bekleniyor'
        : 'BLE bağlı değil, gönderilemedi');
  }

  Future<void> _leave() async {
    final yes = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Takımdan ayrıl?'),
        content: const Text(
            'Admin\'e bildirilir, node kimliğini siler ve konum yayınını durdurur. '
            'Tekrar çalışması için yeniden katılıp admin onayı alması gerekir.'),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(ctx, false),
              child: const Text('Vazgeç')),
          FilledButton(
              style: FilledButton.styleFrom(backgroundColor: Colors.red),
              onPressed: () => Navigator.pop(ctx, true),
              child: const Text('Ayrıl')),
        ],
      ),
    );
    if (yes != true) return;
    final ok = await widget.onLeave();
    if (mounted) {
      _snack(ok ? 'Ayrılma isteği admin\'e gönderiliyor' : 'BLE bağlı değil, gönderilemedi');
    }
  }

  Widget _statusCard(JoinStatus? s) {
    late final IconData icon;
    late final Color color;
    late final String title;
    String sub = '';
    if (s == null) {
      icon = Icons.help_outline;
      color = Colors.grey;
      title = 'Node durumu bilinmiyor';
      sub = 'BLE bağlantısı ve node bekleniyor (en geç ~10 sn)';
    } else {
      switch (s.state) {
        case JoinState.disabled:
          icon = Icons.block;
          color = Colors.grey;
          title = 'Bu node takım sistemi kullanmıyor';
          sub = 'Adres: ${hexAddr(s.addr)} (JOIN_ENABLED=0)';
        case JoinState.needParams:
          icon = s.result == 5 ? Icons.person_remove : Icons.person_add;
          color = s.result == 5 ? Colors.redAccent : Colors.orange;
          title = s.result == 5 ? 'Admin seni takımdan çıkardı' : 'Kimlik yok: takıma katıl';
          sub = s.result == 6
              ? 'Takımdan ayrıldın. Onay gelene kadar node konum yayınlamaz'
              : 'Onay gelene kadar node konum yayınlamaz';
        case JoinState.waiting:
          icon = Icons.hourglass_top;
          color = Colors.amber;
          title = 'Admin onayı bekleniyor';
          sub = '${teamName(s.team)}${s.num == 0 ? '' : '-${s.num}'} · '
              'istek #${s.reqSeq} (10-15 sn\'de bir tekrar)';
        case JoinState.joined:
          icon = Icons.verified;
          color = teamColor(s.team);
          title = nodeName(s.addr);
          sub = 'Adres ${hexAddr(s.addr)} · node konum yayınlıyor';
        case JoinState.leaving:
          icon = Icons.logout;
          color = Colors.amber;
          title = 'Takımdan ayrılınıyor...';
          sub = '${nodeName(s.addr)} · admin onayı bekleniyor (en fazla ~15 sn)';
        case JoinState.failed:
          icon = Icons.error;
          color = Colors.redAccent;
          title = 'Katılma reddedildi';
          sub = s.resultText;
      }
    }
    return Card(
      child: ListTile(
        leading: Icon(icon, color: color, size: 36),
        title: Text(title,
            style: TextStyle(color: color, fontWeight: FontWeight.bold)),
        subtitle: sub.isEmpty ? null : Text(sub),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final s = widget.status.value;
    final joined = s?.state == JoinState.joined;
    final leaving = s?.state == JoinState.leaving;
    final disabled = s?.state == JoinState.disabled;
    return Scaffold(
      appBar: AppBar(title: const Text('Takım / Kimlik')),
      body: SafeArea(
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            _statusCard(s),
            const SizedBox(height: 12),
            if (!joined && !disabled && !leaving) ...[
              Row(children: [
                Expanded(
                  flex: 3,
                  child: DropdownButtonFormField<int>(
                    initialValue: _team,
                    decoration: const InputDecoration(
                        labelText: 'Takım', border: OutlineInputBorder()),
                    items: [
                      for (var t = 1; t <= teamMax; t++)
                        DropdownMenuItem(
                          value: t,
                          child: Row(children: [
                            Icon(Icons.circle, size: 14, color: teamColor(t)),
                            const SizedBox(width: 8),
                            Text(teamName(t)),
                          ]),
                        ),
                    ],
                    onChanged: (v) => setState(() => _team = v ?? 1),
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  flex: 2,
                  child: TextField(
                    controller: _num,
                    keyboardType: TextInputType.number,
                    inputFormatters: [
                      FilteringTextInputFormatter.digitsOnly,
                      LengthLimitingTextInputFormatter(2),
                    ],
                    decoration: const InputDecoration(
                        labelText: 'Numara',
                        helperText: '0 = admin seçsin',
                        border: OutlineInputBorder()),
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  flex: 2,
                  child: TextField(
                    controller: _pin,
                    keyboardType: TextInputType.number,
                    obscureText: _hidePin,
                    inputFormatters: [
                      FilteringTextInputFormatter.digitsOnly,
                      LengthLimitingTextInputFormatter(4),
                    ],
                    decoration: InputDecoration(
                      labelText: 'PIN',
                      border: const OutlineInputBorder(),
                      suffixIcon: IconButton(
                        icon: Icon(
                            _hidePin ? Icons.visibility : Icons.visibility_off),
                        onPressed: () => setState(() => _hidePin = !_hidePin),
                      ),
                    ),
                  ),
                ),
              ]),
              const SizedBox(height: 16),
              FilledButton.icon(
                icon: const Icon(Icons.send),
                label: Text(s?.state == JoinState.waiting
                    ? 'Tekrar gönder'
                    : 'Katıl'),
                onPressed: _join,
              ),
            ],
            if (joined) ...[
              const SizedBox(height: 8),
              OutlinedButton.icon(
                icon: const Icon(Icons.logout, color: Colors.red),
                label: const Text('Takımdan ayrıl',
                    style: TextStyle(color: Colors.red)),
                onPressed: _leave,
              ),
            ],
          ],
        ),
      ),
    );
  }
}
