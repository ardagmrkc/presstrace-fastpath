#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Hat bicimi (odev sartnamesi): her mesaj ASCII metindir, bosluklarla 63
 * bayta tamamlanir ve 64. bayt LF'dir -> her mesaj tam 64 bayt. Metin 63
 * bayti asarsa mesaj KESILMEZ, hic gonderilmez (encode_error sayilir); asagidaki
 * bicimler en kotu durumda da 63 bayta sigacak sekilde secilmistir.
 * Tam tablo: docs/code-notes.md#paket-protokolu.
 *
 *   TEL,<seq>,S<n>,<temp_centi_c>,<temp_raw>,<vdda_mv>,<extra_us>,<period_us>,<txq>
 *   BTN,<event_id>,S<n>,PRESSED,<seq>,<t0>,<t1>,<t2>
 *   ACK,<seq>,S<n>
 *   REC,S<n>,<event_id>,<t0>,<t1-t0>,<t2-t1>,<t3-t2>,<t4-t3>,<status>
 *   CNT,S<n>,<ad>,<deger>
 *   END,S<n>,<gonderilen_REC_sayisi>
 *
 * REC'teki sureler bilesen sureleridir (sartnamedeki hesap tablosu); mutlak
 * zamanlar t0'dan toplanarak bulunur. Bilinmeyen ya da >= 1 s olan bir sure
 * BOS birakilir (0 yazilmaz). seq yalnizca TEL/BTN/ACK satirlarinda vardir ve
 * yalnizca onlar icin artar (kayip tespiti); REC/CNT/END yalnizca olcum
 * penceresi kapaninca gonderilir, sayilari END ile dogrulanir.
 */
#define PROTOCOL_FRAME_SIZE 64U
#define PROTOCOL_LINE_MAX   63U

/*
 * Yer istasyonu -> kart komutu (5 bayt, ikili), yalnizca kullanici bir secim
 * yaptiginda gonderilir. Sartnamenin 64 baytlik TEL/BTN kurali karttan
 * cikan mesajlar icindir; bu komut ek bir ozelliktir:
 *   AA 55 'C' arg checksum
 * arg: 0..5 = senaryoyu degistir (yeni olcum penceresi),
 *      0xFE = olcumu bitir (kart S0'a gecer ve kayitlari doker),
 *      0xFF = yalnizca sorgula.
 * checksum: bayt 0..3 toplaminin mod 256'si. Kart her gecerli komuta ACK ile
 * yanit verir.
 */
#define CMD_SYNC0          0xAAU
#define CMD_SYNC1          0x55U
#define CMD_FRAME_SIZE     5U
#define CMD_TYPE_SCENARIO  ((uint8_t)'C')
#define CMD_ARG_DUMP       0xFEU
#define CMD_ARG_QUERY      0xFFU

#define EXPERIMENT_TIMEOUT_US 1000000U /* sartname: deney zaman asimi 1 s */

typedef enum
{
    /* Hatta giden satirlar */
    MSG_TEL = 0,
    MSG_BTN,
    MSG_ACK,
    MSG_REC,
    MSG_CNT,
    MSG_END,

    /* Yalnizca TX kuyrugu icinde, TelemetryTask -> UartTxTask; hatta cikmaz */
    MSG_CMD_POOL_RESET, /* yeni olcum penceresi: kayitlar ve pencere sayaclari sifirlanir */
    MSG_CMD_DUMP,       /* kayitlari REC + CNT + END olarak gonder */

    /* Yalnizca TX kuyrugu icinde, UART TC ISR'si -> UartTxTask; hatta cikmaz.
     * Kesmeden gonderilen (hizli yol) BTN satirinin sonucu: u.rec doludur,
     * UartTxTask yalnizca kayit havuzuna yazar. */
    MSG_BTN_DONE,
} msg_type_t;

typedef enum
{
    REC_STATUS_OK = 0,
    REC_STATUS_TX_DROP,  /* t2'de TX kuyrugu dolu: yanit hic gonderilmedi */
    REC_STATUS_BTN_DROP, /* ISR'de buton kuyrugu dolu: gorev olayi hic almadi */
    REC_STATUS_TX_ERROR, /* UART baslatilamadi ya da TC baska aktarima aitti */
    REC_STATUS_TIMEOUT,  /* TC 50 ms icinde gelmedi ya da R >= 1 s */
} rec_status_t;

typedef enum
{
    CNT_ACCEPTED = 0,      /* filtrenin kabul ettigi basis */
    CNT_REPEAT,            /* filtrenin reddettigi kenar (sicrama) */
    CNT_BTN_QUEUE_DROP,    /* buton kuyrugu dolu */
    CNT_TX_QUEUE_DROP,     /* TX kuyrugu dolu (TEL + BTN) */
    CNT_TX_QUEUE_MAX,      /* TX kuyrugu en yuksek doluluk */
    CNT_POOL_OVERFLOW,     /* 64'luk kayit havuzu dolu */
    CNT_DROPLOG_OVERFLOW,  /* dusen olay kaydi dolu */
    CNT_TX_TIMEOUT,
    CNT_TX_START_FAIL,
    CNT_SPURIOUS_TC,
    CNT_ENCODE_ERROR,      /* 63 bayta sigmayan satir (olmamali) */
    CNT_TEL_SENT,          /* TX kuyruguna giren TEL sayisi */
    CNT_TEL_PERIOD_MIN_US, /* olculen gercek telemetri periyodu */
    CNT_TEL_PERIOD_AVG_US,
    CNT_TEL_PERIOD_MAX_US,
    CNT_BTN_DIRECT,        /* hizli yol: hat bosken EXTI ISR'sinden hemen baslatildi */
    CNT_BTN_LATCHED,       /* hizli yol: hat mesguldu, mandal kuruldu, TC ISR'si baslatti */
    CNT_BTN_FALLBACK,      /* mandal doluydu: olay ButtonTask yoluna gitti */
    CNT_BTN_RESULT_LOST,   /* hizli yol sonucu TX kuyruguna giremedi (olmamali) */
    CNT_COUNT
} cnt_id_t;

/* UART TX kuyruguna konulan mantiksal mesaj. Metne cevirme yalnizca
 * UartTxTask icinde, gonderim aninda yapilir. */
typedef struct
{
    msg_type_t type;
    uint8_t scenario_id;
    union
    {
        struct
        {
            int16_t temp_centi_c;
            uint16_t temp_raw;
            uint16_t vdda_mv;
            uint8_t txq_depth;
            uint32_t extra_load_us;
            uint32_t period_us; /* bir onceki TEL uretiminden bu yana; ilkinde 0 */
        } tel;
        struct
        {
            uint16_t event_id;
            uint32_t t0, t1, t2;
        } btn;
        struct
        {
            uint16_t event_id;
            rec_status_t status;
            uint8_t known; /* gecerli zaman sayisi: 1 = yalniz t0 ... 5 = t0..t4 */
            uint32_t t[5];
        } rec;
        struct
        {
            cnt_id_t id;
            uint32_t value;
        } cnt;
        struct
        {
            uint32_t records;
        } end;
    } u;
} tx_message_t;

/* msg'yi 64 baytlik satira cevirir. seq yalnizca TEL/BTN/ACK icin
 * kullanilir. Metin 63 bayti asarsa false doner ve out gecersizdir. */
bool protocol_encode(const tx_message_t *msg, uint16_t seq, uint8_t out[PROTOCOL_FRAME_SIZE]);

/* seq numarasi tasiyan (ve dolayisiyla artiran) satir mi? */
bool protocol_has_seq(msg_type_t type);

#endif /* PROTOCOL_H */
