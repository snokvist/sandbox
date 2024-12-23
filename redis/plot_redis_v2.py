#!/usr/bin/python3

import json
from redis import Redis
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
from dotenv import load_dotenv
import os
import fnmatch

# Load environment variables from .env file
load_dotenv()

# Configuration for Redis connection
REDIS_HOST = '192.168.1.51'  # Server to connect to
REDIS_PORT = int(os.getenv('REDIS_PORT', 6379))

# Establish a connection to Redis
try:
    redis_client = Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)
    redis_client.ping()
except Exception as e:
    print(f"Failed to connect to Redis: {e}")
    exit(1)


def is_numeric(value):
    """Check if a string is numeric-like."""
    try:
        float(value)
        return True
    except ValueError:
        return False


def get_all_sessions():
    """Retrieve all session keys from Redis."""
    try:
        session_keys = redis_client.keys("session:*:settings")
        sessions = [key.split(":")[1] for key in session_keys]
        return sessions
    except Exception as e:
        print(f"Error fetching session keys: {e}")
        return []


def get_session_data(session_id, data_type, limit=None):
    """Retrieve a specific number of data points for a session from Redis."""
    data_list_key = f"session:{session_id}:{data_type}_list"
    try:
        # Fetch only the most recent `limit` keys, if specified
        data_keys = redis_client.lrange(data_list_key, -(limit or 0), -1)
        data_entries = []
        for key in data_keys:
            entry = redis_client.hgetall(key)
            decoded_entry = {k: float(v) if is_numeric(v) else v for k, v in entry.items()}
            data_entries.append(decoded_entry)
        return data_entries
    except Exception as e:
        print(f"Error fetching session data: {e}")
        return []


def filter_data_by_id(data, selected_id):
    """Filter data entries by a specific ID."""
    return [entry for entry in data if entry.get("id") == selected_id]


def downsample_data(data, max_points=300):
    """Downsample data to a maximum number of points."""
    if len(data) > max_points:
        step = len(data) // max_points
        return data[::step]
    return data


def initialize_plot(ax, selected_keys):
    """Initialize the plot and return line objects."""
    lines = {key: ax.plot([], [], label=f"{key} Data", marker="o")[0] for key in selected_keys}
    ax.set_title("Initializing...")
    ax.set_xlabel("Timestamp")
    ax.grid(True)

    # Lock legend to the bottom left and prevent movement
    legend = ax.legend(loc="lower left", bbox_to_anchor=(0, 0))
    for line, legend_entry in zip(lines.values(), legend.get_lines()):
        legend_entry.set_picker(True)  # Enable picking for legend entries
        legend_entry.set_pickradius(5)

    def on_pick(event):
        legend_entry = event.artist
        index = list(legend.get_lines()).index(legend_entry)
        key = list(lines.keys())[index]
        line = lines[key]
        line.set_visible(not line.get_visible())  # Toggle visibility
        legend_entry.set_alpha(1.0 if line.get_visible() else 0.2)  # Dim the legend entry
        ax.figure.canvas.draw_idle()  # Redraw the plot

    ax.figure.canvas.mpl_connect("pick_event", on_pick)
    return lines


def update_plot(lines, data, selected_keys, ax):
    """Update the plot with new data."""
    if not data:
        return

    ax.set_title("Live Data")
    for key in selected_keys:
        timestamps = [entry.get('timestamp', 0) for entry in data if key in entry]
        values = [entry[key] for entry in data if key in entry]
        lines[key].set_data(timestamps, values)

    # Adjust axes dynamically
    ax.relim()
    ax.autoscale_view()


def select_keys(all_keys, input_str):
    """Select keys based on user input, supporting mixed numbers and wildcards."""
    selected_keys = set()
    patterns = [s.strip() for s in input_str.split(",")]
    for pattern in patterns:
        if pattern.isdigit():  # Numeric selection
            try:
                selected_keys.add(sorted(all_keys)[int(pattern) - 1])
            except (ValueError, IndexError):
                print(f"Invalid key number: {pattern}")
        else:  # Wildcard selection
            selected_keys.update(fnmatch.filter(all_keys, pattern))
    return sorted(selected_keys)


if __name__ == "__main__":
    # Retrieve all sessions
    sessions = get_all_sessions()
    if not sessions:
        print("No sessions found in Redis.")
        exit()

    # Display sessions with sequential numbering
    print("Available sessions:")
    for idx, session in enumerate(sessions, start=1):
        print(f"{idx}. {session}")

    # User selects a session by number
    try:
        session_choice = int(input("Select a session by number: "))
        session_id = sessions[session_choice - 1]
    except (ValueError, IndexError):
        print("Invalid selection.")
        exit()

    print(f"Selected session: {session_id}")

    # Data limit selection
    print("\nSelect data range:")
    print("1. Use all data")
    print("2. Use only the latest 300 data points")
    data_limit_choice = input("Enter your choice (1/2): ")
    data_limit = 300 if data_limit_choice == "2" else None

    # User selects data type (rx or tx)
    print("\nSelect data type:")
    print("1. tx")
    print("2. rx")
    try:
        data_type_choice = int(input("Enter your choice (1/2): "))
        data_type = {1: "tx", 2: "rx"}[data_type_choice]
    except (ValueError, KeyError):
        print("Invalid data type selection.")
        exit()

    # Fetch limited session data
    data = get_session_data(session_id, data_type, data_limit)

    # Filter by ID
    unique_ids = {entry.get("id") for entry in data if "id" in entry}
    print("Available IDs for filtering:")
    for idx, unique_id in enumerate(sorted(unique_ids), start=1):
        print(f"{idx}. {unique_id}")

    try:
        id_choice = int(input("Select an ID by number: "))
        selected_id = sorted(unique_ids)[id_choice - 1]
    except (ValueError, IndexError):
        print("Invalid selection.")
        exit()

    print(f"Selected ID: {selected_id}")
    data = filter_data_by_id(data, selected_id)

    # User selects keys for plotting
    all_keys = set().union(*(entry.keys() for entry in data))
    print("Available keys for plotting:")
    for idx, key in enumerate(sorted(all_keys), start=1):
        print(f"{idx}. {key}")

    print("\nSelect keys for plotting (comma-separated numbers and/or wildcard patterns):")
    selected_keys_input = input("Enter selection: ")
    selected_keys = select_keys(all_keys, selected_keys_input)

    if not selected_keys:
        print("No matching keys found.")
        exit()

    print(f"Selected keys: {selected_keys}")

    # Refresh rate selection
    print("\nSelect refresh rate:")
    print("1. No reload")
    print("2. Every 1 second")
    print("3. Every 5 seconds")
    print("4. Every 10 seconds")
    refresh_choice = input("Enter your choice (1/2/3/4): ")
    refresh_rate = {1: None, 2: 1, 3: 5, 4: 10}.get(int(refresh_choice), None)

    # Initialize the plot
    fig, ax = plt.subplots()
    lines = initialize_plot(ax, selected_keys)

    def update(frame):
        """Fetch new data and update the plot."""
        current_data = get_session_data(session_id, data_type, data_limit)
        current_data = filter_data_by_id(current_data, selected_id)
        current_data = downsample_data(current_data, 300)
        update_plot(lines, current_data, selected_keys, ax)

    if refresh_rate:
        ani = FuncAnimation(fig, update, interval=refresh_rate * 1000, cache_frame_data=False)
    else:
        update(0)  # Static plot

    plt.show()
