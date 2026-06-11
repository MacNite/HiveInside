// button.h — pairing / identify / factory-reset button.
//
// With BTHome's broadcast model no pairing handshake is strictly required, but
// the button is useful to (a) trigger an immediate "identify" advertisement so
// the user can find the device in Home Assistant, (b) enter encrypted-pairing
// mode to hand over the bind key, and (c) long-press for factory reset / key
// rotation. It can also serve as a hardware wake source from System-OFF sleep.
#pragma once

#include <Arduino.h>

enum class ButtonEvent { None, ShortPress, LongPress };

void buttonInit();

// Poll the button; returns the detected event since last call.
ButtonEvent buttonPoll();
