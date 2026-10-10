# 7-Pin SPI OLED Display Upgrade Guide

## Overview
This guide explains how to upgrade from a 4-pin I2C OLED display to a 7-pin SPI OLED display for the XminiC3 board.

## Pin Connections

### 4-Pin I2C OLED (Current)
- **VCC**: 3.3V power
- **GND**: Ground
- **SCL**: I2C clock line
- **SDA**: I2C data line

### 7-Pin SPI OLED (Target)
- **GND**: Ground
- **VCC**: 3.3V power  
- **SCK**: SPI clock (GPIO 18)
- **SDA**: SPI MOSI data (GPIO 23)
- **RES**: Reset pin (GPIO 2)
- **DC**: Data/Command control (GPIO 4)
- **CS**: Chip Select (GPIO 5)

## Hardware Connections

```
ESP32-C3    →    7-Pin OLED Display
GPIO 18     →    SCK (Clock)
GPIO 23     →    SDA (MOSI Data)
GPIO 5      →    CS (Chip Select)
GPIO 4      →    DC (Data/Command)
GPIO 2      →    RES (Reset)
3.3V        →    VCC (Power)
GND         →    GND (Ground)
```

## Software Configuration

### Option 1: Use the Updated Board File
Replace `xmini_c3_board.cc` with `xmini_c3_board_updated.cc` and set:
```cpp
#define USE_SPI_OLED_DISPLAY true  // Set to true for 7-pin SPI, false for 4-pin I2C
```

### Option 2: Use Dedicated SPI Board File
Use `xmini_c3_board_spi.cc` for SPI-only configuration.

## Key Differences

### Communication Protocol
- **I2C**: 2-wire protocol, slower (400kHz max), shared bus
- **SPI**: 4-wire protocol, faster (20MHz), dedicated connection

### Performance Benefits
- **Faster refresh rates**: SPI is significantly faster than I2C
- **Better responsiveness**: Reduced latency for display updates
- **Higher bandwidth**: Supports more complex animations and graphics

### Additional Control Pins
- **CS (Chip Select)**: Enables multiple SPI devices on same bus
- **DC (Data/Command)**: Distinguishes between commands and data
- **RES (Reset)**: Hardware reset capability

## Configuration Parameters

### SPI Configuration
```cpp
.clock_speed_hz = 20 * 1000 * 1000,  // 20MHz SPI clock
.spi_mode = 0,                       // CPOL=0, CPHA=0
.trans_queue_depth = 10,             // Transaction queue depth
```

### Display Configuration
```cpp
.lcd_cmd_bits = 8,                   // 8-bit commands
.lcd_param_bits = 8,                 // 8-bit parameters
.bits_per_pixel = 1,                 // Monochrome display
```

## Migration Steps

1. **Hardware Update**:
   - Disconnect 4-pin I2C OLED
   - Connect 7-pin SPI OLED according to pin mapping
   - Verify all connections

2. **Software Update**:
   - Replace board configuration file
   - Set `USE_SPI_OLED_DISPLAY = true`
   - Rebuild and flash firmware

3. **Verification**:
   - Display should initialize with SPI logging
   - All UI elements should function normally
   - Performance should be noticeably improved

## Troubleshooting

### Display Not Working
- Check pin connections, especially CS and DC
- Verify 3.3V power supply
- Ensure SPI pins are correctly configured

### Garbled Display
- Check SPI clock polarity and phase settings
- Verify data/command pin configuration
- Ensure proper reset sequence

### Performance Issues
- Reduce SPI clock speed if needed
- Check for signal integrity issues
- Verify proper grounding

## Benefits of Upgrade

1. **Performance**: ~50x faster communication speed
2. **Reliability**: Dedicated connection reduces interference
3. **Future-proof**: SPI is more common for high-performance displays
4. **Compatibility**: Maintains same software interface

## Notes

- The `OledDisplay` class remains unchanged - only the initialization differs
- All existing UI code and animations will work without modification
- Power consumption is slightly higher due to additional pins
- SPI uses DMA for efficient data transfer
