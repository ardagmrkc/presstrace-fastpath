#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx.h"

/* ------------------------------------------------------------------------
 * Senaryo secimi — README.md bolum 3 ile birebir eslesir. Senaryo calisma
 * aninda yer istasyonundan degistirilir (docs/code-notes.md, "Senaryo
 * komutu"). ACTIVE_SCENARIO yalnizca ACILISTAKI senaryodur.
 * ------------------------------------------------------------------------ */
#ifndef ACTIVE_SCENARIO
#define ACTIVE_SCENARIO 5 /* 0=S0 ... 5=S5 */
#endif

typedef struct
{
    uint8_t  id;              /* 0..5, TEL/BTN paketlerindeki scenario_id alani */
    uint16_t telemetry_hz;    /* 0 = telemetri kapali (S0) */
    uint16_t extra_load_ms;   /* TelemetryTask icinde hedeflenen ek CPU isi (0, 2 veya 5) */
} scenario_config_t;

#define SCENARIO_COUNT 6U

/* S0..S5 tablosu (main.c). */
extern const scenario_config_t g_scenarios[SCENARIO_COUNT];

/* Aktif senaryo. main() acilista ACTIVE_SCENARIO ile yazar; sonrasinda
 * YALNIZCA TelemetryTask, bir periyot sinirinda degistirir. Diger gorevler
 * yalnizca .id alanini (tek bayt, atomik) okur. */
extern scenario_config_t g_scenario;

/* ------------------------------------------------------------------------
 * Deney butcesi
 * ------------------------------------------------------------------------ */
#define RESPONSE_DEADLINE_US   20000U /* R = t4 - t0 <= 20 ms */

/* ------------------------------------------------------------------------
 * Gorev oncelikleri: Telemetry (3) > Button (2) > UartTx (1)
 * ------------------------------------------------------------------------ */
#define PRIO_UART_TX_TASK   (tskIDLE_PRIORITY + 1U)
#define PRIO_BUTTON_TASK    (tskIDLE_PRIORITY + 2U)
#define PRIO_TELEMETRY_TASK (tskIDLE_PRIORITY + 3U)

/* ------------------------------------------------------------------------
 * Kuyruk derinlikleri
 * ------------------------------------------------------------------------ */
#define BUTTON_EVENT_QUEUE_LEN  8U  /* ISR -> ButtonTask */
#define UART_TX_QUEUE_LEN       16U /* ButtonTask/TelemetryTask -> UartTxTask, FIFO */

/* ------------------------------------------------------------------------
 * Donanim atamalari — STM32F407VG Discovery
 * ------------------------------------------------------------------------ */
/* Kullanici butonu B1 (mavi), aktif-HIGH, kart uzerinde harici pull-down. */
#define BUTTON_GPIO_PORT    GPIOA
#define BUTTON_GPIO_PIN     0U
#define BUTTON_EXTI_LINE    0U

/* Gozlem LED'i (yesil, PD12) — ButtonTask her olayi islediginde toggle eder. */
#define LED_GPIO_PORT       GPIOD
#define LED_GPIO_PIN        12U

/* USART2: PA2 = TX, PA3 = RX (AF7). Discovery kartinda USB-UART koprusu
 * YOKTUR; harici bir USB-TTL adaptor gerekir (bkz. docs/setup.md). */
#define UART_PERIPH         USART2
#define UART_TX_GPIO_PORT   GPIOA
#define UART_TX_GPIO_PIN    2U
#define UART_RX_GPIO_PORT   GPIOA
#define UART_RX_GPIO_PIN    3U
#define UART_GPIO_AF        7U

/* UART hizi. Sartnamedeki taban deger ve teslim edilen S0-S5 olcumleri
 * 115200 ile alindi. Jitter deneyi icin 230400'e cikarildi: 64 baytlik satir
 * 5,556 ms -> 2,778 ms; S5'te 5 + 2,78 < 10 ms oldugu icin kuyruk birikmez.
 * BRR 42 MHz'den 11,375 (gercek 230 769 baud, +%0,16).
 * Tabana donmek icin 115200 satirini acip 230400 satirini kapatin;
 * interface/app.js icindeki baudRate de ayni degere cekilmelidir. */
/* #define UART_BAUDRATE       115200U */
#define UART_BAUDRATE       230400U

/* Tekrar-kenar filtresi: bir kenarin gecerli sayilmasi icin oncesinde
 * gereken kenarsiz (sessiz) sure. Daha yakin gelen kenarlar sicrama
 * (bounce) sayilir, repeat_count'u arttirir. */
#define BUTTON_REPEAT_WINDOW_US  30000U /* 30 ms */

#endif /* MAIN_H */
