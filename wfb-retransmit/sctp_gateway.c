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

#define DEFAULT_UDP_PORT 5600
#define DEFAULT_SCTP_PORT 6600
#define DEFAULT_SCTP_ADDRESS "10.5.0.1"
#define BUFFER_SIZE 2048
#define QUEUE_SIZE 1024
#define DEFAULT_RTO_MIN 2      // Minimum RTO in ms
#define DEFAULT_RTO_MAX 10     // Maximum RTO in ms
#define DEFAULT_RTO_INITIAL 2  // Initial RTO in ms
#define DEFAULT_BUFFER_SIZE 16 // Default buffer size in KB
#define MAX_RECONNECT_DELAY 10000 // Max delay in ms for exponential backoff

typedef struct {
    char data[QUEUE_SIZE][BUFFER_SIZE];
    int head;
    int tail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} CircularBuffer;

volatile int running = 1; // Flag to handle graceful shutdown
int verbose = 0;          // Verbose flag (set by --verbose)

CircularBuffer udp_to_sctp_queue = {
    .head = 0,
    .tail = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

// Configurable parameters
int udp_port = DEFAULT_UDP_PORT;
int sctp_port = DEFAULT_SCTP_PORT;
char sctp_address[INET_ADDRSTRLEN] = DEFAULT_SCTP_ADDRESS;
int rto_min = DEFAULT_RTO_MIN;
int rto_max = DEFAULT_RTO_MAX;
int rto_initial = DEFAULT_RTO_INITIAL;
int udp_buffer_size = DEFAULT_BUFFER_SIZE * 1024;  // Convert KB to bytes
int sctp_buffer_size = DEFAULT_BUFFER_SIZE * 1024; // Convert KB to bytes;

// Function to parse command-line arguments
void parse_arguments(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--udp-port") == 0 && i + 1 < argc) {
            udp_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--sctp-port") == 0 && i + 1 < argc) {
            sctp_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--sctp-address") == 0 && i + 1 < argc) {
            strncpy(sctp_address, argv[++i], INET_ADDRSTRLEN);
        } else if (strcmp(argv[i], "--rto-min") == 0 && i + 1 < argc) {
            rto_min = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rto-max") == 0 && i + 1 < argc) {
            rto_max = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rto-initial") == 0 && i + 1 < argc) {
            rto_initial = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--udp-buffer") == 0 && i + 1 < argc) {
            udp_buffer_size = atoi(argv[++i]) * 1024; // Convert KB to bytes
        } else if (strcmp(argv[i], "--sctp-buffer") == 0 && i + 1 < argc) {
            sctp_buffer_size = atoi(argv[++i]) * 1024; // Convert KB to bytes
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            exit(EXIT_FAILURE);
        }
    }
}

// Function to add data to the circular buffer
int enqueue(CircularBuffer *queue, const char *data, size_t len) {
    pthread_mutex_lock(&queue->lock);

    int next_head = (queue->head + 1) % QUEUE_SIZE;
    if (next_head == queue->tail) {
        pthread_mutex_unlock(&queue->lock);
        return -1; // Queue full
    }

    memcpy(queue->data[queue->head], data, len);
    queue->head = next_head;

    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->lock);
    return 0;
}

// Function to retrieve data from the circular buffer
int dequeue(CircularBuffer *queue, char *data) {
    pthread_mutex_lock(&queue->lock);

    while (queue->head == queue->tail && running) {
        pthread_cond_wait(&queue->cond, &queue->lock);
    }

    if (!running) {
        pthread_mutex_unlock(&queue->lock);
        return -1; // Shutdown
    }

    memcpy(data, queue->data[queue->tail], BUFFER_SIZE);
    queue->tail = (queue->tail + 1) % QUEUE_SIZE;

    pthread_mutex_unlock(&queue->lock);
    return 0;
}

// Signal handler for graceful shutdown
void handle_signal(int sig) {
    running = 0;
    pthread_cond_broadcast(&udp_to_sctp_queue.cond);
}

// SCTP connection establishment with reconnection logic
int connect_sctp_socket() {
    int sctp_sock;
    struct sockaddr_in sctp_addr = {0};

    sctp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
    if (sctp_sock < 0) {
        perror("SCTP socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Set SCTP options
    struct sctp_rtoinfo rtoinfo = {0};
    rtoinfo.srto_initial = rto_initial;
    rtoinfo.srto_max = rto_max;
    rtoinfo.srto_min = rto_min;
    setsockopt(sctp_sock, IPPROTO_SCTP, SCTP_RTOINFO, &rtoinfo, sizeof(rtoinfo));

    int flag = 1;
    setsockopt(sctp_sock, IPPROTO_SCTP, SCTP_NODELAY, &flag, sizeof(flag));
    setsockopt(sctp_sock, SOL_SOCKET, SO_SNDBUF, &sctp_buffer_size, sizeof(sctp_buffer_size));

    // Configure SCTP address
    sctp_addr.sin_family = AF_INET;
    sctp_addr.sin_port = htons(sctp_port);
    inet_pton(AF_INET, sctp_address, &sctp_addr.sin_addr);

    // Attempt connection with exponential backoff
    int delay = 100; // Initial delay in ms
    while (connect(sctp_sock, (struct sockaddr *)&sctp_addr, sizeof(sctp_addr)) < 0) {
        if (verbose) fprintf(stderr, "SCTP connection failed: %s. Retrying in %d ms...\n", strerror(errno), delay);

        usleep(delay * 1000);
        delay = delay < MAX_RECONNECT_DELAY ? delay * 2 : MAX_RECONNECT_DELAY;
        if (!running) {
            close(sctp_sock);
            return -1;
        }
    }

    if (verbose) printf("Connected to SCTP receiver at %s:%d\n", sctp_address, sctp_port);
    return sctp_sock;
}

// UDP receiver thread
void *udp_receiver(void *arg) {
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        perror("UDP socket creation failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in udp_addr = {0};
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_port = htons(udp_port);
    udp_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(udp_sock, (struct sockaddr *)&udp_addr, sizeof(udp_addr)) < 0) {
        perror("UDP bind failed");
        close(udp_sock);
        exit(EXIT_FAILURE);
    }

    setsockopt(udp_sock, SOL_SOCKET, SO_RCVBUF, &udp_buffer_size, sizeof(udp_buffer_size));

    if (verbose) printf("Listening for UDP packets on 127.0.0.1:%d...\n", udp_port);

    char buffer[BUFFER_SIZE];
    while (running) {
        ssize_t recv_len = recvfrom(udp_sock, buffer, BUFFER_SIZE, 0, NULL, NULL);
        if (recv_len > 0) {
            if (enqueue(&udp_to_sctp_queue, buffer, recv_len) < 0 && verbose) {
                fprintf(stderr, "UDP packet dropped (queue full)\n");
            }
        } else if (recv_len < 0 && errno != EAGAIN) {
            perror("UDP recvfrom failed");
        }
    }

    close(udp_sock);
    return NULL;
}

// SCTP sender thread
void *sctp_sender(void *arg) {
    char buffer[BUFFER_SIZE];
    int sctp_sock = connect_sctp_socket();
    if (sctp_sock < 0) return NULL;

    while (running) {
        if (dequeue(&udp_to_sctp_queue, buffer) == 0) {
            if (sctp_sendmsg(sctp_sock, buffer, BUFFER_SIZE, NULL, 0, 0, 0, 0, 0, 0) < 0 && verbose) {
                perror("SCTP sendmsg failed");
            }
        }
    }

    close(sctp_sock);
    return NULL;
}

int main(int argc, char *argv[]) {
    parse_arguments(argc, argv);

    signal(SIGINT, handle_signal);

    pthread_t udp_thread, sctp_thread;
    pthread_create(&udp_thread, NULL, udp_receiver, NULL);
    pthread_create(&sctp_thread, NULL, sctp_sender, NULL);

    pthread_join(udp_thread, NULL);
    pthread_join(sctp_thread, NULL);

    return 0;
}
