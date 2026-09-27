#ifndef TELEMETRY_TASK_H
#define TELEMETRY_TASK_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"

/* En yuksek oncelikli (PRIO_TELEMETRY_TASK = 3) gorev. g_scenario.telemetry_hz
 * periyodunda TEL paketi uretir; S4/S5'te periyot basina ~Cek ms'lik ek CPU
 * isini (workload_run) de bu gorev icinde calistirir — boylece Telemetry,
 * daha dusuk oncelikli ButtonTask'i kesintiye ugratarak yapay CPU yuku
 * senaryosunu olusturur. Yer istasyonunun senaryo komutlarini da (command.c
 * uzerinden gelen bildirim) periyot sinirinda uygular ve ACK ile yanitlar. */
void telemetry_task_create(QueueHandle_t uart_tx_queue);

/* Olcum penceresi icin gercek telemetri uretim periyodu (ardisik iki TEL
 * uretiminin baslangici arasindaki sure). Pencere, senaryo secilince
 * sifirlanir. Yalnizca telemetri durmusken (dokum sirasinda) okunmalidir. */
typedef struct
{
    uint32_t sent;      /* TX kuyruguna giren TEL */
    uint32_t periods;   /* olculen periyot sayisi */
    uint32_t min_us;
    uint32_t max_us;
    uint64_t sum_us;
} telemetry_stats_t;

void telemetry_get_stats(telemetry_stats_t *out);

#endif /* TELEMETRY_TASK_H */
