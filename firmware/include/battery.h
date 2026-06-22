// battery.h — optional LiPo voltage sense via an external divider on an ADC pin.
//
// NOTE on the XIAO ESP32-C6: its BAT+/BAT- pads charge the cell over USB-C
// (on-board charger), but there is NO battery-sense divider, so the cell
// voltage cannot be read directly. To monitor it, add an external resistor
// divider (e.g. 2×220 kΩ) from BAT+ to PIN_VBAT_ADC (A0 / GPIO0). See
// docs/esp32c6-prototype.md.
#pragma once

#include "config.h"
#include "measurement.h"

#if ENABLE_BATTERY

namespace battery {
void begin();
void read(Measurement& m); // fills m.battery_v / m.battery_pct / m.battery_ok
} // namespace battery

#endif // ENABLE_BATTERY
