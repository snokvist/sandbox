#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "config_loader.h"

#define MAX_MISSING_PACKETS 100  // Maximum tracked missing packets
#define MAX_RETRANSMIT_QUEUE_SIZE 100  // Maximum retransmits in queue
#define MAX_BATCH_SIZE 20  // Max retransmit requests in one batch

typedef struct {
    int sequence_number;
    long long expiration_time;
} MissingPacket;

MissingPacket missing_packets[MAX_MISSING_PACKETS];
int missing_count = 0;
pthread_mutex_t missing_mutex = PTHREAD_MUTEX_INITIALIZER;

long long packets_received = 0;
long long packets_retransmitted = 0;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

void add_missing_packet(int seq, long long hold_duration) {
    pthread_mutex_lock(&missing_mutex);
    if (missing_count < MAX_MISSING_PACKETS) {
        missing_packets[missing_count].sequence_number = seq;
        missing_packets[missing_count].expiration_time = time(NULL) * 1000 + hold_duration;
        missing_count++;
    }
    pthread_mutex_unlock(&missing_mutex);
}

void remove_expired_packets() {
    pthread_mutex_lock(&missing_mutex);
    long long current_time = time(NULL) * 1000;
    int i = 0;
    while (i < missing_count) {
        if (missing_packets[i].expiration_time <= current_time) {
            missing_packets[i] = missing_packets[missing_count - 1];
            missing_count--;
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&missing_mutex);
}

void *receive_packets(void *arg) {
    Config *config = (Config *)arg;
    int sock_recv;
    struct sockaddr_in addr_recv;
    int last_seq = -1;

    // Create UDP socket
    sock_recv = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_recv < 0) {
        perror("Failed to create socket");
        exit(EXIT_FAILURE);
    }

    // Configure the address
    memset(&addr_recv, 0, sizeof(addr_recv));
    addr_recv.sin_family = AF_INET;
    addr_recv.sin_port = htons(config->client_recv_port);
    addr_recv.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Bind the socket
    if (bind(sock_recv, (struct sockaddr *)&addr_recv, sizeof(addr_recv)) < 0) {
        perror("Failed to bind socket");
        exit(EXIT_FAILURE);
    }

    printf("Listening for UDP packets on 127.0.0.1:%d...\n", config->client_recv_port);

    while (1) {
        char *recv_buf = malloc(config->buf_size);
        ssize_t recv_len = recvfrom(sock_recv, recv_buf, config->buf_size, 0, NULL, NULL);
        if (recv_len <= 0) {
            perror("Error receiving data");
            free(recv_buf);
            continue;
        }

        int seq = *(int *)recv_buf;

        pthread_mutex_lock(&stats_mutex);
        packets_received++;
        pthread_mutex_unlock(&stats_mutex);

        if (seq != last_seq + 1) {
            for (int i = last_seq + 1; i < seq; ++i) {
                add_missing_packet(i, config->hold_duration_ms);
            }
        }
        last_seq = seq;

        if (config->client_verbose) {
            printf("Received Packet: Seq=%d, Size=%ld bytes\n", seq, recv_len - sizeof(int));
        }

        free(recv_buf);
    }

    close(sock_recv);
    return NULL;
}

void *request_missing_packets(void *arg) {
    Config *config = (Config *)arg;
    int sock_req;
    struct sockaddr_in addr_req;
    char request[1 + MAX_BATCH_SIZE * sizeof(int)];
    int batch_size;

    sock_req = socket(AF_INET, SOCK_DGRAM, 0);

    memset(&addr_req, 0, sizeof(addr_req));
    addr_req.sin_family = AF_INET;
    addr_req.sin_port = htons(config->client_retransmit_port);
    addr_req.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    while (1) {
        batch_size = 0;

        pthread_mutex_lock(&missing_mutex);
        for (int i = 0; i < missing_count && batch_size < MAX_BATCH_SIZE; ++i) {
            if (batch_size < MAX_RETRANSMIT_QUEUE_SIZE) {
                int seq = missing_packets[i].sequence_number;
                *(int *)(request + 1 + batch_size * sizeof(int)) = htonl(seq);
                batch_size++;
            }
        }
        pthread_mutex_unlock(&missing_mutex);

        if (batch_size > 0) {
            request[0] = (char)batch_size;
            sendto(sock_req, request, 1 + batch_size * sizeof(int), 0, (struct sockaddr *)&addr_req, sizeof(addr_req));

            pthread_mutex_lock(&stats_mutex);
            packets_retransmitted += batch_size;
            pthread_mutex_unlock(&stats_mutex);
        }

        usleep(1000);  // Wait 1ms to avoid flooding
        remove_expired_packets();
    }

    close(sock_req);
    return NULL;
}

void *print_statistics(void *arg) {
    Config *config = (Config *)arg;

    while (1) {
        usleep(config->stats_interval_ms * 1000);

        if (config->client_verbose) {
            pthread_mutex_lock(&stats_mutex);
            printf("Statistics: Packets Received=%lld, Packets Retransmitted=%lld\n",
                   packets_received, packets_retransmitted);
            pthread_mutex_unlock(&stats_mutex);
        }
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3 || strcmp(argv[1], "--config") != 0) {
        fprintf(stderr, "Usage: %s --config <config_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    Config config;
    load_config(argv[2], &config);

    pthread_t thread1, thread2, thread3;

    pthread_create(&thread1, NULL, receive_packets, &config);
    pthread_create(&thread2, NULL, request_missing_packets, &config);
    pthread_create(&thread3, NULL, print_statistics, &config);

    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);
    pthread_join(thread3, NULL);

    return 0;
}
