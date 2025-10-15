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

// LIS3DH pins
#define LIS3DH_INT1_PIN 33

// Audio trigger pin
#define TRIGGER_PIN 13
#define COOLDOWN_MS 1000

// LIS3DH sensor
Adafruit_LIS3DH lis = Adafruit_LIS3DH(&Wire);
volatile bool tapIRQ = false;

// Audio objects
AudioGeneratorMP3 *mp3;
AudioFileSourceSD *file;
AudioOutputI2S *out;

volatile bool playing = 0;
volatile byte loadTrack = 0;
bool lastTriggerState = LOW;
unsigned long lastTriggerTime = 0;

bool canPlayAudio = false;

// Track list from SD
static const uint8_t MAX_TRACKS = 50;
String tracks[MAX_TRACKS];
uint8_t trackCount = 0;
String currentName;

void IRAM_ATTR lisIntHandler() {
  tapIRQ = true;
}

void setup() {
  Serial.begin(115200);
  delay(200);

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

  pinMode(LIS3DH_INT1_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(LIS3DH_INT1_PIN), lisIntHandler, RISING);

  Serial.println("Tap detection ready (single & double). Tap the sensor/enclosure.");

  // Audio trigger pin
  pinMode(TRIGGER_PIN, INPUT_PULLDOWN);
  Serial.println("Trigger pin configured (GPIO13 with internal pulldown)");

  // Audio output
  out = new AudioOutputI2S();
  out->SetPinout(26, 25, 32);
  mp3 = new AudioGeneratorMP3();

  delay(1000);
  Serial.print("Initializing SD card...");

  // SD card initialization
  SPI.begin(18, 19, 23, 5);
  if (!SD.begin(5, SPI, 5000000)) {
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
      String name = String("/") + String(entry.name());
      name.toLowerCase();
      if (name.endsWith(".mp3")) {
        if (trackCount < MAX_TRACKS) {
          tracks[trackCount++] = String("/") + String(entry.name());
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
  Serial.println("Waiting for trigger signal on GPIO13...");
  Serial.println("Each trigger will play the first track (1s cooldown).");
}

void loop() {
  unsigned long currentTime = millis();

  // Handle LIS3DH tap interrupt
  if (tapIRQ) {
    tapIRQ = false;

    uint8_t click = lis.getClick();
    if (click == 0) return;

    bool isDouble = click & 0x20;
    bool isSingle = click & 0x10;

    String axes = "";
    if (click & 0x01) axes += "X";
    if (click & 0x02) axes += (axes.length() ? "+Y" : "Y");
    if (click & 0x04) axes += (axes.length() ? "+Z" : "Z");
    if (axes.length() == 0) axes = "unknown axis";

    if (isDouble) {
      canPlayAudio = true;
      Serial.print("[INT] Double-tap on ");
      Serial.println(axes);
    } else if (isSingle) {
      Serial.print("[INT] Single-tap on ");
      Serial.println(axes);
    } else {
      Serial.print("[INT] Other click src: 0x");
      Serial.println(click, HEX);
    }
  }

  // Poll tap detection
  uint8_t c = lis.getClick();
  if (c & 0x10) Serial.println("Polled: single tap");
  if (c & 0x20) Serial.println("Polled: double tap");

  // Handle double-tap audio trigger
  if (canPlayAudio) {
    if (currentTime - lastTriggerTime >= COOLDOWN_MS) {
      Serial.println("=== Double-tap audio trigger! ===");
      lastTriggerTime = currentTime;

      if (playing && mp3->isRunning()) {
        mp3->stop();
        if (file) {
          delete file;
          file = nullptr;
        }
        Serial.println("Stopped current playback");
      }

      loadTrack = 1;
      canPlayAudio = false;
    } else {
      Serial.println("Double-tap trigger ignored (cooldown period)");
      canPlayAudio = false;
    }
  }

  // Handle GPIO13 audio trigger
  bool currentTriggerState = digitalRead(TRIGGER_PIN);

  if (currentTriggerState == HIGH && lastTriggerState == LOW) {
    if (currentTime - lastTriggerTime >= COOLDOWN_MS) {
      Serial.println("=== Trigger detected! ===");
      lastTriggerTime = currentTime;

      if (playing && mp3->isRunning()) {
        mp3->stop();
        if (file) {
          delete file;
          file = nullptr;
        }
        Serial.println("Stopped current playback");
      }

      loadTrack = 1;

    } else {
      Serial.println("Trigger ignored (cooldown period)");
    }
  }

  lastTriggerState = currentTriggerState;

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