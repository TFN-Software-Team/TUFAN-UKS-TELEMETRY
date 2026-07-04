#ifndef E22_REGS_H
#define E22_REGS_H

#include <stdint.h>

/* =========================================================================
 * EBYTE E22-400T30D-V2 (SX1268) — Register Sozlesmesi
 *
 *  TEK DOGRULUK KAYNAGI: Bu dosyadaki adres/deger/komut sabitleri disinda
 *  hicbir yerde (lora.c, main.c) register adresi/hedef degeri hardcode
 *  EDILMEMELI. Adres haritasi yanlissa (bench dump datasheet ile
 *  celisirse) TEK degisiklik noktasi burasi olmali.
 *
 *  KAYNAK: Ekip ici "ORTAK BLOK — E22-400T30D-V2 Konfigurasyon Sozlesmesi"
 *  (AKS ve UKS arasinda paylasilan, iki tarafta birebir ayni olmasi
 *  gereken register hedef degerleri).
 *
 *  DOGRULAMA NOTU (baglayici, sozlesmeden): E22'nin V1 ve V2
 *  firmware'lerinde register haritasi FARKLIDIR (V2'de NETID eklendi,
 *  sonraki adresler kaydi). Asagidaki harita V2 VARSAYIMIDIR ve HENUZ
 *  bench'te dogrulanmadi. Lora_Init() her boot'ta C1 ile TUM bloğu okuyup
 *  hex dump basar; bu dump elimizdeki modulun gercek davranisiyla
 *  karsilastirilip adres sabitleri (yalniz burada) duzeltilecek.
 *
 *  DATASHEET CELISKISI: Bu implementasyon sirasinda erisilen genel EBYTE
 *  E22 dokumantasyon bilgisi asagidaki REG0/REG1 bit yerlesimiyle
 *  CELISMEDI (ayni encoding E32'nin SPED alaniyla da tutarli: UART baud
 *  ve hava hizi ayni bit-alani sirasiyla kodlaniyor). Bulunabilen ek bir
 *  celiski yok; yine de bench dump nihai hakemdir.
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * Register ADRESLERI (V2 haritasi — bench dump ile teyit edilecek)
 * ------------------------------------------------------------------------- */
#define E22_REG_ADDH        0x00U   /* V2 haritasi — bench dump ile teyit edilecek */
#define E22_REG_ADDL        0x01U   /* V2 haritasi — bench dump ile teyit edilecek */
#define E22_REG_NETID       0x02U   /* V2'de eklendi (V1'de yok) — bench dump ile teyit edilecek */
#define E22_REG_REG0        0x03U   /* V2 haritasi — bench dump ile teyit edilecek: UART/hava hizi */
#define E22_REG_REG1        0x04U   /* V2 haritasi — bench dump ile teyit edilecek: alt-paket/RSSI/guc */
#define E22_REG_REG2        0x05U   /* V2 haritasi — bench dump ile teyit edilecek: kanal */
#define E22_REG_REG3        0x06U   /* V2 haritasi — bench dump ile teyit edilecek: RSSI-byte/mod/LBT */
#define E22_REG_CRYPT_H     0x07U   /* V2 haritasi — bench dump ile teyit edilecek; GERI OKUNAMAZ */
#define E22_REG_CRYPT_L     0x08U   /* V2 haritasi — bench dump ile teyit edilecek; GERI OKUNAMAZ */

/** Config blogunun basi ve toplam uzunlugu (ADDH..CRYPT_L, 9 byte). */
#define E22_REG_BLOCK_START E22_REG_ADDH
#define E22_REG_BLOCK_LEN   9U

/** CRYPT_H/CRYPT_L GERI OKUNAMAZ (okuma her zaman 0 doner) — bu yuzden
 *  dogrulama/karsilastirma kapsami disinda tutulur; yalnizca ADDH..REG3
 *  (7 byte) okunup hedefle karsilastirilir. */
#define E22_REG_VERIFY_LEN  7U   /* ADDH..REG3 */

/* -------------------------------------------------------------------------
 * Hedef DEGERLER (sozlesme — AKS ile birebir AYNI olmali)
 * ------------------------------------------------------------------------- */
#define E22_VAL_ADDH        0x00U
#define E22_VAL_ADDL        0x00U   /* adres 0x0000: hedefli degil, genel/broadcast */
#define E22_VAL_NETID       0x00U

/* REG0 = 0x64 = 0110 0100b
 *   bit[7:5] = 011 -> UART 9600 baud (STM32 USART2 ile AYNI olmali)
 *   bit[4:3] = 00  -> 8N1, parity yok
 *   bit[2:0] = 100 -> hava hizi 9.6 kbps (AKS ile AYNI olmak ZORUNDA —
 *                     fiziksel RF parametresi, yerel degil) */
#define E22_VAL_REG0        0x64U

/* REG1 = 0x00 = 0000 0000b
 *   bit[7:6] = 00 -> alt-paket 240 B
 *   bit[5]   = 0  -> RSSI ortam gurultusu KAPALI
 *   bit[1:0] = 00 -> TX gucu EN YUKSEK (T30D icin 30 dBm)
 *   DIKKAT: E22'de de 00=maks, 11=min — E32'deki guc-biti karisikligi
 *   (bkz. eski lora.h OPTION notu) burada TEKRARLANMADI. */
#define E22_VAL_REG1        0x00U

/* REG2 = 0x17 = kanal 23 -> frekans = 410.125 + 23 = 433.125 MHz
 *   (433 ISM bandi 433.05-434.79 icinde) */
#define E22_VAL_REG2        0x17U

/* REG3 = 0x00 = 0000 0000b
 *   bit[7] = 0 -> RSSI byte'i EKLENMEZ (KRITIK, asagiya bak)
 *   bit[6] = 0 -> transparan mod
 *   bit[5] = 0 -> röle kapali
 *   bit[4] = 0 -> LBT kapali
 *   WOR bitleri kullanilmiyor
 *
 *   KRITIK — RSSI byte'i (bit7) KAPALI KALMALI: acilirsa E22 her alinan
 *   paketin sonuna 1 byte RSSI ekler; UKS satir-parser'i (telemetry.c) ve
 *   AKS komut/heartbeat RX'i bu byte'i veri sanir. Link kalitesi ileride
 *   istenirse ayri, koordineli bir is olarak yapilir. */
#define E22_VAL_REG3        0x00U

#define E22_VAL_CRYPT_H     0x00U   /* yazilir ama GERI OKUNAMAZ (bkz. yukari) */
#define E22_VAL_CRYPT_L     0x00U   /* yazilir ama GERI OKUNAMAZ (bkz. yukari) */

/** ADDH..REG3 sirasiyla hedef degerler — E22_ReadRegs sonucuyla
 *  karsilastirmak icin. CRYPT bilerek DAHIL DEGIL (bkz. E22_REG_VERIFY_LEN). */
#define E22_TARGET_BLOCK_INIT { \
        E22_VAL_ADDH, E22_VAL_ADDL, E22_VAL_NETID, \
        E22_VAL_REG0, E22_VAL_REG1, E22_VAL_REG2, E22_VAL_REG3 }

/** Flash'a kalici yazilacak TAM blok (CRYPT dahil, 9 byte). */
#define E22_WRITE_BLOCK_INIT { \
        E22_VAL_ADDH, E22_VAL_ADDL, E22_VAL_NETID, \
        E22_VAL_REG0, E22_VAL_REG1, E22_VAL_REG2, E22_VAL_REG3, \
        E22_VAL_CRYPT_H, E22_VAL_CRYPT_L }

/* -------------------------------------------------------------------------
 * Komut sabitleri
 * ------------------------------------------------------------------------- */
#define E22_CMD_WRITE_SAVED 0xC0U   /* Kalici (flash) yazma: C0 addr len vals... */
#define E22_CMD_READ        0xC1U   /* Secmeli okuma: C1 addr len -> C1 addr len data... */
#define E22_CMD_WRITE_TEMP  0xC2U   /* Kalici OLMAYAN (RAM) yazma — bu surucude KULLANILMIYOR */

/** Hatali/format-disi komutta modulun dondugu 3 byte. */
static const uint8_t E22_RSP_BAD[3] = { 0xFFU, 0xFFU, 0xFFU };

/* -------------------------------------------------------------------------
 * Mod pin seviyeleri (M0/M1) — E32'DEN FARKLI!
 *
 *   E32: Normal M0=0,M1=0 | Config M0=1,M1=1
 *   E22: Normal M0=0,M1=0 (AYNI) | Config M0=0,M1=1 (FARKLI — E32'de bu WOR
 *        modu idi; E22'de config modu)
 * ------------------------------------------------------------------------- */
#define E22_MODE_NORMAL_M0  0U
#define E22_MODE_NORMAL_M1  0U
#define E22_MODE_CONFIG_M0  0U
#define E22_MODE_CONFIG_M1  1U

/* -------------------------------------------------------------------------
 * AUX bekleme zaman asimi (ms) — E32 ile ayni marjlar korunuyor
 *
 *  BOOT : Modul guc acilisinda AUX'u LOW tutar. 3000 ms genis marj.
 *  MODE : M0/M1 degisiminden sonra AUX HIGH'a gelme suresi.
 *  CFG  : Flash yazma (C0) sonrasi AUX HIGH bekleme suresi.
 * ------------------------------------------------------------------------- */
#define E22_AUX_BOOT_TIMEOUT_MS   3000U
#define E22_AUX_MODE_TIMEOUT_MS    500U
#define E22_AUX_CFG_TIMEOUT_MS    2000U

/* -------------------------------------------------------------------------
 * Boot-log decode yardimcilari — TEK yerden turetilir (main.c bunlari
 * dogrudan cagirir; hicbir yerde ayrica baud/hiz/frekans hardcode edilmez).
 * ------------------------------------------------------------------------- */

/** REG0 bit[7:5] -> UART baud (bps). */
static inline uint32_t E22_DecodeUartBaud(uint8_t reg0)
{
    static const uint32_t table[8] = {
        1200U, 2400U, 4800U, 9600U, 19200U, 38400U, 57600U, 115200U
    };
    return table[(reg0 >> 5) & 0x07U];
}

/** REG0 bit[2:0] -> hava hizi (bps). */
static inline uint32_t E22_DecodeAirRateBps(uint8_t reg0)
{
    static const uint32_t table[8] = {
        300U, 1200U, 2400U, 4800U, 9600U, 19200U, 38400U, 62500U
    };
    return table[reg0 & 0x07U];
}

/** REG1 bit[1:0] -> TX guc kademesi (0..3). 0 = EN YUKSEK (T30D: 30 dBm).
 *  Diger kademelerin (1..3) dBm karsiligi sozlesmede verilmedi; bench
 *  dump / datasheet ile teyit edilmeden sayisal dBm iddia EDILMEZ. */
static inline uint8_t E22_DecodeTxPowerStep(uint8_t reg1)
{
    return (uint8_t)(reg1 & 0x03U);
}

/** REG2 (kanal) -> frekans, MHz*1000 cinsinden tam sayi (ondalik kayip
 *  olmadan). freq_mhz_x1000 / 1000 = MHz, % 1000 = kalan (.125 -> 125). */
static inline uint32_t E22_DecodeFreqMhzX1000(uint8_t reg2)
{
    return 410125U + ((uint32_t)reg2 * 1000U);
}

#endif /* E22_REGS_H */
