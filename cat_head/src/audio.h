// WAV player: microSD (SPI) -> MAX98357A (I2S), running in its own task.
//
// Sounds are grouped by folder on the SD card; play(Sound) picks a random
// .wav from that folder. Files must be PCM 16-bit, mono or stereo, any rate.
#pragma once
#include <Arduino.h>

enum class Sound { MEOW, PURR, HISS, YAWN, SURPRISE, COUNT };

namespace Audio {
struct Pins {
  uint8_t sdSck, sdMiso, sdMosi, sdCs;
  uint8_t i2sBclk, i2sLrc, i2sDout;
};

bool begin(const Pins &pins);
void play(Sound s, bool loop = false);
void playFile(const char *path, bool loop = false);
void stop();
bool isPlaying();
void setVolume(float v);  // 0..1
}  // namespace Audio
