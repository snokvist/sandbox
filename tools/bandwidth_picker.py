#!/usr/bin/python3
import math
from rich.console import Console
from rich.table import Table
from rich.text import Text
from rich import box
from rich.prompt import Prompt

console = Console()

# Updated bandwidth sets and rates
ALL_BW_ORDERED = [5,10,20,40,80]

RATES = {
    5: {
        "long":  [1.5,  3.0,  4.5,  6.0,  9.0, 12.0, 13.5, 15.0],
        "short": [1.7,  3.4,  5.1,  6.8, 10.2,13.6, 15.0, 16.7]
    },
    10: {
        "long":  [3.0,  6.0,  9.0, 12.0,18.0, 24.0, 27.0, 30.0],
        "short": [3.3,  6.8,10.2,13.6,20.4,27.2,30.0,33.4]
    },
    20: {
        "long":  [6.5, 13.0,19.5,26.0,39.0,52.0,58.5,65.0],
        "short": [7.2, 14.4,21.7,28.9,43.3,57.8,65.0,72.2]
    },
    40: {
        "long":  [13.5,27.0,40.5,54.0,81.0,108.0,121.5,135.0],
        "short": [15.0,30.0,45.0,60.0,90.0,120.0,135.0,150.0]
    },
    80: {
        "long":  [27.0,54.0,81.0,108.0,162.0,216.0,243.0,270.0],
        "short": [30.0,60.0,90.0,120.0,180.0,240.0,270.0,300.0]
    }
}

DEFAULT_MCS_RANGE = (0,7)
DEFAULT_BW = [20]
# Only one FEC rate by default now:
DEFAULT_FEC = [(8,12)]
DEFAULT_GI = ["long"]
DEFAULT_OVERHEAD = 0.25

ALL_BW = [5,10,20,40,80]
ALL_GI = ["long","short"]
# Not changed here, but available if reset is called:
ALL_FEC = [(1,1),(1,2),(8,12),(10,12)]

CURRENT_MCS_RANGE = DEFAULT_MCS_RANGE
CURRENT_BW = DEFAULT_BW[:]
CURRENT_FEC = DEFAULT_FEC[:]
CURRENT_GI = DEFAULT_GI[:]
CURRENT_OVERHEAD = DEFAULT_OVERHEAD
CURRENT_SORT_ORDER = None
CURRENT_SORT_KEY = None
CURRENT_DATARATE_FILTER = None

reference_config = None

def calculate_rates(mcs, bw, fec_ratio, gi, overhead_ratio):
    if bw in RATES and gi in RATES[bw]:
        base_rate = RATES[bw][gi][mcs]
    else:
        base_rate = 0.0

    data_factor = base_rate
    unusable = data_factor * overhead_ratio
    usable = data_factor - unusable

    data_pkts, total_pkts = fec_ratio
    ratio = data_pkts / total_pkts if total_pkts > 0 else 1.0
    # If data_pkts==total_pkts, ratio=1 (no overhead)

    if total_pkts > 0:
        fec_overhead_ratio = (total_pkts - data_pkts) / total_pkts
    else:
        fec_overhead_ratio = 0.0

    # If data_pkts == total_pkts (e.g. 1/1), no FEC overhead
    if total_pkts == data_pkts:
        fec_overhead_ratio = 0.0

    fec_overhead = usable * fec_overhead_ratio
    app_data = usable - fec_overhead
    return data_factor, unusable, fec_overhead, app_data

def mcs_score(mcs):
    # Custom MCS scoring
    # mcs0=10, mcs1=9, mcs2=8, mcs3=5, mcs4=4, mcs5=3, mcs6=2, mcs7=1
    MCS_SCORES = {0:10,1:9,2:8,3:5,4:4,5:3,6:2,7:1}
    return MCS_SCORES.get(mcs,1)

def bw_score(bw):
    # 5MHz=+4,10MHz=+2,20MHz=0,40MHz=-2,80MHz=-4
    if bw == 5:
        return 4
    elif bw == 10:
        return 2
    elif bw == 20:
        return 0
    elif bw == 40:
        return -2
    elif bw == 80:
        return -4
    return 0

def gi_score(gi):
    # long GI = +1, short GI = 0
    return 1 if gi == "long" else 0

def fec_range_score(fec):
    data_pkts, total_pkts = fec
    if total_pkts == 0:
        # no real scenario, but handle gracefully
        return 0
    ratio = data_pkts/total_pkts
    # ratio=1 =>0 points, ratio=0.5 =>3 points, linear between
    # if ratio<0.5, cap at 3 points (max)
    if ratio >= 1.0:
        return 0
    elif ratio <= 0.5:
        return 3
    else:
        # between 0.5 and 1.0 linearly from 3 to 0
        # fec_benefit = round(6*(1-ratio))
        # ratio=0.75 => 6*(0.25)=1.5 ~2 points
        benefit = round(6*(1-ratio))
        # ensure it does not exceed 3 or go below 0
        if benefit > 3:
            benefit = 3
        if benefit < 0:
            benefit = 0
        return benefit

def calculate_range_score(mcs, bw, gi, fec):
    # sum mcs_score + bw_score + gi_score + fec_range_score
    return mcs_score(mcs) + bw_score(bw) + gi_score(gi) + fec_range_score(fec)

def draw_bar(data_factor, unusable, fec_overhead, app_data, max_data_factor, highlight=False):
    width = 30
    if max_data_factor <= 0:
        max_data_factor = 1e-9
    scale = data_factor / max_data_factor
    bar_length = int(width * scale)
    bar_length = max(bar_length, 1)

    if data_factor <= 0:
        return Text("[red]No Rate[/]")

    unusable_prop = unusable / data_factor if data_factor > 0 else 0
    fec_prop = fec_overhead / data_factor if data_factor > 0 else 0
    unusable_len = int(round(unusable_prop * bar_length))
    fec_len = int(round(fec_prop * bar_length))
    app_len = bar_length - unusable_len - fec_len

    if highlight:
        app_color = "bold green"
        fec_color = "bold blue"
        unusable_color = "bold red"
    else:
        app_color = "green"
        fec_color = "blue"
        unusable_color = "red"

    if app_len < 0: app_len = 0
    if fec_len < 0: fec_len = 0
    if unusable_len < 0: unusable_len = 0

    app_segment = Text("█" * app_len, style=app_color)
    fec_segment = Text("█" * fec_len, style=fec_color)
    unusable_segment = Text("█" * unusable_len, style=unusable_color)

    bar = app_segment + fec_segment + unusable_segment
    return bar

def feature_requirements(bw):
    reqs = []
    if bw == 80:
        reqs.append("VHT required")
    return ", ".join(reqs) if reqs else "None"

def get_configurations(mcs_range, bws, fecs, gis, overhead_ratio):
    mcs_start, mcs_end = mcs_range
    configs = []
    for mcs in range(mcs_start, mcs_end+1):
        for bw in bws:
            for gi in gis:
                for fec in fecs:
                    configs.append((mcs, bw, fec, gi, overhead_ratio))
    return configs

def apply_filters_and_sort(configs):
    filtered = []
    for cfg in configs:
        mcs, bw, fec, gi, oh = cfg
        df, unusable, foh, app_data = calculate_rates(mcs, bw, fec, gi, oh)

        # Datarate filter
        if CURRENT_DATARATE_FILTER is not None:
            min_mbps, max_mbps = CURRENT_DATARATE_FILTER
            if app_data < min_mbps:
                continue
            if max_mbps is not None and app_data > max_mbps:
                continue

        filtered.append((cfg, df, unusable, foh, app_data))

    if CURRENT_SORT_ORDER is not None and CURRENT_SORT_KEY is not None:
        reverse = (CURRENT_SORT_ORDER == "desc")

        def sort_key_func(item):
            cfg, df, unusable, foh, app_data = item
            mcs, bw, fec, gi, oh = cfg
            if CURRENT_SORT_KEY == "mcs":
                return mcs
            elif CURRENT_SORT_KEY == "bw":
                return bw
            elif CURRENT_SORT_KEY == "app":
                return app_data
            elif CURRENT_SORT_KEY == "fec":
                return foh
            elif CURRENT_SORT_KEY == "total":
                return df
            elif CURRENT_SORT_KEY == "range":
                return calculate_range_score(mcs, bw, gi, fec)
            else:
                return app_data

        filtered.sort(key=sort_key_func, reverse=reverse)

    return [f[0] for f in filtered]

def display_configurations(configs, reference_cfg=None):
    all_cfgs = configs[:]
    if reference_cfg is not None:
        if reference_cfg not in all_cfgs:
            all_cfgs.append(reference_cfg)

    if not all_cfgs and reference_cfg is None:
        console.print("[red]No configurations to display.[/]")
        return

    details_map = {}
    for cfg in all_cfgs:
        mcs, bw, fec, gi, oh = cfg
        df, unusable, foh, app_data = calculate_rates(mcs, bw, fec, gi, oh)
        details_map[cfg] = (df, unusable, foh, app_data)

    max_data_factor = max(details_map[cfg][0] for cfg in all_cfgs) if all_cfgs else 1.0

    table = Table(title="UDP Link Configurations", box=box.HEAVY, show_lines=True)
    table.add_column("Index", justify="right")
    table.add_column("MCS", justify="right")
    table.add_column("BW (MHz)", justify="right")
    table.add_column("GI", justify="center")
    table.add_column("FEC (data/total)", justify="center")
    table.add_column("Overhead (%)", justify="right")
    table.add_column("Total Rate (Mbps)", justify="right")
    table.add_column("App Rate (Mbps)", justify="right")
    table.add_column("Features", justify="center")
    table.add_column("Range Score", justify="right")
    table.add_column("Bar (App|FEC|Unusable)", justify="left")

    for i, cfg in enumerate(configs):
        (mcs, bw, fec, gi, oh) = cfg
        (df, unusable, foh, app_data) = details_map[cfg]
        bar = draw_bar(df, unusable, foh, app_data, max_data_factor, highlight=False)
        feat_req = feature_requirements(bw)
        fec_str = f"{fec[0]}/{fec[1]}"
        oh_str = f"{oh*100:.0f}%"
        range_val = calculate_range_score(mcs, bw, gi, fec)
        table.add_row(
            str(i),
            str(mcs),
            str(bw),
            gi,
            fec_str,
            oh_str,
            f"{df:.2f}",
            f"{app_data:.2f}",
            feat_req,
            str(range_val),
            bar,
        )

    # Add reference row if any
    if reference_cfg is not None:
        (mcs, bw, fec, gi, oh) = reference_cfg
        (df, unusable, foh, app_data) = details_map[reference_cfg]
        bar = draw_bar(df, unusable, foh, app_data, max_data_factor, highlight=True)
        fec_str = f"{fec[0]}/{fec[1]}"
        oh_str = f"{oh*100:.0f}%"
        range_val = calculate_range_score(mcs, bw, gi, fec)

        # Insert a blank row for clarity
        table.add_section()
        # Add reference row labeled "REF"
        table.add_row(
            "REF",
            str(mcs),
            str(bw),
            gi,
            fec_str,
            oh_str,
            f"{df:.2f}",
            f"{app_data:.2f}",
            feature_requirements(bw),
            str(range_val),
            bar,
            style="bold magenta"
        )

    console.print(table)

def reset_all():
    global CURRENT_MCS_RANGE, CURRENT_BW, CURRENT_FEC, CURRENT_GI, CURRENT_OVERHEAD
    global CURRENT_SORT_ORDER, CURRENT_SORT_KEY, CURRENT_DATARATE_FILTER, reference_config
    CURRENT_MCS_RANGE = (0,7)
    CURRENT_BW = ALL_BW[:]
    # On reset, you might revert to all possible, but user requested only one FEC at start.
    # If you want to keep one FEC on reset, comment the next line. Otherwise, revert to all:
    # CURRENT_FEC = ALL_FEC[:]
    # The user didn't say we can't revert to previous behavior on reset, but let's keep it simple.
    CURRENT_FEC = DEFAULT_FEC[:]  # revert to the single default FEC (8/12)
    CURRENT_GI = ALL_GI[:]
    CURRENT_OVERHEAD = 0.25
    CURRENT_SORT_ORDER = None
    CURRENT_SORT_KEY = None
    CURRENT_DATARATE_FILTER = None
    reference_config = None

def parse_datarate_filter(arg):
    if '-' in arg:
        parts = arg.split('-')
        if len(parts) == 2:
            low = float(parts[0])/1000.0
            high = float(parts[1])/1000.0
            return (low, high)
    else:
        val = float(arg)/1000.0
        return (val, None)
    return None

def parse_mcs_range(arg):
    if '-' in arg:
        start_str, end_str = arg.split('-')
        start = int(start_str)
        end = int(end_str)
    else:
        start = int(arg)
        end = start
    return (start, end)

def parse_bw_arguments(args):
    if len(args) == 1 and '-' in args[0]:
        rng = args[0]
        start_str, end_str = rng.split('-')
        start = int(start_str)
        end = int(end_str)
        return [b for b in ALL_BW_ORDERED if b >= start and b <= end]
    else:
        new_bws = []
        for p in args:
            bw_val = int(p)
            if bw_val in ALL_BW_ORDERED:
                new_bws.append(bw_val)
        return new_bws

def display_current_settings():
    mcs_str = f"MCS: {CURRENT_MCS_RANGE[0]}-{CURRENT_MCS_RANGE[1]}"
    bw_str = f"BW: {','.join(str(b) for b in CURRENT_BW)}"
    fec_str = "FEC: " + ",".join(f"{d}/{t}" for (d,t) in CURRENT_FEC)
    gi_str = "GI: " + ",".join(CURRENT_GI)
    oh_str = f"Overhead: {CURRENT_OVERHEAD*100:.0f}%"
    sort_str = "Sort: "
    if CURRENT_SORT_ORDER and CURRENT_SORT_KEY:
        sort_str += f"{CURRENT_SORT_ORDER} {CURRENT_SORT_KEY}"
    else:
        sort_str += "None"
    dr_filter_str = "Datarate filter: "
    if CURRENT_DATARATE_FILTER:
        min_mbps, max_mbps = CURRENT_DATARATE_FILTER
        if max_mbps is None:
            dr_filter_str += f">={min_mbps} Mbps"
        else:
            dr_filter_str += f"{min_mbps}-{max_mbps} Mbps"
    else:
        dr_filter_str += "None"

    console.print(f"[bold cyan]{mcs_str} | {bw_str} | {fec_str} | {gi_str} | {oh_str} | {sort_str} | {dr_filter_str}[/]")

def display_commands_legend():
    console.print("[bold magenta]Commands:[/]")
    console.print("- mcs X or mcs X-Y: set MCS range")
    console.print("- bw X [X ...] or bw X-Y: set BW. E.g. bw 5-40 => [5,10,20,40]")
    console.print("- fec X/Y [X/Y ...]: set multiple FEC ratios (e.g., fec 1/1 1/2 8/12)")
    console.print("- gi long|short [long|short ...]: set GIs")
    console.print("- overhead P: set overhead percentage (0-100)")
    console.print("- sort asc|desc [mcs|bw|app|fec|total|range]: e.g., sort asc total")
    console.print("- datarate N or N-M: filter by app rate range in Kbps")
    console.print("- s <index>: select an entry as reference")
    console.print("- reset: show default combinations")
    console.print("- q: quit")

def main():
    global CURRENT_BW, CURRENT_FEC, CURRENT_MCS_RANGE, CURRENT_SORT_ORDER, CURRENT_SORT_KEY, CURRENT_GI, CURRENT_OVERHEAD
    global CURRENT_DATARATE_FILTER, reference_config

    # Initialize defaults
    # Only one FEC rate (8/12) as requested
    CURRENT_MCS_RANGE = DEFAULT_MCS_RANGE
    CURRENT_BW = DEFAULT_BW[:]
    CURRENT_FEC = DEFAULT_FEC[:] # Only (8,12)
    CURRENT_GI = DEFAULT_GI[:]
    CURRENT_OVERHEAD = DEFAULT_OVERHEAD
    CURRENT_SORT_ORDER = None
    CURRENT_SORT_KEY = None
    CURRENT_DATARATE_FILTER = None
    reference_config = None

    while True:
        console.clear()
        configs = get_configurations(CURRENT_MCS_RANGE, CURRENT_BW, CURRENT_FEC, CURRENT_GI, CURRENT_OVERHEAD)
        configs = apply_filters_and_sort(configs)

        display_configurations(configs, reference_cfg=reference_config)
        display_current_settings()
        display_commands_legend()

        cmd = Prompt.ask("Enter command")
        if cmd == "q":
            break

        elif cmd.startswith("mcs "):
            try:
                arg = cmd.split(" ",1)[1]
                CURRENT_MCS_RANGE = parse_mcs_range(arg)
            except:
                console.print("[red]Invalid MCS command[/]")

        elif cmd.startswith("bw "):
            try:
                parts = cmd.split()[1:]
                new_bws = parse_bw_arguments(parts)
                if new_bws:
                    CURRENT_BW = new_bws
                else:
                    console.print("[red]No valid bandwidth specified.[/]")
            except:
                console.print("[red]Invalid command[/]")

        elif cmd.startswith("fec "):
            try:
                parts = cmd.split()[1:]
                new_fec = []
                for ratio_str in parts:
                    d,t = ratio_str.split("/")
                    d = int(d)
                    t = int(t)
                    if t > 0 and d <= t:
                        new_fec.append((d,t))
                    else:
                        console.print("[red]Invalid FEC ratio[/]")
                if new_fec:
                    CURRENT_FEC = new_fec
                else:
                    console.print("[red]No valid FEC ratio provided[/]")
            except:
                console.print("[red]Invalid FEC format.[/]")

        elif cmd.startswith("gi "):
            try:
                parts = cmd.split()[1:]
                new_gis = []
                for g in parts:
                    g = g.lower()
                    if g in ["long","short"]:
                        new_gis.append(g)
                if new_gis:
                    CURRENT_GI = new_gis
                else:
                    console.print("[red]No valid GI specified.[/]")
            except:
                console.print("[red]Invalid command[/]")

        elif cmd.startswith("overhead "):
            try:
                oh_str = cmd.split(" ",1)[1]
                oh_val = float(oh_str)
                if 0 <= oh_val <= 100:
                    CURRENT_OVERHEAD = oh_val / 100.0
                else:
                    console.print("[red]Overhead must be between 0 and 100[/]")
            except:
                console.print("[red]Invalid overhead value[/]")

        elif cmd.startswith("sort "):
            try:
                parts = cmd.split()
                if len(parts) == 3:
                    order = parts[1].lower()
                    key = parts[2].lower()
                    if order in ["asc","desc"] and key in ["mcs","bw","app","fec","total","range"]:
                        CURRENT_SORT_ORDER = order
                        CURRENT_SORT_KEY = key
                    else:
                        console.print("[red]Invalid sort parameters.[/]")
                else:
                    console.print("[red]Sort command requires two arguments: order and key[/]")
            except:
                console.print("[red]Invalid command[/]")

        elif cmd.startswith("datarate "):
            try:
                arg = cmd.split(" ",1)[1]
                dr_filter = parse_datarate_filter(arg)
                if dr_filter is not None:
                    CURRENT_DATARATE_FILTER = dr_filter
                else:
                    console.print("[red]Invalid datarate value[/]")
            except:
                console.print("[red]Invalid datarate value[/]")

        elif cmd.startswith("s "):
            try:
                idx = int(cmd.split()[1])
                current_list = get_configurations(CURRENT_MCS_RANGE, CURRENT_BW, CURRENT_FEC, CURRENT_GI, CURRENT_OVERHEAD)
                current_list = apply_filters_and_sort(current_list)
                if 0 <= idx < len(current_list):
                    reference_config = current_list[idx]
                else:
                    console.print("[red]Invalid index[/]")
            except:
                console.print("[red]Invalid command[/]")

        elif cmd == "reset":
            reset_all()

        # Otherwise, unrecognized command.

    console.print("Goodbye!")


if __name__ == "__main__":
    main()
