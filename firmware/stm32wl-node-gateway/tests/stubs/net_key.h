/**
  ******************************************************************************
  * @file    net_key.h
  * @brief   Ag anahtari (AES-128, Faz 1: tum node'lar + Zybo ayni anahtar).
  *          !!! Bu dosya git'e EKLENMEZ (.gitignore). Zybo'daki anahtarla AYNI olmali. !!!
  *          Asagidaki deger SADECE TEST anahtaridir (testler bu anahtarla calisir);
  *          sahada kullanmadan once iki tarafta da degistirin.
  ******************************************************************************
  */
#ifndef __NET_KEY_H__
#define __NET_KEY_H__

#define NET_KEY_BYTES  { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, \
                         0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F }

#endif /* __NET_KEY_H__ */
