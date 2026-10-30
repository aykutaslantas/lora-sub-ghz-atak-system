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
#define GW_FORWARD_CMD                              0
#endif

#endif /* __APP_MODE_H__ */
