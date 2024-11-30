# `tx_ctrl.c` Program Overview

The `tx_ctrl.c` program is a tool designed to dynamically adjust the transmission (TX) power of a wireless network interface based on real-time signal metrics such as RSSI (Received Signal Strength Indicator) or SNR (Signal-to-Noise Ratio). It uses a PID (Proportional-Integral-Derivative) controller to maintain a target signal level and can execute external scripts when certain signal thresholds are crossed.

## Features

- **Dynamic TX Power Adjustment**: Automatically adjusts the TX power to maintain a target RSSI or SNR value.
- **PID Controller**: Uses a PID control algorithm to calculate the necessary TX power adjustments.
- **Hysteresis Logic**: Implements hysteresis to prevent rapid toggling when signal levels fluctuate around thresholds.
- **Script Execution**: Executes external scripts when signal levels cross predefined hysteresis points.
- **Manual Mode**: Allows manual control of TX power via a TCP socket interface.
- **Signal Handling**: Responds to signals (`SIGUSR1`, `SIGUSR2`, `SIGINT`, `SIGTERM`) to control program behavior.
- **Robust Error Handling**: Includes input validation and error checking for reliable operation.
- **Resource Management**: Properly manages system resources, ensuring cleanup upon termination.

## Program Behavior

### Initialization

Upon startup, the program:

1. **Parses Command-Line Arguments**: Reads and validates the provided options.
2. **Sets Up Signal Handlers**: Registers handlers for `SIGUSR1`, `SIGUSR2`, `SIGINT`, and `SIGTERM`.
3. **Initializes Variables**: Sets default values for parameters like TX power limits, PID constants, and hysteresis thresholds.
4. **Starts Main Loop**: Enters the main processing loop to read input and adjust TX power accordingly.

### Main Processing Loop

The program continuously performs the following:

- **Reads Input**: Processes lines from standard input, which should contain signal metrics and packet statistics.
- **Updates Signal Metrics**: Calculates exponential moving averages (EMA) for RSSI and SNR.
- **Adjusts TX Power**: If PID control is enabled, calculates the new TX power based on the PID algorithm.
- **Executes Hysteresis Logic**: Checks if the signal has crossed hysteresis thresholds and executes scripts if necessary.
- **Handles Manual Mode**: If manual mode is activated, listens for commands on a TCP socket to adjust TX power directly.
- **Responds to Signals**: Adjusts behavior based on received signals (e.g., switching modes, terminating).

## Command-Line Arguments

The program accepts several command-line arguments to configure its behavior:

### `--help`

Displays the help message and exits.

```bash
$ ./tx_ctrl --help
