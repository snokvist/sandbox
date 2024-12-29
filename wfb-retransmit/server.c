#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "config_loader.h"

typedef struct {
    int sequence_number;
    char *data;
} Packet;

Packet *buffer;
int buffer_size;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

long long packets_received = 0;
long long packets_retransmitted = 0;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

void *receive_and_forward(void *arg) {
    Config *config = (Config *)arg;
    int sock_recv, sock_forward;
    struct sockaddr_in addr_recv, addr_forward;
    int seq = 0;

    // Create UDP sockets
    sock_recv = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_recv < 0) {
        perror("Failed to create receive socket");
        exit(EXIT_FAILURE);
    }
    sock_forward = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_forward < 0) {
        perror("Failed to create forward socket");
        exit(EXIT_FAILURE);
    }

    // Configure the receive socket
    memset(&addr_recv, 0, sizeof(addr_recv));
    addr_recv.sin_family = AF_INET;
    addr_recv.sin_port = htons(config->server_recv_port);
    addr_recv.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(sock_recv, (struct sockaddr *)&addr_recv, sizeof(addr_recv)) < 0) {
        perror("Failed to bind receive socket");
        exit(EXIT_FAILURE);
    }

    // Configure the forward socket
    memset(&addr_forward, 0, sizeof(addr_forward));
    addr_forward.sin_family = AF_INET;
    addr_forward.sin_port = htons(config->server_send_port);
    addr_forward.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    printf("Server listening for packets on 127.0.0.1:%d...\n", config->server_recv_port);

    while (1) {
        char *recv_buf = malloc(config->buf_size);
        ssize_t recv_len = recvfrom(sock_recv, recv_buf, config->buf_size, 0, NULL, NULL);
        if (recv_len <= 0) {
            perror("Error receiving data");
            free(recv_buf);
            continue;
        }

        pthread_mutex_lock(&stats_mutex);
        packets_received++;
        pthread_mutex_unlock(&stats_mutex);

        pthread_mutex_lock(&buffer_mutex);
        int buffer_index = seq % buffer_size;
        if (buffer[buffer_index].data != NULL) {
            free(buffer[buffer_index].data);  // Free old data in the buffer
        }
        buffer[buffer_index].sequence_number = seq;
        buffer[buffer_index].data = recv_buf;
        seq++;
        pthread_mutex_unlock(&buffer_mutex);

        sendto(sock_forward, recv_buf, recv_len, 0, (struct sockaddr *)&addr_forward, sizeof(addr_forward));

        if (config->server_verbose) {
            printf("Forwarded Packet: Seq=%d, Size=%ld bytes\n", seq - 1, recv_len);
        }
    }

    close(sock_recv);
    close(sock_forward);
    return NULL;
}

void *handle_retransmit_requests(void *arg) {
    Config *config = (Config *)arg;
    int sock_req;
    struct sockaddr_in addr_req, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buf[1 + config->buf_size * sizeof(int)];
    int count, seq;

    // Create the retransmit socket
    sock_req = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_req < 0) {
        perror("Failed to create retransmit socket");
        exit(EXIT_FAILURE);
    }

    // Configure the retransmit socket
    memset(&addr_req, 0, sizeof(addr_req));
    addr_req.sin_family = AF_INET;
    addr_req.sin_port = htons(config->server_retransmit_port);
    addr_req.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock_req, (struct sockaddr *)&addr_req, sizeof(addr_req)) < 0) {
        perror("Failed to bind retransmit socket");
        exit(EXIT_FAILURE);
    }

    printf("Server listening for retransmit requests on port %d...\n", config->server_retransmit_port);

    while (1) {
        int recv_len = recvfrom(sock_req, buf, sizeof(buf), 0, (struct sockaddr *)&client_addr, &addr_len);
        if (recv_len <= 0) continue;

        count = buf[0];
        pthread_mutex_lock(&stats_mutex);
        packets_retransmitted += count;
        pthread_mutex_unlock(&stats_mutex);

        pthread_mutex_lock(&buffer_mutex);
        for (int i = 0; i < count; ++i) {
            seq = ntohl(*(int *)(buf + 1 + i * sizeof(int)));
            int buffer_index = seq % buffer_size;

            if (buffer[buffer_index].sequence_number == seq) {
                sendto(sock_req, buffer[buffer_index].data, config->buf_size, 0, (struct sockaddr *)&client_addr, addr_len);
                if (config->server_verbose) {
                    printf("Retransmitted Packet: Seq=%d\n", seq);
                }
            }
        }
        pthread_mutex_unlock(&buffer_mutex);
    }

    close(sock_req);
    return NULL;
}

void *print_statistics(void *arg) {
    Config *config = (Config *)arg;

    while (1) {
        usleep(config->stats_interval_ms * 1000);  // Wait for the configured interval

        if (config->server_verbose) {
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

    buffer_size = config.buffer_size;
    buffer = malloc(buffer_size * sizeof(Packet));
    for (int i = 0; i < buffer_size; ++i) {
        buffer[i].data = NULL;
    }

    pthread_t thread1, thread2, thread3;

    pthread_create(&thread1, NULL, receive_and_forward, &config);
    pthread_create(&thread2, NULL, handle_retransmit_requests, &config);
    pthread_create(&thread3, NULL, print_statistics, &config);

    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);
    pthread_join(thread3, NULL);

    for (int i = 0; i < buffer_size; ++i) {
        free(buffer[i].data);
    }
    free(buffer);
    return 0;
}
