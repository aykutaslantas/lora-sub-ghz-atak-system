// ATAK tarzi halka menu: dokunulan noktanin etrafinda daire seklinde butonlar.

import 'dart:math' as math;

import 'package:flutter/material.dart';

class RadialItem<T> {
  final T value;
  final IconData icon;
  final String label;
  final Color color;

  const RadialItem({
    required this.value,
    required this.icon,
    required this.label,
    required this.color,
  });
}

/// [position]: ekrandaki (global) dokunma noktasi. Secilen ogenin value'su,
/// disari dokunulursa null doner.
Future<T?> showRadialMenu<T>({
  required BuildContext context,
  required Offset position,
  required List<RadialItem<T>> items,
  String? title,
}) {
  return showGeneralDialog<T>(
    context: context,
    barrierDismissible: true,
    barrierLabel: 'Kapat',
    barrierColor: Colors.black38,
    transitionDuration: const Duration(milliseconds: 150),
    pageBuilder: (ctx, a1, a2) =>
        _RadialMenu<T>(position: position, items: items, title: title),
    transitionBuilder: (ctx, anim, a2, child) => ScaleTransition(
      scale: CurvedAnimation(parent: anim, curve: Curves.easeOutBack),
      child: child,
    ),
  );
}

class _RadialMenu<T> extends StatelessWidget {
  final Offset position;
  final List<RadialItem<T>> items;
  final String? title;

  const _RadialMenu({required this.position, required this.items, this.title});

  static const double radius = 105;
  static const double itemW = 86;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(builder: (context, box) {
      // Menu ekrandan tasmasin diye merkezi kenarlardan iceri al
      const margin = radius + 40;
      final cx = position.dx.clamp(margin, math.max(margin, box.maxWidth - margin));
      final cy =
          position.dy.clamp(margin, math.max(margin, box.maxHeight - margin));

      final children = <Widget>[
        // Bos alana dokununca kapat
        Positioned.fill(
          child: GestureDetector(onTap: () => Navigator.pop(context)),
        ),
      ];

      for (var i = 0; i < items.length; i++) {
        final it = items[i];
        final ang = -math.pi / 2 + i * 2 * math.pi / items.length;
        final x = cx + radius * math.cos(ang);
        final y = cy + radius * math.sin(ang);
        children.add(Positioned(
          left: x - itemW / 2,
          top: y - 24,
          width: itemW,
          child: GestureDetector(
            onTap: () => Navigator.pop(context, it.value),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                CircleAvatar(
                  radius: 22,
                  backgroundColor: it.color,
                  child: Icon(it.icon, color: Colors.white, size: 22),
                ),
                const SizedBox(height: 2),
                Container(
                  padding:
                      const EdgeInsets.symmetric(horizontal: 4, vertical: 1),
                  decoration: BoxDecoration(
                    color: Colors.black87,
                    borderRadius: BorderRadius.circular(4),
                  ),
                  child: Text(
                    it.label,
                    textAlign: TextAlign.center,
                    style: const TextStyle(color: Colors.white, fontSize: 10),
                  ),
                ),
              ],
            ),
          ),
        ));
      }

      // Ortada: nokta + kapat
      children.add(Positioned(
        left: cx - 22,
        top: cy - 22,
        child: GestureDetector(
          onTap: () => Navigator.pop(context),
          child: const CircleAvatar(
            radius: 22,
            backgroundColor: Colors.black87,
            child: Icon(Icons.close, color: Colors.white),
          ),
        ),
      ));
      if (title != null) {
        children.add(Positioned(
          left: cx - 90,
          top: cy + 26,
          width: 180,
          child: Text(
            title!,
            textAlign: TextAlign.center,
            style: const TextStyle(
                color: Colors.white,
                fontSize: 10,
                fontFamily: 'monospace',
                shadows: [Shadow(blurRadius: 3)]),
          ),
        ));
      }

      return Material(type: MaterialType.transparency, child: Stack(children: children));
    });
  }
}
