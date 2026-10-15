# KREATIF – ESP32 PlatformIO + Wokwi projects

Interactive cat robot made of two ESP32 boards:

- **Top layer – `cat_head`**: face on a 1.3" OLED, sounds from microSD through a
  MAX98357A speaker, TTP223 touch sensor. Reacts to touch, idle time and to what
  the body is doing.
- **Bottom layer – `robot_avoider`**: L298N + 2 DC motors + 3× HC-SR04 obstacle avoidance.

The two talk over UART (see *Layer link* below). `oled_eyes` is the earlier
eyes-only prototype (Wokwi simulation of the face idea).

Open `KREATIF.code-workspace` in VS Code (PlatformIO IDE + Wokwi extensions).

`robot_avoider` and `oled_eyes` have two build environments (`cat_head` only has `esp32`):

| env     | use                                      |
|---------|------------------------------------------|
| `esp32` | real hardware (upload with PlatformIO)   |
| `wokwi` | simulation (what `wokwi.toml` points to) |

## Running a simulation

1. Build the `wokwi` env:  `pio run -d robot_avoider -e wokwi` (or `oled_eyes`).
   In VS Code: PlatformIO sidebar → project → `wokwi` → Build.
2. `F1` → **Wokwi: Select Config File** → choose `robot_avoider/wokwi.toml` or `oled_eyes/wokwi.toml`.
3. `F1` → **Wokwi: Start Simulator**.

First time only: `F1` → **Wokwi: Request a New License** (free) and follow the browser prompt.

---

## 0. cat_head – top layer (face + voice)

### Behaviour

| Trigger                              | Face                       | Sound              | Body          |
|--------------------------------------|----------------------------|--------------------|---------------|
| short tap                            | HAPPY (^ ^)                | meow               |               |
| hold > 0.7 s (petting)               | PURR (eyes nearly closed)  | purr (looping)     | STOP while held, GO 1 s after release |
| 5 taps within 5 s                    | ANGRY                      | hiss               |               |
| bottom: obstacle                     | SURPRISED                  | surprise (max every 4 s) |         |
| bottom: turning left / right         | looks left / right         |                    |               |
| no touch 45 s                        | SLEEPY                     | yawn               |               |
| no touch 90 s                        | SLEEP (Zzz)                |                    | STOP          |
| touch while asleep                   | SURPRISED → NEUTRAL        | meow               | GO            |
| asleep 2–4 min                       | wakes up by itself         | yawn               | GO            |
| awake, every 1–3 min                 | HAPPY briefly              | meow               |               |

Eyes blink and glance around on their own. All timings are at the top of
`cat_head/src/main.cpp`; expressions are in `cat_head/src/face.cpp` (`EXPRS` table).

### Wiring (top ESP32)

| Module       | Pin        | ESP32 |
|--------------|------------|-------|
| OLED 1.3"    | SDA / SCL  | 21 / 22 |
| TTP223       | I/O        | 4     |
| microSD (SPI)| SCK / MISO / MOSI / CS | 18 / 19 / 23 / 5 |
| MAX98357A    | BCLK / LRC / DIN | 26 / 25 / 27 |
| MAX98357A    | VIN / GND  | 5V / GND (SD pin: leave floating or to VIN = on) |
| Link to bottom | TX / RX  | 17 / 16 |

Sound files: see [`cat_head/sd_card/README.md`](cat_head/sd_card/README.md).
If the SD card is missing the cat still works, just silently.
If your OLED is an SSD1306 (not SH1106), add `-DUSE_SSD1306` to `build_flags`.

### Layer link (UART2, 115200 8N1, one text line per message)

| Top ESP32 | Bottom ESP32 |
|-----------|--------------|
| TX 17     | RX 16        |
| RX 16     | TX 17        |
| GND       | GND          |

- bottom → top: `OBSTACLE`, `TURN L`, `TURN R`, `FORWARD`, `PAUSED`
- top → bottom: `STOP`, `GO` (resent every 2 s)

The bottom layer starts in GO, so it also runs on its own without the top.

---

## 1. robot_avoider – obstacle-avoiding differential-drive robot

ESP32 + L298N + 2× DC motor + 3× HC-SR04.
Drives forward. If something is closer than 25 cm in front (or 15 cm at a side),
it brakes, picks left or right at random, and spins that way until the front is
clear (> 35 cm) and both sides are clear. Then it drives forward again.

### Wiring

| L298N | ESP32 |       | HC-SR04 | TRIG | ECHO |
|-------|-------|-------|---------|------|------|
| ENA   | 25    |       | front   | 5    | 18   |
| IN1   | 26    |       | left    | 19   | 21   |
| IN2   | 27    |       | right   | 22   | 23   |
| ENB   | 14    |       |         |      |      |
| IN3   | 33    |       |         |      |      |
| IN4   | 32    |       |         |      |      |

- Remove the ENA/ENB jumpers on the L298N so the ESP32 controls speed via PWM.
- Connect L298N GND to ESP32 GND (common ground). Power motors from the battery, not the ESP32.
- HC-SR04 runs at 5 V and its ECHO pin outputs 5 V. Put a voltage divider
  (e.g. 1 kΩ / 2 kΩ) on each ECHO line before the ESP32.
- If a motor spins the wrong way, swap its two wires on the L298N output.

### In Wokwi
Wokwi has no L298N or DC motor part, so the six L298N inputs are shown as LEDs:
yellow = ENA/ENB (speed), green = IN1/IN3 (forward), red = IN2/IN4 (reverse).
Click an HC-SR04 and drag its distance slider below 25 cm to trigger avoidance.
The serial monitor prints distances and the current state.

Tunables (`SPEED_*`, `OBSTACLE_CM`, `CLEAR_CM`, `SIDE_CM`, …) are at the top of `src/main.cpp`.

---

## 2. oled_eyes – animated eyes with touch

ESP32 + 1.3" I2C OLED (128×64) + TTP223 capacitive touch module.
Idle: two wide-open eyes that glance around now and then. Touch: a random
pattern plays (blink, double blink, wink left/right, sleepy half-open,
half + close, close + half, alternating, doze off, squint, skeptical,
half-wink, flutter), never the same one twice in a row, then the eyes reopen.

### Wiring

| Module      | Pin  | ESP32 |
|-------------|------|-------|
| OLED        | SDA  | 21    |
| OLED        | SCL  | 22    |
| OLED        | VCC  | 3V3   |
| TTP223      | I/O  | 4     |
| TTP223      | VCC  | 3V3   |
| both        | GND  | GND   |

The 1.3" modules use the **SH1106** controller (env `esp32`). If yours is
actually an SSD1306, build with `-DUSE_SSD1306` (as the `wokwi` env does).

### In Wokwi
Wokwi only has an SSD1306 display, and the TTP223 is replaced by a red
pushbutton to 3V3 (the TTP223 output is also active-HIGH, so the code is the same).
Patterns are defined in the `PATTERN(...)` table in `src/main.cpp`; add your own there.
