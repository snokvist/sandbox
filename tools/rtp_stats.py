#!/usr/bin/env python3
import socket
import struct
import time
from collections import deque
from rich.console import Console
from rich.table import Table
from rich.live import Live
from rich.panel import Panel
from rich.layout import Layout

console = Console()

#
# Separate bins for Packets (7) vs Frames (9)
#
PACKET_BINS = [
    "<0.1 ms",
    "0.1–0.5 ms",
    "0.5–1 ms",
    "1–2 ms",
    "2–5 ms",
    "5–8 ms",
    ">8 ms",
]

FRAME_BINS = [
    "<5 ms",
    "5–8 ms",
    "8–10 ms",
    "10–12 ms",
    "12–14 ms",
    "14–16 ms",
    "16–18 ms",
    "18–20 ms",
    ">20 ms",
]

# 7-step color scale for PACKETS
PACKET_COLORS = [
    "green",         # idx=0
    "chartreuse3",   # idx=1
    "yellow3",       # idx=2
    "gold3",         # idx=3
    "orange3",       # idx=4
    "dark_orange3",  # idx=5
    "bold red",      # idx=6
]

# 9-step color scale for FRAMES
FRAME_COLORS = [
    "green",         # idx=0
    "chartreuse3",   # idx=1
    "yellow3",       # idx=2
    "gold3",         # idx=3
    "orange3",       # idx=4
    "dark_orange3",  # idx=5
    "red1",          # idx=6
    "red3",          # idx=7
    "bold red",      # idx=8
]

FRAME_BOUNDARY_MARKER = "###FRAME_BOUNDARY###"

def parse_rtp_header(packet: bytes):
    """Parse the first 12 bytes of the RTP header and return (payload_type, sequence_number, timestamp)."""
    if len(packet) < 12:
        raise ValueError("Packet too short to contain an RTP header.")
    rtp_header = struct.unpack("!BBHII", packet[:12])
    flags = rtp_header[0]
    version = flags >> 6
    if version != 2:
        raise ValueError("Unsupported RTP version (expected 2).")
    marker_payload = rtp_header[1]
    payload_type = marker_payload & 0x7F
    sequence_number = rtp_header[2]
    timestamp = rtp_header[3]
    return payload_type, sequence_number, timestamp

def bin_index_for_packet_latency(lat_ms: float) -> int:
    """Return the bin index (0..6) for packet latencies."""
    if lat_ms < 0.1:
        return 0
    elif lat_ms < 0.5:
        return 1
    elif lat_ms < 1:
        return 2
    elif lat_ms < 2:
        return 3
    elif lat_ms < 5:
        return 4
    elif lat_ms < 8:
        return 5
    else:
        return 6

def bin_index_for_frame_latency(lat_ms: float) -> int:
    """Return the bin index (0..8) for frame latencies."""
    if lat_ms < 5:
        return 0
    elif lat_ms < 8:
        return 1
    elif lat_ms < 10:
        return 2
    elif lat_ms < 12:
        return 3
    elif lat_ms < 14:
        return 4
    elif lat_ms < 16:
        return 5
    elif lat_ms < 18:
        return 6
    elif lat_ms < 20:
        return 7
    else:
        return 8

def packet_color_symbol(bin_index: int) -> str:
    """Return a colored '#' based on the packet bin index."""
    if 0 <= bin_index < len(PACKET_COLORS):
        return f"[{PACKET_COLORS[bin_index]}]#[/{PACKET_COLORS[bin_index]}]"
    return "[white]#[/white]"

def frame_color_symbol(bin_index: int) -> str:
    """Return a colored '#' based on the frame bin index."""
    if 0 <= bin_index < len(FRAME_COLORS):
        return f"[{FRAME_COLORS[bin_index]}]#[/{FRAME_COLORS[bin_index]}]"
    return "[white]#[/white]"

class RTPStats:
    """Tracks and displays RTP stats, bins, timelines, plus jitter measurements, using a 20s rolling window."""
    ROLLING_WINDOW_S = 20.0

    def __init__(self):
        self.total_packets = 0
        self.out_of_order_packets = 0

        # "Missing frames" can be tracked if you have logic to detect them. Otherwise just set 0 or remove entirely.
        self.missing_frames = 0

        # Rolling 20s latencies
        self.packet_samples = deque()   # (arrival_time, lat_ms)
        self.frame_samples = deque()    # (arrival_time, lat_ms)

        # Rolling 20s for packet bytes (for bitrate, packet rate)
        self.packet_bytes = deque()     # (arrival_time, size_bytes)

        # Rolling 20s frame sizes (end_time, frame_size_bytes)
        self.frame_sizes_20s = deque()
        self.current_frame_size = 0

        # Timelines
        self.last_frames = deque(maxlen=1200)   # store frame lat ms
        self.last_packets = deque(maxlen=2560)  # store packet lat ms or boundary markers

        # Sequence/time
        self.last_sequence = None
        self.last_arrival_time = None
        self.last_timestamp = None
        self.last_frame_ts = None
        self.last_frame_arrival_time = None

        # Bin counts
        self.packet_bin_counts = [0]*len(PACKET_BINS)
        self.frame_bin_counts  = [0]*len(FRAME_BINS)

        # Jitter calculations
        self._prev_pkt_lat = None
        self._prev_frm_lat = None
        self.packet_jitter_samples = []
        self.frame_jitter_samples = []

    def update(self, seq: int, ts: int, arrival_time: float, packet_size: int):
        """Called for every new RTP packet."""
        self.total_packets += 1
        self.packet_bytes.append((arrival_time, packet_size))

        # Detect out-of-order
        if self.last_sequence is not None:
            expected_seq = self.last_sequence + 1
            if seq == expected_seq:
                pass
            elif seq < expected_seq:
                self.out_of_order_packets += 1
        self.last_sequence = seq

        # Inter-packet
        if self.last_arrival_time is not None:
            pkt_lat_ms = (arrival_time - self.last_arrival_time)*1000.0
            self.packet_samples.append((arrival_time, pkt_lat_ms))
            self.last_packets.append(pkt_lat_ms)

            # packet jitter
            if self._prev_pkt_lat is not None:
                self.packet_jitter_samples.append(abs(pkt_lat_ms - self._prev_pkt_lat))
            self._prev_pkt_lat = pkt_lat_ms

            # accumulate frame size
            self.current_frame_size += packet_size
        else:
            self.current_frame_size = packet_size

        self.last_arrival_time = arrival_time

        # Check for a new frame boundary
        if self.last_timestamp is not None and ts != self.last_timestamp:
            # old frame ended
            end_time = arrival_time
            self.frame_sizes_20s.append((end_time, self.current_frame_size))

            # measure frame latency
            if self.last_frame_arrival_time is not None:
                frm_lat_ms = (arrival_time - self.last_frame_arrival_time)*1000.0
                self.frame_samples.append((arrival_time, frm_lat_ms))
                self.last_frames.append(frm_lat_ms)

                # frame jitter
                if self._prev_frm_lat is not None:
                    self.frame_jitter_samples.append(abs(frm_lat_ms - self._prev_frm_lat))
                self._prev_frm_lat = frm_lat_ms

            # boundary marker in packet timeline
            self.last_packets.append(FRAME_BOUNDARY_MARKER)

            # new frame size
            self.current_frame_size = packet_size

        if self.last_timestamp is None:
            self.current_frame_size = packet_size

        if self.last_timestamp is None or ts != self.last_timestamp:
            self.last_frame_arrival_time = arrival_time
            self.last_frame_ts = ts

        self.last_timestamp = ts

    def prune_and_rebin(self):
        """Prune old samples > 20s, then rebuild bin counts."""
        now = time.time()

        # packet_samples
        while self.packet_samples and (now - self.packet_samples[0][0]) > self.ROLLING_WINDOW_S:
            self.packet_samples.popleft()

        # frame_samples
        while self.frame_samples and (now - self.frame_samples[0][0]) > self.ROLLING_WINDOW_S:
            self.frame_samples.popleft()

        # packet_bytes
        while self.packet_bytes and (now - self.packet_bytes[0][0]) > self.ROLLING_WINDOW_S:
            self.packet_bytes.popleft()

        # frame_sizes_20s
        while self.frame_sizes_20s and (now - self.frame_sizes_20s[0][0]) > self.ROLLING_WINDOW_S:
            self.frame_sizes_20s.popleft()

        # Rebuild bin counts
        self.packet_bin_counts = [0]*len(PACKET_BINS)
        self.frame_bin_counts  = [0]*len(FRAME_BINS)

        for (_, lat_ms) in self.packet_samples:
            idx = bin_index_for_packet_latency(lat_ms)
            self.packet_bin_counts[idx] += 1

        for (_, lat_ms) in self.frame_samples:
            idx = bin_index_for_frame_latency(lat_ms)
            self.frame_bin_counts[idx] += 1

        # Optional: you might prune old jitter samples too, or keep them rolling.

    #
    # Rolling 20s stats
    #
    def get_avg_fps(self) -> float:
        if self.frame_samples:
            return len(self.frame_samples) / 20.0
        return 0.0

    def get_avg_packet_rate(self) -> float:
        if self.packet_bytes:
            return len(self.packet_bytes) / 20.0
        return 0.0

    def get_avg_bitrate(self) -> float:
        total_bytes = sum(sz for (_, sz) in self.packet_bytes)
        if total_bytes:
            return (8.0 * total_bytes) / (20.0 * 1000.0)
        return 0.0

    def get_frame_size_stats_20s(self):
        if not self.frame_sizes_20s:
            return (0.0, 0.0, 0.0)
        sizes = [sz for (_, sz) in self.frame_sizes_20s]
        avg_sz = sum(sizes) / len(sizes)
        min_sz = min(sizes)
        max_sz = max(sizes)
        return (avg_sz / 1024.0, min_sz / 1024.0, max_sz / 1024.0)

    #
    # Jitter (avg/min/max) for packets & frames
    #
    def get_packet_jitter_stats(self):
        if not self.packet_jitter_samples:
            return (0.0, 0.0, 0.0)
        avg_j = sum(self.packet_jitter_samples) / len(self.packet_jitter_samples)
        min_j = min(self.packet_jitter_samples)
        max_j = max(self.packet_jitter_samples)
        return (avg_j, min_j, max_j)

    def get_frame_jitter_stats(self):
        if not self.frame_jitter_samples:
            return (0.0, 0.0, 0.0)
        avg_j = sum(self.frame_jitter_samples) / len(self.frame_jitter_samples)
        min_j = min(self.frame_jitter_samples)
        max_j = max(self.frame_jitter_samples)
        return (avg_j, min_j, max_j)

    #
    # Building output tables
    #
    def get_stats_table(self) -> Table:
        t = Table(title="RTP Stats + Rolling 20s", show_lines=True)
        t.add_column("Metric", justify="right")
        t.add_column("Value", justify="right")

        t.add_row("Total Packets", str(self.total_packets))
        t.add_row("Out-of-Order", str(self.out_of_order_packets))
        t.add_row("Missing Frames", str(self.missing_frames))

        fps = self.get_avg_fps()
        pps = self.get_avg_packet_rate()
        kbps = self.get_avg_bitrate()
        t.add_row("Avg FPS (20s)", f"{fps:.2f}")
        t.add_row("Avg Packet Rate", f"{pps:.1f} pps")
        t.add_row("Avg Bitrate", f"{kbps:.1f} Kbps")

        avgKB, minKB, maxKB = self.get_frame_size_stats_20s()
        t.add_row("Avg FrameSize (KB)", f"{avgKB:.2f}")
        t.add_row("Min FrameSize (KB)", f"{minKB:.2f}")
        t.add_row("Max FrameSize (KB)", f"{maxKB:.2f}")

        # Jitter line: "PktJit=(avg/min/max) FrmJit=(avg/min/max)"
        pktA, pktN, pktX = self.get_packet_jitter_stats()
        frmA, frmN, frmX = self.get_frame_jitter_stats()
        jitter_str = (f"PktJit=({pktA:.2f}/{pktN:.2f}/{pktX:.2f}) "
                      f"FrmJit=({frmA:.2f}/{frmN:.2f}/{frmX:.2f})")
        t.add_row("Jitter (ms)", jitter_str)

        return t

    def get_latency_hist_legend_table(self) -> Table:
        table = Table(title="Latency Hist + Legend", show_lines=True)
        table.add_column("PktClr", justify="center")
        table.add_column("PktBin", justify="left")
        table.add_column("PktCnt", justify="right")
        table.add_column("FrmClr", justify="center")
        table.add_column("FrmBin", justify="left")
        table.add_column("FrmCnt", justify="right")

        max_len = max(len(PACKET_BINS), len(FRAME_BINS))
        for i in range(max_len):
            if i < len(PACKET_BINS):
                pkt_bin_label = PACKET_BINS[i]
                pkt_count = self.packet_bin_counts[i]
                pkt_color = packet_color_symbol(i)
            else:
                pkt_bin_label = "N/A"
                pkt_count = 0
                pkt_color = "N/A"

            if i < len(FRAME_BINS):
                frm_bin_label = FRAME_BINS[i]
                frm_count = self.frame_bin_counts[i]
                frm_color = frame_color_symbol(i)
            else:
                frm_bin_label = "N/A"
                frm_count = 0
                frm_color = "N/A"

            table.add_row(
                pkt_color, pkt_bin_label, str(pkt_count),
                frm_color, frm_bin_label, str(frm_count)
            )
        return table

    def get_frame_timeline_panel(self) -> Panel:
        frames_list = list(self.last_frames)
        lines = []
        for row_start in range(0, len(frames_list), 80):
            chunk = frames_list[row_start : row_start+80]
            row_str = ""
            for lat_ms in chunk:
                idx = bin_index_for_frame_latency(lat_ms)
                row_str += frame_color_symbol(idx)
            lines.append(row_str)
        ascii_map = "\n".join(lines) if lines else "No frames yet."
        return Panel(ascii_map, title="Last 1200 Frames")

    def get_packet_timeline_panel(self) -> Panel:
        pkts_list = list(self.last_packets)
        lines = []
        i = 0
        while i < len(pkts_list):
            row_str = ""
            row_end = i + 80
            while i < len(pkts_list) and i < row_end:
                item = pkts_list[i]
                if item == FRAME_BOUNDARY_MARKER:
                    row_str += "[bold white]|[/bold white]"
                else:
                    bin_idx = bin_index_for_packet_latency(item)
                    row_str += packet_color_symbol(bin_idx)
                i += 1
            lines.append(row_str)
        ascii_map = "\n".join(lines) if lines else "No packets yet."
        return Panel(ascii_map, title="Last 2560 Packets")

def render_stats(stats: RTPStats) -> Layout:
    """Render the layout for top (stats/hist) and bottom (timelines)."""
    layout = Layout(name="root")
    layout.split_column(
        Layout(name="top", size=30),
        Layout(name="bottom")
    )

    layout["top"].split_row(
        Layout(name="left"),
        Layout(name="right")
    )

    stats_tbl = stats.get_stats_table()
    layout["top"]["left"].update(Panel(stats_tbl, title="Stats + Rolling 20s"))

    hist_tbl = stats.get_latency_hist_legend_table()
    layout["top"]["right"].update(Panel(hist_tbl, title="Latency Hist + Legend"))

    layout["bottom"].split_row(
        Layout(name="frames"),
        Layout(name="packets")
    )

    fpanel = stats.get_frame_timeline_panel()
    layout["bottom"]["frames"].update(fpanel)

    ppanel = stats.get_packet_timeline_panel()
    layout["bottom"]["packets"].update(ppanel)

    return layout

def main(udp_port=5600):
    """
    Binds a UDP socket with SO_REUSEPORT (if available), captures data
    in 5-second cycles but uses only second 1..4 for stats, then updates once.
    """
    # Attempt to set SO_REUSEPORT for multiple binding processes (Linux only)
    reuseport_opt = None
    if hasattr(socket, "SO_REUSEPORT"):
        reuseport_opt = socket.SO_REUSEPORT

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if reuseport_opt is not None:
        try:
            sock.setsockopt(socket.SOL_SOCKET, reuseport_opt, 1)
        except OSError:
            console.print("[red]Could not set SO_REUSEPORT. Continuing without it.[/red]")

    sock.bind(("0.0.0.0", udp_port))
    sock.setblocking(False)

    stats = RTPStats()

    console.print(f"Listening for RTP on UDP port [bold green]{udp_port}[/bold green], with SO_REUSEPORT if available.")
    console.print("Press [bold magenta]Ctrl+C[/bold magenta] to stop.\n")

    with Live(console=console, auto_refresh=False) as live:
        try:
            while True:
                cycle_start = time.time()
                capture_buf = []  # store (arrival_time, packet) for entire 5s

                # Read data continuously for 5s
                while (time.time() - cycle_start) < 5.0:
                    try:
                        pkt, _ = sock.recvfrom(65536)
                        arrival = time.time()
                        capture_buf.append((arrival, pkt))
                    except BlockingIOError:
                        time.sleep(0.001)  # Avoid busy looping if no data

                # Only keep arrivals from second 1..4
                for (arr_time, p) in capture_buf:
                    delta = arr_time - cycle_start
                    if 1.0 <= delta <= 4.0:
                        try:
                            _, seq, ts = parse_rtp_header(p)
                            stats.update(seq, ts, arr_time, len(p))
                        except ValueError:
                            pass

                # Now prune old data, re-bin
                stats.prune_and_rebin()

                # Update display once
                layout = render_stats(stats)
                live.update(layout, refresh=True)

        except KeyboardInterrupt:
            console.print("[yellow]Stopped by user.[/yellow]")
        finally:
            sock.close()

    # Final snapshot
    stats.prune_and_rebin()
    console.print(render_stats(stats))

if __name__ == "__main__":
    main(udp_port=5600)
