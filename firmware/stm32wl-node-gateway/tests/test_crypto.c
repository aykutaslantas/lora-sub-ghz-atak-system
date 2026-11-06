/* AES-128 (FIPS-197 C.1) ve AES-128-CCM test vektorleri */
#include <stdio.h>
#include "../SubGHz_Phy/App/lora_secure.h"
#define CHECK(c,msg) do{ if(c) printf("  OK   %s\n",msg); else { printf("  FAIL %s\n",msg); fails++; } }while(0)
int main(void){
  int fails=0;
  uint8_t key[16]; for(int i=0;i<16;i++) key[i]=(uint8_t)i; Aes_SetKey(key);
  const uint8_t p[16]={0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
  const uint8_t c_ref[16]={0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
  uint8_t c[16]; Aes_Encrypt(p,c);
  CHECK(memcmp(c,c_ref,16)==0,"AES-128 FIPS-197 C.1");
  /* zarf: dev_id 0x12345678, sayac 1, dir 0, ic paket = konum 0x8101 */
  const uint8_t hdr[9]={0x51,0x78,0x56,0x34,0x12,0x01,0,0,0};
  const uint8_t nonce[13]={0x78,0x56,0x34,0x12,0x01,0,0,0,0x00,0,0,0,0};
  const uint8_t pt[12]={0x01,0x81,0x07,0xd0,0x5a,0x71,0x18,0xc0,0xc0,0x45,0x11,0x01};
  const uint8_t ct_ref[12]={0x5b,0x0d,0x40,0x6a,0xae,0x8d,0x1e,0xff,0xcd,0xe1,0x0f,0x18};
  const uint8_t mic_ref[4]={0xb6,0x2d,0x73,0x27};
  uint8_t ct[12], mic[4], back[12];
  Ccm_Encrypt(nonce,hdr,9,pt,ct,12,mic);
  CHECK(memcmp(ct,ct_ref,12)==0 && memcmp(mic,mic_ref,4)==0,"CCM: zarf test vektoru (sifreli veri + MIC b6 2d 73 27)");
  int ok=Ccm_Decrypt(nonce,hdr,9,ct,back,12,mic);
  CHECK(ok && memcmp(back,pt,12)==0,"CCM: cozme + MIC dogrulama");
  ct[3]^=1; ok=Ccm_Decrypt(nonce,hdr,9,ct,back,12,mic);
  CHECK(!ok,"CCM: degistirilmis paket reddedildi");
  printf(fails? "HATA: %d test basarisiz\n" : "HEPSI GECTI\n", fails);
  return fails?1:0;
}
