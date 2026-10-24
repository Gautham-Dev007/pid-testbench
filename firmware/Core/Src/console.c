#include "console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "usart.h"

#define RX_SIZE   256u
#define TX_SIZE   2048u
#define LINE_MAX  64u

static volatile uint16_t rx_head, rx_tail;
static uint8_t           rx_buf[RX_SIZE];
static uint8_t           rx_byte;

static volatile uint16_t tx_head, tx_tail;
static volatile uint16_t tx_inflight;
static uint8_t           tx_buf[TX_SIZE];

static char     line[LINE_MAX];
static uint16_t line_len;
static bool     line_overflow;

static uint32_t irq_save(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  return primask;
}

static void irq_restore(uint32_t primask) { __set_PRIMASK(primask); }

static void rx_arm(void) { HAL_UART_Receive_IT(&huart2, &rx_byte, 1); }

/* Starts the next contiguous chunk if the UART is idle. */
static void tx_kick(void)
{
  uint32_t s = irq_save();
  if (tx_inflight == 0 && tx_head != tx_tail) {
    uint16_t head = tx_head, tail = tx_tail;
    uint16_t len  = head > tail ? (uint16_t)(head - tail) : (uint16_t)(TX_SIZE - tail);
    if (HAL_UART_Transmit_IT(&huart2, &tx_buf[tx_tail], len) == HAL_OK) tx_inflight = len;
  }
  irq_restore(s);
}

void console_init(void)
{
  rx_head = rx_tail = 0;
  tx_head = tx_tail = 0;
  tx_inflight = 0;
  line_len = 0;
  line_overflow = false;
  rx_arm();
}

void console_write(const char *data, size_t len)
{
  uint32_t s = irq_save();
  for (size_t i = 0; i < len; i++) {
    uint16_t next = (uint16_t)((tx_head + 1u) % TX_SIZE);
    if (next == tx_tail) break;  /* full: drop the rest */
    tx_buf[tx_head] = (uint8_t)data[i];
    tx_head = next;
  }
  irq_restore(s);
  tx_kick();
}

void console_printf(const char *fmt, ...)
{
  char    buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if ((size_t)n >= sizeof buf) n = sizeof buf - 1;
  console_write(buf, (size_t)n);
}

bool console_read_line(char *out, size_t size)
{
  while (rx_tail != rx_head) {
    char ch = (char)rx_buf[rx_tail];
    rx_tail = (uint16_t)((rx_tail + 1u) % RX_SIZE);

    if (ch == '\r' || ch == '\n') {
      if (line_overflow) {
        line_overflow = false;
        line_len = 0;
        console_printf("ERR line too long\r\n");
        continue;
      }
      if (line_len == 0) continue;  /* blank line, or the LF of a CRLF */
      size_t n = line_len < size - 1 ? line_len : size - 1;
      memcpy(out, line, n);
      out[n] = '\0';
      line_len = 0;
      return true;
    }

    if (line_len < LINE_MAX - 1) line[line_len++] = ch;
    else line_overflow = true;
  }
  return false;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart != &huart2) return;
  uint16_t next = (uint16_t)((rx_head + 1u) % RX_SIZE);
  if (next != rx_tail) {
    rx_buf[rx_head] = rx_byte;
    rx_head = next;
  }
  rx_arm();
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart != &huart2) return;
  tx_tail = (uint16_t)((tx_tail + tx_inflight) % TX_SIZE);
  tx_inflight = 0;
  tx_kick();
}

/* An overrun or framing error aborts the receive; without re-arming here
 * the console would go deaf after the first bit of line noise. */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart != &huart2) return;
  __HAL_UART_CLEAR_OREFLAG(huart);
  __HAL_UART_CLEAR_FEFLAG(huart);
  __HAL_UART_CLEAR_NEFLAG(huart);
  rx_arm();
}
