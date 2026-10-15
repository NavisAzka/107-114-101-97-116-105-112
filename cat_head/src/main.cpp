// Interactive cat robot - TOP layer
//
// Face (OLED) + voice (SD card -> I2S speaker) + touch sensor.
// Receives movement events from the bottom layer (robot_avoider) over UART
// and can tell it to STOP (while being petted / asleep) or GO.
//
// Behaviour:
//   tap            -> happy face + meow
//   hold (petting) -> content face + looping purr, robot stops
//   many taps      -> angry face + hiss
//   obstacle event -> surprised face + sound
//   turning event  -> looks towards the turn
//   no touch 45 s  -> sleepy + yawn, 90 s -> asleep (robot stops)
//   touch while asleep -> wakes up surprised; also wakes by itself after a while

#include <Arduino.h>

#include "audio.h"
#include "face.h"

// ---------------- Pins ----------------
constexpr uint8_t PIN_SDA = 21;
constexpr uint8_t PIN_SCL = 22;
constexpr uint8_t PIN_TOUCH = 4;  // TTP223 OUT (active HIGH)
constexpr uint8_t PIN_LINK_RX = 16;  // <- bottom TX (17)
constexpr uint8_t PIN_LINK_TX = 17;  // -> bottom RX (16)

const Audio::Pins AUDIO_PINS = {
    /*sdSck*/ 18, /*sdMiso*/ 19, /*sdMosi*/ 23, /*sdCs*/ 5,
    /*i2sBclk*/ 26, /*i2sLrc*/ 25, /*i2sDout*/ 27,
};

// ---------------- Tunables ----------------
constexpr float VOLUME = 0.7f;
constexpr uint32_t DEBOUNCE_MS = 30;
constexpr uint32_t PETTING_HOLD_MS = 700;
constexpr uint32_t TAP_WINDOW_MS = 5000;
constexpr uint8_t ANNOY_TAPS = 5;
constexpr uint32_t SLEEPY_AFTER_MS = 45000;
constexpr uint32_t SLEEP_AFTER_MS = 90000;
constexpr uint32_t WAKE_MIN_MS = 120000;  // asleep this long at least...
constexpr uint32_t WAKE_RAND_MS = 120000;  // ...plus up to this much
constexpr uint32_t MEOW_MIN_MS = 60000;   // random idle meows
constexpr uint32_t MEOW_RAND_MS = 120000;
constexpr uint32_t SURPRISE_COOLDOWN_MS = 4000;
constexpr uint32_t RESUME_DELAY_MS = 1000;  // after petting, before robot moves again
constexpr uint32_t LINK_RESEND_MS = 2000;

// ---------------- State ----------------
enum class Mood { AWAKE, SLEEPY, ASLEEP };

Mood mood = Mood::AWAKE;
uint32_t lastTouchAt = 0;
uint32_t reactionUntil = 0;  // temporary expression active until this time
uint32_t wakeAt = 0;
uint32_t nextMeowAt = 0;
uint32_t lastSurpriseSound = 0;
uint32_t resumeAt = 0;

bool petting = false;
bool ignoreRelease = false;  // touch that woke the cat shouldn't also count as a tap
uint8_t tapCount = 0;
uint32_t firstTapAt = 0;

uint32_t now() { return millis(); }

Expr baseExpr() {
  switch (mood) {
    case Mood::SLEEPY: return Expr::SLEEPY;
    case Mood::ASLEEP: return Expr::SLEEP;
    default: return Expr::NEUTRAL;
  }
}

void setMood(Mood m) {
  mood = m;
  if (!reactionUntil && !petting) Face::set(baseExpr());
  Serial.printf("mood -> %s\n", m == Mood::AWAKE ? "AWAKE" : m == Mood::SLEEPY ? "SLEEPY" : "ASLEEP");
}

// Show an expression for a while, then fall back to the mood's base expression
void react(Expr e, uint32_t ms) {
  Face::set(e);
  reactionUntil = now() + ms;
}

void scheduleMeow() { nextMeowAt = now() + MEOW_MIN_MS + esp_random() % MEOW_RAND_MS; }

// ---------------- Link to bottom layer ----------------
bool robotStopped = false;
uint32_t lastLinkSend = 0;

void updateLink() {
  bool wantStop = petting || mood == Mood::ASLEEP || now() < resumeAt;
  // Resend periodically in case the bottom layer rebooted
  if (wantStop != robotStopped || now() - lastLinkSend >= LINK_RESEND_MS) {
    robotStopped = wantStop;
    lastLinkSend = now();
    Serial2.println(wantStop ? "STOP" : "GO");
  }
}

void onRobotEvent(const String &ev) {
  Serial.printf("robot: %s\n", ev.c_str());
  if (ev == "OBSTACLE") {
    if (mood == Mood::ASLEEP || petting) return;
    react(Expr::SURPRISED, 900);
    if (now() - lastSurpriseSound >= SURPRISE_COOLDOWN_MS) {
      lastSurpriseSound = now();
      Audio::play(Sound::SURPRISE);
    }
  } else if (ev == "TURN L") {
    Face::look(-1, 0);
  } else if (ev == "TURN R") {
    Face::look(1, 0);
  } else if (ev == "FORWARD") {
    Face::lookAuto();
  }
}

void pollLink() {
  static String line;
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      line.trim();
      if (line.length()) onRobotEvent(line);
      line = "";
    } else if (c != '\r' && line.length() < 32) {
      line += c;
    }
  }
}

// ---------------- Touch ----------------
void wakeUp() {
  setMood(Mood::AWAKE);
  react(Expr::SURPRISED, 1000);
  Face::lookAuto();
  Audio::play(Sound::MEOW);
  scheduleMeow();
}

void onTouchDown() {
  lastTouchAt = now();
  if (mood == Mood::ASLEEP) {
    wakeUp();
    ignoreRelease = true;
    return;
  }
  if (mood == Mood::SLEEPY) setMood(Mood::AWAKE);
}

void onTouchHeld(uint32_t heldMs) {
  if (ignoreRelease || petting || heldMs < PETTING_HOLD_MS) return;
  petting = true;
  reactionUntil = 0;
  Face::set(Expr::PURR);
  Face::look(0, 0);
  Audio::play(Sound::PURR, true);
  Serial.println("petting...");
}

void onTouchUp() {
  lastTouchAt = now();
  if (ignoreRelease) {
    ignoreRelease = false;
    return;
  }
  if (petting) {
    petting = false;
    Audio::stop();
    Face::lookAuto();
    react(Expr::HAPPY, 1500);
    resumeAt = now() + RESUME_DELAY_MS;
    return;
  }
  // Short tap
  if (now() - firstTapAt > TAP_WINDOW_MS) {
    firstTapAt = now();
    tapCount = 0;
  }
  if (++tapCount >= ANNOY_TAPS) {
    tapCount = 0;
    react(Expr::ANGRY, 2500);
    Audio::play(Sound::HISS);
  } else {
    react(Expr::HAPPY, 1200);
    Audio::play(Sound::MEOW);
  }
  scheduleMeow();
}

void pollTouch() {
  static bool stable = false, lastRaw = false;
  static uint32_t changedAt = 0, pressedAt = 0;
  bool raw = digitalRead(PIN_TOUCH) == HIGH;
  if (raw != lastRaw) {
    lastRaw = raw;
    changedAt = now();
  }
  if (now() - changedAt >= DEBOUNCE_MS && raw != stable) {
    stable = raw;
    if (stable) {
      pressedAt = now();
      onTouchDown();
    } else {
      onTouchUp();
    }
  }
  if (stable) onTouchHeld(now() - pressedAt);
}

// ---------------- Idle behaviour ----------------
void updateMood() {
  uint32_t idle = now() - lastTouchAt;
  if (petting) return;

  if (mood == Mood::AWAKE && idle >= SLEEPY_AFTER_MS) {
    setMood(Mood::SLEEPY);
    Audio::play(Sound::YAWN);
  } else if (mood == Mood::SLEEPY && idle >= SLEEP_AFTER_MS) {
    setMood(Mood::ASLEEP);
    Face::look(0, 0.6f);  // head down
    wakeAt = now() + WAKE_MIN_MS + esp_random() % WAKE_RAND_MS;
  } else if (mood == Mood::ASLEEP && now() >= wakeAt) {
    // Woke up on its own
    lastTouchAt = now();
    setMood(Mood::AWAKE);
    Face::lookAuto();
    react(Expr::SLEEPY, 1500);
    Audio::play(Sound::YAWN);
    scheduleMeow();
  } else if (mood == Mood::AWAKE && !reactionUntil && now() >= nextMeowAt) {
    react(Expr::HAPPY, 800);
    Audio::play(Sound::MEOW);
    scheduleMeow();
  }
}

void updateReaction() {
  if (reactionUntil && now() >= reactionUntil && !petting) {
    reactionUntil = 0;
    Face::set(baseExpr());
  }
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);
  // TTP223 drives the line; pulldown keeps it LOW if unplugged
  pinMode(PIN_TOUCH, INPUT_PULLDOWN);

  Face::begin(PIN_SDA, PIN_SCL);
  Audio::begin(AUDIO_PINS);
  Audio::setVolume(VOLUME);

  lastTouchAt = now();
  scheduleMeow();
  Serial.println("Cat head ready");
  Audio::play(Sound::MEOW);
}

void loop() {
  pollLink();
  pollTouch();
  updateMood();
  updateReaction();
  updateLink();
  Face::update();
}
