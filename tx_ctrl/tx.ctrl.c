// tx_ctrl.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <math.h>
#include <getopt.h>        // For getopt_long and related definitions
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/wait.h>      // For waitpid

// Constants and Macros
#define INTERFACE_NAME_DEFAULT "wlan0"
#define CARD_TYPE_RTL8812EU 1
#define CARD_TYPE_RTL8812AU 2
#define CARD_TYPE_RTL8733BU 3
#define CARD_TYPE_DEFAULT CARD_TYPE_RTL8812EU  // Default to rtl8812eu

#define TX_POWER_MIN_DEFAULT 100   // 1 dBm
#define TX_POWER_MAX_LIMIT 3000    // 30 dBm
#define TX_POWER_ADJUST_MIN 100    // Minimum adjustment: 100 mBm (1 dBm)
#define TX_POWER_ADJUST_MAX 500    // Maximum adjustment: 500 mBm (5 dBm)

#define ROUND_UP_TO_NEAREST_100(x) (((x) + 99) / 100 * 100)

#define HYSTERESIS_OFFSET_RSSI 12
#define HYSTERESIS_OFFSET_SNR 6
#define DEAD_BAND_HALF_RSSI 6
#define DEAD_BAND_HALF_SNR 3

#define FEC_LIMIT_DEFAULT 50
#define LOST_LIMIT_DEFAULT 5
#define RECOVER_TIMEOUT_DEFAULT 10  // in seconds

#define MAX_BUFFER_SIZE 1024
#define MAX_LINE_LENGTH 512

#define TCP_PORT 9995

// Enumerations
typedef enum {
    PID_CONTROL_RSSI,
    PID_CONTROL_SNR
} pid_control_type_t;

typedef enum {
    HYSTERESIS_STATE_LOW = -1,
    HYSTERESIS_STATE_DEADBAND = 0,
    HYSTERESIS_STATE_HIGH = 1
} hysteresis_state_t;

// Structures
typedef struct {
    int verbose;
    char interface_name[256];
    int card_type;
    int tx_power_min;
    int tx_power_max;
    int tx_power_adjust_min;
    int tx_power_adjust_max;
    int current_tx_power;

    pid_control_type_t pid_control_type;
    int target_value;

    unsigned int fec_limit;       // Changed to unsigned int
    unsigned int lost_limit;      // Changed to unsigned int
    int recover_timeout;

    int alink_enabled;
    double hysteresis_value;
    double deadband_lower;
    hysteresis_state_t hysteresis_state;
    time_t last_script_call_time;

    double ema_rssi;
    double ema_snr;
    double ema_alpha;
    int ema_initialized;

    double integral;
    double previous_error;

    double Kp;
    double Ki;
    double Kd;

    uint64_t total_packets;
    uint64_t lost_packets;
    uint64_t unrecoverable_packets;

    int pid_control_enabled;
    int manual_mode_enabled;
    time_t pid_paused_until;

    int tcp_server_fd;
    volatile sig_atomic_t signal_received;
    volatile sig_atomic_t terminate;

    // For command-line argument parsing
    int argc;
    char **argv;
} tx_ctrl_config_t;

// Function Prototypes
void print_help(const char *program_name);
void initialize_config(tx_ctrl_config_t *config);
int parse_arguments(tx_ctrl_config_t *config);
const char* get_card_type_name(int card_type);
void signal_handler(int sig);
void cleanup();
int setup_tcp_server(tx_ctrl_config_t *config);
void close_tcp_server(tx_ctrl_config_t *config);
void main_loop(tx_ctrl_config_t *config);
int process_stdin_input(tx_ctrl_config_t *config);
void process_line(tx_ctrl_config_t *config, const char *line);
int adjust_tx_power(tx_ctrl_config_t *config, int tx_power);
int calculate_pid_output(tx_ctrl_config_t *config, double current_value);
void update_hysteresis(tx_ctrl_config_t *config, double current_value);
void call_script(tx_ctrl_config_t *config, const char *script_name, const char *argument);
void initialize_hysteresis(tx_ctrl_config_t *config);
void initialize_tx_power(tx_ctrl_config_t *config);
void print_current_settings(tx_ctrl_config_t *config);
void process_tcp_connection(tx_ctrl_config_t *config);
int execute_command(const char *command, char *const argv[]);

// Global Configuration
tx_ctrl_config_t config;

// Main Function
int main(int argc, char *argv[]) {
    // Initialize configuration
    memset(&config, 0, sizeof(tx_ctrl_config_t));
    config.argc = argc;
    config.argv = argv;
    initialize_config(&config);

    // Parse command-line arguments
    if (parse_arguments(&config) != 0) {
        return EXIT_FAILURE;
    }

    // Set up signal handlers
    struct sigaction sa;
    sa.sa_handler = &signal_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGUSR1, &sa, NULL) == -1 ||
        sigaction(SIGUSR2, &sa, NULL) == -1 ||
        sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("Error setting up signal handlers");
        return EXIT_FAILURE;
    }

    // Register cleanup function
    if (atexit(cleanup) != 0) {
        fprintf(stderr, "Failed to register cleanup function.\n");
        return EXIT_FAILURE;
    }

    // Initialize hysteresis if alink is enabled
    if (config.alink_enabled) {
        initialize_hysteresis(&config);
    }

    // Print current settings
    print_current_settings(&config);

    // Initialize TX power
    initialize_tx_power(&config);

    // Start main loop
    main_loop(&config);

    return EXIT_SUCCESS;
}

// Function Implementations

/**
 * @brief Prints the help message.
 * @param program_name The name of the program.
 */
void print_help(const char *program_name) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Options:\n");
    printf("  --help                 Show this help message and exit\n");
    printf("  --verbose              Enable verbose output\n");
    printf("  --wlanid=ID            Specify the network interface name (required)\n");
    printf("  --card-type=NAME       Specify WiFi card type:\n");
    printf("                           'rtl8812eu'\n");
    printf("                           'rtl8812au'\n");
    printf("                           'rtl8733bu'\n");
    printf("                         (default: %s)\n", get_card_type_name(CARD_TYPE_DEFAULT));
    printf("  --tx-min=VALUE         Override minimum TX power (in mBm)\n");
    printf("  --tx-max=VALUE         Override maximum TX power (in mBm)\n");
    printf("                         Values must be between 100 and 3000, rounded up to nearest 100.\n");
    printf("  --target-value=VAL     Set target RSSI/SNR value (default: -70 for RSSI, 20 for SNR)\n");
    printf("  --pid-control=TYPE     Use 'rssi' or 'snr' for PID controller (default: rssi)\n");
    printf("  --fec-limit=VALUE      Set FEC recovered packets limit (default: %d)\n", FEC_LIMIT_DEFAULT);
    printf("  --lost-limit=VALUE     Set lost packets limit (default: %d)\n", LOST_LIMIT_DEFAULT);
    printf("                         Values must be between 1 and 100.\n");
    printf("  --recover-timeout=SEC  Set recovery timeout in seconds (default: %d)\n", RECOVER_TIMEOUT_DEFAULT);
    printf("  --alink                Enable hysteresis logic and script execution\n");
}

/**
 * @brief Initializes the configuration with default values.
 * @param config Pointer to the configuration structure.
 */
void initialize_config(tx_ctrl_config_t *config) {
    // Set default values
    config->verbose = 0;
    strncpy(config->interface_name, INTERFACE_NAME_DEFAULT, sizeof(config->interface_name) - 1);
    config->card_type = CARD_TYPE_DEFAULT;
    config->tx_power_min = TX_POWER_MIN_DEFAULT;
    config->tx_power_max = 0; // Will be set based on card type
    config->tx_power_adjust_min = TX_POWER_ADJUST_MIN;
    config->tx_power_adjust_max = TX_POWER_ADJUST_MAX;

    config->pid_control_type = PID_CONTROL_RSSI;
    config->target_value = -70; // Default RSSI target

    config->fec_limit = FEC_LIMIT_DEFAULT;
    config->lost_limit = LOST_LIMIT_DEFAULT;
    config->recover_timeout = RECOVER_TIMEOUT_DEFAULT;

    config->alink_enabled = 0;
    config->hysteresis_value = 0.0;
    config->deadband_lower = 0.0;
    config->hysteresis_state = HYSTERESIS_STATE_DEADBAND;
    config->last_script_call_time = 0;

    config->ema_rssi = 0.0;
    config->ema_snr = 0.0;
    config->ema_alpha = 0.2;
    config->ema_initialized = 0;

    config->integral = 0.0;
    config->previous_error = 0.0;

    config->Kp = 1.0;
    config->Ki = 0.1;
    config->Kd = 0.05;

    config->total_packets = 0;
    config->lost_packets = 0;
    config->unrecoverable_packets = 0;

    config->pid_control_enabled = 1;
    config->manual_mode_enabled = 0;
    config->pid_paused_until = 0;

    config->tcp_server_fd = -1;
    config->signal_received = 0;
    config->terminate = 0;
}

/**
 * @brief Parses command-line arguments.
 * @param config Pointer to the configuration structure.
 * @return 0 on success, non-zero on failure.
 */
int parse_arguments(tx_ctrl_config_t *config) {
    int opt;
    int option_index = 0;
    int wlanid_provided = 0;
    int tx_max_set = 0;
    char *endptr = NULL;

    static struct option long_options[] = {
        {"help",          no_argument,       0,  0 },
        {"verbose",       no_argument,       0,  0 },
        {"wlanid",        required_argument, 0,  0 },
        {"card-type",     required_argument, 0,  0 },
        {"tx-min",        required_argument, 0,  0 },
        {"tx-max",        required_argument, 0,  0 },
        {"target-value",  required_argument, 0,  0 },
        {"pid-control",   required_argument, 0,  0 },
        {"fec-limit",     required_argument, 0,  0 },
        {"lost-limit",    required_argument, 0,  0 },
        {"recover-timeout", required_argument, 0, 0 },
        {"alink",         no_argument,       0,  0 },
        {0, 0, 0, 0}
    };

    optind = 1; // Reset optind to 1 before parsing

    while ((opt = getopt_long(config->argc, config->argv, "", long_options, &option_index)) != -1) {
        if (opt == '?') {
            // Unknown option
            print_help(config->argv[0]);
            return -1;
        }

        if (strcmp(long_options[option_index].name, "help") == 0) {
            print_help(config->argv[0]);
            exit(EXIT_SUCCESS);
        } else if (strcmp(long_options[option_index].name, "verbose") == 0) {
            config->verbose = 1;
        } else if (strcmp(long_options[option_index].name, "wlanid") == 0) {
            strncpy(config->interface_name, optarg, sizeof(config->interface_name) - 1);
            wlanid_provided = 1;
        } else if (strcmp(long_options[option_index].name, "card-type") == 0) {
            if (strcmp(optarg, "rtl8812eu") == 0) {
                config->card_type = CARD_TYPE_RTL8812EU;
            } else if (strcmp(optarg, "rtl8812au") == 0) {
                config->card_type = CARD_TYPE_RTL8812AU;
            } else if (strcmp(optarg, "rtl8733bu") == 0) {
                config->card_type = CARD_TYPE_RTL8733BU;
            } else {
                fprintf(stderr, "Invalid card-type: should be 'rtl8812eu', 'rtl8812au', or 'rtl8733bu'.\n");
                return -1;
            }
        } else if (strcmp(long_options[option_index].name, "tx-min") == 0) {
            config->tx_power_min = (int)strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || config->tx_power_min < 100 || config->tx_power_min > 3000) {
                fprintf(stderr, "Invalid tx-min value. Must be between 100 and 3000.\n");
                return -1;
            }
            config->tx_power_min = ROUND_UP_TO_NEAREST_100(config->tx_power_min);
        } else if (strcmp(long_options[option_index].name, "tx-max") == 0) {
            config->tx_power_max = (int)strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || config->tx_power_max < 100 || config->tx_power_max > 3000) {
                fprintf(stderr, "Invalid tx-max value. Must be between 100 and 3000.\n");
                return -1;
            }
            config->tx_power_max = ROUND_UP_TO_NEAREST_100(config->tx_power_max);
            tx_max_set = 1;
        } else if (strcmp(long_options[option_index].name, "target-value") == 0) {
            config->target_value = (int)strtol(optarg, &endptr, 10);
            if (*endptr != '\0') {
                fprintf(stderr, "Invalid target-value.\n");
                return -1;
            }
        } else if (strcmp(long_options[option_index].name, "pid-control") == 0) {
            if (strcmp(optarg, "rssi") == 0) {
                config->pid_control_type = PID_CONTROL_RSSI;
            } else if (strcmp(optarg, "snr") == 0) {
                config->pid_control_type = PID_CONTROL_SNR;
                if (config->target_value == -70) {
                    config->target_value = 20; // Default SNR target
                }
            } else {
                fprintf(stderr, "Invalid pid-control type: should be 'rssi' or 'snr'.\n");
                return -1;
            }
        } else if (strcmp(long_options[option_index].name, "fec-limit") == 0) {
            unsigned long val = strtoul(optarg, &endptr, 10);
            if (*endptr != '\0' || val < 1 || val > 100) {
                fprintf(stderr, "Invalid fec-limit. Must be between 1 and 100.\n");
                return -1;
            }
            config->fec_limit = (unsigned int)val;
        } else if (strcmp(long_options[option_index].name, "lost-limit") == 0) {
            unsigned long val = strtoul(optarg, &endptr, 10);
            if (*endptr != '\0' || val < 1 || val > 100) {
                fprintf(stderr, "Invalid lost-limit. Must be between 1 and 100.\n");
                return -1;
            }
            config->lost_limit = (unsigned int)val;
        } else if (strcmp(long_options[option_index].name, "recover-timeout") == 0) {
            config->recover_timeout = (int)strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || config->recover_timeout < 1) {
                fprintf(stderr, "Invalid recover-timeout. Must be greater than 0.\n");
                return -1;
            }
        } else if (strcmp(long_options[option_index].name, "alink") == 0) {
            config->alink_enabled = 1;
        }
    }

    if (!wlanid_provided) {
        fprintf(stderr, "Error: --wlanid option is required.\n");
        print_help(config->argv[0]);
        return -1;
    }

    // Set default tx_power_max based on card_type if not overridden
    if (!tx_max_set) {
        if (config->card_type == CARD_TYPE_RTL8812AU) {
            config->tx_power_max = 2000;  // 20 dBm
        } else if (config->card_type == CARD_TYPE_RTL8812EU) {
            config->tx_power_max = 2800;  // 28 dBm
        } else if (config->card_type == CARD_TYPE_RTL8733BU) {
            config->tx_power_max = 2000;  // 20 dBm
        }
    }

    // Ensure MIN is less than MAX
    if (config->tx_power_min > config->tx_power_max) {
        fprintf(stderr, "Error: tx-min (%d) must be less than or equal to tx-max (%d).\n",
                config->tx_power_min, config->tx_power_max);
        return -1;
    }

    return 0;
}

/**
 * @brief Returns the card type name.
 * @param card_type The card type enum value.
 * @return The card type name as a string.
 */
const char* get_card_type_name(int card_type) {
    switch (card_type) {
        case CARD_TYPE_RTL8812EU:
            return "rtl8812eu";
        case CARD_TYPE_RTL8812AU:
            return "rtl8812au";
        case CARD_TYPE_RTL8733BU:
            return "rtl8733bu";
        default:
            return "Unknown";
    }
}

/**
 * @brief Signal handler for various signals.
 * @param sig The signal number received.
 */
void signal_handler(int sig) {
    if (sig == SIGUSR1) {
        config.signal_received = SIGUSR1;
    } else if (sig == SIGUSR2) {
        config.signal_received = SIGUSR2;
    } else if (sig == SIGINT || sig == SIGTERM) {
        config.terminate = 1;
    }
}

/**
 * @brief Cleanup function called upon program exit.
 */
void cleanup() {
    if (config.tcp_server_fd != -1) {
        close_tcp_server(&config);
    }
}

/**
 * @brief Sets up the TCP server for manual mode.
 * @param config Pointer to the configuration structure.
 * @return 0 on success, -1 on failure.
 */
int setup_tcp_server(tx_ctrl_config_t *config) {
    struct sockaddr_in server_addr;
    int opt = 1;

    // Create socket file descriptor
    if ((config->tcp_server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket failed");
        config->tcp_server_fd = -1;
        return -1;
    }

    // Set socket options
    if (setsockopt(config->tcp_server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
        perror("setsockopt");
        close_tcp_server(config);
        return -1;
    }

    // Set socket to non-blocking mode
    if (fcntl(config->tcp_server_fd, F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl");
        close_tcp_server(config);
        return -1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(TCP_PORT);

    // Bind the socket to the port
    if (bind(config->tcp_server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind failed");
        close_tcp_server(config);
        return -1;
    }

    if (listen(config->tcp_server_fd, 1) == -1) {
        perror("listen");
        close_tcp_server(config);
        return -1;
    }

    return 0;
}

/**
 * @brief Closes the TCP server socket.
 * @param config Pointer to the configuration structure.
 */
void close_tcp_server(tx_ctrl_config_t *config) {
    if (config->tcp_server_fd != -1) {
        close(config->tcp_server_fd);
        config->tcp_server_fd = -1;
    }
}

/**
 * @brief The main loop of the program.
 * @param config Pointer to the configuration structure.
 */
void main_loop(tx_ctrl_config_t *config) {
    fd_set read_fds;
    int max_fd;
    struct timespec timeout;
    int select_ret;

    while (!config->terminate) {
        // Handle signals
        if (config->signal_received == SIGUSR1) {
            config->pid_control_enabled = 1;
            config->pid_paused_until = 0;
            config->manual_mode_enabled = 0;
            close_tcp_server(config);
            if (config->verbose) {
                printf("PID control enabled via SIGUSR1\n");
            }
            config->signal_received = 0;
        } else if (config->signal_received == SIGUSR2) {
            config->manual_mode_enabled = 1;
            config->pid_control_enabled = 0;
            config->pid_paused_until = 0;
            if (setup_tcp_server(config) == 0) {
                if (config->verbose) {
                    printf("Manual mode enabled via SIGUSR2. Listening on port %d.\n", TCP_PORT);
                }
            } else {
                fprintf(stderr, "Failed to set up TCP server. Exiting Manual mode.\n");
                config->manual_mode_enabled = 0;
            }
            config->signal_received = 0;
        }

        // Check if PID control needs to be resumed
        if (!config->pid_control_enabled && config->pid_paused_until > 0) {
            struct timespec current_time;
            clock_gettime(CLOCK_MONOTONIC, &current_time);
            if (current_time.tv_sec >= config->pid_paused_until) {
                config->pid_control_enabled = 1;
                config->pid_paused_until = 0;
                // Reset PID controller variables if needed
                config->integral = 0.0;
                config->previous_error = 0.0;
                if (config->verbose) {
                    printf("PID control resumed after recovery timeout.\n");
                }
            }
        }

        FD_ZERO(&read_fds);

        // Add stdin to the set
        FD_SET(STDIN_FILENO, &read_fds);
        max_fd = STDIN_FILENO;

        // If in manual mode and tcp_server_fd is valid, add it to the set
        if (config->manual_mode_enabled && config->tcp_server_fd != -1) {
            FD_SET(config->tcp_server_fd, &read_fds);
            if (config->tcp_server_fd > max_fd) {
                max_fd = config->tcp_server_fd;
            }
        }

        // Set timeout for select()
        timeout.tv_sec = 1;
        timeout.tv_nsec = 0;

        select_ret = pselect(max_fd + 1, &read_fds, NULL, NULL, &timeout, NULL);

        if (select_ret == -1) {
            if (errno != EINTR) {
                perror("select error");
                break;
            }
        } else if (select_ret > 0) {
            // Check if data is available on stdin
            if (FD_ISSET(STDIN_FILENO, &read_fds)) {
                // Read from stdin
                if (!process_stdin_input(config)) {
                    // EOF or error
                    break;
                }
            }

            // Check if data is available on TCP socket
            if (config->manual_mode_enabled && config->tcp_server_fd != -1 && FD_ISSET(config->tcp_server_fd, &read_fds)) {
                // Accept new connection and process command
                process_tcp_connection(config);
            }
        }

        // Perform periodic tasks here if needed
    }

    // After processing all input, print summary
    if (config->verbose) {
        printf("Summary Statistics:\n");
        printf("  Total Packets: %" PRIu64 "\n", config->total_packets);
        printf("  Lost Packets: %" PRIu64 "\n", config->lost_packets);
        printf("  Unrecoverable Packets: %" PRIu64 "\n", config->unrecoverable_packets);
    }
}

/**
 * @brief Processes input from stdin.
 * @param config Pointer to the configuration structure.
 * @return 1 to continue, 0 to stop.
 */
int process_stdin_input(tx_ctrl_config_t *config) {
    char line[MAX_LINE_LENGTH];

    if (fgets(line, sizeof(line), stdin) != NULL) {
        process_line(config, line);
        return 1;
    } else {
        if (feof(stdin)) {
            // EOF
            return 0;
        } else {
            perror("fgets");
            return 0;
        }
    }
}

/**
 * @brief Processes a single line of input.
 * @param config Pointer to the configuration structure.
 * @param line The line of input to process.
 */
void process_line(tx_ctrl_config_t *config, const char *line) {
    uint64_t timestamp;
    char type[16];
    char *line_copy = strdup(line);
    char *token = NULL;
    char *saveptr = NULL;

    if (!line_copy) {
        fprintf(stderr, "Memory allocation error.\n");
        return;
    }

    // Remove newline character
    line_copy[strcspn(line_copy, "\r\n")] = '\0';

    // Tokenize the line
    token = strtok_r(line_copy, "\t ", &saveptr);
    if (token == NULL) {
        free(line_copy);
        return;
    }

    // Parse timestamp
    char *endptr = NULL;
    timestamp = strtoull(token, &endptr, 10);
    if (*endptr != '\0') {
        if (config->verbose) {
            printf("Invalid timestamp in line: %s\n", line);
        }
        free(line_copy);
        return;
    }

    // Parse type
    token = strtok_r(NULL, "\t ", &saveptr);
    if (token == NULL) {
        free(line_copy);
        return;
    }
    strncpy(type, token, sizeof(type) - 1);
    type[sizeof(type) - 1] = '\0'; // Ensure null-termination

    // Handle different types
    if (strcmp(type, "RX_ANT") == 0) {
        // Handle RX_ANT line
        char *freq_info = strtok_r(NULL, "\t ", &saveptr);
        char *antenna_id_str = strtok_r(NULL, "\t ", &saveptr);
        char *stats = strtok_r(NULL, "\t ", &saveptr);

        if (freq_info && antenna_id_str && stats) {
            int antenna_id = (int)strtol(antenna_id_str, &endptr, 10);
            if (*endptr != '\0') {
                if (config->verbose) {
                    printf("Invalid antenna ID in line: %s\n", line);
                }
                free(line_copy);
                return;
            }

            int count_all, rssi_min, rssi_avg, rssi_max, snr_min, snr_avg, snr_max;
            if (sscanf(stats, "%d:%d:%d:%d:%d:%d:%d",
                       &count_all, &rssi_min, &rssi_avg, &rssi_max,
                       &snr_min, &snr_avg, &snr_max) == 7) {

                // Update Exponential Moving Average
                if (!config->ema_initialized) {
                    config->ema_rssi = rssi_avg;
                    config->ema_snr = snr_avg;
                    config->ema_initialized = 1;
                } else {
                    config->ema_rssi = config->ema_alpha * rssi_avg + (1 - config->ema_alpha) * config->ema_rssi;
                    config->ema_snr = config->ema_alpha * snr_avg + (1 - config->ema_alpha) * config->ema_snr;
                }

                if (config->verbose) {
                    printf("Timestamp: %" PRIu64 "\n", timestamp);
                    printf("Antenna ID: %d\n", antenna_id);
                    printf("EMA RSSI: %.2f dBm\n", config->ema_rssi);
                    printf("EMA SNR: %.2f dB\n", config->ema_snr);
                    printf("-------------------------\n");
                }

                // Hysteresis logic
                if (config->alink_enabled && config->pid_control_enabled) {
                    double current_value = (config->pid_control_type == PID_CONTROL_RSSI) ? config->ema_rssi : config->ema_snr;
                    update_hysteresis(config, current_value);
                }

                // Adjust TX power if PID control is enabled
                if (config->pid_control_enabled && config->ema_initialized && !config->manual_mode_enabled) {
                    double current_value = (config->pid_control_type == PID_CONTROL_RSSI) ? config->ema_rssi : config->ema_snr;
                    int tx_power = calculate_pid_output(config, current_value);

                    // Ensure tx_power is within min and max
                    if (tx_power < config->tx_power_min) {
                        tx_power = config->tx_power_min;
                    }
                    if (tx_power > config->tx_power_max) {
                        tx_power = config->tx_power_max;
                    }

                    // Adjust TX power
                    adjust_tx_power(config, tx_power);
                } else if (!config->pid_control_enabled && !config->manual_mode_enabled) {
                    // If PID control is disabled and not in manual mode, set TX power to max
                    adjust_tx_power(config, config->tx_power_max);
                }
            } else {
                if (config->verbose) {
                    fprintf(stderr, "Failed to parse RX_ANT stats in line: %s\n", line);
                }
            }
        } else {
            if (config->verbose) {
                fprintf(stderr, "Failed to parse RX_ANT line: %s\n", line);
            }
        }
    } else if (strcmp(type, "PKT") == 0) {
        // Handle PKT line
        char *pkt_stats = strtok_r(NULL, "\t ", &saveptr);
        if (pkt_stats) {
            unsigned int count_p_all, count_b_all, count_p_dec_err, count_p_dec_ok;
            unsigned int count_p_fec_recovered, count_p_lost, count_p_bad;
            unsigned int count_p_outgoing, count_b_outgoing;

            if (sscanf(pkt_stats, "%u:%u:%u:%u:%u:%u:%u:%u:%u",
                       &count_p_all, &count_b_all, &count_p_dec_err, &count_p_dec_ok,
                       &count_p_fec_recovered, &count_p_lost, &count_p_bad,
                       &count_p_outgoing, &count_b_outgoing) == 9) {

                config->total_packets += count_p_all;
                config->lost_packets += count_p_lost;
                config->unrecoverable_packets += count_p_bad;

                if (config->verbose) {
                    printf("Timestamp: %" PRIu64 "\n", timestamp);
                    printf("Packet Stats:\n");
                    printf("  Total Packets: %u\n", count_p_all);
                    printf("  FEC Recovered: %u\n", count_p_fec_recovered);
                    printf("  Packets Lost: %u\n", count_p_lost);
                    printf("  Unrecoverable Packets: %u\n", count_p_bad);
                    printf("-------------------------\n");
                }

                // Check for limits
                if ((count_p_fec_recovered > config->fec_limit || count_p_lost > config->lost_limit) && !config->manual_mode_enabled) {
                    // Set tx power to maximum and pause PID controller
                    adjust_tx_power(config, config->tx_power_max);
                    config->pid_control_enabled = 0;
                    struct timespec current_time;
                    clock_gettime(CLOCK_MONOTONIC, &current_time);
                    config->pid_paused_until = current_time.tv_sec + config->recover_timeout;
                    if (config->verbose) {
                        printf("High FEC recovered (%u) or lost packets (%u) detected. TX power set to max and PID control paused for %d seconds.\n",
                               count_p_fec_recovered, count_p_lost, config->recover_timeout);
                    }
                    // Call fallback script
                    if (config->alink_enabled) {
                        call_script(config, "/usr/bin/tx_fallback.sh", "");
                    }
                }
            } else {
                if (config->verbose) {
                    fprintf(stderr, "Failed to parse PKT stats in line: %s\n", line);
                }
            }
        } else {
            if (config->verbose) {
                fprintf(stderr, "Failed to parse PKT line: %s\n", line);
            }
        }
    } else {
        // Unknown or unhandled type
        if (config->verbose) {
            printf("Unknown line type '%s' in line: %s\n", type, line);
        }
    }

    free(line_copy);
}

/**
 * @brief Adjusts the TX power.
 * @param config Pointer to the configuration structure.
 * @param tx_power The TX power to set in mBm.
 * @return 0 on success, -1 on failure.
 */
int adjust_tx_power(tx_ctrl_config_t *config, int tx_power) {
    char *args[8];
    char tx_power_str[16];
    int ret;

    // Adjust tx_power sign for rtl8812au
    int adjusted_tx_power = tx_power;
    if (config->card_type == CARD_TYPE_RTL8812AU) {
        adjusted_tx_power = -tx_power;
    }

    snprintf(tx_power_str, sizeof(tx_power_str), "%d", adjusted_tx_power);

    args[0] = "iw";
    args[1] = "dev";
    args[2] = config->interface_name;
    args[3] = "set";
    args[4] = "txpower";
    args[5] = "fixed";
    args[6] = tx_power_str;
    args[7] = NULL;

    if (config->verbose) {
        printf("Adjusting TX power to %d mBm\n", tx_power);
    }

    ret = execute_command("/sbin/iw", args);

    if (ret != 0) {
        fprintf(stderr, "Failed to set TX power.\n");
        return -1;
    }

    config->current_tx_power = tx_power;
    return 0;
}

/**
 * @brief Executes a command without invoking the shell.
 * @param command The command to execute.
 * @param argv The argument vector.
 * @return The exit status of the command.
 */
int execute_command(const char *command, char *const argv[]) {
    pid_t pid;
    int status;

    pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    } else if (pid == 0) {
        // Child process
        execv(command, argv);
        // If execv returns, an error occurred
        perror("execv");
        _exit(EXIT_FAILURE);
    } else {
        // Parent process
        do {
            if (waitpid(pid, &status, 0) == -1) {
                if (errno != EINTR) {
                    perror("waitpid");
                    return -1;
                }
            } else {
                break;
            }
        } while (1);

        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        } else {
            return -1;
        }
    }
}

/**
 * @brief Calculates the PID controller output.
 * @param config Pointer to the configuration structure.
 * @param current_value The current RSSI or SNR value.
 * @return The new TX power value.
 */
int calculate_pid_output(tx_ctrl_config_t *config, double current_value) {
    double error = config->target_value - current_value;
    config->integral += error;
    double derivative = error - config->previous_error;
    config->previous_error = error;

    double output = config->Kp * error + config->Ki * config->integral + config->Kd * derivative;

    // Map the PID output to an adjustment in tx_power
    int tx_power_change = (int)(output);

    // Limit the tx_power_change to be within tx_power_adjust_min and tx_power_adjust_max
    if (tx_power_change > config->tx_power_adjust_max) {
        tx_power_change = config->tx_power_adjust_max;
    } else if (tx_power_change < -config->tx_power_adjust_max) {
        tx_power_change = -config->tx_power_adjust_max;
    }

    // Ensure the change is at least tx_power_adjust_min if not zero
    if (tx_power_change > 0 && tx_power_change < config->tx_power_adjust_min) {
        tx_power_change = config->tx_power_adjust_min;
    } else if (tx_power_change < 0 && tx_power_change > -config->tx_power_adjust_min) {
        tx_power_change = -config->tx_power_adjust_min;
    }

    // Calculate the new tx_power by adding the change, ensure within tx_power_min and tx_power_max
    int tx_power = config->current_tx_power + tx_power_change;

    if (tx_power > config->tx_power_max) tx_power = config->tx_power_max;
    if (tx_power < config->tx_power_min) tx_power = config->tx_power_min;

    // Round tx_power to nearest 100 mBm
    tx_power = ROUND_UP_TO_NEAREST_100(tx_power);

    return tx_power;
}

/**
 * @brief Updates the hysteresis state and executes scripts if necessary.
 * @param config Pointer to the configuration structure.
 * @param current_value The current RSSI or SNR value.
 */
void update_hysteresis(tx_ctrl_config_t *config, double current_value) {
    hysteresis_state_t previous_state = config->hysteresis_state;

    if (current_value > config->hysteresis_value) {
        config->hysteresis_state = HYSTERESIS_STATE_HIGH;
    } else if (current_value <= config->hysteresis_value && current_value >= config->deadband_lower) {
        config->hysteresis_state = HYSTERESIS_STATE_DEADBAND;
    } else {
        config->hysteresis_state = HYSTERESIS_STATE_LOW;
    }

    if (previous_state != config->hysteresis_state) {
        if (config->hysteresis_state == HYSTERESIS_STATE_HIGH && previous_state <= HYSTERESIS_STATE_DEADBAND) {
            // Moving from low to high signal past hysteresis point
            call_script(config, "/usr/bin/tx_high_signal.sh", "up");
        } else if (config->hysteresis_state == HYSTERESIS_STATE_LOW && previous_state >= HYSTERESIS_STATE_DEADBAND) {
            // Moving from high to low signal past hysteresis point
            call_script(config, "/usr/bin/tx_low_signal.sh", "down");
        }
    }
}

/**
 * @brief Calls an external script with the given argument.
 * @param config Pointer to the configuration structure.
 * @param script_name The script to execute.
 * @param argument The argument to pass to the script.
 */
void call_script(tx_ctrl_config_t *config, const char *script_name, const char *argument) {
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);

    // Enforce timeout between script calls
    if (current_time.tv_sec - config->last_script_call_time >= config->recover_timeout) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            return;
        } else if (pid == 0) {
            // Child process
            execl(script_name, script_name, argument, (char *)NULL);
            // If execl returns, an error occurred
            perror("execl");
            _exit(EXIT_FAILURE);
        } else {
            // Parent process
            int status;
            waitpid(pid, &status, 0);
            config->last_script_call_time = current_time.tv_sec;
            if (config->verbose) {
                printf("Executed script: %s %s\n", script_name, argument);
            }
        }
    } else {
        if (config->verbose) {
            printf("Script call suppressed due to timeout (%d seconds).\n", config->recover_timeout);
        }
    }
}

/**
 * @brief Initializes hysteresis parameters.
 * @param config Pointer to the configuration structure.
 */
void initialize_hysteresis(tx_ctrl_config_t *config) {
    if (config->pid_control_type == PID_CONTROL_RSSI) {
        config->hysteresis_value = config->target_value - HYSTERESIS_OFFSET_RSSI;
        config->deadband_lower = config->hysteresis_value - DEAD_BAND_HALF_RSSI;
    } else {
        config->hysteresis_value = config->target_value - HYSTERESIS_OFFSET_SNR;
        config->deadband_lower = config->hysteresis_value - DEAD_BAND_HALF_SNR;
    }
    config->hysteresis_state = HYSTERESIS_STATE_DEADBAND;
    config->last_script_call_time = 0;
    if (config->verbose) {
        printf("Hysteresis initialized. Hysteresis value: %.2f, Deadband lower limit: %.2f\n",
               config->hysteresis_value, config->deadband_lower);
    }
}

/**
 * @brief Initializes the TX power to minimum at startup.
 * @param config Pointer to the configuration structure.
 */
void initialize_tx_power(tx_ctrl_config_t *config) {
    config->current_tx_power = config->tx_power_min;
    adjust_tx_power(config, config->current_tx_power);
    if (config->verbose) {
        printf("Initial TX power set to MIN: %d mBm\n", config->current_tx_power);
    }
}

/**
 * @brief Prints the current settings.
 * @param config Pointer to the configuration structure.
 */
void print_current_settings(tx_ctrl_config_t *config) {
    printf("Current Settings:\n");
    printf("  Verbose mode: %s\n", config->verbose ? "Enabled" : "Disabled");
    printf("  Interface name: %s\n", config->interface_name);
    printf("  Card type: %s\n", get_card_type_name(config->card_type));
    printf("  TX Power Min: %d mBm\n", config->tx_power_min);
    printf("  TX Power Max: %d mBm\n", config->tx_power_max);
    printf("  Target Value: %d %s\n", config->target_value,
           config->pid_control_type == PID_CONTROL_RSSI ? "dBm (RSSI)" : "dB (SNR)");
    printf("  PID Control using: %s\n", config->pid_control_type == PID_CONTROL_RSSI ? "RSSI" : "SNR");
    printf("  PID Control enabled: %s\n", config->pid_control_enabled ? "Yes" : "No");
    printf("  FEC Limit: %u\n", config->fec_limit);
    printf("  Lost Limit: %u\n", config->lost_limit);
    printf("  Recover Timeout: %d seconds\n", config->recover_timeout);
    printf("  A-Link Enabled: %s\n", config->alink_enabled ? "Yes" : "No");
    if (config->alink_enabled) {
        printf("  Hysteresis Value: %.2f\n", config->hysteresis_value);
        printf("  Deadband Lower Limit: %.2f\n", config->deadband_lower);
    }
    printf("-------------------------\n");
}

/**
 * @brief Processes incoming TCP connections in manual mode.
 * @param config Pointer to the configuration structure.
 */
void process_tcp_connection(tx_ctrl_config_t *config) {
    int new_socket;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    char buffer[MAX_BUFFER_SIZE] = {0};
    int valread;

    // Accept the connection
    new_socket = accept(config->tcp_server_fd, (struct sockaddr *)&address, &addrlen);
    if (new_socket == -1) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            perror("accept");
        }
        return;
    }

    // Read the command
    valread = recv(new_socket, buffer, sizeof(buffer) - 1, 0);
    if (valread == -1) {
        perror("recv");
        close(new_socket);
        return;
    }
    buffer[valread] = '\0'; // Null-terminate

    // Process the command
    char response[256];
    if (strncmp(buffer, "set_tx ", 7) == 0) {
        char *percent_str = buffer + 7;
        int percent = (int)strtol(percent_str, NULL, 10);
        if (percent < 1 || percent > 100) {
            snprintf(response, sizeof(response), "Error: Invalid percentage. Must be between 1 and 100.\n");
        } else {
            // Calculate TX power
            int tx_power_range = config->tx_power_max - config->tx_power_min;
            int tx_power = config->tx_power_min + (tx_power_range * (percent - 1)) / 99;
            // Round to nearest 100
            tx_power = ROUND_UP_TO_NEAREST_100(tx_power);
            // Ensure tx_power is within min and max
            if (tx_power < config->tx_power_min) tx_power = config->tx_power_min;
            if (tx_power > config->tx_power_max) tx_power = config->tx_power_max;
            // Adjust TX power
            if (adjust_tx_power(config, tx_power) == 0) {
                snprintf(response, sizeof(response), "TX power set to %d%% (%d mBm)\n", percent, tx_power);
            } else {
                snprintf(response, sizeof(response), "Error: Failed to set TX power.\n");
            }
        }
    } else if (strcmp(buffer, "set mode pid\n") == 0 || strcmp(buffer, "set mode pid") == 0) {
        // Switch back to PID control
        config->manual_mode_enabled = 0;
        config->pid_control_enabled = 1;
        close_tcp_server(config);
        snprintf(response, sizeof(response), "Switched to PID mode.\n");
        if (config->verbose) {
            printf("Switched to PID mode via command.\n");
        }
    } else {
        snprintf(response, sizeof(response), "Error: Unknown command.\n");
    }

    // Send response
    send(new_socket, response, strlen(response), 0);

    // Close the connection
    close(new_socket);
}
