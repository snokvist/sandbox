#!/usr/bin/env python

import argparse
import time
import subprocess
from pymavlink import mavutil

def parse_channels(channels_str):
    """Parse a comma separated list of channel numbers into a list of ints."""
    return [int(ch.strip()) for ch in channels_str.split(',') if ch.strip()]

def main():
    parser = argparse.ArgumentParser(
        description="Replicate MAVLink UDP stream and execute commands based on RC channel values."
    )
    parser.add_argument('-o', '--out', type=int, default=14550,
                        help="Remote output port (default: 14550)")
    parser.add_argument('-i', '--in', dest='in_port', type=int, default=14600,
                        help="Remote input port (default: 14600)")
    parser.add_argument('-c', '--channels', type=str, default=None,
                        help=("RC channels to monitor (comma separated list, e.g., '5,6,7'). "
                              "If not provided, the RC channel logic is skipped."))
    parser.add_argument('-w', '--wait', type=int, default=2000,
                        help="Delay in milliseconds after each command execution (default: 2000)")
    parser.add_argument('-p', '--persist', type=int, default=0,
                        help=("Time in milliseconds that a channel value must persist to generate a command "
                              "(default: 0)."))
    parser.add_argument('--tolerance', type=float, default=3.0,
                        help="Tolerance percentage for value stability (default: 3.0%%)")
    parser.add_argument('-v', '--verbose', action='store_true',
                        help="Display each received packet (default: off)")
    args = parser.parse_args()

    # Convert wait and persist times from milliseconds to seconds
    wait_seconds = args.wait / 1000.0
    persist_seconds = args.persist / 1000.0
    tolerance_ratio = args.tolerance / 100.0

    # Parse channels if provided; if omitted, no channel logic is run.
    if args.channels is not None:
        channels_list = parse_channels(args.channels)
    else:
        channels_list = []

    # Dictionary to maintain state per channel.
    # For each channel, we keep:
    #  - "candidate": the value currently being monitored (updated when a new value is detected)
    #  - "stable_since": timestamp when the candidate was first observed
    #  - "last_triggered_value": the last value for which channels.sh was executed
    #  - "last_executed": timestamp of the last command execution
    channel_state = {}

    # Set up MAVLink connections:
    # - in_conn listens on UDP port provided by --in (default 14600)
    # - out_conn sends messages to UDP port provided by --out (default 14550)
    in_conn_str = f'udpin:0.0.0.0:{args.in_port}'
    out_conn_str = f'udpout:127.0.0.1:{args.out}'
    print(f"Listening for MAVLink messages on UDP port {args.in_port} and replicating to UDP port {args.out}")
    in_conn = mavutil.mavlink_connection(in_conn_str)
    out_conn = mavutil.mavlink_connection(out_conn_str)

    while True:
        # Wait for a new MAVLink message (blocking call)
        msg = in_conn.recv_match(blocking=True)
        if msg is None:
            continue

        # Verbose output: display the message type and details
        if args.verbose:
            print(f"Received message: Type = {msg.get_type()}, ID = {msg.get_msgId() if hasattr(msg, 'get_msgId') else 'N/A'}")
            print("Full message details:")
            print(msg.to_dict())

        # Forward the exact raw message to the output connection
        raw_msg = msg.get_msgbuf()
        out_conn.write(raw_msg)

        # If RC channel logic is enabled and the message type is RC_CHANNELS_OVERRIDE, process it.
        if channels_list and msg.get_type() == 'RC_CHANNELS_OVERRIDE':
            now = time.time()
            for ch in channels_list:
                attr_name = f'chan{ch}_raw'
                if not hasattr(msg, attr_name):
                    continue
                value = getattr(msg, attr_name)

                # Initialize state on first received message for this channel.
                if ch not in channel_state:
                    # Do not trigger on the first message; simply record the value.
                    channel_state[ch] = {
                        "candidate": value,
                        "stable_since": now,
                        "last_triggered_value": value,
                        "last_executed": 0
                    }
                    continue

                state = channel_state[ch]
                # Check if the new value is within tolerance of the current candidate.
                if abs(value - state["candidate"]) <= tolerance_ratio * state["candidate"]:
                    # Candidate is stable.
                    if (now - state["stable_since"] >= persist_seconds) and (now - state["last_executed"] >= wait_seconds):
                        # Only trigger if the new stable value differs from the last triggered value beyond tolerance.
                        if abs(value - state["last_triggered_value"]) > tolerance_ratio * state["last_triggered_value"]:
                            cmd = ["/usr/bin/channels.sh", str(ch), str(value)]
                            subprocess.Popen(cmd)
                            state["last_triggered_value"] = value
                            state["last_executed"] = now
                else:
                    # New value is not within tolerance of the current candidate.
                    # Reset the candidate and stable timer.
                    state["candidate"] = value
                    state["stable_since"] = now

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("Exiting...")
