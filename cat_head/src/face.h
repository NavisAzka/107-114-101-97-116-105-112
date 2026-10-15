// Cat face on a 128x64 OLED: two eyes, nose, mouth and whiskers.
#pragma once
#include <Arduino.h>

enum class Expr {
  NEUTRAL,    // eyes wide open
  HAPPY,      // ^ ^ arched eyes
  PURR,       // eyes almost shut in contentment
  SURPRISED,  // big eyes, "o" mouth
  ANGRY,      // inner lids slanted down, flat mouth
  SLEEPY,     // half-open drooping eyes
  SLEEP,      // closed eyes + Zzz
};

namespace Face {
void begin(uint8_t sda, uint8_t scl);
void set(Expr e);
Expr current();
const char *name(Expr e);
// Gaze override, x/y in -1..1. lookAuto() goes back to random idle glances.
void look(float x, float y);
void lookAuto();
void blink();
// Call every loop(); animates and redraws at a fixed frame rate.
void update();
}  // namespace Face
