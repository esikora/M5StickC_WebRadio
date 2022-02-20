# M5StickC_WebRadio
Audio project based on an M5StickC Plus (ESP32) with I²S digital audio output:
- Internet radio
- Bluetooth A2DP sink
- Song info can be sent to IFTTT webhook

## Getting Started
#### Development environment
- Visual Studio Code (version 1.64.0)
- PlatformIO IDE for VSCode

#### System
- Device: [M5StickC Plus](https://docs.m5stack.com/en/core/m5stickc_plus)
- Platform: espressif32
- Board: m5stick-c
- Framework: arduino

#### Peripherals
- PCM5102 I²S DAC board
- [Dual-button unit](https://docs.m5stack.com/en/unit/dual_button)

#### Libraries used
- [M5StickCPlus](https://github.com/m5stack/M5StickC-Plus)
- [ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S)

#### Usage
- Button A: Change radio station / Resume playing (if paused)
- Button B: Switch device mode (internet radio, bluetooth A2DP sink)
- Button Pwr: Pause playing radio station
- Blue button (dual-button unit): Send current song info to IFTTT webhook

## Project Description

A comprehensive description of this project is available at hackster.io:
- [Part I](https://www.hackster.io/esikora/esp32-internet-radio-with-i-s-dac-a5515c)
- [Part II](https://www.hackster.io/esikora/esp32-audio-project-part-ii-bluetooth-receiver-add-on-1b7005)

## License

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

See the [LICENSE](LICENSE) file for details.

Copyright 2022 © Ernst Sikora
