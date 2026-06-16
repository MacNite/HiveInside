// battery.h — optional LiPo voltage sense via an external divider on an ADC pin.
//
// NOTE on the ESP32-C6 SuperMini: its BAT+/BAT- pads are a battery *input* only
// (the cell feeds the 3.3 V LDO, on most clones through a series Schottky
// diode). There is NO charge IC and NO on-board sense divider, so you can
// neither charge through those pads nor read the cell voltage directly. To
// monitor the battery, add an external resistor divider (e.g. 2×100 kΩ) from
// BAT+ to PIN_VBAT_ADC. See docs/esp32c6-prototype.md.
#pragma once

#include "config.h"
#include "measurement.h"

#if ENABLE_BATTERY

namespace battery {
void begin();
void read(Measurement& m); // fills m.battery_v / m.battery_pct / m.battery_ok
} // namespace battery

#endif // ENABLE_BATTERY
