<div align="center">

# 🚗 VW CDC ESP32 Bluetooth Emulator V2

[![PlatformIO](https://img.shields.io/badge/PlatformIO-Compatible-orange.svg)](https://platformio.org/)
[![ESP32](https://img.shields.io/badge/ESP32-FreeRTOS-blue.svg)](https://www.espressif.com/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**Upgrade your classic VW / Audi / Skoda / Seat car radio with modern Bluetooth 5.0 audio streaming while retaining full factory button controls!**

</div>

---

## 📌 What is this?
This project is an advanced **CD Changer (CDC) Emulator** built on the powerful dual-core ESP32 microcontroller. Instead of just tricking the radio into opening its AUX port, this system seamlessly bridges the classic Volkswagen SPI/NEC IR data bus with modern high-end Bluetooth modules (like the QCC3034-based Feasycom BT1026D / BT1036C).

When you press "Next Track" on your 20-year-old car radio, the ESP32 intercepts the signal and commands your smartphone to skip the song via Bluetooth!

## ✨ Key Features
- **📻 Native Button Integration**: Control Spotify, Apple Music, or calls using your factory head unit buttons (Next, Prev, CD1-CD6, Scan).
- **🎶 Smart Bluetooth Integration**: Communicates directly with QCC3034/QCC5125 Bluetooth chips via AT commands. No "dumb" AUX cables. The ESP32 knows when music is playing, paused, or disconnected.
- **🌐 Built-in WebUI**: Hosts a Wi-Fi Access Point and Dashboard. You can connect from your phone to view real-time protocol diagnostics, raw CDC pulse logs, and device status without needing a USB cable.
- **🧠 Dual-Core FreeRTOS Architecture**: VW CDC protocol timings are incredibly strict (~1200μs / 3200μs). We use FreeRTOS "Core Pinning" to lock the CDC SPI interface to `Core 1`, while the WebUI and Wi-Fi stack run safely on `Core 0`. Absolute zero timing jitter.

---

## 🔌 Hardware Wiring

| Signal Name | ESP32 Pin | Radio Pin | Purpose |
| :--- | :--- | :--- | :--- |
| **CDC SCK** | `GPIO 18` | `CDC CLOCK` | Hardware SPI Clock out |
| **CDC MOSI** | `GPIO 23` | `DATA IN` | Spoofed display data to Radio |
| **CDC DATA OUT** | `GPIO 4` | `DATA OUT` | Reads button presses (NEC pulses) |
| **BT RX** | `GPIO 16` | UART TX | Receives AT responses from BT1026 |
| **BT TX** | `GPIO 17` | UART RX | Sends AT commands to BT1026 |

*(Note: Audio L/R/GND from the Bluetooth module goes directly into the Radio's CD audio input pins).*

---

## 🚀 Getting Started (PlatformIO)

1. Clone this repository:
```bash
git clone https://github.com/ValeraBat/VWCDCESP32V2.git
```
2. Open the folder in **VS Code** with the **PlatformIO** extension installed.
3. Hook up your ESP32 via USB and click `Upload and Monitor`.
4. Connect your phone to the ESP32 Wi-Fi network (if enabled in code) and navigate to `http://192.168.4.1` to access the WebUI dashboard.

---

## 📚 Open Source Universal Libraries
During the development of this project, we separated the pure hardware abstraction layers into standalone, OS-independent libraries. If you want to build a super-cheap version of this emulator on an **Arduino Nano, ATmega328, or STM32** (without FreeRTOS or Wi-Fi), you can use our officially published standard libraries:

* [**VWCDC_Driver**](https://github.com/ValeraBat/VWCDC_Driver): A superloop-based VW CD-Changer emulator.
* [**BT1026D_Driver**](https://github.com/ValeraBat/BT1026D_Driver): A generalized AT-command interface for QCC audio modules.

## 🤝 Credits & Acknowledgements
Built upon the knowledge gathered by the open-source car-hacking community. Special thanks to original AVR concepts from `shyd`, `tomaskovacik`, and recent architectural inspirations from `NullString1`.