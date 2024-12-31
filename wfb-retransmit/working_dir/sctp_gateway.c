#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/sctp.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

// -----------------------------------------------------------------------------
// Version/Defaults
// -----------------------------------------------------------------------------
#define VERSION "1.6.0"
#define DEFAULT_UDP_PORT       5600
#define DEFAULT_SCTP_PORT      6600
#define DEFAULT_SCTP_ADDRESS   "10.5.0.1"
#define DEFAULT_MTU            1450
#define DEFAULT_RTO_MIN        2
#define DEFAULT_RTO_MAX        10
#define DEFAULT_RTO_INITIAL    2
#define DEFAULT_BUFFER_SIZE    16    // KB for UDP/SCTP buffers
#define DEFAULT_SACK_DELAY     10    // ms
#define DEFAULT_PR_TTL         50    // ms
#define DEFAULT_QUEUE_SIZE     1024  // Slots in the circular buffer
#define DEFAULT_SCTP_MAXATTEMPTS 2   // default attempts
#define DEFAULT_HEARTBEAT_INTERVAL 30000  // 30s in ms

// Number of bins for the inter-arrival histogram
// We have 8 edges => 9 bins total
#define HIST_BINS 9

// -----------------------------------------------------------------------------
// Circular Buffer (Dynamic Allocation)
// -----------------------------------------------------------------------------
typedef struct {
    char **data;        // Each element is a pointer to a chunk of 'mtu' bytes
    size_t *size;       // Each element's actual data size
    int head;
    int tail;
    int capacity;       // Number of slots (configurable)
    int mtu;            // Maximum chunk size
    pthread_mutex_t lock;
    pthread_cond_t cond;
} CircularBuffer;

// -----------------------------------------------------------------------------
// Global/Volatile State
// -----------------------------------------------------------------------------
volatile int running = 1;
int verbose = 0;

// User-configurable parameters
int udp_port          = DEFAULT_UDP_PORT;
int sctp_port         = DEFAULT_SCTP_PORT;
char sctp_address[INET_ADDRSTRLEN] = DEFAULT_SCTP_ADDRESS;
int mtu               = DEFAULT_MTU;
int rto_min           = DEFAULT_RTO_MIN;
int rto_max           = DEFAULT_RTO_MAX;
int rto_initial       = DEFAULT_RTO_INITIAL;
int udp_buffer_size   = DEFAULT_BUFFER_SIZE * 1024;
int sctp_buffer_size  = DEFAULT_BUFFER_SIZE * 1024;
int sack_delay        = DEFAULT_SACK_DELAY;
int pr_ttl            = DEFAULT_PR_TTL;
int queue_size        = DEFAULT_QUEUE_SIZE; 
int sctp_maxattempts  = DEFAULT_SCTP_MAXATTEMPTS;  
int sctp_heartbeat    = DEFAULT_HEARTBEAT_INTERVAL; // ms

// Stats counters
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t bytes_in   = 0;  // Total bytes received from UDP
static size_t bytes_out  = 0;  // Total bytes sent over SCTP
static size_t packets_in = 0;  // Total packets received from UDP
static size_t packets_out = 0; // Total packets sent over SCTP

// Drop counters
static size_t udp_drops  = 0;  // Dropped UDP packets (queue full)
static size_t sctp_drops = 0;  // Failed SCTP sends

// Inter-arrival histogram data
static pthread_mutex_t hist_lock = PTHREAD_MUTEX_INITIALIZER;
static long long last_packet_time_ns = 0;  // Timestamp of last UDP packet (ns)

// 8 edges => 9 bins
static unsigned long hist_bins[HIST_BINS];
static const long long bin_edges[HIST_BINS - 1] = {
    1000000LL,    // <1 ms
    2000000LL,    // <2 ms
    5000000LL,    // <5 ms
    10000000LL,   // <10 ms
    20000000LL,   // <20 ms
    50000000LL,   // <50 ms
    100000000LL,  // <100 ms
    200000000LL   // <200 ms
};

// Dynamic circular buffer
static CircularBuffer *udp_to_sctp_queue = NULL;

// -----------------------------------------------------------------------------
// Function Prototypes
// -----------------------------------------------------------------------------
void print_help();
void print_version();
void parse_arguments(int argc, char *argv[]);
void handle_signal(int sig);
void graceful_exit();

// Circular buffer helpers
CircularBuffer *init_circular_buffer(int capacity, int mtu);
void destroy_circular_buffer(CircularBuffer *cb);

// Socket functions
int create_udp_socket();
int connect_sctp_socket();
int sctp_reconnect_loop();

// Thread routines
void *udp_receiver(void *arg);
void *sctp_sender(void *arg);
void *stats_printer(void *arg);

// Statistics helpers
void update_histogram(struct timespec *now);
void print_sctp_snmp_stats();

// -----------------------------------------------------------------------------
// Helper Functions
// -----------------------------------------------------------------------------
static long long timespec_to_ns(const struct timespec *ts) {
    return (long long)ts->tv_sec * 1000000000LL + ts->tv_nsec;
}

void handle_signal(int sig) {
    (void)sig;
    running = 0;
    pthread_cond_broadcast(&udp_to_sctp_queue->cond);
}

// -----------------------------------------------------------------------------
// Argument Parsing
// -----------------------------------------------------------------------------
void print_help() {
    printf("Usage: sctp_gateway [OPTIONS]\n");
    printf("Options:\n");
    printf("  --udp-port <port>         Set the UDP listening port (default: %d)\n", DEFAULT_UDP_PORT);
    printf("  --sctp-port <port>        Set the SCTP destination port (default: %d)\n", DEFAULT_SCTP_PORT);
    printf("  --sctp-address <address>  Set the SCTP destination address (default: %s)\n", DEFAULT_SCTP_ADDRESS);
    printf("  --mtu <size>              Set the maximum transmission unit (default: %d)\n", DEFAULT_MTU);
    printf("  --queue-size <slots>      Set the queue capacity (default: %d)\n", DEFAULT_QUEUE_SIZE);
    printf("  --rto-min <ms>            Set the SCTP minimum retransmission timeout (default: %d ms)\n", DEFAULT_RTO_MIN);
    printf("  --rto-max <ms>            Set the SCTP maximum retransmission timeout (default: %d ms)\n", DEFAULT_RTO_MAX);
    printf("  --rto-initial <ms>        Set the SCTP initial retransmission timeout (default: %d ms)\n", DEFAULT_RTO_INITIAL);
    printf("  --udp-buffer <size_kb>    Set the UDP socket buffer size in KB (default: %d KB)\n", DEFAULT_BUFFER_SIZE);
    printf("  --sctp-buffer <size_kb>   Set the SCTP socket buffer size in KB (default: %d KB)\n", DEFAULT_BUFFER_SIZE);
    printf("  --sack-delay <ms>         Set the SCTP delayed acknowledgment time (default: %d ms)\n", DEFAULT_SACK_DELAY);
    printf("  --pr-ttl <ms>             Set the Partial Reliability TTL (default: %d ms)\n", DEFAULT_PR_TTL);
    printf("  --sctp-maxattempts <n>    Set the SCTP max connection attempts (default: %d)\n", DEFAULT_SCTP_MAXATTEMPTS);
    printf("  --sctp-heartbeat <ms>     Set the SCTP heartbeat interval in ms (default: %d ms)\n", DEFAULT_HEARTBEAT_INTERVAL);
    printf("  --verbose                 Enable verbose logging and stats\n");
    printf("  --help                    Show this help message\n");
    printf("  --version                 Show version information\n");
    exit(EXIT_SUCCESS);
}

void print_version() {
    printf("sctp_gateway version %s\n", VERSION);
    exit(EXIT_SUCCESS);
}

void parse_arguments(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_help();
        } else if (strcmp(argv[i], "--version") == 0) {
            print_version();
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--udp-port") == 0 && i + 1 < argc) {
            udp_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--sctp-port") == 0 && i + 1 < argc) {
            sctp_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--sctp-address") == 0 && i + 1 < argc) {
            strncpy(sctp_address, argv[++i], INET_ADDRSTRLEN);
        } else if (strcmp(argv[i], "--mtu") == 0 && i + 1 < argc) {
            mtu = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--queue-size") == 0 && i + 1 < argc) {
            queue_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rto-min") == 0 && i + 1 < argc) {
            rto_min = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rto-max") == 0 && i + 1 < argc) {
            rto_max = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rto-initial") == 0 && i + 1 < argc) {
            rto_initial = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--udp-buffer") == 0 && i + 1 < argc) {
            udp_buffer_size = atoi(argv[++i]) * 1024;
        } else if (strcmp(argv[i], "--sctp-buffer") == 0 && i + 1 < argc) {
            sctp_buffer_size = atoi(argv[++i]) * 1024;
        } else if (strcmp(argv[i], "--sack-delay") == 0 && i + 1 < argc) {
            sack_delay = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pr-ttl") == 0 && i + 1 < argc) {
            pr_ttl = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--sctp-maxattempts") == 0 && i + 1 < argc) {
            sctp_maxattempts = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--sctp-heartbeat") == 0 && i + 1 < argc) {
            sctp_heartbeat = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            exit(EXIT_FAILURE);
        }
    }
}

// -----------------------------------------------------------------------------
// Circular Buffer (Dynamic Allocation)
// -----------------------------------------------------------------------------
CircularBuffer *init_circular_buffer(int capacity, int mtu) {
    CircularBuffer *cb = (CircularBuffer *)malloc(sizeof(CircularBuffer));
    if (!cb) {
        fprintf(stderr, "Failed to allocate CircularBuffer\n");
        exit(EXIT_FAILURE);
    }
    cb->capacity = capacity;
    cb->mtu = mtu;
    cb->head = 0;
    cb->tail = 0;

    pthread_mutex_init(&cb->lock, NULL);
    pthread_cond_init(&cb->cond, NULL);

    // Allocate array of pointers
    cb->data = (char **)malloc(capacity * sizeof(char *));
    if (!cb->data) {
        fprintf(stderr, "Failed to allocate cb->data\n");
        exit(EXIT_FAILURE);
    }
    cb->size = (size_t *)malloc(capacity * sizeof(size_t));
    if (!cb->size) {
        fprintf(stderr, "Failed to allocate cb->size\n");
        exit(EXIT_FAILURE);
    }

    // Allocate each slot
    for (int i = 0; i < capacity; i++) {
        cb->data[i] = (char *)malloc(mtu);
        if (!cb->data[i]) {
            fprintf(stderr, "Failed to allocate buffer slot %d\n", i);
            exit(EXIT_FAILURE);
        }
        cb->size[i] = 0;
    }
    return cb;
}

void destroy_circular_buffer(CircularBuffer *cb) {
    if (!cb) return;
    for (int i = 0; i < cb->capacity; i++) {
        free(cb->data[i]);
    }
    free(cb->data);
    free(cb->size);

    pthread_mutex_destroy(&cb->lock);
    pthread_cond_destroy(&cb->cond);

    free(cb);
}

// -----------------------------------------------------------------------------
// Socket Setup
// -----------------------------------------------------------------------------
int create_udp_socket() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("UDP socket creation failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(udp_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Allow socket reuse
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("UDP bind failed");
        close(sock);
        exit(EXIT_FAILURE);
    }

    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &udp_buffer_size, sizeof(udp_buffer_size));

    if (verbose) {
        printf("UDP socket created and bound to port %d\n", udp_port);
    }

    return sock;
}

/**
 * Attempt to connect the SCTP socket. Return 0 on success, -1 on error.
 */
static int sctp_try_connect(int sock) {
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(sctp_port);
    inet_pton(AF_INET, sctp_address, &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        if (verbose) {
            printf("Connected to SCTP server at %s:%d\n", sctp_address, sctp_port);
        }
        return 0;
    }
    return -1;
}

/**
 * Creates an SCTP socket and tries to connect, re-attempting infinitely
 * on failure with 1-second intervals. Returns connected socket descriptor.
 */
int sctp_reconnect_loop() {
    while (running) {
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
        if (sock < 0) {
            perror("SCTP socket creation failed");
            sleep(1);
            continue;
        }

        // Allow socket reuse
        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

        // SCTP RTO settings
        struct sctp_rtoinfo rtoinfo = {0};
        rtoinfo.srto_initial = rto_initial;
        rtoinfo.srto_max = rto_max;
        rtoinfo.srto_min = rto_min;
        setsockopt(sock, IPPROTO_SCTP, SCTP_RTOINFO, &rtoinfo, sizeof(rtoinfo));

        // Partial Reliability
        struct sctp_prinfo prinfo = {0};
        prinfo.pr_policy = SCTP_PR_SCTP_TTL;
        prinfo.pr_value = pr_ttl;  // TTL in milliseconds
        setsockopt(sock, IPPROTO_SCTP, SCTP_PR_SUPPORTED, &prinfo, sizeof(prinfo));

        // Delayed ACK
        struct sctp_assoc_value ack_delay = {0};
        ack_delay.assoc_id = SCTP_FUTURE_ASSOC;
        ack_delay.assoc_value = sack_delay;  // Delayed ACK time in ms
        setsockopt(sock, IPPROTO_SCTP, SCTP_DELAYED_ACK_TIME, &ack_delay, sizeof(ack_delay));

        // Disable Nagle's algorithm
        int flag = 1;
        setsockopt(sock, IPPROTO_SCTP, SCTP_NODELAY, &flag, sizeof(flag));

        // Increase buffer sizes
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sctp_buffer_size, sizeof(sctp_buffer_size));
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &sctp_buffer_size, sizeof(sctp_buffer_size));

        // Heartbeat
        struct sctp_paddrparams paddrparams;
        memset(&paddrparams, 0, sizeof(paddrparams));
        paddrparams.spp_flags = SPP_HB_ENABLE; 
        paddrparams.spp_hbinterval = sctp_heartbeat; // ms
        paddrparams.spp_pathmaxrxt = 5;  // A typical default 
        paddrparams.spp_assoc_id = SCTP_FUTURE_ASSOC;
        setsockopt(sock, IPPROTO_SCTP, SCTP_PEER_ADDR_PARAMS, &paddrparams, sizeof(paddrparams));

        // Max Attempts
        struct sctp_initmsg initmsg;
        socklen_t opt_len = sizeof(initmsg);
        memset(&initmsg, 0, opt_len);
        // first get defaults
        if (getsockopt(sock, IPPROTO_SCTP, SCTP_INITMSG, &initmsg, &opt_len) == 0) {
            initmsg.sinit_max_attempts = (uint16_t)sctp_maxattempts;
            setsockopt(sock, IPPROTO_SCTP, SCTP_INITMSG, &initmsg, opt_len);
        }

        if (sctp_try_connect(sock) == 0) {
            // success
            return sock;
        }

        if (verbose) {
            fprintf(stderr, "SCTP connection failed: %s. Retrying in 1s...\n", strerror(errno));
        }
        close(sock);
        sleep(1);
    }
    return -1; // Should not get here if running is toggled
}

int connect_sctp_socket() {
    // Use the infinite reconnect loop
    return sctp_reconnect_loop();
}

// -----------------------------------------------------------------------------
// UDP Receiver Thread
// -----------------------------------------------------------------------------
void *udp_receiver(void *arg) {
    int udp_sock = create_udp_socket();
    char *buffer = (char *)malloc(mtu);
    if (!buffer) {
        fprintf(stderr, "Failed to allocate temp UDP buffer\n");
        exit(EXIT_FAILURE);
    }

    struct timespec now;
    while (running) {
        ssize_t len = recvfrom(udp_sock, buffer, mtu, 0, NULL, NULL);
        if (len < 0) {
            if (errno == EINTR || !running) break; // Interrupted by signal
            continue;
        }
        if (len > 0) {
            // Update stats
            pthread_mutex_lock(&stats_lock);
            bytes_in += (size_t)len;
            packets_in++;
            pthread_mutex_unlock(&stats_lock);

            // Calculate inter-arrival time for histogram
            clock_gettime(CLOCK_MONOTONIC, &now);
            update_histogram(&now);

            // Enqueue data
            pthread_mutex_lock(&udp_to_sctp_queue->lock);
            int next_head = (udp_to_sctp_queue->head + 1) % udp_to_sctp_queue->capacity;
            if (next_head == udp_to_sctp_queue->tail) {
                // Queue full => increment drop counter
                pthread_mutex_lock(&stats_lock);
                udp_drops++;
                pthread_mutex_unlock(&stats_lock);
            } else {
                memcpy(udp_to_sctp_queue->data[udp_to_sctp_queue->head], buffer, (size_t)len);
                udp_to_sctp_queue->size[udp_to_sctp_queue->head] = (size_t)len;
                udp_to_sctp_queue->head = next_head;
                pthread_cond_signal(&udp_to_sctp_queue->cond);
            }
            pthread_mutex_unlock(&udp_to_sctp_queue->lock);
        }
    }

    free(buffer);
    close(udp_sock);
    return NULL;
}

// -----------------------------------------------------------------------------
// SCTP Sender Thread
// -----------------------------------------------------------------------------
void *sctp_sender(void *arg) {
    (void)arg;
    // Connect once initially (infinite attempts if necessary)
    int sctp_sock = connect_sctp_socket();

    while (running) {
        pthread_mutex_lock(&udp_to_sctp_queue->lock);
        while (udp_to_sctp_queue->head == udp_to_sctp_queue->tail && running) {
            pthread_cond_wait(&udp_to_sctp_queue->cond, &udp_to_sctp_queue->lock);
        }
        if (!running) {
            pthread_mutex_unlock(&udp_to_sctp_queue->lock);
            break;
        }

        int tail = udp_to_sctp_queue->tail;
        size_t data_size = udp_to_sctp_queue->size[tail];
        udp_to_sctp_queue->tail = (tail + 1) % udp_to_sctp_queue->capacity;
        pthread_mutex_unlock(&udp_to_sctp_queue->lock);

        ssize_t ret = sctp_sendmsg(
            sctp_sock,
            udp_to_sctp_queue->data[tail],
            data_size,
            NULL,
            0,
            0, 0, 0, 0, 0
        );
        if (ret < 0) {
            // If error implies peer closed, attempt to reconnect
            pthread_mutex_lock(&stats_lock);
            sctp_drops++;
            pthread_mutex_unlock(&stats_lock);

            if (verbose) perror("SCTP sendmsg failed");
            if ((errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) && running) {
                if (verbose) fprintf(stderr, "SCTP peer disconnected, reconnecting...\n");
                close(sctp_sock);
                // Attempt infinite reconnections
                sctp_sock = connect_sctp_socket();
                if (sctp_sock < 0) {
                    if (verbose) fprintf(stderr, "Reconnection logic failed, stopping.\n");
                    running = 0;
                    break;
                }
            } else {
                // Some other error
                if (verbose) fprintf(stderr, "Non-recoverable SCTP error. Exiting.\n");
                running = 0;
                close(sctp_sock);
                break;
            }
        } else {
            // Update stats
            pthread_mutex_lock(&stats_lock);
            bytes_out += (size_t)ret;
            packets_out++;
            pthread_mutex_unlock(&stats_lock);
        }
    }

    close(sctp_sock);
    return NULL;
}

// -----------------------------------------------------------------------------
// Statistics Thread
// -----------------------------------------------------------------------------
void print_sctp_snmp_stats() {
    // Open and parse /proc/net/sctp/snmp
    FILE *fp = fopen("/proc/net/sctp/snmp", "r");
    if (!fp) {
        fprintf(stderr, "Failed to open /proc/net/sctp/snmp\n");
        return;
    }

    struct {
        const char *key;
        const char *friendly;
        unsigned long value;
        int found;
    } map[] = {
        {"SctpCurrEstab",        "Current Established"},
        {"SctpActiveEstabs",     "Active Establishments"},
        {"SctpPassiveEstabs",    "Passive Establishments"},
        {"SctpAborteds",         "Aborted"},
        {"SctpShutdowns",        "Shutdowns"},
        {"SctpOutOfBlues",       "Out Of Blue Packets"},
        {"SctpChecksumErrors",   "Checksum Errors"},
        {"SctpOutCtrlChunks",    "Out Control Chunks"},
        {"SctpOutOrderChunks",   "Out Ordered Chunks"},
        {"SctpOutUnorderChunks", "Out Unordered Chunks"},
        {"SctpInCtrlChunks",     "In Control Chunks"},
        {"SctpInOrderChunks",    "In Ordered Chunks"},
        {"SctpInUnorderChunks",  "In Unordered Chunks"},
        {"SctpOutSCTPPacks",     "SCTP Pkts Out"},
        {"SctpInSCTPPacks",      "SCTP Pkts In"},
        {"SctpT3RtxExpireds",    "T3 RTX Expired"},
        {"SctpFastRetransmits",  "Fast Retransmits"}
    };

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char key[128];
        unsigned long val;
        if (sscanf(line, "%127s %lu", key, &val) == 2) {
            // Match against our map
            for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
                if (strcmp(map[i].key, key) == 0) {
                    map[i].value = val;
                    map[i].found = 1;
                    break;
                }
            }
        }
    }
    fclose(fp);

    // Print results
    printf("\n--- SCTP SNMP Stats ---\n");
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
        if (map[i].found) {
            printf("  %-25s : %lu\n", map[i].friendly, map[i].value);
        }
    }
}

void *stats_printer(void *arg) {
    (void)arg;
    // We'll track how many bytes/packets have passed in the last 2s
    size_t prev_bytes_in = 0, prev_bytes_out = 0;
    size_t prev_packets_in = 0, prev_packets_out = 0;

    while (running) {
        sleep(2);

        // Snapshot current stats
        pthread_mutex_lock(&stats_lock);
        size_t cur_bytes_in   = bytes_in;
        size_t cur_bytes_out  = bytes_out;
        size_t cur_packets_in = packets_in;
        size_t cur_packets_out = packets_out;
        size_t cur_udp_drops  = udp_drops;
        size_t cur_sctp_drops = sctp_drops;
        pthread_mutex_unlock(&stats_lock);

        // Compute deltas over this 2s interval
        size_t delta_in      = cur_bytes_in - prev_bytes_in;
        size_t delta_out     = cur_bytes_out - prev_bytes_out;
        size_t delta_in_pkts = cur_packets_in - prev_packets_in;
        size_t delta_out_pkts = cur_packets_out - prev_packets_out;

        double interval_sec = 2.0;

        // Convert to Mbit/s
        double mbit_in  = (double)delta_in  * 8.0 / (1e6 * interval_sec);
        double mbit_out = (double)delta_out * 8.0 / (1e6 * interval_sec);

        // Convert to packets/s
        double pps_in  = (double)delta_in_pkts  / interval_sec;
        double pps_out = (double)delta_out_pkts / interval_sec;

        // Update "previous" counters
        prev_bytes_in   = cur_bytes_in;
        prev_bytes_out  = cur_bytes_out;
        prev_packets_in = cur_packets_in;
        prev_packets_out = cur_packets_out;

        // Print throughput stats
        printf("\n=== 2s Interval Stats ===\n");
        printf("UDP In  : %.2f Mbit/s, %.2f packets/s\n", mbit_in, pps_in);
        printf("SCTP Out: %.2f Mbit/s, %.2f packets/s\n", mbit_out, pps_out);

        // Print drop counters
        printf("\n--- Drop Counters ---\n");
        printf("UDP drops: %zu\n", cur_udp_drops);
        printf("SCTP drops: %zu\n", cur_sctp_drops);

        // Inter-arrival histogram
        pthread_mutex_lock(&hist_lock);
        printf("\n--- UDP Inter-arrival Histogram (Last 2s) ---\n");
        const char *labels[HIST_BINS] = {
            "<1ms", "<2ms", "<5ms", "<10ms", "<20ms",
            "<50ms", "<100ms", "<200ms", ">200ms"
        };
        for (int i = 0; i < HIST_BINS; i++) {
            printf("  %6s : %lu\n", labels[i], hist_bins[i]);
        }
        // Reset histogram each interval
        memset(hist_bins, 0, sizeof(hist_bins));
        pthread_mutex_unlock(&hist_lock);

        // Print configured buffers
        printf("\n--- Buffer Configuration ---\n");
        printf("UDP RCVBUF  : %d bytes\n", udp_buffer_size);
        printf("SCTP SNDBUF : %d bytes\n", sctp_buffer_size);
        printf("Gateway queue capacity : %d slots\n", udp_to_sctp_queue->capacity);

        // Print current queue usage
        pthread_mutex_lock(&udp_to_sctp_queue->lock);
        int used = (udp_to_sctp_queue->head - udp_to_sctp_queue->tail + udp_to_sctp_queue->capacity)
                    % udp_to_sctp_queue->capacity;
        pthread_mutex_unlock(&udp_to_sctp_queue->lock);
        printf("Gateway queue usage   : %d of %d slots\n", used, udp_to_sctp_queue->capacity);

        // Also read /proc/net/sctp/snmp
        print_sctp_snmp_stats();
    }
    return NULL;
}

// -----------------------------------------------------------------------------
// Histogram Update
// -----------------------------------------------------------------------------
void update_histogram(struct timespec *now) {
    pthread_mutex_lock(&hist_lock);

    long long now_ns = timespec_to_ns(now);
    if (last_packet_time_ns != 0) {
        long long delta = now_ns - last_packet_time_ns;
        // Classify into a bin
        int bin_index;
        for (bin_index = 0; bin_index < (HIST_BINS - 1); bin_index++) {
            if (delta < bin_edges[bin_index]) {
                hist_bins[bin_index]++;
                break;
            }
        }
        // If >= last bin edge, it goes in the final bin
        if (bin_index == (HIST_BINS - 1)) {
            hist_bins[HIST_BINS - 1]++;
        }
    }
    last_packet_time_ns = now_ns;

    pthread_mutex_unlock(&hist_lock);
}

// -----------------------------------------------------------------------------
// Graceful Exit
// -----------------------------------------------------------------------------
void graceful_exit() {
    running = 0;
    pthread_cond_broadcast(&udp_to_sctp_queue->cond);
}

// -----------------------------------------------------------------------------
// main()
// -----------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    parse_arguments(argc, argv);
    signal(SIGINT, handle_signal);

    // Initialize the circular buffer based on user-specified (or default) capacity
    udp_to_sctp_queue = init_circular_buffer(queue_size, mtu);

    pthread_t udp_thread, sctp_thread, stats_thread;

    // Start threads
    pthread_create(&udp_thread, NULL, udp_receiver, NULL);
    pthread_create(&sctp_thread, NULL, sctp_sender, NULL);

    // If verbose, start stats thread
    if (verbose) {
        pthread_create(&stats_thread, NULL, stats_printer, NULL);
    }

    // Join threads
    pthread_join(udp_thread, NULL);
    pthread_join(sctp_thread, NULL);

    if (verbose) {
        // Signal stats thread to exit if it's running
        pthread_cancel(stats_thread);
        pthread_join(stats_thread, NULL);
    }

    destroy_circular_buffer(udp_to_sctp_queue);
    return 0;
}
