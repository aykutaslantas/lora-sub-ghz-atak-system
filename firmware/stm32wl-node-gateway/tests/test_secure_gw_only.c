#define ROLE 9
#include "test_secure_prelude.h"
int main(void){ memset(secFlash,0xFF,2048); SubghzApp_Init(); int f=0;
 uint8_t c1[15]={0xC1,0x01,0x81,0xFF,0xFF,7,2}; int uc=uartCount; memcpy(BufferRx,c1,15); RxBufferSize=15; HandleRxPacket();
 if(uartCount!=uc){printf("FAIL plain RX forwarded\n");f++;} else printf("  OK   gateway SECURE_ONLY=1: duz paket atildi\n");
 memcpy(dlFrame,c1,15); dlFrameLen=15; dlFrameReady=true; txCount=0; for(int k=0;k<5;k++){Node_Process(); txBusy=false;}
 if(txCount){printf("FAIL plain DL sent\n");f++;} else printf("  OK   gateway SECURE_ONLY=1: duz downlink atildi\n");
 printf(f?"HATA\n":"HEPSI GECTI\n"); return f;}
