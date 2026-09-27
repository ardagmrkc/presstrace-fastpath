#ifndef UART_TX_TASK_H
#define UART_TX_TASK_H

#include "FreeRTOS.h"
#include "queue.h"
#include "uart.h"

/* En dusuk oncelikli (PRIO_UART_TX_TASK = 1) gorev. uart_tx_queue'dan FIFO
 * sirayla mesaj alir, hatti uart.c'deki hakemden sahiplenip gonderir (t3) ve
 * TC tamamlanmasini sureli bekler (t4). Olcum kaydi havuzunun (64) tek
 * sahibidir: hem kendi gonderdigi (yedek yol) BTN olaylarini hem de hizli
 * yoldan gonderilen BTN olaylarinin sonuclarini (MSG_BTN_DONE) buraya yazar.
 * Havuz yalnizca "olcumu bitir" komutuyla REC + CNT + END olarak dokulur. */
void uart_tx_task_create(QueueHandle_t uart_tx_queue);

/* UART TC ISR'si (ya da hakemin kritik bolumu) tarafindan cagrilir: hizli
 * yoldaki bir buton satirinin sonucunu MSG_BTN_DONE olarak TX kuyruguna
 * koyar. Kuyruk doluysa sonuc kaybolur ve btn_result_lost sayilir. */
void uart_tx_task_btn_done_from_isr(const uart_btn_result_t *res, BaseType_t *woken);

#endif /* UART_TX_TASK_H */
