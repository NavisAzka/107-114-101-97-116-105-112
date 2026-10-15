#include "face.h"

#include <U8g2lib.h>
#include <Wire.h>

#ifdef USE_SSD1306
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#else
// 1.3" OLED modules use the SH1106 controller
static U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#endif

namespace {

// ---------------- Geometry ----------------
constexpr int EYE_W = 30;
constexpr int EYE_H = 34;
constexpr int EYE_R = 8;
constexpr int LEFT_CX = 34;
constexpr int RIGHT_CX = 94;
constexpr int EYE_CY = 24;
constexpr int MOUTH_X = 64;
constexpr float GAZE_X_PX = 8;
constexpr float GAZE_Y_PX = 3;

constexpr uint32_t FRAME_MS = 33;  // ~30 fps (full SH1106 frame over I2C ~25 ms)
constexpr uint32_t BLINK_MS = 160;
constexpr float EASE = 0.25f;

enum class Mouth { CAT, O, FLAT };

// Eye shape parameters, interpolated between expressions
struct Shape {
  float open;   // 0 closed .. 1 open
  float happy;  // 0 normal .. 1 thin ^ arc
  float tilt;   // >0 angry (inner lid down), <0 sad (outer lid down)
  float scale;  // overall eye size
};

struct ExprDef {
  const char *name;
  Shape shape;
  Mouth mouth;
  bool autoBlink;
  bool zzz;
};

const ExprDef EXPRS[] = {
    // name        open  happy tilt   scale  mouth        blink  zzz
    {"NEUTRAL",   {1.0f, 0.0f, 0.0f, 1.0f},  Mouth::CAT,  true,  false},
    {"HAPPY",     {1.0f, 0.55f, 0.0f, 1.0f}, Mouth::CAT,  true,  false},
    {"PURR",      {1.0f, 0.9f, 0.0f, 1.0f},  Mouth::CAT,  false, false},
    {"SURPRISED", {1.0f, 0.0f, 0.0f, 1.15f}, Mouth::O,    false, false},
    {"ANGRY",     {0.8f, 0.0f, 0.8f, 1.0f},  Mouth::FLAT, true,  false},
    {"SLEEPY",    {0.4f, 0.0f, -0.35f, 1.0f}, Mouth::CAT, true,  false},
    {"SLEEP",     {0.0f, 0.0f, 0.0f, 1.0f},  Mouth::CAT,  false, true},
};

Expr expr = Expr::NEUTRAL;
Shape cur = EXPRS[0].shape;

float gazeX = 0, gazeY = 0, gazeTX = 0, gazeTY = 0;
bool gazeAuto = true;
uint32_t nextGlanceAt = 0;

uint32_t blinkStart = 0;
bool blinking = false;
bool doubleBlink = false;
uint32_t nextBlinkAt = 0;

uint32_t lastFrame = 0;

const ExprDef &def() { return EXPRS[static_cast<int>(expr)]; }

float lerp(float a, float b, float t) { return a + (b - a) * t; }

void easeShape() {
  const Shape &t = def().shape;
  cur.open = lerp(cur.open, t.open, EASE);
  cur.happy = lerp(cur.happy, t.happy, EASE);
  cur.tilt = lerp(cur.tilt, t.tilt, EASE);
  cur.scale = lerp(cur.scale, t.scale, EASE);
}

// 1 = fully open, dips to 0 mid-blink
float blinkFactor() {
  if (!blinking) return 1.0f;
  uint32_t t = millis() - blinkStart;
  if (t >= BLINK_MS) {
    if (doubleBlink) {
      doubleBlink = false;
      blinkStart = millis();
    } else {
      blinking = false;
    }
    return 1.0f;
  }
  float p = (float)t / BLINK_MS;  // 0..1
  return fabsf(p * 2 - 1);        // 1 -> 0 -> 1
}

void updateBlink() {
  if (millis() < nextBlinkAt) return;
  nextBlinkAt = millis() + 2500 + esp_random() % 3500;
  if (def().autoBlink && !blinking) {
    blinking = true;
    doubleBlink = esp_random() % 5 == 0;
    blinkStart = millis();
  }
}

void updateGaze() {
  if (gazeAuto && millis() >= nextGlanceAt) {
    if (esp_random() % 3 == 0) {
      gazeTX = gazeTY = 0;
    } else {
      gazeTX = ((int)(esp_random() % 201) - 100) / 100.0f;
      gazeTY = ((int)(esp_random() % 201) - 100) / 100.0f;
    }
    nextGlanceAt = millis() + 1200 + esp_random() % 2800;
  }
  gazeX = lerp(gazeX, gazeTX, 0.2f);
  gazeY = lerp(gazeY, gazeTY, 0.2f);
}

// isLeft: which side is "inner" (towards the nose) for the tilt mask
void drawEye(int cx, int cy, bool isLeft, float open) {
  int w = (int)roundf(EYE_W * cur.scale);
  int h = max(2, (int)roundf(EYE_H * cur.scale * open));
  int x = cx - w / 2;
  int y = cy - h / 2;

  u8g2.setDrawColor(1);
  int r = min(EYE_R, (h - 1) / 2);
  if (r >= 1)
    u8g2.drawRBox(x, y, w, h, r);
  else
    u8g2.drawBox(x, y, w, h);
  if (h <= 3) return;  // closed: just a line

  u8g2.setDrawColor(0);

  // Happy: carve the bottom with an ellipse -> ∩ shaped arc
  if (cur.happy > 0.05f) {
    int ry = max(1, (int)roundf(h * cur.happy));
    int rx = (int)roundf(w * 0.75f);
    u8g2.drawFilledEllipse(cx, y + h + 2, rx, ry, U8G2_DRAW_ALL);
  }

  // Tilt: carve a triangle from the top edge
  if (fabsf(cur.tilt) > 0.05f) {
    int drop = (int)roundf(h * 0.55f * fabsf(cur.tilt));
    bool lowerRightCorner = (cur.tilt > 0) == isLeft;  // angry: inner side; sad: outer side
    int cornerX = lowerRightCorner ? x + w + 1 : x - 1;
    u8g2.drawTriangle(x - 1, y - 1, x + w + 1, y - 1, cornerX, y - 1 + drop);
  }

  u8g2.setDrawColor(1);
}

void drawMouth() {
  // Nose
  u8g2.drawTriangle(MOUTH_X - 3, 44, MOUTH_X + 4, 44, MOUTH_X, 47);
  switch (def().mouth) {
    case Mouth::CAT:  // "ω"
      u8g2.drawCircle(MOUTH_X - 3, 48, 3, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      u8g2.drawCircle(MOUTH_X + 3, 48, 3, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      break;
    case Mouth::O:
      u8g2.drawCircle(MOUTH_X, 52, 3);
      break;
    case Mouth::FLAT:
      u8g2.drawHLine(MOUTH_X - 5, 50, 11);
      break;
  }
  // Whiskers
  u8g2.drawLine(48, 47, 32, 45);
  u8g2.drawLine(48, 50, 32, 52);
  u8g2.drawLine(80, 47, 96, 45);
  u8g2.drawLine(80, 50, 96, 52);
}

void drawZzz() {
  u8g2.setFont(u8g2_font_6x10_tf);
  uint32_t t = millis() / 400;
  for (int i = 0; i < 3; i++) {
    if ((t + i) % 4 == 3) continue;  // twinkle
    u8g2.drawStr(108 + i * 6, 20 - i * 7, i == 2 ? "Z" : "z");
  }
}

void render() {
  float open = cur.open * blinkFactor();
  int dx = (int)roundf(gazeX * GAZE_X_PX);
  int dy = (int)roundf(gazeY * GAZE_Y_PX);

  u8g2.clearBuffer();
  drawEye(LEFT_CX + dx, EYE_CY + dy, true, open);
  drawEye(RIGHT_CX + dx, EYE_CY + dy, false, open);
  drawMouth();
  if (def().zzz) drawZzz();
  u8g2.sendBuffer();
}

}  // namespace

namespace Face {

void begin(uint8_t sda, uint8_t scl) {
  Wire.begin(sda, scl);
  u8g2.begin();
  u8g2.setBusClock(400000);
  nextBlinkAt = millis() + 2000;
  nextGlanceAt = millis() + 2000;
}

void set(Expr e) { expr = e; }

Expr current() { return expr; }

const char *name(Expr e) { return EXPRS[static_cast<int>(e)].name; }

void look(float x, float y) {
  gazeAuto = false;
  gazeTX = constrain(x, -1.0f, 1.0f);
  gazeTY = constrain(y, -1.0f, 1.0f);
}

void lookAuto() {
  if (gazeAuto) return;
  gazeAuto = true;
  gazeTX = gazeTY = 0;
  nextGlanceAt = millis() + 1500;
}

void blink() {
  blinking = true;
  doubleBlink = false;
  blinkStart = millis();
}

void update() {
  if (millis() - lastFrame < FRAME_MS) return;
  lastFrame = millis();
  updateBlink();
  updateGaze();
  easeShape();
  render();
}

}  // namespace Face
