/* V1.17 aktarici secimi testleri (guvenli mod). ROLE: 0 = aktarici (komsu), 1 = uzak node */
#include "test_secure_prelude.h"
/* ---- helpers ---- */
static void advance(uint32_t ms){ uint32_t end=g_now+ms; while(g_now<end){ g_now++; for(int i=0;i<nT;i++) if(T[i].running && g_now>=T[i].due){ T[i].running=0; T[i].cb(T[i].ctx);} } }
static void rxs(const uint8_t*p,int n,int16_t rssi,int8_t snr){ memcpy(BufferRx,p,n); RxBufferSize=n; RssiValue=rssi; SnrValue=snr; HandleRxPacket(); }
static void txdone(void){ txBusy=false; }
static void pump(void){ Node_Process(); }
static void mkpos(uint8_t*b,uint16_t addr,uint8_t seq,uint8_t flags){ b[0]=addr&0xFF;b[1]=addr>>8;b[2]=seq; memset(b+3,0x11,8); b[11]=flags; }
static void mkcmd(uint8_t*b,uint8_t type,uint16_t src,uint16_t dst,uint8_t id,uint8_t kind){ memset(b,0,15); b[0]=type;b[1]=src&0xFF;b[2]=src>>8;b[3]=dst&0xFF;b[4]=dst>>8;b[5]=id;b[6]=kind; }
static void hexdump(const char*name,const uint8_t*p,int n){ printf("FRAME %s ",name); for(int i=0;i<n;i++) printf("%02x",p[i]); printf("\n"); }
#define CHECK(c,msg) do{ if(c) printf("  OK   %s\n",msg); else { printf("  FAIL %s\n",msg); fails++; } }while(0)
/* Test tarafinin sifreleyicisi: v2=0 -> 0x51, v2=1 -> 0x52 (relay_dev acik baslikta, AAD 13 B) */
static int ext_wrapv(int v2, uint32_t dev, uint32_t ctr, uint32_t rdev, const uint8_t *in, int len, uint8_t *out){
  uint8_t nonce[13]; int hl=v2?13:9; out[0]=v2?0x52:0x51; memcpy(out+1,&dev,4); memcpy(out+5,&ctr,4); if(v2) memcpy(out+9,&rdev,4);
  memcpy(nonce,out+1,8); nonce[8]=(dev==0xFFFFFFFEu)?1:0; memset(nonce+9,0,4);
  Ccm_Encrypt(nonce,out,hl,in,out+hl,len,out+hl+len); return len+hl+4; }
static int ext_open(const uint8_t *in, int len, uint8_t *out, uint32_t *rd){ uint16_t n=0;
  if(Sec_OpenEx(in,len,out,&n,SEC_RP_NONE,rd)!=SEC_OK) return -1; return n; }
/* bir sonraki TX'i bekle (en fazla ms), yollanan cerceveyi coz. Donus: ic boy, -1 = TX yok */
static uint32_t tStart;
static int wait_tx(uint32_t ms, uint8_t *in, uint32_t *rd){ tStart=g_now; for(uint32_t k=0;k<ms;k+=5){ advance(5); pump(); if(txBusy){ int n=ext_open(lastTx,lastTxLen,in,rd); txdone(); return n; } } return -1; }
static uint32_t zctr=1000;
static void zybo_table(uint16_t a1,uint8_t s1,uint8_t f1,int8_t snr){ uint8_t t[26],w[64]; t[0]=0xD1;t[1]=2; mkpos(t+2,a1,s1,f1); mkpos(t+14,0x8399,1,1); int n=ext_wrapv(0,0xFFFFFFFE,zctr++,0,t,26,w); rxs(w,n,-60,snr); }

int main(void){
  int fails=0; uint8_t b[256], w[256], o[256]; int n; uint32_t rd;
  memset(secFlash,0xFF,sizeof secFlash); memset(fakeFlash,0xFF,sizeof fakeFlash);
  SubghzApp_Init();
  printf("myAddr=0x%04X dev_id=0x%08X\n", myAddr, (unsigned)myDevId);
#if ROLE == 0
  /* ======================= AKTARICI (komsu) ======================= */
  const uint32_t FARDEV=0x11223344; const uint16_t FAR=0x8203; uint32_t fctr=50;
  /* GW kalitesi: tabloda yokken 0 */
  n=wait_tx(7000,o,&rd);
  CHECK(n==12 && ((o[11]>>3)&0xF)==0 && lastTxLen==25,"tabloda kendini gormeden GW kalitesi 0, zarf 0x51 (25 B)");
  /* Zybo tablosunda kendimizi dogrudan gorduk, SNR 6 -> q = 13 */
  zybo_table(myAddr,1,1,6);
  n=wait_tx(7000,o,&rd);
  CHECK(n==12 && ((o[11]>>3)&0xF)==13 && !(o[11]&0x02),"tabloda dogrudan + Zybo SNR 6 -> konum bayraginda GW kalitesi 13 (bit3..6)");
  zybo_table(myAddr,2,1,-25);
  n=wait_tx(7000,o,&rd);
  CHECK(n==12 && ((o[11]>>3)&0xF)==1,"Zybo SNR -25 -> GW kalitesi en az 1 (0 = aktarici olamaz anlamina ayrildi)");
  zybo_table(myAddr,3,1,13);
  n=wait_tx(7000,o,&rd);
  CHECK(n==12 && ((o[11]>>3)&0xF)==15,"Zybo SNR +13 -> GW kalitesi 15'e kirpildi");

  /* 1) 0x52, relay_dev = BEN -> 20..70 ms icinde E1 */
  { uint8_t fp[12]; mkpos(fp,FAR,5,0x01|0x02); uint8_t far[64]; int fn=ext_wrapv(1,FARDEV,fctr++,myDevId,fp,12,far);
    CHECK(fn==29,"uzak node'un 0x52 konumu 29 B");
    rxs(far,fn,-85,-3);
    n=wait_tx(1000,o,&rd); uint32_t dt=g_now-tStart;
    CHECK(n==34 && o[0]==0xE1 && memcmp(o+5,far,29)==0 && lastTxLen==47,"secildim: E1 ici 34 B (uzak 0x52 paketi aynen), hava 47 B");
    CHECK(dt>=20 && dt<=75,"secilen aktarici rastgele yarisa girmeden 20..70 ms icinde aktardi");
    CHECK(lastTx[0]==0x51,"aktaricinin kendi zarfi 0x51 (aktarim istemiyor)");
    hexdump("relay_E1_v2",lastTx,lastTxLen); hexdump("relay_E1_v2_inner_far",far,fn);
    CHECK(Client_IsActive(FAR),"uzak node musterim (E3 ile tablo/komut alacak)");
  }
  /* 2) uzak node'un C5'i (0x52, beni secti) -> E2 */
  { uint8_t c5[15]; mkcmd(c5,0xC5,FAR,0xFFFE,3,1); c5[7]=0xFE;c5[8]=0xFF;c5[9]=0x40;
    uint8_t far[64]; int fn=ext_wrapv(1,FARDEV,fctr++,myDevId,c5,15,far);
    rxs(far,fn,-85,-3);
    n=wait_tx(1000,o,&rd); uint32_t dt=g_now-tStart;
    CHECK(n==37 && o[0]==0xE2 && memcmp(o+5,far,32)==0 && lastTxLen==50 && dt<=75,"C5 (0x52, beni secti): E2 ici 37 B, hava 50 B, hemen");
    hexdump("relay_E2_v2",lastTx,lastTxLen); hexdump("relay_E2_v2_inner_far",far,fn);
  }
  /* 3) relay_dev = FFFFFFFF -> eski rastgele bekleme (100..600 ms) */
  { uint8_t fp[12]; mkpos(fp,FAR,6,0x01|0x02); uint8_t far[64]; int fn=ext_wrapv(1,FARDEV,fctr++,0xFFFFFFFF,fp,12,far);
    rxs(far,fn,-85,-3);
    n=wait_tx(1000,o,&rd); uint32_t dt=g_now-tStart;
    CHECK(n==34 && o[0]==0xE1 && dt>=100 && dt<=605,"relay_dev FFFFFFFF: herkes aktarabilir, 100..600 ms rastgele bekleme");
    hexdump("far_pos_any",far,fn);
  }
  /* 4) relay_dev = baskasi -> aktarmaz, musteri listesinden cikar */
  { uint8_t fp[12]; mkpos(fp,FAR,7,0x01|0x02); uint8_t far[64]; int fn=ext_wrapv(1,FARDEV,fctr++,0xAABBCCDD,fp,12,far);
    rxs(far,fn,-85,-3);
    int e1=0; for(int k=0;k<20;k++){ advance(50); pump(); if(txBusy){ n=ext_open(lastTx,lastTxLen,o,&rd); if(n>0&&o[0]==0xE1) e1=1; txdone(); } }
    CHECK(!e1,"baska komsu secildi: aktarmadi");
    CHECK(!Client_IsActive(FAR),"uzak node artik musterim degil");
    uint8_t c1[15]; mkcmd(c1,0xC1,0xFFFE,FAR,51,7); int zn=ext_wrapv(0,0xFFFFFFFE,zctr++,0,c1,15,w);
    rxs(w,zn,-60,8); int e3=0; for(int k=0;k<20;k++){ advance(50); pump(); if(txBusy){ n=ext_open(lastTx,lastTxLen,o,&rd); if(n>0&&o[0]==0xE3) e3=1; txdone(); } }
    CHECK(!e3,"musteri olmadigi icin Zybo komutunu asagi (E3) aktarmadi: tek kopya secilen aktaricidan");
    uint8_t c5[15]; mkcmd(c5,0xC5,FAR,0xFFFE,4,1); fn=ext_wrapv(1,FARDEV,fctr++,0xAABBCCDD,c5,15,far);
    rxs(far,fn,-85,-3); int e2=0; for(int k=0;k<20;k++){ advance(50); pump(); if(txBusy){ n=ext_open(lastTx,lastTxLen,o,&rd); if(n>0&&o[0]==0xE2) e2=1; txdone(); } }
    CHECK(!e2,"baskasini secen uzak node'un C5'ini de aktarmadi");
  }
  /* 5) relay_dev sahte: baslik degistirilirse MIC tutmaz */
  { uint8_t fp[12]; mkpos(fp,FAR,8,0x01|0x02); uint8_t far[64]; int fn=ext_wrapv(1,FARDEV,fctr++,0xAABBCCDD,fp,12,far);
    memcpy(far+9,&myDevId,4);   /* araya giren biri relay_dev'i bana cevirdi */
    rxs(far,fn,-85,-3);
    int e1=0; for(int k=0;k<20;k++){ advance(50); pump(); if(txBusy){ n=ext_open(lastTx,lastTxLen,o,&rd); if(n>0&&o[0]==0xE1) e1=1; txdone(); } }
    CHECK(!e1,"relay_dev MIC ile korunuyor: degistirilmis baslik atildi");
  }
  /* 6) secildim ama gateway'e ulasamiyorum -> aktarmam */
  { advance(26000); while(txBusy){ txdone(); pump(); }
    uint8_t fp[12]; mkpos(fp,FAR,9,0x01|0x02); uint8_t far[64]; int fn=ext_wrapv(1,FARDEV,fctr++,myDevId,fp,12,far);
    rxs(far,fn,-85,-3);
    int e1=0, gq=-1; for(int k=0;k<140;k++){ advance(50); pump(); if(txBusy){ n=ext_open(lastTx,lastTxLen,o,&rd); if(n>0&&o[0]==0xE1) e1=1; if(n==12) gq=(o[11]>>3)&0xF; txdone(); } }
    CHECK(!e1,"secildim ama 25 sn'dir tabloda yokum: aktarmadim (uzak node fallback yapar)");
    CHECK(gq==0,"bu durumda kendi GW kalitem 0");
  }
  /* 7) gateway duz boru: 0x52'yi de Zybo'ya gecirir (gateway rolu ayri testte) */
#else
  /* ======================= UZAK NODE ======================= */
  const uint32_t DA=0xA0000001, DB=0xB0000002, DC=0xC0000003; const uint16_t AA=0x8105, AB=0x8106, AC=0x8107;
  uint32_t ca=10, cb=10, cc=10; uint8_t sa=0, sb=0, sc=0;
  #define NB(dev,addr,ctr,sq,gwq,snr) do{ uint8_t p_[12]; mkpos(p_,addr,(sq)++,0x01|((gwq)<<3)); int n_=ext_wrapv(0,dev,(ctr)++,0,p_,12,w); rxs(w,n_,-70,snr); }while(0)
  /* komsular: A link 14 (snr 8) gw 10 -> 10;  B link 10 (snr 0) gw 15 -> 10;  C gw 0 (kendisi aktarimda) */
  NB(DA,AA,ca,sa,10,8); NB(DB,AB,cb,sb,15,0); NB(DC,AC,cc,sc,0,10);
  /* 25 sn tablo yok -> aktarim istenir */
  int got=0; uint8_t mySeq=0;
  for(int k=0;k<10 && !got;k++){ n=wait_tx(7000,o,&rd); if(n==12 && (o[11]&0x02)){ got=1; mySeq=o[2]; }
    NB(DA,AA,ca,sa,10,8); NB(DB,AB,cb,sb,15,0); NB(DC,AC,cc,sc,0,10); }
  CHECK(got && lastTx[0]==0x52 && lastTxLen==29,"aktarim isteyen konum 0x52 zarfla, 29 B");
  CHECK(got && rd==DA,"esit puanda baglantisi iyi olan A secildi (C: GW 0, aday degil)");
  CHECK(got && ((o[11]>>3)&0xF)==0,"uzak node'un kendi GW kalitesi 0");
  hexdump("far_pos_v2",lastTx,lastTxLen);
  uint8_t mine[64]; int mineLen=lastTxLen; memcpy(mine,lastTx,lastTxLen);
  /* A aktardi (E1'ini duyduk) -> onay */
  #define OVERHEAR(dev,addr,ctr) do{ uint8_t in_[80]; in_[0]=0xE1; in_[1]=(addr)&0xFF; in_[2]=(addr)>>8; in_[3]=(uint8_t)-80; in_[4]=0; memcpy(in_+5,mine,mineLen); int n_=ext_wrapv(0,dev,(ctr)++,0,in_,5+mineLen,w); rxs(w,n_,-70,5); }while(0)
  OVERHEAR(DA,AA,ca);
  CHECK(!chkActive && chkMiss==0,"secilen A'nin E1'ini duyduk: aktarim onaylandi");
  /* emniyet: Zybo bit2 koymadan tabloda (E3 ile) seq'imizi gorduk -> aktarimla say */
  { uint8_t t[26]; t[0]=0xD1;t[1]=2; mkpos(t+2,myAddr,mySeq,0x01); mkpos(t+14,AA,1,1);
    uint8_t z[64]; int zn=ext_wrapv(0,0xFFFFFFFE,zctr++,0,t,26,z);
    uint8_t in[80]; in[0]=0xE3; in[1]=AA&0xFF; in[2]=AA>>8; memcpy(in+3,z,zn); int en=ext_wrapv(0,DA,ca++,0,in,3+zn,w);
    uint32_t before=lastInTableTime; rxs(w,en,-70,5);
    CHECK(lastInTableTime==before && Node_NeedsRelay(),"bit2'siz tablo + aktarildigini duydugumuz seq: dogrudan sayilmadi (salinim yok)");
  }
  /* histerezis: B biraz iyilesti (11 > 10) -> A kalir; sonra B 15 (5 basamak) -> B */
  NB(DB,AB,cb,sb,15,2);   /* link 11 */
  n=wait_tx(7000,o,&rd); memcpy(mine,lastTx,lastTxLen); mineLen=lastTxLen;
  CHECK(n==12 && rd==DA,"B 1 basamak iyi: histerezis, A ile devam");
  OVERHEAR(DA,AA,ca);
  NB(DA,AA,ca,sa,10,8); NB(DB,AB,cb,sb,15,10);   /* B link 15 gw 15 -> 15 */
  n=wait_tx(7000,o,&rd); memcpy(mine,lastTx,lastTxLen); mineLen=lastTxLen;
  CHECK(n==12 && rd==DB,"B 5 basamak iyi: B'ye gecildi");
  /* fallback: B iki kez aktarmadi -> 60 sn kotu, A'ya don */
  NB(DA,AA,ca,sa,10,8); NB(DB,AB,cb,sb,15,10);
  n=wait_tx(7000,o,&rd);
  CHECK(n==12 && rd==DB,"B'nin ilk kacirmasi: hala B");
  NB(DA,AA,ca,sa,10,8); NB(DB,AB,cb,sb,15,10);
  n=wait_tx(7000,o,&rd); memcpy(mine,lastTx,lastTxLen); mineLen=lastTxLen;
  CHECK(n==12 && rd==DA,"B ust uste 2 kez aktarmadi: kotu isaretlendi, A secildi");
  OVERHEAR(DA,AA,ca);
  NB(DA,AA,ca,sa,10,8); NB(DB,AB,cb,sb,15,10);
  n=wait_tx(7000,o,&rd);
  CHECK(n==12 && rd==DA,"B 60 sn aday degil (puani yuksek olsa da)");
  /* aktarim modunda telefondan C1: 0x52, mevcut aktarici */
  { mkcmd(dlFrame,0xC1,0,0xFFFF,9,2); dlFrameLen=15; dlFrameReady=true; Node_HandleUartFrame();
    int c1=0; for(int k=0;k<40 && !c1;k++){ advance(50); pump(); if(txBusy){ n=ext_open(lastTx,lastTxLen,o,&rd); if(n==15&&o[0]==0xC1) c1=1; else txdone(); } }
    CHECK(c1 && lastTx[0]==0x52 && lastTxLen==32 && rd==curRelayDev,"aktarim modunda C1: 0x52, 32 B, relay_dev = secili aktarici");
    txdone(); }
  /* komsular susarsa (30 sn) -> FFFFFFFF */
  { uint32_t last=0; for(int k=0;k<8;k++){ n=wait_tx(7000,o,&rd); if(n==12) last=rd; }
    CHECK(last==0xFFFFFFFF && lastTx[0]==0x52,"30 sn'dir komsu duyulmadi: relay_dev = FFFFFFFF (herkes)");
    hexdump("far_pos_ffffffff",lastTx,lastTxLen); }
  /* tabloda aktarilmadigini bildigimiz seq bit2'siz: gercekten dogrudan ulasmisiz -> istek kapanir */
  { n=wait_tx(7000,o,&rd); uint8_t s=o[2];
    zybo_table(myAddr,s,0x01,5);
    CHECK(!Node_NeedsRelay(),"aktarildigini duymadigimiz seq bit2'siz geldi: dogrudan, aktarim istegi kapandi");
    n=wait_tx(7000,o,&rd);
    CHECK(n==12 && !(o[11]&0x02) && lastTx[0]==0x51 && lastTxLen==25,"dogrudan modda konum yine 0x51 (25 B)"); }
  /* Zybo bit2'yi koymaya baslayinca emniyet devre disi */
  { zybo_table(myAddr,1,0x01|0x04,5); CHECK(sawViaBit,"tabloda bit2 goruldu: emniyet kapandi (Zybo yeni surum)"); }
#endif
  printf(fails? "HATA: %d test basarisiz\n" : "HEPSI GECTI\n", fails);
  return fails?1:0;
}
