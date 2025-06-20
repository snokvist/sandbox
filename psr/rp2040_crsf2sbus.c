// rp2040_crsf2sbus.c – standalone CRSF‑to‑SBUS relay with stats
//
//  * RX  : CRSF frames @ 460 800 8N1 on UART0 (pin 1)
//  * TX  : SBUS stream  @ 100 000 8E2, **inverted** via GPIO OUTOVER on UART1 (pin 4)
//  * TX0 : ASCII status line every 1000 ms on UART0 (pin 0)
//
// Build (Pico‑SDK):
//   mkdir build && cd build && cmake .. && make -j
// Flash: copy UF2 to RP2040 BOOTSEL drive on the X2L.
//
// Written for clarity – optimise as you wish.
// MIT License.

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

/* =========== CRSF constants =========== */
#define CRSF_UART              uart0
#define CRSF_TX_PIN            0      // stats / echo back to host
#define CRSF_RX_PIN            1      // CRSF frames from host

#define CRSF_BAUD              460800

#define CRSF_DEST_FC           0xC8   // standard FC address
#define CRSF_TYPE_RC_PACKED    0x16
#define CRSF_FRAME_LEN         24     // len field for RC_PACKED (TYPE+PAYLOAD+CRC)

/* =========== SBUS constants =========== */
#define SBUS_UART              uart1
#define SBUS_TX_PIN            4      // inverted in hardware

#define SBUS_BAUD              100000
#define SBUS_FRAME_LEN_BYTES   25
#define SBUS_HEADER            0x0F
#define SBUS_FOOTER            0x00

/* =========== Timing =========== */
#define FAILSAFE_MS            100U    // failsafe threshold (no CRSF frame)
#define STATUS_MS              1000U   // stats interval
#define SBUS_MAX_GAP_US        16000U  // ensure max 16 ms between SBUS frames

/* =========== CRC‑8/MAXIM =========== */
static uint8_t crc8_maxim(const uint8_t *data, int len)
{
    uint8_t crc = 0x00;
    for (int i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

/* =========== CRSF decoder state =========== */
typedef struct {
    uint8_t buf[32];   // enough for 26‑byte frame
    int     idx;
    int     need;      // total bytes expected in current frame (dest+len+need)
} crsf_state_t;

/*
 * crsf_poll_channels()
 *   Read UART0 and, when a complete RC_CHANNELS_PACKED frame arrives,
 *   decode 16 channel values into `ch[16]`.
 *
 * Returns:  1 on valid frame, 0 if none yet, -1 on CRC or format error.
 */
static int crsf_poll_channels(uart_inst_t *u, uint16_t ch[16])
{
    static crsf_state_t s = { .idx = 0, .need = 0 };

    while (uart_is_readable(u)) {
        uint8_t byte = uart_getc(u);

        if (s.idx == 0) {
            if (byte != CRSF_DEST_FC) continue;   // wait for dest byte
            s.buf[s.idx++] = byte;
        } else if (s.idx == 1) {
            if (byte != CRSF_FRAME_LEN) { s.idx = 0; continue; }
            s.buf[s.idx++] = byte;
            s.need = 2 + byte;                   // dest + len + LEN bytes
        } else {
            s.buf[s.idx++] = byte;
            if (s.idx >= s.need) {
                /* Frame complete */
                int res = -1;                    // assume bad
                uint8_t calc = crc8_maxim(&s.buf[2], CRSF_FRAME_LEN);   // TYPE..CRC‑1
                if (calc == s.buf[s.need - 1] &&
                    s.buf[2] == CRSF_TYPE_RC_PACKED) {
                    /* Decode 22‑byte payload (indexes 3..24) */
                    const uint8_t *p = &s.buf[3];
                    for (int i = 0; i < 16; ++i) {
                        int bit = i * 11;
                        int byte_idx = bit >> 3;
                        int bit_off  = bit & 7;
                        uint32_t raw = (uint32_t)p[byte_idx] |
                                       ((uint32_t)p[byte_idx + 1] << 8) |
                                       ((uint32_t)p[byte_idx + 2] << 16);
                        ch[i] = (raw >> bit_off) & 0x7FF;
                    }
                    res = 1;                     // good frame
                }
                s.idx = 0;   // ready for next frame
                return res;
            }
        }
    }
    return 0;   // nothing yet
}

/*
 * sbus_pack() – pack 16 channels into a 25‑byte SBUS frame.
 *    ch[i]  : 0‑2047 values (use 992 for neutral 1.5 ms)
 *    frame  : caller‑supplied 25‑byte buffer
 *    fs     : set TRUE to mark failsafe (sets bits 2 & 3)
 */
static void sbus_pack(const uint16_t ch[16], uint8_t frame[SBUS_FRAME_LEN_BYTES], bool fs)
{
    frame[0] = SBUS_HEADER;

    /* Zero payload */
    for (int i = 1; i <= 22; ++i) frame[i] = 0;

    /* Bit‑pack 16 × 11‑bit channels into bytes 1‑22 (little‑endian) */
    for (int i = 0; i < 16; ++i) {
        int bit = i * 11;
        int byte_idx = 1 + (bit >> 3);
        int bit_off  = bit & 7;
        uint32_t val = ch[i] & 0x7FF;
        frame[byte_idx]     |= (uint8_t)(val << bit_off);
        frame[byte_idx + 1] |= (uint8_t)(val >> (8 - bit_off));
        frame[byte_idx + 2] |= (uint8_t)(val >> (16 - bit_off));
    }

    /* Flags byte */
    uint8_t flags = 0;
    if (fs) flags |= (1 << 2) | (1 << 3);   // lost‑frame & failsafe bits
    frame[23] = flags;

    frame[24] = SBUS_FOOTER;
}

/* =========== Stats counters =========== */
static volatile uint32_t crsf_ok      = 0;
static volatile uint32_t crsf_bad     = 0;
static volatile uint32_t sbus_sent    = 0;
static volatile uint32_t failsafe_cnt = 0;

/* Helper to send SBUS */
static absolute_time_t last_sbus_tx;
static void send_sbus(uart_inst_t *u, const uint16_t ch[16], bool fs)
{
    uint8_t frame[SBUS_FRAME_LEN_BYTES];
    sbus_pack(ch, frame, fs);
    uart_write_blocking(u, frame, SBUS_FRAME_LEN_BYTES);
    sbus_sent++;
    last_sbus_tx = get_absolute_time();
}

int main()
{
    /* === Init UART0 (CRSF in + status out) === */
    uart_init(CRSF_UART, CRSF_BAUD);
    gpio_set_function(CRSF_RX_PIN, GPIO_FUNC_UART);
    gpio_set_function(CRSF_TX_PIN, GPIO_FUNC_UART);

    /* === Init UART1 (SBUS out, inverted) === */
    uart_init(SBUS_UART, SBUS_BAUD);
    uart_set_format(SBUS_UART, 8, 2, UART_PARITY_EVEN);  // 8E2
    gpio_set_function(SBUS_TX_PIN, GPIO_FUNC_UART);
    gpio_set_outover(SBUS_TX_PIN, GPIO_OVERRIDE_INVERT); // idle LOW (SBUS requirement)

    stdio_init_all();  // optional – enables USB‑CDC if you want

    /* Neutral channel template */
    uint16_t neutral[16];
    for (int i = 0; i < 16; ++i) neutral[i] = 992;

    /* Timers */
    absolute_time_t last_crsf_ok = get_absolute_time();
    last_sbus_tx                = get_absolute_time();
    absolute_time_t next_status = make_timeout_time_ms(STATUS_MS);

    while (true) {
        uint16_t ch[16];
        int res = crsf_poll_channels(CRSF_UART, ch);
        if (res == 1) {
            crsf_ok++;
            last_crsf_ok = get_absolute_time();
            send_sbus(SBUS_UART, ch, false);
        } else if (res == -1) {
            crsf_bad++;
        }

        /* Failsafe watchdog */
        int32_t since_ok = absolute_time_diff_us(last_crsf_ok, get_absolute_time());
        int32_t since_tx = absolute_time_diff_us(last_sbus_tx, get_absolute_time());

        if (since_ok > FAILSAFE_MS * 1000U && since_tx > 7000) {
            failsafe_cnt++;
            send_sbus(SBUS_UART, neutral, true);
        } else if (since_tx > SBUS_MAX_GAP_US) {
            send_sbus(SBUS_UART, neutral, false);
        }

        /* 1‑second stats ticker */
        if (absolute_time_diff_us(get_absolute_time(), next_status) <= 0) {
            next_status = delayed_by_ms(next_status, STATUS_MS);
            char buf[128];
            int len = snprintf(buf, sizeof(buf),
                               "OK:%lu BAD:%lu SBUS:%lu FS:%lu\r\n",
                               crsf_ok, crsf_bad, sbus_sent, failsafe_cnt);
            uart_write_blocking(CRSF_UART, (uint8_t *)buf, len);
        }
    }
}
