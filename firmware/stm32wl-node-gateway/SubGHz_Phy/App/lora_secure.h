/**
  ******************************************************************************
  * @file    lora_secure.h
  * @brief   AES-128 (yazilim) + AES-128-CCM (RFC 3610), 13 B nonce, 4 B MIC.
  *          Header-only: sadece subghz_phy_app.c icinden #include edilir
  *          (CubeIDE projesi dosyalari tek tek bagladigi icin ayri .c eklenmedi).
  *          Bir paket icin birkac AES blogu: Cortex-M4 48 MHz'de ~0.1 ms, onemsiz.
  ******************************************************************************
  */
#ifndef __LORA_SECURE_H__
#define __LORA_SECURE_H__

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ AES-128 */
static const uint8_t aesSbox[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static uint8_t aesRk[176];                  /* genisletilmis anahtar (11 tur anahtari) */

static uint8_t Aes_Xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80U) ? 0x1BU : 0x00U)); }

/* Anahtar genisletme: acilista bir kez */
static void Aes_SetKey(const uint8_t key[16])
{
  static const uint8_t rcon[10] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1B,0x36 };
  memcpy(aesRk, key, 16);
  for (uint32_t i = 4; i < 44; i++)
  {
    uint8_t t[4];
    memcpy(t, &aesRk[(i - 1) * 4], 4);
    if ((i % 4) == 0)
    {
      uint8_t u = t[0];
      t[0] = (uint8_t)(aesSbox[t[1]] ^ rcon[(i / 4) - 1]);
      t[1] = aesSbox[t[2]];
      t[2] = aesSbox[t[3]];
      t[3] = aesSbox[u];
    }
    for (uint32_t j = 0; j < 4; j++)
    {
      aesRk[i * 4 + j] = (uint8_t)(aesRk[(i - 4) * 4 + j] ^ t[j]);
    }
  }
}

/* Tek blok sifrele (CCM sadece sifreleme yonunu kullanir) */
static void Aes_Encrypt(const uint8_t in[16], uint8_t out[16])
{
  uint8_t s[16];
  for (uint32_t i = 0; i < 16; i++) { s[i] = (uint8_t)(in[i] ^ aesRk[i]); }
  for (uint32_t r = 1; r <= 10; r++)
  {
    uint8_t t[16];
    /* SubBytes + ShiftRows */
    for (uint32_t c = 0; c < 4; c++)
    {
      for (uint32_t row = 0; row < 4; row++)
      {
        t[c * 4 + row] = aesSbox[s[((c + row) % 4) * 4 + row]];
      }
    }
    /* MixColumns (son turda yok) */
    if (r != 10)
    {
      for (uint32_t c = 0; c < 4; c++)
      {
        uint8_t a0 = t[c * 4], a1 = t[c * 4 + 1], a2 = t[c * 4 + 2], a3 = t[c * 4 + 3];
        uint8_t x = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
        t[c * 4]     = (uint8_t)(a0 ^ x ^ Aes_Xt((uint8_t)(a0 ^ a1)));
        t[c * 4 + 1] = (uint8_t)(a1 ^ x ^ Aes_Xt((uint8_t)(a1 ^ a2)));
        t[c * 4 + 2] = (uint8_t)(a2 ^ x ^ Aes_Xt((uint8_t)(a2 ^ a3)));
        t[c * 4 + 3] = (uint8_t)(a3 ^ x ^ Aes_Xt((uint8_t)(a3 ^ a0)));
      }
    }
    /* AddRoundKey */
    for (uint32_t i = 0; i < 16; i++) { s[i] = (uint8_t)(t[i] ^ aesRk[r * 16 + i]); }
  }
  memcpy(out, s, 16);
}

/* ------------------------------------------------- AES-128-CCM (M=4, L=2) */
#define CCM_NONCE_LEN   13
#define CCM_MIC_LEN     4

/* CBC-MAC hesabi: B0 | len(aad) | aad | pad | msg | pad  -> T (16 B) */
static void Ccm_Mac(const uint8_t nonce[13], const uint8_t *aad, uint32_t aadLen,
                    const uint8_t *msg, uint32_t msgLen, uint8_t T[16])
{
  uint8_t b[16];
  uint32_t i, n;
  /* B0: flags = Adata(0x40) | ((M-2)/2)<<3 | (L-1) */
  b[0] = (uint8_t)(((aadLen > 0) ? 0x40U : 0x00U) | (((CCM_MIC_LEN - 2) / 2) << 3) | 0x01U);
  memcpy(&b[1], nonce, 13);
  b[14] = (uint8_t)(msgLen >> 8);
  b[15] = (uint8_t)(msgLen & 0xFF);
  Aes_Encrypt(b, T);
  if (aadLen > 0)
  {
    /* ilk blok: 2 B uzunluk + aad'nin ilk 14 byte'i */
    memset(b, 0, 16);
    b[0] = (uint8_t)(aadLen >> 8);
    b[1] = (uint8_t)(aadLen & 0xFF);
    n = (aadLen < 14U) ? aadLen : 14U;
    memcpy(&b[2], aad, n);
    for (i = 0; i < 16; i++) { T[i] ^= b[i]; }
    Aes_Encrypt(T, T);
    for (uint32_t off = n; off < aadLen; off += 16)
    {
      memset(b, 0, 16);
      n = ((aadLen - off) < 16U) ? (aadLen - off) : 16U;
      memcpy(b, &aad[off], n);
      for (i = 0; i < 16; i++) { T[i] ^= b[i]; }
      Aes_Encrypt(T, T);
    }
  }
  for (uint32_t off = 0; off < msgLen; off += 16)
  {
    memset(b, 0, 16);
    n = ((msgLen - off) < 16U) ? (msgLen - off) : 16U;
    memcpy(b, &msg[off], n);
    for (i = 0; i < 16; i++) { T[i] ^= b[i]; }
    Aes_Encrypt(T, T);
  }
}

/* CTR akisi: A_i = (L-1) | nonce | i (2 B, big endian). ctr=0 MIC icin, veri 1'den baslar */
static void Ccm_Ctr(const uint8_t nonce[13], uint16_t ctr, uint8_t S[16])
{
  uint8_t a[16];
  a[0] = 0x01U;
  memcpy(&a[1], nonce, 13);
  a[14] = (uint8_t)(ctr >> 8);
  a[15] = (uint8_t)(ctr & 0xFF);
  Aes_Encrypt(a, S);
}

static void Ccm_Crypt(const uint8_t nonce[13], const uint8_t *in, uint8_t *out, uint32_t len)
{
  uint8_t S[16];
  for (uint32_t off = 0; off < len; off += 16)
  {
    Ccm_Ctr(nonce, (uint16_t)(1U + off / 16U), S);
    uint32_t n = ((len - off) < 16U) ? (len - off) : 16U;
    for (uint32_t i = 0; i < n; i++) { out[off + i] = (uint8_t)(in[off + i] ^ S[i]); }
  }
}

/* Sifrele + MIC: out'a len byte sifreli veri, mic'e 4 B (in ile out ayni tampon olabilir) */
static void Ccm_Encrypt(const uint8_t nonce[13], const uint8_t *aad, uint32_t aadLen,
                        const uint8_t *in, uint8_t *out, uint32_t len, uint8_t mic[4])
{
  uint8_t T[16], S0[16];
  Ccm_Mac(nonce, aad, aadLen, in, len, T);
  Ccm_Ctr(nonce, 0, S0);
  for (uint32_t i = 0; i < CCM_MIC_LEN; i++) { mic[i] = (uint8_t)(T[i] ^ S0[i]); }
  Ccm_Crypt(nonce, in, out, len);
}

/* Coz + dogrula: MIC tutarsa true. Tutmazsa out'un icerigi gecersizdir. */
static bool Ccm_Decrypt(const uint8_t nonce[13], const uint8_t *aad, uint32_t aadLen,
                        const uint8_t *in, uint8_t *out, uint32_t len, const uint8_t mic[4])
{
  uint8_t T[16], S0[16];
  uint8_t diff = 0;
  Ccm_Crypt(nonce, in, out, len);
  Ccm_Mac(nonce, aad, aadLen, out, len, T);
  Ccm_Ctr(nonce, 0, S0);
  for (uint32_t i = 0; i < CCM_MIC_LEN; i++) { diff |= (uint8_t)((T[i] ^ S0[i]) ^ mic[i]); }
  return diff == 0;
}

#endif /* __LORA_SECURE_H__ */
