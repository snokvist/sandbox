#!/usr/bin/python3
import socket
import msgpack
import struct
import argparse
import curses
import time
import configparser
import os
import logging
import textwrap
from collections import deque, defaultdict
import sys
import select
import traceback
import pprint  # For pretty-printing

# Data rate table for MCS0-7 and BW 10/20/40 MHz
data_rates = {
    (0, 10): 3.25,
    (1, 10): 6.5,
    (2, 10): 9.75,
    (3, 10): 13.0,
    (4, 10): 19.5,
    (5, 10): 26.0,
    (6, 10): 29.25,
    (7, 10): 32.5,
    (0, 20): 6.5,
    (1, 20): 13.0,
    (2, 20): 19.5,
    (3, 20): 26.0,
    (4, 20): 39.0,
    (5, 20): 52.0,
    (6, 20): 58.5,
    (7, 20): 65.0,
    (0, 40): 13.5,
    (1, 40): 27.0,
    (2, 40): 40.5,
    (3, 40): 54.0,
    (4, 40): 81.0,
    (5, 40): 108.0,
    (6, 40): 121.5,
    (7, 40): 135.0,
}

def load_config(config_file='config.ini'):
    """
    Load configuration from a file. If the file doesn't exist,
    create it with default values.
    """
    config = configparser.ConfigParser()
    if not os.path.exists(config_file):
        default_config_content = """
[DEFAULT]
Host = 127.0.0.1
Port = 8003
MaxRSSISamples = 10
RSSIMin = -95
RSSIMax = -30
RefreshRate = 1200
SmoothingFactor = 0.3
DataTimeout = 2
LogLevel = DISABLED
LogReceivedData = false
OutputFile = /tmp/output.log
ColorLowRSSI = red
ColorMediumRSSI = yellow
ColorHighRSSI = green
ColorOutData = cyan
ColorFECOverhead = blue
ColorRedundancy = magenta
LoopSleepTime = 0.05
SocketTimeout = 1.0
ScreenRefreshInterval = 2
ReconnectionDelay = 5
GraphPacketsMin = 0
GraphPacketsMax = 50
GraphWidth = 40
UtilizationBarWidth = 34
LogFile = wfb_visualize.log
DataRateMultiplier = 0.75
ShowPackets = False
ShowAntennaDetails = False
GroupByFrequency = True
"""
        with open(config_file, 'w') as f:
            f.write(default_config_content.strip())
        print(f"Default configuration file created at {config_file}")
    config.read(config_file)
    return config

def extract_antenna_info(antenna_id):
    """
    Extract the IP address, WLAN index, and WLAN antenna ID.
    """
    ip_address = (antenna_id >> 32) & 0xFFFFFFFF
    wlan_idx = (antenna_id >> 8) & 0xFF
    wlan_antenna_id = antenna_id & 0xFF
    ip_address_str = f"{(ip_address >> 24) & 0xFF}." \
                     f"{(ip_address >> 16) & 0xFF}." \
                     f"{(ip_address >> 8) & 0xFF}." \
                     f"{ip_address & 0xFF}"
    ip_address_hex = f"{ip_address:08X}"
    return ip_address_str, ip_address_hex, wlan_idx, wlan_antenna_id

def rssi_to_bar(rssi_value, max_width, rssi_min, rssi_max):
    """
    Converts an RSSI value to a bar length.
    """
    rssi_value = max(rssi_min, min(rssi_max, rssi_value))
    bar_length = int(round(
        ((rssi_value - rssi_min) / (rssi_max - rssi_min))
        * (max_width - 1))) + 1
    return bar_length

def rssi_to_color(rssi_value, color_pairs):
    """
    Determines the color for an RSSI value using fixed thresholds.
    """
    if rssi_value >= -50:
        return color_pairs['high_rssi']
    elif rssi_value >= -80:
        return color_pairs['medium_rssi']
    else:
        return color_pairs['low_rssi']

def display_data(data_win, max_y, max_x, data, rssi_min, rssi_max,
                 show_packets, show_antenna_details, id_shared_data,
                 color_pairs, connection_lost=False, graph_packets_min=0, graph_packets_max=50,
                 graph_width=40, data_rate_multiplier=0.75,
                 utilization_bar_width=34, group_by_frequency=True,
                 smoothing_factor=0.3):
    """
    Display the processed data in the curses window.
    """
    try:
        # Determine attribute based on connection status
        if connection_lost:
            overall_attr = curses.A_DIM
        else:
            overall_attr = curses.A_NORMAL

        # Clear the window and set attribute
        data_win.erase()
        data_win.attrset(overall_attr)

        current_line = 1
        packet_line = current_line + 3
        data_id = data.get('id', '')
        data_type = data.get('type', '').upper()
        data_win.addstr(current_line, 1, f"ID: {data_id}")
        data_win.addstr(current_line + 1, 1, f"Type: {data_type}")
        total_available_rate = None
        out_mbit = None
        fec_overhead_mbit = None
        fec_n = None
        fec_k = None
        session_data = data.get('session') or {}
        fec_n = session_data.get('fec_n')
        fec_k = session_data.get('fec_k')
        freq = mcs = bw = None
        packets = data.get('packets', {})
        out_bytes = packets.get('out_bytes', (0, 0))
        all_bytes = packets.get('all_bytes', (0, 0))
        if data_type == 'RX':
            if 'rx_ant_stats' in data:
                first_ant_key = next(iter(data['rx_ant_stats']), None)
                if first_ant_key and len(first_ant_key[0]) == 3:
                    freq, mcs, bw = first_ant_key[0]
                    total_available_rate = data_rates.get((mcs, bw))
                    if total_available_rate is not None:
                        total_available_rate *= data_rate_multiplier
            else:
                total_available_rate = 0
            out_mbit = (out_bytes[0] * 8) / (1024 * 1024)
            if fec_n and fec_k and fec_k > 0 and fec_n >= fec_k:
                fec_ratio = fec_n / fec_k
                total_mbit_used = out_mbit * fec_ratio
                fec_overhead_mbit = total_mbit_used - out_mbit
            else:
                fec_ratio = 1
                total_mbit_used = out_mbit
                fec_overhead_mbit = 0
        elif data_type == 'TX':
            # For TX, total_available_rate may not be applicable
            total_available_rate = None

        if show_packets:
            data_win.addstr(packet_line, 1, "Packets:")
            packet_line += 1
            # Display packet stats for both RX and TX
            for key, value in packets.items():
                if packet_line >= max_y - 1:
                    break
                if isinstance(value, tuple):
                    if key in ['all_bytes', 'out_bytes', 'incoming_bytes', 'injected_bytes']:
                        value_mbyte = (value[0] / (1024 * 1024),
                                       value[1] / (1024 * 1024))
                        data_win.addstr(
                            packet_line, 3,
                            f"{key}: {value_mbyte[0]:.1f} MByte/s, "
                            f"{value_mbyte[1]:.1f} MByte total")
                    else:
                        data_win.addstr(
                            packet_line, 3,
                            f"{key}: {value[0]} pps, {value[1]} total")
                else:
                    data_win.addstr(packet_line, 3, f"{key}: {value}")
                packet_line += 1
            if 'session' in data:
                if packet_line < max_y - 2:
                    data_win.addstr(packet_line, 1, "Session Data:")
                    packet_line += 1
                    fec_n_display = session_data.get('fec_n', 'N/A')
                    fec_k_display = session_data.get('fec_k', 'N/A')
                    fec_type = session_data.get('fec_type', 'N/A')
                    epoch = session_data.get('epoch', 'N/A')
                    data_win.addstr(
                        packet_line, 3,
                        f"FEC Type: {fec_type}, FEC_n: {fec_n_display}, "
                        f"FEC_k: {fec_k_display}, Epoch: {epoch}")
                    packet_line += 1
            # Display latency and RF temperature for TX
            if data_type == 'TX':
                latency = data.get('latency', {})
                if latency and packet_line < max_y - 2:
                    data_win.addstr(packet_line, 1, "Latency:")
                    packet_line += 1
                    for key, value in latency.items():
                        if packet_line >= max_y - 1:
                            break
                        data_win.addstr(packet_line, 3, f"{key}: {value}")
                        packet_line += 1
                rf_temperature = data.get('rf_temperature', {})
                if rf_temperature and packet_line < max_y - 2:
                    data_win.addstr(packet_line, 1, "RF Temperature:")
                    packet_line += 1
                    for key, value in rf_temperature.items():
                        if packet_line >= max_y - 1:
                            break
                        data_win.addstr(packet_line, 3, f"{key}: {value}")
                        packet_line += 1
            packet_line += 1
        else:
            packet_line = current_line + 3

        antenna_history = id_shared_data.get('antenna_history', {})
        avg_rssi_history = id_shared_data.get('avg_rssi_history', [])
        fec_history = id_shared_data.get('fec_history', [])
        lost_history = id_shared_data.get('lost_history', [])

        # Antenna Stats Section
        if data_type == 'RX':
            if packet_line < max_y - 2:
                data_win.addstr(packet_line, 1, "Antenna Stats:")
                packet_line += 1

                # Move Utilization Bar here
                labels = []
                label_values = {}
                if total_available_rate and total_available_rate > 0:
                    # Utilization Bar
                    utilization = min(total_mbit_used / total_available_rate, 1.0)
                    utilization_percent = utilization * 100
                    utilization_percent = int(round(utilization_percent))  # Round up the decimal
                    bar_length = utilization_bar_width
                    used_bar_len = int(utilization * bar_length)
                    out_bar_len = int((out_mbit / total_available_rate)
                                      * bar_length)
                    fec_bar_len = used_bar_len - out_bar_len
                    bar_chars = []
                    for i in range(used_bar_len):
                        if i < out_bar_len:
                            bar_chars.append(('█', color_pairs['out_data'] | overall_attr))
                        else:
                            bar_chars.append(('█', color_pairs['fec_overhead'] | overall_attr))
                    bar_chars += [(' ', curses.A_NORMAL | overall_attr)] * \
                        (bar_length - used_bar_len)
                    utilization_label = f"Utilization:"
                    labels.append(utilization_label)
                    label_values[utilization_label] = {
                        'bar_chars': bar_chars,
                        'percent': utilization_percent,
                    }
                    # We will display this after calculating max_label_width

                # Per WLAN IDX Redundancy Bars
                wlan_idx_packet_counts = {}
                wlan_idx_last_updated = {}
                wlan_idx_freqs = {}
                current_time = time.time()
                STALE_THRESHOLD = 5
                for antenna_id, ant in antenna_history.items():
                    last_updated = ant.get('last_updated', 0)
                    if (current_time - last_updated) <= STALE_THRESHOLD:
                        packets_count = ant.get('packets_count', 0)
                        freq = ant.get('freq')
                        ip_address_str, ip_address_hex, wlan_idx, _ = extract_antenna_info(antenna_id)
                        key = (ip_address_hex, wlan_idx)
                        wlan_idx_packet_counts[key] = packets_count
                        wlan_idx_last_updated[key] = ant['last_updated']
                        wlan_idx_freqs[key] = freq

                if wlan_idx_packet_counts:
                    max_packets_count = max(wlan_idx_packet_counts.values())
                else:
                    max_packets_count = 1  # Avoid division by zero

                # Prepare labels for alignment
                for key in wlan_idx_packet_counts.keys():
                    ip_address_hex, wlan_idx = key
                    redundancy_label = f"{ip_address_hex}:{wlan_idx}:"
                    labels.append(redundancy_label)
                if labels:
                    max_label_width = max(len(label) for label in labels)
                else:
                    max_label_width = 0

                # Now display the utilization bar with aligned labels
                if total_available_rate and total_available_rate > 0:
                    label_padding = ' ' * (max_label_width - len(utilization_label))
                    data_win.addstr(packet_line, 3, utilization_label + label_padding + " [")
                    for char, attr in label_values[utilization_label]['bar_chars']:
                        data_win.addstr(char, attr)
                    data_win.addstr("] ")
                    data_win.addstr(f"{label_values[utilization_label]['percent']}%")
                    packet_line += 1

                bar_length = utilization_bar_width  # Ensure bar length is defined

                # Define frequency grouping function
                def freq_group(freq):
                    if freq is None:
                        return 2  # Put None frequencies at the end
                    else:
                        return -freq  # Sort by frequency descending

                # Collect frequencies for grouping
                freq_to_wlan_idxs = defaultdict(list)
                for key in wlan_idx_packet_counts.keys():
                    freq = wlan_idx_freqs.get(key)
                    freq_to_wlan_idxs[freq].append(key)

                if group_by_frequency:
                    sorted_frequencies = sorted(freq_to_wlan_idxs.keys(), reverse=True)
                else:
                    # Combine all wlan_idxs into one list
                    all_wlan_idxs = []
                    for wlan_list in freq_to_wlan_idxs.values():
                        all_wlan_idxs.extend(wlan_list)
                    freq_to_wlan_idxs = {None: all_wlan_idxs}
                    sorted_frequencies = [None]

                for freq in sorted_frequencies:
                    if packet_line >= max_y - 2:
                        break
                    if group_by_frequency:
                        if freq is not None:
                            freq_label = f"{freq} MHz"
                        else:
                            freq_label = "Unknown Frequency"
                        data_win.addstr(packet_line, 3, freq_label, curses.A_BOLD | overall_attr)
                        packet_line += 1
                    wlan_keys = freq_to_wlan_idxs[freq]
                    for key in wlan_keys:
                        ip_address_hex, wlan_idx = key
                        packets_count = wlan_idx_packet_counts[key]
                        freq = wlan_idx_freqs.get(key)

                        # Calculate redundancy percentage
                        raw_redundancy_percent = (packets_count / max_packets_count) * 100 if max_packets_count > 0 else 0

                        # Exponential smoothing
                        prev_redundancy_percent = id_shared_data['redundancy_smoothed'].get(key, raw_redundancy_percent)
                        smoothed_redundancy_percent = (smoothing_factor * raw_redundancy_percent +
                                                       (1 - smoothing_factor) * prev_redundancy_percent)
                        id_shared_data['redundancy_smoothed'][key] = smoothed_redundancy_percent
                        redundancy_percent = smoothed_redundancy_percent

                        redundancy_percent = min(redundancy_percent, 100)  # Cap at 100%
                        redundancy_bar_len = int((redundancy_percent / 100) * bar_length)
                        redundancy_bar_len = min(max(redundancy_bar_len, 0), bar_length)
                        bar_chars = []
                        for i in range(redundancy_bar_len):
                            bar_chars.append(('█', color_pairs['redundancy'] | overall_attr))
                        bar_chars += [(' ', curses.A_NORMAL | overall_attr)] * \
                            (bar_length - redundancy_bar_len)
                        redundancy_label = f"{ip_address_hex}:{wlan_idx}:"
                        label_padding = ' ' * (max_label_width - len(redundancy_label))
                        data_win.addstr(packet_line, 3, redundancy_label + label_padding + " [")
                        for char, attr in bar_chars:
                            data_win.addstr(char, attr)
                        data_win.addstr("] ")
                        data_win.addstr(f"{redundancy_percent:.1f}%")
                        packet_line += 1

                # Legend
                legend_line = "Legend: "
                data_win.addstr(packet_line, 3, legend_line)
                legend_pos = len(legend_line) + 3
                data_win.addstr(packet_line, legend_pos, "█",
                                color_pairs['out_data'] | overall_attr)
                data_win.addstr(" Out Data ")
                data_win.addstr("█", color_pairs['fec_overhead'] | overall_attr)
                data_win.addstr(" FEC Overhead ")
                data_win.addstr("█", color_pairs['redundancy'] | overall_attr)
                data_win.addstr(" Redundancy")
                packet_line += 2  # Added an extra line for spacing

            else:
                # No data flowing, skip redundancy and utilization bars
                pass

            sorted_antennas = []
            for antenna_id, ant in antenna_history.items():
                smoothed_rssi = ant['rssi_smoothed']
                if smoothed_rssi < rssi_min:
                    continue
                prev_rssi = ant['prev_rssi']
                smoothed_snr = ant.get('snr_smoothed', None)
                prev_snr = ant.get('prev_snr', smoothed_snr)
                freq = ant.get('freq')
                mcs = ant.get('mcs')
                bw = ant.get('bw')
                packet_errors = ant.get('packet_errors')
                last_updated = ant.get('last_updated', 0)
                sorted_antennas.append({
                    'antenna_id': antenna_id,
                    'rssi_avg': smoothed_rssi,
                    'prev_rssi': prev_rssi,
                    'snr': smoothed_snr,
                    'prev_snr': prev_snr,
                    'freq': freq,
                    'mcs': mcs,
                    'bw': bw,
                    'packet_errors': packet_errors,
                    'last_updated': last_updated,
                })

            # Build freq_to_antennas mapping
            freq_to_antennas = defaultdict(list)
            for ant in sorted_antennas:
                freq = ant['freq']
                freq_to_antennas[freq].append(ant)

            if group_by_frequency:
                sorted_frequencies = sorted([f for f in freq_to_antennas.keys() if f is not None], reverse=True)
                if None in freq_to_antennas:
                    sorted_frequencies.append(None)
            else:
                # All antennas together
                freq_to_antennas = {None: sorted_antennas}
                sorted_frequencies = [None]

            max_bar_width = graph_width  # Use graph_width for bar width
            STALE_THRESHOLD = 5
            all_antennas_stale = True
            for freq in sorted_frequencies:
                if packet_line >= max_y - 2:
                    break
                if group_by_frequency:
                    if freq is not None:
                        freq_label = f"{freq} MHz"
                    else:
                        freq_label = "Unknown Frequency"
                    data_win.addstr(packet_line, 3, freq_label, curses.A_BOLD | overall_attr)
                    packet_line += 1
                antennas_in_freq = freq_to_antennas[freq]
                antennas_in_freq.sort(key=lambda x: -x['rssi_avg'])
                for ant in antennas_in_freq:
                    if packet_line >= max_y - 2:
                        break
                    antenna_id = ant['antenna_id']
                    smoothed_rssi = ant['rssi_avg']
                    prev_rssi = ant['prev_rssi']
                    snr = ant.get('snr')
                    prev_snr = ant.get('prev_snr')
                    freq = ant['freq']
                    mcs = ant['mcs']
                    bw = ant['bw']
                    packet_errors = ant.get('packet_errors')
                    last_updated = ant.get('last_updated', 0)
                    is_stale = (time.time() - last_updated) > STALE_THRESHOLD
                    if not is_stale:
                        all_antennas_stale = False
                    running_avg = smoothed_rssi
                    bar_len = rssi_to_bar(
                        running_avg, max_width=max_bar_width,
                        rssi_min=rssi_min, rssi_max=rssi_max)
                    if is_stale:
                        antenna_color = color_pairs['stale_antenna']
                    else:
                        antenna_color = rssi_to_color(
                            running_avg, color_pairs)
                    bar = ('█' * bar_len).ljust(max_bar_width)
                    if running_avg > prev_rssi:
                        trend = '+'
                    elif running_avg < prev_rssi:
                        trend = '-'
                    else:
                        trend = '='
                    if snr is not None:
                        snr_display = int(round(snr))
                    else:
                        snr_display = 'N/A'
                    rssi_value_display = int(round(running_avg))
                    rssi_line = f"{rssi_value_display:<4}{trend} {bar} " \
                                f"SNR: {snr_display}"
                    data_win.addstr(packet_line, 3, rssi_line, antenna_color | overall_attr)
                    data_win.attrset(overall_attr)
                    packet_line += 1
                    if show_antenna_details:
                        if packet_line >= max_y - 2:
                            break
                        ip_address, ip_address_hex, wlan_idx, wlan_antenna_id = \
                            extract_antenna_info(antenna_id)
                        ant_id_display = f"IP:{ip_address_hex} " \
                                         f"WLAN IDX:{wlan_idx} " \
                                         f"ANT ID:{wlan_antenna_id}"
                        data_win.addstr(packet_line, 3, ant_id_display)
                        packet_line += 1
                        if freq and mcs is not None and bw:
                            data_rate = data_rates.get((mcs, bw), "N/A")
                            if data_rate != "N/A":
                                data_rate *= data_rate_multiplier
                                data_rate_display = f"{data_rate:.2f} Mbps"
                            else:
                                data_rate_display = "N/A"
                            freq_display = f"F:{freq} MCS:{mcs} BW:{bw} " \
                                           f"Rate:{data_rate_display}"
                        else:
                            freq_display = "F:N/A MCS:N/A BW:N/A Rate:N/A"
                        data_win.addstr(packet_line, 3, freq_display)
                        packet_line += 1
                        if packet_errors is not None:
                            data_win.addstr(packet_line, 3,
                                            f"Packet Errors: {packet_errors}")
                            packet_line += 1

            packet_line += 1
            data_win.addstr(
                packet_line, 3,
                f"RSSI Range: {rssi_min} dBm (min) to "
                f"{rssi_max} dBm (max)", overall_attr)
            packet_line += 1

            # Average RSSI Over Time Graph
            if avg_rssi_history:
                graph_height = 10
                normalized_values = []
                for rssi_value in avg_rssi_history:
                    if rssi_value is not None:
                        normalized_value = int(
                            ((rssi_value - rssi_min) / (rssi_max - rssi_min))
                            * (graph_height - 1))
                    else:
                        normalized_value = None
                    normalized_values.append(normalized_value)
                # Truncate or pad normalized_values to graph_width
                normalized_values = normalized_values[-graph_width:]
                graph_grid = [[' ' for _ in range(graph_width)]
                              for _ in range(graph_height)]
                for col_idx, value in enumerate(normalized_values):
                    if value is None:
                        continue  # No data, leave column empty
                    for row_idx in range(graph_height):
                        if row_idx >= graph_height - value - 1:
                            rssi_value = avg_rssi_history[-len(normalized_values) + col_idx]
                            color = rssi_to_color(
                                rssi_value, color_pairs)
                            graph_grid[row_idx][col_idx] = ('█', color | overall_attr)
                legend_labels = [
                    f"{rssi_max} ",
                    f"{(rssi_max + rssi_min) // 2} ",
                    f"{rssi_min} "
                ]
                data_win.addstr(packet_line, 1,
                                "Average RSSI Over Time:", overall_attr)
                packet_line += 1
                for row_idx, row in enumerate(graph_grid):
                    if packet_line >= max_y - 1:
                        break
                    label = ''
                    if row_idx == 0:
                        label = legend_labels[0]
                    elif row_idx == graph_height // 2:
                        label = legend_labels[1]
                    elif row_idx == graph_height - 1:
                        label = legend_labels[2]
                    label_width = 8
                    label_str = label.rjust(label_width) if label \
                        else ' ' * label_width
                    line_content = ''
                    line_colors = []
                    for cell in row:
                        if isinstance(cell, tuple):
                            char, color = cell
                            line_content += char
                            line_colors.append(color)
                        else:
                            line_content += cell
                            line_colors.append(None)
                    x_pos = 1
                    data_win.addstr(packet_line, x_pos, label_str, overall_attr)
                    graph_x_pos = x_pos + label_width
                    for idx, char in enumerate(line_content):
                        if graph_x_pos + idx >= max_x - 1:
                            break
                        if line_colors[idx]:
                            data_win.addstr(packet_line,
                                            graph_x_pos + idx,
                                            char, line_colors[idx])
                        else:
                            data_win.addstr(packet_line,
                                            graph_x_pos + idx, char, overall_attr)
                    packet_line += 1
            # Add FEC and Lost Packets Graph
            if fec_history and lost_history:
                packet_line += 1
                data_win.addstr(packet_line, 1,
                                "FEC Recovered and Lost Packets Over Time:", overall_attr)
                packet_line += 1
                graph_height = 10
                fec_normalized = []
                lost_normalized = []
                scale_factor = (graph_height - 1) / (graph_packets_max - graph_packets_min) if (graph_packets_max - graph_packets_min) != 0 else 0
                for fec_value, lost_value in zip(fec_history, lost_history):
                    fec_norm = int((fec_value - graph_packets_min) * scale_factor)
                    if fec_value > 0 and fec_norm == 0:
                        fec_norm = 1
                    lost_norm = int((lost_value - graph_packets_min) * scale_factor)
                    if lost_value > 0 and lost_norm == 0:
                        lost_norm = 1
                    fec_normalized.append(fec_norm)
                    lost_normalized.append(lost_norm)
                # Truncate or pad normalized values to graph_width
                fec_normalized = fec_normalized[-graph_width:]
                lost_normalized = lost_normalized[-graph_width:]
                graph_grid = [[' ' for _ in range(graph_width)]
                              for _ in range(graph_height)]
                for col_idx in range(len(fec_normalized)):
                    fec_value = fec_normalized[col_idx]
                    lost_value = lost_normalized[col_idx]
                    original_fec_value = fec_history[-len(fec_normalized) + col_idx]
                    original_lost_value = lost_history[-len(lost_normalized) + col_idx]
                    if original_lost_value > 0:
                        # Plot lost packets (red)
                        for row_idx in range(graph_height - lost_value, graph_height):
                            if 0 <= row_idx < graph_height:
                                graph_grid[row_idx][col_idx] = ('█', color_pairs['lost_packets'] | overall_attr)
                    elif original_fec_value > 0:
                        # Plot FEC recovered packets (magenta)
                        for row_idx in range(graph_height - fec_value, graph_height):
                            if 0 <= row_idx < graph_height:
                                graph_grid[row_idx][col_idx] = ('█', color_pairs['fec_packets'] | overall_attr)
                    else:
                        # No data, leave empty (space)
                        pass  # Do nothing, cell remains ' '
                legend_labels = [
                    f"{graph_packets_max} ",
                    f"{(graph_packets_max + graph_packets_min) // 2} ",
                    f"{graph_packets_min} "
                ]
                for row_idx, row in enumerate(graph_grid):
                    if packet_line >= max_y - 1:
                        break
                    label = ''
                    if row_idx == 0:
                        label = legend_labels[0]
                    elif row_idx == graph_height // 2:
                        label = legend_labels[1]
                    elif row_idx == graph_height - 1:
                        label = legend_labels[2]
                    label_width = 8
                    label_str = label.rjust(label_width) if label \
                        else ' ' * label_width
                    line_content = ''
                    line_colors = []
                    for cell in row:
                        if isinstance(cell, tuple):
                            char, color = cell
                            line_content += char
                            line_colors.append(color)
                        else:
                            line_content += cell
                            line_colors.append(None)
                    x_pos = 1
                    data_win.addstr(packet_line, x_pos, label_str, overall_attr)
                    graph_x_pos = x_pos + label_width
                    for idx, char in enumerate(line_content):
                        if graph_x_pos + idx >= max_x - 1:
                            break
                        if line_colors[idx]:
                            data_win.addstr(packet_line,
                                            graph_x_pos + idx,
                                            char, line_colors[idx])
                        else:
                            data_win.addstr(packet_line,
                                            graph_x_pos + idx, char, overall_attr)
                    packet_line += 1
        data_win.attrset(curses.A_NORMAL)
        data_win.border()
    except curses.error:
        pass

def main():
    """
    Main function to run the TCP MessagePack client with ncurses display.
    """
    parser = argparse.ArgumentParser(
        description="TCP MessagePack client with ncurses display.")
    parser.add_argument('--host', type=str,
                        help='Host to connect to (e.g., 127.0.0.1)')
    parser.add_argument('--port', type=int,
                        help='Port to connect to (e.g., 8003)')
    parser.add_argument('--config', type=str, default='config.ini',
                        help='Path to configuration file')
    args = parser.parse_args()
    config_file = args.config
    config = load_config(config_file)
    host = config['DEFAULT'].get('Host', '127.0.0.1')
    port = int(config['DEFAULT'].get('Port', '8003'))
    if args.host:
        host = args.host
    if args.port:
        port = args.port
    max_rssi_samples = int(config['DEFAULT'].get('MaxRSSISamples', '10'))
    rssi_min = int(config['DEFAULT'].get('RSSIMin', '-95'))
    rssi_max = int(config['DEFAULT'].get('RSSIMax', '-30'))
    refresh_rate = int(config['DEFAULT'].get('RefreshRate', '1200'))
    smoothing_factor = float(config['DEFAULT'].get('SmoothingFactor', '0.3'))
    data_timeout = float(config['DEFAULT'].get('DataTimeout', '2'))
    log_level_str = config['DEFAULT'].get('LogLevel', 'DISABLED').upper()
    log_received_data = config['DEFAULT'].get('LogReceivedData',
                                              'false').lower() == 'true'
    output_file_path = config['DEFAULT'].get('OutputFile', '/tmp/output.log')
    loop_sleep_time = float(config['DEFAULT'].get('LoopSleepTime', '0.05'))
    socket_timeout = float(config['DEFAULT'].get('SocketTimeout', '1.0'))
    screen_refresh_interval = float(config['DEFAULT'].get('ScreenRefreshInterval', '2'))
    reconnection_delay = float(config['DEFAULT'].get('ReconnectionDelay', '5'))
    graph_packets_min = int(config['DEFAULT'].get('GraphPacketsMin', '0'))
    graph_packets_max = int(config['DEFAULT'].get('GraphPacketsMax', '50'))
    graph_width = int(config['DEFAULT'].get('GraphWidth', '40'))
    utilization_bar_width = int(config['DEFAULT'].get('UtilizationBarWidth', str(graph_width - 6)))
    log_file = config['DEFAULT'].get('LogFile', 'wfb_visualize.log')
    data_rate_multiplier = float(config['DEFAULT'].get('DataRateMultiplier', '0.75'))
    show_packets = config['DEFAULT'].get('ShowPackets', 'False').lower() == 'true'
    show_antenna_details = config['DEFAULT'].get('ShowAntennaDetails', 'False').lower() == 'true'
    group_by_frequency = config['DEFAULT'].get('GroupByFrequency', 'True').lower() == 'true'

    color_names = {
        'black': curses.COLOR_BLACK,
        'red': curses.COLOR_RED,
        'green': curses.COLOR_GREEN,
        'yellow': curses.COLOR_YELLOW,
        'blue': curses.COLOR_BLUE,
        'magenta': curses.COLOR_MAGENTA,
        'cyan': curses.COLOR_CYAN,
        'white': curses.COLOR_WHITE,
    }
    color_low_rssi = config['DEFAULT'].get('ColorLowRSSI',
                                           'red').lower()
    color_medium_rssi = config['DEFAULT'].get('ColorMediumRSSI',
                                              'yellow').lower()
    color_high_rssi = config['DEFAULT'].get('ColorHighRSSI',
                                            'green').lower()
    color_out_data = config['DEFAULT'].get('ColorOutData',
                                           'cyan').lower()
    color_fec_overhead = config['DEFAULT'].get('ColorFECOverhead',
                                               'blue').lower()
    color_redundancy = config['DEFAULT'].get('ColorRedundancy',
                                             'magenta').lower()
    # Set up logging
    if log_level_str == 'DISABLED':
        logging.disable(logging.CRITICAL)
        numeric_level = None  # Set numeric_level to None when logging is disabled
    else:
        numeric_level = getattr(logging, log_level_str, None)
        if not isinstance(numeric_level, int):
            numeric_level = logging.INFO
        logging.basicConfig(
            level=numeric_level,
            filename=log_file,
            filemode='a',
            format='%(asctime)s - %(levelname)s - %(message)s'
        )

    # Determine if we are in debug mode
    is_debug_mode = numeric_level is not None and numeric_level <= logging.DEBUG

    history_length = graph_width  # Use graph_width for history length
    shared_data = {}
    current_id_index = 0
    loop_count = 0
    iteration_times = deque(maxlen=5)

    # Initialize curses before the connection loop
    stdscr = curses.initscr()
    curses.start_color()
    curses.noecho()
    curses.cbreak()
    curses.curs_set(0)
    stdscr.keypad(True)
    stdscr.nodelay(True)
    max_y, max_x = stdscr.getmaxyx()
    footer_lines_needed = 2
    footer_height = footer_lines_needed + 2
    title_height = 5  # Increased from 4 to 5
    data_height = max_y - title_height - footer_height
    title_win = curses.newwin(title_height, max_x, 0, 0)
    data_win = curses.newwin(data_height, max_x, title_height, 0)
    footer_win = curses.newwin(footer_height, max_x,
                               title_height + data_height, 0)

    try:
        # Initialize color pairs
        curses.init_pair(1, color_names.get(color_low_rssi,
                                            curses.COLOR_RED), curses.COLOR_BLACK)
        curses.init_pair(2, color_names.get(color_medium_rssi,
                                            curses.COLOR_YELLOW),
                         curses.COLOR_BLACK)
        curses.init_pair(3, color_names.get(color_high_rssi,
                                            curses.COLOR_GREEN),
                         curses.COLOR_BLACK)
        curses.init_pair(5, color_names.get(color_out_data,
                                            curses.COLOR_CYAN),
                         curses.COLOR_BLACK)
        curses.init_pair(6, color_names.get(color_fec_overhead,
                                            curses.COLOR_BLUE),
                         curses.COLOR_BLACK)
        curses.init_pair(7, curses.COLOR_RED, curses.COLOR_BLACK)         # lost_packets color
        curses.init_pair(8, curses.COLOR_MAGENTA, curses.COLOR_BLACK)     # fec_packets color
        curses.init_pair(9, color_names.get(color_redundancy,
                                            curses.COLOR_MAGENTA),
                         curses.COLOR_BLACK)
        curses.init_pair(11, curses.COLOR_WHITE, curses.COLOR_BLACK)
        color_pairs = {
            'low_rssi': curses.color_pair(1),
            'medium_rssi': curses.color_pair(2),
            'high_rssi': curses.color_pair(3),
            'out_data': curses.color_pair(5),
            'fec_overhead': curses.color_pair(6),
            'stale_antenna': curses.color_pair(11) | curses.A_DIM,
            'lost_packets': curses.color_pair(7),
            'fec_packets': curses.color_pair(8),
            'redundancy': curses.color_pair(9),
        }

        shared_data = {
            'cli_title': {'cli_title': 'N/A', 'is_cluster': 'N/A'},
            'connection_lost': False
        }

        while True:
            client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            client_socket.settimeout(socket_timeout)
            client_socket.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            if hasattr(socket, 'TCP_KEEPIDLE'):
                client_socket.setsockopt(socket.IPPROTO_TCP,
                                         socket.TCP_KEEPIDLE, 1)
            if hasattr(socket, 'TCP_KEEPINTVL'):
                client_socket.setsockopt(socket.IPPROTO_TCP,
                                         socket.TCP_KEEPINTVL, 3)
            if hasattr(socket, 'TCP_KEEPCNT'):
                client_socket.setsockopt(socket.IPPROTO_TCP,
                                         socket.TCP_KEEPCNT, 5)

            # Reset variables
            last_resize = time.time()
            last_refresh_time = time.time()
            last_data_time = time.time()
            last_screen_refresh = time.time()
            refresh_needed = True
            current_id_index = 0
            recv_buffer = b''
            incomplete_msg_start_time = None

            try:
                # Attempt to connect
                shared_data['connection_lost'] = True  # Assume connection lost until successful
                overall_attr = curses.A_DIM
                title_win.erase()
                title_win.attrset(overall_attr)
                title_win.addstr(1, 1, f"Attempting to connect to {host}:{port}...")
                title_win.addstr(2, 1, "Reconnect ongoing...")
                title_win.border()
                title_win.noutrefresh()
                curses.doupdate()

                client_socket.connect((host, port))
                client_socket.setblocking(False)
                last_data_time = time.time()
                logging.info(f"Connected to {host}:{port}")

                # Connection successful
                shared_data['connection_lost'] = False
                overall_attr = curses.A_NORMAL
                title_win.erase()
                title_win.attrset(overall_attr)
                title_win.addstr(1, 1, f"Connected to {host}:{port}")
                title_win.border()
                title_win.noutrefresh()
                curses.doupdate()
                time.sleep(1)  # Brief pause to show connection message

                while True:
                    loop_start_time = time.time()
                    loop_count += 1

                    # Handle keyboard input
                    try:
                        ch = stdscr.getch()
                        if ch == ord('q'):
                            raise KeyboardInterrupt
                        elif ch == ord('t'):
                            current_id_index += 1
                            refresh_needed = True
                        elif ch == ord('p'):
                            show_packets = not show_packets
                            refresh_needed = True
                        elif ch == ord('d'):
                            show_antenna_details = not show_antenna_details
                            refresh_needed = True
                        elif ch == ord('s'):
                            group_by_frequency = not group_by_frequency
                            refresh_needed = True
                    except curses.error:
                        pass

                    # Socket data reception
                    try:
                        readable, _, _ = select.select([client_socket], [], [], loop_sleep_time)
                        if client_socket in readable:
                            try:
                                chunk = client_socket.recv(4096)
                                if not chunk:
                                    logging.info("Socket closed by the server.")
                                    shared_data['connection_lost'] = True
                                    break
                                recv_buffer += chunk
                                last_data_time = time.time()
                                incomplete_msg_start_time = None
                            except socket.timeout:
                                pass
                            except Exception as e:
                                logging.debug(f"Error receiving data: {e}", exc_info=True)
                                shared_data['connection_lost'] = True
                                break
                    except Exception as e:
                        logging.debug(f"Select error: {e}", exc_info=True)
                        shared_data['connection_lost'] = True

                    data_received_in_this_iteration = False  # Flag to track data reception

                    # Message processing
                    try:
                        while True:
                            if len(recv_buffer) < 4:
                                incomplete_msg_start_time = None
                                break
                            msg_length = struct.unpack('!I', recv_buffer[:4])[0]
                            total_msg_length = 4 + msg_length
                            if len(recv_buffer) < total_msg_length:
                                if incomplete_msg_start_time is None:
                                    incomplete_msg_start_time = time.time()
                                else:
                                    if time.time() - incomplete_msg_start_time > data_timeout:
                                        logging.warning("Incomplete message timeout "
                                                        "exceeded. Discarding.")
                                        recv_buffer = recv_buffer[4:]
                                        incomplete_msg_start_time = None
                                break
                            incomplete_msg_start_time = None
                            msg_data = recv_buffer[4:total_msg_length]
                            recv_buffer = recv_buffer[total_msg_length:]
                            try:
                                data = msgpack.unpackb(msg_data, use_list=False, strict_map_key=False)
                                if log_received_data:
                                    with open(output_file_path, 'a') as f:
                                        human_readable_data = pprint.pformat(data)
                                        f.write(human_readable_data + '\n')
                                data_id = data.get('id')
                                if data_id:
                                    if data_id not in shared_data:
                                        shared_data[data_id] = {
                                            'data': data,
                                            'antenna_history': {},
                                            'avg_rssi_history': deque(maxlen=history_length),
                                            'fec_history': deque(maxlen=history_length),
                                            'lost_history': deque(maxlen=history_length),
                                            'redundancy_smoothed': {},  # For exponential smoothing
                                            'last_data_time': time.time(),
                                        }
                                    else:
                                        shared_data[data_id]['data'] = data
                                        shared_data[data_id]['last_data_time'] = time.time()
                                    id_shared_data = shared_data[data_id]
                                    data_received_in_this_iteration = True
                                    data_type = data.get('type', '').upper()
                                    if data_type == 'RX' and 'rx_ant_stats' in data:
                                        ant_stats = data['rx_ant_stats']
                                        antenna_history = id_shared_data['antenna_history']
                                        current_time = time.time()
                                        for ant_key, ant_value in ant_stats.items():
                                            freq_tuple, antenna_id = ant_key
                                            packets_count, rssi_min_val, rssi_avg, \
                                                rssi_max_val, *rest = ant_value
                                            snr = rest[0] if len(rest) > 0 else None
                                            if antenna_id not in antenna_history:
                                                antenna_history[antenna_id] = {
                                                    'rssi_smoothed': rssi_avg,
                                                    'snr_smoothed': snr if snr
                                                                     is not None else 0,
                                                    'prev_rssi': rssi_avg,
                                                    'prev_snr': snr if snr
                                                                  is not None else 0,
                                                    'freq': freq_tuple[0]
                                                    if freq_tuple else None,
                                                    'mcs': freq_tuple[1]
                                                    if freq_tuple else None,
                                                    'bw': freq_tuple[2]
                                                    if freq_tuple else None,
                                                    'packet_errors': rest[1]
                                                    if len(rest) > 1 else None,
                                                    'last_updated': current_time,
                                                    'packets_count': packets_count,
                                                }
                                            else:
                                                antenna_history[antenna_id]['prev_rssi'] = \
                                                    antenna_history[antenna_id]['rssi_smoothed']
                                                antenna_history[antenna_id]['prev_snr'] = \
                                                    antenna_history[antenna_id]['snr_smoothed']
                                                prev_rssi = antenna_history[antenna_id]['rssi_smoothed']
                                                smoothed_rssi = smoothing_factor * rssi_avg \
                                                    + (1 - smoothing_factor) * prev_rssi
                                                antenna_history[antenna_id]['rssi_smoothed'] = \
                                                    smoothed_rssi
                                                if snr is not None:
                                                    prev_snr = antenna_history[antenna_id]['snr_smoothed']
                                                    smoothed_snr = smoothing_factor * snr \
                                                        + (1 - smoothing_factor) * prev_snr
                                                    antenna_history[antenna_id]['snr_smoothed'] = \
                                                        smoothed_snr
                                                antenna_history[antenna_id]['freq'] = \
                                                    freq_tuple[0] if freq_tuple else None
                                                antenna_history[antenna_id]['mcs'] = \
                                                    freq_tuple[1] if freq_tuple else None
                                                antenna_history[antenna_id]['bw'] = \
                                                    freq_tuple[2] if freq_tuple else None
                                                antenna_history[antenna_id]['packet_errors'] = \
                                                    rest[1] if len(rest) > 1 else None
                                                antenna_history[antenna_id]['last_updated'] = \
                                                    current_time
                                                antenna_history[antenna_id]['packets_count'] = packets_count
                                        # Only include antennas within RSSI range
                                        smoothed_rssi_values = [ant['rssi_smoothed']
                                                                for ant in antenna_history.values()
                                                                if rssi_min <= ant['rssi_smoothed'] <= rssi_max]
                                        if smoothed_rssi_values:
                                            avg_rssi = sum(smoothed_rssi_values) / \
                                                       len(smoothed_rssi_values)
                                            id_shared_data['avg_rssi_history'].append(avg_rssi)
                                        else:
                                            # Append rssi_min to reflect no active antennas
                                            id_shared_data['avg_rssi_history'].append(rssi_min)
                                        # Update FEC and Lost packets history
                                        packets = data.get('packets', {})
                                        lost_packets = packets.get('lost', (0,))[0]
                                        fec_rec_packets = packets.get('fec_rec', (0,))[0]
                                        id_shared_data['fec_history'].append(fec_rec_packets)
                                        id_shared_data['lost_history'].append(lost_packets)
                                elif data.get('type') == 'cli_title':
                                    shared_data['cli_title'] = {
                                        'cli_title': data.get('cli_title', 'N/A'),
                                        'is_cluster': data.get('is_cluster', 'N/A')
                                    }
                                    refresh_needed = True
                            except Exception as e:
                                logging.debug(f"Error unpacking message: {e}", exc_info=True)
                                continue
                    except Exception as e:
                        logging.debug(f"Message processing exception: {e}", exc_info=True)

                    # For each ID in shared_data, update avg_rssi_history if no data received
                    current_time = time.time()
                    for data_id, id_shared_data in shared_data.items():
                        if data_id in ['cli_title', 'connection_lost', 'x_link_enabled', 'active_frequencies']:
                            continue  # Skip non-data entries
                        last_data_time_id = id_shared_data.get('last_data_time', 0)
                        if current_time - last_data_time_id > data_timeout:
                            # No data received recently, append rssi_min to avg_rssi_history
                            id_shared_data['avg_rssi_history'].append(rssi_min)
                            id_shared_data['fec_history'].append(0)
                            id_shared_data['lost_history'].append(0)

                    # Collect all active frequencies
                    all_frequencies = set()
                    for data_id, id_shared_data in shared_data.items():
                        if data_id in ['cli_title', 'connection_lost', 'x_link_enabled', 'active_frequencies']:
                            continue
                        antenna_history = id_shared_data.get('antenna_history', {})
                        for ant in antenna_history.values():
                            freq = ant.get('freq')
                            if freq:
                                all_frequencies.add(freq)

                    if len(all_frequencies) > 1:
                        shared_data['x_link_enabled'] = True
                        shared_data['active_frequencies'] = sorted(all_frequencies)
                    else:
                        shared_data['x_link_enabled'] = False
                        shared_data['active_frequencies'] = sorted(all_frequencies)

                    current_time = time.time()
                    if (current_time - last_screen_refresh) >= screen_refresh_interval or refresh_needed:
                        try:
                            # Determine attribute based on connection status
                            if shared_data.get('connection_lost', False):
                                overall_attr = curses.A_DIM
                            else:
                                overall_attr = curses.A_NORMAL

                            # Clear title and footer windows
                            title_win.attrset(overall_attr)
                            footer_win.attrset(overall_attr)
                            title_win.erase()
                            footer_win.erase()

                            cli_title_data = shared_data.get('cli_title', {})
                            if shared_data.get('connection_lost', False):
                                title_win.addstr(1, 1, f"Reconnect ongoing...", overall_attr)
                            else:
                                title_win.addstr(
                                    1, 1, f"CLI Title: {cli_title_data.get('cli_title', 'N/A')}")
                                title_win.addstr(
                                    2, 1, f"Is Cluster: {cli_title_data.get('is_cluster', 'N/A')}")
                                if shared_data.get('x_link_enabled', False):
                                    frequencies_str = ', '.join(f"{freq}MHz" for freq in shared_data.get('active_frequencies', []))
                                    title_win.addstr(3, 1, f"OpenIPC X-link enabled", overall_attr)
                                    title_win.addstr(4, 1, f"Frequencies: {frequencies_str}", overall_attr)
                                else:
                                    frequencies = shared_data.get('active_frequencies', [])
                                    if frequencies:
                                        frequencies_str = ', '.join(f"{freq}MHz" for freq in frequencies)
                                        title_win.addstr(3, 1, f"Frequency: {frequencies_str}", overall_attr)
                                    else:
                                        title_win.addstr(3, 1, f"Frequency: N/A", overall_attr)
                            if is_debug_mode:
                                avg_iter_time = (sum(iteration_times) / len(iteration_times)
                                                 if iteration_times else 0)
                                title_win.addstr(
                                    1, max_x - 30,
                                    f"Loop Count: {loop_count}")
                                title_win.addstr(
                                    2, max_x - 30,
                                    f"Avg Iter Time: {avg_iter_time:.4f}s")
                            exclude_keys = ['cli_title', 'connection_lost', 'x_link_enabled', 'active_frequencies']
                            available_ids = [key for key in shared_data.keys()
                                             if key not in exclude_keys]
                            if available_ids:
                                if 'video rx' in available_ids:
                                    available_ids.remove('video rx')
                                    available_ids.insert(0, 'video rx')
                                current_id_index %= len(available_ids)
                                current_id = available_ids[current_id_index]
                                id_shared_data = shared_data[current_id]
                                data_entry = id_shared_data['data']
                                data_type = data_entry.get('type', '').upper()
                                if data_type == 'TX':
                                    effective_show_packets = True
                                else:
                                    effective_show_packets = show_packets
                                display_data(
                                    data_win, data_height, max_x, data_entry,
                                    rssi_min, rssi_max, effective_show_packets,
                                    show_antenna_details, id_shared_data,
                                    color_pairs,
                                    connection_lost=shared_data.get('connection_lost', False),
                                    graph_packets_min=graph_packets_min,
                                    graph_packets_max=graph_packets_max,
                                    graph_width=graph_width,
                                    data_rate_multiplier=data_rate_multiplier,
                                    utilization_bar_width=utilization_bar_width,
                                    group_by_frequency=group_by_frequency,
                                    smoothing_factor=smoothing_factor)
                            else:
                                data_win.addstr(1, 1, "No data available", overall_attr)
                            title_win.border()
                            footer_win.border()
                            footer_text = "Keys: 'q' to quit | 't' to cycle IDs | 'p' to " \
                                          "toggle Packets | 'd' to toggle Antenna Details | 's' to toggle Grouping by Frequency"
                            wrapped_footer = textwrap.wrap(footer_text, max_x - 2)
                            for idx, line in enumerate(wrapped_footer):
                                if idx >= footer_height - 2:
                                    break
                                footer_win.addstr(idx + 1, 1, line)
                            data_win.noutrefresh()
                            title_win.noutrefresh()
                            footer_win.noutrefresh()
                            curses.doupdate()
                            refresh_needed = False
                            last_refresh_time = current_time
                            last_screen_refresh = current_time
                        except curses.error as e:
                            logging.debug(f"Curses display exception: {e}", exc_info=True)
                        except Exception as e:
                            # Display exception on the screen
                            data_win.addstr(1, 1, "An error occurred:")
                            error_lines = traceback.format_exc().splitlines()
                            for idx, line in enumerate(error_lines):
                                if idx + 2 >= data_height:
                                    break
                                data_win.addstr(idx + 2, 1, line)
                            data_win.noutrefresh()
                            curses.doupdate()
                            time.sleep(5)
                            break  # Exit the inner loop to reconnect

                    # Handle window resizing
                    current_size = stdscr.getmaxyx()
                    if current_size != (max_y, max_x):
                        try:
                            max_y, max_x = current_size
                            data_height = max_y - title_height - footer_height
                            title_win.resize(title_height, max_x)
                            data_win.resize(data_height, max_x)
                            data_win.mvwin(title_height, 0)
                            footer_win.resize(footer_height, max_x)
                            footer_win.mvwin(title_height + data_height, 0)
                            refresh_needed = True
                        except curses.error as e:
                            logging.debug(f"Curses resize exception: {e}", exc_info=True)

                    # Add a small sleep to reduce CPU usage
                    time.sleep(loop_sleep_time)

            except (ConnectionRefusedError, socket.error) as e:
                # Connection failed
                shared_data['connection_lost'] = True
                overall_attr = curses.A_DIM
                title_win.attrset(overall_attr)
                title_win.erase()
                title_win.addstr(1, 1, f"Could not connect to {host}:{port}")
                title_win.addstr(2, 1, f"Retrying in {reconnection_delay} seconds...")
                title_win.border()
                title_win.noutrefresh()
                curses.doupdate()
                logging.error(f"Connection failed: {e}")
                time.sleep(reconnection_delay)
                continue  # Retry connection
            except KeyboardInterrupt:
                break
            except Exception as e:
                # Display exception on the screen
                data_win.addstr(1, 1, "An error occurred:")
                error_lines = traceback.format_exc().splitlines()
                for idx, line in enumerate(error_lines):
                    if idx + 2 >= data_height:
                        break
                    data_win.addstr(idx + 2, 1, line)
                data_win.noutrefresh()
                curses.doupdate()
                time.sleep(5)
                break  # Exit the main loop
            finally:
                client_socket.close()

    finally:
        # Clean up curses
        curses.nocbreak()
        stdscr.keypad(False)
        curses.echo()
        curses.endwin()

if __name__ == "__main__":
    main()
