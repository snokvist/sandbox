#!/usr/bin/python3
import time
import socket
from pymavlink import mavutil

# Create a MAVLink connection (UDP output)
udp_ip = "127.0.0.1"  # Replace with the target IP address
udp_port = 14550      # Replace with the target UDP port
mav = mavutil.mavlink_connection(f"udpout:{udp_ip}:{udp_port}")

# Create a MAVLink ATTITUDE message
def send_attitude_message():
    # Example attitude data (replace with your actual data)
    time_boot_ms = int(time.time() * 1000) % 4294967296  # Ensure it fits in 32 bits
    roll = 0.1  # Roll angle in radians
    pitch = 0.2  # Pitch angle in radians
    yaw = 1.5  # Yaw angle in radians
    rollspeed = 0.01  # Roll angular speed in radians/sec
    pitchspeed = 0.02  # Pitch angular speed in radians/sec
    yawspeed = 0.03  # Yaw angular speed in radians/sec

    # Pack the MAVLink ATTITUDE message
    mav.mav.attitude_send(
        time_boot_ms,  # Time since system boot in milliseconds (32-bit unsigned)
        roll,          # Roll angle in radians
        pitch,         # Pitch angle in radians
        yaw,           # Yaw angle in radians
        rollspeed,     # Roll angular speed in radians/sec
        pitchspeed,    # Pitch angular speed in radians/sec
        yawspeed       # Yaw angular speed in radians/sec
    )

# Send the message periodically
try:
    while True:
        send_attitude_message()
        print("Sent MAVLINK_MSG_ID_ATTITUDE message")
        time.sleep(1)  # Send every 1 second
except KeyboardInterrupt:
    print("Program terminated")
