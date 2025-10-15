#include <Wire.h>
#include <Adafruit_LIS3DH.h>
#include <Adafruit_Sensor.h>
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include "AudioFileSourceSD.h"
#include "driver/i2s.h"
// hello JDX
// LIS3DH pins
#define LIS3DH_INT1_PIN 33

// Audio trigger
#define COOLDOWN_MS 1000

// LIS3DH sensor
Adafruit_LIS3DH lis = Adafruit_LIS3DH(&Wire);

// Audio objects
AudioGeneratorMP3 *mp3;
AudioFileSourceSD *file;
AudioOutputI2S *out;

volatile bool playing = 0;
volatile byte loadTrack = 0;
unsigned long lastTriggerTime = 0;

// Double-tap counter
unsigned long doubleTapCount = 0;
unsigned long lastDoubleTapTime = 0;
#define DOUBLETAP_DEBOUNCE_MS 300  // Prevent counting same double-tap multiple times
#define COUNT_RESET_TIMEOUT_MS 60000  // Reset counter after 2 minutes of no double-taps

// Track list from SD
static const uint8_t MAX_TRACKS = 50;
String tracks[MAX_TRACKS];
uint8_t trackCount = 0;
String currentName;

void setup() {
  Serial.begin(115200);
  delay(200);

  // Initialize random seed
  randomSeed(analogRead(0));

  // Start I2C
  Wire.begin();

  // Try common LIS3DH I2C addresses
  if (!lis.begin(0x18)) {
    if (!lis.begin(0x19)) {
      Serial.println("LIS3DH not found on I2C (0x18/0x19). Check wiring.");
      while (1) delay(10);
    }
  }
  Serial.println("LIS3DH found!");

  // Range & data rate
  lis.setRange(LIS3DH_RANGE_2_G);
  lis.setDataRate(LIS3DH_DATARATE_400_HZ);

  // Tap configuration
  uint8_t threshold = 80;
  uint8_t timeLimit = 10;
  uint8_t latency = 50;
  uint8_t window = 200;

  lis.setClick(2, threshold, timeLimit, latency, window);

  Serial.println("Tap detection ready (single & double). Tap the sensor/enclosure.");

  // Audio output
  out = new AudioOutputI2S();
  out->SetPinout(26, 25, 32);
  mp3 = new AudioGeneratorMP3();

  delay(1000);
  Serial.print("Initializing SD card...");

  // SD card initialization
  SPI.begin(18, 19, 23, 5);
  if (!SD.begin(5, SPI, 10000000)) {
    Serial.println("initialization failed!");
    return;
  }
  Serial.println("initialization done.");
  delay(100);

  // Scan SD for .mp3 files at root
  File root = SD.open("/");
  if (!root) {
    Serial.println("Failed to open SD root.");
    return;
  }

  File entry;
  while ((entry = root.openNextFile())) {
    if (!entry.isDirectory()) {
      String fileName = String(entry.name());

      // Skip hidden files (starting with . or ._)
      if (fileName.startsWith(".")) {
        entry.close();
        continue;
      }

      String name = String("/") + fileName;
      name.toLowerCase();
      if (name.endsWith(".mp3")) {
        if (trackCount < MAX_TRACKS) {
          tracks[trackCount++] = String("/") + fileName;
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

  // Sort tracks alphabetically
  for (uint8_t i = 0; i < trackCount - 1; i++) {
    for (uint8_t j = i + 1; j < trackCount; j++) {
      if (tracks[i] > tracks[j]) {
        String temp = tracks[i];
        tracks[i] = tracks[j];
        tracks[j] = temp;
      }
    }
  }

  Serial.print("Found ");
  Serial.print(trackCount);
  Serial.println(" MP3 files (sorted):");
  Serial.println("====================");

  // Print numbered list of all tracks
  for (uint8_t i = 0; i < trackCount; i++) {
    Serial.print(i + 1);
    Serial.print(". ");
    Serial.println(tracks[i]);
  }

  Serial.println("====================");
  Serial.println("Waiting for double-tap to play audio...");
  Serial.println("Playback pattern:");
  Serial.println("  Taps 1-3: Random from tracks 1-5");
  Serial.println("  Taps 4+:  Random from tracks 6-8");
  Serial.println("  (Resets after 1 min of inactivity)");
}

void loop() {
  unsigned long currentTime = millis();

  // Check if counter should be reset (2 minutes of inactivity)
  if (doubleTapCount > 0 && lastDoubleTapTime > 0 && (currentTime - lastDoubleTapTime >= COUNT_RESET_TIMEOUT_MS)) {
    Serial.println("*** Counter reset after 2 minutes of inactivity ***");
    doubleTapCount = 0;
  }

  // Poll tap detection
  uint8_t c = lis.getClick();
  
  if (c & 0x10) {
   // Serial.println("Polled: single tap");
  }
  
  if (c & 0x20) {
    // Check if this is a new double-tap (debounce)
    if (currentTime - lastDoubleTapTime >= DOUBLETAP_DEBOUNCE_MS) {
      // Increment double-tap counter
      doubleTapCount++;
      lastDoubleTapTime = currentTime;

      Serial.print("Polled: double tap #");
      Serial.println(doubleTapCount);

      // Check cooldown for audio playback
      if (currentTime - lastTriggerTime >= COOLDOWN_MS) {
        Serial.print("=== Double-tap audio trigger! === (Total count: ");
        Serial.print(doubleTapCount);
        Serial.println(")");
        lastTriggerTime = currentTime;

        if (playing && mp3->isRunning()) {
          mp3->stop();
          if (file) {
            delete file;
            file = nullptr;
          }
          Serial.println("Stopped current playback");
        }

        // Play track based on double-tap count
        // First 3 taps: randomly play tracks 1-5
        // After 3 taps: randomly play tracks 6-8

        if (doubleTapCount <= 3) {
          // First 3 taps: play tracks 1-5 (index 0-4)
          if (trackCount >= 5) {
            loadTrack = random(1, 6);  // Random between 1-5
            Serial.print("First 3 taps (#");
            Serial.print(doubleTapCount);
            Serial.print("): Will play random track #");
            Serial.println(loadTrack);
          } else {
            Serial.println("Error: Not enough tracks for first group (need at least 5)");
            loadTrack = 0;
          }
        } else {
          // After 3 taps: play tracks 6-8 (index 5-7)
          if (trackCount >= 8) {
            loadTrack = random(6, 9);  // Random between 6-8
            Serial.print("After 3 taps (#");
            Serial.print(doubleTapCount);
            Serial.print("): Will play random track #");
            Serial.println(loadTrack);
          } else {
            Serial.println("Error: Not enough tracks for second group (need at least 8)");
            loadTrack = 0;
          }
        }
      } else {
        Serial.println("Double-tap trigger ignored (cooldown period)");
      }
    }
  }

  // Load track
  if (loadTrack) {
    if (loadTrack < 1 || loadTrack > trackCount) {
      Serial.println("Invalid track index");
      loadTrack = 0;
      return;
    }

    currentName = tracks[loadTrack - 1];

    Serial.print("Loading: ");
    Serial.println(currentName);

    file = new AudioFileSourceSD(currentName.c_str());
    out->SetGain(0.90);
    mp3->begin(file, out);

    Serial.print("Started: ");
    Serial.println(currentName);

    playing = 1;
    loadTrack = 0;
  }

  // Audio playback loop
  static unsigned long lastPlayLog = 0;
  if (playing && mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      if (file) {
        delete file;
        file = nullptr;
      }
      playing = 0;
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

  delay(1);
}