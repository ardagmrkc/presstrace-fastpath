#ifndef BUTTON_TASK_H
#define BUTTON_TASK_H

#include "FreeRTOS.h"
#include "queue.h"

/* Orta oncelikli (PRIO_BUTTON_TASK = 2) gorev: buton olaylarinin YEDEK yolu.
 * Normalde buton satiri EXTI0 ISR'sinden hizli yolla (uart.c) gonderilir;
 * hizli yol reddederse (mandal dolu ya da UART henuz hazir degil) olay
 * evt_queue'ya gelir. Bu gorev olayi alir (t1), BTN mesaji olusturup (t2)
 * uart_tx_queue'ya gonderir. Kendi GPIO/EXTI kuyrugunu olusturur ve
 * button_init() ile kaydeder. */
void button_task_create(QueueHandle_t uart_tx_queue);

#endif /* BUTTON_TASK_H */
