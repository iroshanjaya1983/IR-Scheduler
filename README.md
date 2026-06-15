# IR Scheduler Pro v3.1

[cite_start]An autonomous, web-enabled Infrared (IR) remote scheduler built for the ESP32-C3 Super Mini[cite: 170]. [cite_start]This device allows you to learn IR codes (both RAW and decoded) [cite: 206][cite_start], schedule them to transmit at specific times, and toggle between a configuration mode and an autonomous execution mode using a physical switch[cite: 319, 320]. 

[cite_start]Accurate timekeeping is maintained using a DS3231 RTC[cite: 172], ensuring scheduled events fire reliably even without an active internet connection.

## Features
* [cite_start]**Dual Modes (Hardware Toggled):** * **Receiver Mode (AP):** Broadcasts a Wi-Fi Access Point (`irauto`)[cite: 172]. Hosts an interactive web UI to capture IR codes, sync the RTC, and manage schedules[cite: 321].
  * [cite_start]**Auto Mode:** Disables Wi-Fi to save power and operates autonomously, executing scheduled IR signals based on the RTC[cite: 326].
* [cite_start]**Advanced IR Learning:** Supports learning and transmitting both standard decoded protocols and RAW IR signals[cite: 208, 210].
* [cite_start]**Thermal Management:** Reduces the ESP_WiFi TX power to 40 (10dBm) to prevent thermal throttling [cite: 324, 325] [cite_start]and utilizes `vTaskDelay` to pass idle time back to FreeRTOS, keeping the CPU cool[cite: 337, 338].
* [cite_start]**Persistent Storage:** Schedules and learned codes are saved to non-volatile flash memory using the `Preferences.h` library[cite: 173, 174].

## Hardware Requirements
* [cite_start]**Microcontroller:** ESP32-C3 Super Mini [cite: 170]
* [cite_start]**RTC Module:** DS3231 [cite: 172]
* **IR Hardware:** Standard 38kHz IR Receiver and IR LED

## Pin Configuration
| Component | Pin | Note |
| :--- | :--- | :--- |
| IR Transmitter LED | [cite_start]`GPIO 4` | [cite: 170] |
| IR Receiver | [cite_start]`GPIO 6` | [cite: 170] |
| Mode Switch | `GPIO 21` | [cite_start]Pull to GND for Receiver Mode [cite: 170, 320] |
| Built-in LED | `GPIO 10` | [cite_start]Active LOW [cite: 170, 179] |
| I2C SDA (RTC) | `GPIO 8` | [cite_start]Remapped [cite: 170] |
| I2C SCL (RTC) | `GPIO 9` | [cite_start]Remapped [cite: 171] |

## Software Dependencies
Install the following libraries via the Arduino IDE Library Manager or PlatformIO:
* [cite_start]`IRremoteESP8266` (Provides `IRrecv.h`, `IRsend.h`, `IRutils.h`) [cite: 170]
* [cite_start]`RTClib` (For the DS3231) [cite: 170]
* [cite_start]`ArduinoJson` [cite: 170]

## Usage Instructions
1. **Initial Setup:** Wire the components according to the pinout.
2. [cite_start]**Configuration:** Flip the mode switch to **Receiver Mode** (pull GPIO 21 to GND)[cite: 320]. Connect to the Wi-Fi AP:
   * [cite_start]**SSID:** `irauto` [cite: 172]
   * [cite_start]**Password:** `12345678` [cite: 173]
3. **Web UI:** Open a browser and navigate to the router's gateway IP (usually `192.168.4.1`). [cite_start]Sync the time, learn your IR codes, and set up your schedules[cite: 236].
4. **Execution:** Flip the switch back to **Auto Mode**. [cite_start]The ESP32 will disconnect from Wi-Fi and automatically execute your schedules[cite: 326].