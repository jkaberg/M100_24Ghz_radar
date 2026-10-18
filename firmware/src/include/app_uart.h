#ifndef SRC_INCLUDE_APP_UART_H_
#define SRC_INCLUDE_APP_UART_H_

/* UART link to the radar MCU (ZT3L TXD = PB1, RXD = PB7). RX is DMA-driven; the callback copies
 * each chunk into a ring buffer that the main loop drains. */

#define UART_DATA_LEN           256     /* the DMA itself fills at most UART_DATA_LEN - 4 */
#define UART_RING_SIZE          1024    /* power of two */

#define UART_BAUDRATE_9600      9600
#define UART_BAUDRATE_115200    115200

typedef struct {
    uint32_t dma_len;           /* written by the DMA, must stay 4 bytes */
    uint8_t  data[UART_DATA_LEN];
} uart_data_t;

void     app_uart_init(uint32_t baudrate);
uint32_t app_uart_baudrate(void);
/* Returns 1 if the frame was handed to the UART, 0 if the transmitter is still busy. */
uint8_t  app_uart_tx(uint8_t *data, uint8_t len);
/* Returns 1 and the next received byte in *b, or 0 when empty. */
uint8_t  app_uart_rx_byte(uint8_t *b);
uint32_t app_uart_rx_dropped(void);
/* Must run in the main loop: recovers the driver after RX errors. */
void     app_uart_poll(void);

#endif /* SRC_INCLUDE_APP_UART_H_ */
