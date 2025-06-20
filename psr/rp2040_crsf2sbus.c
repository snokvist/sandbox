// rp2040_crsf2sbus.c – standalone CRSF-to-SBUS relay with stats
//
//  * RX  : CRSF frames @ 460 800 8N1 on UART0 (pin 1)
//  * TX  : SBUS stream  @ 100 000 8E2, **inverted** via GPIO OUTOVER on UART1 (pin 4)
//  * TX0 : ASCII status line every 1000 ms on UART0 (pin 0)
//
// Build (Pico-SDK):
//   mkdir build && cd build && cmake .. && make -j
// Flash: copy UF2 to RP2040 BOOTSEL drive on the X2L.

#include <stdio.h>               // snprintf
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

/* ──────────── CRSF constants ──────────── */
#define CRSF_UART              uart0
#define CRSF_TX_PIN            0      // stats out
#define CRSF_RX_PIN            1      // CRSF in
#define CRSF_BAUD              115200

#define CRSF_DEST_FC           0xC8
#define CRSF_TYPE_RC_PACKED    0x16
#define CRSF_FRAME_LEN         24     // TYPE + PAYLOAD + CRC

/* ──────────── SBUS constants ──────────── */
#define SBUS_UART              uart1
#define SBUS_TX_PIN            4      // inverted in hardware
#define SBUS_BAUD              100000
#define SBUS_FRAME_LEN_BYTES   25
#define SBUS_HEADER            0x0F
#define SBUS_FOOTER            0x00

/* ──────────── Timing ──────────── */
#define FAILSAFE_MS            100U     // no CRSF ⇒ failsafe
#define STATUS_MS              1000U    // stats interval
#define SBUS_MAX_GAP_US        16000U   // guard ≤16 ms between frames

/* ──────────── CRC-8 / MAXIM ──────────── */
static uint8_t crc8_maxim(const uint8_t *d, int len)
{
    uint8_t c = 0;
    while (len--) {
        c ^= *d++;
        for (int i = 0; i < 8; ++i)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1);
    }
    return c;
}

/* ──────────── CRSF decoder ──────────── */
typedef struct { uint8_t buf[32]; int idx, need; } crsf_state_t;

/* Return: 1 = good frame, 0 = none, –1 = CRC/format error */
static int crsf_poll(uart_inst_t *u, uint16_t ch[16])
{
    static crsf_state_t s = { .idx = 0, .need = 0 };

    while (uart_is_readable(u)) {
        uint8_t b = uart_getc(u);

        if (s.idx == 0) {                         /* DEST                */
            if (b != CRSF_DEST_FC) continue;
            s.buf[s.idx++] = b;
        } else if (s.idx == 1) {                  /* LEN                 */
            if (b != CRSF_FRAME_LEN) { s.idx = 0; continue; }
            s.buf[s.idx++] = b;
            s.need = 2 + b;                       /* total frame length  */
        } else {
            s.buf[s.idx++] = b;
            if (s.idx == s.need) {                /* frame complete      */
                int ok = -1;
                if (crc8_maxim(&s.buf[2], CRSF_FRAME_LEN) == s.buf[s.need - 1] &&
                    s.buf[2] == CRSF_TYPE_RC_PACKED) {

                    const uint8_t *p = &s.buf[3];            /* 22-byte payload */
                    for (int i = 0; i < 16; ++i) {
                        int bit   = i * 11;
                        int byte  = bit >> 3;
                        int shift = bit & 7;
                        uint32_t raw =  p[byte]
                                      | (uint32_t)p[byte + 1] << 8
                                      | (uint32_t)p[byte + 2] << 16;
                        ch[i] = (raw >> shift) & 0x7FF;
                    }
                    ok = 1;
                }
                s.idx = 0;
                return ok;
            }
        }
    }
    return 0;
}

/* ──────────── SBUS encoder ──────────── */
static void sbus_pack(const uint16_t ch[16],
                      uint8_t f[SBUS_FRAME_LEN_BYTES],
                      bool fs)
{
    f[0] = SBUS_HEADER;
    for (int i = 1; i <= 22; ++i) f[i] = 0;

    for (int i = 0; i < 16; ++i) {
        uint32_t v = ch[i] & 0x7FF;
        int bit    = i * 11;
        int byte   = 1 + (bit >> 3);
        int shift  = bit & 7;
        f[byte]     |= (uint8_t)(v << shift);
        f[byte + 1] |= (uint8_t)(v >> (8 - shift));
        f[byte + 2] |= (uint8_t)(v >> (16 - shift));
    }
    f[23] = fs ? 0x0C : 0x00;   /* lost-frame & failsafe bits */
    f[24] = SBUS_FOOTER;
}

/* ──────────── Globals ──────────── */
static volatile uint32_t ok_cnt, bad_cnt, sbus_cnt, fs_cnt;
static absolute_time_t   last_sbus_tx;

/* ──────────── Helpers ──────────── */
static void send_sbus(const uint16_t ch[16], bool fs)
{
    uint8_t frame[SBUS_FRAME_LEN_BYTES];
    sbus_pack(ch, frame, fs);
    uart_write_blocking(SBUS_UART, frame, SBUS_FRAME_LEN_BYTES);
    sbus_cnt++;
    last_sbus_tx = get_absolute_time();
}

/* ──────────── main ──────────── */
int main(void)
{
    /* UART0: CRSF in + stats out */
    uart_init(CRSF_UART, CRSF_BAUD);
    gpio_set_function(CRSF_RX_PIN, GPIO_FUNC_UART);
    gpio_set_function(CRSF_TX_PIN, GPIO_FUNC_UART);
    uart_set_format(CRSF_UART, 8, 1, UART_PARITY_NONE);

    /* UART1: SBUS out (inverted, 8E2) */
    uart_init(SBUS_UART, SBUS_BAUD);
    uart_set_format(SBUS_UART, 8, 2, UART_PARITY_EVEN);
    gpio_set_function(SBUS_TX_PIN, GPIO_FUNC_UART);
    gpio_set_outover(SBUS_TX_PIN, GPIO_OVERRIDE_INVERT);

    uint16_t neutral[16];
    for (int i = 0; i < 16; ++i) neutral[i] = 992;

    absolute_time_t last_ok = get_absolute_time();
    last_sbus_tx            = get_absolute_time();
    absolute_time_t next_st = make_timeout_time_ms(STATUS_MS);

    while (true) {
        uint16_t ch[16];
        int r = crsf_poll(CRSF_UART, ch);
        if (r == 1) {                           /* good frame */
            ok_cnt++;  last_ok = get_absolute_time();
            send_sbus(ch, false);
        } else if (r == -1) {                   /* CRC error  */
            bad_cnt++;
        }

        int32_t dt_ok = absolute_time_diff_us(last_ok,   get_absolute_time());
        int32_t dt_tx = absolute_time_diff_us(last_sbus_tx, get_absolute_time());

        if (dt_ok > FAILSAFE_MS * 1000U && dt_tx > 7000) {
            fs_cnt++;  send_sbus(neutral, true);
        } else if (dt_tx > SBUS_MAX_GAP_US) {
            send_sbus(neutral, false);
        }

        if (absolute_time_diff_us(get_absolute_time(), next_st) <= 0) {
            next_st = delayed_by_ms(next_st, STATUS_MS);
            char buf[96];
            int len = snprintf(buf, sizeof(buf),
                               "OK:%lu BAD:%lu SBUS:%lu FS:%lu\r\n",
                               ok_cnt, bad_cnt, sbus_cnt, fs_cnt);
            uart_write_blocking(CRSF_UART, (uint8_t *)buf, len);
        }
    }
}
