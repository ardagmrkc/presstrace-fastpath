#ifndef COMMAND_H
#define COMMAND_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

/* Gecerli komutlar target gorevine bildirim (notification) degeri olarak
 * iletilir: 0..5 = senaryo, CMD_ARG_DUMP = olcumu bitir, CMD_ARG_QUERY =
 * sorgu. Scheduler
 * baslamadan once cagrilmalidir. */
void command_init(TaskHandle_t target);

/* USART2 ISR'si her alinan bayt icin cagirir. Komut cercevesini
 * (protocol.h: AA 55 'C' arg checksum) bayt bayt cozer; gecersiz
 * cerceveleri sessizce atar. */
void command_rx_byte_from_isr(uint8_t b, BaseType_t *higher_prio_woken);

#endif /* COMMAND_H */
