// button.cpp — simple debounced short/long press detector.
#include "button.h"
#include "config.h"

#if ENABLE_BUTTON

static uint32_t pressStart = 0;
static bool wasDown = false;

static bool rawDown() {
  int v = digitalRead(PIN_BUTTON);
  return BUTTON_ACTIVE_LOW ? (v == LOW) : (v == HIGH);
}

void buttonInit() {
  pinMode(PIN_BUTTON, BUTTON_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  wasDown = false;
  pressStart = 0;
}

ButtonEvent buttonPoll() {
  bool down = rawDown();

  if (down && !wasDown) {
    wasDown = true;
    pressStart = millis();
    return ButtonEvent::None;
  }

  if (!down && wasDown) {
    wasDown = false;
    uint32_t held = millis() - pressStart;
    if (held >= BUTTON_LONGPRESS_MS) return ButtonEvent::LongPress;
    if (held >= 30) return ButtonEvent::ShortPress;  // 30 ms debounce
  }

  return ButtonEvent::None;
}

#else

void buttonInit() {}
ButtonEvent buttonPoll() { return ButtonEvent::None; }

#endif
