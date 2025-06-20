#!/usr/bin/env python3
"""
ELRS Configurator – now with ENUM‐string lookup

● Auto-reconnect UART every 3 s
● Scans all CRSF parameters (indices 0-63 by default)
● Reads *current* value via PARAMETER_READ (0x2B)
● Fetches option strings for every ENUM (type 7) → human-readable labels
● Lets you filter + edit any type:
      0 UINT8   1 B
      1 INT8
      2 UINT16  2 B LE
      3 INT16
      4 UINT32  4 B LE
      5 INT32
      6 FLOAT   4 B IEEE-754 LE
      7 ENUM    choose label from list   (string table auto-downloaded)
      8 STRING  free-form ≤15 B
● SAVE / REBOOT commands
"""

import struct
import sys
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

import serial
from prompt_toolkit import PromptSession
from prompt_toolkit.patch_stdout import patch_stdout
from prompt_toolkit.shortcuts import button_dialog

# ---------- user-tunable ----------
DEVICE   = "/dev/ttyUSB0"
BAUD     = 420_000
RETRY_S  = 3.0          # UART reconnect interval
MAX_IDX  = 64           # how many indices to probe (0-63 covers ELRS TX)
# ----------------------------------

ADDR_TX  = 0xEE
POLY     = 0xD5

# CRSF frame types
T_DEVICE_PING              = 0x28
T_PARAM_SETTINGS_ENTRY     = 0x2A
T_PARAM_READ               = 0x2B
T_PARAM_WRITE              = 0x2C
T_COMMAND                  = 0x2D
T_PARAM_STATUS             = 0x2E

CMD_SAVE_SETTINGS = 0x02
CMD_REBOOT        = 0x03

# Parameter type → (label, size)
PTYPES: Dict[int, Tuple[str, int]] = {
    0: ("UINT8",   1), 1: ("INT8",    1),
    2: ("UINT16",  2), 3: ("INT16",   2),
    4: ("UINT32",  4), 5: ("INT32",   4),
    6: ("FLOAT",   4),
    7: ("ENUM",    1),
    8: ("STRING", -1),          # variable length (max 16 incl. NUL)
}

# ---------------------------------------------------------------- CRC-8
def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ POLY) if crc & 0x80 else (crc << 1)
            crc &= 0xFF
    return crc & 0xFF

# ---------------------------------------------------------------- UART with auto-reconnect
class SerialManager:
    def __init__(self, dev: str, baud: int, retry: float):
        self.dev, self.baud, self.retry = dev, baud, retry
        self.ser: Optional[serial.Serial] = None
        self.last_try = 0.0

    def ensure(self) -> bool:
        """Make sure UART is open; attempt every self.retry seconds."""
        if self.ser and self.ser.is_open:
            return True
        if time.time() - self.last_try < self.retry:
            return False
        self.last_try = time.time()
        try:
            self.ser = serial.Serial(self.dev, self.baud, timeout=0.1)
            print(f"[INFO] Connected {self.dev}")
            return True
        except (serial.SerialException, FileNotFoundError) as e:
            print(f"[WARN] {e} – retrying")
            return False

    def write(self, data: bytes):
        if self.ensure():
            try:
                self.ser.write(data)
            except serial.SerialException:
                self.ser.close()

    def read(self, n: int = 128) -> bytes:
        if not self.ensure():
            return b""
        try:
            return self.ser.read(n)
        except serial.SerialException:
            self.ser.close()
            return b""

# ---------------------------------------------------------------- CRSF helpers
def mk_frame(ftype: int, payload: bytes) -> bytes:
    hdr = bytes([ADDR_TX, ftype, len(payload)]) + payload
    return hdr + bytes([crc8(hdr[1:])])

def parse_frames(buf: bytearray) -> List[Tuple[int, bytes]]:
    out = []
    while len(buf) >= 5:
        if buf[0] != ADDR_TX:
            buf.pop(0)
            continue
        ftype, ln = buf[1], buf[2]
        need = 3 + ln + 1
        if len(buf) < need:
            break
        payload, crc = buf[3:3+ln], buf[3+ln]
        if crc8(buf[1:3+ln]) == crc:
            out.append((ftype, bytes(payload)))
        buf[:need] = b""
    return out

# ---------------------------------------------------------------- value decode/encode
def decode_value(ptype: int, data: bytes):
    if ptype == 0:   return data[0]
    if ptype == 1:   return struct.unpack("<b", data[:1])[0]
    if ptype == 2:   return struct.unpack("<H", data[:2])[0]
    if ptype == 3:   return struct.unpack("<h", data[:2])[0]
    if ptype == 4:   return struct.unpack("<I", data[:4])[0]
    if ptype == 5:   return struct.unpack("<i", data[:4])[0]
    if ptype == 6:   return struct.unpack("<f", data[:4])[0]
    if ptype == 7:   return data[0]          # enum index
    if ptype == 8:   return data.split(b"\x00")[0].decode(errors="ignore")
    return None

def encode_value(ptype: int, value_str: str) -> bytes:
    if ptype in (0, 7):  return struct.pack("<B", int(value_str) & 0xFF)
    if ptype == 1:       return struct.pack("<b", int(value_str))
    if ptype == 2:       return struct.pack("<H", int(value_str) & 0xFFFF)
    if ptype == 3:       return struct.pack("<h", int(value_str) & 0xFFFF)
    if ptype == 4:       return struct.pack("<I", int(value_str) & 0xFFFFFFFF)
    if ptype == 5:       return struct.pack("<i", int(value_str) & 0xFFFFFFFF)
    if ptype == 6:       return struct.pack("<f", float(value_str))
    if ptype == 8:
        s = value_str.encode()[:15]
        return s + b"\x00" * (16 - len(s))
    raise ValueError("unsupported type")

# ---------------------------------------------------------------- Param model
@dataclass
class Param:
    idx: int
    name: str
    ptype: int
    value: Optional[object] = None    # decoded current value
    enum: List[str] = field(default_factory=list)  # labels for type 7

# ---------------------------------------------------------------- discovery helpers
def req_entry(conn: SerialManager, idx: int, opt: int = 0):
    conn.write(mk_frame(T_PARAM_SETTINGS_ENTRY, bytes([idx, opt])))

def req_read(conn: SerialManager, idx: int):
    conn.write(mk_frame(T_PARAM_READ, bytes([idx])))

def fetch_enum_strings(conn: SerialManager, idx: int, max_opts: int = 32) -> List[str]:
    strings: List[str] = []
    buf = bytearray()
    for opt in range(1, max_opts + 1):
        req_entry(conn, idx, opt)
        t_end = time.time() + 0.1
        got = False
        while time.time() < t_end:
            buf.extend(conn.read())
            for ftype, pl in parse_frames(buf):
                if ftype == T_PARAM_SETTINGS_ENTRY and pl[0] == idx and pl[1] == opt:
                    txt = pl[2:18].split(b"\x00")[0].decode(errors="ignore")
                    strings.append(txt)
                    got = True
                    break
            if got:
                break
        if not got:      # missing => table finished
            break
    return strings

def discover_params(conn: SerialManager) -> List[Param]:
    buf = bytearray()
    params: Dict[int, Param] = {}
    # --- meta pass ---
    for i in range(MAX_IDX):
        req_entry(conn, i, 0)
    t_end = time.time() + 1.2
    while time.time() < t_end:
        buf.extend(conn.read())
        for typ, pl in parse_frames(buf):
            if typ != T_PARAM_SETTINGS_ENTRY or len(pl) < 22:
                continue
            idx = pl[0]
            if pl[1] != 0:        # pl[1] non-zero means enum string – ignore in meta pass
                continue
            name = pl[1:17].split(b"\x00")[0].decode(errors="ignore")
            ptype = pl[17]
            params[idx] = Param(idx, name, ptype)
    # --- current values pass ---
    for idx in params:
        req_read(conn, idx)
    t_end = time.time() + 1.0
    while time.time() < t_end and any(p.value is None for p in params.values()):
        buf.extend(conn.read())
        for typ, pl in parse_frames(buf):
            if typ == T_PARAM_READ and pl:
                idx = pl[0]; ptype = pl[1]; val = decode_value(ptype, pl[2:])
                if idx in params:
                    params[idx].value = val
    # --- ENUM string tables ---
    for p in params.values():
        if p.ptype == 7:
            p.enum = fetch_enum_strings(conn, p.idx)
    return list(params.values())

# ---------------------------------------------------------------- writing
def write_param(conn: SerialManager, p: Param, raw: bytes):
    conn.write(mk_frame(T_PARAM_WRITE, bytes([p.idx, p.ptype, 0x00]) + raw))

# ---------------------------------------------------------------- TUI
def tui():
    conn = SerialManager(DEVICE, BAUD, RETRY_S)
    session = PromptSession()
    log: List[str] = []
    params: List[Param] = []

    def println(msg: str):
        log.append(msg)
        print("\n".join(log[-20:]))

    while True:
        with patch_stdout():
            action = button_dialog(
                title="ELRS Configurator",
                text="Choose action:",
                buttons=[
                    ("Scan / Refresh", "scan"),
                    ("Browse & Edit", "edit"),
                    ("Save settings", "save"),
                    ("Reboot TX", "reboot"),
                    ("Quit", "quit"),
                ],
            ).run()
        if action == "quit":
            break

        if action == "scan":
            println("[*] Scanning parameters …")
            params = discover_params(conn)
            println(f"[✓] {len(params)} parameters")
            for p in params:
                typ_lbl = PTYPES.get(p.ptype, ("?",))[0]
                if p.ptype == 7 and p.enum and isinstance(p.value, int) and p.value < len(p.enum):
                    val_str = f"{p.enum[p.value]} ({p.value})"
                else:
                    val_str = str(p.value)
                println(f"[{p.idx:02}] {p.name:<16} = {val_str} ({typ_lbl})")

        elif action == "edit":
            if not params:
                println("(!) Scan first.")
                continue
            flt = session.prompt("Filter substring (empty=all): ").lower()
            sel_list = [p for p in params if flt in p.name.lower()]
            if not sel_list:
                println("(!) No matches.")
                continue
            buttons = []
            for p in sel_list:
                display = p.enum[p.value] if p.ptype == 7 and p.enum and isinstance(p.value, int) and p.value < len(p.enum) else p.value
                buttons.append((f"[{p.idx}] {p.name} = {display}", p))
            target: Param = button_dialog(
                title="Select parameter",
                text="",
                buttons=buttons,
            ).run()
            if not target:
                continue
            typ_lbl = PTYPES.get(target.ptype, ("UNK",))[0]

            # ENUM → pull list
            if target.ptype == 7 and target.enum:
                choice = button_dialog(
                    title=f"{target.name} ({typ_lbl})",
                    text="Choose value:",
                    buttons=[(f"{i}: {lbl}", i) for i, lbl in enumerate(target.enum)],
                ).run()
                if choice is None:
                    continue
                raw = encode_value(7, str(choice))
            else:
                new_val = session.prompt(f"Enter new {typ_lbl} value: ")
                try:
                    raw = encode_value(target.ptype, new_val)
                except Exception as e:
                    println(f"(!) {e}")
                    continue
            write_param(conn, target, raw)
            # verify
            req_read(conn, target.idx)
            buf = bytearray()
            t0 = time.time()
            while time.time() - t0 < 0.3:
                buf.extend(conn.read())
                for typ, pl in parse_frames(buf):
                    if typ == T_PARAM_READ and pl[0] == target.idx:
                        target.value = decode_value(target.ptype, pl[2:])
                        break
                else:
                    continue
                break
            println(f"[✓] {target.name} now = {target.enum[target.value] if target.ptype==7 and target.enum and isinstance(target.value,int) and target.value<len(target.enum) else target.value}")

        elif action == "save":
            println("[*] Saving …")
            conn.write(mk_frame(T_COMMAND, bytes([CMD_SAVE_SETTINGS])))

        elif action == "reboot":
            println("[*] Rebooting TX …")
            conn.write(mk_frame(T_COMMAND, bytes([CMD_REBOOT])))

if __name__ == "__main__":
    try:
        tui()
    except KeyboardInterrupt:
        print("\nBye")
        sys.exit(0)
