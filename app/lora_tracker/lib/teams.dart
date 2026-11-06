// Takimlar (JOIN v1): admin'in verdigi adres 0x8000 | (takim << 8) | numara
//   ALPHA-2 = 0x8102. bit15 = 0 olan adresler eski UID adresleri (KG200Z gibi
//   telefonsuz node'lar), onlar hex gosterilir ve kirmizi kalir.
//   0xFF00 | takim = ayrilmis (takim yayini icin), 0xFFFE admin, 0xFFFF herkes.

import 'package:flutter/material.dart';

const int addrJoinedFlag = 0x8000;
const int teamMax = 26;
const int teamNumMax = 99;

const List<String> kTeamNames = [
  'ALPHA', 'BRAVO', 'CHARLIE', 'DELTA', 'ECHO', 'FOXTROT', 'GOLF', 'HOTEL',
  'INDIA', 'JULIETT', 'KILO', 'LIMA', 'MIKE', 'NOVEMBER', 'OSCAR', 'PAPA',
  'QUEBEC', 'ROMEO', 'SIERRA', 'TANGO', 'UNIFORM', 'VICTOR', 'WHISKEY',
  'XRAY', 'YANKEE', 'ZULU',
];

// ATAK'taki takim renklerine benzer. Kirmizi bilerek yok: kirmizi = takimsiz
// (UID adresli) node. 26 takim icin palet basa sarar.
const List<Color> kTeamColors = [
  Color(0xFF00FFFF), // cyan
  Color(0xFFFFFF00), // yellow
  Color(0xFF00E676), // green
  Color(0xFFFF00FF), // magenta
  Color(0xFFFF9800), // orange
  Color(0xFF448AFF), // blue
  Color(0xFFFFFFFF), // white
  Color(0xFF1DE9B6), // teal
  Color(0xFFB388FF), // purple
  Color(0xFFA1887F), // brown
  Color(0xFFFF80AB), // pink
  Color(0xFFC6FF00), // lime
  Color(0xFF8C9EFF), // indigo
];

int teamOfAddr(int a) => (a >> 8) & 0x7F;
int numOfAddr(int a) => a & 0xFF;

/// Admin'in verdigi takimli adres mi (ALPHA-2 gibi)
bool isTeamAddr(int a) =>
    (a & addrJoinedFlag) != 0 &&
    a < 0xFF00 &&
    teamOfAddr(a) >= 1 &&
    teamOfAddr(a) <= teamMax &&
    numOfAddr(a) <= teamNumMax;

String teamName(int t) =>
    (t >= 1 && t <= teamMax) ? kTeamNames[t - 1] : 'TAKIM $t';

String hexAddr(int a) =>
    '0x${a.toRadixString(16).toUpperCase().padLeft(4, '0')}';

/// Ekranda gosterilecek isim: ALPHA-2, ADMIN, ya da 0x157C
String nodeName(int a) {
  if (a == 0xFFFE) return 'ADMIN';
  if (a == 0xFFFF) return 'Herkes';
  if (isTeamAddr(a)) return '${teamName(teamOfAddr(a))}-${numOfAddr(a)}';
  return hexAddr(a);
}

Color teamColor(int t) =>
    t >= 1 ? kTeamColors[(t - 1) % kTeamColors.length] : Colors.red;

/// Haritadaki isaretci rengi: takim rengi, takimsiz node kirmizi
Color nodeColor(int a) => isTeamAddr(a) ? teamColor(teamOfAddr(a)) : Colors.red;
