#!/usr/bin/env python3
"""
ps4_full.py  –  DualShock-4 reader with
• 250 Hz low-latency loop + timing probe
• live dashboard of axes / buttons / D-pad
• optional SCHED_FIFO priority
"""

import sys, time, shutil, statistics
import pygame

# ───────── user settings ─────────
TARGET_HZ   = 250            # 4 ms loop
UI_HZ       = 20             # dashboard redraw
REPORT_SEC  = 2.0            # timing stats interval
DEAD_ZONE   = 0.05
RT_PRIO     = 10             # real-time priority (1-99)
# ─────────────────────────────────

PERIOD_S = 1.0 / TARGET_HZ
UI_S     = 1.0 / UI_HZ
COLS     = shutil.get_terminal_size((80, 24)).columns

# ────── helpers ──────
def cls():
    sys.stdout.write("\033[H\033[2J")
    sys.stdout.flush()

def banner(t):
    print(t);  print("─" * COLS)

def dz(v):                          # dead-zone
    return 0.0 if abs(v) < DEAD_ZONE else v

def read_hat(js):
    """
    Return D-pad state as (x, y) ∈ {-1, 0, 1}².
    Order of preference:
      1. Real SDL hat
      2. Axes 6 (hor) & 7 (vert)
      3. Buttons 11-14  (U D L R)
    """
    # 1. SDL hat
    if js.get_numhats():
        return js.get_hat(0)

    # 2. Axes 6/7 (round to int so ±1 or 0)
    if js.get_numaxes() >= 8:
        x = round(js.get_axis(6))
        y = -round(js.get_axis(7))           # SDL up = –1
        if (x, y) != (0, 0):
            return (x, y)

    # 3. Buttons 11-14
    mapping = {11: (0, 1), 12: (0, -1), 13: (-1, 0), 14: (1, 0)}
    x = y = 0
    for btn, vec in mapping.items():
        if btn < js.get_numbuttons() and js.get_button(btn):
            dx, dy = vec
            x += dx;  y += dy
    return (max(-1, min(1, x)), max(-1, min(1, y)))

def try_realtime(prio=10):
    try:
        import ctypes, ctypes.util, os
        lib = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
        class P(ctypes.Structure): _fields_=[("sched_priority", ctypes.c_int)]
        if lib.sched_setscheduler(0, 1, ctypes.byref(P(prio))):
            raise OSError(ctypes.get_errno(), os.strerror(ctypes.get_errno()))
        print(f"◎ SCHED_FIFO priority {prio}")
    except Exception as e:
        print(f"ℹ Normal scheduling ({e})")

def dump_state(js, stats):
    cls();  banner(f"Controller : {js.get_name()}")
    if stats: print(stats, "\n")

    print("Axes:")
    for i in range(js.get_numaxes()):
        print(f"  axis {i:2d}: {dz(js.get_axis(i)):>+7.3f}")

    print("\nButtons:")
    for i in range(js.get_numbuttons()):
        print(f"  btn {i:2d}: {'◉' if js.get_button(i) else '○'}")

    print("\nD-pad:")
    print(f"  (x, y) = {read_hat(js)}")

def main():
    pygame.init();  pygame.joystick.init()
    if not pygame.joystick.get_count():
        sys.exit("No joystick detected.")
    js = pygame.joystick.Joystick(0);  js.init()
    print("▶ Detected:", js.get_name())

    loop_dt, last = [], time.perf_counter()
    next_ui   = last
    next_rep  = last + REPORT_SEC
    next_loop = last + PERIOD_S
    stats_line = ""                       # persist across frames

    try:
        while True:
            pygame.event.pump()           # update joystick state
            now = time.perf_counter()
            loop_dt.append(now - last);  last = now

            # timing stats
            if now >= next_rep:
                pmin = min(loop_dt)*1e3; pmax = max(loop_dt)*1e3
                pavg = statistics.mean(loop_dt)*1e3
                p50  = statistics.median(loop_dt)*1e3
                stats_line = (f"Timing  min {pmin:5.2f}  "
                              f"p50 {p50:5.2f}  avg {pavg:5.2f}  "
                              f"max {pmax:5.2f}  n={len(loop_dt)}  (ms)")
                loop_dt.clear();  next_rep += REPORT_SEC

            # dashboard redraw
            if now >= next_ui:
                dump_state(js, stats_line)
                next_ui += UI_S

            # wait until next 4 ms boundary
            next_loop += PERIOD_S
            slp = next_loop - time.perf_counter()
            if slp > 0:
                time.sleep(slp * 0.9)
                while time.perf_counter() < next_loop:
                    pass
            else:
                next_loop = time.perf_counter()
    except KeyboardInterrupt:
        print("\nBye!")

if __name__ == "__main__":
    try_realtime(RT_PRIO)
    main()
