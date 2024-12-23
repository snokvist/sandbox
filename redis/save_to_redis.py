#!/usr/bin/python3

import socket
import json
import logging
from datetime import datetime
from typing import Any, Dict
from redis import Redis, ConnectionError

# Logging configuration
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")

# Redis configuration
REDIS_HOST = '192.168.1.51'
REDIS_PORT = 6379
redis_client = Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)

# JSON server configuration
SERVER_HOST = '192.168.1.49'
SERVER_PORT = 8103
BUFFER_SIZE = 4096

current_session = None


def flatten_and_store(json_obj: Dict[str, Any], redis_key: str):
    """Recursively flatten JSON and store it in Redis."""
    def recursive_flatten(data, parent_key=""):
        for key, value in data.items():
            full_key = f"{parent_key}{key}" if not parent_key else f"{parent_key}:{key}"
            if isinstance(value, dict):
                recursive_flatten(value, full_key)
            elif isinstance(value, list):
                for i, item in enumerate(value):
                    list_key = f"{full_key}:{i}"
                    if isinstance(item, dict):
                        recursive_flatten(item, list_key)
                    else:
                        redis_client.hset(redis_key, list_key, normalize_redis_value(item))
            else:
                redis_client.hset(redis_key, full_key, normalize_redis_value(value))

    recursive_flatten(json_obj)


def normalize_redis_value(value):
    """Normalize values to a Redis-compatible format."""
    if isinstance(value, bool):
        return "true" if value else "false"  # Convert boolean to string
    elif value is None:
        return "null"  # Represent `None` as "null"
    elif isinstance(value, (int, float, str)):
        return value  # Pass compatible types as is
    else:
        raise TypeError(f"Unsupported data type: {type(value)} for value: {value}")


def process_settings(data: Dict[str, Any]):
    """Process and save 'settings' type data."""
    global current_session
    current_session = data.get("profile", "default") + "_" + datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    redis_key = f"session:{current_session}:settings"
    try:
        flatten_and_store(data.get("settings", {}), redis_key)
        logging.info(f"Settings saved under session: {redis_key}")
    except ConnectionError as e:
        logging.error(f"Redis error while saving settings: {e}")


def process_tx(data: Dict[str, Any]):
    """Process and save 'tx' type data."""
    global current_session
    timestamp = str(data.get("timestamp", datetime.now().timestamp()))
    redis_key = f"session:{current_session}:tx:{timestamp}"

    try:
        # Flatten and store the TX data
        flatten_and_store(data, redis_key)
        # Append key to the session TX list
        redis_client.rpush(f"session:{current_session}:tx_list", redis_key)
        logging.info(f"TX data saved: {redis_key}")
    except ConnectionError as e:
        logging.error(f"Redis error while saving TX data: {e}")


def process_rx(data: Dict[str, Any]):
    """Process and save 'rx' type data."""
    global current_session
    timestamp = str(data.get("timestamp", datetime.now().timestamp()))
    redis_key = f"session:{current_session}:rx:{timestamp}"

    try:
        # Flatten and store the RX data
        flatten_and_store(data, redis_key)
        # Append key to the session RX list
        redis_client.rpush(f"session:{current_session}:rx_list", redis_key)
        logging.info(f"RX data saved: {redis_key}")
    except ConnectionError as e:
        logging.error(f"Redis error while saving RX data: {e}")


def process_data(data: Dict[str, Any]):
    """Route data to the appropriate handler based on its type."""
    data_type = data.get("type")
    if data_type == "settings":
        process_settings(data)
    elif data_type == "tx":
        process_tx(data)
    elif data_type == "rx":
        process_rx(data)
    else:
        logging.warning(f"Unknown data type: {data_type}")


def connect_to_server() -> socket.socket | None:
    """Establish a connection to the JSON server."""
    while True:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect((SERVER_HOST, SERVER_PORT))
            logging.info(f"Connected to server at {SERVER_HOST}:{SERVER_PORT}")
            return sock
        except socket.error as e:
            logging.error(f"Error connecting to server: {e}")
            time.sleep(5)


def process_data_from_socket(sock: socket.socket):
    """Process incoming data from the server."""
    buffer = ""
    try:
        while True:
            chunk = sock.recv(BUFFER_SIZE).decode('utf-8')
            if not chunk:
                logging.warning("Connection closed by the server.")
                break

            buffer += chunk
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                try:
                    data = json.loads(line)
                    process_data(data)
                except json.JSONDecodeError as e:
                    logging.error(f"Error decoding JSON: {e}")
    except socket.error as e:
        logging.error(f"Error receiving data: {e}")
    finally:
        sock.close()


def main():
    """Main function to handle server connection and data processing."""
    while True:
        sock = connect_to_server()
        if sock:
            process_data_from_socket(sock)


if __name__ == "__main__":
    main()
