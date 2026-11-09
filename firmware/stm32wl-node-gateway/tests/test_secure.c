/* Faz 1 guvenlik testleri. ROLE: 0 = node (JOIN=0), 1 = gateway, 2 = node JOIN=1 (altin cerceveler) */
#include "test_secure_prelude.h"
/* ---- helpers ---- */
static void advance(uint32_t ms){ uint32_t end=g_now+ms; while(g_now<end){ g_now++; for(int i=0;i<nT;i++) if(T[i].running && g_now>=T[i].due){ T[i].running=0; T[i].cb(T[i].ctx);} } }
static void rx(const uint8_t*p,int n,int16_t rssi){ memcpy(BufferRx,p,n); RxBufferSize=n; RssiValue=rssi; SnrValue=10; HandleRxPacket(); }
static void txdone(void){ txBusy=false; }
static void pump(void){ Node_Process(); }
static void mkpos(uint8_t*b,uint16_t addr,uint8_t seq,uint8_t flags){ b[0]=addr&0xFF;b[1]=addr>>8;b[2]=seq; memset(b+3,0x11,8); b[11]=flags; }
static void mkcmd(uint8_t*b,uint8_t type,uint16_t src,uint16_t dst,uint8_t id,uint8_t kind){ memset(b,0,15); b[0]=type;b[1]=src&0xFF;b[2]=src>>8;b[3]=dst&0xFF;b[4]=dst>>8;b[5]=id;b[6]=kind; }
static void hexdump(const char*name,const uint8_t*p,int n){ printf("FRAME %s ",name); for(int i=0;i<n;i++) printf("%02x",p[i]); printf("\n"); }
#define CHECK(c,msg) do{ if(c) printf("  OK   %s\n",msg); else { printf("  FAIL %s\n",msg); fails++; } }while(0)

/* Test tarafinin kendi sifreleyicisi (Zybo / baska node taklidi), firmware'in Ccm_* fonksiyonlarini kullanir.
   Bagimsiz dogrulama: FRAME satirlari Python'un cryptography kutuphanesiyle ayrica cozulur. */
#if ROLE != 1
static int ext_wrap(uint32_t dev, uint32_t ctr, const uint8_t *in, int len, uint8_t *out){
  uint8_t nonce[13]; out[0]=0x51; memcpy(out+1,&dev,4); memcpy(out+5,&ctr,4);
  memcpy(nonce,out+1,8); nonce[8]=(dev==0xFFFFFFFEu)?1:0; memset(nonce+9,0,4);
  Ccm_Encrypt(nonce,out,9,in,out+9,len,out+9+len); return len+13; }
static int ext_open(const uint8_t *in, int len, uint8_t *out){ uint16_t n=0;
  if(Sec_Open(in,len,out,&n,SEC_RP_NONE)!=SEC_OK) return -1; return n; }
#endif

int main(void){
  int fails=0; uint8_t b[256], w[256], o[256]; int n;
  memset(secFlash,0xFF,sizeof secFlash); memset(fakeFlash,0xFF,sizeof fakeFlash);
  SubghzApp_Init();
  printf("myAddr=0x%04X\n", myAddr);
#if ROLE == 1
  /* ======================= GATEWAY ======================= */
  { /* protokol test vektoru: guvenli konum, RSSI -60 SNR 8 -> UART altin cerceve */
    uint8_t sec[25]={0x51,0x78,0x56,0x34,0x12,0x01,0,0,0,0x5b,0x0d,0x40,0x6a,0xae,0x8d,0x1e,0xff,0xcd,0xe1,0x0f,0x18,0xb6,0x2d,0x73,0x27};
    uint8_t gold[]={0xaa,0x55,0x1b,0x51,0x78,0x56,0x34,0x12,0x01,0,0,0,0x5b,0x0d,0x40,0x6a,0xae,0x8d,0x1e,0xff,0xcd,0xe1,0x0f,0x18,0xb6,0x2d,0x73,0x27,0xc4,0x08,0x90};
    int uc=uartCount; memcpy(BufferRx,sec,25); RxBufferSize=25; RssiValue=-60; SnrValue=8; HandleRxPacket();
    CHECK(uartCount==uc+1 && lastUartLen==31 && memcmp(lastUart,gold,31)==0,"guvenli konum acilmadan Zybo'ya: test vektorundeki UART cercevesiyle birebir (CRC 0x90)");
    txCount=0; for(int k=0;k<6;k++){ pump(); txdone(); advance(500); }
    CHECK(txCount==0,"gateway guvenli paketi havada tekrar yayinlamadi (tekrar yayin Zybo'nun isi)");
    uc=uartCount; memset(b,0x51,46); b[0]=0x51; rx(b,46,-70);
    CHECK(uartCount==uc+1 && lastUartLen==52 && lastUart[2]==48,"ic ice E2 boyunda (46 B) paket LEN 48 ile iletildi");
    { /* V1.17: 0x52 (aktarici secimli) uzak node konumu da acilmadan Zybo'ya */
      uint8_t v2[29]={0x52,0x55,0xba,0xc8,0xa3,0x05,0x00,0x00,0x00,0x01,0x00,0x00,0xa0,0x33,0x2b,0xf5,0x23,0x54,0xfd,0xb4,0x65,0xf3,0x74,0xf4,0xb6,0x64,0xd4,0x02,0x46};
      uc=uartCount; memcpy(BufferRx,v2,29); RxBufferSize=29; RssiValue=-90; SnrValue=-5; HandleRxPacket();
      CHECK(uartCount==uc+1 && lastUartLen==35 && lastUart[2]==31 && memcmp(lastUart+3,v2,29)==0,"0x52 konum (29 B) LEN 31 ile aynen Zybo'ya");
      hexdump("gw_uart_far_pos_v2",lastUart,lastUartLen);
      uc=uartCount; memset(b,0x52,28); b[0]=0x52; rx(b,28,-70);
      CHECK(uartCount==uc,"0x52 ile baslayan 28 B (guvenli icin fazla kisa) boruya alinmadi");
    }
    memset(b,0x51,243); rx(b,243,-70);
    CHECK(lastUartLen==249 && lastUart[2]==245,"buyuk guvenli paket (243 B) LEN 245 ile iletildi");
  }
  { /* duz C1 (gecis): eski yol, 3x tekrar yayin */
    mkcmd(b,0xC1,0x8101,0xFFFF,7,2); txCount=0; int uc=uartCount; rx(b,15,-50);
    for(int k=0;k<8;k++){ pump(); txdone(); advance(500); }
    CHECK(txCount==3 && uartCount==uc+1,"SECURE_ONLY=0: duz C1 eski yolla (3x yayin + Zybo'ya) islendi");
  }
  { /* Zybo downlink: [tx_repeat][guvenli] */
    uint8_t sec[28]; memset(sec,0xAB,28); sec[0]=0x51;
    dlFrame[0]=3; memcpy(dlFrame+1,sec,28); dlFrameLen=29; dlFrameReady=true; txCount=0;
    uint32_t t0=g_now, times[8]; int tn=0;
    for(int k=0;k<40;k++){ int before=txCount; pump(); if(txCount>before){ times[tn++]=g_now; txdone(); } advance(100); }
    CHECK(txCount==3 && lastTxLen==28 && memcmp(lastTx,sec,28)==0,"tx_repeat=3: guvenli paket 3 kez, byte'i byte'ina ayni yayinlandi");
    CHECK(tn==3 && (times[1]-times[0])>=400 && (times[1]-times[0])<=700 && (times[2]-times[1])>=400,"tekrarlar arasi 400-600 ms");
    uint8_t tab[39]; memset(tab,0xCD,39); tab[0]=0x51;
    dlFrame[0]=1; memcpy(dlFrame+1,tab,39); dlFrameLen=40; dlFrameReady=true; txCount=0;
    for(int k=0;k<20;k++){ pump(); txdone(); advance(100); }
    CHECK(txCount==1 && lastTxLen==39,"tx_repeat=1 (tablo): tek yayin");
    /* araya girme: A2 (3x) sonra tablo (1x) -> tablo A2'nin tekrarlarini iptal etmez */
    uint8_t a2s[28]; memset(a2s,0xA2,28); a2s[0]=0x51;
    dlFrame[0]=3; memcpy(dlFrame+1,a2s,28); dlFrameLen=29; dlFrameReady=true; pump(); txdone();
    dlFrame[0]=1; memcpy(dlFrame+1,tab,39); dlFrameLen=40; dlFrameReady=true; pump(); txdone();
    int a2n=1, tn2=1; txCount=0;
    for(int k=0;k<30;k++){ int before=txCount; pump(); if(txCount>before){ if(lastTx[1]==0xA2) a2n++; else tn2++; txdone(); } advance(100); }
    CHECK(a2n==3 && tn2==1,"tablo araya girse de A2'nin 3 yayini tamamlandi");
    /* kuyruk dolu */
    for(int q=0;q<5;q++){ uint8_t x[28]; memset(x,0x10+q,28); x[0]=0x51; dlFrame[0]=3; memcpy(dlFrame+1,x,28); dlFrameLen=29; dlFrameReady=true; txBusy=true; pump(); }
    txBusy=false; int seen[5]={0};
    for(int k=0;k<60;k++){ int before=txCount; pump(); if(txCount>before){ if(lastTx[1]>=0x10&&lastTx[1]<0x15) seen[lastTx[1]-0x10]++; txdone(); } advance(100); }
    CHECK(seen[0]==0 && seen[1]==3 && seen[4]==3,"kuyruk dolunca (4) en eski paket atildi, digerleri 3'er kez gitti");
    /* duz downlink hala eski yolla (SECURE_ONLY=0) */
    mkcmd(b,0xC3,0xFFFE,0x8101,9,1); memcpy(dlFrame,b,15); dlFrameLen=15; dlFrameReady=true; txCount=0;
    for(int k=0;k<8;k++){ pump(); txdone(); advance(500); }
    CHECK(txCount==3 && lastTx[0]==0xC3,"duz C3 downlink eski yolla 3x yayinlandi");
  }
#if SECURE_ONLY == 1
#error "bu harness rol 1 icin SECURE_ONLY=0 ile derlenir"
#endif
#else
  /* ======================= NODE ======================= */
  CHECK(secCtr==1 && secSlot==1 && secSaved==1,"ilk acilis: sayac 1 flash'a yazildi (yuva 0)");
  { uint32_t lo,hi; memcpy(&lo,secFlash,4); memcpy(&hi,secFlash+4,4); CHECK(lo==1 && hi==~1u,"yuva formati [sayac][~sayac]"); }
#if ROLE == 0
  /* ---- konum yayini zarfli ---- */
  txCount=0; for(int k=0;k<70;k++){ advance(100); pump(); if(txBusy) break; }
  CHECK(txBusy && lastTxLen==25 && lastTx[0]==0x51,"konum paketi 25 B guvenli zarfla yollandi");
  { uint32_t dv,ct; memcpy(&dv,lastTx+1,4); memcpy(&ct,lastTx+5,4);
    CHECK(dv==myDevId && ct==1,"zarfta dev_id ve sayac dogru");
    n=ext_open(lastTx,25,o); CHECK(n==12 && o[0]==(myAddr&0xFF) && o[1]==(myAddr>>8),"zarf acilinca icte bugunku 12 B konum paketi");
    hexdump("node_pos",lastTx,25); }
  txdone();
  /* ---- sayac / flash ---- */
  { secCtr=1000; secSaved=999-255+0; /* secSaved+256 <= 1000 -> kayit */
    uint16_t sl=secSlot; uint32_t c=Sec_NextCtr();
    CHECK(c==1000 && secSlot==sl+1 && secSaved==1000,"sayac kayit esigini gecince flash'a yazildi");
    for(int i=0;i<100;i++) (void)Sec_NextCtr();
    CHECK(secSlot==sl+1,"esik gelmeden tekrar yazilmadi");
    uint32_t used=secCtr; Sec_Init();
    CHECK(secCtr==1000+256 && secCtr>used,"yeniden acilis: sayac = son kayit + 256 (kullanilanlarin hepsinden buyuk)");
    int e0=secErases; for(int i=0;i<300;i++){ Sec_SaveCtr(5000+i); }
    CHECK(secErases==e0+1,"256 yuva dolunca sayfa bir kez silindi");
    Sec_Init(); CHECK(secCtr==5299+256,"sayfa silindikten sonra da son kayit dogru bulundu");
    memset(secFlash,0x00,64); Sec_Init();
    CHECK(secNeedErase==false && secCtr>=1 && secSlot==1,"bozuk sayfa: silinip yeniden yazildi");
  }
  /* ---- Zybo downlink: tablo (kendimiz dogrudan) ---- */
  uint8_t tab[2+12*2]; tab[0]=0xD1; tab[1]=2; mkpos(tab+2,myAddr,3,0x01); mkpos(tab+14,0x8302,9,0x01);
  int uc=uartCount; int d1=uartType[0xD1]; n=ext_wrap(0xFFFFFFFE,100,tab,26,w); hexdump("zybo_table",w,n); rx(w,n,-40);
  CHECK(uartCount>uc && uartType[0xD1]==d1+1,"Zybo'nun guvenli tablosu cozuldu, telefona duz tablo gitti");
  uc=uartCount; rx(w,n,-40); CHECK(uartCount==uc,"ayni kopya (gateway tx_repeat) tekrar diye atildi");
  { uint8_t c1[15]; mkcmd(c1,0xC1,0xFFFE,0xFFFF,40,2);
    int n2=ext_wrap(0xFFFFFFFE,102,c1,15,o); uc=uartCount; rx(o,n2,-40);
    CHECK(uartCount>uc,"sayac 102 kabul");
    mkcmd(c1,0xC1,0xFFFE,0xFFFF,41,2); n2=ext_wrap(0xFFFFFFFE,101,c1,15,o); uc=uartCount; rx(o,n2,-40);
    CHECK(uartCount>uc,"geciken sayac 101 (pencere icinde, gorulmemis) kabul");
    uc=uartCount; rx(o,n2,-40); CHECK(uartCount==uc,"101 ikinci kez: atildi");
    mkcmd(c1,0xC1,0xFFFE,0xFFFF,42,2); n2=ext_wrap(0xFFFFFFFE,102-40,c1,15,o); uc=uartCount; rx(o,n2,-40);
    CHECK(uartCount==uc,"pencerenin disinda kalan eski sayac (62) atildi");
    mkcmd(c1,0xC1,0xFFFE,0xFFFF,43,2); n2=ext_wrap(0xFFFFFFFE,103,c1,15,o); o[12]^=0x04; uc=uartCount; rx(o,n2,-40);
    CHECK(uartCount==uc,"bir biti degistirilmis paket (MIC hatasi) islenmedi");
    mkcmd(c1,0xC1,0xFFFE,0xFFFF,44,2); uint8_t k2[16]; memcpy(k2,aesRk,16); k2[0]^=1;
    uint8_t save[176]; memcpy(save,aesRk,176); Aes_SetKey(k2); n2=ext_wrap(0xFFFFFFFE,104,c1,15,o); memcpy(aesRk,save,176);
    uc=uartCount; rx(o,n2,-40); CHECK(uartCount==uc,"yanlis anahtarla uretilmis sahte admin komutu islenmedi");
    mkcmd(c1,0xC1,0xFFFE,0xFFFF,45,2); uc=uartCount; rx(c1,15,-40);
#if SECURE_ONLY == 1
    CHECK(uartCount==uc,"SECURE_ONLY=1: duz paket atildi");
    { uint8_t t2[26]; memcpy(t2,tab,26); t2[2]=0x99; uc=uartCount; rx(t2,26,-40); CHECK(uartCount==uc,"SECURE_ONLY=1: duz tablo atildi"); }
#else
    CHECK(uartCount>uc,"SECURE_ONLY=0: duz paket (gecis) hala kabul");
#endif
  }
  /* ---- aktarim: uzak node'un konumu -> ic ice E1 ---- */
  { uint8_t fp[12]; mkpos(fp,0x8203,5,0x01|0x02);
    uint8_t far[64]; int fn=ext_wrap(0x11223344,7,fp,12,far);
    txCount=0; rx(far,fn,-80);
    int got=0; for(int k=0;k<10;k++){ advance(100); pump(); if(txBusy){ if(lastTxLen==43) got=1; break; } }
    CHECK(got,"uzak node'un aktarim istegi: 43 B ic ice E1 yollandi");
    n=ext_open(lastTx,lastTxLen,o);
    CHECK(n==30 && o[0]==0xE1 && o[1]==(myAddr&0xFF) && o[2]==(myAddr>>8) && (int8_t)o[3]==-80 && memcmp(o+5,far,25)==0,
          "E1 ici: [E1][aktaran][RSSI][SNR] + uzak node'un guvenli paketi AYNEN");
    hexdump("relay_E1",lastTx,lastTxLen); hexdump("relay_E1_inner_far",far,fn);
    txdone();
  }
  /* ---- aktarim: uzak node'un ACK'i -> ic ice E2 ---- */
  { uint8_t c5[15]; mkcmd(c5,0xC5,0x8203,0xFFFE,3,1); c5[7]=0xFE;c5[8]=0xFF;c5[9]=0x40;
    uint8_t far[64]; int fn=ext_wrap(0x11223344,8,c5,15,far);
    rx(far,fn,-81);
    int got=0; for(int k=0;k<10;k++){ advance(100); pump(); if(txBusy){ if(lastTxLen==46) got=1; break; } }
    n=ext_open(lastTx,lastTxLen,o);
    CHECK(got && n==33 && o[0]==0xE2 && memcmp(o+5,far,28)==0,"musterinin C5'i: 46 B ic ice E2, icte guvenli paketi aynen");
    hexdump("relay_E2",lastTx,lastTxLen); txdone();
  }
  /* ---- aktarim: Zybo komutu -> E3 (musteri var) ---- */
  { uint8_t c1[15]; mkcmd(c1,0xC1,0xFFFE,0x8203,50,7); uint8_t z[64]; int zn=ext_wrap(0xFFFFFFFE,110,c1,15,z);
    rx(z,zn,-40);
    int got=0; for(int k=0;k<10;k++){ advance(100); pump(); if(txBusy){ if(lastTxLen==44) got=1; break; } }
    n=ext_open(lastTx,lastTxLen,o);
    CHECK(got && n==31 && o[0]==0xE3 && memcmp(o+3,z,28)==0,"Zybo komutu musteriye 44 B E3, icte Zybo'nun paketi aynen");
    hexdump("relay_E3",lastTx,lastTxLen); txdone();
  }
  /* ---- bastirma: baska aktaran ayni uzak paketi aktardi ---- */
  { uint8_t fp[12]; mkpos(fp,0x8203,6,0x01|0x02); uint8_t far[64]; int fn=ext_wrap(0x11223344,9,fp,12,far);
    rx(far,fn,-80);   /* bizim aktarimimiz zamanlandi */
    uint8_t inner[64]; inner[0]=0xE1; inner[1]=0x05; inner[2]=0x84; inner[3]=(uint8_t)-70; inner[4]=9; memcpy(inner+5,far,fn);
    uint8_t other[96]; int on=ext_wrap(0x55667788,3,inner,5+fn,other); rx(other,on,-60);
    int e1=0; for(int k=0;k<10;k++){ advance(100); pump(); if(txBusy){ if(lastTxLen==43) e1=1; txdone(); } }
    CHECK(!e1,"baska komsu ayni paketi aktarinca bizimki iptal edildi (ic paketin anahtari cozuldu)");
  }
  /* ---- musteri olarak: 30 sn tablo yok -> aktarim isteriz; E3 ile gelen Zybo paketi, sonra dogrudan kopyasi ---- */
  { int rq=0; for(int k=0;k<320;k++){ advance(100); pump(); if(txBusy){ n=ext_open(lastTx,lastTxLen,o); if(n==12 && (o[11]&0x02)) rq=1; txdone(); } }
    CHECK(rq,"30 sn kendini tabloda gormeyince konum paketine 'beni aktarin' bayragi kondu (sifreli icte)"); }
  { uint8_t c1[15]; mkcmd(c1,0xC1,0xFFFE,0xFFFF,60,2); uint8_t z[64]; int zn=ext_wrap(0xFFFFFFFE,120,c1,15,z);
    uint8_t inner[64]; inner[0]=0xE3; inner[1]=0x05; inner[2]=0x84; memcpy(inner+3,z,zn);
    uint8_t e3[96]; int en=ext_wrap(0x55667788,4,inner,3+zn,e3);
    int uc2=uartCount; rx(e3,en,-60);
    CHECK(uartCount>uc2,"E3 ile gelen Zybo komutu cozuldu, telefona gitti");
    uc2=uartCount; rx(z,zn,-40);
    CHECK(uartCount==uc2,"ayni Zybo paketinin dogrudan kopyasi tekrar diye atildi (bedava tekrar eleme)");
    uc2=uartCount; rx(e3,en,-60);
    CHECK(uartCount==uc2,"kaydedilmis E3'un tekrar oynatilmasi atildi (dis zarfin sayaci)");
  }
#elif ROLE == 2
  /* ---- altin cerceveler: JOIN ile ---- */
  memset(dlFrame,0,15); dlFrame[0]=0xC9; dlFrame[1]=1; dlFrame[2]=2; dlFrame[3]=0xD2; dlFrame[4]=0x04; dlFrameLen=15; dlFrameReady=true; Node_HandleUartFrame();
  int a1=0; for(int k=0;k<20;k++){ advance(100); pump(); if(txBusy){ if(lastTxLen==30){ a1=1; hexdump("node_A1",lastTx,30);} txdone(); if(a1) break; } }
  CHECK(a1,"A1 30 B guvenli zarfla yollandi");
  uint8_t a2[15]={0xA2,0xFE,0xFF,0xFF,0xFF,0x30, 0,0,0,0, 0x02,0x81,1,2,0}; memcpy(a2+6,&myDevId,4);
  n=ext_wrap(0xFFFFFFFE,500,a2,15,w); hexdump("zybo_A2",w,n); rx(w,n,-40);
  CHECK(joinState==JOIN_ST_JOINED && myAddr==0x8102,"Zybo'nun guvenli A2'si cozuldu: ALPHA-2 (0x8102)");
  rx(w,n,-40); rx(w,n,-40);   /* tx_repeat kopyalari */
  int acks=0,pos=0; for(int k=0;k<80;k++){ advance(100); pump(); if(txBusy){ n=ext_open(lastTx,lastTxLen,o);
      if(n==15 && o[0]==0xC5){ if(!acks) hexdump("node_C5",lastTx,lastTxLen); acks++; }
      if(n==12 && !pos){ pos=1; hexdump("node_pos",lastTx,lastTxLen);} txdone(); } }
  CHECK(acks==3,"ULASTI 3 kez, her biri yeni sayacla (kopyalar tekrar ACK uretmedi)");
  CHECK(pos,"konum yayini guvenli basladi");
  mkcmd(dlFrame,0xC1,0,0xFFFF,9,2); dlFrameLen=15; dlFrameReady=true; Node_HandleUartFrame();
  int c1=0; for(int k=0;k<10;k++){ advance(100); pump(); if(txBusy){ n=ext_open(lastTx,lastTxLen,o); if(n==15 && o[0]==0xC1){ c1=1; hexdump("node_C1",lastTx,lastTxLen);} txdone(); if(c1) break; } }
  CHECK(c1,"telefondan gelen C1 guvenli yollandi");
#endif
#endif
  printf(fails? "HATA: %d test basarisiz\n" : "HEPSI GECTI\n", fails);
  return fails?1:0;
}
