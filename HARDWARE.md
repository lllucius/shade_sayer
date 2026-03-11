# Hardware Reference

This document describes the hardware design for the Color Detection Device using the Unexpected Maker TinyS3D.

## System Block Diagram

```
                    ┌─────────────────────────────────────────┐
                    │     Unexpected Maker TinyS3D (ESP32-S3)  │
                    │  ┌─────────────────────────────────────┐ │
                    │  │          Application                 │ │
                    │  │  ┌────────┐ ┌────────┐ ┌──────────┐ │ │
                    │  │  │TCS3530 │ │ Color  │ │   TTS    │ │ │
                    │  │  │ Driver │ │Pipeline│ │ Manager  │ │ │
                    │  │  └────────┘ └────────┘ └──────────┘ │ │
                    │  └─────────────────────────────────────┘ │
                    │                                           │
    POWER_ENABLE ──────GPIO6 (RTC GPIO) ────── AO3401 Gate ────┤
                    │                                           │
    I2C Bus         │  GPIO8 (SDA) ──── 2N7002 ────── TCS3530  │
    ───────────────────GPIO9 (SCL) ──── 2N7002 ────── TCS3530  │
                    │                                           │
    Button ────────────GPIO1 (RTC GPIO) ───────────────────────┤
                    │                                           │
    USB VBUS ──────────GPIO7 (RTC GPIO, voltage divider) ──────┤
                    │                                           │
    SD_MODE ───────────GPIO2 (RTC GPIO) ───────────────────────┤
                    │                                           │
    I2S Audio ─────────GPIO34,36,37 (I2S) ─────────────────────┤
                    │                                           │
                    └─────────────────────────────────────────┘
```

## TCS3530 Connection

The TCS3530 uses I2C for communication and operates at 1.8V.

### Pin Connections

| TCS3530 Pin | Signal | ESP32 Pin | Notes |
|-------------|--------|-----------|-------|
| 1 | INT | Not connected | Driver uses I2C polling instead of interrupts |
| 2 | PGND | GND | Connect to ground |
| 3 | VDD | 1.8V | Supply voltage |
| 4 | VSS | GND | Connect to ground |
| 5 | VBUS | 1.8V | I2C bus voltage |
| 6 | SDA | GPIO8 | Via level shifter if ESP32 is 3.3V |
| 7 | SCL | GPIO9 | Via level shifter if ESP32 is 3.3V |
| 8 | VSYNC/GPIO | LED output | Internal LED control for illumination |

### Internal LED

The TCS3530 includes an internal LED controlled via the VSYNC/GPIO pin (Pin 8). The driver uses the `TCS3530::setLed(bool on)` method to control this LED for sample illumination. This approach:
- Eliminates the need for an external LED and GPIO
- Provides consistent, close-proximity illumination
- Is controlled via the VSYNC_GPIO_INT register (0xB0) in the TCS3530

### Level Shifting

The TCS3530 operates at 1.8V I/O while ESP32 is 3.3V. You need level shifters:
- Recommended: TXS0102 or similar bidirectional level shifter
- Or use I2C-specific level shifters like PCA9306

### I2C Bus Pullup Resistors

**IMPORTANT:** External I2C pullup resistors are **required** for proper operation.

**Required Configuration:**
- **Value**: 4.7kΩ pullup resistors on both SDA and SCL
- **Connection**: Connect pullups to the switched VCC rail (AO3401 drain) on the sensor side of the 2N7002 isolation MOSFETs
- **Placement**: Install pullups on the sensor side so they are de-powered when the rail is OFF

### Switched Peripheral Rail and I2C Isolation

An AO3401 P-MOSFET high-side switch and 2× 2N7002 N-MOSFET I2C isolation transistors eliminate deep sleep leakage (~282 µA → ~40 µA).

#### AO3401 P-MOSFET Power Switch

| Pin | Connection | Notes |
|-----|------------|-------|
| Source | BAT (battery rail) | Always connected to battery |
| Drain | TCS3530 VCC, 2N7002 gates, I2C pullups | Switched peripheral rail |
| Gate | GPIO6 (POWER_ENABLE) | LOW = ON, HIGH = OFF |

#### 2N7002 I2C Isolation MOSFETs (×2)

One 2N7002 per I2C line (SDA and SCL):

| Pin | Connection | Notes |
|-----|------------|-------|
| Drain | ESP32 GPIO (GPIO8 or GPIO9) | ESP32 side |
| Source | TCS3530 I2C pin | Sensor side |
| Gate | Switched VCC (AO3401 drain) | Star topology (not daisy-chained) |

When the switched rail is OFF, the 2N7002 gates are at 0 V and the MOSFETs are open, disconnecting the ESP32 from the sensor.  This prevents phantom powering of the TCS3530 through the I2C lines.

#### Circuit Diagram

```
    BAT ────┐
            │S
        ┌───┴───┐
        │AO3401 │  Gate ◄──── GPIO6 (POWER_ENABLE)
        │P-FET  │  LOW = ON, HIGH = OFF
        └───┬───┘
            │D
            │ Switched VCC
            ├──────────────── TCS3530 VDD
            │
            ├───── 4.7kΩ ──── SDA (sensor side) ──── TCS3530 SDA
            │                        │S
            │                    ┌───┴───┐
            │         Gate ──────┤2N7002 │
            │                    └───┬───┘
            │                        │D
            │                   GPIO8 (ESP32 SDA)
            │
            ├───── 4.7kΩ ──── SCL (sensor side) ──── TCS3530 SCL
            │                        │S
            │                    ┌───┴───┐
            │         Gate ──────┤2N7002 │
            │                    └───┬───┘
            │                        │D
            │                   GPIO9 (ESP32 SCL)
            │
            └── (star: both 2N7002 gates tied directly to switched VCC)
```

### Power Supply

The TCS3530 requires:
- VDD: 1.7V - 1.98V (typical 1.8V)
- VBUS: 1.08V - 3.3V (match to I2C voltage)
- Decoupling: 4.7µF on VDD, 1µF on VBUS

## Button Interface

### Circuit
```
          3.3V
           │
          ┌┴┐
          │/│ Button
          │ │ (NO)
          └┬┘
           │
          ┌┴┐
          │ │ 1kΩ (series protection)
          │ │
          └┬┘
           │
           ├─────── GPIO1 (RTC GPIO)
           │
           ├────┬── 100nF (debounce capacitor)
           │    │
           │   ─┴─
           │
          ┌┴┐
          │ │ 47kΩ (external pull-down)
          │ │
          └┬┘
           │
          GND
```

### Button Features
- GPIO1 with external 47kΩ pull-down resistor
- RTC GPIO - supports ext1 deep sleep wakeup (active HIGH)
- Button pressed pulls GPIO1 HIGH through 1kΩ series resistor
- **100nF debounce capacitor** (connect between GPIO1 node and GND)
  - Provides RC debounce at the GPIO pin
  - Without debounce cap, you may see "Button still pressed" warnings
  - Prevents false triggers and ensures clean button release detection
  - Critical for reliable sleep/wake operation
- **1kΩ series protection resistor** protects GPIO from short-circuit current
- Supports single, double, triple click and long press detection
- Uses custom synchronous polling for robust event handling
- Matches USB VBUS detection polarity (both active HIGH) allowing ext1-only wakeup

## Unexpected Maker TinyS3D Pinout

The configuration is designed for the Unexpected Maker TinyS3D development board.
Below is the GPIO mapping for the color detector:

| Function | GPIO | RTC GPIO | Notes |
|----------|------|----------|-------|
| Button | GPIO1 | Yes | RTC_GPIO1, ext1 wakeup source |
| I2S SD_MODE | GPIO2 | Yes | RTC_GPIO2, MAX98357A shutdown control |
| POWER_ENABLE | GPIO6 | Yes | AO3401 P-MOSFET gate, deep sleep hold enabled |
| USB VBUS Detect | GPIO7 | Yes | USB detection via voltage divider, ext1 wakeup |
| I2C SDA | GPIO8 | Yes | TCS3530 + MAX17048 shared bus (2N7002 isolated) |
| I2C SCL | GPIO9 | Yes | TCS3530 + MAX17048 shared bus (2N7002 isolated) |
| MAX17048 ALRT | GPIO10 | Yes | Battery alert (optional, not used for wakeup) |
| I2S LRCLK | GPIO34 | No | I2S word select |
| I2S BCLK | GPIO36 | No | I2S bit clock |
| I2S DOUT | GPIO37 | No | I2S data output to MAX98357A |

### RTC GPIO Notes

**ESP32-S3 RTC GPIOs (GPIO 0-21):** These pins can be used with ext1 wakeup from deep sleep and support gpio_deep_sleep_hold_en() to maintain their state during deep sleep.

**Non-RTC GPIOs (GPIO 22+):** Cannot wake from deep sleep or maintain state during deep sleep.

**Key Design Decisions:**
- **GPIO 1**: Button input - RTC GPIO required for ext1 deep sleep wakeup
- **GPIO 2**: SD_MODE control - RTC GPIO allows deep sleep hold to keep amplifier disabled
- **GPIO 6**: POWER_ENABLE - RTC GPIO controls AO3401 P-MOSFET gate, deep sleep hold keeps sensor rail OFF
- **GPIO 7**: USB VBUS detection via voltage divider - RTC GPIO allows ext1 wakeup from deep sleep when USB connected

## Audio Output - MAX98357A I2S Amplifier

The audio output uses a MAX98357A I2S Class D amplifier. This provides high-quality digital audio
without the noise of DAC-based solutions.

### I2S Connection to MAX98357A
| MAX98357A Pin | Signal | ESP32 Pin | Notes |
|---------------|--------|-----------|-------|
| BCLK | I2S Bit Clock | GPIO36 | Serial clock |
| LRC/LRCLK | Word Select | GPIO34 | Left/Right clock |
| DIN | Data In | GPIO37 | Audio data |
| GAIN | Gain Select | - | Set on board (tie to VDD for 15dB) |
| SD | Shutdown | GPIO2 | RTC GPIO, deep sleep hold enabled |
| VDD | Power | 3.3V | Supply voltage |
| GND | Ground | GND | Common ground |

### MAX98357A Pinout
```
         ┌─────────────────┐
         │   MAX98357A     │
         │                 │
 ESP32   │ BCLK ◄───────── │ ──── GPIO36
 GPIO34 ─│ ►LRCLK          │
 GPIO37 ─│ ►DIN            │
 GPIO2  ─│ ►SD             │  (RTC GPIO)
         │                 │
    3.3V─│ VDD        SPK+ │──┐
     GND─│ GND        SPK- │──┴── 4Ω-8Ω Speaker
         └─────────────────┘
```

### Connection Notes
- No external amplifier circuit needed - MAX98357A is a complete amplifier
- I2S provides clean digital audio path (no DAC noise)
- Speaker: 4Ω to 8Ω, minimum 1W rating
- Gain is typically set on the board (default 9dB, can be changed)
- SD_MODE (GPIO2) enables amplifier shutdown with deep sleep hold for ultra-low power

### TTS Library: picotts
The audio uses the picotts library (https://github.com/lllucius/picotts)
for text-to-speech synthesis:
- Output: 16-bit mono PCM at 16000 Hz
- Lightweight SVox Pico engine
- Supports English and other languages
- Memory requirement: ~2.5MB (uses PSRAM if available)

## Power Management

The device uses ESP32-S3 deep sleep for ultra-low power consumption.

### Deep Sleep Architecture

```
         ┌───────────────────────────────┐
Battery ─┤ Power Supply (3.3V Regulator) │
         └───────┬───────────────────────┘
                 │
                 ├─── ESP32-S3 (VCC)
                 │    │
                 │    ├─── GPIO1 (Button with pull-down)
                 │    │    └─── EXT1 Wakeup Source
                 │    │
                 │    └─── GPIO6 (POWER_ENABLE)
                 │         │
                 │    ┌────┴────┐
                 │    │ AO3401  │ P-MOSFET high-side switch
                 │    │  S=BAT  │
                 │    └────┬────┘
                 │         │ Switched VCC
                 │         ├─── TCS3530 (1.8V LDO)
                 │         └─── 2N7002 Gates (I2C isolation)
                 │
                 └─── MAX98357A Amplifier
                      │
                      └─── GPIO2 (SD_MODE)
```

### Power States

1. **Active**: Full operation
   - ESP32-S3: ~80mA
   - TCS3530: ~0.5mA (with internal LED active: ~20mA additional)
   - Total: ~80-100mA depending on LED state

2. **Deep Sleep**: Ultra-low power
   - ESP32-S3: ~20µA (with RTC RAM retention)
   - TCS3530: Unpowered (rail OFF), 0 µA
   - 2N7002 I2C isolation: gates at 0 V, no leakage path
   - MAX98357A: SD_MODE LOW, ~1µA
   - Total: ~40µA

### Wakeup Configuration

The ESP32 wakes from deep sleep via ext1 wakeup on either button or USB GPIO:
- Button press (GPIO1 goes HIGH) triggers ext1 wakeup interrupt
- USB plug-in (GPIO7 goes HIGH) triggers ext1 wakeup interrupt
- Both sources use ext1 with ANY_HIGH trigger mode
- ESP32 boots from deep sleep in ~100ms
- All state is re-initialized (no state retention needed)

### Wake Sequence

After waking from deep sleep:
1. Release GPIO holds (POWER_ENABLE, I2C SDA/SCL) before reconfiguring pins
2. Drive POWER_ENABLE LOW (AO3401 gate) to turn ON sensor rail
3. Initialize I2C bus (2N7002 gates powered, I2C lines connected)
4. Initialize TCS3530 and MAX17048 on the shared I2C bus

### Shutdown Sequence

When entering deep sleep:
1. Turn off TCS3530 internal LED
2. Put TCS3530 to sleep mode
3. Set MAX98357A SD_MODE LOW (RTC hold enabled)
4. Delete I2C driver and set SDA/SCL to INPUT with no pulls
5. Drive POWER_ENABLE HIGH (AO3401 gate OFF), enable gpio_hold_en() + gpio_deep_sleep_hold_en()
6. Configure ext1 wakeup on button and USB GPIOs (active HIGH)
7. Isolate unused GPIOs (skip held GPIOs)
8. Enter deep sleep

### Battery Monitoring

The device uses the MAX17048G+T10 fuel gauge IC for accurate battery monitoring:
- I2C interface (address 0x36)
- Direct state-of-charge (SOC) percentage reading
- Battery voltage measurement
- No calibration required for typical Li-ion cells
- Ultra-low power consumption (~23µA)

**MAX17048 I2C Connection:**
```
MAX17048     ESP32-S3 TinyS3D
--------     ----------------
SDA      <-> GPIO8 (I2C SDA, shared with TCS3530)
SCL      <-> GPIO9 (I2C SCL, shared with TCS3530)
ALRT     <-> GPIO10 (optional - can be left unconnected, not used for wakeup)
VDD      <-> 3.3V
GND      <-> GND
CELL     <-> Battery + (via TinyS3D BAT pin)
GND      <-> Battery - (via TinyS3D GND pin)
```

The fuel gauge shares the I2C bus with the TCS3530 color sensor, eliminating the need for additional GPIOs or ADC channels. The ALRT pin is optional and can be left unconnected since it's no longer used for charging detection or deep sleep wakeup.

Features:
- Accurate SOC percentage (0-100%)
- Battery voltage reading
- Automatic detection when available
- Announce low battery warnings
- Detect USB vs battery power via GPIO7 voltage divider

### USB Power Detection

USB VBUS detection is configured on GPIO7 via voltage divider:
- Circuit: 5V -> 1MΩ -> GPIO7 -> 1MΩ -> GND, with 100nF capacitor from GPIO7 to GND
- Voltage divider reduces 5V USB to 2.5V (safe for ESP32-S3 GPIO input)
- 100nF capacitor filters noise and transients
- GPIO7 is an RTC GPIO, allowing ext1 wakeup from deep sleep when USB is connected
- Used to differentiate between button press and USB plug-in boot
- When USB is detected, device announces charging status
- Battery level can be checked with triple-click while charging

## Enclosure Design

### Sensor Aperture
- Keep sensor aperture ~5mm from sample
- Use light shield to prevent stray light
- Consider using a diffuser for uniform sampling
- The TCS3530 internal LED provides illumination

### User Ergonomics
- Large, tactile button (>15mm diameter)
- Textured grip surfaces
- Audio output facing user
- Lanyard attachment point
- Clear marking for sensor location

## PCB Layout Guidelines

1. **Power Supply**
   - Separate analog and digital ground planes
   - Keep bypass capacitors close to IC power pins
   - Use ground pour for EMI reduction

2. **I2C Traces**
   - Keep I2C traces short (<10cm)
   - Route SDA and SCL together
   - Add test points for debugging
   - Include series termination resistors (22-33Ω) near ESP32

3. **I2S Audio Traces**
   - Keep I2S traces short and matched length
   - Route away from high-speed signals
   - Ground plane underneath for EMI suppression

4. **Sensor Placement**
   - Orient sensor aperture perpendicular to PCB
   - Keep metal away from sensor aperture
   - Provide light shielding around sensor

## Bill of Materials (Reference)

| Component | Part Number | Quantity | Notes |
|-----------|-------------|----------|-------|
| Unexpected Maker TinyS3D | TinyS3D | 1 | ESP32-S3 with 8MB flash, 8MB PSRAM |
| TCS3530 | TCS3530FN | 1 | True color sensor with internal LED |
| MAX17048 | MAX17048G+T10 | 1 | Battery fuel gauge (optional) |
| AO3401 P-MOSFET | AO3401A | 1 | High-side switch for sensor power rail (SOT-23) |
| 2N7002 N-MOSFET | 2N7002 | 2 | I2C isolation (one per SDA/SCL line, SOT-23) |
| Level Shifter | TXS0102 | 1 | I2C level shift for 3.3V to 1.8V |
| 1.8V LDO | MCP1700-1802 | 1 | TCS3530 power (on switched rail) |
| Button | SPST-NO | 1 | 12mm tactile, normally open |
| Button Pull-down | 47kΩ | 1 | External pull-down resistor |
| Button Series R | 1kΩ | 1 | Series protection resistor |
| Button Debounce Cap | 100nF | 1 | Ceramic capacitor for RC debounce |
| MAX98357A | MAX98357A | 1 | I2S Class-D Amplifier |
| Speaker | 8Ω 1-2W | 1 | 28mm diameter |
| 3.7V LiPo Battery | - | 1 | 500mAh or larger, connects to TinyS3D BAT/GND |
| I2C Pullup Resistors | 4.7kΩ | 2 | On switched rail, sensor side of 2N7002 |
| Resistors | Various | - | 0603 SMD |
| Capacitors | Various | - | 0603 SMD |

## References

- TCS3530 Datasheet (included as TCS3530_DS.txt)
- ESP32-S3 Technical Reference Manual
- MAX98357A Datasheet
- MAX17048 Datasheet
- Unexpected Maker TinyS3D Documentation
