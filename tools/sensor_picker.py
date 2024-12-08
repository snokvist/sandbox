#!/usr/bin/python3
import curses
import math

# Updated sensor modes based on user's last provided data
sensors = {
    "IMX415": [
        {"resolution": (3248, 1828), "fps": 60},  # Index 2
        {"resolution": (1920, 1080), "fps": 90}, # Index 3
        {"resolution": (1920, 816),  "fps": 120} # Index 4
    ],
    "IMX335": [
        {"resolution": (2560, 1920), "fps": 30},   # Index 0
        {"resolution": (2560, 1920), "fps": 70},   # Index 1
        {"resolution": (2560, 1440), "fps": 90},   # Index 2
        {"resolution": (2560, 1080), "fps": 120}   # Index 3
    ]
}

# Predefined aspect ratios
aspect_ratios = {
    "4:3": (4, 3),
    "16:9": (16, 9)
}

def draw_menu(stdscr, title, items, current_idx, chosen_values):
    stdscr.clear()
    h, w = stdscr.getmaxyx()

    required_lines = 1 + 1 + len(items) + 1
    if required_lines > h:
        stdscr.clear()
        msg = "Terminal too small to display menu. Resize terminal and press any key."
        stdscr.addstr(0, 0, msg[:w])
        stdscr.refresh()
        stdscr.getch()
        return

    summary_str = "Chosen: " + " ".join(f"{k}={v}" for k,v in chosen_values.items())
    stdscr.addstr(0, 0, summary_str[:w])

    stdscr.addstr(1, 0, title[:w], curses.A_BOLD)

    for idx, item in enumerate(items):
        y = 2 + idx
        line = str(item)
        if idx == current_idx:
            stdscr.attron(curses.color_pair(1))
            stdscr.addstr(y, 0, line[:w])
            stdscr.attroff(curses.color_pair(1))
        else:
            stdscr.addstr(y, 0, line[:w])

    instructions = "[UP/DOWN: Move][ENTER:Sel][q:Quit]"
    stdscr.addstr(2 + len(items), 0, instructions[:w])

    stdscr.refresh()

def get_choice(stdscr, title, choices, chosen_values):
    current_idx = 0
    while True:
        draw_menu(stdscr, title, choices, current_idx, chosen_values)
        key = stdscr.getch()

        if key == curses.KEY_UP and current_idx > 0:
            current_idx -= 1
        elif key == curses.KEY_DOWN and current_idx < len(choices)-1:
            current_idx += 1
        elif key == ord('q'):
            return None
        elif key in [curses.KEY_ENTER, 10, 13]:
            return choices[current_idx]

def calculate_aspect_fit(original_w, original_h, ar_w, ar_h):
    # Find largest ar_w:ar_h fitting inside original_w x original_h
    height_fit = original_h
    width_fit = int(height_fit * (ar_w / ar_h))
    if width_fit <= original_w:
        return width_fit, height_fit
    else:
        width_fit = original_w
        height_fit = int(width_fit * (ar_h / ar_w))
        return width_fit, height_fit

def generate_fps_choices(max_fps):
    increments = list(range(10, max_fps+1, 10))
    if increments and increments[-1] != max_fps:
        increments.append(max_fps)
    elif not increments:
        increments = [max_fps]
    increments.append("CUSTOM")
    return increments

def generate_resolution_choices(mode_res, aspect_ratio):
    original_w, original_h = mode_res
    ar_w, ar_h = aspect_ratio

    big_w, big_h = calculate_aspect_fit(original_w, original_h, ar_w, ar_h)
    small_w = int(big_w * 2/3)
    small_h = int(big_h * 2/3)
    return (big_w, big_h), (small_w, small_h)

def prompt_custom_fps(stdscr, max_fps):
    curses.echo()
    curses.curs_set(1)
    stdscr.clear()
    h, w = stdscr.getmaxyx()

    prompt = f"Enter custom FPS (1-{max_fps}): "
    stdscr.addstr(0,0, prompt[:w])
    stdscr.refresh()

    while True:
        user_input = stdscr.getstr(1,0, 10)
        try:
            val = int(user_input)
            if 1 <= val <= max_fps:
                curses.noecho()
                curses.curs_set(0)
                return val
            else:
                stdscr.clear()
                stdscr.addstr(0,0,"Invalid input. Must be an integer within range.")
                stdscr.addstr(1,0,prompt[:w])
                stdscr.refresh()
        except ValueError:
            stdscr.clear()
            stdscr.addstr(0,0,"Invalid input. Enter a whole number.")
            stdscr.addstr(1,0,prompt[:w])
            stdscr.refresh()

def align_to_4(value):
    # Ensure value is divisible by 4, rounding down
    adjusted = value - (value % 4)
    if adjusted == 0:
        adjusted = 4
    return adjusted

def main(stdscr):
    curses.curs_set(0)
    curses.init_pair(1, curses.COLOR_BLACK, curses.COLOR_WHITE)

    chosen_values = {}

    # Step 1: Choose Sensor
    sensor_name = get_choice(stdscr, "Choose Sensor", list(sensors.keys()), chosen_values)
    if sensor_name is None:
        return
    chosen_values["Sensor"] = sensor_name

    # Step 2: Choose Sensor Index (mode)
    sensor_modes = sensors[sensor_name]
    mode_choices = []
    for i, m in enumerate(sensor_modes):
        w, h = m["resolution"]
        fps = m["fps"]
        mode_choices.append(f"Index {i}: {w}x{h}@{fps}fps")
    chosen_mode_str = get_choice(stdscr, "Choose Sensor Mode", mode_choices, chosen_values)
    if chosen_mode_str is None:
        return
    chosen_index = int(chosen_mode_str.split(':')[0].replace("Index ", ""))
    chosen_mode = sensor_modes[chosen_index]
    chosen_values["Mode_Index"] = chosen_index

    # Step 3: Choose Actual FPS
    max_fps = chosen_mode["fps"]
    fps_choices = generate_fps_choices(max_fps)
    chosen_fps = get_choice(stdscr, f"FPS (1-{max_fps}, increments of 10 or CUSTOM)", fps_choices, chosen_values)
    if chosen_fps is None:
        return
    if chosen_fps == "CUSTOM":
        chosen_fps = prompt_custom_fps(stdscr, max_fps)
    chosen_values["FPS"] = chosen_fps

    # Step 4: Choose Aspect Ratio
    ar_choices = ["4:3", "16:9", "Sensor AR"]
    chosen_ar = get_choice(stdscr, "Aspect Ratio", ar_choices, chosen_values)
    if chosen_ar is None:
        return
    chosen_values["Aspect_Ratio"] = chosen_ar

    orig_w, orig_h = chosen_mode["resolution"]
    if chosen_ar == "4:3":
        ar_w, ar_h = aspect_ratios["4:3"]
    elif chosen_ar == "16:9":
        ar_w, ar_h = aspect_ratios["16:9"]
    else:
        # Sensor AR
        ar_w, ar_h = orig_w, orig_h

    # Step 5: Choose High or Low resolution
    (big_w, big_h), (small_w, small_h) = generate_resolution_choices((orig_w, orig_h), (ar_w, ar_h))

    # Prepare possible resolutions
    possible_res = [(big_w, big_h), (small_w, small_h)]

    # Always include 1920x1080 and 1280x720 if missing
    forced_resolutions = [(1920,1080), (1280,720)]
    adjusted_forced = []
    for frw, frh in forced_resolutions:
        # Adjust them to chosen aspect ratio
        adj_w, adj_h = calculate_aspect_fit(frw, frh, ar_w, ar_h)
        if (adj_w, adj_h) not in possible_res:
            possible_res.append((adj_w, adj_h))

    # Create menu strings
    resolution_choices = [f"{rw}x{rh}" for (rw, rh) in possible_res]

    chosen_res_str = get_choice(stdscr, "Resolution", resolution_choices, chosen_values)
    if chosen_res_str is None:
        return

    # Parse chosen resolution
    final_w, final_h = map(int, chosen_res_str.split('x'))

    # Align final dimensions to 4
    final_w = align_to_4(final_w)
    final_h = align_to_4(final_h)
    chosen_values["Resolution"] = (final_w, final_h)

    mode_fps = chosen_mode["fps"]
    exposure = math.floor((1/float(mode_fps))*1000) - 1
    if exposure < 0:
        exposure = 0

    video_size_str = f"{final_w}x{final_h}"
    index_fps = mode_fps
    online_fps = chosen_fps

    # Offset = (orig_w - final_w)/2, aligned to 4
    final_offset = (orig_w - final_w) // 2
    final_offset = final_offset - (final_offset % 4)
    if final_offset < 0:
        final_offset = 0

    commands = [
        f"yaml-cli -s .isp.exposure {exposure}",
        f"yaml-cli -s .video0.size {video_size_str}",
        f"yaml-cli -s .video0.fps {index_fps}",
        "sleep 0.5",
        "/etc/init.d/S95majestic restart",
        "sleep 0.5",
        f"echo setfps 0 {online_fps} > /proc/mi_modules/mi_sensor/mi_sensor0"
    ]

    # If final res == original res, no precrop needed
    if (final_w, final_h) != (orig_w, orig_h):
        precrop_w = align_to_4(big_w)
        precrop_h = align_to_4(big_h)

        precrop_cmd = (f"echo setprecrop 0 0 {final_offset} 0 "
                       f"{precrop_w} {precrop_h} > /proc/mi_modules/mi_vpe/mi_vpe0")
        commands.append(precrop_cmd)

    stdscr.clear()
    h, w = stdscr.getmaxyx()

    lines = [
        f"Sensor: {chosen_values['Sensor']}",
        f"Index: {chosen_values['Mode_Index']} ({orig_w}x{orig_h}@{index_fps}fps)",
        f"FPS chosen: {chosen_values['FPS']}",
        f"AR: {chosen_values['Aspect_Ratio']}",
        f"Res: {final_w}x{final_h}",
        "",
        "Commands:"
    ] + commands + ["", "Press any key to exit."]

    for idx, line in enumerate(lines):
        stdscr.addstr(idx, 0, line[:w])

    stdscr.refresh()
    stdscr.getch()

if __name__ == "__main__":
    curses.wrapper(main)
