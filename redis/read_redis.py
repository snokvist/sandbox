#!/usr/bin/python3

import json
from redis import Redis
import matplotlib.pyplot as plt
from dotenv import load_dotenv
import os

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


def get_all_sessions():
    """Retrieve all session keys from Redis."""
    try:
        session_keys = redis_client.keys("session:*:settings")
        sessions = [key.split(":")[1] for key in session_keys]
        return sessions
    except Exception as e:
        print(f"Error fetching session keys: {e}")
        return []


def get_session_data(session_id, data_type):
    """Retrieve all data of a specific type for a session."""
    data_list_key = f"session:{session_id}:{data_type}_list"
    try:
        data_keys = redis_client.lrange(data_list_key, 0, -1)
        data_entries = []
        for key in data_keys:
            entry = redis_client.hgetall(key)
            decoded_entry = {k: float(v) if v.replace('.', '', 1).isdigit() else v for k, v in entry.items()}
            data_entries.append(decoded_entry)
        return data_entries
    except Exception as e:
        print(f"Error fetching session data: {e}")
        return []


def filter_data_by_id(data, selected_id):
    """Filter data entries by a specific ID."""
    return [entry for entry in data if entry.get("id") == selected_id]


def plot_data(data, selected_key, title="Data Plot", x_label="Timestamp", y_label="Value"):
    """Plot data for a specific key."""
    if not data:
        print("No data to plot.")
        return

    timestamps = []
    values = []

    print("\nDebug Data Processing:")
    for i, entry in enumerate(data):
        if selected_key in entry:
            try:
                value = entry[selected_key]
                timestamps.append(entry.get('timestamp', 0))
                values.append(value)
            except (ValueError, TypeError) as e:
                print(f"Entry {i} skipped: Invalid value for key '{selected_key}' ({entry[selected_key]}). Error: {e}")
        else:
            print(f"Entry {i} skipped: Key '{selected_key}' not found.")

    print("\nSample Processed Data:")
    for i in range(min(5, len(timestamps))):  # Display the first 5 entries
        print(f"Timestamp: {timestamps[i]}, Value: {values[i]}")

    if not timestamps or not values:
        print("No valid data to plot.")
        return

    plt.figure(figsize=(10, 5))
    plt.plot(timestamps, values, label=f"{selected_key} Data", marker="o")
    plt.title(title)
    plt.xlabel(x_label)
    plt.ylabel(y_label)
    plt.legend()
    plt.grid(True)
    plt.show()


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

    # User selects data type (rx or tx)
    data_type = input("Enter data type (rx/tx): ").lower()
    if data_type not in ("rx", "tx"):
        print("Invalid data type.")
        exit()

    # Retrieve session data
    data = get_session_data(session_id, data_type)
    if not data:
        print(f"No {data_type.upper()} data found for session '{session_id}'.")
        exit()

    print(f"Retrieved {len(data)} entries for session '{session_id}'.")

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
    print(f"Filtered down to {len(data)} entries with ID '{selected_id}'.")

    # Extract available keys for selection
    all_keys = set().union(*(entry.keys() for entry in data))
    print("Available keys for plotting:")
    for idx, key in enumerate(sorted(all_keys), start=1):
        print(f"{idx}. {key}")

    # User selects a key for plotting
    try:
        key_choice = int(input("Select a key by number: "))
        selected_key = sorted(all_keys)[key_choice - 1]
    except (ValueError, IndexError):
        print("Invalid selection.")
        exit()

    print(f"Selected key: {selected_key}")

    # Plot the data for the selected key
    plot_title = f"{data_type.upper()} Data for Session {session_id} - ID: {selected_id} - Key: {selected_key}"
    plot_data(data, selected_key, title=plot_title, x_label="Timestamp", y_label=selected_key)
