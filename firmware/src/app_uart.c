#include "tl_common.h"

#include "app_main.h"

static uart_data_t rx_dma __attribute__((aligned(4))) = {0, {0}};
static uint8_t  ring[UART_RING_SIZE];
static volatile uint16_t ring_head;     /* written in the RX callback */
static volatile uint16_t ring_tail;     /* written in the main loop */
static uint32_t rx_dropped;
static uint32_t baudrate = UART_BAUDRATE_115200;

static void app_uartRecvCb(void) {
    uint32_t len = rx_dma.dma_len;

    if (len == 0 || len > UART_DATA_LEN - 4) {
        return;
    }

    for (uint32_t i = 0; i < len; i++) {
        uint16_t next = (ring_head + 1) & (UART_RING_SIZE - 1);
        if (next == ring_tail) {
            rx_dropped += len - i;
            break;
        }
        ring[ring_head] = rx_dma.data[i];
        ring_head = next;
    }
}

void app_uart_init(uint32_t rate) {
    baudrate = rate;
    ring_head = ring_tail = 0;
    drv_uart_pin_set(GPIO_UART_TX, GPIO_UART_RX);
    drv_uart_init(baudrate, (uint8_t *)&rx_dma, sizeof(uart_data_t), app_uartRecvCb);
}

uint32_t app_uart_baudrate(void) {
    return baudrate;
}

uint8_t app_uart_tx(uint8_t *data, uint8_t len) {
    return drv_uart_tx_start(data, len) ? 1 : 0;
}

uint8_t app_uart_rx_byte(uint8_t *b) {
    if (ring_tail == ring_head) {
        return 0;
    }
    *b = ring[ring_tail];
    ring_tail = (ring_tail + 1) & (UART_RING_SIZE - 1);
    return 1;
}

uint32_t app_uart_rx_dropped(void) {
    return rx_dropped;
}

void app_uart_poll(void) {
    drv_uart_exceptionProcess();
}
