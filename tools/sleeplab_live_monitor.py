#!/usr/bin/env python3
"""SleepLab live USB CDC telemetry monitor.

Reads the existing *text* telemetry firmware, not binary WAV test firmware.
Graphs live readings and records timestamped telemetry to CSV.

Install: py -m pip install pyserial matplotlib
Run:     py sleeplab_live_monitor.py
"""

from __future__ import annotations

import csv
from collections import deque
from datetime import datetime, timedelta
from pathlib import Path
import queue
import re
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

try:
    import serial
    from serial.tools import list_ports
    from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
    from matplotlib.figure import Figure
    import matplotlib.dates as mdates
except ImportError as exc:
    raise SystemExit(
        f"Missing dependency: {exc}\nInstall with: py -m pip install pyserial matplotlib"
    ) from exc


BG = "#101827"
PANEL = "#1a2639"
PLOT = "#142033"
FG = "#edf3fc"
MUTED = "#9eb0c5"
BLUE = "#6cb3ff"
GREEN = "#5bdbad"
YELLOW = "#ffc777"
GRID = "#354357"

FLOAT_FIELDS = ("T", "RH", "RMS", "dBFS")
INT_FIELDS = ("H", "F", "E", "Miss", "N")
FIELDS = FLOAT_FIELDS + INT_FIELDS
VALUE_RE = {
    name: re.compile(r"(?<![\w])" + re.escape(name) + r":\s*([-+]?\d+(?:\.\d+)?)")
    for name in FIELDS
}
CSV_FIELDS = [
    "timestamp_local", "elapsed_s", "temperature_c", "humidity_pct",
    "dma_half", "dma_full", "dma_errors", "missed_blocks",
    "processed_samples", "rms", "dbfs", "raw_line"
]
WINDOWS = {
    "30 seconds": 30,
    "1 minute": 60,
    "5 minutes": 300,
    "15 minutes": 900,
    "30 minutes": 1800,
    "1 hour": 3600,
    "3 hours": 10800,
    "All stored": None,
}
GRAPH_OPTIONS = ["Overview", "Sound (dBFS)", "RMS", "Temperature", "Humidity", "Processed samples"]


def parse_telemetry(line: str) -> dict | None:
    """Parse one serial text line. Ignore lines with no measurement fields."""
    data = {}
    for field, pattern in VALUE_RE.items():
        found = pattern.search(line)
        if found:
            try:
                data[field] = (float(found.group(1)) if field in FLOAT_FIELDS
                               else int(found.group(1)))
            except ValueError:
                pass
    # Do not treat random boot messages or an incompatible binary stream as data.
    if "dBFS" not in data and "RMS" not in data:
        return None
    data["line"] = line
    return data


class SleepLabGUI:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("SleepLab  |  Live telemetry")
        root.geometry("1160x790")
        root.minsize(850, 620)
        root.configure(bg=BG)
        self._style()

        self.events: queue.Queue = queue.Queue()
        self.serial_port = None
        self.stop_reader = threading.Event()
        self.history = deque(maxlen=86400)  # ~24 h at 1 reading/second
        self.dirty = True
        self.last_plot_time = 0.0
        self.connected_at = None
        self.last_measurement_at = None

        self.rec_state = "idle"  # idle, waiting, recording
        self.rec_path: Path | None = None
        self.rec_writer = None
        self.rec_file = None
        self.rec_deadline = 0.0
        self.rec_start = 0.0
        self.rec_duration = 0
        self.rec_count = 0

        self.port_var = tk.StringVar(value="COM8")
        self.graph_var = tk.StringVar(value="Overview")
        self.window_var = tk.StringVar(value="5 minutes")
        self.delay_var = tk.StringVar(value="0")
        self.duration_var = tk.StringVar(value="60")
        self.conn_var = tk.StringVar(value="Disconnected")
        self.rec_var = tk.StringVar(value="Not recording")
        self.stat_vars = {key: tk.StringVar(value="—") for key in
                          ("dbfs", "rms", "temp", "humidity", "n", "miss", "errors")}
        self._make_ui()
        self._refresh_ports()
        self._plot()
        root.protocol("WM_DELETE_WINDOW", self._close)
        root.after(150, self._pump)

    def _style(self):
        st = ttk.Style(self.root)
        st.theme_use("clam")
        st.configure("TFrame", background=BG)
        st.configure("Panel.TFrame", background=PANEL)
        st.configure("TLabel", background=BG, foreground=FG, font=("Segoe UI", 10))
        st.configure("Secondary.TLabel", background=BG, foreground=MUTED)
        st.configure("Panel.TLabel", background=PANEL, foreground=FG)
        st.configure("TButton", background="#2a3c55", foreground=FG, padding=(12, 8), borderwidth=0)
        st.map("TButton", background=[("active", "#405775")])
        st.configure("Accent.TButton", background="#2166ad", foreground="white", padding=(12, 8))
        st.map("Accent.TButton", background=[("active", "#307ecf")])
        st.configure("TEntry", fieldbackground=PANEL, foreground=FG)
        st.configure("TCombobox", fieldbackground=PANEL, foreground=FG, background=PANEL)
        st.configure("TSpinbox", fieldbackground=PANEL, foreground=FG, arrowsize=13)

    def _make_ui(self):
        outer = ttk.Frame(self.root, padding=14)
        outer.pack(fill=tk.BOTH, expand=True)

        ttk.Label(outer, text="SleepLab  /  LIVE TELEMETRY",
                  font=("Segoe UI", 18, "bold")).pack(anchor="w", pady=(0, 10))

        controls = ttk.Frame(outer)
        controls.pack(fill=tk.X, pady=(0, 8))
        ttk.Label(controls, text="Port").pack(side=tk.LEFT, padx=(0, 6))
        self.port_box = ttk.Combobox(controls, textvariable=self.port_var, width=10)
        self.port_box.pack(side=tk.LEFT, padx=(0, 5))
        ttk.Button(controls, text="Refresh", command=self._refresh_ports).pack(side=tk.LEFT, padx=(0, 8))
        self.connect_btn = ttk.Button(controls, text="Connect", command=self._toggle_connection,
                                      style="Accent.TButton")
        self.connect_btn.pack(side=tk.LEFT, padx=(0, 20))
        ttk.Label(controls, text="Graph").pack(side=tk.LEFT, padx=(0, 6))
        graph_box = ttk.Combobox(controls, textvariable=self.graph_var, width=19,
                                  values=GRAPH_OPTIONS, state="readonly")
        graph_box.pack(side=tk.LEFT, padx=(0, 14))
        graph_box.bind("<<ComboboxSelected>>", lambda _: self._force_plot())
        ttk.Label(controls, text="Show last").pack(side=tk.LEFT, padx=(0, 6))
        time_box = ttk.Combobox(controls, textvariable=self.window_var, width=13,
                                values=list(WINDOWS), state="readonly")
        time_box.pack(side=tk.LEFT)
        time_box.bind("<<ComboboxSelected>>", lambda _: self._force_plot())

        rec = ttk.Frame(outer)
        rec.pack(fill=tk.X, pady=(0, 8))
        ttk.Label(rec, text="Start delay (s)").pack(side=tk.LEFT, padx=(0, 6))
        ttk.Spinbox(rec, from_=0, to=3600, textvariable=self.delay_var, width=7).pack(side=tk.LEFT, padx=(0, 16))
        ttk.Label(rec, text="Duration (s)").pack(side=tk.LEFT, padx=(0, 6))
        ttk.Spinbox(rec, from_=0, to=86400, textvariable=self.duration_var,
                    width=8).pack(side=tk.LEFT, padx=(0, 7))
        ttk.Label(rec, text="0 = until Stop", style="Secondary.TLabel").pack(side=tk.LEFT, padx=(0, 16))
        self.start_btn = ttk.Button(rec, text="Start recording", command=self._start_recording,
                                    style="Accent.TButton", state="disabled")
        self.start_btn.pack(side=tk.LEFT, padx=(0, 8))
        self.stop_btn = ttk.Button(rec, text="Stop / Cancel", command=self._stop_recording,
                                   state="disabled")
        self.stop_btn.pack(side=tk.LEFT, padx=(0, 8))
        ttk.Button(rec, text="Clear graph", command=self._clear).pack(side=tk.LEFT)

        stat_row = ttk.Frame(outer)
        stat_row.pack(fill=tk.X, pady=(3, 10))
        metrics = [
            ("dbfs", "SOUND (dBFS)", "dBFS"),
            ("rms", "RMS", ""),
            ("temp", "TEMPERATURE", "°C"),
            ("humidity", "HUMIDITY", "%"),
            ("n", "SAMPLES / REPORT", ""),
            ("miss", "MISSED BLOCKS", ""),
            ("errors", "DMA ERRORS", ""),
        ]
        for key, label, suffix in metrics:
            box = tk.Frame(stat_row, bg=PANEL, padx=10, pady=8)
            box.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 7))
            tk.Label(box, text=label, fg=MUTED, bg=PANEL,
                     font=("Segoe UI", 8)).pack(anchor="w")
            line = tk.Frame(box, bg=PANEL)
            line.pack(anchor="w")
            tk.Label(line, textvariable=self.stat_vars[key], fg=GREEN if key == "dbfs" else FG,
                     bg=PANEL, font=("Segoe UI", 15, "bold")).pack(side=tk.LEFT)
            if suffix:
                tk.Label(line, text=" " + suffix, fg=MUTED, bg=PANEL,
                         font=("Segoe UI", 9)).pack(side=tk.LEFT, anchor="s")

        fig_frame = ttk.Frame(outer)
        fig_frame.pack(fill=tk.BOTH, expand=True)
        self.figure = Figure(figsize=(12, 6), dpi=100, facecolor=BG)
        self.canvas = FigureCanvasTkAgg(self.figure, master=fig_frame)
        self.canvas.get_tk_widget().configure(bg=BG, highlightthickness=0)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

        lower = ttk.Frame(outer)
        lower.pack(fill=tk.X, pady=(9, 1))
        ttk.Label(lower, textvariable=self.conn_var, style="Secondary.TLabel").pack(side=tk.LEFT)
        ttk.Label(lower, textvariable=self.rec_var, style="Secondary.TLabel").pack(side=tk.RIGHT)
        ttk.Label(outer, text="Records numeric USB text measurements to CSV (not raw audio/WAV). "
                  "Timestamps come from the PC.", style="Secondary.TLabel").pack(anchor="w", pady=(5, 0))

    def _refresh_ports(self):
        ports = [port.device for port in list_ports.comports()]
        if not ports:
            ports = [self.port_var.get() or "COM8"]
        self.port_box["values"] = ports
        if self.port_var.get() not in ports and not self.serial_port:
            self.port_var.set(ports[0])

    def _toggle_connection(self):
        if self.serial_port is not None:
            self._disconnect()
            return
        port_name = self.port_var.get().strip()
        if not port_name:
            messagebox.showerror("Serial port", "Select or type a COM port.")
            return
        try:
            port = serial.Serial(port=port_name, baudrate=115200, timeout=0.25)
        except (OSError, serial.SerialException) as exc:
            messagebox.showerror("Could not connect", f"Cannot open {port_name}:\n{exc}\n\n"
                                 "Close any serial terminal using this port.")
            return
        self.serial_port = port
        self.stop_reader = threading.Event()
        self.connected_at = time.monotonic()
        self.conn_var.set(f"Connected to {port_name} · waiting for telemetry…")
        self.connect_btn.configure(text="Disconnect")
        self.start_btn.configure(state="normal")
        self.port_box.configure(state="disabled")
        threading.Thread(target=self._reader, args=(port, self.stop_reader), daemon=True).start()

    def _reader(self, port, stop_event):
        while not stop_event.is_set():
            try:
                raw = port.readline()
            except (OSError, serial.SerialException) as exc:
                if not stop_event.is_set():
                    self.events.put(("error", str(exc)))
                break
            if raw:
                received_mono = time.monotonic()
                received_wall = datetime.now().astimezone()
                line = raw.decode("utf-8", errors="replace").strip()
                if line:
                    self.events.put(("line", received_mono, received_wall, line))

    def _disconnect(self):
        if self.rec_state != "idle":
            self._stop_recording(show_dialog=False)
        self.stop_reader.set()
        if self.serial_port is not None:
            try:
                self.serial_port.close()
            except (OSError, serial.SerialException):
                pass
        self.serial_port = None
        self.connect_btn.configure(text="Connect")
        self.port_box.configure(state="normal")
        self.start_btn.configure(state="disabled")
        self.stop_btn.configure(state="disabled")
        self.conn_var.set("Disconnected")

    def _start_recording(self):
        if self.serial_port is None or self.rec_state != "idle":
            return
        try:
            delay = int(self.delay_var.get())
            duration = int(self.duration_var.get())
            if not (0 <= delay <= 3600 and 0 <= duration <= 86400):
                raise ValueError("Out of range")
        except ValueError:
            messagebox.showerror("Invalid timing", "Delay: 0–3600 s. Duration: 0–86400 s. "
                                 "Use duration 0 for manual stopping.")
            return
        suggested = f"sleeplab_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
        dest = filedialog.asksaveasfilename(
            title="Save SleepLab measurement recording",
            defaultextension=".csv", initialfile=suggested,
            filetypes=[("CSV (comma-separated values)", "*.csv")],
        )
        if not dest:
            return
        self.rec_path = Path(dest)
        self.rec_deadline = time.monotonic() + delay
        self.rec_duration = duration
        self.rec_count = 0
        self.rec_state = "waiting"
        self.start_btn.configure(state="disabled")
        self.stop_btn.configure(state="normal")
        self.rec_var.set(f"Recording starts in {delay} s · {self.rec_path.name}")
        if delay == 0:
            self._begin_recording()

    def _begin_recording(self):
        try:
            self.rec_file = self.rec_path.open("w", newline="", encoding="utf-8")
            self.rec_writer = csv.DictWriter(self.rec_file, fieldnames=CSV_FIELDS)
            self.rec_writer.writeheader()
            self.rec_file.flush()
        except OSError as exc:
            self.rec_state = "idle"
            self.rec_file = None
            self.rec_writer = None
            self.rec_var.set("Could not create recording")
            self.start_btn.configure(state="normal" if self.serial_port else "disabled")
            self.stop_btn.configure(state="disabled")
            messagebox.showerror("Cannot save", str(exc))
            return
        self.rec_start = self.rec_deadline
        self.rec_state = "recording"
        self.rec_var.set(f"Recording · 0 rows · {self.rec_path.name}")
        self.dirty = True

    def _stop_recording(self, show_dialog=False):
        previous = self.rec_state
        if previous == "idle":
            return
        self.rec_state = "idle"
        if self.rec_file is not None:
            try:
                self.rec_file.flush()
                self.rec_file.close()
            except OSError:
                pass
        self.rec_file = None
        self.rec_writer = None
        self.start_btn.configure(state="normal" if self.serial_port else "disabled")
        self.stop_btn.configure(state="disabled")
        if previous == "waiting":
            self.rec_var.set("Delayed recording cancelled")
        else:
            self.rec_var.set(f"Saved {self.rec_count} rows · {self.rec_path}")
            if show_dialog:
                messagebox.showinfo("Recording complete",
                                    f"Saved {self.rec_count} samples to:\n{self.rec_path}")
        self.dirty = True

    def _on_measurement(self, received_mono, received_wall, line):
        reading = parse_telemetry(line)
        if reading is None:
            return
        reading["mono"] = received_mono
        reading["wall"] = received_wall.replace(tzinfo=None)
        reading["wall_iso"] = received_wall.isoformat(timespec="milliseconds")
        self.history.append(reading)
        self.last_measurement_at = received_mono
        self.conn_var.set(f"Connected to {self.port_var.get()} · {len(self.history)} readings stored")
        names = {
            "dbfs": ("dBFS", "{:.1f}"), "rms": ("RMS", "{:.5f}"),
            "temp": ("T", "{:.2f}"), "humidity": ("RH", "{:.2f}"),
            "n": ("N", "{:d}"), "miss": ("Miss", "{:d}"),
            "errors": ("E", "{:d}")
        }
        for label, (field, fmt) in names.items():
            if field in reading:
                self.stat_vars[label].set(fmt.format(reading[field]))
        self.dirty = True

        if self.rec_state != "recording" or self.rec_writer is None:
            return
        if received_mono < self.rec_start:
            return
        if self.rec_duration and received_mono >= self.rec_start + self.rec_duration:
            return
        row = {
            "timestamp_local": reading["wall_iso"],
            "elapsed_s": f"{received_mono - self.rec_start:.3f}",
            "temperature_c": reading.get("T", ""),
            "humidity_pct": reading.get("RH", ""),
            "dma_half": reading.get("H", ""),
            "dma_full": reading.get("F", ""),
            "dma_errors": reading.get("E", ""),
            "missed_blocks": reading.get("Miss", ""),
            "processed_samples": reading.get("N", ""),
            "rms": reading.get("RMS", ""),
            "dbfs": reading.get("dBFS", ""),
            "raw_line": line,
        }
        try:
            self.rec_writer.writerow(row)
            self.rec_file.flush()  # keep the file usable even if the GUI exits unexpectedly
            self.rec_count += 1
        except OSError as exc:
            self._stop_recording(show_dialog=False)
            messagebox.showerror("Recording interrupted", str(exc))

    def _pump(self):
        # Run timer transitions before draining the incoming serial queue.
        now = time.monotonic()
        if self.rec_state == "waiting":
            left = self.rec_deadline - now
            if left <= 0:
                self._begin_recording()
            else:
                self.rec_var.set(f"Starts in {left:.1f} s · {self.rec_path.name}")

        # Bound work per tick to keep the interface responsive.
        for _ in range(400):
            try:
                item = self.events.get_nowait()
            except queue.Empty:
                break
            if item[0] == "error":
                self._disconnect()
                messagebox.showerror("USB serial connection lost", item[1])
                break
            _, stamp, wall, line = item
            self._on_measurement(stamp, wall, line)

        now = time.monotonic()
        if self.rec_state == "recording":
            elapsed = now - self.rec_start
            self.rec_var.set(f"REC · {elapsed:.1f} s · {self.rec_count} rows · {self.rec_path.name}")
            if self.rec_duration and elapsed >= self.rec_duration:
                self._stop_recording(show_dialog=False)

        if self.serial_port is not None and self.last_measurement_at is None and self.connected_at:
            if now - self.connected_at > 4:
                self.conn_var.set(f"Connected to {self.port_var.get()}, but no matching RMS/dBFS text received")
        elif self.serial_port is not None and self.last_measurement_at is not None:
            if now - self.last_measurement_at > 5:
                self.conn_var.set(f"Connected to {self.port_var.get()}, no telemetry for {now-self.last_measurement_at:.0f} s")

        if self.dirty and now - self.last_plot_time >= 0.35:
            self._plot()
        self.root.after(150, self._pump)

    def _clear(self):
        self.history.clear()
        self.dirty = True
        self._plot()

    def _force_plot(self):
        self.dirty = True
        self._plot()

    def _plot(self):
        self.figure.clear()
        option = self.graph_var.get()
        if option == "Overview":
            specs = [("dBFS", "Sound level (dBFS)", BLUE),
                     ("T", "Temperature (°C)", GREEN),
                     ("RH", "Relative humidity (%)", YELLOW)]
        else:
            options = {
                "Sound (dBFS)": ("dBFS", "Sound level (dBFS)", BLUE),
                "RMS": ("RMS", "Normalized RMS", BLUE),
                "Temperature": ("T", "Temperature (°C)", GREEN),
                "Humidity": ("RH", "Relative humidity (%)", YELLOW),
                "Processed samples": ("N", "Processed samples per USB report", BLUE),
            }
            specs = [options.get(option, options["Sound (dBFS)"])]

        selected_window = WINDOWS.get(self.window_var.get(), 300)
        now = time.monotonic()
        cutoff = -float("inf") if selected_window is None else now - selected_window
        visible = [r for r in self.history if r["mono"] >= cutoff]
        if len(visible) > 3500:
            step = max(1, len(visible) // 3500)
            visible = visible[::step]

        axes = self.figure.subplots(len(specs), 1, sharex=(len(specs) > 1))
        if len(specs) == 1:
            axes = [axes]
        current_time = datetime.now()

        for axis, (field, ylabel, color) in zip(axes, specs):
            axis.set_facecolor(PLOT)
            axis.grid(True, color=GRID, linewidth=0.6, alpha=0.5)
            axis.tick_params(colors=MUTED, labelsize=9)
            for spine in axis.spines.values():
                spine.set_color(GRID)
            axis.set_ylabel(ylabel, color=FG, fontsize=10)
            samples = [(r["wall"], r[field]) for r in visible if field in r]
            if samples:
                xs, ys = zip(*samples)
                axis.plot(xs, ys, color=color, linewidth=1.7)
                axis.scatter([xs[-1]], [ys[-1]], s=22, color=color, zorder=3)
                axis.margins(y=0.22)
                if field == "dBFS":
                    axis.axhline(-60, color=MUTED, linestyle="--", alpha=0.3)
            else:
                axis.text(0.5, 0.5, "Waiting for measurements…",
                          transform=axis.transAxes, color=MUTED, ha="center", va="center")
            if selected_window is not None:
                axis.set_xlim(current_time - timedelta(seconds=selected_window), current_time)
            axis.xaxis.set_major_locator(mdates.AutoDateLocator(minticks=3, maxticks=7))
            axis.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))

        axes[-1].set_xlabel("PC local time", color=MUTED)
        self.figure.tight_layout(pad=2.1)
        self.canvas.draw_idle()
        self.last_plot_time = time.monotonic()
        self.dirty = False

    def _close(self):
        self._disconnect()
        self.root.destroy()


def main():
    app = tk.Tk()
    SleepLabGUI(app)
    app.mainloop()


if __name__ == "__main__":
    main()
