"""
LilyGo T-Display S3 display hand-setup. Not normally required if using https://circuitpython.org/board/lilygo_tdisplay_s3/
"""
import time
import board
import digitalio
import displayio
import paralleldisplaybus
import adafruit_st7789
from adafruit_display_text import label
from adafruit_bitmap_font import bitmap_font

# Release any resources currently in use for the displays
displayio.release_displays()

# Initialize display power
lcd_power = digitalio.DigitalInOut(board.LCD_POWER_ON)   # IO15
lcd_power.switch_to_output(value=True)

# Setup parallel bus
display_bus = paralleldisplaybus.ParallelBus(
    # IO39,40,41,42,43,44,45,46,47,48
    data_pins = (board.LCD_D0, board.LCD_D1, board.LCD_D2, board.LCD_D3,
                 board.LCD_D4, board.LCD_D5, board.LCD_D6, board.LCD_D7),
    command = board.LCD_DC,      # IO7,
    chip_select = board.LCD_CS,  # IO6,
    write = board.LCD_WR,        # IO8,
    read = board.LCD_RD,         # IO9,
    reset = board.LCD_RST,       # IO5,
    frequency = 15_000_000,
)

# Initialize display
display = adafruit_st7789.ST7789(display_bus, width=320, height=170, rotation=270, colstart=35)

# Create a main display group
main_group = displayio.Group()

# Add color swatch on the right
color_width = 50
color_height = 170
color_bitmap = displayio.Bitmap(color_width, color_height, 1)  # 1 color
color_palette = displayio.Palette(1)
color_palette[0] = 0xFF0000  # Red color

# Create a TileGrid for the color swatch
color_tilegrid = displayio.TileGrid(color_bitmap, pixel_shader=color_palette, x=270, y=0)

# Add the color swatch to the main group
main_group.append(color_tilegrid)

# Add text output on the left using LeagueSpartan_Bold_16.bdf
font = bitmap_font.load_font("/fonts/LeagueSpartan_Bold_16.bdf")  # Ensure this font file is available
text_area = label.Label(font, text="Initializing...", color=0xFFFFFF, x=10, y=10)
main_group.append(text_area)

# Add FPS counter in the upper-right corner
fps_label = label.Label(font, text="FPS: 0", color=0xFFFFFF, x=230, y=10)
main_group.append(fps_label)

# Assign the main group to the display's root_group
display.root_group = main_group

# Main loop
counter = 0
last_time = time.monotonic()
frame_count = 0

while True:
    # Update color for the swatch and text
    color_palette[0] = (color_palette[0] + 0x010101) & 0xFFFFFF  # Increment color
    text_area.color = color_palette[0]  # Update text color

    # Update the text with a counter
    text_area.text = f"Counter: {counter}\nColor: #{color_palette[0]:06X}"
    counter += 1

    # Calculate FPS
    current_time = time.monotonic()
    frame_count += 1
    elapsed_time = current_time - last_time
    if elapsed_time >= 1.0:  # Update FPS every second
        fps = frame_count / elapsed_time
        fps_label.text = f"FPS: {fps:.1f}"
        frame_count = 0
        last_time = current_time

    # Print serial output for debugging
    print(f"Counter: {counter}, Current color: #{color_palette[0]:06X}, FPS: {fps_label.text}")

    time.sleep(0.016)  # Approximate frame time for ~60 FPS
