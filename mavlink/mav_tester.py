#!/usr/bin/env python

from pymavlink import mavutil

def main():
    # Create a connection that listens on UDP port 14550 on all network interfaces.
    # The format 'udpin:0.0.0.0:14550' tells pymavlink to open a UDP port.
    master = mavutil.mavlink_connection('udpin:0.0.0.0:14550')
    print("Listening for MAVLink messages on UDP port 14550...")

    while True:
        # Wait for a new MAVLink message (blocking call)
        msg = master.recv_match(blocking=True)
        if msg is not None:
            # Print the message in a human-readable format
            print(msg)

if __name__ == "__main__":
    main()
