# SleepLab

SleepLab is an embedded learning project that explores environmental measurements related to sleep. It runs on an STM32F411 (Arm Cortex-M4). The project is in early development and is not yet a complete sleep-monitoring device.

## Hardware

- STM32F411CE development board (USB-C)
- SHT3x (SHT31) temperature and relative-humidity sensor connected through I²C1
- Onboard LED (PC13)

## Software

- C with STM32 HAL (Hardware Abstraction Layer)
- STM32CubeMX, STM32CubeCLT and ARM GNU Toolchain
- CMake + Ninja
- USB CDC (Communications Device Class) virtual serial port

## Current Progress

- [x] STM32 startup, clocks, GPIO and LED
- [x] SHT3x I²C driver with CRC-8 validation and error reporting
- [x] Non-blocking sensor conversion using a state machine
- [x] Periodic measurements (1 second) using `HAL_GetTick()`
- [x] USB CDC temperature and humidity output
- [x] Independent LED heartbeat and `WFI` (Wait For Interrupt) CPU sleep
- [x] Timing experiments with DWT (Data Watchpoint and Trace) and TIM2 (Timer 2)
- [ ] Additional environmental sensors
- [ ] Microphone/audio acquisition with DMA (Direct Memory Access)
- [ ] SD card logging and sleep-environment analysis

The TIM2 interrupt and DWT measurements were learning experiments. They are not required for normal sensor acquisition.

## Project Structure

- `Core/Src/main.c` — CubeMX initialization and main loop
- `Core/Src/app.c` — measurement scheduling, LED and USB output
- `Core/Src/sht3x.c` — SHT3x sensor commands, data conversion and CRC checks
- `USB_DEVICE/` — CubeMX-generated USB CDC interface

## Building

Requires CMake, Ninja and the ARM GNU toolchain (for example, from STM32CubeCLT).

```bash
cmake --preset Debug
cmake --build --preset Debug
```

Flash the firmware with STM32CubeProgrammer using the board's USB DFU boot mode. In normal operation, open the USB CDC virtual COM port to read temperature and humidity data.

## Status

Early development. SHT3x measurement and USB output work on hardware. Audio capture, additional sensors and data logging are planned.
