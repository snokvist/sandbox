tx_ctrl - TX Power Control Program
tx_ctrl is a C program designed to dynamically adjust the transmission (TX) power of a wireless interface based on real-time RSSI (Received Signal Strength Indicator) or SNR (Signal-to-Noise Ratio) values. It incorporates a PID (Proportional-Integral-Derivative) controller for fine-tuned adjustments and includes hysteresis logic (A-Link) for managing state transitions and executing external scripts during significant link changes.

Table of Contents
Features
Requirements
Compilation and Installation
Usage
Command-Line Options
Examples
Functionality Overview
PID Controller
Hysteresis Logic (A-Link)
Fallback Mechanism
External Scripts
Signals and Manual Mode
Testing and Tuning
License
Contributing
Features
Dynamic TX Power Adjustment: Adjusts the TX power based on real-time RSSI or SNR values.
PID Control: Utilizes a PID controller for smooth and stable TX power adjustments.
Hysteresis Logic (A-Link): Implements hysteresis to prevent frequent toggling between states and executes external scripts during significant link changes.
Fallback Mechanism: Detects high packet loss or FEC errors and increases TX power to maximum for recovery.
Manual Mode: Allows manual control of TX power via TCP commands.
Configurable Parameters: Offers extensive command-line options to customize behavior.
Requirements
Operating System: Linux-based systems.
Dependencies: None (standard C libraries and system calls).
Permissions: Requires root privileges to adjust TX power settings via iw.
Compilation and Installation
Clone the Repository:

bash
Kopiera kod
git clone https://github.com/yourusername/tx_ctrl.git
cd tx_ctrl
Compile the Program:

bash
Kopiera kod
gcc -Wall -Wextra tx_ctrl.c -o tx_ctrl
The -Wall and -Wextra flags enable all compiler warnings, which is good practice.
Set Executable Permissions (if needed):

bash
Kopiera kod
chmod +x tx_ctrl
Install (optional):

You can move the executable to a directory in your PATH, such as /usr/local/bin.

bash
Kopiera kod
sudo mv tx_ctrl /usr/local/bin/
Usage
The tx_ctrl program reads input lines from stdin and adjusts the TX power of a specified wireless interface accordingly.

Command-Line Options
plaintext
Kopiera kod
Usage: tx_ctrl [OPTIONS]

Options:
  --help                 Show this help message and exit
  --verbose              Enable verbose output
  --wlanid=ID            Specify the network interface name (required)
  --card-type=NAME       Specify WiFi card type:
                           'rtl8812eu'
                           'rtl8812au'
                           'rtl8733bu'
                         (default: rtl8812eu)
  --tx-min=VALUE         Override minimum TX power (in mBm)
  --tx-max=VALUE         Override maximum TX power (in mBm)
                         Values must be between 100 and 3000, rounded up to nearest 100.
  --target-value=VAL     Set target RSSI/SNR value (default: -70 for RSSI, 20 for SNR)
  --pid-control=TYPE     Use 'rssi' or 'snr' for PID controller (default: rssi)
  --fec-limit=VALUE      Set FEC recovered packets limit (default: 50)
  --lost-limit=VALUE     Set lost packets limit (default: 5)
                         Values must be between 1 and 100.
  --recover-timeout=SEC  Set recovery timeout in seconds (default: 10)
  --alink                Enable hysteresis logic and script execution
  --set-delay=MS         Minimum delay between TX power adjustments in milliseconds (default: 2000)
  --rx-ant-timeout=SEC   Set RX_ANT timeout duration in seconds (default: 5)
  --kp=VALUE             Set proportional gain Kp for PID controller (default: 50.0)
  --ki=VALUE             Set integral gain Ki for PID controller (default: 0.0)
  --kd=VALUE             Set derivative gain Kd for PID controller (default: 0.0)
  --error-threshold=VAL  Set error threshold for adjustments (default: 1.0)
Examples
Basic Usage:

bash
Kopiera kod
sudo ./tx_ctrl --wlanid=wlan0
Adjusts TX power of wlan0 using default settings.
Enable Verbose Output:

bash
Kopiera kod
sudo ./tx_ctrl --wlanid=wlan0 --verbose
Specify Card Type and TX Power Limits:

bash
Kopiera kod
sudo ./tx_ctrl --wlanid=wlan0 --card-type=rtl8812au --tx-min=100 --tx-max=2000
Set Target RSSI and Use SNR for PID Control:

bash
Kopiera kod
sudo ./tx_ctrl --wlanid=wlan0 --pid-control=snr --target-value=25
Enable A-Link Hysteresis Logic:

bash
Kopiera kod
sudo ./tx_ctrl --wlanid=wlan0 --alink
Customize PID Parameters:

bash
Kopiera kod
sudo ./tx_ctrl --wlanid=wlan0 --kp=60.0 --ki=0.5 --kd=0.1 --error-threshold=0.5
Set Minimum Delay Between Adjustments:

bash
Kopiera kod
sudo ./tx_ctrl --wlanid=wlan0 --set-delay=1000
Set RX_ANT Timeout Duration:

bash
Kopiera kod
sudo ./tx_ctrl --wlanid=wlan0 --rx-ant-timeout=10
Functionality Overview
PID Controller
Purpose: Adjusts TX power to maintain the target RSSI or SNR value.
Parameters:
Kp: Proportional gain. Increases responsiveness.
Ki: Integral gain. Eliminates steady-state error.
Kd: Derivative gain. Reduces overshoot and oscillations.
Error Threshold: Minimum error required to trigger an adjustment.
Hysteresis Logic (A-Link)
Purpose: Prevents frequent toggling between high and low signal states.
States:
High Signal: Signal strength above the hysteresis threshold.
Deadband: Signal strength within a neutral range.
Low Signal: Signal strength below the deadband lower limit.
Scripts: Executes external scripts during transitions.
tx_high_signal.sh: Called when entering high signal state.
tx_low_signal.sh: Called when entering low signal state.
tx_fallback.sh: Called when entering fallback state.
Fallback Mechanism
Trigger Conditions:
High number of FEC recovered packets.
High number of lost packets.
RX_ANT timeout exceeded.
Actions:
Sets TX power to maximum.
Pauses PID control for a specified recovery timeout.
Executes tx_fallback.sh script.
Recovery:
Monitors link status during suppression period.
Transitions out of fallback state when link recovers.
Calls appropriate scripts with "up" argument upon recovery.
External Scripts
The program calls external scripts to handle significant link state transitions.

Script Execution:
Scripts are executed using execlp.
Ensure scripts are executable and located in a directory in the system PATH.
Scripts:
tx_high_signal.sh up: Called when transitioning to a higher signal state.
tx_low_signal.sh down: Called when transitioning to a lower signal state.
tx_low_signal.sh up: Called when recovering from fallback to a low signal state.
tx_fallback.sh: Called when entering the fallback state.
Script Examples
tx_high_signal.sh:

bash
Kopiera kod
#!/bin/bash
echo "High signal detected. Action: $1"
# Add your custom actions here
tx_low_signal.sh:

bash
Kopiera kod
#!/bin/bash
echo "Low signal detected. Action: $1"
# Add your custom actions here
tx_fallback.sh:

bash
Kopiera kod
#!/bin/bash
echo "Fallback state triggered."
# Add your custom actions here
Make Scripts Executable:

bash
Kopiera kod
chmod +x tx_high_signal.sh tx_low_signal.sh tx_fallback.sh
Signals and Manual Mode
Supported Signals
SIGUSR1: Switches to PID control mode.
SIGUSR2: Switches to manual mode.
Manual Mode
Purpose: Allows manual control of TX power via TCP commands.
Usage:
Send SIGUSR2 to the process to enable manual mode.
The program listens on TCP port 9995 for commands.
Commands:
set_tx <percentage>: Sets TX power to the specified percentage between tx-min and tx-max.
set mode pid: Switches back to PID control mode.
Example: Enabling Manual Mode
Send SIGUSR2 to Process:

bash
Kopiera kod
kill -SIGUSR2 <pid_of_tx_ctrl>
Send Command via TCP:

bash
Kopiera kod
echo "set_tx 75" | nc localhost 9995
Switch Back to PID Mode:

bash
Kopiera kod
echo "set mode pid" | nc localhost 9995
Testing and Tuning
Run the Program with Desired Parameters:

bash
Kopiera kod
sudo ./tx_ctrl --wlanid=wlan0 --verbose --kp=60.0 --ki=0.5 --kd=0.1 --error-threshold=0.5 --alink
Monitor the Output:

Verbose mode provides detailed information about adjustments and state transitions.
Adjust PID Parameters:

Increase Kp: More aggressive response to error.
Increase Ki: Eliminates steady-state errors over time.
Increase Kd: Reduces overshoot and dampens oscillations.
Adjust Hysteresis Parameters:

Modify HYSTERESIS_OFFSET_RSSI, HYSTERESIS_OFFSET_SNR, DEAD_BAND_HALF_RSSI, and DEAD_BAND_HALF_SNR in the source code as needed.
Test Fallback Mechanism:

Simulate high packet loss or FEC errors.
Verify that the program enters fallback state and executes tx_fallback.sh.
Ensure that upon recovery, the appropriate script is called with "up" argument.
Monitor Script Execution:

Check that scripts are called at the correct times with correct arguments.
Use logging within scripts for verification.
License
MIT License

Contributing
Contributions are welcome! Please open an issue or submit a pull request on GitHub.

Note: Ensure that you have the necessary permissions and that you comply with all applicable laws and regulations when adjusting wireless transmission power and handling network interfaces.
