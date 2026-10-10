# Pin Mapping Diagram: 4-Pin I2C → 7-Pin SPI OLED

## Visual Connection Guide

```
ESP32-C3 Board                          7-Pin SPI OLED Display
┌─────────────┐                        ┌─────────────────┐
│             │                        │                 │
│ GPIO 18     ├───────────────────────→│ SCK (Clock)    │
│             │                        │                 │
│ GPIO 23     ├───────────────────────→│ SDA (MOSI)     │
│             │                        │                 │
│ GPIO 5      ├───────────────────────→│ CS (Chip Sel)  │
│             │                        │                 │
│ GPIO 4      ├───────────────────────→│ DC (Data/Cmd)  │
│             │                        │                 │
│ GPIO 2      ├───────────────────────→│ RES (Reset)    │
│             │                        │                 │
│ 3.3V        ├───────────────────────→│ VCC (Power)    │
│             │                        │                 │
│ GND         ├───────────────────────→│ GND (Ground)   │
│             │                        │                 │
└─────────────┘                        └─────────────────┘
```

## Pin Function Comparison

| 4-Pin I2C OLED | 7-Pin SPI OLED | ESP32-C3 GPIO | Function |
|----------------|----------------|---------------|----------|
| VCC            | VCC            | 3.3V          | Power    |
| GND            | GND            | GND           | Ground   |
| SCL            | SCK            | GPIO 18       | Clock    |
| SDA            | SDA            | GPIO 23       | Data     |
| -              | CS             | GPIO 5        | Chip Select |
| -              | DC             | GPIO 4        | Data/Command |
| -              | RES            | GPIO 2        | Reset    |

## Signal Types

### Clock Signals
- **I2C SCL**: Bidirectional clock, shared bus
- **SPI SCK**: Unidirectional clock, dedicated connection

### Data Signals  
- **I2C SDA**: Bidirectional data, shared bus
- **SPI SDA/MOSI**: Unidirectional data (Master → Slave)

### Control Signals (SPI Only)
- **CS**: Chip Select - activates specific SPI device
- **DC**: Data/Command - distinguishes commands from data
- **RES**: Reset - hardware reset of display

## Connection Checklist

### Required Connections
- [ ] GPIO 18 → SCK (Clock)
- [ ] GPIO 23 → SDA (MOSI Data)  
- [ ] GPIO 5 → CS (Chip Select)
- [ ] GPIO 4 → DC (Data/Command)
- [ ] GPIO 2 → RES (Reset)
- [ ] 3.3V → VCC (Power)
- [ ] GND → GND (Ground)

### Optional Connections
- [ ] Add 0.1μF capacitor between VCC and GND (power filtering)
- [ ] Add 10kΩ pull-up resistor on CS line (if needed)

## Wiring Tips

1. **Use short wires** to reduce signal interference
2. **Keep power and ground wires separate** from signal wires
3. **Double-check pin assignments** before powering on
4. **Test continuity** with multimeter if possible
5. **Secure connections** to prevent loose wires

## Common Mistakes to Avoid

1. **Swapping SCK and SDA** - Display won't initialize
2. **Wrong CS pin** - SPI communication fails
3. **Missing DC connection** - Data gets corrupted
4. **Incorrect voltage** - 5V will damage the display
5. **Loose ground connection** - Unstable operation

## Testing Procedure

1. **Power off** the ESP32-C3
2. **Make all connections** according to diagram
3. **Double-check** all pin assignments
4. **Power on** and monitor serial output
5. **Look for SPI initialization messages**
6. **Verify display shows expected content**

## Expected Serial Output

When using SPI display, you should see:
```
I (xxx) XminiC3Board: Initializing 7-pin SPI OLED display
I (xxx) XminiC3Board: Install SSD1306 SPI driver  
I (xxx) XminiC3Board: SSD1306 SPI driver installed
I (xxx) XminiC3Board: 7-pin SPI OLED display initialized successfully
```
