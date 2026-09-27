#include "telemetry_task.h"
#include "main.h"
#include "protocol.h"
#include "timestamp.h"
#include "workload.h"
#include "stats.h"
#include "temp_sensor.h"
#include "command.h"
#include "task.h"

static QueueHandle_t s_tx_queue;

/* Pencere istatistikleri: yalnizca bu gorev yazar. */
static telemetry_stats_t s_stats;
static uint32_t s_last_start_us;
static bool s_have_last;

static void tel_stats_reset(void)
{
    s_stats = (telemetry_stats_t){ .min_us = UINT32_MAX };
    s_have_last = false;
}

void telemetry_get_stats(telemetry_stats_t *out)
{
    *out = s_stats;
}

/* Kontrol mesajlari (ACK, havuz sifirlama, dokum) BLOKLAYARAK kuyruga
 * konur: kaybolmalari pencerelerin karismasi demektir. Yalnizca kullanici
 * bir secim yaptiginda olur; UartTxTask kuyrugu bosalttikca ilerler. */
static void send_control(msg_type_t type)
{
    tx_message_t msg = { .type = type };
    msg.scenario_id = g_scenario.id;
    (void)xQueueSend(s_tx_queue, &msg, portMAX_DELAY);
}

/* Yer istasyonu komutu (command.c):
 *   0..5          senaryoyu degistir = yeni olcum penceresi (havuz bosalir)
 *   CMD_ARG_DUMP  olcumu bitir: telemetri susar (S0), havuz dokulur
 *   CMD_ARG_QUERY yalnizca sorgu
 * Her durumda aktif senaryo ACK ile bildirilir. Sira FIFO'da korunur:
 * sifirlama ACK'ten once, dokum ACK'ten sonra islenir. */
static void handle_command(uint32_t arg)
{
    if (arg < SCENARIO_COUNT)
    {
        g_scenario = g_scenarios[arg];
        tel_stats_reset();
        send_control(MSG_CMD_POOL_RESET);
        send_control(MSG_ACK);
    }
    else if (arg == CMD_ARG_DUMP)
    {
        /* Telemetri durur (S0); pencere istatistikleri dokum icin korunur. */
        g_scenario = g_scenarios[0];
        send_control(MSG_ACK);
        send_control(MSG_CMD_DUMP);
    }
    else
    {
        send_control(MSG_ACK);
    }
}

static void send_telemetry(void)
{
    /* Gercek uretim periyodu: ardisik iki uretimin baslangici arasi. */
    const uint32_t start_us = timestamp_now_us();
    uint32_t period_us = 0;
    if (s_have_last)
    {
        period_us = start_us - s_last_start_us;
        s_stats.periods++;
        s_stats.sum_us += period_us;
        if (period_us < s_stats.min_us)
        {
            s_stats.min_us = period_us;
        }
        if (period_us > s_stats.max_us)
        {
            s_stats.max_us = period_us;
        }
    }
    s_last_start_us = start_us;
    s_have_last = true;

    const uint32_t workload_iters =
        (g_scenario.extra_load_ms == 0U) ? 0U :
        (g_scenario.extra_load_ms <= 2U) ? WORKLOAD_ITERS_2MS : WORKLOAD_ITERS_5MS;

    uint32_t extra_load_us = 0;
    if (workload_iters > 0U)
    {
        uint32_t t_start = timestamp_now_us();
        (void)workload_run(workload_iters);
        extra_load_us = timestamp_now_us() - t_start;
    }

    temp_sample_t temp;
    temp_sensor_poll(&temp);

    tx_message_t msg = { .type = MSG_TEL };
    msg.scenario_id = g_scenario.id;
    msg.u.tel.temp_centi_c = temp.temp_centi_c;
    msg.u.tel.temp_raw = temp.raw;
    msg.u.tel.vdda_mv = temp.vdda_mv;
    msg.u.tel.extra_load_us = extra_load_us;
    msg.u.tel.period_us = period_us;
    msg.u.tel.txq_depth = (uint8_t)uxQueueMessagesWaiting(s_tx_queue);

    /* Bloklamayan gonderim: telemetri periyodikligi, dolu bir TX
     * kuyrugu yuzunden asla gecikmemelidir. Basarisiz olursa bu ornek
     * dusurulur ve pencere sayacina (tx_queue_drop) yazilir. */
    if (xQueueSend(s_tx_queue, &msg, 0) == pdPASS)
    {
        s_stats.sent++;
        stats_note_tx_depth(uxQueueMessagesWaiting(s_tx_queue));
    }
    else
    {
        stats_record_tx_drop();
    }
}

static void telemetry_task(void *arg)
{
    (void)arg;
    uint32_t cmd;

    /* Tek seferlik ~0,2 ms'lik baslatma; surekli yuk degildir. Donusumler
     * yalnizca telemetri acikken (S1-S5) yapilir. */
    temp_sensor_init();
    tel_stats_reset();
    send_control(MSG_ACK); /* acilis senaryosunu bildir */

    TickType_t last_wake = xTaskGetTickCount();

    for (;;)
    {
        if (g_scenario.telemetry_hz == 0U)
        {
            /* S0: periyodik is yok, yalnizca bir komut beklenir. */
            (void)xTaskNotifyWait(0U, UINT32_MAX, &cmd, portMAX_DELAY);
            handle_command(cmd);
            last_wake = xTaskGetTickCount();
            continue;
        }

        /* Komut yalnizca periyot sinirinda uygulanir: bir periyot icindeki
         * is hic yarida kesilmez. Degisiklikte periyot yeniden baslar. */
        if (xTaskNotifyWait(0U, UINT32_MAX, &cmd, 0) == pdTRUE)
        {
            handle_command(cmd);
            last_wake = xTaskGetTickCount();
            if (g_scenario.telemetry_hz == 0U)
            {
                continue;
            }
        }

        send_telemetry();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000U / g_scenario.telemetry_hz));
    }
}

void telemetry_task_create(QueueHandle_t uart_tx_queue)
{
    TaskHandle_t handle = NULL;

    s_tx_queue = uart_tx_queue;
    (void)xTaskCreate(telemetry_task, "TelemetryTask", configMINIMAL_STACK_SIZE * 2U,
                       NULL, PRIO_TELEMETRY_TASK, &handle);
    configASSERT(handle != NULL);
    command_init(handle);
}
