#!/usr/bin/env python3  
import gi  
gi.require_version("Gst", "1.0")  
gi.require_version("GstVideo", "1.0")  
gi.require_version("Gtk", "3.0")  
gi.require_version("GdkX11", "3.0")  
from gi.repository import Gst, GstVideo, Gtk, Gdk, GObject, GLib  
  
import sys  
import argparse  
import signal  
from pynput import keyboard  
  
Gst.init(None)  
  
# Apply a CSS style to force drawing areas to have a black background.  
css = b"""  
drawingarea {  
  background-color: #000000;  
}  
"""  
provider = Gtk.CssProvider()  
provider.load_from_data(css)  
screen = Gdk.Screen.get_default()  
Gtk.StyleContext.add_provider_for_screen(screen, provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)  
  
class VideoOverlay(Gtk.Window):  
    def __init__(self, ports, screen_width, screen_height, initial_mode=1):  
        super().__init__(title="GStreamer Grid")  
        self.ports = ports  
        self.pipelines = {}   # Map port -> Gst.Pipeline  
        self.sinks = {}       # Map port -> (ximagesink element, drawing area)  
        # Default: show stream 1 (ports[0]) on startup.  
        self.active_streams = [ports[0]]  
        self.screen_width = screen_width  
        self.screen_height = screen_height  
  
        self.connect("destroy", self.on_destroy)  
        self.set_default_size(self.screen_width, self.screen_height)  
        self.fullscreen()  
  
        self.grid = Gtk.Fixed()  
        self.add(self.grid)  
  
        self._create_pipelines()  
        self._start_global_keyboard_listener()  
  
        # Delay embedding until the ximagesink windows are created.  
        GLib.timeout_add(500, self._embed_video_sinks)  
  
    def _create_pipelines(self):  
        """Creates pipelines for each UDP stream using ximagesink with minimized buffering."""  
        for i, port in enumerate(self.ports):  
            pipeline_str = f"""  
                udpsrc port={port} !  
                queue max-size-time=0 max-size-buffers=0 max-size-bytes=0 !  
                application/x-rtp, payload=97, clock-rate=90000, encoding-name=H265 !  
                rtpjitterbuffer latency=1 !  
                rtph265depay !  
                vaapih265dec !  
                videoscale ! videoconvert !  
                ximagesink name=sink_{i} sync=true  
            """  
            pipeline = Gst.parse_launch(pipeline_str)  
            sink = pipeline.get_by_name(f"sink_{i}")  
  
            drawing_area = Gtk.DrawingArea()  
            drawing_area.set_size_request(640, 360)  # initial placeholder size  
            self.grid.put(drawing_area, 0, 0)  
  
            self.pipelines[port] = pipeline  
            self.sinks[port] = (sink, drawing_area)  
        self.show_all()  
  
    def _embed_video_sinks(self):  
        """Embeds ximagesink output into the GTK DrawingAreas using GstVideoOverlay.  
        Once embedded, update the display layout."""  
        for port, (sink, drawing_area) in self.sinks.items():  
            window = drawing_area.get_window()  
            if window:  
                xid = window.get_xid()  
                print(f"[INFO] Embedding sink for port {port} into drawing area (xid: {xid})")  
                GstVideo.VideoOverlay.set_window_handle(sink, xid)  
        # Now that sinks are embedded, update the display.  
        self._update_display()  
        return False  # Run only once  
  
    def _get_layout_positions(self, num_active):  
        """Calculates positions for active streams:  
           1 stream: full screen.  
           2 streams: side-by-side.  
           3 or 4 streams: 2×2 grid.  
        """  
        positions = {}  
        if num_active == 1:  
            positions[0] = (self.screen_width, self.screen_height, 0, 0)  
        elif num_active == 2:  
            positions[0] = (self.screen_width // 2, self.screen_height, 0, 0)  
            positions[1] = (self.screen_width // 2, self.screen_height, self.screen_width // 2, 0)  
        elif num_active == 3:  
            positions[0] = (self.screen_width // 2, self.screen_height // 2, 0, 0)  
            positions[1] = (self.screen_width // 2, self.screen_height // 2, self.screen_width // 2, 0)  
            positions[2] = (self.screen_width // 2, self.screen_height // 2, self.screen_width // 4, self.screen_height // 2)  
        else:  
            positions[0] = (self.screen_width // 2, self.screen_height // 2, 0, 0)  
            positions[1] = (self.screen_width // 2, self.screen_height // 2, self.screen_width // 2, 0)  
            positions[2] = (self.screen_width // 2, self.screen_height // 2, 0, self.screen_height // 2)  
            positions[3] = (self.screen_width // 2, self.screen_height // 2, self.screen_width // 2, self.screen_height // 2)  
        return positions  
  
    def _update_display(self):  
        """Updates the grid layout based on the active streams.  
        For streams not active, their DrawingAreas are hidden, ensuring no artifacts remain.  
        """  
        num_active = len(self.active_streams)  
        if num_active == 0:  
            self.active_streams = self.ports[:]  
            num_active = len(self.active_streams)  
        positions = self._get_layout_positions(num_active)  
        for i, port in enumerate(self.ports):  
            _, drawing_area = self.sinks[port]  
            if port in self.active_streams:  
                new_width, new_height, offset_x, offset_y = positions[self.active_streams.index(port)]  
                drawing_area.set_size_request(new_width, new_height)  
                self.grid.move(drawing_area, offset_x, offset_y)  
                drawing_area.show()  
                drawing_area.queue_draw()  # Force redraw to clear artifacts  
            else:  
                drawing_area.hide()  
                drawing_area.queue_draw()  
        self.queue_draw()  
  
    def _set_mode(self, mode):  
        """Handles key-based mode switching.  
           0: Show all streams (2×2 grid).  
           1–4: Toggle the corresponding stream (pressing again resets to only that stream).  
        """  
        print(f"[ACTION] Key Pressed: {mode}")  
        if mode == 0:  
            self.active_streams = self.ports[:]  
        else:  
            port = self.ports[mode - 1]  
            if port in self.active_streams:  
                self.active_streams = [port]  
            else:  
                self.active_streams.append(port)  
        self._update_display()  
  
    def _start_global_keyboard_listener(self):  
        """Starts a global keyboard listener using pynput."""  
        def on_press(key):  
            try:  
                if key.char in ("0", "1", "2", "3", "4"):  
                    GLib.idle_add(self._set_mode, int(key.char))  
            except AttributeError:  
                pass  
        self.listener = keyboard.Listener(on_press=on_press)  
        self.listener.start()  
  
    def start(self):  
        """Starts all GStreamer pipelines."""  
        for pipeline in self.pipelines.values():  
            pipeline.set_state(Gst.State.PLAYING)  
  
    def stop(self):  
        """Stops all pipelines and quits GTK."""  
        for pipeline in self.pipelines.values():  
            pipeline.set_state(Gst.State.NULL)  
        if self.listener:  
            self.listener.stop()  
        Gtk.main_quit()  
  
    def on_destroy(self, widget):  
        self.stop()  
  
    def run(self):  
        """Runs the GTK event loop."""  
        self.start()  
        Gtk.main()  
  
def main():  
    parser = argparse.ArgumentParser(description="GTK-based 4-stream UDP viewer with ximagesink embedding and minimal buffering.")  
    parser.add_argument("--ports", nargs=4, type=int, default=[5600, 5601, 5602, 5603],  
                        help="UDP ports for 4 feeds (default: 5600 5601 5602 5603)")  
    parser.add_argument("--screen-size", type=str, default="1280x800",  
                        help="Screen resolution (default: 1280x800)")  
    args = parser.parse_args()  
  
    screen_width, screen_height = map(int, args.screen_size.split("x"))  
    app = VideoOverlay(ports=args.ports, screen_width=screen_width, screen_height=screen_height)  
  
    def signal_handler(sig, frame):  
        app.stop()  
    signal.signal(signal.SIGINT, signal_handler)  
    signal.signal(signal.SIGTERM, signal_handler)  
  
    app.run()  
  
if __name__ == "__main__":  
    main()  
  
