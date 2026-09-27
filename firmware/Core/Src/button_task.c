#include "button_task.h"
#include "main.h"
#include "button.h"
#include "protocol.h"
#include "timestamp.h"
#include "stats.h"
#include "task.h"

static QueueHandle_t s_tx_queue;
static QueueHandle_t s_evt_queue;
static StaticQueue_t s_evt_queue_ctrl;
static uint8_t s_evt_queue_items[BUTTON_EVENT_QUEUE_LEN * sizeof(button_event_t)];

static void button_task(void *arg)
{
    (void)arg;
    button_event_t evt;

    for (;;)
    {
        if (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        /* t1: ButtonTask olayi aldiktan hemen sonra (olay aktarimi + CPU
         * beklemesi dahil edilmis olur, cunku bu satir schedule edilmeden
         * once ISR->kuyruk->context-switch zinciri tamamlanmis olmalidir). */
        uint32_t t1_us = timestamp_now_us();

        tx_message_t msg = { .type = MSG_BTN };
        msg.scenario_id = g_scenario.id;
        msg.u.btn.event_id = evt.event_id;
        msg.u.btn.t0 = evt.t0_us;
        msg.u.btn.t1 = t1_us;

        /* t2: xQueueSend cagrisindan hemen once. */
        msg.u.btn.t2 = timestamp_now_us();

        if (xQueueSend(s_tx_queue, &msg, 0) == pdPASS)
        {
            stats_note_tx_depth(uxQueueMessagesWaiting(s_tx_queue));
        }
        else
        {
            /* Olcum zinciri burada kirilir, ama olay kaybolmaz: kimligi ve
             * t0..t2 olcum sonunda REC(tx_drop) olarak gonderilir. */
            stats_record_tx_drop();
            stats_droplog_add(evt.event_id, msg.scenario_id, msg.u.btn.t0, msg.u.btn.t1,
                              msg.u.btn.t2);
        }
    }
}

void button_task_create(QueueHandle_t uart_tx_queue)
{
    s_tx_queue = uart_tx_queue;

    /* Sira: kuyruk -> gorev -> kesme. ISR'nin yazacagi kuyruk, kesme
     * acilmadan once hazir olmalidir. */
    s_evt_queue = xQueueCreateStatic(BUTTON_EVENT_QUEUE_LEN, sizeof(button_event_t),
                                     s_evt_queue_items, &s_evt_queue_ctrl);
    configASSERT(s_evt_queue != NULL);

    (void)xTaskCreate(button_task, "ButtonTask", configMINIMAL_STACK_SIZE * 2U,
                       NULL, PRIO_BUTTON_TASK, NULL);

    button_init(s_evt_queue); /* GPIO/EXTI kurulur, NVIC en son acilir */
}
