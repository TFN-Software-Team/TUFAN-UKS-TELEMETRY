/**
 * @file    lora.c
 * @brief   EBYTE E22-400T30D-V2 (SX1268) LoRa modulu surucusu (STM32 HAL).
 *
 *  E32 -> E22 MIGRASYONU:
 *  Donanim pin-uyumlu (M0/M1/RXD/TXD/AUX/VCC/GND) ama konfigurasyon
 *  protokolu TAMAMEN farkli:
 *    - Config-modu pin seviyeleri: E32 M0=1,M1=1 idi; E22'de M0=0,M1=1.
 *    - E32'de sabit 6-byte'lik C0 blogu (ADDH,ADDL,SPED,CHAN,OPTION)
 *      yazilirdi ve modul GONDERILEN BYTE'LARI AYNEN echo'lardi. E22'de
 *      komutlar register-adres/uzunluk tabanlidir (C0/C1 addr len ...)
 *      ve yanit basligi HER ZAMAN C1'dir (echo degil).
 *    - Hata sinyali: E22 gecersiz/reddedilen komutta 3 byte FF FF FF
 *      doner — bu net bir hata sinyalidir (E32'deki "echo gelmedi ama
 *      yine de LORA_OK don, RF test edilsin" toleransi E22'de KORUNMADI,
 *      bkz. E22_ReadRegs/E22_WriteRegsSaved).
 *  Register adresleri/hedef degerleri e22_regs.h icinde (TEK dogruluk
 *  kaynagi); burada hardcode edilmez.
 *
 *  TX/RX veri duzlemi (transparan mod, IT-RX mimarisi, rx_active
 *  watchdog) E32 ile birebir AYNI kaldi — bu katman (Lora_Send,
 *  Lora_StartReceive, Lora_OnUartRxCplt/Error) DEGISMEDI, yalnizca
 *  isim/yorumlar E22'ye guncellendi.
 *
 *  Korunan E32-donemi duzeltmeleri (davranis olarak hala gecerli):
 *  FIX-4 (KRITIK): Half-duplex TX/RX cakismasi. USART2 uzerinde
 *         HAL_UART_Receive_IT ile byte-byte RX surerken dogrudan
 *         HAL_UART_Transmit cagrilirsa HAL __HAL_LOCK yuzunden TX
 *         HAL_BUSY donebilir ya da devam eden RX bozulur. Cozum:
 *         Lora_Send icinde TX oncesi IT-RX abort, TX, ardindan IT-RX
 *         yeniden arm. rx_active bayragi ile takip.
 *
 *  9.2.a: UKS -> AKS komut kanali (eski Lora_SendCritical, acil durdurma
 *  komutu gonderimi icin kullanilirdi) sistemden tamamen kaldirildi. UKS ->
 *  AKS yonunde yalnizca 0xB0 heartbeat'i (Lora_Send ile, best-effort)
 *  gonderilir.
 */

#include "lora.h"
#include <stdio.h>
#include <string.h>

/* =========================================================================
 * Yardimci: AUX HIGH bekle
 * ========================================================================= */
static LoraStatus_t E22_WaitAuxHigh(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while (HAL_GPIO_ReadPin(LORA_AUX_GPIO_Port, LORA_AUX_Pin) == GPIO_PIN_RESET)
    {
        if ((HAL_GetTick() - start) >= timeout_ms)
            return LORA_ERR_TIMEOUT;
    }
    return LORA_OK;
}

/* =========================================================================
 * GPIO init:
 *   PB6 (M0)  -> push-pull cikis, LOW  (boot'ta normal mod)
 *   PB7 (M1)  -> push-pull cikis, LOW  (boot'ta normal mod)
 *   PB10 (AUX)-> input, pull-up        (E22 tarafindan surulur)
 * ========================================================================= */
static void E22_GPIO_Init(void)
{
    GPIO_InitTypeDef g = {0};

    /* Once LOW yaz, sonra configure et — pin glitch onleme */
    HAL_GPIO_WritePin(GPIOB, E22_M0_Pin | E22_M1_Pin, GPIO_PIN_RESET);

    g.Pin   = E22_M0_Pin | E22_M1_Pin;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &g);

    /* Configure sonrasi tekrar LOW garantile */
    HAL_GPIO_WritePin(GPIOB, E22_M0_Pin | E22_M1_Pin, GPIO_PIN_RESET);

    /* AUX: acik-kolektor surucudur; pull-up zorunlu */
    g.Pin  = LORA_AUX_Pin;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(LORA_AUX_GPIO_Port, &g);
}

static inline void E22_SetMode(uint8_t m0, uint8_t m1)
{
    HAL_GPIO_WritePin(GPIOB, E22_M0_Pin,
                       m0 ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, E22_M1_Pin,
                       m1 ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* =========================================================================
 * Normal mod: M0=0, M1=0 (E32 ile AYNI)
 * ========================================================================= */
static LoraStatus_t E22_EnterNormalMode(void)
{
    E22_SetMode(E22_MODE_NORMAL_M0, E22_MODE_NORMAL_M1);
    HAL_Delay(2);
    return E22_WaitAuxHigh(E22_AUX_MODE_TIMEOUT_MS);
}

/* =========================================================================
 * Config modu: M0=0, M1=1 — E32'DEN FARKLI (E32'de M0=1,M1=1 idi; bu
 * seviye E32'de WOR modu anlamina gelirdi, E22'de config modudur).
 * ========================================================================= */
static LoraStatus_t E22_EnterConfigMode(UART_HandleTypeDef *huart)
{
    /* RX tamponunu temizle — config komutuna yanit olarak gelen
     * artik baytlari engelle */
    __HAL_UART_CLEAR_OREFLAG(huart);

    E22_SetMode(E22_MODE_CONFIG_M0, E22_MODE_CONFIG_M1);
    HAL_Delay(2);
    return E22_WaitAuxHigh(E22_AUX_MODE_TIMEOUT_MS);
}

/* =========================================================================
 * E22_ReadRegs — "C1 addr len" gonderir, "C1 addr len data..." yanitini
 * dogrular.
 *
 *  Baslik (ilk 3 byte) her zaman ONCE okunur: normal yanitta bu C1/addr/len
 *  aynasidir, hata yanitinda ise MODUL SADECE 3 byte FF FF FF doner — ayni
 *  3-byte'lik blocking okuma ile ikisi de yakalanir, boylece "kac byte
 *  gelecegi onceden bilinmiyor" sorunu cozulur. Baslik gecerliyse (ve hata
 *  degilse) kalan `len` veri byte'i ikinci bir okumayla alinir.
 *
 * @retval LORA_OK          basari, buf'a len byte yazildi
 *         LORA_ERR_TIMEOUT UART yaniti gelmedi
 *         LORA_ERR         FF FF FF (modul reddetti) ya da beklenmeyen baslik
 * ========================================================================= */
static LoraStatus_t E22_ReadRegs(UART_HandleTypeDef *huart, uint8_t addr,
                                 uint8_t len, uint8_t *buf)
{
    const uint8_t cmd[3] = { E22_CMD_READ, addr, len };

    __HAL_UART_CLEAR_OREFLAG(huart);
    if (HAL_UART_Transmit(huart, (uint8_t *)cmd, sizeof(cmd), 200U) != HAL_OK)
        return LORA_ERR;

    uint8_t hdr[3] = {0};
    if (HAL_UART_Receive(huart, hdr, sizeof(hdr), 500U) != HAL_OK)
    {
        HAL_UART_Abort(huart);
        __HAL_UART_CLEAR_OREFLAG(huart);
        return LORA_ERR_TIMEOUT;
    }

    if (memcmp(hdr, E22_RSP_BAD, sizeof(hdr)) == 0)
        return LORA_ERR;   /* FF FF FF: modul komutu reddetti */

    if (hdr[0] != E22_CMD_READ || hdr[1] != addr || hdr[2] != len)
        return LORA_ERR;   /* beklenmeyen baslik */

    if (len == 0U) return LORA_OK;

    if (HAL_UART_Receive(huart, buf, len, 500U) != HAL_OK)
    {
        HAL_UART_Abort(huart);
        __HAL_UART_CLEAR_OREFLAG(huart);
        return LORA_ERR_TIMEOUT;
    }
    return LORA_OK;
}

/* =========================================================================
 * E22_WriteRegsSaved — "C0 addr len vals..." gonderir (kalici/flash
 * yazma), "C1 addr len vals" yanitini dogrular.
 *
 *  E32'deki "gonderilen byte'larin aynen echo'su" mantigi burada
 *  GECERSIZDIR — E22'de yanit basligi (ve komut byte'i) HER ZAMAN C1'dir,
 *  C0 DEGIL.
 *
 *  CRYPT (E22_REG_CRYPT_H/L) alanlari yazilir AMA geri OKUNAMAZ; bu yuzden
 *  dogrulama karsilastirmasindan (yaniti vals ile eslestirme) CRYPT
 *  adresleri HARIC TUTULUR — aksi halde her yazim, gecerli olsa bile
 *  CRYPT uyumsuzlugu yuzunden LORA_ERR donerdi.
 * ========================================================================= */
static LoraStatus_t E22_WriteRegsSaved(UART_HandleTypeDef *huart, uint8_t addr,
                                       uint8_t len, const uint8_t *vals)
{
    if (len == 0U || len > E22_REG_BLOCK_LEN) return LORA_ERR;

    uint8_t cmd[3U + E22_REG_BLOCK_LEN];
    cmd[0] = E22_CMD_WRITE_SAVED;
    cmd[1] = addr;
    cmd[2] = len;
    memcpy(&cmd[3], vals, len);

    __HAL_UART_CLEAR_OREFLAG(huart);
    if (HAL_UART_Transmit(huart, cmd, (uint16_t)(3U + len), 200U) != HAL_OK)
        return LORA_ERR;

    /* Flash yazma tamamlansin (AUX LOW->HIGH) */
    if (E22_WaitAuxHigh(E22_AUX_CFG_TIMEOUT_MS) != LORA_OK)
        return LORA_ERR_TIMEOUT;

    uint8_t hdr[3] = {0};
    if (HAL_UART_Receive(huart, hdr, sizeof(hdr), 500U) != HAL_OK)
    {
        HAL_UART_Abort(huart);
        __HAL_UART_CLEAR_OREFLAG(huart);
        return LORA_ERR_TIMEOUT;
    }

    if (memcmp(hdr, E22_RSP_BAD, sizeof(hdr)) == 0)
        return LORA_ERR;   /* FF FF FF: modul yazmayi reddetti */

    /* Yanit basligi C1'dir (C0 DEGIL) — bkz. yukaridaki fonksiyon yorumu */
    if (hdr[0] != E22_CMD_READ || hdr[1] != addr || hdr[2] != len)
        return LORA_ERR;

    uint8_t resp_vals[E22_REG_BLOCK_LEN] = {0};
    if (HAL_UART_Receive(huart, resp_vals, len, 500U) != HAL_OK)
    {
        HAL_UART_Abort(huart);
        __HAL_UART_CLEAR_OREFLAG(huart);
        return LORA_ERR_TIMEOUT;
    }

    for (uint8_t i = 0U; i < len; i++)
    {
        uint8_t reg_addr = (uint8_t)(addr + i);
        if (reg_addr == E22_REG_CRYPT_H || reg_addr == E22_REG_CRYPT_L)
            continue;   /* geri okunamaz — dogrulama disi */
        if (resp_vals[i] != vals[i])
            return LORA_ERR;
    }
    return LORA_OK;
}

/* =========================================================================
 * E22_WriteRegsTemp — "C2 addr len vals..." gonderir (KALICI OLMAYAN/RAM
 * yazma — guc kesilince silinir), "C1 addr len vals" yanitini bekler.
 *
 *  G7-FIX-2: Read-before-write skip yolu (Lora_Init, needs_write==0 dali)
 *  CRYPT'e hicbir zaman dokunmuyordu — CRYPT geri okunamadigi icin
 *  karsilastirmaya girmiyor (bkz. E22_REG_VERIFY_LEN) ve REG0-3 hedefleri
 *  degismedigi surece bu dal her boot'ta tetiklenir. Sonuc: CRYPT hedefi
 *  git'te degistirilse bile (bkz. E22_CRYPT_SENKRON.md "G7-FIX-2"),
 *  flash'taki gercek anahtar ilk basarili tam yazimdan beri hic
 *  degismemis olabilir. Bu fonksiyon skip dalinda her boot'ta CRYPT'i
 *  C2 (RAM/gecici) ile yeniden yazarak bu kor noktayi kapatir — flash
 *  omrunu etkilemez (kalici DEGIL) ve ADDH..REG3'e DOKUNMAZ.
 *
 *  CRYPT geri okunamadigi icin yanitta deger karsilastirmasi YAPILMAZ;
 *  yalnizca baslik (C1 addr len) ve FF FF FF hata cercevesi kontrol
 *  edilir, kalan deger byte'lari UART hatti temiz kalsin diye tuketilir.
 * ========================================================================= */
static LoraStatus_t E22_WriteRegsTemp(UART_HandleTypeDef *huart, uint8_t addr,
                                      uint8_t len, const uint8_t *vals)
{
    if (len == 0U || len > E22_REG_BLOCK_LEN) return LORA_ERR;

    uint8_t cmd[3U + E22_REG_BLOCK_LEN];
    cmd[0] = E22_CMD_WRITE_TEMP;
    cmd[1] = addr;
    cmd[2] = len;
    memcpy(&cmd[3], vals, len);

    __HAL_UART_CLEAR_OREFLAG(huart);
    if (HAL_UART_Transmit(huart, cmd, (uint16_t)(3U + len), 200U) != HAL_OK)
        return LORA_ERR;

    /* RAM yazma flash'a gitmez ama modul yine de AUX ile "mesgul" sinyali
     * verir; kalici yazimla ayni bekleme marjini kullanilir. */
    if (E22_WaitAuxHigh(E22_AUX_CFG_TIMEOUT_MS) != LORA_OK)
        return LORA_ERR_TIMEOUT;

    uint8_t hdr[3] = {0};
    if (HAL_UART_Receive(huart, hdr, sizeof(hdr), 500U) != HAL_OK)
    {
        HAL_UART_Abort(huart);
        __HAL_UART_CLEAR_OREFLAG(huart);
        return LORA_ERR_TIMEOUT;
    }

    if (memcmp(hdr, E22_RSP_BAD, sizeof(hdr)) == 0)
        return LORA_ERR;   /* FF FF FF: modul yazmayi reddetti */

    /* Yanit basligi C1'dir (C0/C2 DEGIL) — bkz. E22_WriteRegsSaved yorumu */
    if (hdr[0] != E22_CMD_READ || hdr[1] != addr || hdr[2] != len)
        return LORA_ERR;

    uint8_t resp_vals[E22_REG_BLOCK_LEN] = {0};
    if (HAL_UART_Receive(huart, resp_vals, len, 500U) != HAL_OK)
    {
        HAL_UART_Abort(huart);
        __HAL_UART_CLEAR_OREFLAG(huart);
        return LORA_ERR_TIMEOUT;
    }

    return LORA_OK;   /* deger karsilastirmasi yok — CRYPT geri okunamaz */
}

/* =========================================================================
 * Boot-log: register blogunu hex dump'la (bench dump teyidi icin — HER
 * boot'ta, yazma yapilsa da yapilmasa da basilir).
 * ========================================================================= */
static void E22_DumpBlock(const char *baslik, const uint8_t *blk, uint8_t len)
{
    printf("[CFG] %s:\r\n", baslik);
    for (uint8_t i = 0U; i < len; i++)
        printf("E22REG,0x%02X,0x%02X\r\n",
               (unsigned)(E22_REG_BLOCK_START + i), (unsigned)blk[i]);
}

/* =========================================================================
 * Lora_Init — public API
 *
 *  1. GPIO: M0/M1 -> output LOW (normal mod), AUX -> input pull-up
 *  2. E22 boot: AUX HIGH bekle
 *  3. Config moduna gir (M0=0, M1=1)
 *  4. TUM register blogunu (ADDH..CRYPT_L) C1 ile oku, hex dump'la
 *  5. ADDH..REG3 (CRYPT haric — geri okunamaz) hedefle ayniysa YAZMA
 *     (flash omru); farkliysa C0 ile yaz, yaniti dogrula, tekrar oku +
 *     dump'la
 *  6. Normal moda don
 * ========================================================================= */
LoraStatus_t Lora_Init(LoraCtx_t *ctx, UART_HandleTypeDef *huart)
{
    if (!ctx || !huart) return LORA_ERR;

    ctx->huart     = huart;
    ctx->rx_cb     = NULL;
    ctx->rx_user   = NULL;
    ctx->rx_active = 0U;          /* FIX-4: henuz RX baslamadi */

    /* 1. GPIO hazirla */
    E22_GPIO_Init();

    /* AUX TANI: boot oncesi pin durumunu goster.
     * Eger "AUX zaman asimi" mesaji cikiyorsa AUX floating olabilir. */
    {
        uint8_t aux_now = (HAL_GPIO_ReadPin(LORA_AUX_GPIO_Port, LORA_AUX_Pin) == GPIO_PIN_SET) ? 1U : 0U;
        printf("[AUX] Boot oncesi AUX=%u  M0=%u  M1=%u\r\n",
               aux_now,
               (HAL_GPIO_ReadPin(GPIOB, E22_M0_Pin) == GPIO_PIN_SET) ? 1U : 0U,
               (HAL_GPIO_ReadPin(GPIOB, E22_M1_Pin) == GPIO_PIN_SET) ? 1U : 0U);
    }

    /* 2. Modul boot AUX HIGH bekle */
    if (E22_WaitAuxHigh(E22_AUX_BOOT_TIMEOUT_MS) != LORA_OK)
        return LORA_ERR_TIMEOUT;

    /* 3. Config moduna gir */
    if (E22_EnterConfigMode(huart) != LORA_OK)
        return LORA_ERR_TIMEOUT;

    /* Config modu sonrasi pin durumu */
    {
        uint8_t aux_now = (HAL_GPIO_ReadPin(LORA_AUX_GPIO_Port, LORA_AUX_Pin) == GPIO_PIN_SET) ? 1U : 0U;
        printf("[AUX] Config modunda AUX=%u  M0=%u  M1=%u\r\n",
               aux_now,
               (HAL_GPIO_ReadPin(GPIOB, E22_M0_Pin) == GPIO_PIN_SET) ? 1U : 0U,
               (HAL_GPIO_ReadPin(GPIOB, E22_M1_Pin) == GPIO_PIN_SET) ? 1U : 0U);
    }

    /* 4. Read-before-write: TUM blogu oku ve dump'la */
    uint8_t cur[E22_REG_BLOCK_LEN] = {0};
    LoraStatus_t read_st = E22_ReadRegs(huart, E22_REG_BLOCK_START,
                                        E22_REG_BLOCK_LEN, cur);
    LoraStatus_t cfg_st;

    if (read_st != LORA_OK)
    {
        printf("[CFG] Blok okunamadi (st=%d) — yazma denenecek.\r\n", (int)read_st);

        const uint8_t target_full[E22_REG_BLOCK_LEN] = E22_WRITE_BLOCK_INIT;
        cfg_st = E22_WriteRegsSaved(huart, E22_REG_BLOCK_START,
                                    E22_REG_BLOCK_LEN, target_full);

        if (cfg_st == LORA_OK)
        {
            uint8_t after[E22_REG_BLOCK_LEN] = {0};
            if (E22_ReadRegs(huart, E22_REG_BLOCK_START, E22_REG_BLOCK_LEN, after) == LORA_OK)
                E22_DumpBlock("Yazma sonrasi", after, E22_REG_BLOCK_LEN);
        }
    }
    else
    {
        E22_DumpBlock("Mevcut ayarlar (yazma oncesi)", cur, E22_REG_BLOCK_LEN);

        const uint8_t target_verify[E22_REG_VERIFY_LEN] = E22_TARGET_BLOCK_INIT;
        uint8_t needs_write = 0U;
        for (uint8_t i = 0U; i < E22_REG_VERIFY_LEN; i++)
        {
            if (cur[i] != target_verify[i]) { needs_write = 1U; break; }
        }

        if (!needs_write)
        {
            /* Read-before-write: ADDH..REG3 zaten hedefle ayni -> kalici
             * (C0) YAZMA ATLANDI (flash omru). CRYPT geri okunamadigi
             * icin karsilastirma disi (bkz. E22_REG_VERIFY_LEN).
             *
             * G7-FIX-2: bu yuzden CRYPT hedefi git'te degistirilse bile
             * flash'taki gercek anahtar guncellenmiyor olabilirdi (bkz.
             * E22_CRYPT_SENKRON.md "G7-FIX-2"). Her boot'ta CRYPT'i
             * C2/RAM (kalici olmayan) ile yeniden yazarak bu kor noktayi
             * kapatiyoruz — best-effort, hata boot'u durdurmaz. */
            printf("[CFG] Tum alanlar (ADDH..REG3) hedefle ayni — kalici (C0) YAZMA ATLANDI (flash omru).\r\n");

            const uint8_t crypt_vals[2] = { E22_VAL_CRYPT_H, E22_VAL_CRYPT_L };
            if (E22_WriteRegsTemp(huart, E22_REG_CRYPT_H, 2U, crypt_vals) == LORA_OK)
            {
                printf("[CFG] CRYPT (0x5A3C) gecici (C2/RAM) yazildi.\r\n");
            }
            else
            {
                printf("[CFG] UYARI: CRYPT gecici (C2/RAM) yazma basarisiz — link calismayabilir.\r\n");
            }

            cfg_st = LORA_OK;
        }
        else
        {
            printf("[CFG] En az bir alan hedeften farkli — flash'a yaziliyor.\r\n");
            const uint8_t target_full[E22_REG_BLOCK_LEN] = E22_WRITE_BLOCK_INIT;
            cfg_st = E22_WriteRegsSaved(huart, E22_REG_BLOCK_START,
                                        E22_REG_BLOCK_LEN, target_full);

            if (cfg_st == LORA_OK)
            {
                uint8_t after[E22_REG_BLOCK_LEN] = {0};
                if (E22_ReadRegs(huart, E22_REG_BLOCK_START, E22_REG_BLOCK_LEN, after) == LORA_OK)
                    E22_DumpBlock("Yazma sonrasi", after, E22_REG_BLOCK_LEN);
            }
            else
            {
                printf("[CFG] Yazma/dogrulama basarisiz (st=%d).\r\n", (int)cfg_st);
            }
        }
    }

    /* 5. Normal moda gecmeden once UART'i sifirla.
     * Yukaridaki tum bloklayan HAL_UART_Receive cagrilari timeout'la
     * bitmis olabilir ve donanim/HAL state'inde artik veri, ORE/NE/FE
     * bayraklari veya BUSY_RX birakmis olabilir. Bunlar temizlenmezse
     * Lora_StartReceive arm edilse bile IT-RX sagir kalir -> rx_byte=0. */
    HAL_UART_Abort(huart);
    __HAL_UART_CLEAR_OREFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);
    __HAL_UART_CLEAR_FEFLAG(huart);

    /* 6. Hata olsa da normal moda don — modul calismali */
    if (E22_EnterNormalMode() != LORA_OK)
        return LORA_ERR_TIMEOUT;

    /* Normal moda geçiş sonrasi AUX stabilizasyonu */
    HAL_Delay(50U);

    /* 7. KRITIK: Modul Config->Normal gecisinde UART TX hattina anlik glitch
     * veya anlamsiz startup byte'lari firlatabilir. IT-RX henuz kurulmadigi
     * icin donanim bu veriyi yakalayip ORE bayragini TEKRAR kaldirir. Bu
     * yuzden stabilizasyon beklemesinden SONRA bir kez daha temizle. RX'in
     * ilk arm'i (main: Lora_StartReceive) bu temizlikten sonra olacak,
     * StartReceive de kendi icinde kosulsuz temizledigi icin cift guvence. */
    HAL_UART_Abort(huart);
    __HAL_UART_CLEAR_OREFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);
    __HAL_UART_CLEAR_FEFLAG(huart);

    return cfg_st;
}

/* =========================================================================
 * Geri kalan public API — TX/RX veri duzlemi, E32 ile birebir AYNI
 * ========================================================================= */
void Lora_SetRxByteHandler(LoraCtx_t *ctx, LoraRxCb_t cb, void *user)
{
    if (!ctx) return;
    ctx->rx_cb   = cb;
    ctx->rx_user = user;
}

LoraStatus_t Lora_StartReceive(LoraCtx_t *ctx)
{
    if (!ctx || !ctx->huart) return LORA_ERR;

    /* Guvence 1: arm oncesi RxState READY degilse abort et. Onceki bloklayan
     * islemlerden veya yarim kalan IT-RX'ten kalan BUSY_RX, Receive_IT'i
     * HAL_BUSY'ye dusurur ve RX sessizce arm edilemez. */
    if (ctx->huart->RxState != HAL_UART_STATE_READY)
    {
        HAL_UART_AbortReceive(ctx->huart);
    }

    /* Guvence 2 (KRITIK): Hata bayraklarini KOSULSUZ temizle.
     * TX sirasinda (IT-RX kapaliyken) hatta veri dustuyse donanimda ORE
     * asili kalir AMA RxState READY'dir (AbortReceive onu zaten READY
     * yapmistir). Bu durumda yukaridaki if'e GIRILMEZ; eger temizlik if
     * icinde olsaydi ORE asili kalir, Receive_IT baslar ama donanim ORE
     * yuzunden yeni byte kesmesi uretmez -> RX sonsuza kadar sagir kalir.
     * Bu yuzden temizlik her cagri da kosulsuz yapilmali. */
    __HAL_UART_CLEAR_OREFLAG(ctx->huart);
    __HAL_UART_CLEAR_NEFLAG(ctx->huart);
    __HAL_UART_CLEAR_FEFLAG(ctx->huart);

    if (HAL_UART_Receive_IT(ctx->huart, &ctx->rx_byte_buf, 1) != HAL_OK)
    {
        ctx->rx_active = 0U;
        return LORA_ERR;
    }

    ctx->rx_active = 1U;          /* FIX-4 */
    return LORA_OK;
}

/* =========================================================================
 * FIX-4 / FIX-5: TX-safe cekirdek.
 *
 *  1. (varsa) devam eden IT-RX'i abort et — TX ile cakismayi onler
 *  2. Bloklayan TX yap
 *  3. RX onceden aktifse yeniden arm et
 *
 *  block_aux=1 : AUX HIGH beklenir, timeout'ta LORA_ERR_BUSY (normal veri)
 *  block_aux=0 : AUX kisa beklenir, timeout olsa bile gonderir (kritik)
 * ========================================================================= */
static LoraStatus_t Lora_TxSafe(LoraCtx_t *ctx, const uint8_t *data,
                                uint16_t len, uint8_t block_aux,
                                uint32_t tx_timeout_ms)
{
    if (!ctx || !ctx->huart || !data || len == 0U) return LORA_ERR;

    LoraStatus_t result = LORA_OK;
    uint8_t was_rx = ctx->rx_active;

    /* AUX kontrolu */
    if (block_aux) {
        if (E22_WaitAuxHigh(200U) != LORA_OK)
            return LORA_ERR_BUSY;            /* normal veri: mesgulse atla */
    } else {
        (void)E22_WaitAuxHigh(50U);          /* kritik: timeout olsa da devam */
    }

    /* 1. IT-RX'i guvenli durdur */
    if (was_rx) {
        HAL_UART_AbortReceive(ctx->huart);
        ctx->rx_active = 0U;
    }

    /* 2. Bloklayan TX */
    if (HAL_UART_Transmit(ctx->huart, (uint8_t *)data, len, tx_timeout_ms)
            != HAL_OK)
        result = LORA_ERR;

    /* 3. RX'i yeniden baslat (onceden aktifse) */
    if (was_rx) {
        if (Lora_StartReceive(ctx) != LORA_OK)
            result = LORA_ERR;
    }

    return result;
}

LoraStatus_t Lora_Send(LoraCtx_t *ctx, const uint8_t *data, uint16_t len)
{
    return Lora_TxSafe(ctx, data, len, /*block_aux=*/1U, /*tx_to=*/1000U);
}

void Lora_OnUartRxCplt(LoraCtx_t *ctx, UART_HandleTypeDef *huart)
{
    if (!ctx || ctx->huart != huart) return;

    if (ctx->rx_cb)
        ctx->rx_cb(ctx->rx_byte_buf, HAL_GetTick(), ctx->rx_user);

    /* Bir sonraki byte icin interrupt'i yeniden arm et */
    __HAL_UART_CLEAR_OREFLAG(ctx->huart);
    __HAL_UART_CLEAR_NEFLAG(ctx->huart);
    __HAL_UART_CLEAR_FEFLAG(ctx->huart);
    if (HAL_UART_Receive_IT(ctx->huart, &ctx->rx_byte_buf, 1) == HAL_OK)
        ctx->rx_active = 1U;
    else
        ctx->rx_active = 0U;
}

void Lora_OnUartError(LoraCtx_t *ctx, UART_HandleTypeDef *huart)
{
    if (!ctx || ctx->huart != huart) return;

    /* RX'i guvenli yeniden baslat. Lora_StartReceive ZATEN kosulsuz
     * ORE/NE/FE temizligi + RxState guard yapiyor; cipak Receive_IT
     * cagirmak yerine onu kullaniyoruz. Boylece HAL'in bazi versiyonlarinda
     * ORE aninda RxState BUSY_RX'te asili kalinca olusan kalici sagirlasma
     * (Receive_IT -> HAL_BUSY -> rx_active=0 -> sonsuza kadar sessiz) engellenir. */
    if (Lora_StartReceive(ctx) != LORA_OK)
        ctx->rx_active = 0U;
}
