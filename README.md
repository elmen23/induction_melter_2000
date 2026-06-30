# IH-2000 — Induction Heater Controller 🔥

[![Build](https://github.com/elmen23/induction-heater-controller/actions/workflows/build.yml/badge.svg)](https://github.com/elmen23/induction-heater-controller/actions/workflows/build.yml)

Production-ready **ESP32** induction heater controller with **MCPWM** half-bridge output, adjustable frequency, dead time, duty cycle, and a responsive web dashboard.

> ⚡ **50 kHz half-bridge** | 🎛️ **20–100 kHz** | 🧩 **0–1000 ns dead time** | 🌐 **Web UI**

---

## Features

- **MCPWM hardware output** — complementary half-bridge signals (GPIO18/GPIO19) using ESP-IDF's new MCPWM driver API
- **Adjustable frequency** — 20 kHz – 100 kHz (1 Hz resolution)
- **Adjustable duty cycle** — 0% – 100%
- **Hardware dead time** — Rising edge (RED) & falling edge (FED), 0–1000 ns (25 ns/tick)
- **Master enable** — GPIO4, shuts down all outputs when disabled
- **Web dashboard** — responsive dark-themed UI with sliders, numeric inputs, real-time status
- **PWM waveform visualizer** — SVG-based display of complementary signals with dead time
- **REST API** — `/api/config` GET/POST, `/api/status` GET, `/api/estop` POST
- **Simulated feedback** — power (kW), voltage, current, temperature for testing
- **Emergency stop** — immediate output shutdown via button or API
- **WiFi AP mode** — standalone hotspot `IH-2000`, no router needed
- **NVS persistence** — settings survive power cycles (ESP-IDF NVS flash)
- **OTA-ready** — partition table with two firmware slots

---

## Hardware Pinout

| Signal | GPIO | Function |
|--------|------|----------|
| MCPWM0A | 18 | High-side gate driver (e.g. IR2101/IR2110) |
| MCPWM0B | 19 | Low-side gate driver (complementary) |
| Enable | 4 | Master enable (active HIGH, pull-down) |

### Half-Bridge Topology

```
          ┌───────┐
  MCPWM_A ─┤ High  ├───→ To resonant LC / work coil
            │ Side  │
  MCPWM_B ─┤ Low   │
            └───────┘
```

**Important:** The dead time values (RED / FED) must be tuned for your specific IGBT/MOSFET gate driver and switching speed. Insufficient dead time causes shoot-through!

---

## Project Structure

```
induction-heater-controller/
├── .github/workflows/build.yml   # CI: build firmware, upload artifacts, release
├── platformio.ini                 # ESP-IDF 5.5 + PlatformIO config
├── CMakeLists.txt                 # ESP-IDF build system
├── partitions.csv                 # 2 × OTA app slots + SPIFFS + NVS
├── sdkconfig.esp32dev             # ESP-IDF sdkconfig
├── dev_server.py                  # Local dev server (serves web UI without ESP32)
└── src/
    ├── main.cpp                   # Entry point, init sequence, main loop
    ├── mcpwm_control.h / .cpp     # MCPWM driver: frequency, duty, dead time, enable
    ├── web_server.h / .cpp        # REST API: HTTP server, URI handlers, JSON
    ├── wifi_manager.h / .cpp      # WiFi AP mode (IH-2000) with event handlers
    ├── index.html                 # Web UI — HTML
    ├── style.css                  # Web UI — dark theme stylesheet
    ├── script.js                  # Web UI — sliders, waveform viewer, polling
    └── embedded_files.h           # Embedded HTML/CSS/JS (ESP-IDF binary embed)
```

---

## Quick Start

### Prerequisites

1. [PlatformIO CLI](https://platformio.org/install/cli) or VS Code + PlatformIO extension
2. ESP32 dev board (e.g. ESP32-DevKitC, NodeMCU-32S)
3. Half-bridge driver (e.g. IR2101/IR2110) + IGBTs / MOSFETs + resonant tank

### Build

```bash
cd induction-heater-controller
pio run
```

### Upload

```bash
pio run --target upload
```

### Monitor serial output

```bash
pio device monitor
```

---

## Usage

1. Flash the firmware to ESP32
2. Connect to WiFi **`IH-2000`** (password: `induction2000`)
3. Open browser to **http://192.168.4.1**
4. Use the dashboard to control frequency, duty cycle, dead time, and enable/disable output

### Web UI Controls

| Control | Range | Description |
|---------|-------|-------------|
| **Frequency** | 20.0 – 100.0 kHz | Switching frequency |
| **Duty Cycle** | 0.0 – 100.0 % | PWM duty (recommended: 45–50%) |
| **RED (Dead Time)** | 0 – 1000 ns | Rising edge delay |
| **FED (Dead Time)** | 0 – 1000 ns | Falling edge delay |
| **Enable** | ON / OFF | Master output toggle |
| **Emergency Stop** | — | Immediate output shutdown |

---

## API Reference

All endpoints return `application/json`.

### `GET /api/config` — Read current configuration

```json
{
  "enable": false,
  "frequency": 40000,
  "duty": 45.0,
  "dead_time_red": 200,
  "dead_time_fed": 200
}
```

### `POST /api/config` — Update configuration

```json
{
  "enable": true,
  "frequency": 50000,
  "duty": 48.0,
  "dead_time_red": 150,
  "dead_time_fed": 150
}
```

### `GET /api/status` — Read live status

```json
{
  "enable": true,
  "wifi_mode": "AP",
  "wifi_ip": "192.168.4.1",
  "frequency": 50000,
  "duty": 48.0,
  "power": 8.2,
  "voltage": 230.5,
  "current": 35.7,
  "temperature": 42
}
```

### `POST /api/estop` — Emergency stop

Immediately disables all PWM outputs and forces duty to 0%.

---

## Development

### Local Web UI Testing

Run the dev server to test the web interface without an ESP32:

```bash
python dev_server.py
```

Then open **http://localhost:8080** in your browser.

### Embedded Files

The HTML, CSS, and JS are embedded in the firmware binary using ESP-IDF's `EMBED_FILES` mechanism. After editing web files, rebuild:

```bash
pio run
```

The files are embedded as raw byte arrays via `embedded_files.h`.

---

## CI/CD

The project includes a GitHub Actions workflow (`.github/workflows/build.yml`) that:

- Builds the firmware on push/PR to `main` or `master`
- Renames binaries to `IH-2000-v1.0.bin`, `bootloader.bin`, `partitions.bin`
- Uploads firmware as a build artifact
- Creates a GitHub Release on version tags (`v*`)

---

## Safety

- **Emergency stop** — immediately forces all outputs LOW and disables MCPWM
- **No PWM outputs active at boot** — must be explicitly enabled
- **Dead time** — hardware-enforced, prevents shoot-through in half-bridge topology
- **All settings validated** — range-checked before applying
- **NVS erase recovery** — flash corruption handled gracefully

---

## License

MIT
