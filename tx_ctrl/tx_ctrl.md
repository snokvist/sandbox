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

--verbose
Enables verbose output, providing detailed information during execution.

bash
Kopiera kod
$ ./tx_ctrl --verbose [other options]
--wlanid=ID (Required)
Specifies the network interface name to control. This argument is mandatory.

bash
Kopiera kod
$ ./tx_ctrl --wlanid=wlan0 [other options]
--card-type=NAME
Specifies the WiFi card type. Supported options are:

rtl8812eu
rtl8812au
rtl8733bu
Example:

bash
Kopiera kod
$ ./tx_ctrl --wlanid=wlan0 --card-type=rtl8812eu
--tx-min=VALUE
Overrides the minimum TX power (in mBm). Values must be between 100 and 3000 and are rounded up to the nearest 100 mBm.

bash
Kopiera kod
$ ./tx_ctrl --wlanid=wlan0 --tx-min=200
--tx-max=VALUE
Overrides the maximum TX power (in mBm). Values must be between 100 and 3000 and are rounded up to the nearest 100 mBm.

bash
Kopiera kod
$ ./tx_ctrl --wlanid=wlan0 --tx-max=2800
--target-value=VAL
Sets the target RSSI or SNR value for the PID controller.

For RSSI (default), values are in dBm (e.g., -70).
For SNR, values are in dB (e.g., 20).
Example:

bash
Kopiera kod
$ ./tx_ctrl --wlanid=wlan0 --target-value=-75
--pid-control=TYPE
Specifies whether to use rssi or snr for the PID controller. Default is rssi.

bash
Kopiera kod
$ ./tx_ctrl --wlanid=wlan0 --pid-control=snr
--fec-limit=VALUE
Sets the FEC (Forward Error Correction) recovered packets limit. When exceeded, the program takes corrective action.

Default: 50
Range: 1 to 100
bash
Kopiera kod
$ ./tx_ctrl --wlanid=wlan0 --fec-limit=60
--lost-limit=VALUE
Sets the lost packets limit. When exceeded, the program takes corrective action.

Default: 5
Range: 1 to 100
bash
Kopiera kod
$ ./tx_ctrl --wlanid=wlan0 --lost-limit=10
--recover-timeout=SEC
Sets the recovery timeout in seconds. This defines how long the PID controller remains paused after taking corrective action.

Default: 10
bash
Kopiera kod
$ ./tx_ctrl --wlanid=wlan0 --recover-timeout=15
--alink
Enables the hysteresis logic and script execution feature.

bash
Kopiera kod
$ ./tx_ctrl --wlanid=wlan0 --alink
Hysteresis Logic and Script Execution
When the --alink option is enabled, the program monitors signal levels and executes external scripts when certain thresholds are crossed.

Hysteresis Calculation
For RSSI:
Hysteresis value: target_value - 12
Deadband lower limit: hysteresis_value - 6
For SNR:
Hysteresis value: target_value - 6
Deadband lower limit: hysteresis_value - 3
Script Execution
Scripts are executed when the signal crosses hysteresis points:

Low to High Signal Transition:

Script: /usr/bin/tx_high_signal.sh
Argument: up
High to Low Signal Transition:

Script: /usr/bin/tx_low_signal.sh
Argument: down
Fallback Action (when FEC or lost packet limits are exceeded):

Script: /usr/bin/tx_fallback.sh
Example of Script Invocation:

bash
Kopiera kod
/usr/bin/tx_high_signal.sh up
Timeout Enforcement
To prevent excessive script execution, scripts are not called more frequently than the recover_timeout value.

Manual Mode
The program can be switched to manual mode using the SIGUSR2 signal. In manual mode, you can control the TX power via a TCP socket.

Activating Manual Mode
Send the SIGUSR2 signal to the process:

bash
Kopiera kod
$ kill -USR2 <pid>
Controlling TX Power
Connect to the TCP server on localhost port 9995 (can use telnet or nc):

bash
Kopiera kod
$ telnet localhost 9995
Commands:

set_tx X% : Sets the TX power to X% of the maximum value.
Example: set_tx 50%
set mode pid : Exits manual mode and resumes PID control.
Exiting Manual Mode
Send the SIGUSR1 signal to the process:

bash
Kopiera kod
$ kill -USR1 <pid>
Signal Handling
The program responds to the following signals:

SIGUSR1: Enables PID control and exits manual mode.
SIGUSR2: Enters manual mode.
SIGINT / SIGTERM: Initiates graceful shutdown.
Input Processing
The program reads input from standard input, which should be lines containing signal metrics and packet statistics.

Expected Input Format
RX_ANT Lines: Contain RSSI and SNR values.

Example:

ruby
Kopiera kod
15594044 RX_ANT 5805:2:20 1 5:-39:-38:-38:29:33:37
PKT Lines: Contain packet statistics.

Example:

Kopiera kod
15594044 PKT 5:200:0:5:0:0:0:2:0
Error Handling and Logging
Verbose Mode: When enabled, provides detailed logging of operations and any errors encountered.
Input Validation: Checks all inputs and command-line arguments for validity.
Resource Cleanup: Ensures that all resources are properly released upon termination.
Example Usage
bash
Kopiera kod
$ ./tx_ctrl --wlanid=wlan0 --card-type=rtl8812eu --tx-min=100 --tx-max=2800 \
            --target-value=-70 --pid-control=rssi --fec-limit=50 --lost-limit=5 \
            --recover-timeout=10 --alink --verbose
