#include "button.h"
#include "main.h"
#include "timestamp.h"
#include "stats.h"
#include "uart.h"

static QueueHandle_t s_evt_queue = NULL;

/* Yalnizca ISR yazar/okur (init disinda). */
static uint32_t s_last_edge_us;
static bool s_have_edge;
static bool s_pressed;
static uint16_t s_event_id;

/* ISR yazar, gorevler okur: 32-bit hizali okuma Cortex-M4'te atomiktir. */
static volatile uint32_t s_repeat_count;
static volatile uint32_t s_drop_count;
static volatile uint32_t s_accepted_count;
static volatile uint32_t s_fallback_count;

static bool button_is_high(void)
{
    return (BUTTON_GPIO_PORT->IDR & (1UL << BUTTON_GPIO_PIN)) != 0U;
}

static uint16_t next_id(void)
{
    return ++s_event_id;
}

/* Iki kenar da buraya gelir. Bir kenar ancak oncesinde en az
 * BUTTON_REPEAT_WINDOW_US boyunca hic kenar yoksa (veya ilk kenarsa)
 * gecerlidir; aksi halde sicramadir, sayilir ve pencereyi yeniden baslatir.
 * Yalnizca "basis kenari + son kabulden bu yana gecen sure" yetmez:
 * birakma sirasindaki sicrama da basis yonunde kenar uretir ve basistan
 * >30 ms sonra geldigi icin yeni basis sanilir. Bu yuzden buton durumu
 * (basili/birakilmis) takip edilir; olay yalnizca birakilmis durumdayken
 * gelen basista uretilir. */
static bool accept_edge(uint32_t now_us, bool high)
{
    if (s_have_edge && (uint32_t)(now_us - s_last_edge_us) < BUTTON_REPEAT_WINDOW_US)
    {
        s_last_edge_us = now_us;
        s_repeat_count++;
        return false;
    }
    s_have_edge = true;
    s_last_edge_us = now_us;

    if (!high)
    {
        s_pressed = false; /* birakma: olay uretmez */
        return false;
    }
    if (s_pressed)
    {
        return false; /* zaten basili: basili tutarken gelen parazit */
    }
    s_pressed = true;
    return true;
}

void button_init(QueueHandle_t evt_queue)
{
    s_evt_queue = evt_queue;

    /* --- GPIO: PA0 giris, pull yok (kartta harici pull-down; B1 aktif-HIGH,
     *     yani basis kenari YUKSELEN kenardir) --- */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    BUTTON_GPIO_PORT->MODER &= ~(0x3UL << (BUTTON_GPIO_PIN * 2U)); /* 00 = input */
    BUTTON_GPIO_PORT->PUPDR &= ~(0x3UL << (BUTTON_GPIO_PIN * 2U)); /* 00 = no pull */

    /* --- Gozlem LED'i: PD12 push-pull cikis --- */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    LED_GPIO_PORT->MODER = (LED_GPIO_PORT->MODER & ~(0x3UL << (LED_GPIO_PIN * 2U))) |
                           (0x1UL << (LED_GPIO_PIN * 2U)); /* 01 = output */
    LED_GPIO_PORT->BSRR = (1UL << (LED_GPIO_PIN + 16U));   /* baslangicta sondurulu */

    /* Aciliste basili tutulan buton, birakilip yeniden basilana kadar olay
     * uretmez. s_have_edge = false: ilk gercek kenar her zaman kabul edilir. */
    s_pressed = button_is_high();
    s_have_edge = false;

    /* --- EXTI0'i PA0'a bagla (SYSCFG_EXTICR1, EXTI0 alani = 0000b = PA) --- */
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    SYSCFG->EXTICR[0] &= ~SYSCFG_EXTICR1_EXTI0;

    /* Iki kenar da dinlenir: birakma kenari filtrenin buton durumunu
     * takip edebilmesi icin gereklidir. */
    EXTI->IMR  |= (1UL << BUTTON_EXTI_LINE);
    EXTI->RTSR |= (1UL << BUTTON_EXTI_LINE);
    EXTI->FTSR |= (1UL << BUTTON_EXTI_LINE);
    EXTI->PR    = (1UL << BUTTON_EXTI_LINE);

    /* FromISR cagiran kesme, FreeRTOS sistem cagrisi tavaninda veya daha
     * dusuk aciliyette olmalidir (Cortex-M: kucuk sayi = daha acil).
     * configASSERT tanimli oldugu icin yanlis oncelik, ilk FromISR
     * cagrisinda assert'e duser. Kesme EN SON acilir. */
    NVIC_SetPriority(EXTI0_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
    NVIC_ClearPendingIRQ(EXTI0_IRQn);
    NVIC_EnableIRQ(EXTI0_IRQn);
}

void button_exti_isr_handler(void)
{
    /* Once zaman, sonra bayrak: damga kenara en yakin an olsun. */
    const uint32_t now_us = timestamp_now_us();

    if ((EXTI->PR & (1UL << BUTTON_EXTI_LINE)) == 0U)
    {
        return; /* bu hat icin bekleyen bayrak yok */
    }
    EXTI->PR = (1UL << BUTTON_EXTI_LINE); /* yazarak temizlenir */

    if (!accept_edge(now_us, button_is_high()))
    {
        return;
    }

    button_event_t evt = { next_id(), now_us }; /* t0 = kabul edilen basis kenari */
    s_accepted_count++;

    LED_GPIO_PORT->ODR ^= (1UL << LED_GPIO_PIN);

    BaseType_t wake = pdFALSE;

    /* Hizli yol (uart.c): buton satiri gorevleri beklemeden, hat bossa
     * hemen, mesgulse hattaki satir biter bitmez gonderilir. Gorev
     * oncelikleri degismez; bu yol NVIC duzleminde (oncelik 5) calisir. */
    if (uart_btn_submit_from_isr(evt.event_id, g_scenario.id, evt.t0_us, &wake) ==
        UART_BTN_REJECTED)
    {
        /* Yedek yol: mandal dolu (bir satir zaten bekliyor) ya da UART henuz
         * hazir degil. Olay eskisi gibi ButtonTask'a gider. */
        s_fallback_count++;
        if (xQueueSendFromISR(s_evt_queue, &evt, &wake) != pdPASS)
        {
            /* Olay kaybolmaz: kimligi ve t0'i olcum sonunda REC(btn_drop) olur. */
            s_drop_count++;
            stats_droplog_add_from_isr(evt.event_id, g_scenario.id, evt.t0_us);
        }
    }
    portYIELD_FROM_ISR(wake);
}

uint32_t button_get_accepted_count(void)
{
    return s_accepted_count;
}

uint32_t button_get_repeat_count(void)
{
    return s_repeat_count;
}

uint32_t button_get_drop_count(void)
{
    return s_drop_count;
}

uint32_t button_get_fallback_count(void)
{
    return s_fallback_count;
}
