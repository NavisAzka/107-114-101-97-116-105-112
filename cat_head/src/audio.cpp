#include "audio.h"

#include <SD.h>
#include <SPI.h>
#include <driver/i2s.h>

#include <vector>

namespace {

constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
constexpr size_t CHUNK_BYTES = 1024;
constexpr size_t MAX_PATH = 96;

const char *const FOLDERS[] = {"/meow", "/purr", "/hiss", "/yawn", "/surprise"};
static_assert(sizeof(FOLDERS) / sizeof(FOLDERS[0]) == static_cast<size_t>(Sound::COUNT),
              "one folder per Sound");

std::vector<String> library[static_cast<int>(Sound::COUNT)];
int lastPick[static_cast<int>(Sound::COUNT)];

// Request handed from loop() to the audio task
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
char reqPath[MAX_PATH] = "";
bool reqLoop = false;
volatile uint32_t reqGen = 0;
volatile bool playing = false;
volatile float gain = 0.7f;
bool ready = false;

struct WavInfo {
  uint32_t rate;
  uint16_t channels;
  uint16_t bits;
  uint32_t dataStart;
  uint32_t dataLen;
};

uint32_t le32(const uint8_t *b) {
  return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}
uint16_t le16(const uint8_t *b) { return b[0] | b[1] << 8; }

bool parseWav(File &f, WavInfo &w) {
  uint8_t hdr[12];
  if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
    return false;
  bool haveFmt = false;
  uint8_t ch[8];
  while (f.read(ch, 8) == 8) {
    uint32_t len = le32(ch + 4);
    if (!memcmp(ch, "fmt ", 4)) {
      uint8_t b[16];
      if (len < 16 || f.read(b, 16) != 16) return false;
      if (le16(b) != 1) return false;  // PCM only
      w.channels = le16(b + 2);
      w.rate = le32(b + 4);
      w.bits = le16(b + 14);
      f.seek(f.position() + (len - 16) + (len & 1));
      haveFmt = true;
    } else if (!memcmp(ch, "data", 4)) {
      w.dataStart = f.position();
      w.dataLen = min<uint32_t>(len, f.size() - w.dataStart);
      return haveFmt;
    } else {
      f.seek(f.position() + len + (len & 1));
    }
  }
  return false;
}

bool openWav(const char *path, File &file, WavInfo &wav) {
  file = SD.open(path);
  if (!file) {
    Serial.printf("[audio] cannot open %s\n", path);
    return false;
  }
  if (!parseWav(file, wav) || wav.bits != 16 || wav.channels < 1 || wav.channels > 2) {
    Serial.printf("[audio] %s: need PCM 16-bit mono/stereo WAV\n", path);
    file.close();
    return false;
  }
  i2s_set_sample_rates(I2S_PORT, wav.rate);
  return true;
}

void audioTask(void *) {
  static int16_t in[CHUNK_BYTES / 2];
  static int16_t out[CHUNK_BYTES];  // always stereo
  uint32_t gen = 0;
  File file;
  WavInfo wav{};
  bool loop = false;
  uint32_t remaining = 0;

  for (;;) {
    if (gen != reqGen) {
      char path[MAX_PATH];
      portENTER_CRITICAL(&mux);
      gen = reqGen;
      strlcpy(path, reqPath, sizeof(path));
      loop = reqLoop;
      portEXIT_CRITICAL(&mux);

      if (file) file.close();
      remaining = 0;
      i2s_zero_dma_buffer(I2S_PORT);
      if (path[0] && openWav(path, file, wav)) remaining = wav.dataLen;
      playing = remaining > 0;
    }

    if (remaining == 0) {
      playing = false;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    size_t want = min<uint32_t>(remaining, sizeof(in)) & ~1u;
    int n = file.read(reinterpret_cast<uint8_t *>(in), want);
    if (n <= 0) {
      remaining = 0;
      file.close();
      continue;
    }
    remaining -= n;

    int samples = n / 2;
    float g = gain;
    size_t outSamples;
    if (wav.channels == 1) {
      for (int i = 0; i < samples; i++) out[2 * i] = out[2 * i + 1] = in[i] * g;
      outSamples = samples * 2;
    } else {
      for (int i = 0; i < samples; i++) out[i] = in[i] * g;
      outSamples = samples;
    }
    size_t written;
    i2s_write(I2S_PORT, out, outSamples * sizeof(int16_t), &written, portMAX_DELAY);

    if (remaining == 0) {
      if (loop) {
        file.seek(wav.dataStart);
        remaining = wav.dataLen;
      } else {
        file.close();
      }
    }
  }
}

void scanLibrary() {
  for (int s = 0; s < static_cast<int>(Sound::COUNT); s++) {
    lastPick[s] = -1;
    File dir = SD.open(FOLDERS[s]);
    if (!dir || !dir.isDirectory()) {
      Serial.printf("[audio] missing folder %s\n", FOLDERS[s]);
      continue;
    }
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      String name = f.name();
      String lower = name;
      lower.toLowerCase();
      if (!f.isDirectory() && !name.startsWith(".") && lower.endsWith(".wav"))
        library[s].push_back(String(FOLDERS[s]) + "/" + name.substring(name.lastIndexOf('/') + 1));
      f.close();
    }
    dir.close();
    Serial.printf("[audio] %-10s %u file(s)\n", FOLDERS[s], (unsigned)library[s].size());
  }
}

bool initI2S(const Audio::Pins &p) {
  i2s_config_t cfg = {};
  cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = 22050;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.tx_desc_auto_clear = true;  // output silence when starved
  if (i2s_driver_install(I2S_PORT, &cfg, 0, nullptr) != ESP_OK) return false;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = p.i2sBclk;
  pins.ws_io_num = p.i2sLrc;
  pins.data_out_num = p.i2sDout;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  return i2s_set_pin(I2S_PORT, &pins) == ESP_OK;
}

}  // namespace

namespace Audio {

bool begin(const Pins &p) {
  if (!initI2S(p)) {
    Serial.println("[audio] I2S init failed");
    return false;
  }
  SPI.begin(p.sdSck, p.sdMiso, p.sdMosi, p.sdCs);
  if (!SD.begin(p.sdCs, SPI, 20000000)) {
    Serial.println("[audio] SD card not found - running without sound");
    return false;
  }
  scanLibrary();
  // Core 0, so display/I2C work on core 1 can't starve the speaker
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 3, nullptr, 0);
  ready = true;
  return true;
}

void playFile(const char *path, bool loop) {
  if (!ready) return;
  portENTER_CRITICAL(&mux);
  strlcpy(reqPath, path ? path : "", sizeof(reqPath));
  reqLoop = loop;
  reqGen = reqGen + 1;
  portEXIT_CRITICAL(&mux);
  playing = path && path[0];
}

void play(Sound s, bool loop) {
  int i = static_cast<int>(s);
  auto &files = library[i];
  if (files.empty()) return;
  int pick = esp_random() % files.size();
  if (files.size() > 1 && pick == lastPick[i]) pick = (pick + 1) % files.size();
  lastPick[i] = pick;
  Serial.printf("[audio] play %s%s\n", files[pick].c_str(), loop ? " (loop)" : "");
  playFile(files[pick].c_str(), loop);
}

void stop() { playFile("", false); }

bool isPlaying() { return playing; }

void setVolume(float v) { gain = constrain(v, 0.0f, 1.0f); }

}  // namespace Audio
