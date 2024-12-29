#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int buf_size;               // Shared packet size
    int client_recv_port;
    int client_send_port;
    int client_retransmit_port;
    int hold_duration_ms;
    int stats_interval_ms;
    int client_verbose;
} Config;

void load_config(const char *filename, Config *config) {
    // Initialize default values
    config->buf_size = 4096;
    config->client_recv_port = 5601;
    config->client_send_port = 5600;
    config->client_retransmit_port = 5666;
    config->hold_duration_ms = 4;
    config->stats_interval_ms = 1000;
    config->client_verbose = 1;

    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Error opening config file");
        exit(EXIT_FAILURE);
    }

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '#' || line[0] == '\n') continue; // Skip comments and empty lines

        char key[128];
        int value;
        if (sscanf(line, "%127[^=]=%d", key, &value) == 2) {
            if (strcmp(key, "buf_size") == 0) config->buf_size = value;
            else if (strcmp(key, "client_recv_port") == 0) config->client_recv_port = value;
            else if (strcmp(key, "client_send_port") == 0) config->client_send_port = value;
            else if (strcmp(key, "client_retransmit_port") == 0) config->client_retransmit_port = value;
            else if (strcmp(key, "hold_duration_ms") == 0) config->hold_duration_ms = value;
            else if (strcmp(key, "stats_interval_ms") == 0) config->stats_interval_ms = value;
            else if (strcmp(key, "client_verbose") == 0) config->client_verbose = value;
        } else {
            fprintf(stderr, "Invalid config line: %s", line);
        }
    }

    fclose(file);
}

#endif
