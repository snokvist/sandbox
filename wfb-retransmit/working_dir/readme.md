SCTP (Stream Control Transmission Protocol) is a transport-layer protocol designed to combine features of both TCP and UDP while introducing unique enhancements. SCTP provides reliable, message-oriented communication with support for multi-streaming and multi-homing, making it suitable for a wide range of applications, from telecommunications signaling to real-time streaming.

How SCTP Works: SCTP establishes an association (similar to a connection in TCP) between two endpoints. Data is transmitted as chunks within messages, and each chunk is assigned a Transmission Sequence Number (TSN) for tracking. The protocol uses Selective Acknowledgments (SACKs) to inform the sender about received and missing chunks, enabling efficient retransmissions. Multi-streaming allows independent data streams within a single association, preventing head-of-line blocking. Multi-homing supports fault tolerance by enabling communication over multiple network paths, switching automatically if the primary path fails. Unlike TCP, SCTP preserves message boundaries, ensuring application-level messages remain intact.

Comparison with TCP and UDP: TCP is a connection-oriented, reliable byte-stream protocol. It ensures in-order delivery but can suffer from head-of-line blocking, where one lost packet delays all subsequent data. UDP is a connectionless, unreliable protocol that is lightweight and ideal for low-latency, best-effort delivery. SCTP combines the reliability of TCP and the message orientation of UDP while introducing features like multi-streaming and multi-homing. SCTP is more complex than both TCP and UDP, which can lead to slightly higher overhead. However, it is more efficient in scenarios requiring reliability and reduced latency for independent streams.

Optimizing SCTP for Low Latency in High Throughput Scenarios (Below 50ms): To achieve low latency in high-throughput environments, optimizing SCTP requires fine-tuning on both the sender and receiver sides. Key factors include reducing retransmission delays, minimizing overhead, and prioritizing timely data delivery.

Sender-Side Optimizations:

Use Partial Reliability (PR-SCTP): PR-SCTP allows messages to have a Time-to-Live (TTL), ensuring outdated messages are discarded rather than retransmitted. This reduces latency by focusing resources on timely data delivery.
Enable SCTP_NODELAY: Disables the Nagle algorithm, ensuring chunks are sent immediately instead of waiting to batch them. This minimizes delays for small messages.
Optimize RTO Settings (SCTP_RTOINFO): Configure low retransmission timeout values to ensure prompt retransmissions. For example, set a minimum RTO of 10ms, an initial RTO of 50ms, and a maximum RTO of 200ms.
Limit Retransmission Attempts (SCTP_MAXATTEMPTS): Reduce the maximum retransmissions to prioritize new data over repeated attempts for lost messages.
Set Maximum Burst Size (SCTP_MAX_BURST): Restrict the number of chunks sent in a burst to prevent congestion and packet drops.
Receiver-Side Optimizations:

Respond to Heartbeats Promptly: Heartbeat messages are used by the sender to monitor path health. Ensuring quick responses helps maintain efficient communication.
Enable Selective Acknowledgments (SACK): Efficiently acknowledge received chunks and identify missing ones to minimize retransmission delays.
Optimize Buffer Sizes (SO_RCVBUF): Adjust receive buffer sizes to match application requirements, avoiding excessive buffering that could increase latency.
Minimize Processing Delays: Use efficient algorithms and hardware to handle high throughput without introducing bottlenecks.
General Recommendations:

Avoid Head-of-Line Blocking: Leverage multi-streaming to isolate different types of data in separate streams, ensuring one stream's delay does not affect others.
Tune Congestion Control: If the network is stable and controlled, consider reducing congestion control aggressiveness to prioritize throughput and latency.
Use High-Performance Hardware: Network interface cards (NICs) with SCTP offload support can reduce processing overhead.
Prioritize Path Selection: For multi-homing, configure the primary path to be the one with the lowest latency and most stable connection.
In summary, SCTP provides a robust and versatile alternative to TCP and UDP, combining reliability, message orientation, multi-streaming, and multi-homing. When optimized correctly, SCTP can achieve low latency and high throughput, making it suitable for real-time applications. Key optimizations involve tuning retransmission settings, enabling partial reliability, and leveraging multi-streaming to avoid bottlenecks. Properly configured, SCTP can deliver latency under 50ms, even in demanding scenarios.

scp_gateway:
This program is an SCTP Gateway that listens for UDP packets and forwards them over an SCTP connection, with features like partial reliability, configurable buffers, and statistics reporting. It uses a circular buffer to queue incoming UDP data and re-establishes the SCTP connection if it goes down, retrying indefinitely every second.

To start the program, compile it and run with options, for example: ./sctp_gateway --udp-port 5600 --sctp-port 6600 --sctp-address 10.5.0.1 --verbose This will bind UDP to port 5600 and attempt SCTP connection to 10.5.0.1 on port 6600, printing statistics if verbose is enabled.

Key settings include: --queue-size <slots>: defines the circular buffer capacity. --udp-buffer <size_kb>, --sctp-buffer <size_kb>: adjusts the socket buffer sizes. Larger values can help throughput but may increase latency. --rto-min, --rto-max, --rto-initial: tune SCTP retransmission timeouts. --pr-ttl <ms>: sets Partial Reliability TTL for dropping stale data. If omitted, every packet is reliably delivered unless the user sets it. --sctp-maxattempts <n>: how many times SCTP tries to establish the association. --sctp-heartbeat <ms>: heartbeat interval in milliseconds, helping detect disconnections or maintain NAT mappings.

Statistics, updated every two seconds in verbose mode, show throughput (Mbit/s), packet rates, drop counters, a UDP inter-arrival histogram, queue usage, and SCTP SNMP data. On receiving CTRL+C, or if a nonrecoverable error occurs, the program shuts down gracefully.

sctp_listener:
This program is an SCTP-based receiver that uses partial reliability, tracks packet statistics in a 10-second ring buffer, and displays results via an Ncurses interface. It listens indefinitely for SCTP connections on a chosen port and, upon each accepted connection, spawns a thread to receive data, handle partial reliability logic (missing, recovered, irretrievable sequences), and forward the packets to UDP on localhost. When the peer disconnects, the receiver returns to listening and can accept a new connection, preserving global statistics (packets, bytes) and a continuously updated ring buffer. To start, compile with “gcc -o sctp_receiver_ncurses_relisten sctp_receiver_ncurses_relisten.c -lsctp -lncurses -lpthread” and run “./sctp_receiver_ncurses_relisten” plus optional arguments like “--port <port>”, “--rto-min <ms>”, and “--buffer-kb <size>” to customize settings. The most important parameters include the SCTP port (defaults to 6600), partial reliability TTL (“--pr-sctp-ttl”), RTO values (“--rto-min”, “--rto-max”), delayed ACK time (“--delayed-ack-time”), and the buffer sizes (“--buffer-kb”). Once running, the Ncurses interface updates every two seconds, showing current statistics, ASCII histograms, and details on available SCTP counters from /proc/net/sctp/snmp. Ctrl+C cleanly stops the receiver.
