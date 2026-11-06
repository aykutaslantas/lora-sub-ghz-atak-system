/**
  ******************************************************************************
  * @file    net_key.example.h
  * @brief   Ag anahtari sablonu (AES-128, Faz 1: tum node'lar + Zybo ayni anahtar).
  *          net_key.h olarak kopyalayin:  cp net_key.example.h net_key.h
  *          net_key.h git'e EKLENMEZ (.gitignore). Zybo'daki anahtarla AYNI olmali.
  *          Asagidaki deger SADECE TEST anahtaridir (docs/protocol.md test vektoru);
  *          sahada kullanmadan once iki tarafta da degistirin.
  ******************************************************************************
  */
#ifndef __NET_KEY_H__
#define __NET_KEY_H__

#define NET_KEY_BYTES  { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, \
                         0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F }

#endif /* __NET_KEY_H__ */
