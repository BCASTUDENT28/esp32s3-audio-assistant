# ESP32-S3 AI Voice Assistant Firmware

This repository contains a modular ESP-IDF firmware for the **ESP32-S3 N16R8** development board, implementing an offline-activated AI voice assistant. It features a Captive WiFi onboarding portal, OLED visual feedback, standard I2S capture/playback, Whisper Speech-to-Text, DeepSeek Chat completions, Text-to-Speech audio streaming, and asynchronous Telegram bot remote control.

---

## 🛠 Hardware Configuration

| Component | Pin Name | ESP32-S3 GPIO Pin | Connection Notes |
|---|---|---|---|
| **SSD1306 OLED** | SDA | **GPIO 9** | Pull-up Resistor (~4.7kΩ) to 3.3V |
| | SCL | **GPIO 8** | Pull-up Resistor (~4.7kΩ) to 3.3V |
| | VCC / GND | 3.3V / GND | Power from ESP32 |
| **INMP441 Mic** | SCK (BCLK) | **GPIO 41** | Clock pin for I2S RX |
| | WS (LRC) | **GPIO 42** | Word Select pin for I2S RX |
| | SD (DOUT) | **GPIO 2** | Serial Data input |
| | L/R (Select) | GND | Configured to Left Channel output |
| | VCC / GND | 3.3V / GND | Power from ESP32 |
| **PAM8403 Amp** | BCLK | **GPIO 18** | Clock pin for I2S TX |
| | WS (LRC) | **GPIO 17** | Word Select pin for I2S TX |
| | DOUT (DIN) | **GPIO 16** | Serial Data output to DAC |
| | VCC / GND | 5V / GND | PAM8403 requires 5V for full 3W output |
| **BOOT Button** | Push Button | **GPIO 0** | Integrated Boot button on ESP32-S3 |

> [!IMPORTANT]
> The ESP32-S3 does **not** contain an internal DAC. To drive the analog input of the PAM8403 amplifier, you must interface a standard external I2S DAC (e.g., **MAX98357A** or **PCM5102A**) between the ESP32-S3 I2S TX pins and the PAM8403 input pins, or use an external integrated I2S DAC/Amplifier module.

---

## 📁 Project Architecture

The code is structured into modular ESP-IDF components to separate responsibilities:

*   **`main/`**: Gluing the state machine together. Handles BOOT button polling (push-to-talk) and coordinates voice recording -> STT -> DeepSeek -> TTS -> Playback flow.
*   **`components/storage`**: Manages secure NVS parameter storage (SSIDs, API keys, tokens) and mounts the SPIFFS filesystem on partition `storage` to hold audio buffers.
*   **`components/oled_display`**: Implements custom SSD1306 horizontal page rendering. Spawns an independent FreeRTOS task rendering beautiful custom animations (thinking wheel, bouncing sound waves, OTA warning, WiFi logo) at 20 FPS.
*   **`components/audio_io`**: Configures dual-channel hardware I2S. Converts raw 32-bit microphone frames to amplified 16-bit PCM and duplicates mono output channels to drive stereo I2S DACs.
*   **`components/wifi_manager`**: Auto-connects to Station. Launches `ESP32-S3-Assistant` open Access Point, spawns a DNS hijack redirect server, and serves a modern dark-mode setup portal on `http://192.168.4.1`.
*   **`components/backend_client`**: Coordinates all HTTPS endpoint calls. Uploads recorded files in chunks to Whisper STT, parses DeepSeek JSON prompts, and streams output speech streams directly to SPIFFS.
*   **`components/telegram`**: Long-polls Telegram Updates, executing administrative instructions.
*   **`components/ota`**: Securely installs firmware bin files over HTTPS on the secondary partition.

---

## 🚀 Get Started

### 1. Requirements
*   ESP-IDF v5.1+ (or compatible newer version) installed and configured on your machine.
*   An OpenAI-compatible API key (for Whisper STT and TTS) and a DeepSeek API key.
*   A Telegram Bot token (created via [@BotFather](https://t.me/BotFather)).

### 2. Compile and Flash
Clone the project, setup your target device, and flash the code:

```bash
# Setup target to ESP32-S3
idf.py set-target esp32s3

# Build project binaries
idf.py build

# Flash and open serial monitor
idf.py flash monitor
```

### 3. Onboarding & Configuration
1. Turn on the device. The OLED will show `System Booting...` followed by `[WIFI SETUP]`.
2. Connect your smartphone/computer to the Wi-Fi network **`ESP32-S3-Assistant`**.
3. A captive portal page should automatically pop up. If not, open your browser and go to `http://192.168.4.1`.
4. Enter your home WiFi name, Password, DeepSeek API Key, and Telegram Bot token.
5. Click **Save Configuration**. The device will save credentials to NVS and restart.
6. Upon reboot, the display will connect to WiFi and show `Assistant READY`.

---

## 🎙 Operation Guide

*   **Talk to AI**: Press and hold the **BOOT button (GPIO 0)** on your board. Speak into the microphone. The screen will display a bouncing wave reflecting your audio volume. Release the button when done. The assistant will transcribe your voice, request a concise response from DeepSeek, download the synthesized audio file, and play it back through the speaker.
*   **Telegram Control**: Add your bot on Telegram and send the following commands:
    *   `/status` - Check assistant IP, WiFi connection, and free RAM.
    *   `/reboot` - Remotely reboot the device.
    *   `/clear` - Erase NVS configurations and return to WiFi setup portal.
    *   `/ota <https://your-server/firmware.bin>` - Trigger a remote HTTPS firmware update.
