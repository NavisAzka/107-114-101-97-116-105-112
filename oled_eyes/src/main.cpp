// Animated eyes on a 1.3" I2C OLED + TTP223 capacitive touch sensor
//
// Idle: two wide-open eyes that glance around now and then.
// Touch: plays a randomly chosen blink/wink pattern, then the eyes reopen.

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

// ---------------- Pins ----------------
constexpr uint8_t PIN_SDA = 21;
constexpr uint8_t PIN_SCL = 22;
constexpr uint8_t PIN_TOUCH = 4;  // TTP223 OUT (active HIGH)

#ifdef USE_SSD1306
// Wokwi only provides an SSD1306
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#else
// 1.3" OLED modules use the SH1106 controller
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#endif

// ---------------- Eye geometry ----------------
constexpr int EYE_W = 36;
constexpr int EYE_H = 44;
constexpr int EYE_R = 8;
constexpr int EYE_GAP = 20;
constexpr int SCREEN_W = 128;
constexpr int SCREEN_H = 64;
constexpr int LEFT_CX = SCREEN_W / 2 - EYE_GAP / 2 - EYE_W / 2;
constexpr int RIGHT_CX = SCREEN_W / 2 + EYE_GAP / 2 + EYE_W / 2;
constexpr int EYE_CY = SCREEN_H / 2;

constexpr uint32_t FRAME_MS = 16;  // ~60 fps
constexpr uint32_t DEBOUNCE_MS = 30;

// Openness values
constexpr float OPEN = 1.0f;
constexpr float HALF = 0.45f;
constexpr float SQUINT = 0.25f;
constexpr float CLOSED = 0.0f;

// Easing speeds (fraction of remaining distance per frame)
constexpr float FAST = 0.45f;
constexpr float MED = 0.22f;
constexpr float SLOW = 0.08f;

struct Eye {
  float open = OPEN;
  float target = OPEN;
};

Eye leftEye, rightEye;
float lidSpeed = FAST;

// Gaze offset (whole-eye shift), eased toward a random target while idle
float gazeX = 0, gazeY = 0, gazeTX = 0, gazeTY = 0;
uint32_t nextGlanceAt = 0;

// ---------------- Patterns ----------------
// Each keyframe: target openness for left/right eye, easing speed, duration.
struct Keyframe {
  float left;
  float right;
  float speed;
  uint16_t ms;
};

struct Pattern {
  const char *name;
  const Keyframe *frames;
  uint8_t count;
};

#define PATTERN(name, ...)                                   \
  static const Keyframe name##_kf[] = {__VA_ARGS__};         \
  static const Pattern name = {#name, name##_kf,             \
                               sizeof(name##_kf) / sizeof(Keyframe)}

// Every pattern ends by returning to OPEN
PATTERN(blink,          {CLOSED, CLOSED, FAST, 120}, {OPEN, OPEN, FAST, 150});
PATTERN(doubleBlink,    {CLOSED, CLOSED, FAST, 100}, {OPEN, OPEN, FAST, 120},
                        {CLOSED, CLOSED, FAST, 100}, {OPEN, OPEN, FAST, 150});
PATTERN(winkLeft,       {CLOSED, OPEN, FAST, 450},   {OPEN, OPEN, MED, 200});
PATTERN(winkRight,      {OPEN, CLOSED, FAST, 450},   {OPEN, OPEN, MED, 200});
PATTERN(sleepy,         {HALF, HALF, SLOW, 900},     {OPEN, OPEN, MED, 250});
PATTERN(halfLeftClose,  {HALF, CLOSED, MED, 700},    {OPEN, OPEN, MED, 250});
PATTERN(closeHalfRight, {CLOSED, HALF, MED, 700},    {OPEN, OPEN, MED, 250});
PATTERN(alternate,      {CLOSED, OPEN, FAST, 160},   {OPEN, CLOSED, FAST, 160},
                        {CLOSED, OPEN, FAST, 160},   {OPEN, CLOSED, FAST, 160},
                        {OPEN, OPEN, FAST, 150});
PATTERN(dozeOff,        {HALF, HALF, SLOW, 700},     {CLOSED, CLOSED, SLOW, 1000},
                        {CLOSED, CLOSED, SLOW, 500}, {OPEN, OPEN, FAST, 200});
PATTERN(squint,         {SQUINT, SQUINT, MED, 800},  {OPEN, OPEN, MED, 250});
PATTERN(skeptical,      {OPEN, SQUINT, MED, 800},    {OPEN, OPEN, MED, 250});
PATTERN(halfWink,       {SQUINT, CLOSED, FAST, 500}, {OPEN, OPEN, MED, 250});
PATTERN(fluttery,       {CLOSED, CLOSED, FAST, 70},  {HALF, HALF, FAST, 70},
                        {CLOSED, CLOSED, FAST, 70},  {HALF, HALF, FAST, 70},
                        {CLOSED, CLOSED, FAST, 70},  {OPEN, OPEN, FAST, 150});

static const Pattern *const PATTERNS[] = {
    &blink,         &doubleBlink,    &winkLeft,  &winkRight, &sleepy,
    &halfLeftClose, &closeHalfRight, &alternate, &dozeOff,   &squint,
    &skeptical,     &halfWink,       &fluttery,
};
constexpr uint8_t PATTERN_COUNT = sizeof(PATTERNS) / sizeof(PATTERNS[0]);

const Pattern *activePattern = nullptr;
uint8_t frameIndex = 0;
uint32_t frameStart = 0;
int lastPattern = -1;

void applyKeyframe() {
  const Keyframe &kf = activePattern->frames[frameIndex];
  leftEye.target = kf.left;
  rightEye.target = kf.right;
  lidSpeed = kf.speed;
  frameStart = millis();
}

void startRandomPattern() {
  int idx;
  do {
    idx = esp_random() % PATTERN_COUNT;
  } while (idx == lastPattern);
  lastPattern = idx;
  activePattern = PATTERNS[idx];
  frameIndex = 0;
  applyKeyframe();
  // Look straight ahead while blinking
  gazeTX = gazeTY = 0;
  Serial.printf("Touch -> %s\n", activePattern->name);
}

void updatePattern() {
  if (!activePattern) return;
  if (millis() - frameStart < activePattern->frames[frameIndex].ms) return;
  if (++frameIndex < activePattern->count) {
    applyKeyframe();
  } else {
    activePattern = nullptr;
    leftEye.target = rightEye.target = OPEN;
    lidSpeed = MED;
    nextGlanceAt = millis() + 1500;
  }
}

// ---------------- Idle glances ----------------
void updateGaze() {
  if (!activePattern && millis() >= nextGlanceAt) {
    // Mostly small glances, sometimes back to centre
    if (esp_random() % 3 == 0) {
      gazeTX = gazeTY = 0;
    } else {
      gazeTX = (int)(esp_random() % 17) - 8;  // -8..8 px
      gazeTY = (int)(esp_random() % 9) - 4;   // -4..4 px
    }
    nextGlanceAt = millis() + 1200 + esp_random() % 2800;
  }
  gazeX += (gazeTX - gazeX) * 0.2f;
  gazeY += (gazeTY - gazeY) * 0.2f;
}

// ---------------- Drawing ----------------
void ease(Eye &e) {
  e.open += (e.target - e.open) * lidSpeed;
  if (fabsf(e.target - e.open) < 0.01f) e.open = e.target;
}

void drawEye(int cx, int cy, float open) {
  int h = max(2, (int)roundf(EYE_H * open));
  int x = cx - EYE_W / 2;
  int y = cy - h / 2;
  // U8g2 needs w,h >= 2r+1 for rounded boxes
  int r = min(EYE_R, (h - 1) / 2);
  if (r >= 1)
    u8g2.drawRBox(x, y, EYE_W, h, r);
  else
    u8g2.drawBox(x, y, EYE_W, h);
}

void render() {
  u8g2.clearBuffer();
  int dx = (int)roundf(gazeX);
  int dy = (int)roundf(gazeY);
  drawEye(LEFT_CX + dx, EYE_CY + dy, leftEye.open);
  drawEye(RIGHT_CX + dx, EYE_CY + dy, rightEye.open);
  u8g2.sendBuffer();
}

// ---------------- Touch ----------------
bool touchPressed() {
  static bool stable = false;
  static bool lastRaw = false;
  static uint32_t changedAt = 0;
  bool raw = digitalRead(PIN_TOUCH) == HIGH;
  if (raw != lastRaw) {
    lastRaw = raw;
    changedAt = millis();
  }
  bool rising = false;
  if (millis() - changedAt >= DEBOUNCE_MS && raw != stable) {
    stable = raw;
    rising = stable;
  }
  return rising;
}

void setup() {
  Serial.begin(115200);
  // TTP223 drives the line itself; pulldown keeps it LOW if unplugged
  // (and makes a Wokwi pushbutton to 3V3 behave the same way)
  pinMode(PIN_TOUCH, INPUT_PULLDOWN);

  Wire.begin(PIN_SDA, PIN_SCL);
  u8g2.begin();
  u8g2.setBusClock(400000);

  nextGlanceAt = millis() + 2000;
  Serial.println("OLED eyes ready - touch the sensor!");
}

void loop() {
  static uint32_t lastFrame = 0;

  // Ignore touches while a pattern is playing
  if (touchPressed() && !activePattern) startRandomPattern();

  if (millis() - lastFrame < FRAME_MS) return;
  lastFrame = millis();

  updatePattern();
  updateGaze();
  ease(leftEye);
  ease(rightEye);
  render();
}
