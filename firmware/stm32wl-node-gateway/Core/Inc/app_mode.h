/**
  ******************************************************************************
  * @file    app_mode.h
  * @brief   Kartin calisma modu. TEK YER: node / gateway secimi buradan yapilir.
  *          Bu dosya CubeMX tarafindan uretilmez, Generate Code'da silinmez.
  ******************************************************************************
  */
#ifndef __APP_MODE_H__
#define __APP_MODE_H__

/* 0 = NODE    : konum yollar + dinler, STOP2 uyku modu acik (pil dostu)
   1 = GATEWAY : sadece dinler, paketleri USART1 (D1/PB6) ile Zybo'ya iletir,
                 STOP2 kapali (bkz. sys_conf.h)                              */
#ifndef GATEWAY_MODE
#define GATEWAY_MODE                                0
#endif

/* Sanal node sayisi (sadece NODE modunda): tek fiziksel kart, bu kadar ayri node gibi
   konum yollar. Adresler: kartin adresi, +1, +2, ... Her birinin seq'i, sahte konumu ve
   jitter'li TX zamanlayicisi ayridir.
   1 = normal tek node. En fazla 8.
   NOT: Zybo whitelist'ine yeni adresler eklenmezse filtre onlari duşurur. */
#ifndef VIRTUAL_NODES
#define VIRTUAL_NODES                               1
#endif

/* GATEWAY_LOCAL_TABLE (sadece GATEWAY modunda):
   1 = Zybo yokken test: gateway "kim nerede" tablosunu KENDISI tutar ve
       DL_LOCAL_PERIOD_MS'de bir downlink olarak yayinlar (whitelist yok).
   0 = normal: tabloyu Zybo tutar, gateway USART1 RX'ten gelen tabloyu yayinlar. */
#ifndef GATEWAY_LOCAL_TABLE
#define GATEWAY_LOCAL_TABLE                         0
#endif
#define DL_LOCAL_PERIOD_MS                          10000

/* NODE_UART_BRIDGE (sadece NODE modunda):
   1 = node, aldigi tabloyu USART1 TX (D1/PB6) ile ESP32'ye (BLE -> telefon) yollar.
       USART1 STOP2'de calismadigi icin node'da da STOP2 kapanir (bkz. sys_conf.h).
   0 = kapali, node STOP2 ile pil dostu calisir. */
#ifndef NODE_UART_BRIDGE
#define NODE_UART_BRIDGE                            1
#endif

/* GW_FORWARD_CMD (sadece GATEWAY modunda):
   1 = gateway, duydugu her YENI komutu (0xC1) Zybo'ya da iletir:
       [AA 55 11][15 B komut][RSSI][SNR][CRC8] (21 byte). Zybo PL komut FIFO'su hazir olunca ac.
   0 = kapali: komutlar sadece havada tekrar yayinlanir (Zybo'nun haberi olmaz). */
#ifndef GW_FORWARD_CMD
#define GW_FORWARD_CMD                              1
#endif

/* GW_FORWARD_RELAY_INFO (sadece GATEWAY modunda):
   1 = komsu uzerinden aktarilmis konumu (0xE1) Zybo'ya aktaran bilgisiyle yolla:
       [AA 55 13][0xE1][aktaran u16][aktaran RSSI][aktaran SNR][12 B konum][RSSI][SNR][CRC8] (23 byte)
       Zybo PL bu cerceveyi taniyinca ac.
   0 = eski yol: icindeki 12 byte normal 18 byte'lik konum cercevesiyle gider (Zybo aktarani bilmez). */
#ifndef GW_FORWARD_RELAY_INFO
#define GW_FORWARD_RELAY_INFO                       1
#endif

/* JOIN_ENABLED (sadece NODE modunda; NODE_UART_BRIDGE=1 ve VIRTUAL_NODES=1 olmali):
   1 = takimli kimlik: flash'ta kimlik yoksa node konum YOLLAMAZ, telefondan takim / numara /
       PIN gelince 0xA1 ile admin'den adres ister. Admin onaylayinca adresi (ALPHA-2 = 0x8102)
       flash'a yazar ve normal calisir. Telefondan "takimdan ayril" ile katilma moduna doner.
   0 = eski yol: adres kartin UID'sinden (bit15 = 0). Telefonu olmayan node'lar boyle kalir. */
#ifndef JOIN_ENABLED
#define JOIN_ENABLED                                1
#endif

/* SECURE_ENABLED (Faz 1 guvenlik, AES-128-CCM, net_key.h'deki ortak anahtar):
   0 = kapali: bugunku duz paketler (degisiklik yok).
   1 = NODE: havaya cikan her paket guvenli zarfla gider
           [0x51][dev_id 4][sayac 4][sifreli ic paket][MIC 4] (+13 byte).
       GATEWAY: duz boru. Guvenli paketleri acmadan Zybo'ya verir (AA 55 LEN [paket] RSSI SNR CRC),
           Zybo'dan gelen AA 55 LEN [tx_repeat][guvenli paket] CRC cercevelerini tx_repeat kez yayinlar.
   SECURE_ONLY (sadece SECURE_ENABLED=1 iken):
   0 = gecis: duz ve guvenli paketlerin ikisi de kabul edilir.
   1 = sadece guvenli paketler kabul edilir, duz olanlar atilir. */
#ifndef SECURE_ENABLED
#define SECURE_ENABLED                              1
#endif
#ifndef SECURE_ONLY
#define SECURE_ONLY                                 1
#endif

/* ====================== MESH MASA TESTI (GECICI) ======================
   !!! Gercek menzil testinden ONCE ikisi de 0 yapilacak / bu blok kaldirilacak !!!
   TEST_FAR_NODE (sadece NODE modunda):
     1 = gateway'den gelen her seyi (0xD1 tablo, 0xC1/0xC3) duymamis gibi davran.
         Tablo goremedigi icin REACH_TIMEOUT_MS (25 sn) sonra konum paketlerine
         "beni aktarin" bayragi (flags bit1) koyar. Uzak node'a yuklenir (or. KG200Z). */
#ifndef TEST_FAR_NODE
#define TEST_FAR_NODE                               0
#endif
/* TEST_GW_IGNORE_DIRECT (sadece GATEWAY modunda):
     bu adresten DOGRUDAN gelen konum ve kontrol (0xC1/0xC2/0xC5) paketlerini at,
     sadece komsu uzerinden aktarilmis (0xE1/0xE2) olanlari kabul et.
     0 = kapali. Ornek: 0x0F53 */
#ifndef TEST_GW_IGNORE_DIRECT
#define TEST_GW_IGNORE_DIRECT                       0
#endif

#endif /* __APP_MODE_H__ */
