/******************************************************************************
 * sctp_receiver_ncurses_relisten.c
 *
 * Demonstrates:
 *   - Partial Reliability (missing, recovered, irretrievable) logic
 *   - 10-second event ring buffer for stats
 *   - Inter-arrival time & packet-size ASCII histograms
 *   - UDP forwarding to 127.0.0.1:5600
 *   - Ncurses UI that updates every 2 seconds
 *   - Threaded approach with indefinite re-listen:
 *       * Main thread repeatedly accept() an SCTP connection
 *         => spawn an RX thread for each accepted conn
 *         => if peer disconnects, we close and go back to accept().
 *       * The UI thread runs the entire time, showing continuous stats.
 *   - Shows "In listen mode for X seconds" in the UI.
 *
 * Usage:
 *   ./sctp_receiver_ncurses_relisten [--port 6600 ...]
 *   Press Ctrl+C to exit.
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/sctp.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <linux/sockios.h>  // For SIOCINQ / SIOCOUTQ on Linux
#include <pthread.h>
#include <curses.h>

#ifndef SOL_SCTP
#define SOL_SCTP IPPROTO_SCTP
#endif

/* Default SCTP parameters */
#define DEFAULT_SCTP_PORT       6600
#define DEFAULT_RTO_MIN         2      // ms
#define DEFAULT_RTO_MAX         10     // ms
#define DEFAULT_RTO_INITIAL     2      // ms
#define DEFAULT_PR_SCTP_TTL     50     // ms
#define DEFAULT_DELAYED_ACK_MS  10     // ms
#define DEFAULT_BUFFER_SIZE_KB  16

/* We'll do a 10-second ring buffer window,
   but update the Ncurses interface every 2 seconds. */
#define STATS_UPDATE_SEC        2

static volatile int g_running = 1; /* For clean shutdown on Ctrl+C */

/* We track when we started listening, to show "X seconds in listen mode." */
static time_t g_listen_start_time = 0;

/* -------------------------------------------------------------------------
 * Signal handler for Ctrl+C
 * ------------------------------------------------------------------------- */
static void handle_signal(int signo) {
    (void)signo;
    g_running = 0;
}

/* -------------------------------------------------------------------------
 * Minimal RTP header structure & parser
 * ------------------------------------------------------------------------- */
typedef struct {
    unsigned version:2;
    unsigned padding:1;
    unsigned extension:1;
    unsigned csrc_count:4;
    unsigned marker:1;
    unsigned payload_type:7;
    unsigned short sequence_number;
    unsigned int timestamp;
    unsigned int ssrc;
} rtp_header_t;

static void parse_rtp_header(const unsigned char *buf, rtp_header_t *hdr) {
    hdr->version         = (buf[0] >> 6) & 0x03;
    hdr->padding         = (buf[0] >> 5) & 0x01;
    hdr->extension       = (buf[0] >> 4) & 0x01;
    hdr->csrc_count      =  buf[0]       & 0x0F;
    hdr->marker          = (buf[1] >> 7) & 0x01;
    hdr->payload_type    =  buf[1]       & 0x7F;
    hdr->sequence_number = (buf[2] << 8) | buf[3];
    hdr->timestamp       = (buf[4] << 24)|(buf[5] << 16)|(buf[6] << 8)| buf[7];
    hdr->ssrc            = (buf[8] << 24)|(buf[9] << 16)|(buf[10] << 8)| buf[11];
}

/* -------------------------------------------------------------------------
 * Partial Reliability Logic (missing, recovered, irretrievable)
 * We'll track missing sequences in an array keyed by seq (16 bits).
 * ------------------------------------------------------------------------- */
typedef struct {
    int missing;                // 1 if missing
    struct timespec detect_ts;  // when gap discovered
} lost_seq_t;

/* We'll allocate globally, but re-initialize on each new SCTP connection. */
static lost_seq_t g_lost_seq[65536]; /* for 16-bit seq numbers */
static int g_pr_sctp_ttl = DEFAULT_PR_SCTP_TTL; /* in ms */

static int g_first_packet = 1;
static unsigned short g_expected_seq = 0;

/* Reset partial reliability state for each new association */
static void reset_partial_reliability_state()
{
    memset(g_lost_seq, 0, sizeof(g_lost_seq));
    g_first_packet = 1;
    g_expected_seq = 0;
}

/* -------------------------------------------------------------------------
 * 10-second ring buffer events
 *   - ARRIVAL: a packet arrived
 *   - RECOVERED: a missing packet arrived out-of-order
 *   - IRRETRIEVABLE: a missing packet timed out
 *
 * We store:
 *   - bytes: for ARRIVAL
 *   - intra_ms: inter-arrival time in ms for ARRIVAL
 * ------------------------------------------------------------------------- */
typedef enum {
    EVT_ARRIVAL,
    EVT_RECOVERED,
    EVT_IRRETRIEVABLE
} event_type_t;

typedef struct {
    struct timespec time;
    event_type_t    type;
    double          recovery_time; // only for EVT_RECOVERED
    size_t          bytes;         // only for EVT_ARRIVAL
    double          intra_ms;      // only for EVT_ARRIVAL
} event_t;

#define MAX_EVENTS 20000
static event_t g_events[MAX_EVENTS];
static int g_evt_head = 0; // insertion index
static int g_evt_tail = 0; // oldest event index

/* We need a mutex to protect ring buffer & partial reliability data. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Overall totals since start */
static unsigned long g_total_packets = 0;
static unsigned long g_total_bytes   = 0;

/* Inter-arrival time logic */
static struct timespec g_last_arrival_ts;
static int g_first_arrival = 1;

/* Ncurses window pointer */
static WINDOW *g_win = NULL;

/* The UDP socket is global for convenience. We keep it open across re-listens. */
static int g_udp_sock = -1;
static struct sockaddr_in g_udp_dest; /* 127.0.0.1:5600 */

/* We keep track of the current SCTP connection FD in the UI. -1 => no conn. */
static int g_current_conn_fd = -1;

/* The listening socket */
static int do_listen_fd = -1;

/* -------------------------------------------------------------------------
 * Time helpers
 * ------------------------------------------------------------------------- */
static void get_monotonic_time(struct timespec *ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static double timespec_diff_sec(const struct timespec *end,
                                const struct timespec *start)
{
    double s  = (double)(end->tv_sec - start->tv_sec);
    double ns = (double)(end->tv_nsec - start->tv_nsec) / 1e9;
    return s + ns;
}

static double timespec_diff_ms(const struct timespec *end,
                               const struct timespec *start)
{
    return timespec_diff_sec(end, start) * 1000.0;
}

/* -------------------------------------------------------------------------
 * Event ring buffer: add & prune (with concurrency lock)
 * ------------------------------------------------------------------------- */
static void add_event(event_type_t type, double recovery_time,
                      size_t bytes, double intra_ms)
{
    struct timespec now;
    get_monotonic_time(&now);

    g_events[g_evt_head].time          = now;
    g_events[g_evt_head].type          = type;
    g_events[g_evt_head].recovery_time = (type == EVT_RECOVERED) ? recovery_time : 0.0;
    g_events[g_evt_head].bytes         = (type == EVT_ARRIVAL)   ? bytes : 0;
    g_events[g_evt_head].intra_ms      = (type == EVT_ARRIVAL)   ? intra_ms : 0.0;

    g_evt_head = (g_evt_head + 1) % MAX_EVENTS;
    if (g_evt_head == g_evt_tail) {
        /* ring buffer full, overwrite oldest */
        g_evt_tail = (g_evt_tail + 1) % MAX_EVENTS;
    }
}

static void prune_old_events() {
    struct timespec now;
    get_monotonic_time(&now);

    while (g_evt_tail != g_evt_head) {
        double age = timespec_diff_sec(&now, &g_events[g_evt_tail].time);
        if (age > 10.0) {
            g_evt_tail = (g_evt_tail + 1) % MAX_EVENTS;
        } else {
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * handle_packet_seq: partial reliability logic
 * Must be called under lock.
 * ------------------------------------------------------------------------- */
static void handle_packet_seq(unsigned short seq) {
    if (g_first_packet) {
        g_first_packet = 0;
        g_expected_seq = seq + 1;
        return;
    }

    if (seq < g_expected_seq) {
        /* out-of-order => if missing => recovered */
        if (g_lost_seq[seq].missing) {
            struct timespec now;
            get_monotonic_time(&now);
            double rec_time = timespec_diff_sec(&now, &g_lost_seq[seq].detect_ts);
            g_lost_seq[seq].missing = 0;
            add_event(EVT_RECOVERED, rec_time, 0, 0.0);
        }
        return;
    }

    if (seq > g_expected_seq) {
        /* gap => mark missing */
        for (unsigned short s = g_expected_seq; s < seq; s++) {
            if (!g_lost_seq[s].missing) {
                g_lost_seq[s].missing = 1;
                get_monotonic_time(&g_lost_seq[s].detect_ts);
            }
        }
    }

    g_expected_seq = seq + 1;

    /* if seq was missing => recovered */
    if (g_lost_seq[seq].missing) {
        struct timespec now;
        get_monotonic_time(&now);
        double rec_time = timespec_diff_sec(&now, &g_lost_seq[seq].detect_ts);
        g_lost_seq[seq].missing = 0;
        add_event(EVT_RECOVERED, rec_time, 0, 0.0);
    }

    /* older missing => irretrievable if older than TTL */
    {
        struct timespec now;
        get_monotonic_time(&now);
        double ttl_sec = (double)g_pr_sctp_ttl / 1000.0;

        unsigned short check_start = (seq > 1000) ? (unsigned short)(seq - 1000) : 0;
        for (unsigned short s = seq; s > check_start; s--) {
            if (s >= 65535) break;
            if (s < g_expected_seq) {
                if (g_lost_seq[s].missing) {
                    double age_sec = timespec_diff_sec(&now, &g_lost_seq[s].detect_ts);
                    if (age_sec > ttl_sec) {
                        g_lost_seq[s].missing = 0;
                        add_event(EVT_IRRETRIEVABLE, 0.0, 0, 0.0);
                    }
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * /proc/net/sctp/snmp parsing for interesting counters
 * ------------------------------------------------------------------------- */
typedef struct {
    int SctpCurrEstab;
    int SctpInSCTPPacks;
    int SctpInDataChunkDiscards;
    int SctpOutOfBlues;
    int SctpInPktDiscards;
} sctp_snmp_t;

static int parse_sctp_snmp(sctp_snmp_t *out)
{
    FILE *fp = fopen("/proc/net/sctp/snmp", "r");
    if (!fp) return -1;

    memset(out, 0, sizeof(*out));
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char key[64];
        int val;
        if (sscanf(line, "%63s %d", key, &val) == 2) {
            if (!strcmp(key, "SctpCurrEstab"))             out->SctpCurrEstab= val;
            else if(!strcmp(key,"SctpInSCTPPacks"))        out->SctpInSCTPPacks= val;
            else if(!strcmp(key,"SctpInDataChunkDiscards"))out->SctpInDataChunkDiscards= val;
            else if(!strcmp(key,"SctpOutOfBlues"))         out->SctpOutOfBlues= val;
            else if(!strcmp(key,"SctpInPktDiscards"))      out->SctpInPktDiscards= val;
        }
    }
    fclose(fp);
    return 0;
}

/* -------------------------------------------------------------------------
 * Basic helper to get inq/outq for buffer usage
 * (May not work for SCTP on all kernels.)
 * ------------------------------------------------------------------------- */
static int get_inq(int fd)
{
    int bytes=-1;
#ifdef SIOCINQ
    if(ioctl(fd, SIOCINQ, &bytes)<0){
        bytes=-1;
    }
#endif
    return bytes;
}
static int get_outq(int fd)
{
    int bytes=-1;
#ifdef SIOCOUTQ
    if(ioctl(fd, SIOCOUTQ, &bytes)<0){
        bytes=-1;
    }
#endif
    return bytes;
}

/* -------------------------------------------------------------------------
 * Building histograms for inter-arrival times and packet sizes
 * Must be called under lock if we read ring buffer.
 * ------------------------------------------------------------------------- */
static void build_intra_time_histogram(int hist[8]) {
    memset(hist,0,8*sizeof(int));

    int idx= g_evt_tail;
    while(idx!=g_evt_head){
        if(g_events[idx].type==EVT_ARRIVAL){
            double ms= g_events[idx].intra_ms;
            if     (ms<1.0)   hist[0]++;
            else if(ms<2.0)   hist[1]++;
            else if(ms<5.0)   hist[2]++;
            else if(ms<10.0)  hist[3]++;
            else if(ms<20.0)  hist[4]++;
            else if(ms<50.0)  hist[5]++;
            else if(ms<100.0) hist[6]++;
            else              hist[7]++;
        }
        idx=(idx+1)%MAX_EVENTS;
    }
}

static void build_packet_size_histogram(int hist[8]){
    memset(hist,0,8*sizeof(int));

    int idx= g_evt_tail;
    while(idx!=g_evt_head){
        if(g_events[idx].type==EVT_ARRIVAL){
            size_t sz= g_events[idx].bytes;
            if     (sz<256)    hist[0]++;
            else if(sz<512)    hist[1]++;
            else if(sz<1024)   hist[2]++;
            else if(sz<1500)   hist[3]++;
            else if(sz<3000)   hist[4]++;
            else if(sz<5000)   hist[5]++;
            else if(sz<10000)  hist[6]++;
            else               hist[7]++;
        }
        idx=(idx+1)%MAX_EVENTS;
    }
}

/* -------------------------------------------------------------------------
 * The function that draws stats in the Ncurses window.
 * Called periodically by the UI thread.
 * ------------------------------------------------------------------------- */
static void draw_stats(int conn_fd)
{
    /* We'll lock while reading ring buffer & global stats. */
    pthread_mutex_lock(&g_lock);

    /* Prune old events first. */
    prune_old_events();

    /* Gather ring buffer data in last 10s */
    unsigned long arrivals=0, recovered=0, irretrievable=0;
    size_t bytes_window=0;
    double total_rec_time=0.0;

    int idx= g_evt_tail;
    while(idx!=g_evt_head){
        event_t *ev= &g_events[idx];
        switch(ev->type){
            case EVT_ARRIVAL:
                arrivals++;
                bytes_window+= ev->bytes;
                break;
            case EVT_RECOVERED:
                recovered++;
                total_rec_time+= ev->recovery_time;
                break;
            case EVT_IRRETRIEVABLE:
                irretrievable++;
                break;
        }
        idx=(idx+1)%MAX_EVENTS;
    }

    double avg_recovery=0.0;
    if(recovered>0){
        avg_recovery= total_rec_time/(double)recovered;
    }

    /* Packets/s and Mbits/s for the last 10s window */
    double pkts_sec=(arrivals/10.0);
    double mbits_sec= 0.0;
    if(bytes_window>0){
        mbits_sec= (8.0*(double)bytes_window)/1e6 /10.0;
    }

    /* parse /proc/net/sctp/snmp if available */
    sctp_snmp_t snmp;
    int snmp_ok= parse_sctp_snmp(&snmp);

    /* get buffer sizes (max) */
    int max_sndbuf=0,len=sizeof(max_sndbuf);
    int max_rcvbuf=0;
    if(conn_fd>=0){
        getsockopt(conn_fd, SOL_SOCKET, SO_SNDBUF, &max_sndbuf, (socklen_t*)&len);
        len=sizeof(max_rcvbuf);
        getsockopt(conn_fd, SOL_SOCKET, SO_RCVBUF, &max_rcvbuf, (socklen_t*)&len);
    }

    /* inq/outq usage if possible */
    int outq= -1, inq= -1;
    if(conn_fd>=0){
        outq= get_outq(conn_fd);
        inq = get_inq(conn_fd);
    }

    /* build histograms */
    int intra_hist[8], size_hist[8];
    build_intra_time_histogram(intra_hist);
    build_packet_size_histogram(size_hist);

    /* Done reading ring buffer => unlock before we do ncurses. */
    pthread_mutex_unlock(&g_lock);

    /* Clear window first */
    werase(g_win);

    /* Show how long we've been in "listen mode." */
    time_t now_time = time(NULL);
    long seconds_listening = now_time - g_listen_start_time;

    mvwprintw(g_win,0,0,"=== SCTP RECEIVER STATS (re-listen) - updates every 2s ===");
    mvwprintw(g_win,1,0,"Listening for: %ld seconds", seconds_listening);

    mvwprintw(g_win,3,0,"Last 10s Window:");
    mvwprintw(g_win,4,0,"  Arrivals:         %lu", arrivals);
    mvwprintw(g_win,5,0,"  Bytes in Window:  %zu", bytes_window);
    mvwprintw(g_win,6,0,"  Recovered:        %lu", recovered);
    mvwprintw(g_win,7,0,"  Irretrievable:    %lu", irretrievable);
    mvwprintw(g_win,8,0,"  Avg Recovery (s): %.3f", avg_recovery);
    mvwprintw(g_win,9,0,"  Packets/s:        %.2f", pkts_sec);
    mvwprintw(g_win,10,0," Mbits/s:           %.3f", mbits_sec);

    mvwprintw(g_win,12,0,"--- Overall Totals (Since Start) ---");
    mvwprintw(g_win,13,0,"  Total Packets:    %lu", g_total_packets);
    mvwprintw(g_win,14,0,"  Total Bytes:      %lu", g_total_bytes);

    mvwprintw(g_win,16,0,"--- Socket Buffers (SCTP) ---");
    if(conn_fd>=0){
        mvwprintw(g_win,17,0,"  MaxSendBuf: %d  UsedOutQ: %s",
                  max_sndbuf,(outq>=0)?"":"N/A");
        if(outq>=0){
            wprintw(g_win," %d bytes", outq);
        }
        mvwprintw(g_win,18,0,"  MaxRecvBuf: %d  UsedInQ:  %s",
                  max_rcvbuf,(inq>=0)?"":"N/A");
        if(inq>=0){
            wprintw(g_win," %d bytes", inq);
        }
    } else {
        mvwprintw(g_win,17,0,"[No current SCTP connection]");
    }

    mvwprintw(g_win,20,0,"--- /proc/net/sctp/snmp snapshot ---");
    if(snmp_ok==0){
        mvwprintw(g_win,21,2,"SctpCurrEstab: %d", snmp.SctpCurrEstab);
        mvwprintw(g_win,22,2,"SctpInSCTPPacks: %d", snmp.SctpInSCTPPacks);
        mvwprintw(g_win,23,2,"SctpInDataChunkDiscards: %d", snmp.SctpInDataChunkDiscards);
        mvwprintw(g_win,24,2,"SctpOutOfBlues: %d", snmp.SctpOutOfBlues);
        mvwprintw(g_win,25,2,"SctpInPktDiscards: %d", snmp.SctpInPktDiscards);
    } else {
        mvwprintw(g_win,21,2,"[Not available on this system]");
    }

    /* Print the ASCII histograms for inter-arrival times and packet sizes. */
    int row=27;
    mvwprintw(g_win,row++,0,"-- Inter-Arrival Time Dist (10s) --");
    {
        static const char *bin_labels[8] = {
            "<1ms", "1-2ms", "2-5ms", "5-10ms",
            "10-20ms", "20-50ms", "50-100ms", ">=100ms"
        };
        int max_count=0;
        for(int i=0;i<8;i++){
            if(intra_hist[i]>max_count) max_count=intra_hist[i];
        }
        if(max_count==0){
            mvwprintw(g_win,row++,2,"No arrivals or no inter-arrival data in last 10s.");
        } else {
            double scale=(max_count>0)?(40.0/(double)max_count):1.0;
            for(int i=0;i<8;i++){
                int bar_len=(int)(intra_hist[i]*scale);
                mvwprintw(g_win,row,2,"[%6s] %4d | ",bin_labels[i],intra_hist[i]);
                for(int j=0;j<bar_len;j++){
                    wprintw(g_win,"#");
                }
                row++;
            }
        }
    }

    mvwprintw(g_win,row++,0,"-- Packet Size Dist (10s) --");
    {
        static const char *szlabels[8]={
            "<256", "256-512", "512-1024","1024-1500",
            "1500-3000","3000-5000","5000-10000",">=10000"
        };
        int size_hist[8]={0};
        build_packet_size_histogram(size_hist);

        int size_hist_max=0;
        for(int i=0;i<8;i++){
            if(size_hist[i]>size_hist_max) size_hist_max=size_hist[i];
        }
        if(size_hist_max==0){
            mvwprintw(g_win,row++,2,"No packet-size data in last 10s.");
        } else {
            double scale=(size_hist_max>0)?(40.0/(double)size_hist_max):1.0;
            for(int i=0;i<8;i++){
                int bar_len=(int)(size_hist[i]*scale);
                mvwprintw(g_win,row,2,"[%8s] %4d | ",szlabels[i],size_hist[i]);
                for(int j=0;j<bar_len;j++){
                    wprintw(g_win,"#");
                }
                row++;
            }
        }
    }

    wrefresh(g_win);
}

/* -------------------------------------------------------------------------
 * The "UI thread" function: updates Ncurses every 2 seconds
 * until g_running=0. The connection FD might be -1 if no current conn.
 * ------------------------------------------------------------------------- */
static void *ui_thread_func(void *arg)
{
    (void)arg;
    while(g_running){
        draw_stats(g_current_conn_fd);
        for(int i=0; i<STATS_UPDATE_SEC && g_running; i++){
            sleep(1);
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * The "receiver thread" function:
 *   - read from SCTP conn_fd in a loop
 *   - partial reliability logic
 *   - forward to UDP
 *   - if peer closes => exit the thread
 * ------------------------------------------------------------------------- */
struct rx_thread_args {
    int conn_fd;
};

static void *rx_thread_func(void *varg)
{
    struct rx_thread_args *args = (struct rx_thread_args*)varg;
    int conn_fd= args->conn_fd;
    free(args);

    unsigned char buffer[65536];

    while(g_running){
        ssize_t n= recv(conn_fd, buffer, sizeof(buffer), 0);
        if(n<0){
            if(errno==EINTR) continue;
            perror("recv()");
            break;
        } else if(n==0){
            /* peer closed */
            break;
        }

        pthread_mutex_lock(&g_lock);

        /* Update overall totals */
        g_total_packets++;
        g_total_bytes += n;

        /* Inter-arrival time */
        struct timespec now_ts;
        get_monotonic_time(&now_ts);
        double intra_ms= 0.0;
        if(!g_first_arrival){
            intra_ms= timespec_diff_ms(&now_ts, &g_last_arrival_ts);
        } else {
            g_first_arrival=0;
        }
        g_last_arrival_ts= now_ts;

        /* Add ARRIVAL event */
        add_event(EVT_ARRIVAL, 0.0, (size_t)n, intra_ms);

        /* If >=12 bytes, parse RTP & partial reliability check */
        if(n>=12){
            rtp_header_t rtp_hdr;
            parse_rtp_header(buffer, &rtp_hdr);
            handle_packet_seq(rtp_hdr.sequence_number);
        }

        /* Forward to UDP */
        if(sendto(g_udp_sock, buffer, n, 0,
                  (struct sockaddr*)&g_udp_dest,sizeof(g_udp_dest))<0)
        {
            perror("sendto(UDP) failed");
        }

        pthread_mutex_unlock(&g_lock);
    }

    close(conn_fd);

    /* Mark no active connection */
    pthread_mutex_lock(&g_lock);
    g_current_conn_fd= -1;
    pthread_mutex_unlock(&g_lock);

    return NULL;
}

/* -------------------------------------------------------------------------
 * Accept loop in main thread:
 *   - Binds and listens once
 *   - Repeatedly accept a new connection
 *   - Reset partial reliability state for that conn
 *   - Spawn an rx_thread
 *   - Wait for that thread to finish (peer close or error)
 *   - Then accept again, until Ctrl+C
 * ------------------------------------------------------------------------- */
static int setup_listening_socket(int port, int rto_min, int rto_max,
                                  int rto_init, int ack_time, int buf_kb)
{
    do_listen_fd= socket(AF_INET,SOCK_STREAM,IPPROTO_SCTP);
    if(do_listen_fd<0){
        perror("socket(AF_INET,SOCK_STREAM,IPPROTO_SCTP)");
        return -1;
    }

    int sock_buf_size= buf_kb*1024;
    setsockopt(do_listen_fd, SOL_SOCKET, SO_SNDBUF,
               &sock_buf_size,sizeof(sock_buf_size));
    setsockopt(do_listen_fd, SOL_SOCKET, SO_RCVBUF,
               &sock_buf_size,sizeof(sock_buf_size));

    int reuse=1;
    setsockopt(do_listen_fd, SOL_SOCKET, SO_REUSEADDR,&reuse,sizeof(reuse));
    setsockopt(do_listen_fd, SOL_SOCKET, SO_REUSEPORT,&reuse,sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family=AF_INET;
    addr.sin_port=htons(port);
    addr.sin_addr.s_addr= INADDR_ANY;

    if(bind(do_listen_fd,(struct sockaddr*)&addr,sizeof(addr))<0){
        perror("bind()");
        close(do_listen_fd);
        do_listen_fd=-1;
        return -1;
    }

    if(listen(do_listen_fd,1)<0){
        perror("listen()");
        close(do_listen_fd);
        do_listen_fd=-1;
        return -1;
    }

    /* SCTP options: RTO, PR-SCTP, delayed ACK, NODELAY */
    {
        struct sctp_rtoinfo rtoinfo;
        memset(&rtoinfo,0,sizeof(rtoinfo));
        rtoinfo.srto_initial= rto_init;
        rtoinfo.srto_max    = rto_max;
        rtoinfo.srto_min    = rto_min;
        setsockopt(do_listen_fd, SOL_SCTP, SCTP_RTOINFO,
                   &rtoinfo,sizeof(rtoinfo));

        struct sctp_prinfo prinfo;
        memset(&prinfo,0,sizeof(prinfo));
        prinfo.pr_policy= SCTP_PR_SCTP_TTL;
        prinfo.pr_value = g_pr_sctp_ttl;
        setsockopt(do_listen_fd, SOL_SCTP, SCTP_PR_SUPPORTED,
                   &prinfo,sizeof(prinfo));

        struct sctp_assoc_value ack_delay;
        memset(&ack_delay,0,sizeof(ack_delay));
        ack_delay.assoc_id   = SCTP_FUTURE_ASSOC;
        ack_delay.assoc_value= ack_time;
        setsockopt(do_listen_fd, SOL_SCTP, SCTP_DELAYED_ACK_TIME,
                   &ack_delay,sizeof(ack_delay));

        int one=1;
        setsockopt(do_listen_fd, SOL_SCTP, SCTP_NODELAY,
                   &one,sizeof(one));
    }

    printf("Listening on SCTP port %d...\n",port);
    return do_listen_fd;
}

/* -------------------------------------------------------------------------
 * The UI thread is declared above; here's main
 * ------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    int port      = DEFAULT_SCTP_PORT;
    int rto_min   = DEFAULT_RTO_MIN;
    int rto_max   = DEFAULT_RTO_MAX;
    int rto_init  = DEFAULT_RTO_INITIAL;
    int ack_time  = DEFAULT_DELAYED_ACK_MS;
    int buf_kb    = DEFAULT_BUFFER_SIZE_KB;

    g_pr_sctp_ttl = DEFAULT_PR_SCTP_TTL;

    /* Record the start time of listening */
    g_listen_start_time = time(NULL);

    /* Parse command-line arguments */
    for(int i=1; i<argc; i++){
        if(!strcmp(argv[i],"--port") && i+1<argc){
            port= atoi(argv[++i]);
        } else if(!strcmp(argv[i],"--rto-min") && i+1<argc){
            rto_min= atoi(argv[++i]);
        } else if(!strcmp(argv[i],"--rto-max") && i+1<argc){
            rto_max= atoi(argv[++i]);
        } else if(!strcmp(argv[i],"--rto-initial") && i+1<argc){
            rto_init= atoi(argv[++i]);
        } else if(!strcmp(argv[i],"--pr-sctp-ttl") && i+1<argc){
            g_pr_sctp_ttl= atoi(argv[++i]);
        } else if(!strcmp(argv[i],"--delayed-ack-time") && i+1<argc){
            ack_time= atoi(argv[++i]);
        } else if(!strcmp(argv[i],"--buffer-kb") && i+1<argc){
            buf_kb= atoi(argv[++i]);
        } else if(!strcmp(argv[i],"--help")){
            printf("Usage: %s [OPTIONS]\n", argv[0]);
            printf("  --port <port>             (default=%d)\n", port);
            printf("  --rto-min <ms>            (default=%d)\n", rto_min);
            printf("  --rto-max <ms>            (default=%d)\n", rto_max);
            printf("  --rto-initial <ms>        (default=%d)\n", rto_init);
            printf("  --pr-sctp-ttl <ms>        (default=%d)\n", g_pr_sctp_ttl);
            printf("  --delayed-ack-time <ms>   (default=%d)\n", ack_time);
            printf("  --buffer-kb <size>        (default=%d)\n", buf_kb);
            exit(EXIT_SUCCESS);
        }
    }

    signal(SIGINT, handle_signal);

    if(setup_listening_socket(port, rto_min, rto_max, rto_init, ack_time, buf_kb)<0){
        return 1; // error
    }

    /* Create a UDP socket to forward data to 127.0.0.1:5600 */
    g_udp_sock= socket(AF_INET,SOCK_DGRAM,0);
    if(g_udp_sock<0){
        perror("socket(AF_INET,SOCK_DGRAM)");
        close(do_listen_fd);
        return 1;
    }
    memset(&g_udp_dest,0,sizeof(g_udp_dest));
    g_udp_dest.sin_family= AF_INET;
    g_udp_dest.sin_port  = htons(5600);
    inet_pton(AF_INET,"127.0.0.1",&g_udp_dest.sin_addr);

    /* Initialize Ncurses */
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    g_win= stdscr; /* use stdscr directly */

    /* Start UI thread */
    pthread_t ui_thread;
    if(pthread_create(&ui_thread,NULL, &ui_thread_func, NULL)!=0){
        perror("pthread_create(ui_thread)");
        endwin();
        close(g_udp_sock);
        close(do_listen_fd);
        return 1;
    }

    /* Repeatedly accept new connections until Ctrl+C. */
    while(g_running){
        struct sockaddr_in peer_addr;
        socklen_t peer_len= sizeof(peer_addr);

        int conn_fd= accept(do_listen_fd,(struct sockaddr*)&peer_addr,&peer_len);
        if(conn_fd<0){
            if(!g_running) break; /* interrupted by Ctrl+C? */
            perror("accept()");
            /* Sleep a bit, then continue. */
            sleep(1);
            continue;
        }

        printf("Accepted SCTP connection from %s:%u\n",
               inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port));

        /* Reset partial reliability for this new association */
        pthread_mutex_lock(&g_lock);
        reset_partial_reliability_state();
        /* Also reset the "first arrival" logic for inter-arrival times: */
        g_first_arrival=1;
        /* Mark the current conn fd so the UI can track buffer usage, etc. */
        g_current_conn_fd= conn_fd;
        pthread_mutex_unlock(&g_lock);

        /* Spawn an RX thread for this connection */
        pthread_t rx_thread;
        struct rx_thread_args *args= malloc(sizeof(struct rx_thread_args));
        args->conn_fd= conn_fd;
        if(pthread_create(&rx_thread,NULL, &rx_thread_func,args)!=0){
            perror("pthread_create(rx_thread)");
            close(conn_fd);
            pthread_mutex_lock(&g_lock);
            g_current_conn_fd= -1;
            pthread_mutex_unlock(&g_lock);
            sleep(1);
            continue;
        }

        /* Wait until that thread finishes (peer closed or error).
           Then we re-listen in the loop. */
        pthread_join(rx_thread,NULL);
    }

    /* Cleanup */
    g_running=0;
    /* wait for UI thread to exit */
    pthread_join(ui_thread,NULL);

    endwin();

    if(g_udp_sock>=0) close(g_udp_sock);
    if(do_listen_fd>=0) close(do_listen_fd);

    printf("Receiver exiting.\n");
    printf("Total packets received: %lu\n", g_total_packets);
    printf("Total bytes received:   %lu\n", g_total_bytes);

    return 0;
}
