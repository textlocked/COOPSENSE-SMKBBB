///
/// BOARDS
///
- esp32 by Espressif Systems

Additional boards manager URLs
https://espressif.github.io/arduino-esp32/package_esp32_index.json

///
/// THIRD PARTY LIBRARIES 
///
- HX711 by Rob Tillaart
- LiquidCrystal I2C by Frank de Brabander

///
/// LIST OF ASSIGNED PINS
///

---- HX711 pins ----
- HX711_DT   18
- HX711_SCK  19

---- LCD pins (I2C) ----
LCD_SDA 33
LCD_SCL 32

---- RGB LED pins ----
- LED_G 27
- LED_R 26
- LED_B 25
- LED_SILENCE 13

---- Buzzer + buttons ----
- BUZZER      4
- TARE_BTN    17
- SILENCE_BTN 16
- REWEIGH_BTN 35


// ---- Tunables V2 ONLY ----
- const int   SETTLE_BUFFER_SIZE   = 6;     // ~6 samples @ 200ms = 1.2s window
- const float SETTLE_NOISE_FLOOR_G = 4.0;   // max spread within buffer to call it "settled"
- const float EVENT_MIN_DELTA_G    = 15.0;  // ignore deltas smaller than this (load-cell noise floor)
- const float PROFILE_MATCH_TOL    = 0.12;  // +/-12% unit-weight tolerance to match an existing profile
- const float LEAKY_MAX_ALPHA_UP   = 0.6;   // fast: new peak >= current capacity estimate
- const float LEAKY_MAX_ALPHA_DOWN = 0.05;  // slow: new peak < current capacity estimate

// Percentage thresholds (of fullQtyEstimate), with hysteresis bands
- const float PCT_RESTOCK_ENTER = 15.0;
- const float PCT_RESTOCK_CLEAR = 20.0;
- const float PCT_LOW_ENTER     = 40.0;
- const float PCT_LOW_CLEAR     = 45.0;

enum ThresholdMode {
  MODE_WEIGHT_ONLY = 0,
  MODE_FOLLOW_DISPLAY = 1,
  MODE_QTY_ONLY = 2
};
- const ThresholdMode THRESHOLD_MODE = MODE_WEIGHT_ONLY; // <-- change here

- float qtyThresholdRstk = 15;
- float qtyThresholdLow = 40;
- float gramThresholdRstk = 500;
- float gramThresholdLow = 1500;

