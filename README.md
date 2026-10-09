# SleepLab

SleepLab is an open-source embedded project focused on exploring sleep monitoring and sleep-related environmental conditions. It combines embedded systems development with an interest in sleep science.

The project is currently in its early development stage.

## Hardware
- **Microcontroller:** STM32F411 (ARM Cortex-M4)
- **Development platform:** STM32

## Software
- C (STM32 HAL)
- STM32CubeMX
- CMake + Ninja
- ARM GNU Toolchain
- VS Code

## Current Progress
- [x] STM32 project initialization
- [x] CMake build configuration
- [x] GPIO configuration and LED blink test
- [ ] Sensor integration
- [ ] Data acquisition and processing
- [ ] Sleep monitoring functionality

## Building

Requires CMake, Ninja and an ARM GNU toolchain.

```bash
cd SleepLab
cmake --preset Debug
cmake --build --preset Debug
```

## Project Status

**Early development** — basic firmware and hardware verification completed. Further functionality will be added incrementally.
