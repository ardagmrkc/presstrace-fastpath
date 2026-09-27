#include "main.h"
#include "system_clock.h"
#include "timestamp.h"
#include "protocol.h"
#include "telemetry_task.h"
#include "button_task.h"
#include "uart_tx_task.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* README.md senaryo tablosu (S0..S5) ile birebir eslesir. */
const scenario_config_t g_scenarios[SCENARIO_COUNT] = {
    {0, 0,   0}, /* S0: kapali, referans */
    {1, 10,  0}, /* S1: 10 Hz */
    {2, 50,  0}, /* S2: 50 Hz */
    {3, 100, 0}, /* S3: 100 Hz */
    {4, 100, 2}, /* S4: 100 Hz + ~2 ms ek is */
    {5, 100, 5}, /* S5: 100 Hz + ~5 ms ek is */
};

#if (ACTIVE_SCENARIO < 0) || (ACTIVE_SCENARIO > 5)
#error "ACTIVE_SCENARIO 0..5 araliginda olmalidir (S0..S5)"
#endif

scenario_config_t g_scenario;

int main(void)
{
    system_clock_config();

    /* 4 oncelik bitinin tamami preemption, alt oncelik yok (HAL'deki
     * NVIC_PRIORITYGROUP_4). FreeRTOS portu bunu varsayar; kesme
     * oncelikleri bu satirdan sonra atanir. */
    NVIC_SetPriorityGrouping(3U);

    timestamp_init();

    /* Acilis senaryosu; sonrasinda yer istasyonu komutuyla TelemetryTask
     * degistirir. */
    g_scenario = g_scenarios[ACTIVE_SCENARIO];

    QueueHandle_t uart_tx_queue = xQueueCreate(UART_TX_QUEUE_LEN, sizeof(tx_message_t));
    configASSERT(uart_tx_queue != NULL);

    telemetry_task_create(uart_tx_queue);
    button_task_create(uart_tx_queue);
    uart_tx_task_create(uart_tx_queue);

    vTaskStartScheduler();

    /* Buraya yalnizca heap yetersizse (xTaskCreate/xQueueCreate icin ilk
     * tahsisler basarili oldugundan, esas olarak scheduler'in kendi ic
     * yapilari icin) ulasilir. */
    for (;;)
    {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}
