# PresentAR

PresentAR is a comprehensive interactive presentation and 3D application system. It orchestrates a web-based frontend dashboard, a Python backend for file and model handling, and ESP32-based firmware using LVGL for a rich hardware interface.

## � Core Idea
The core concept of PresentAR is to bridge the gap between physical presentation tools and immersive digital content. By utilizing an M5Stack CoreS3 SE as a tactile, interactive remote and extended display, users can seamlessly control, rotate, and interact with 3D models and digital presentations on a web dashboard. It brings a new layer of engagement to presentations by combining physical hardware inputs with augmented/3D web rendering.

## ⚙️ How It Works (Working)

> **[Insert System Architecture Diagram Here]**
> *(e.g., `![Architecture Diagram](docs/images/architecture.png)`)*

1. **Hardware Interaction:** The presenter uses the M5Stack CoreS3 SE device to interact with the system. The device's touch screen and built-in sensors capture inputs, rendering a local UI using LVGL.
2. **Backend Communication:** The firmware communicates over Wi-Fi/UART with a local Python backend. The backend acts as the central hub, processing commands, and managing static assets like 3D models and recording files.
3. **Real-time Web Dashboard:** The frontend (powered by Vite and Bun) fetches this real-time state and media from the backend. When the presenter interacts with the M5Stack device, the web dashboard immediately updates—rotating 3D models, changing slides, or playing media.
4. **Recording & Saving:** Presenters can record their live sessions directly through the dashboard. The system captures the presentation flow, slide transitions, and 3D manipulations, saving the recordings locally to the backend (`/recordings`) for future playback, review, or export.

## ✨ Benefits
- **Enhanced Engagement:** Moves beyond traditional slide clickers by allowing physical manipulation of digital 3D space.
- **Session Recording:** Easily capture, save, and export complete presentations for asynchronous sharing or archiving.
- **Real-Time Synchronization:** Instantaneous feedback between the physical device and the web application.
- **Portable & Wireless:** Built on the ESP32-S3 architecture, the system is designed to be untethered over Wi-Fi.
- **Rich Visuals:** Combines the embedded graphics capability of LVGL on the device with high-fidelity web-based 3D rendering.


---

## 🛠 Hardware Specifications

### M5Stack CoreS3 SE (Target Device)

> **[Insert Image of M5Stack CoreS3 SE Device Here]**
> *(e.g., `![M5Stack CoreS3 SE](docs/images/device.png)`)*

The firmware is designed around the features and capabilities of the M5Stack CoreS3 SE.

- **Microcontroller (MCU):** ESP32-S3 @ 240MHz, dual-core Xtensa LX7
- **Wireless Connectivity:** 2.4 GHz Wi-Fi & Bluetooth 5.0 (LE)
- **Memory & Storage:** 8MB PSRAM + 16MB Flash Memory
- **Display:** 2.0-inch IPS capacitive touch screen (resolution: 320x240)
- **Power Management:** AXP2101 PMU, built-in RTC (BM8563)
- **Audio:** Built-in 1W speaker (AW88298 amplifier), built-in dual microphones
- **External Sensors:** MPU6500 (6-axis Gyroscope/Accelerometer) connected via I2C
- **Expansion/I/O:** 
  - 1x USB Type-C port (Power/Programming)
  - GROVE/I2C Expansion Ports
  - MicroSD (TF Card) Slot for expanded storage

---

## 📂 Project Structure

This repository is divided into three primary components:

> **[Insert Detailed Flow/Component Diagram Here]**
> *(e.g., `![Flow Diagram](docs/images/diagram.png)`)*

### 1. Frontend (`/frontend`)

> **[Insert Dashboard Screenshot Here]**
> *(e.g., `![Web Dashboard](docs/images/dashboard.png)`)*

The web-based user interface and dashboard built with modern web tools.
- **Tech Stack:** Bun, Vite, TypeScript, Tailwind CSS
- **Purpose:** Serve as the control center, viewing 3D models, recordings, and analytics.

### 2. Backend (`/backend`)
The core server handling API requests, file uploads, and session management.
- **Tech Stack:** Python 
- **Purpose:** Provide API endpoints and manage static media like 3D models (`/3dmodels`), preview states (`/previews`), and video components (`/recordings`).

### 3. Firmware (`/firmware`)
Embedded C++ and Arduino code designed for the ESP32-S3 architecture.
- **Tech Stack:** C++, Arduino Framework, LVGL (Light and Versatile Graphics Library)
- **Purpose:** Drive the M5Stack CoreS3 SE UI, communicate over WiFi/UART, interfaces with various sensors (I2C/SPI), and renders real-time graphical components.

---

## 🚀 Getting Started

### Prerequisites
- [Bun](https://bun.sh/) or Node.js for the Frontend.
- Python 3.10+ for the Backend.
- PlatformIO or Arduino IDE (with ESP32 board support) for the Firmware.

### Running the Frontend
```bash
cd frontend
bun install
bun run dev
```

### Running the Backend
```bash
cd backend
python -m venv .venv
# Activate environment (Windows: .venv\Scripts\activate | macOS/Linux: source .venv/bin/activate)
pip install -r requirements.txt
python main.py
```

### Flashing the Firmware
Depending on your toolchain of choice, open the `/firmware` directory in your IDE, verify your board is set to `M5Stack CoreS3 / ESP32-S3`, compile, and push to the device via USB Type-C.

---

## 📄 License
[Add License Details Here]
