// Obstacle-avoiding differential-drive robot
// ESP32 + L298N + 2x DC motor + 3x HC-SR04
//
// Behaviour: drive forward until an obstacle is seen, then spin in place in a
// randomly chosen direction (left/right) until the path is clear, then repeat.
//
// Bottom layer of the cat robot: reports events to the top layer (cat_head)
// over UART2 ("OBSTACLE", "TURN L", "TURN R", "FORWARD", "PAUSED") and obeys
// "STOP" / "GO" from it. Works standalone too (starts in GO).

#include <Arduino.h>

// ---------------- Pins ----------------
// L298N (remove the ENA/ENB jumpers so the ESP32 can PWM them)
constexpr uint8_t PIN_ENA = 25;  // left motor speed (PWM)
constexpr uint8_t PIN_IN1 = 26;  // left motor direction A
constexpr uint8_t PIN_IN2 = 27;  // left motor direction B
constexpr uint8_t PIN_ENB = 14;  // right motor speed (PWM)
constexpr uint8_t PIN_IN3 = 33;  // right motor direction A
constexpr uint8_t PIN_IN4 = 32;  // right motor direction B

// HC-SR04 (ECHO is 5 V on real hardware -> use a voltage divider)
struct Sonar {
  const char *name;
  uint8_t trig;
  uint8_t echo;
};
Sonar sonarFront = {"F", 5, 18};
Sonar sonarLeft = {"L", 19, 21};
Sonar sonarRight = {"R", 22, 23};

// UART link to the top layer (cross TX/RX, share GND)
constexpr uint8_t PIN_LINK_RX = 16;  // <- top TX (17)
constexpr uint8_t PIN_LINK_TX = 17;  // -> top RX (16)

// ---------------- Tunables ----------------
constexpr int SPEED_FORWARD = 200;      // 0..255
constexpr int SPEED_TURN = 170;         // 0..255
constexpr float OBSTACLE_CM = 25.0;     // front obstacle threshold
constexpr float CLEAR_CM = 35.0;        // front must exceed this to stop turning (hysteresis)
constexpr float SIDE_CM = 15.0;         // side obstacle threshold
constexpr uint32_t BRAKE_MS = 150;      // pause before turning
constexpr uint32_t MIN_TURN_MS = 300;   // minimum spin time
constexpr uint32_t ECHO_TIMEOUT_US = 25000;  // ~4 m
constexpr float NO_ECHO_CM = 999.0;
constexpr uint32_t LOG_INTERVAL_MS = 200;

// ---------------- PWM ----------------
constexpr uint32_t PWM_FREQ = 1000;
constexpr uint8_t PWM_BITS = 8;
constexpr uint8_t PWM_CH_A = 0;
constexpr uint8_t PWM_CH_B = 1;

void pwmInit(uint8_t pin, uint8_t channel) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)channel;
  ledcAttach(pin, PWM_FREQ, PWM_BITS);
#else
  ledcSetup(channel, PWM_FREQ, PWM_BITS);
  ledcAttachPin(pin, channel);
#endif
}

void pwmWrite(uint8_t pin, uint8_t channel, uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)channel;
  ledcWrite(pin, duty);
#else
  (void)pin;
  ledcWrite(channel, duty);
#endif
}

// ---------------- Motors ----------------
// speed: -255 (full reverse) .. 0 (stop) .. 255 (full forward)
void setMotor(uint8_t en, uint8_t ch, uint8_t inA, uint8_t inB, int speed) {
  speed = constrain(speed, -255, 255);
  digitalWrite(inA, speed > 0 ? HIGH : LOW);
  digitalWrite(inB, speed < 0 ? HIGH : LOW);
  pwmWrite(en, ch, abs(speed));
}

void drive(int left, int right) {
  setMotor(PIN_ENA, PWM_CH_A, PIN_IN1, PIN_IN2, left);
  setMotor(PIN_ENB, PWM_CH_B, PIN_IN3, PIN_IN4, right);
}

void forward() { drive(SPEED_FORWARD, SPEED_FORWARD); }
void spinLeft() { drive(-SPEED_TURN, SPEED_TURN); }
void spinRight() { drive(SPEED_TURN, -SPEED_TURN); }
void stopMotors() { drive(0, 0); }

// ---------------- Ultrasonic ----------------
void sonarInit(const Sonar &s) {
  pinMode(s.trig, OUTPUT);
  digitalWrite(s.trig, LOW);
  pinMode(s.echo, INPUT);
}

float readCm(const Sonar &s) {
  digitalWrite(s.trig, LOW);
  delayMicroseconds(2);
  digitalWrite(s.trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(s.trig, LOW);
  uint32_t us = pulseIn(s.echo, HIGH, ECHO_TIMEOUT_US);
  if (us == 0) return NO_ECHO_CM;  // nothing in range
  return us / 58.0f;
}

// ---------------- State machine ----------------
enum class State { FORWARD, BRAKE, TURNING, PAUSED };
enum class TurnDir { LEFT, RIGHT };

State state = State::FORWARD;
TurnDir turnDir = TurnDir::LEFT;
uint32_t stateSince = 0;
float distF = NO_ECHO_CM, distL = NO_ECHO_CM, distR = NO_ECHO_CM;

const char *stateName() {
  switch (state) {
    case State::FORWARD: return "FORWARD";
    case State::BRAKE: return "BRAKE";
    case State::TURNING: return turnDir == TurnDir::LEFT ? "TURN_LEFT" : "TURN_RIGHT";
    case State::PAUSED: return "PAUSED";
  }
  return "?";
}

// Tell the top layer what the body is doing
void sendEvent() {
  switch (state) {
    case State::FORWARD: Serial2.println("FORWARD"); break;
    case State::BRAKE: Serial2.println("OBSTACLE"); break;
    case State::TURNING: Serial2.println(turnDir == TurnDir::LEFT ? "TURN L" : "TURN R"); break;
    case State::PAUSED: Serial2.println("PAUSED"); break;
  }
}

void enter(State s) {
  state = s;
  stateSince = millis();
  switch (s) {
    case State::FORWARD:
      forward();
      break;
    case State::BRAKE:
      stopMotors();
      break;
    case State::TURNING:
      turnDir = (esp_random() & 1) ? TurnDir::LEFT : TurnDir::RIGHT;
      if (turnDir == TurnDir::LEFT) spinLeft(); else spinRight();
      break;
    case State::PAUSED:
      stopMotors();
      break;
  }
  Serial.printf(">> %s\n", stateName());
  sendEvent();
}

void readSensors() {
  // Sequential reads with a short gap so echoes don't cross-talk
  distF = readCm(sonarFront);
  delay(10);
  distL = readCm(sonarLeft);
  delay(10);
  distR = readCm(sonarRight);
}

void onCommand(const String &cmd) {
  if (cmd == "STOP" && state != State::PAUSED) enter(State::PAUSED);
  else if (cmd == "GO" && state == State::PAUSED) enter(State::FORWARD);
}

void pollLink() {
  static String line;
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      line.trim();
      if (line.length()) onCommand(line);
      line = "";
    } else if (c != '\r' && line.length() < 16) {
      line += c;
    }
  }
}

bool obstacleAhead() {
  return distF < OBSTACLE_CM || distL < SIDE_CM || distR < SIDE_CM;
}

bool pathClear() {
  return distF > CLEAR_CM && distL > SIDE_CM && distR > SIDE_CM;
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);

  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  pinMode(PIN_IN3, OUTPUT);
  pinMode(PIN_IN4, OUTPUT);
  pwmInit(PIN_ENA, PWM_CH_A);
  pwmInit(PIN_ENB, PWM_CH_B);
  stopMotors();

  sonarInit(sonarFront);
  sonarInit(sonarLeft);
  sonarInit(sonarRight);

#ifdef WOKWI_SIM
  Serial.println("Robot avoider (Wokwi sim: motors shown as LEDs)");
#else
  Serial.println("Robot avoider");
#endif
  delay(500);
  enter(State::FORWARD);
}

void loop() {
  pollLink();
  readSensors();
  uint32_t elapsed = millis() - stateSince;

  switch (state) {
    case State::FORWARD:
      if (obstacleAhead()) enter(State::BRAKE);
      break;
    case State::BRAKE:
      if (elapsed >= BRAKE_MS) enter(State::TURNING);
      break;
    case State::TURNING:
      // Keep spinning the same way until clear (no left/right jitter)
      if (elapsed >= MIN_TURN_MS && pathClear()) enter(State::FORWARD);
      break;
    case State::PAUSED:
      break;  // waiting for "GO" from the top layer
  }

  static uint32_t lastLog = 0;
  if (millis() - lastLog >= LOG_INTERVAL_MS) {
    lastLog = millis();
    Serial.printf("F=%6.1f L=%6.1f R=%6.1f cm  [%s]\n", distF, distL, distR, stateName());
  }
}
