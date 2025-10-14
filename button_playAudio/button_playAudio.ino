#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include "AudioFileSourceSD.h"
#include "driver/i2s.h"

#define TRIGGER_PIN 13
#define COOLDOWN_MS 1000  

//Initialize ESP8266/ESP32 Audio Library classes
AudioGeneratorMP3 *mp3;
AudioFileSourceSD *file;
AudioOutputI2S *out;

volatile bool playing = 0;
volatile byte loadTrack = 0;   // 1..trackCount

bool lastTriggerState = LOW;
unsigned long lastTriggerTime = 0;  

// --------- track list from SD ---------
static const uint8_t MAX_TRACKS = 50;
String tracks[MAX_TRACKS];
uint8_t trackCount = 0;
String currentName;            // for logging

void setup() {
  Serial.begin(115200);
  
  pinMode(TRIGGER_PIN, INPUT_PULLDOWN);  
  Serial.println("Trigger pin configured (GPIO13 with internal pulldown)");
  
  out = new AudioOutputI2S();
  out->SetPinout(26, 25, 32); // new pin
  mp3 = new AudioGeneratorMP3();
  
  delay(1000);
  Serial.print("Initializing SD card...");
  
  // Use SPI bus config that worked in your test
  SPI.begin(18, 19, 23, 5);           // SCK=18, MISO=19, MOSI=23, CS=5
  if (!SD.begin(5, SPI, 5000000)) {  // CS=5, pass SPI object, 5 MHz
    Serial.println("initialization failed!");
    return;
  }
  Serial.println("initialization done.");
  delay(100);
  
  // --------- scan SD for .mp3 files at root ---------
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
          tracks[trackCount++] = String("/") + String(entry.name()); // preserve case for open
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
}