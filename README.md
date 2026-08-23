///
/// BOARDS
///
- esp32 by Espressif Systems

Additional boards manager URLs
https://espressif.github.io/arduino-esp32/package_esp32_index.json

///
/// LIBRARIES (asterisk - optional)
///
- HX711 by Rob Tillaart
- LiquidCrystal by Arduino, Adafruit*
- Preferences by Volodymyr Shymanskyy*

///
/// LIST OF ASSIGNED PINS
///

---- HX711 pins ----
HX711_DT   18
HX711_SCK  19

---- LCD pins (parallel, bare 1602) ----
                 (rs, enable, d4, d5, d6, d7)
LiquidCrystal lcd(21, 22, 23, 32, 33, 14)

---- RGB LED pins ----
LED_G 27
LED_R 26
LED_B 25
LED_SILENCE 13

---- Buzzer + buttons ----
BUZZER      4
TARE_BTN    17
SILENCE_BTN 16
REWEIGH_BTN 35


///
/// LIST OF USER VARIABLES (CAN'T CHANGE IN OPERATION, ONLY WHEN UPLOADING :p)
///

const ThresholdMode THRESHOLD_MODE = MODE_WEIGHT_ONLY; // (MODE_WEIGHT_ONLY, MODE_FOLLOW_DISPLAY, MODE_QTY_ONLY)

float qtyThresholdRstk = 15; // Threshold variables like this is the one determining whether something will be RSTK, LOW, or OK.
float qtyThresholdLow = 40;

float gramThresholdRstk = 500;
float gramThresholdLow = 1500;

