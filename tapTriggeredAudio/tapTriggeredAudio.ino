#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_LIS3DH.h>
#include <Adafruit_Sensor.h>
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include "AudioFileSourceSD.h"
#include "driver/i2s.h"

// ---------- User pins ----------
#define LIS3DH_INT1_PIN 33  // ESP32 GPIO connected to LIS3DH INT1
#define TRIGGER_COOLDOWN_MS 1000

// Audio pin mapping
#define I2S_BCLK_PIN 26
#define I2S_LRCLK_PIN 25
#define I2S_DATA_PIN 32

// SD card SPI configuration
#define SD_SCK_PIN 18
#define SD_MISO_PIN 19
#define SD_MOSI_PIN 23
#define SD_CS_PIN 5

// --------- Tap sensor globals ---------
Adafruit_LIS3DH lis = Adafruit_LIS3DH(&Wire);
volatile bool tapIRQ = false;

// --------- Audio playback globals ---------
AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceSD *file = nullptr;
AudioOutputI2S *out = nullptr;

static const uint8_t MAX_TRACKS = 50;
String tracks[MAX_TRACKS];
uint8_t trackCount = 0;
uint8_t currentTrackIndex = 0;  // 0-based index into tracks array

bool playing = false;
uint8_t loadTrack = 0;  // 1..trackCount
String currentName;
unsigned long lastTriggerTime = 0;

void IRAM_ATTR lisIntHandler() {
  tapIRQ = true;  // keep ISR tiny; do work in loop()
}

void scanTracksFromSD() {
  File root = SD.open("/");
  if (!root) {
    Serial.println("Failed to open SD root.");
    return;
  }

  File entry;
  while ((entry = root.openNextFile())) {
    if (!entry.isDirectory()) {
      String name = String("/") + String(entry.name());
      String lower = name;
      lower.toLowerCase();
      if (lower.endsWith(".mp3")) {
        if (trackCount < MAX_TRACKS) {
          tracks[trackCount++] = name;  // preserve case for opening later
        }
      }
    }
    entry.close();
  }
  root.close();

  if (trackCount == 0) {
    Serial.println("No MP3 files found on SD.");
    return;
  }

  Serial.print("Found ");
  Serial.print(trackCount);
  Serial.println(" MP3 files.");
}

bool beginSDCard() {
  Serial.print("Initializing SD card...");
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN, SPI, 5000000)) {
    Serial.println("initialization failed!");
    return false;
  }
  Serial.println("initialization done.");
  delay(100);
  return true;
}

bool requestTrackPlayback(uint8_t trackIndex, unsigned long now) {
  if (trackCount == 0) {
    Serial.println("No tracks available to play.");
    return false;
  }

  if (now - lastTriggerTime < TRIGGER_COOLDOWN_MS) {
    Serial.println("Trigger ignored (cooldown period)");
    return false;
  }

  if (trackIndex >= trackCount) {
    Serial.println("Invalid track index");
    return false;
  }

  loadTrack = trackIndex + 1;  // convert to 1-based for compatibility with legacy logic
  lastTriggerTime = now;
  return true;
}

void stopPlayback() {
  if (mp3 && mp3->isRunning()) {
    mp3->stop();
  }
  if (file) {
    delete file;
    file = nullptr;
  }
  playing = false;
}

void startPlaybackIfRequested() {
  if (!loadTrack) return;

  if (loadTrack < 1 || loadTrack > trackCount) {
    Serial.println("Invalid track index");
    loadTrack = 0;
    return;
  }

  currentTrackIndex = loadTrack - 1;
  currentName = tracks[currentTrackIndex];

  Serial.print("Loading: ");
  Serial.println(currentName);

  stopPlayback();
  if (!out || !mp3) {
    Serial.println("Audio output not initialized.");
    loadTrack = 0;
    return;
  }
  file = new AudioFileSourceSD(currentName.c_str());
  out->SetGain(0.90);
  if (!mp3->begin(file, out)) {
    Serial.println("Failed to start MP3 playback.");
    delete file;
    file = nullptr;
    loadTrack = 0;
    return;
  }

  Serial.print("Started: ");
  Serial.println(currentName);

  playing = true;
  loadTrack = 0;
}

void handlePlaybackLoop() {
  static unsigned long lastPlayLog = 0;
  if (!mp3) {
    return;
  }

  if (!playing || !mp3->isRunning()) {
    if (playing) {
      stopPlayback();
      Serial.println("Track finished -> waiting for next trigger");
    }
    return;
  }

  if (!mp3->loop()) {
    stopPlayback();
    Serial.println("Track finished -> waiting for next trigger");
  } else {
    unsigned long now = millis();
    if (now - lastPlayLog >= 1000) {
      Serial.print("Playing: ");
      Serial.println(currentName);
      lastPlayLog = now;
    }
  }
}

void setupTapSensor() {
  // Start I2C
  Wire.begin();  // ESP32 default SDA=21, SCL=22

  // Try common LIS3DH I2C addresses
  if (!lis.begin(0x18)) {
    if (!lis.begin(0x19)) {
      Serial.println("LIS3DH not found on I2C (0x18/0x19). Check wiring.");
      while (1) delay(10);
    }
  }
  Serial.println("LIS3DH found!");

  // Range & data rate
  lis.setRange(LIS3DH_RANGE_2_G);              // 2/4/8/16G
  lis.setDataRate(LIS3DH_DATARATE_400_HZ);     // higher rate helps tap detection

  // Tap (Click) configuration
  uint8_t threshold = 80;   // try 60..120 depending on stiffness
  uint8_t timeLimit = 10;   // ~25 ms
  uint8_t latency   = 50;   // ~125 ms (min gap between taps)
  uint8_t window    = 200;  // ~500 ms (max interval for double tap)
  lis.setClick(2, threshold, timeLimit, latency, window);

  // Route click to INT1, active HIGH, latched until source is read
  pinMode(LIS3DH_INT1_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(LIS3DH_INT1_PIN), lisIntHandler, RISING);

  Serial.println("Tap detection ready (single & double). Tap the sensor/enclosure.");
}

void setup() {
  Serial.begin(115200);
  delay(200);

  setupTapSensor();

  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK_PIN, I2S_LRCLK_PIN, I2S_DATA_PIN);
  mp3 = new AudioGeneratorMP3();

  if (!beginSDCard()) {
    Serial.println("SD card initialization failed. Audio playback disabled.");
    return;
  }

  scanTracksFromSD();
  if (trackCount == 0) {
    Serial.println("Waiting for tracks to be added to SD card...");
  } else {
    Serial.println("Single tap: play current track. Double tap: advance & play next track.");
    Serial.println("Tracks will loop when reaching the end of the list.");
  }
}

void handleTapEvents() {
  if (!tapIRQ) {
    return;
  }
  tapIRQ = false;

  uint8_t click = lis.getClick();
  if (click == 0) {
    return;  // spurious
  }

  bool isDouble = click & 0x20;
  bool isSingle = click & 0x10;

  String axes = "";
  if (click & 0x01) axes += "X";
  if (click & 0x02) axes += (axes.length() ? "+Y" : "Y");
  if (click & 0x04) axes += (axes.length() ? "+Z" : "Z");
  if (axes.length() == 0) axes = "unknown axis";

  unsigned long now = millis();

  if (isDouble) {
    Serial.print("[INT] Double-tap on ");
    Serial.println(axes);

    if (trackCount > 0) {
      currentTrackIndex = (currentTrackIndex + 1) % trackCount;
      Serial.print("Next track index: ");
      Serial.println(currentTrackIndex + 1);
      requestTrackPlayback(currentTrackIndex, now);
    }
  } else if (isSingle) {
    Serial.print("[INT] Single-tap on ");
    Serial.println(axes);
    requestTrackPlayback(currentTrackIndex, now);
  } else {
    Serial.print("[INT] Other click src: 0x");
    Serial.println(click, HEX);
  }
}

void loop() {
  handleTapEvents();
  startPlaybackIfRequested();
  handlePlaybackLoop();
}

