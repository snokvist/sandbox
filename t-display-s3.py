import time
import wifi
import socketpool
import json
import supervisor
import board
import digitalio

# Configuration
SSID = "openipc"  # Open Wi-Fi network without a password
HOST = "192.168.1.49"  # Updated server address
PORT = 8103
TIMEOUT = 10  # Timeout in seconds for data reception

# GPIO14 Button setup
restart_button = digitalio.DigitalInOut(board.IO14)  # GPIO14 (User button for restart)
restart_button.direction = digitalio.Direction.INPUT
restart_button.pull = digitalio.Pull.UP  # Assuming button connects GPIO14 to GND when pressed

# State tracking for buttons
restart_button_was_pressed = False

# Function to parse the ant field into IP, WLAN ID, and antenna index
def parse_ant_field(ant_value):
    ip_part = (ant_value >> 32) & 0xFFFFFFFF
    wlan_idx = (ant_value >> 8) & 0xFFFFFF
    antenna_idx = ant_value & 0xFF
    ip_address = ".".join(str((ip_part >> (8 * i)) & 0xFF) for i in reversed(range(4)))
    return ip_address, wlan_idx, antenna_idx

# Function to clear and refresh the terminal UI
def refresh_ui(antenna_data, lost, fec_rec):
    print("\033[2J\033[H", end="")  # Clear screen and reset cursor
    print("CircuitPython Terminal Interface")
    print("=" * 40)
    for ip_short, data in antenna_data.items():
        rssi, pkt_recv, out_ratio = data
        print(f"IP {ip_short}: RSSI={rssi} dBm, Recv={pkt_recv}, Eff={out_ratio}%")
    print(f"Lost={lost}, FEC={fec_rec}")
    print("=" * 40)

# Function to check for GPIO14 button press
def check_restart_button():
    global restart_button_was_pressed
    if not restart_button.value:  # Button is currently pressed (active-low)
        if not restart_button_was_pressed:  # Detect transition from not pressed to pressed
            restart_button_was_pressed = True
            print("\nGPIO14 button pressed. Rebooting program...")
            time.sleep(0.5)  # Debounce delay
            supervisor.reload()
    else:
        restart_button_was_pressed = False  # Reset state when button is released

# Function to connect to Wi-Fi
def connect_to_wifi():
    while True:
        try:
            print("Connecting to Wi-Fi...")
            wifi.radio.connect(SSID)  # No password needed for open networks
            print(f"Connected to Wi-Fi! IP address: {wifi.radio.ipv4_address}")
            return "Connected"
        except Exception as e:
            print(f"Wi-Fi connection failed: {e}")
            print("Retrying Wi-Fi connection in 2 seconds...")
            time.sleep(2)

# Function to connect to the data server
def connect_to_server():
    while True:
        try:
            print(f"Connecting to server at {HOST}:{PORT}...")
            pool = socketpool.SocketPool(wifi.radio)
            sock = pool.socket(pool.AF_INET, pool.SOCK_STREAM)
            sock.connect((HOST, PORT))
            print("Connected to server!")
            return sock
        except Exception as e:
            print(f"Server connection failed: {e}")
            print("Retrying server connection in 2 seconds...")
            time.sleep(2)

# Main loop
print("Starting program...")
wifi_status = connect_to_wifi()

antenna_data = {}
last_update = time.monotonic()
last_received_time = time.monotonic()
while True:
    # Check for GPIO14 button press
    check_restart_button()

    try:
        # Connect to the server
        sock = connect_to_server()
        buffer = bytearray(2048)
        leftover = b""

        while True:
            # Check for GPIO14 button press
            check_restart_button()

            # Detect timeout for no data
            if time.monotonic() - last_received_time > TIMEOUT:
                print("No data received for 10 seconds. Reconnecting...")
                break  # Exit loop to reconnect

            # Read data from the server
            try:
                bytes_received = sock.recv_into(buffer)
                if bytes_received == 0:
                    print("Server closed the connection.")
                    break  # Exit the loop to reconnect

                last_received_time = time.monotonic()  # Update last received timestamp
                data = leftover + buffer[:bytes_received]
                json_messages = data.split(b"\n")
                leftover = json_messages.pop()  # Keep last partial message for the next loop

                for message in json_messages:
                    try:
                        json_data = json.loads(message.decode("utf-8"))

                        if json_data.get("type") == "rx" and json_data.get("id") == "video rx":
                            # Extract fields from the current JSON object
                            rx_ant_stats = json_data.get("rx_ant_stats", [])
                            packets = json_data.get("packets", {})
                            lost = packets.get("lost", [0])[0]
                            fec_rec = packets.get("fec_rec", [0])[0]
                            out = packets.get("out", [1])[0]  # Default to 1 to avoid division by zero

                            # Process antenna stats
                            unique_ips = {}
                            for ant in rx_ant_stats:
                                ant_value = ant.get("ant", 0)
                                ip_address, _, _ = parse_ant_field(ant_value)
                                ip_short = ".".join(ip_address.split(".")[-2:])
                                rssi_avg = ant.get("rssi_avg", "N/A")
                                pkt_recv = ant.get("pkt_recv", 0)

                                # Calculate efficiency
                                out_ratio = round((pkt_recv / out) * 100)

                                # Store the first antenna for each unique IP
                                if ip_short not in unique_ips:
                                    unique_ips[ip_short] = (rssi_avg, pkt_recv, out_ratio)

                            # Refresh UI only if 1 second has passed since the last update
                            if time.monotonic() - last_update >= 1:
                                refresh_ui(unique_ips, lost, fec_rec)
                                last_update = time.monotonic()

                    except json.JSONDecodeError:
                        print("JSON parse error: Invalid data received.")

            except OSError as e:
                print(f"Data receive error: {e}")
                break  # Reconnect on error

    except Exception as e:
        print(f"Error: {e}")
