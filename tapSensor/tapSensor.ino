#include <Wire.h>
#include <Adafruit_LIS3DH.h>
#include <Adafruit_Sensor.h>

// ---------- User pins ----------
#define LIS3DH_INT1_PIN 33  // ESP32 GPIO connected to LIS3DH INT1

// I2C address: 0x18 (SA0 -> GND) or 0x19 (SA0 -> VCC). Most breakouts use 0x18.
Adafruit_LIS3DH lis = Adafruit_LIS3DH(&Wire);

volatile bool tapIRQ = false;

void IRAM_ATTR lisIntHandler() {
  tapIRQ = true; // keep ISR tiny; do work in loop()
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // Start I2C
  Wire.begin(); // ESP32 Thing Plus C uses SDA=21, SCL=22 by default

  // Try common LIS3DH I2C addresses
  if (!lis.begin(0x18)) {
    if (!lis.begin(0x19)) {
      Serial.println("LIS3DH not found on I2C (0x18/0x19). Check wiring.");
      while (1) delay(10);
    }
  }
  Serial.println("LIS3DH found!");

  // Range & data rate
  lis.setRange(LIS3DH_RANGE_2_G);               // 2/4/8/16G
  lis.setDataRate(LIS3DH_DATARATE_400_HZ);      // higher rate helps tap detection

  // --- Tap (Click) configuration ---
  // setClick(click_mode, threshold, timeLimit, latency, window)
  //
  // click_mode: 0=off, 1=single, 2=double, 3=both (library treats 2 as “enable double” but single is reported as well)
  // threshold: 1..127 -> ~16 mg/LSB at ±2G => 80 ≈ 1.28G (tune!)
  // timeLimit: 1..127 -> ~1/ODR units (at 400 Hz, 1 ≈ 2.5 ms)
  // latency:   quiet time after click (~ms at ODR scale)
  // window:    double-tap max window (~ms at ODR scale)
  //
  // Good starting point for finger taps on small enclosure:
  uint8_t threshold = 80;   // try 60..120 depending on stiffness
  uint8_t timeLimit = 10;   // ~25 ms
  uint8_t latency   = 50;   // ~125 ms (min gap between taps)
  uint8_t window    = 200;  // ~500 ms (max interval for double tap)
  lis.setClick(2, threshold, timeLimit, latency, window);

  // Route click to INT1, active HIGH, latched by the chip until source is read
  // Adafruit library does this as part of setClick(); we just wire the pin.
  pinMode(LIS3DH_INT1_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(LIS3DH_INT1_PIN), lisIntHandler, RISING);

  Serial.println("Tap detection ready (single & double). Tap the sensor/enclosure.");
}

void loop() {
  if (tapIRQ) {
    tapIRQ = false;

    // Reading the click source register via getClick() both decodes and clears the INT latch.
    uint8_t click = lis.getClick();

    if (click == 0) return; // spurious

    // Bits (from LIS3DH CLICK_SRC):
    // bit5: Double Click (0x20)
    // bit4: Single Click (0x10)
    // bit2: Z (0x04), bit1: Y (0x02), bit0: X (0x01)
    bool isDouble = click & 0x20;
    bool isSingle = click & 0x10;

    String axes = "";
    if (click & 0x01) axes += "X";
    if (click & 0x02) axes += (axes.length() ? "+Y" : "Y");
    if (click & 0x04) axes += (axes.length() ? "+Z" : "Z");
    if (axes.length() == 0) axes = "unknown axis";

    if (isDouble) {
      Serial.print("[INT] Double-tap on ");
      Serial.println(axes);
    } else if (isSingle) {
      Serial.print("[INT] Single-tap on ");
      Serial.println(axes);
    } else {
      // Other INT sources (rare here), but reading cleared the latch.
      Serial.print("[INT] Other click src: 0x");
      Serial.println(click, HEX);
    }
  }

// (Optional) also poll without interrupts:
 uint8_t c = lis.getClick();
 if (c & 0x10) Serial.println("Polled: single tap");
 if (c & 0x20) Serial.println("Polled: double tap");

  delay(1);
}