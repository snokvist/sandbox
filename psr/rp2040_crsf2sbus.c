// rp2040_crsf2sbus.c — CRSF-to-SBUS bridge that tolerates LEN=23 *or* LEN=24
//
// RX : CRSF @ 115 200 8N1 on UART0 (pins 1/0)
// TX : SBUS @ 100 000 8E2 (inverted) on UART1 (pin 4)
// Stats line every 1 s on UART0.

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

/* ─── UART wiring ─── */
#define CRSF_UART   uart0
#define CRSF_RX     1
#define CRSF_TX     0
#define CRSF_BAUD   115200

#define SBUS_UART   uart1
#define SBUS_TX     4
#define SBUS_BAUD   100000

/* ─── CRSF constants ─── */
#define DEST_FC     0xC8
#define DEST_EE     0xEE
#define TYPE_RC     0x16
#define MIN_PAYLOAD 22          /* 16×11-bit = 22 B */
#define MAX_LEN     60

/* ─── SBUS constants ─── */
#define SBUS_BYTES  25
#define SBUS_HDR    0x0F
#define SBUS_FOOT   0x00

/* ─── timing ─── */
#define FAILSAFE_MS 100U
#define STATUS_MS   1000U
#define SBUS_GAP_US 16000U

/* ─── CRC-8 / MAXIM (LSB-first, poly 0x8C) ─── */
static uint8_t crc8(const uint8_t *d, int n)
{
    uint8_t c = 0;
    while (n--) {
        c ^= *d++;
        for (int i = 0; i < 8; ++i)
            c = (c & 1) ? (c >> 1) ^ 0x8C : c >> 1;
    }
    return c;
}

/* ─── SBUS pack ─── */
static void sbus_pack(const uint16_t ch[16], uint8_t f[SBUS_BYTES], bool fs)
{
    f[0] = SBUS_HDR;
    for (int i = 1; i <= 22; ++i) f[i] = 0;

    for (int i = 0; i < 16; ++i) {
        uint32_t v = ch[i] & 0x7FF;
        int bit = i * 11, by = 1 + (bit >> 3), sh = bit & 7;
        f[by]     |= v << sh;
        f[by + 1] |= v >> (8  - sh);
        f[by + 2] |= v >> (16 - sh);
    }
    f[23] = fs ? 0x0C : 0x00;
    f[24] = SBUS_FOOT;
}

/* ─── counters ─── */
static uint32_t ok_cnt, alt_crc, crc_err, len_err, dest_skip, type_err;
static uint32_t sbus_cnt, fs_cnt;
static absolute_time_t last_sbus;

/* ─── send SBUS ─── */
static void send_sbus(const uint16_t ch[16], bool fs)
{
    uint8_t fr[SBUS_BYTES];
    sbus_pack(ch, fr, fs);
    uart_write_blocking(SBUS_UART, fr, SBUS_BYTES);
    sbus_cnt++; last_sbus = get_absolute_time();
}

/* ─── streaming parser ─── */
typedef struct { uint8_t buf[64]; int idx, need; } st_t;
static st_t s = { .idx = 0, .need = 0 };

static int feed(uint8_t b, uint16_t ch[16])
{
    if (s.idx == 0) {                       /* need DEST */
        if (b == DEST_FC || b == DEST_EE) s.buf[s.idx++] = b;
        else dest_skip++;
        return 0;
    }

    s.buf[s.idx++] = b;

    if (s.idx == 2) {                       /* LEN byte */
        uint8_t len = s.buf[1];
        if (len < 2 || len > MAX_LEN) { len_err++; s.idx = 0; return 0; }
        s.need = 2 + len;
        return 0;
    }

    if (s.idx < s.need) return 0;           /* frame not finished yet */

    /* Frame complete */
    uint8_t len = s.buf[1];
    uint8_t type = s.buf[2];
    uint8_t crc_rx = s.buf[s.need - 1];
    const uint8_t *data = &s.buf[2];        /* TYPE .. */

    int valid = 0;
    if (type == TYPE_RC && len >= 1 + MIN_PAYLOAD) {
        /* First, spec-compliant CRC over LEN bytes */
        if (crc_rx == crc8(data, len)) valid = 1;
        /* Fallback: some TX count CRC in LEN (LEN-1 data bytes) */
        else if (crc_rx == crc8(data, len - 1)) { valid = 1; alt_crc++; }
    } else {
        type_err++;
    }

    if (valid) {
        const uint8_t *p = &s.buf[3];
        for (int i = 0; i < 16; ++i) {
            int bit = i * 11, by = bit >> 3, sh = bit & 7;
            uint32_t raw = p[by] | p[by + 1] << 8 | p[by + 2] << 16;
            ch[i] = (raw >> sh) & 0x7FF;
        }
        ok_cnt++;
    } else {
        crc_err++;
    }
    s.idx = 0;
    return valid;
}

/* ─── main ─── */
int main(void)
{
    /* CRSF UART0 */
    uart_init(CRSF_UART, CRSF_BAUD);
    gpio_set_function(CRSF_RX, GPIO_FUNC_UART);
    gpio_set_function(CRSF_TX, GPIO_FUNC_UART);
    uart_set_format(CRSF_UART, 8, 1, UART_PARITY_NONE);

    /* SBUS UART1 */
    uart_init(SBUS_UART, SBUS_BAUD);
    uart_set_format(SBUS_UART, 8, 2, UART_PARITY_EVEN);
    gpio_set_function(SBUS_TX, GPIO_FUNC_UART);
    gpio_set_outover(SBUS_TX, GPIO_OVERRIDE_INVERT);

    uint16_t neutral[16]; for (int i = 0; i < 16; ++i) neutral[i] = 992;

    absolute_time_t last_ok = get_absolute_time();
    last_sbus                = get_absolute_time();
    absolute_time_t next_st  = make_timeout_time_ms(STATUS_MS);

    while (true) {
        if (uart_is_readable(CRSF_UART)) {
            uint8_t b = uart_getc(CRSF_UART);
            uint16_t ch[16];
            if (feed(b, ch)) last_ok = get_absolute_time(), send_sbus(ch, false);
        }

        /* failsafe / cadence */
        int32_t d_ok = absolute_time_diff_us(last_ok, get_absolute_time());
        int32_t d_tx = absolute_time_diff_us(last_sbus, get_absolute_time());
        if (d_ok > FAILSAFE_MS * 1000U && d_tx > 7000) fs_cnt++, send_sbus(neutral,true);
        else if (d_tx > SBUS_GAP_US) send_sbus(neutral,false);

        /* 1-second stats */
        if (absolute_time_diff_us(get_absolute_time(), next_st) <= 0) {
            next_st = delayed_by_ms(next_st, STATUS_MS);
            char line[160];
            int n = snprintf(line, sizeof(line),
              "OK:%lu ALTCRC:%lu CRC:%lu LEN:%lu DEST:%lu TYPE:%lu SBUS:%lu FS:%lu\r\n",
              ok_cnt, alt_crc, crc_err, len_err, dest_skip, type_err, sbus_cnt, fs_cnt);
            uart_write_blocking(CRSF_UART, (uint8_t*)line, n);
        }
    }
}
