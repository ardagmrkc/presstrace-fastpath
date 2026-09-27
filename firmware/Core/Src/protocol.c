#include "protocol.h"
#include <string.h>

/* snprintf yerine kucuk, deterministik bir yazici: C kutuphanesinin yigin ve
 * sure degiskenligi UartTxTask'a girmesin. Tampon 63'ten genistir; tasma
 * sonradan kontrol edilir, hicbir zaman sessizce kesilmez. */
typedef struct
{
    char buf[96];
    size_t len;
} line_t;

static void put_c(line_t *l, char c)
{
    if (l->len < sizeof(l->buf))
    {
        l->buf[l->len] = c;
    }
    l->len++;
}

static void put_s(line_t *l, const char *s)
{
    while (*s != '\0')
    {
        put_c(l, *s++);
    }
}

static void put_u32(line_t *l, uint32_t v)
{
    char tmp[10];
    size_t n = 0;
    do
    {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U);
    while (n > 0U)
    {
        put_c(l, tmp[--n]);
    }
}

static void put_i32(line_t *l, int32_t v)
{
    if (v < 0)
    {
        put_c(l, '-');
        put_u32(l, (uint32_t)(-(int64_t)v));
    }
    else
    {
        put_u32(l, (uint32_t)v);
    }
}

static void put_sep_u32(line_t *l, uint32_t v)
{
    put_c(l, ',');
    put_u32(l, v);
}

static void put_scenario(line_t *l, uint8_t id)
{
    put_s(l, ",S");
    put_u32(l, id);
}

static const char *const s_status_names[] = {
    "ok", "tx_drop", "btn_drop", "tx_error", "timeout",
};

static const char *const s_cnt_names[CNT_COUNT] = {
    "accepted", "repeat", "btn_queue_drop", "tx_queue_drop", "tx_queue_max",
    "pool_overflow", "droplog_overflow", "tx_timeout", "tx_start_fail",
    "spurious_tc", "encode_error", "tel_sent", "tel_period_min_us",
    "tel_period_avg_us", "tel_period_max_us", "btn_direct", "btn_latched",
    "btn_fallback", "btn_result_lost",
};

bool protocol_has_seq(msg_type_t type)
{
    return type == MSG_TEL || type == MSG_BTN || type == MSG_ACK;
}

static void encode_rec(line_t *l, const tx_message_t *m)
{
    put_s(l, "REC");
    put_scenario(l, m->scenario_id);
    put_sep_u32(l, m->u.rec.event_id);
    put_sep_u32(l, m->u.rec.t[0]);
    /* Bilesen sureleri: t1-t0, t2-t1, t3-t2, t4-t3. uint32 farki sayac
     * sarmasinda da dogrudur. Bilinmeyen ya da >= 1 s olan sure bos kalir. */
    for (uint8_t i = 1U; i < 5U; i++)
    {
        put_c(l, ',');
        if (i < m->u.rec.known)
        {
            uint32_t d = m->u.rec.t[i] - m->u.rec.t[i - 1U];
            if (d < EXPERIMENT_TIMEOUT_US)
            {
                put_u32(l, d);
            }
        }
    }
    put_c(l, ',');
    put_s(l, s_status_names[m->u.rec.status]);
}

bool protocol_encode(const tx_message_t *m, uint16_t seq, uint8_t out[PROTOCOL_FRAME_SIZE])
{
    line_t l = { .len = 0 };

    switch (m->type)
    {
    case MSG_TEL:
        put_s(&l, "TEL");
        put_sep_u32(&l, seq);
        put_scenario(&l, m->scenario_id);
        put_c(&l, ',');
        put_i32(&l, m->u.tel.temp_centi_c);
        put_sep_u32(&l, m->u.tel.temp_raw);
        put_sep_u32(&l, m->u.tel.vdda_mv);
        put_sep_u32(&l, m->u.tel.extra_load_us);
        put_sep_u32(&l, m->u.tel.period_us);
        put_sep_u32(&l, m->u.tel.txq_depth);
        break;
    case MSG_BTN:
        put_s(&l, "BTN");
        put_sep_u32(&l, m->u.btn.event_id);
        put_scenario(&l, m->scenario_id);
        put_s(&l, ",PRESSED");
        put_sep_u32(&l, seq);
        put_sep_u32(&l, m->u.btn.t0);
        put_sep_u32(&l, m->u.btn.t1);
        put_sep_u32(&l, m->u.btn.t2);
        break;
    case MSG_ACK:
        put_s(&l, "ACK");
        put_sep_u32(&l, seq);
        put_scenario(&l, m->scenario_id);
        break;
    case MSG_REC:
        encode_rec(&l, m);
        break;
    case MSG_CNT:
        put_s(&l, "CNT");
        put_scenario(&l, m->scenario_id);
        put_c(&l, ',');
        put_s(&l, s_cnt_names[m->u.cnt.id]);
        put_sep_u32(&l, m->u.cnt.value);
        break;
    case MSG_END:
        put_s(&l, "END");
        put_scenario(&l, m->scenario_id);
        put_sep_u32(&l, m->u.end.records);
        break;
    default:
        return false; /* ic komutlar hatta cikmaz */
    }

    if (l.len > PROTOCOL_LINE_MAX)
    {
        return false;
    }
    memcpy(out, l.buf, l.len);
    memset(&out[l.len], ' ', PROTOCOL_LINE_MAX - l.len);
    out[PROTOCOL_LINE_MAX] = '\n';
    return true;
}
