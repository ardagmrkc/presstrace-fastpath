# Kod Notları / Tasarım Kararları

## Görev mimarisi

| Görev | Öncelik | Kaynak | Rolü |
|---|---|---|---|
| `TelemetryTask` | 3 (en yüksek) | [telemetry_task.c](../firmware/Core/Src/telemetry_task.c) | `g_scenario.telemetry_hz` periyodunda TEL paketi üretir; S4/S5'te periyot başına `workload_run()` ile ~2/5 ms ek CPU işi de bu görev içinde çalışır. |
| `ButtonTask` | 2 (orta) | [button_task.c](../firmware/Core/Src/button_task.c) | **Yedek yol.** Hızlı yolun reddettiği basışlar (mandal dolu ya da UART henüz hazır değil) buraya gelir: olayı alır (t1), BTN paketini oluşturup (t2) UART TX kuyruğuna yazar. |
| `UartTxTask` | 1 (en düşük) | [uart_tx_task.c](../firmware/Core/Src/uart_tx_task.c) | Görev yolu: TX kuyruğunu FIFO sırayla işler, hattı hakemden **sahiplenir**, gönderimi başlatır (t3), TC'yi süreli bekler (t4). Ölçüm kaydı havuzunun **tek sahibidir**: hızlı yol sonuçlarını (`MSG_BTN_DONE`) da havuza o yazar. |
| Hızlı yol | NVIC 5 (kesme) | [uart.c](../firmware/Core/Src/uart.c), [button.c](../firmware/Core/Src/button.c) | EXTI0 kesmesi buton satırını hat boşsa hemen başlatır, meşgulse mandal bırakır; TC kesmesi mandalı görevi uyandırmadan önce başlatır. Ayrıntı: [UART hakemi ve hızlı yol](#uart-hakemi-ve-hızlı-yol). |

Telemetriyi en yüksek, UART çıkışını en düşük önceliğe koymak **kasıtlıdır**: deneyin amacı,
yüksek öncelikli periyodik işin (ve S4/S5'teki ek CPU yükünün) daha düşük öncelikli yanıt
yolunu ne kadar geciktirdiğini ölçmektir. Bu depoda görev öncelikleri **değiştirilmedi**; butonun
zaman kritik yolu görev düzleminden NVIC düzlemine taşındı.

## Zaman damgası zinciri

```
Hızlı yol (normal durum, hepsi NVIC 5 kesmelerinde):
[EXTI0 ISR]                                              [TC ISR]
  t0 -> t1 -> t2 --(hat boşsa hemen / meşgulse mandal)--> t3 --[UART 64B]--> t4
                                                          (TC ISR'si mandalı başlatır)

Yedek yol (mandal dolu ya da UART hazır değil):
[EXTI0 ISR]      [ButtonTask]                 [UartTxTask]
  t0 ----------> t1 --> t2 -----------------> t3 --[UART 64B]--> t4 (TC ISR)
```

- **t0** — `button_exti_isr_handler()` ([button.c](../firmware/Core/Src/button.c)): tekrar-kenar
  filtresinin kabul ettiği basış kenarının anı. Ayrıntılar aşağıdaki "Buton ISR'si" bölümünde.
- **t1** — Hızlı yolda `uart_btn_submit_from_isr()` girişi (t0'dan 1–2 µs sonra). Yedek yolda
  `button_task()` içinde `xQueueReceive` döndüğü an; bu durumda `t1 − t0`, ButtonTask'ın CPU'yu
  bulma beklemesini içerir.
- **t2** — Hızlı yolda hat kararı öncesi. Yedek yolda `xQueueSend` çağrısından hemen önce;
  gönderim başarısız olursa (`UART_TX_QUEUE_LEN` dolu) olay kaybolmaz: kimliği ve t0..t2 düşen
  olay kaydına yazılır ve ölçüm sonunda `REC,…,tx_drop` olarak gelir.
- **t3** — Satır hatta başlatılmadan hemen önce: hızlı yolda EXTI0 ya da TC kesmesinde, görev
  yolunda `send_frame()` içinde `uart_start_claimed()`'den önce. **İlk fiziksel bitin hatta
  çıktığı an değildir.** Hızlı yolda `t3 − t2`, hattın boşalmasını bekleme ve satırı hazırlama
  süresidir (hat boşken 17–19 µs).
- **t4** — `uart_isr_handler()` ([uart.c](../firmware/Core/Src/uart.c)) içinde, TC (Transmission
  Complete) bayrağını işlerken ISR'nin ilk işi olarak alınır. TC, son baytın **son stop bitiyle
  birlikte** hattan çıktığı anı işaretler (RM0090 USART_SR.TC tanımı). DMA TC değildir; ayrıntı
  için "Ölçüm kaydı ve t₄'ün doğruluğu" bölümüne bakın.

t3, t4 ve olayın durumu kartın kayıt havuzunda tutulur ve ölçüm penceresi bitince **`REC`**
satırlarıyla gelir. Canlı BTN satırı yalnızca t0..t2'yi taşır. `R = t4 − t0` arayüzde ve
`analyze.py`'de hesaplanır ve 20 ms deadline ile karşılaştırılır.

Raporda üç süre tanımlarıyla birlikte verilmelidir:

- **İş ve kuyruk:** `t3 − t0`
- **Hat üstü kalış:** `t4 − t3` ≈ 64 × 10 / 230400 = 2,78 ms (gerçek baud 230 769 ile 2773 µs),
  artı başlatma ve TC kesmesine giriş gecikmesi. Ölçülen: 2775–2781 µs.
- **Uçtan uca:** `R = t4 − t0`

## Kod blokları

Aşağıdaki parçalar kaynak dosyalardan alınmıştır (yorumlar kısaltıldı). Tam hâli için bağlantılı
dosyalara bakın.

### Buton ISR'si: t0 ve kısa ISR ([button.c](../firmware/Core/Src/button.c))

```c
void button_exti_isr_handler(void)
{
    const uint32_t now_us = timestamp_now_us();          /* önce zaman (TIM2->CNT) */
    if ((EXTI->PR & (1UL << BUTTON_EXTI_LINE)) == 0U) return;
    EXTI->PR = (1UL << BUTTON_EXTI_LINE);                /* sonra bayrak */

    if (!accept_edge(now_us, button_is_high())) return;  /* sıçrama: repeat_count++ */

    button_event_t evt = { next_id(), now_us };          /* t0 = kabul edilen basış kenarı */
    s_accepted_count++;
    BaseType_t wake = pdFALSE;

    /* Hızlı yol: hat boşsa satır hemen, meşgulse mandaldan (TC ISR'si) başlar. */
    if (uart_btn_submit_from_isr(evt.event_id, g_scenario.id, evt.t0_us, &wake) ==
        UART_BTN_REJECTED) {
        s_fallback_count++;                              /* yedek yol: ButtonTask */
        if (xQueueSendFromISR(s_evt_queue, &evt, &wake) != pdPASS) {
            s_drop_count++;                              /* olay kaybolmaz: REC(btn_drop) */
            stats_droplog_add_from_isr(evt.event_id, g_scenario.id, evt.t0_us);
        }
    }
    portYIELD_FROM_ISR(wake);
}

static bool accept_edge(uint32_t now_us, bool high)      /* iki kenar da buraya gelir */
{
    if (s_have_edge && (uint32_t)(now_us - s_last_edge_us) < BUTTON_REPEAT_WINDOW_US) {
        s_last_edge_us = now_us;                         /* 30 ms içinde: sıçrama */
        s_repeat_count++;
        return false;
    }
    s_have_edge = true;                                  /* ilk kenar her zaman kabul */
    s_last_edge_us = now_us;
    if (!high) { s_pressed = false; return false; }      /* bırakma: olay yok */
    if (s_pressed) return false;                         /* basılıyken parazit */
    s_pressed = true;
    return true;
}
```

### ButtonTask (yedek yol): t1, t2 ([button_task.c](../firmware/Core/Src/button_task.c))

Yalnızca hızlı yolun reddettiği basışlar buraya gelir (27 Eylül ölçümlerinde hiç kullanılmadı,
`btn_fallback` = 0).

```c
xQueueReceive(s_evt_queue, &evt, portMAX_DELAY);
uint32_t t1_us = timestamp_now_us();                     /* t1: olay alındıktan hemen sonra */

tx_message_t msg = { .type = MSG_BTN };                  /* olay kimliği + t0 kopyalanır */
msg.u.btn.event_id = evt.event_id;
msg.u.btn.t0 = evt.t0_us;
msg.u.btn.t1 = t1_us;
msg.u.btn.t2 = timestamp_now_us();                       /* t2: xQueueSend'den hemen önce */

if (xQueueSend(s_tx_queue, &msg, 0) == pdPASS)
    stats_note_tx_depth(uxQueueMessagesWaiting(s_tx_queue));
else {
    stats_record_tx_drop();                              /* zincir kırıldı: REC(tx_drop) */
    stats_droplog_add(evt.event_id, msg.scenario_id, msg.u.btn.t0, msg.u.btn.t1, msg.u.btn.t2);
}
```

### TelemetryTask: periyodik üretim ve ek CPU işi ([telemetry_task.c](../firmware/Core/Src/telemetry_task.c))

```c
for (;;) {
    if (g_scenario.telemetry_hz == 0U) {                 /* S0: bloklanır, boş döngü yok */
        xTaskNotifyWait(0U, UINT32_MAX, &cmd, portMAX_DELAY);
        handle_command(cmd);
        continue;
    }
    send_telemetry();          /* gerçek periyodu ölç, workload_run(), TEL'i bloklamadan kuyruğa */
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000U / g_scenario.telemetry_hz));
}
```

### UartTxTask: t3, t4 ve kaydın kapanması ([uart_tx_task.c](../firmware/Core/Src/uart_tx_task.c))

```c
static send_result_t send_frame(const tx_message_t *msg, uint32_t *t3_out, uint32_t *t4_out)
{
    uint8_t frame[PROTOCOL_FRAME_SIZE];                  /* TC'ye kadar yaşar: görev bloklu */
    const uint32_t tag = next_tag();
    uint16_t seq;

    /* Hattı hakemden sahiplen: hızlı yoldaki buton hattaysa ya da mandalda
     * bekliyorsa önce o biter (en fazla 50 ms). */
    if (!uart_claim_line(pdMS_TO_TICKS(TX_DONE_TIMEOUT_MS), &seq)) { s_tx_start_fail++; return SEND_START_FAIL; }
    if (!protocol_encode(msg, seq, frame)) { uart_release_line(); s_encode_error++; return SEND_ENCODE_FAIL; }

    const uint32_t t3_us = timestamp_now_us();           /* t3: başlatmadan hemen önce */
    if (t3_out) *t3_out = t3_us;
    uart_start_claimed(frame, PROTOCOL_FRAME_SIZE, tag, protocol_has_seq(msg->type)); /* seq burada artar */

    uint32_t t4_us;                                      /* sonsuz bekleme yok: 50 ms */
    uart_tx_result_t res = uart_wait_tx_done(tag, pdMS_TO_TICKS(TX_DONE_TIMEOUT_MS), &t4_us);
    if (res == UART_TX_TIMEOUT) { s_tx_timeout++; return SEND_TIMEOUT; }
    if (res != UART_TX_DONE)   return SEND_WRONG_TC;
    if (t4_out) *t4_out = t4_us;
    return SEND_OK;
}
/* send_button() (yedek yol): SEND_OK ise R = t4 - t0 < 1 s -> "ok", değilse "timeout";
 * SEND_TIMEOUT -> "timeout"; hat alınamadı / kodlama / yanlış TC -> "tx_error".
 * MSG_BTN_DONE (hızlı yol): satır zaten gitti, record_btn_done() yalnızca kaydı yazar. */
```

### UART TC kesmesi: t4 ve mandal ([uart.c](../firmware/Core/Src/uart.c))

```c
if ((sr & USART_SR_TC) && (cr1 & USART_CR1_TCIE)) {      /* TCIE yalnızca son bayttan sonra açık */
    const uint32_t t4_us = timestamp_now_us();           /* ilk iş: zaman (son stop biti çıktı) */
    uint32_t notify = 0U;
    UART_PERIPH->CR1 &= ~USART_CR1_TCIE;
    UART_PERIPH->SR &= ~USART_SR_TC;

    if (s_owner == OWNER_TASK) {                         /* görevin satırı bitti */
        uint32_t tag = s_in_flight_tag; s_in_flight_tag = 0;
        if (tag == 0U) s_spurious_tc++;
        else { s_done_t4_us = t4_us; s_done_tag = tag; notify |= NOTIFY_TX_DONE; }
    } else if (s_owner == OWNER_BTN) {                   /* hızlı yol satırı bitti */
        s_btn_air.t[4] = t4_us;
        report_btn_locked(&s_btn_air, 5U, false, &higher_prio_woken); /* -> UartTxTask */
    } else {
        s_spurious_tc++;
    }

    s_owner = OWNER_NONE;                                /* hat boşaldı */
    if (s_btn_latch.valid) btn_launch_locked(&higher_prio_woken); /* ÖNCE mandal */
    if (s_owner == OWNER_NONE) notify |= NOTIFY_LINE_FREE;
    if (notify) xTaskNotifyFromISR(s_notify_task, notify, eSetBits, &higher_prio_woken); /* SONRA görev */
}
```

### Zaman hesapları

Tüm zamanlar aynı 32-bit, 1 MHz TIM2 sayacından gelir. Farklar işaretsiz 32-bit çıkarmayla
(mod 2³²) alınır; sayaç ~71,6 dakikada bir başa sarsa da bir olayın süresi bir turdan kısa
olduğu sürece sonuç doğrudur:

```c
uint32_t r_us = t4 - t0;                    /* C: uint32_t aritmetiği zaten mod 2^32 */
```

```js
const u32diff = (b, a) => (b - a) >>> 0;    // app.js
t[i] = (t[i - 1] + d) >>> 0;                // REC süresinden mutlak zamanı geri kur
```

```python
df["r_us"] = ((df["t4_us"] - df["t0_us"]) % 2**32)   # analyze.py
```

| Hesap | Anlamı |
|---|---|
| Hesap | Hızlı yol (normal) | Yedek yol (ButtonTask) |
|---|---|---|
| t₁ − t₀ | Filtreden hızlı yola giriş (1–2 µs) | ISR'den görevin olayı almasına kadar (olay aktarımı + CPU beklemesi) |
| t₂ − t₁ | Kesme içi hazırlık (0–1 µs) | Yanıt hazırlama; preemption dahil olabilir |
| t₃ − t₂ | Hattın boşalmasını bekleme + satırı kodlama (hat boşken 17–19 µs, mandalda en çok ~2,8 ms) | Kuyruğa verme + FIFO + hattın sahiplenilmesi |
| t₄ − t₃ | 64 baytın hattan çıkışı (230400'de 2,78 ms) ve TC gözlem gecikmesi | Aynı |
| R = t₄ − t₀ | Kart tarafında gözlenen toplam yanıt süresi | Aynı |

## UART hakemi ve hızlı yol

**Problem.** Görev tabanlı yanıt yolunda S5'te (100 Hz + 5 ms iş) basış iş sürerken gelirse iki
gecikme art arda eklenir:
- ButtonTask (2) ve UartTxTask (1), TelemetryTask'ın (3) işi bitene kadar CPU alamaz.
- İş bitince önce TEL kuyruğa girer; buton satırı FIFO'da onun arkasına düşer.

230400'deki ara ölçümde bu grup 6,08–8,73 ms'ydi. Oysa iş sürerken hat **boştur**: TEL işten
sonra başlar ve 2,78 ms'de, bir sonraki iş başlamadan biter. Hattı kullanamayan donanım değil,
CPU'ya erişemeyen görevlerdir. NVIC kesmeleri ise görev önceliğinden bağımsız çalışır.

**Çözüm.** Hattın sahipliği tek bir hakemde toplanır ([uart.c](../firmware/Core/Src/uart.c)),
butonun zaman kritik yolu kesme düzlemine taşınır. Görev öncelikleri, NVIC öncelikleri, baud ve
satır biçimi değişmez.

```c
/* EXTI0 ISR'sinden (NVIC 5): */
uart_btn_submit_t uart_btn_submit_from_isr(uint16_t id, uint8_t sc, uint32_t t0, BaseType_t *woken)
{
    const uint32_t t1 = timestamp_now_us();
    if (!s_ready || s_btn_latch.valid) return UART_BTN_REJECTED;   /* -> yedek yol */
    s_btn_latch = (btn_slot_t){ .valid = true, .event_id = id, .scenario_id = sc, .t = { t0, t1 } };
    s_btn_latch.t[2] = timestamp_now_us();                          /* t2: hat kararı öncesi */
    if (s_owner != OWNER_NONE) { s_btn_latched++; return UART_BTN_LATCHED; } /* TC ISR başlatır */
    s_btn_direct++;
    btn_launch_locked(woken);            /* kodla, seq++, t3, owner = BTN, TXEIE */
    return UART_BTN_STARTED;
}

/* UartTxTask'tan: */
bool uart_claim_line(TickType_t timeout, uint16_t *seq)
{
    for (;;) {
        taskENTER_CRITICAL();                           /* NVIC 5..15 maskeli: EXTI0 ve USART2 */
        if (s_owner == OWNER_NONE && !s_btn_latch.valid) {  /* kontrol + sahiplenme tek adım */
            s_owner = OWNER_TASK; *seq = s_seq;
            taskEXIT_CRITICAL();
            return true;
        }
        taskEXIT_CRITICAL();
        /* zaman aşımı: takılan buton aktarımını bir kez kurtar, sonra false */
        xTaskNotifyWait(0, NOTIFY_LINE_FREE, NULL, remaining);  /* hat boşalınca uyan */
    }
}
```

| # | Değişmez | Nasıl sağlanıyor |
|---|---|---|
| D1 | Hatta en fazla bir satır | Sahiplik tek değişkende: `s_owner` ∈ {NONE, TASK, BTN} |
| D2 | Sahiplik kararı atomik, baytlar karışamaz | EXTI0 ve USART2 aynı NVIC önceliğinde (5), birbirini kesemez; görev tarafı `taskENTER_CRITICAL` ile 5–15'i maskeler |
| D3 | TEL, bekleyen butonun önüne geçemez | TC ISR'si önce mandalı başlatır, görevi sonra uyandırır; mandal varken `uart_claim_line` başarısız olur |
| D4 | Hattaki sıra = seq sırası | seq yalnızca satır hatta başlatılırken artar (`btn_launch_locked`, `uart_start_claimed`) |
| D5 | Kayıt havuzunun tek sahibi UartTxTask | Hızlı yol sonucu TC ISR'sinden `MSG_BTN_DONE` ile iletilir |
| D6 | Hiçbir basış kaybolmaz | Mandal → yedek yol (ButtonTask) → düşen olay kaydı |
| D7 | Hat kilitli kalmaz | Görev: 50 ms'de iptal (`uart_abort_task_tx`). Hızlı yol: TC'si 50 ms'de gelmeyen buton satırı `uart_claim_line` zaman aşımında kurtarılır (`timeout`) |

**Neden eski kuralın dışına çıkıldı.** Önceki sürümde "ISR içinde UART yok, iş göreve ertelenir"
kuralı vardı. Bu kural, deneyin ölçtüğü gecikmenin ta kendisiydi. Hızlı yolda kesme satırı
hazırlayıp başlatır: ölçülen maliyet 17–19 µs (`t₃ − t₂`, hat boşken). Telemetri periyodundaki
etkisi en fazla +17 µs (S2 en uzun periyot 20 017 µs).

**Bilgisayar testi:** [tests/host/test_uart_arbiter.c](../tests/host/test_uart_arbiter.c) gerçek
`uart.c`'yi taklit register'lar ve taklit FreeRTOS ile çalıştırır. 8 senaryoda D1–D7'yi ve hattaki
her 64 baytın geçerli satır olduğunu doğrular.

**Sayaçlar:** `btn_direct` (hat boştu, hemen başladı), `btn_latched` (mandaldan başladı),
`btn_fallback` (yedek yol), `btn_result_lost` (sonuç kuyruğa giremedi). Doğru çalışmada
`btn_direct + btn_latched = accepted`, diğer ikisi 0'dır.

## Buton ISR'si: kısa ISR, kopyalanan olay, kontrol edilen sonuç

ISR zamanı alır, filtreyi uygular ve kabul edilen basışı hızlı yola verir. Hızlı yol satırı
hazırlayıp hattı başlatır ya da mandal bırakır (~17 µs); reddederse olayın küçük bir kopyası
(`event_id`, `t0`) eskisi gibi ButtonTask kuyruğuna gider. ISR içinde bekleme veya HAL çağrısı
yoktur.

1. **Önce zaman, sonra bayrak.** `timestamp_now_us()` (TIM2->CNT, tek 32-bit okuma, ISR'de
   atomik) bayrak temizlenmeden önce okunur; damga kenara en yakın andır.
2. **Tekrar-kenar filtresi (30 ms, `BUTTON_REPEAT_WINDOW_US`).** EXTI0 iki kenarı da dinler.
   İlk kenar her zaman kabul edilir (`s_have_edge`). Sonraki bir kenar, önceki kenardan bu yana
   30 ms dolmadan geldiyse sıçramadır: `repeat_count` artar, pencere yeniden başlar ve olay
   üretilmez. Kabul edilen kenarda pin LOW ise buton "bırakılmış" durumuna geçer. Pin HIGH ise
   ve buton bırakılmış durumdaysa bu bir basıştır. Süre farkları `uint32_t` çıkarmasıyla
   hesaplanır, sayaç başa sardığında da doğru kalır.
3. **Kimlik yalnızca kabul edilen basışa verilir** (1'den başlar); sıçramalar kimlik harcamaz.
4. **Kuyruk doluysa** (`BUTTON_EVENT_QUEUE_LEN` = 8) `xQueueSendFromISR` `pdPASS` dönmez,
   `btn_queue_drop` artar ve olayın kimliği ile t0'ı ISR-güvenli düşen olay kaydına yazılır
   (`stats_droplog_add_from_isr`, `taskENTER_CRITICAL_FROM_ISR` ile). Ölçüm sonunda
   `REC,…,btn_drop` olarak gelir. `wake` `pdFALSE` başlar; yalnızca daha acil bir görev
   uyandıysa yield yapılır.

**Neden yalnızca basış kenarı yetmiyor:** İlk sürüm yalnızca yükselen kenarı ve son kabulden
geçen süreyi kontrol ediyordu. Buton bırakılırken kontak sıçrayıp kısa bir an yeniden temas
ediyor ve basış yönünde bir kenar üretiyor. Bu kenar basıştan pencere süresinden daha sonra
geldiği için yeni basış sayılıyordu. İlk S0 denemesinde gözlendi: bazı ardışık olaylar
arasında, basılı tutma süresine denk gelen 60–280 ms vardı. Pencereyi 30 ms'ye çıkarmak bunu
çözmez. Buton durumunu (basılı/bırakılmış) takip etmek çözer.

**Kart notu:** B1 aktif-HIGH ve harici pull-down'lıdır, yani basış kenarı **yükselen**
kenardır. Pull-up'lı butonlar için geçerli olan "basış = düşen kenar" kuralı bu karta uymaz.

**Kurulum sırası** (`button_task_create()`): statik kuyruk (`xQueueCreateStatic`) → ButtonTask →
GPIO/EXTI → NVIC önceliği → `NVIC_ClearPendingIRQ` → `NVIC_EnableIRQ` (en son). TIM2 daha önce
`main()` içinde başlatılmıştır.

**Kontrol listesi** (sayaçlar "Ölçümü bitir" sonrası arayüzün "Pencere sayaçları" bölümünde):

- İlk basış tek olay üretir, `event_id` 1'dir.
- Aynı basışın ve bırakmanın sıçramaları `repeat` sayacını artırır, yeni satır üretmez.
- Her basış tabloya tek satır ekler, LED bir kez durum değiştirir.
- Buton kuyruğu dolarsa `btn_queue_drop` artar ve olay `btn_drop` durumuyla kayda geçer.
- `accepted` (kabul edilen basış) ile alınan kayıt sayısı eşittir; değilse arayüz uyarır.

## Ölçüm kaydı ve t₄'ün doğruluğu

t₄, yanıt gönderildikten sonra bilinir; aynı yanıtın içine yazılamaz. Bu yüzden kayıt yanıtın
içinde değil, olay kimliğiyle eşlenen bir **yan kayıtta** kapanır.

**Neden kayıt ölçüm sırasında gönderilmiyor:** İlk sürüm her olaydan sonra sonucu canlı bir
BTNR paketiyle (5,56 ms hat süresi) gönderiyordu. Bu, ölçümün kendi raporunun sonraki olayların
kuyruk gecikmesini bozması demekti. Kalibrasyon öncesi S5 denemesinde her basış birikime +20 ms
ekliyordu (BTN + BTNR); BTNR olmasaydı artış +10 ms olurdu. Artık ölçüm penceresinde hatta
yalnızca TEL (deneyin yükü) ve BTN (ölçülen yanıtın kendisi) bulunur.

**Kayıt havuzu** ([uart_tx_task.c](../firmware/Core/Src/uart_tx_task.c)): 64 kayıt, tek sahibi
`UartTxTask`.

- **Kayıt durumu (slot):** `REC_FREE` → `REC_TX_ARMED` (t0..t2 yazıldı, gönderim kuruldu) →
  `REC_CLOSED`. Kapanan kayıt şu **olay durumlarından** birini taşır (şartnamedeki
  "ok, drop, tx_error veya timeout"):

  | `REC` satırı | CSV `status` | Anlamı | Bilinen zamanlar |
  |---|---|---|---|
  | `ok` | `ok` | TC bu olayın kendi aktarımından geldi ve R < 1 s | t0..t4 |
  | `timeout` | `timeout` | TC 50 ms içinde gelmedi ya da R ≥ 1 s (deney zaman aşımı) | t0..t3 (R ≥ 1 s ise t0..t4) |
  | `tx_error` | `tx_error` | UART başlatılamadı ya da TC başka aktarıma aitti | t0..t3 |
  | `tx_drop` | `drop` | t2'de TX kuyruğu doluydu, yanıt hiç gönderilmedi | t0..t2 |
  | `btn_drop` | `drop` | ISR'de buton kuyruğu doluydu, görev olayı hiç almadı | t0 |

  Kart `REC` satırında düşmenin nerede olduğunu ayrı bildirir; arayüz CSV'ye şartnamedeki
  `drop` değerini yazar (`app.js`: `csvStatus`). Neden kaybolmaz: CSV'de yalnızca t0 dolu olan
  `drop` buton kuyruğunda, t0..t2 dolu olan TX kuyruğunda düşmüştür. `analyze.py` bunu
  `drop_btn_queue` / `drop_tx_queue` olarak ayırır.

  `tx_drop` ve `btn_drop` olayları havuza değil, `stats.c`'deki 32'lik **düşen olay kaydına**
  yazılır (ButtonTask ve ISR tarafından, kritik bölümle). Böylece havuzun tek sahibi kuralı
  bozulmaz ve kuyruğa giremeyen olayların kimliği kaybolmaz.
- **Erişim kuralı:** Buton ISR'si, ButtonTask ve UART TC ISR'si havuza hiç dokunmaz. Hızlı
  yolun sonucu TC ISR'sinden `MSG_BTN_DONE` mesajıyla UartTxTask'a gider ve havuza yalnızca o
  yazar.
- **Havuz dolunca:** Kayıt ezilmez, `pool_overflow` artar. Olay yine canlı gönderilir.
- **Tampon:**
  - Görev yolunda çerçeve `send_frame()` yığınındadır; görev TC'yi beklerken bloklu olduğu için
    tampon aktarım sonuna kadar yaşar. Zaman aşımında gönderim önce iptal edilir (TXE/TC
    kesmeleri kapanır), sonra tampon bırakılır.
  - Hızlı yolda çerçeve **statiktir** (`s_btn_frame`), çünkü aktarım kesme bittikten sonra da
    sürer. Aynı anda hatta tek buton satırı olabildiği için tek tampon yeterlidir.

**Uçuştaki aktarım koruması** ([uart.c](../firmware/Core/Src/uart.c)): Her aktarımın sıfırdan
farklı bir etiketi (`tag`) vardır.

- Görev etiketi yayınlar, sonra gönderimi başlatır.
- TC ISR'si etiketi bir kez okur:
  - Etiket 0 ise damgayı hiçbir yere yazmaz ve `spurious_tc` sayacını artırır.
  - Değilse etiketi ve t₄'ü yazıp görevi uyandırır.
- Görev uyanınca etiketin kendi aktarımına ait olduğunu kontrol eder.
- Hat sahiplenilirken önceki aktarımlardan kalmış bildirim bitleri (`NOTIFY_TX_DONE`,
  `NOTIFY_LINE_FREE`) temizlenir. Böylece geç gelen bir TC bir sonraki olayı kapatamaz.
- `uart_claim_line()` hattı 50 ms içinde alamazsa (takılan buton aktarımı bir kez kurtarıldıktan
  sonra bile) `false` döner. Bu durumda `tx_start_fail` artar, olay `tx_error` olur ve görev
  bildirim beklemez.

**Neden DMA TC değil UART TC:** DMA TC yalnızca son baytın veri yazmacına taşındığını gösterir.
F407'de bu, son stop bitinden yaklaşık bir karakter süresi (~87 µs) önceye denk gelir. Burada
DMA yok, gönderim IT ile yapılıyor:

- Boşta TC bayrağı zaten 1 olduğu için gönderimden önce temizlenir.
- TCIE ancak son bayt yazıldıktan sonra açılır.

Kanıt:
- 115200'de (PressTrace): `t4 − t3` = 5570 µs, teorik hat süresi 5556 µs. DMA TC kullanılsaydı
  değer ~5470 µs olurdu.
- 230400'de (bu depo): `t4 − t3` = 2775–2781 µs, teorik 2773 µs (gerçek baud 230 769). DMA TC
  kullanılsaydı bir karakter (~43 µs) kısa çıkardı.
- Boştaki TC'nin eski bayrağı kullanılsaydı değer birkaç µs olurdu.

**Ölçüm penceresi:**

1. Senaryo seçimi yeni pencere açar: havuz, düşen olay kaydı ve **tüm pencere sayaçları**
   sıfırlanır. Açılıştan beri biriken sayaçlar (buton ve UART modüllerindeki) için pencere
   başındaki değer saklanır ve farkı raporlanır.
2. "Ölçümü bitir" komutuyla kart S0'a geçer (telemetri durur), TX kuyruğu boşalır ve kayıtlar
   dökülür: önce `REC` satırları (havuz, sonra düşen olaylar), ardından `CNT` satırları
   (pencere sayaçları), en son `END` (gönderilen `REC` sayısı).
3. Döküm havuzu boşaltmaz, gerekirse tekrarlanabilir.

Arayüz `REC` satırlarını `event_id` ile canlı BTN satırlarına eşler. `END`'deki sayı ile alınan
`REC` sayısını, `accepted` sayacı ile de kayıt sayısını karşılaştırır; tutmazsa uyarır.

**Pencere sayaçları (`CNT`):** `accepted`, `repeat`, `btn_queue_drop`, `tx_queue_drop`,
`tx_queue_max` (TX kuyruğu tepe doluluğu, 16 üzerinden), `pool_overflow`, `droplog_overflow`,
`tx_timeout`, `tx_start_fail`, `spurious_tc`, `encode_error`, hızlı yol sayaçları
(`btn_direct`, `btn_latched`, `btn_fallback`, `btn_result_lost`), `tel_sent` ve ölçülen gerçek
telemetri periyodu (`tel_period_min_us/avg/max`; S0'da gönderilmez). `tx_queue_max`, TX
kuyruğundan geçen `MSG_BTN_DONE` mesajlarını da sayabilir. Şartname "özet tabloda
kuyruk yüksek su seviyesini ve gerçek telemetri hızını da raporlayın" diyor; ikisi de buradan
gelir ve `analyze.py` `summary.csv`'ye yazar.

## Paket protokolü

Şartnameye göre her mesaj **ASCII metindir, boşluklarla 63 bayta tamamlanır ve 64. bayt LF'dir**;
yani her satır tam 64 bayttır ve hat süresi sabittir (64 × 10 bit / 230400 = 2,78 ms; şartname
tabanı 115200'de 5,56 ms).
Kodlayıcı: [protocol.c](../firmware/Core/Src/protocol.c) (`protocol_encode`), ayrıştırıcı:
[interface/app.js](../interface/app.js) (`processBuffer`, `handleLine`).

| Satır | Biçim | Ne zaman |
|---|---|---|
| TEL | `TEL,<seq>,S<n>,<temp_centi_c>,<temp_raw>,<vdda_mv>,<extra_us>,<period_us>,<txq>` | Telemetri periyodunda (S1–S5) |
| BTN | `BTN,<event_id>,S<n>,PRESSED,<seq>,<t0>,<t1>,<t2>` | Her kabul edilen basışta (ölçülen yanıtın kendisi) |
| ACK | `ACK,<seq>,S<n>` | Senaryo komutuna yanıt, açılışta |
| REC | `REC,S<n>,<event_id>,<t0>,<t1−t0>,<t2−t1>,<t3−t2>,<t4−t3>,<status>` | Yalnızca ölçüm penceresi kapanınca |
| CNT | `CNT,S<n>,<ad>,<değer>` | Yalnızca ölçüm penceresi kapanınca |
| END | `END,S<n>,<REC_sayısı>` | Dökümün sonu |

Örnekler (firmware kodlayıcısının gerçek çıktısı; sağdaki boşluklar 63'e kadar sürer, sonra LF):

```
TEL,1042,S5,3560,1077,2935,5012,10001,3
BTN,17,S3,PRESSED,104,41107677,41110938,41110940
REC,S5,17,41107677,3261,2,9995,5575,ok
REC,S5,18,43000000,4100,2,,,tx_drop
REC,S5,19,44000000,,,,,btn_drop
CNT,S5,tx_queue_max,16
END,S5,32
```

- **63 bayt garantisi:** Biçimler en kötü durumda da sığacak şekilde seçildi (en uzun satır
  60 bayt: 10 haneli t0 + 6 haneli dört süre + `btn_drop`). Yine de metin 63 baytı aşarsa satır
  **kesilmez, hiç gönderilmez** ve `encode_error` sayılır. Bu, bilgisayarda gcc ile en kötü
  değerlerle test edildi.
- **REC'te neden süreler:** Beş mutlak 10 haneli zaman 63 bayta sığmaz. Süreler şartnamedeki
  hesap tablosunun (t₁−t₀, t₂−t₁, t₃−t₂, t₄−t₃) ta kendisidir. Mutlak zamanlar t0'dan toplanarak
  (mod 2³²) bulunur; arayüz CSV'ye mutlak zamanları yazar.
- **Boş alan:** Bilinmeyen ya da ≥ 1 s olan süre **boş bırakılır**, 0 yazılmaz (şartname: "eksik
  zamanı 0 yapmayın").
- **seq:** Yalnızca TEL/BTN/ACK satırlarında vardır ve yalnızca onlar için artar. Numara, satır
  hatta başlatılırken hakemin içinde verilir (hızlı yol ve görev yolu aynı sayacı kullanır);
  bu yüzden hattaki sıra ile numara sırası her zaman aynıdır. Arayüz boşluktan hatta kaybolan
  satırları sayar. REC/CNT/END sayısı ise END ile doğrulanır.
- **Çerçeveleme:** Arayüz LF'ye kadar okur ve satırın tam 64 bayt olduğunu doğrular; olmayan satır
  "hatalı satır" sayılır. Bağlantı anındaki ilk yarım satır sayılmaz.

`tx_queue_drop` **kuyruğa hiç giremeyen** (henüz seq almamış) mesajları, seq boşluğu ise
gönderildiği hâlde bilgisayara ulaşmayan satırları sayar. İki farklı kayıp türüdür ve raporda
ayrı belirtilmelidir.

## Senaryo komutu

Senaryo, yer istasyonundan çalışma anında değiştirilir. Bu, hattaki tek **yer istasyonu → kart**
trafiğidir ve yalnızca kullanıcı bir senaryo düğmesine bastığında oluşur. Ölçüm sırasında
hatta komut trafiği yoktur, bu yüzden "arayüz ölçüme karışmasın" şartı korunur.

**Komut çerçevesi (5 bayt, ikili):** `AA 55 'C' arg checksum`. Şartnamenin 64 baytlık ASCII
kuralı karttan çıkan TEL/BTN mesajları içindir; bu komut ek bir özelliktir (şartname:
"senaryo derleme ayarıyla seçilebilir; MCU tarafında ek komut görevi zorunlu değil").

- `arg` 0..5 ise senaryoyu değiştirir ve yeni ölçüm penceresi açar (kayıt havuzu boşalır).
- `0xFE` ise ölçümü bitirir: kart S0'a geçer ve havuzu döker.
- `0xFF` ise yalnızca sorgular (arayüz bağlanınca gönderir).
- `checksum`, bayt 0..3 toplamının mod 256'sıdır. Geçersiz çerçeveler sessizce atılır.

**Kartta akış:**

1. USART2 RX kesmesi ([uart.c](../firmware/Core/Src/uart.c)) her baytı
   [command.c](../firmware/Core/Src/command.c) içindeki çözücüye verir.
2. Geçerli komut, TelemetryTask'a görev bildirimi (task notification) değeri olarak iletilir.
3. TelemetryTask komutu **periyot sınırında** uygular: bir periyodun işi hiçbir zaman yarıda
   kesilmez ve değişiklikten sonra periyot yeniden başlar. `g_scenario`'yu yalnızca bu görev
   yazar. Buton ISR'si ve ButtonTask yalnızca tek baytlık `id` alanını okur.
4. S0'da görev askıya alınmaz, bir sonraki komutu bekler.
5. Her geçerli komuta (sorgu dahil) aktif senaryoyu taşıyan bir **ACK** satırıyla yanıt verilir. Kart açılışta da bir ACK gönderir.

**Yer istasyonunda:**

- Aktif senaryo yalnızca ACK ile değişir. Arayüz kendi başına bir senaryo varsaymaz.
- ACK, TX kuyruğunda (FIFO) değişiklikten önce kuyruğa girmiş paketlerin arkasından gelir.
  Onay gelince liste temizlenir. Sonradan gelen eski senaryo paketleri (ör. geç bir TEL/BTN)
  `scenario_id` farklı olduğu için yok sayılır. Döküm satırları (`REC`, `CNT`, `END`) bu
  filtreden muaftır, çünkü döküm sırasında kart zaten S0'a geçmiştir.
- ACK 1,5 s içinde gelmezse kırmızı uyarı gösterilir. En olası neden: USB-TTL TX → PA3
  bağlı değil.

**Kesme tarafında dikkat edilenler:**

- ISR içinde önce TC (t4) işlenir, böylece aynı anda gelen bir RX baytı t4'ü geciktirmez.
- RXNEIE, taşma (ORE) durumunda da kesme ürettiği için DR her durumda okunur. Okunmazsa
  kesme durmadan tekrarlanır.

## Telemetri verisi: kart sıcaklığı

TEL paketi, STM32F407'nin dahili sıcaklık sensörünü taşır
([temp_sensor.c](../firmware/Core/Src/temp_sensor.c), ADC1 kanal 16). Ölçülen değer **çip (die)
sıcaklığıdır**; ortam sıcaklığından birkaç derece yüksek olması normaldir.

- **Kalibrasyon:** Datasheet'teki tipik formül (V25 = 0,76 V, 2,5 mV/°C) yerine, çipe fabrikada
  yazılmış iki noktalı kalibrasyon kullanılır: `TS_CAL1` (30 °C, `0x1FFF7A2C`) ve `TS_CAL2`
  (110 °C, `0x1FFF7A2E`). Başlangıç noktası çipten çipe ±45 °C'ye kadar değişebildiği için
  (RM0090), tipik formül mutlak sıcaklık için güvenilir değildir.
- **VDDA düzeltmesi:** Kalibrasyon VDDA = 3,3 V'ta yapılmıştır, Discovery kartında ise VDDA
  ~3,0 V'tur. Açılışta VREFINT (kanal 17, `VREFINT_CAL` @ `0x1FFF7A2A`) 8 kez ölçülüp ortalanır
  ve her ham sıcaklık değeri önce 3,3 V koşuluna ölçeklenir. Bu düzeltme yapılmasaydı ham
  değerdeki ~%10'luk kayma, sensörün düşük eğimi yüzünden ~30 °C'lik hataya dönüşürdü
  (tipik değerlerle: gerçek 35 °C → düzeltmesiz 66 °C, düzeltmeli 34,8 °C). VDDA deney
  süresince sabit kabul edilir.
- **Ölçüme etkisi:** Bir ADC dönüşümü ~23 µs sürer (480 çevrim / 21 MHz; sensör en az 10 µs
  örnekleme ister). TelemetryTask bu süreyi **beklemez**: her periyotta bir önceki periyotta
  başlatılan dönüşümün sonucunu okuyup yenisini başlatır. Böylece görev başına eklenen iş
  birkaç register erişimi ve bir tamsayı bölmesiyle sınırlıdır. S1–S3 karşılaştırması, "yalnızca
  telemetri sıklığı değişir" varsayımını korur. Bunun bedeli, gönderilen örneğin bir periyot
  (10–100 ms) eski olmasıdır; sıcaklık için önemsizdir.
- **S0:** ADC açılışta bir kez başlatılır (~0,2 ms, senaryo çalışma anında değişebildiği
  için). Dönüşümler yalnızca telemetri açıkken yapılır, yani S0'da sürekli bir yük yoktur.
- Paket boyutu sabit (64 bayt) kaldığı için hat kullanımı yalnızca telemetri hızına bağlıdır:
  230400'de 100 Hz'de %27,8, 50 Hz'de %13,9, 10 Hz'de %2,8 (64 bayt × 10 bit / 230400 baud =
  2,78 ms/paket). 115200'de bu oranlar iki katıydı.

## Zaman damgası kaynağı (TIM2)

[timestamp.c](../firmware/Core/Src/timestamp.c): TIM2, APB1 zamanlayıcı saatinden (168 MHz
SYSCLK'te 84 MHz, bkz. [system_clock.c](../firmware/Core/Src/system_clock.c)) 1 MHz'e bölünerek
32-bit serbest sayaç olarak çalıştırılır; hiçbir zaman durdurulmaz veya sıfırlanmaz. Tüm t0..t4
ölçümleri bu ortak sayaçtan okunur, bu yüzden farklar (`t1-t0`, `R=t4-t0` vb.) doğrudan
mikrosaniye cinsindendir ve 32-bit unsigned aritmetiği sayesinde ~71,58 dakikalık sarma
(rollover) durumunda da doğru sonuç verir.

## Kesme öncelikleri

STM32F4 NVIC 4 öncelik biti uygular. `EXTI0_IRQn` ve `USART2_IRQn`,
`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (5) önceliğine ayarlanır — bu, FreeRTOS
`FromISR` API'lerinin güvenle çağrılabileceği en yüksek (en düşük numaralı) önceliktir. Bkz.
[FreeRTOSConfig.h](../firmware/Core/Inc/FreeRTOSConfig.h) içindeki ayrıntılı not (çifte kaydırma
hatasından kaçınma).

Öncelik gruplaması `main()` içinde, herhangi bir öncelik atanmadan önce
`NVIC_SetPriorityGrouping(3)` ile açıkça kurulur: 4 bitin tamamı preemption'dır, alt öncelik
yoktur (HAL'deki `NVIC_PRIORITYGROUP_4`). `configASSERT` tanımlı olduğu için, tavanın üstünde
bir öncelikten yapılan `FromISR` çağrısı `vPortValidateInterruptPriority` içinde assert'e
düşer. Yani yanlış öncelik sessizce geçmez.

**Hakem için önemi:** EXTI0 ve USART2'nin **aynı** preemption önceliğinde olması bilinçlidir.
Aynı öncelikteki iki kesme birbirini kesemez; bu yüzden hızlı yolun hat kararı (EXTI0) ile
TC'deki mandal başlatma (USART2) kendiliğinden atomiktir. Görev tarafındaki `taskENTER_CRITICAL`
BASEPRI'yi 5'e çektiği için ikisini birden maskeler. EXTI0'ı 5'ten daha acil bir değere çekmek
hem bu atomikliği bozar hem de `FromISR` çağrılarını yasaklar.

## Üçüncü parti bileşenler

| Bileşen | Kaynak | Nasıl dahil edildi |
|---|---|---|
| FreeRTOS-Kernel | [FreeRTOS/FreeRTOS-Kernel](https://github.com/FreeRTOS/FreeRTOS-Kernel), commit `8be86d4` (V11.1.0+) | Kaynak dosyalar depoya dahil (`firmware/Middlewares/FreeRTOS-Kernel`); ölçümlerin yapıldığı sürüm sabit |
| ARM CMSIS-Core | [STMicroelectronics/cmsis-core](https://github.com/STMicroelectronics/cmsis-core), `cm4` dalı | Yalnızca gerekli 5 başlık dosyası vendored (`firmware/Drivers/CMSIS/Include/`) — tam CMSIS_5 deposu (~144 MB, DSP/NN/RTOS2 dahil) yerine |
| ST CMSIS STM32F4 Device Paketi | [STMicroelectronics/cmsis_device_f4](https://github.com/STMicroelectronics/cmsis_device_f4) | Yalnızca STM32F407 için gerekli başlık/kaynak/startup/linker dosyaları vendored (`firmware/Drivers/CMSIS/Device/ST/STM32F4xx/`, `firmware/Core/Startup/`, `firmware/EWARM/stm32f407vg_flash.icf`) |

FreeRTOS-Kernel kaynakları depoya dahil olduğu için klonlama sonrası ek adım gerekmez (bkz.
[setup.md](setup.md)).

## Direct Routing (FreeRTOS vektör kurulumu)

Kullanılan FreeRTOS-Kernel sürümü, SVCall/PendSV/SysTick kesmelerinin vektör tablosunda
doğrudan `vPortSVCHandler`/`xPortPendSVHandler`/`xPortSysTickHandler` fonksiyonlarına işaret
etmesini gerektirir ("Direct Routing", bkz. `portable/IAR/ARM_CM4F/port.c` içindeki
`xPortStartScheduler`). [startup_stm32f407xx.s](../firmware/Core/Startup/startup_stm32f407xx.s)
dosyası bu üç vektör girdisi için buna göre düzenlenmiştir; `SVC_Handler`/`PendSV_Handler`/
`SysTick_Handler` adında ayrı zayıf (weak) saplamalar **yoktur**.
