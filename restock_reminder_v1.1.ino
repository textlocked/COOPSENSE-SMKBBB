#include <HX711.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>

// ---- HX711 pins ----
#define HX711_DT   18
#define HX711_SCK  19

// ---- LCD pins (parallel, bare 1602) ----
// ---- I2C LCD ----
#define LCD_SDA 33
#define LCD_SCL 32

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---- RGB LED pins ----
#define LED_G 27
#define LED_R 26
#define LED_B 25
#define LED_SILENCE 13

// ---- Buzzer + buttons ----
#define BUZZER      4
#define TARE_BTN    17
#define SILENCE_BTN 16
#define REWEIGH_BTN 35

// Set true if using a PASSIVE buzzer.
// Set false if using an ACTIVE buzzer.
#define PASSIVE_BUZZER false



//
// ---- Tunables ----
//

// ---- Threshold mode (compile-time only, not runtime-switchable) ----
// 0 = always use TOTAL WEIGHT thresholds
// 1 = follow whatever displayState currently shows
// 2 = always use QUANTITY thresholds
enum ThresholdMode {
  MODE_WEIGHT_ONLY = 0,
  MODE_FOLLOW_DISPLAY = 1,
  MODE_QTY_ONLY = 2
};
const ThresholdMode THRESHOLD_MODE = MODE_WEIGHT_ONLY; // <-- change here

float qtyThresholdRstk = 15;
float qtyThresholdLow = 40;
float gramThresholdRstk = 500;
float gramThresholdLow = 1500;


const int   SETTLE_BUFFER_SIZE   = 6;     // ~6 samples @ 200ms = 1.2s window
const float SETTLE_NOISE_FLOOR_G = 4.0;   // max spread within buffer to call it "settled"
const float EVENT_MIN_DELTA_G    = 15.0;  // ignore deltas smaller than this (load-cell noise floor)
const float PROFILE_MATCH_TOL    = 0.12;  // +/-12% unit-weight tolerance to match an existing profile

const float LEAKY_MAX_ALPHA_UP   = 0.6;   // fast: new peak >= current capacity estimate
const float LEAKY_MAX_ALPHA_DOWN = 0.05;  // slow: new peak < current capacity estimate

// Percentage thresholds (of fullQtyEstimate), with hysteresis bands
const float PCT_RESTOCK_ENTER = 15.0;
const float PCT_RESTOCK_CLEAR = 20.0;
const float PCT_LOW_ENTER     = 40.0;
const float PCT_LOW_CLEAR     = 45.0;

const int MAX_PROFILES = 6; // adjust to taste; stored in Preferences

// ---- Calibration ----
float calibration_factor = 367.20;

HX711 scale;
Preferences prefs;

// ---- Stock states ----
enum StockState {
  RESTOCK,
  LOW_STOCK,
  GOOD_STOCK,
  UNKNOWN
};

// ---- Silence states ----
enum SilenceState {
  UNMUTED,
  MUTED
};

StockState currentState = UNKNOWN;
SilenceState silenceState = UNMUTED;



// ============================================================
// ITEM PROFILE
// ============================================================
struct ItemProfile {
  bool  valid           = false;
  float unitWeight       = 0.0;   // grams, from REWEIGH_BTN flow
  float fullQtyEstimate  = 0.0;   // learned capacity, in item count (can be fractional internally)
  unsigned long lastSeen = 0;     // millis(), for LRU eviction if you exceed MAX_PROFILES
};

ItemProfile profiles[MAX_PROFILES];
int activeProfileIndex = -1;

// ============================================================
// PASSIVE SETTLE / EVENT DETECTION
// Call this once per weight-read tick (same cadence as your
// existing WEIGHT_READ_INTERVAL block in loop()).
// ============================================================
float settleBuffer[SETTLE_BUFFER_SIZE];
int   settleBufIndex = 0;
bool  settleBufFull   = false;

float lastSettledWeight = 0.0;
bool  haveSettledBaseline = false;


// ---- Silence btn ----
unsigned long lastSilenceDebounce = 0;
const unsigned long debounceSilenceDelay = 200;

// ---- Tare button ----
bool tareButtonState = HIGH;
bool lastTareButtonState = HIGH;

unsigned long tarePressStart = 0;
const unsigned long tareHoldTime = 3000; // 3 seconds

unsigned long lastTareDebounce = 0;
const unsigned long debounceDelay = 20;

bool tareResetTriggered = false;

// ---- Re-weigh button / item calibration ----
enum ReweighState {
  REWEIGH_IDLE,
  REWEIGH_WAITING,
  REWEIGH_STABLE_CHECK,
  REWEIGH_MEASURING,
  REWEIGH_SUCCESS
};

ReweighState reweighState = REWEIGH_IDLE;

bool reweighButtonState = LOW;
bool lastReweighButtonState = LOW;

unsigned long reweighPressStart = 0;
unsigned long lastReweighDebounce = 0;

const unsigned long REWEIGH_HOLD_TIME = 5000;
const unsigned long BUTTON_DEBOUNCE = 20;

unsigned long reweighWaitStart = 0;
const unsigned long REWEIGH_WAIT_TIME = 30000;

unsigned long itemDetectionStart = 0;
const unsigned long ITEM_DETECTION_TIME = 1000;

unsigned long measurementStart = 0;
unsigned long lastSnapshotTime = 0;

const unsigned long MEASUREMENT_TIME = 5000;
const unsigned long SNAPSHOT_INTERVAL = 500;

// 10 snapshots total
float weightSnapshots[10];
int snapshotCount = 0;

float currentItemWeight = 0.0;
bool hasItemWeight = false;

// Original offset is kept so a timed-out re-weigh can be undone
long previousTareOffset = 0;

// Weight captured just before the temporary re-weigh tare, for display
float reweighBeforeWeight = 0.0;

// ---- Display state ----
enum DisplayState {
  DISPLAY_QUANTITY = 1,
  DISPLAY_TOTAL_WEIGHT = 2
};

DisplayState displayState = DISPLAY_QUANTITY;

// Temporary message screen
unsigned long messageScreenUntil = 0;
char messageLine1[17] = "";
char messageLine2[17] = "";

// ---- Non-blocking buzzer state machine ----
enum BuzzerPhase { BUZZ_IDLE, BUZZ_ON, BUZZ_OFF };
BuzzerPhase buzzerPhase = BUZZ_IDLE;

int beepsRemaining = 0;
unsigned long lastBuzzerActionTime = 0;
const unsigned long BEEP_ON_TIME = 500;
const unsigned long BEEP_OFF_TIME = 500;

// ---- Weight read timing (decoupled from button polling) ----
unsigned long lastWeightReadTime = 0;
const unsigned long WEIGHT_READ_INTERVAL = 200; // ms between weight reads
float lastGrams = 0.0;

// ---- LCD refresh timing (independent of weight read timing) ----
unsigned long lastLcdUpdateTime = 0;
const unsigned long LCD_REFRESH_INTERVAL = 500; // ms — adjust freely

// ------ WEBSERVER ---------

WebServer server(80);

const char* WIFI_SSID = "WIFI_SSID_HERE";
const char* WIFI_PASSWORD = "WIFI_PASSWORD_HERE";

unsigned long deviceBootTime = 0; // for a simple uptime/last-seen reference


// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);

  // ---- Outputs ----
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  pinMode(LED_SILENCE, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  // Start with everything off
  setColor(false, false, false);
  digitalWrite(LED_SILENCE, LOW);
  stopBuzzer();

  // ---- Buttons ----
  pinMode(TARE_BTN, INPUT_PULLUP);
  pinMode(SILENCE_BTN, INPUT_PULLUP);
  pinMode(REWEIGH_BTN, INPUT); // GPIO35 - external pull-down required

  // ---- LCD ----
  Wire.begin(LCD_SDA, LCD_SCL);

  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("  COOPSENSE V1  ");
  lcd.setCursor(0, 1);
  lcd.print("  //  Boot  //  ");

  // ---- HX711 ----
  scale.begin(HX711_DT, HX711_SCK);
  scale.set_scale(calibration_factor);

  // ---- Preferences ----
  prefs.begin("restock", false);

  // Restore saved tare offset
  if (prefs.isKey("tareOffset")) {
    long savedOffset = prefs.getLong("tareOffset", 0);
    scale.set_offset(savedOffset);

    Serial.print("Loaded tare offset: ");
    Serial.println(savedOffset);
  } else {
    Serial.println("No saved tare offset. Performing initial tare...");
    scale.tare();

    long newOffset = scale.get_offset();
    prefs.putLong("tareOffset", newOffset);

    Serial.print("Saved new tare offset: ");
    Serial.println(newOffset);
  }

  // Restore saved silence state
  if (prefs.isKey("silenceState")) {
    bool savedMuted = prefs.getBool("silenceState", false);
    silenceState = savedMuted ? MUTED : UNMUTED;
  } else {
    silenceState = UNMUTED;
    prefs.putBool("silenceState", false);
  }

  // Restore saved item weight
  if (prefs.isKey("itemWeight")) {
    currentItemWeight = prefs.getFloat("itemWeight", 0.0);

    if (currentItemWeight > 0.0) {
      hasItemWeight = true;

      Serial.print("Loaded item weight: ");
      Serial.print(currentItemWeight, 1);
      Serial.println(" g");
    }
  }

  // Make physical LED match restored state.
  // HIGH = MUTED, LOW = UNMUTED (consistent with handleSilenceButton)
  digitalWrite(LED_SILENCE, silenceState == MUTED ? LOW : HIGH);

  Serial.print("Silence state: ");
  Serial.println(silenceState == MUTED ? "MUTED" : "UNMUTED");

  delay(1500);
  lcd.clear();

  setupWiFiAndServer();
}


// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
  server.handleClient();
  handleTareButton();
  handleSilenceButton();
  handleReweighButton();
  handleReweighProcess();
  updateBuzzerSequence();

  // Weight read timer (independent of button polling, since HX711
  // reads can take a while and would otherwise slow button response)
  if (millis() - lastWeightReadTime >= WEIGHT_READ_INTERVAL) {
    lastWeightReadTime = millis();
    lastGrams = scale.get_units(2);
    applyStateIfChanged(lastGrams);
    float settled;
      if (updateSettleDetector(lastGrams, &settled)) {
        if (settled > lastSettledWeight) {
          // addition/restock event
          updateCapacityEstimate(activeProfileIndex, settled);
        }
        // (a drop is a consumption event — nothing to learn from it
        // for capacity purposes, just useful telemetry if you want it)
      }
  }

  // LCD refresh timer (independent of weight read timer)
  if (
    reweighState == REWEIGH_IDLE &&
    millis() >= messageScreenUntil &&
    millis() - lastLcdUpdateTime >= LCD_REFRESH_INTERVAL
  ) {
    lastLcdUpdateTime = millis();
    updateNormalLCD(lastGrams);
  }

  delay(10);
}


// ============================================================
// THRESHOLD / STATE LOGIC
// ============================================================

StockState computeEffectiveState(float grams) {

  bool useQty = false;

  if (THRESHOLD_MODE == MODE_QTY_ONLY) {
    useQty = true;
  } else if (THRESHOLD_MODE == MODE_FOLLOW_DISPLAY) {
    useQty = (displayState == DISPLAY_QUANTITY);
  }
  // MODE_WEIGHT_ONLY -> useQty stays false

  // No item weight learned yet? Can't compute quantity, fall back to weight.
  if (useQty && !hasItemWeight) {
    useQty = false;
  }

  if (useQty) {
    float qty = grams / currentItemWeight;

    if (qty <= qtyThresholdRstk)  return RESTOCK;
    if (qty <= qtyThresholdLow) return LOW_STOCK;
    return GOOD_STOCK;
  }

  if (grams < gramThresholdRstk)  return RESTOCK;
  if (grams <= gramThresholdLow) return LOW_STOCK;
  return GOOD_STOCK;
}



// Shared by the main loop AND the display-toggle button, so both
// paths apply LED/buzzer consistently.
void applyStateIfChanged(float grams) {
  StockState newState = (activeProfileIndex == -1)? computeEffectiveState(grams) : computeEffectiveStateAuto(grams);
  if (newState != currentState) {
    currentState = newState;
    
    updateLED(currentState);
    saveStockStateToPrefs(currentState);

    if (silenceState == UNMUTED) {
      triggerBuzzer(currentState);
    }
  }
}

void saveStockStateToPrefs(StockState state) {
  prefs.putInt("stockState", (int)state);
}
// ============================================================
// TARE BUTTON
// ============================================================

void handleTareButton() {

  bool reading = digitalRead(TARE_BTN);

  // Detect physical button-state changes
  if (reading != lastTareButtonState) {
    lastTareDebounce = millis();
  }

  // Accept the new state after debounce time
  if (millis() - lastTareDebounce > debounceDelay) {

    if (reading != tareButtonState) {

      tareButtonState = reading;

      // -----------------------------
      // Button pressed
      // -----------------------------
      if (tareButtonState == LOW) {

        tarePressStart = millis();
        tareResetTriggered = false;

        Serial.println("Tare button pressed.");
      }

      // -----------------------------
      // Button released
      // -----------------------------
      else {

        // If the 3-second reset hasn't
        // already happened, this was a
        // normal short press.
        if (!tareResetTriggered) {

          // Grab the weight BEFORE taring, since tare() wipes it
          float beforeWeight = scale.get_units(2);

          Serial.println("Performing normal tare...");

          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Taring...");

          scale.tare();

          long newOffset = scale.get_offset();
          prefs.putLong("tareOffset", newOffset);

          Serial.print("New tare offset: ");
          Serial.println(newOffset);

          // ---- Bottom row: " XXX.Xg -> 0g" ----
          bool negative = beforeWeight < 0;
          float absWeight = negative ? -beforeWeight : beforeWeight;

          char row[17];
          snprintf(
            row, sizeof(row),
            "%c%.1fg -> 0g",
            negative ? '-' : ' ',
            absWeight
          );

          lcd.setCursor(0, 1);
          lcd.print("                "); // clear row first
          lcd.setCursor(0, 1);
          lcd.print(row);

          delay(800); // hold the before/after message long enough to read
          lcd.clear();
        }
      }
    }
  }

  lastTareButtonState = reading;


  // -----------------------------
  // Check for 3-second hold
  // -----------------------------
  if (
    tareButtonState == LOW &&
    !tareResetTriggered &&
    millis() - tarePressStart >= tareHoldTime
  ) {

    tareResetTriggered = true;

    // Reset the actual HX711 offset
    scale.set_offset(0);

    // Save the reset value permanently
    prefs.putLong("tareOffset", 0);

    Serial.println("TARE OFFSET RESET TO 0.");

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("OFFSET RESET");
    lcd.setCursor(0, 1);
    lcd.print("Offset = 0");
  }
}


// ============================================================
// SILENCE BUTTON
// ============================================================

void handleSilenceButton() {

  if (
    digitalRead(SILENCE_BTN) == LOW &&
    millis() - lastSilenceDebounce > debounceSilenceDelay
  ) {

    lastSilenceDebounce = millis();

    // Toggle mute state
    if (silenceState == MUTED) {
      silenceState = UNMUTED;

      prefs.putBool("silenceState", false);

      digitalWrite(LED_SILENCE, HIGH); // HIGH = UNMUTED

      Serial.println("Buzzer UNMUTED.");
    }
    else {
      silenceState = MUTED;

      prefs.putBool("silenceState", true);

      digitalWrite(LED_SILENCE, LOW); // LOW = MUTED

      // Immediately stop a buzzer that may currently be running,
      // including cancelling any in-progress non-blocking sequence.
      stopBuzzer();
      buzzerPhase = BUZZ_IDLE;
      beepsRemaining = 0;

      Serial.println("Buzzer MUTED.");
    }
  }
}

// ============================================================
// REWEIGH BUTTON (GPIO35 — input-only, external pull-down,
// idles LOW, reads HIGH when pressed)
// ============================================================

void handleReweighButton() {

  bool reading = digitalRead(REWEIGH_BTN);

  // Debounce physical button
  if (reading != lastReweighButtonState) {
    lastReweighDebounce = millis();
  }

  if (millis() - lastReweighDebounce > BUTTON_DEBOUNCE) {

    if (reading != reweighButtonState) {

      reweighButtonState = reading;

      // -----------------------------
      // Button pressed
      // -----------------------------
      if (reweighButtonState == HIGH) {
        reweighPressStart = millis();
        Serial.println("Re-weigh button pressed.");
      }

      // -----------------------------
      // Button released
      // -----------------------------
      else {

        unsigned long heldTime = millis() - reweighPressStart;

        // Only treat it as a normal click if
        // the 5-second hold wasn't triggered.
        if (
          heldTime < REWEIGH_HOLD_TIME &&
          reweighState == REWEIGH_IDLE
        ) {
          handleNormalDisplayButton();
        }
      }
    }
  }

  lastReweighButtonState = reading;


  // -----------------------------
  // Detect 5-second hold
  // -----------------------------
  if (
    reweighButtonState == HIGH &&
    reweighState == REWEIGH_IDLE &&
    millis() - reweighPressStart >= REWEIGH_HOLD_TIME
  ) {
    startReweigh();
  }
}

void handleNormalDisplayButton() {

  if (!hasItemWeight) {

    showMessage(
      "Please weigh",
      "item first!",
      2000
    );

    Serial.println("No item weight saved.");

    return;
  }

  // Toggle between quantity and total weight
  if (displayState == DISPLAY_TOTAL_WEIGHT) {
    displayState = DISPLAY_QUANTITY;
  } else {
    displayState = DISPLAY_TOTAL_WEIGHT;
  }

  // Re-evaluate immediately using the new display mode, since
  // MODE_FOLLOW_DISPLAY needs to react to the switch itself.
  applyStateIfChanged(scale.get_units(2));

  Serial.print("Display state: ");
  Serial.println(displayState == DISPLAY_QUANTITY ? "QUANTITY" : "TOTAL WEIGHT");
}

void startReweigh() {

  Serial.println("Starting re-weigh procedure.");

  // Save current tare so it can be restored if the procedure fails
  previousTareOffset = scale.get_offset();

  // Capture the weight BEFORE the temporary tare, for display purposes
  reweighBeforeWeight = scale.get_units(2);

  // Temporarily tare the empty tray
  scale.tare();

  reweighState = REWEIGH_WAITING;

  reweighWaitStart = millis();
  itemDetectionStart = 0;

  Serial.println("Temporary tare applied.");
  Serial.println("Waiting for item...");

  lcd.clear();
}

void handleReweighProcess() {

  switch (reweighState) {

    // =====================================================
    // WAITING FOR ITEM
    // =====================================================

    case REWEIGH_WAITING: {

      float grams = scale.get_units(5);

      unsigned long elapsed = millis() - reweighWaitStart;

      unsigned long remaining =
        (elapsed < REWEIGH_WAIT_TIME)
        ? (REWEIGH_WAIT_TIME - elapsed)
        : 0;

      int remainingSeconds = (remaining + 999) / 1000;

      // ---- Top row cycles every 2 seconds ----
      unsigned long cyclePhase = (millis() / 2000) % 2;

      lcd.setCursor(0, 0);
      if (cyclePhase == 0) {
        lcd.print("Weigh ONE item  ");
      } else {
        bool negative = reweighBeforeWeight < 0;
        float absW = negative ? -reweighBeforeWeight : reweighBeforeWeight;

        char row[17];
        snprintf(row, sizeof(row), "%c%.1fg -> 0g   ", negative ? '-' : ' ', absW);
        lcd.print(row);
      }

      lcd.setCursor(0, 1);
      lcd.print("Waiting...      ");

      lcd.setCursor(11, 1);
      lcd.print("[");
      if (remainingSeconds < 10) {
        lcd.print("0");
      }
      lcd.print(remainingSeconds);
      lcd.print("]");

      // Item detected
      if (grams > 20.0) {

        if (itemDetectionStart == 0) {
          itemDetectionStart = millis();
          Serial.println("Item detected. Checking stability...");
        }

        // Must remain above 20 g for 1 second
        if (millis() - itemDetectionStart >= ITEM_DETECTION_TIME) {

          startMeasurement();
        }

      } else {

        // Fell back below threshold
        itemDetectionStart = 0;
      }

      // 30-second timeout
      if (elapsed >= REWEIGH_WAIT_TIME) {

        restorePreviousTare();

        Serial.println("Re-weigh timed out.");

        showMessage(
          "Re-weigh timeout",
          "Keeping old data",
          2000
        );

        reweighState = REWEIGH_IDLE;
      }

      break;
    }


    // =====================================================
    // MEASURING
    // =====================================================

    case REWEIGH_MEASURING: {

      unsigned long elapsed = millis() - measurementStart;

      float currentReading = scale.get_units(5);

      int remainingSeconds =
        (elapsed < MEASUREMENT_TIME)
        ? (int)((MEASUREMENT_TIME - elapsed + 999) / 1000)
        : 0;

      lcd.setCursor(0, 0);
      lcd.print("Weighing... [");
      lcd.print(remainingSeconds);
      lcd.print("s]");

      lcd.setCursor(0, 1);
      lcd.print("                ");

      lcd.setCursor(0, 1);
      lcd.print(currentReading, 1);
      lcd.print(" g");

      // Take a snapshot every 500 ms
      if (
        millis() - lastSnapshotTime >= SNAPSHOT_INTERVAL &&
        snapshotCount < 10
      ) {

        weightSnapshots[snapshotCount] = currentReading;
        snapshotCount++;

        lastSnapshotTime = millis();

        Serial.print("Snapshot ");
        Serial.print(snapshotCount);
        Serial.print(": ");
        Serial.print(currentReading, 1);
        Serial.println(" g");
      }

      // Finish after 5 seconds
      if (elapsed >= MEASUREMENT_TIME) {

        finishMeasurement();
      }

      break;
    }


    // =====================================================
    // SUCCESS
    // =====================================================

    case REWEIGH_SUCCESS: {

      // Success message is displayed for 1 second
      if (millis() - measurementStart >= MEASUREMENT_TIME + 1000) {

        reweighState = REWEIGH_IDLE;
        displayState = DISPLAY_TOTAL_WEIGHT;

        lcd.clear();
      }

      break;
    }


    default:
      break;
  }
}

void startMeasurement() {

  reweighState = REWEIGH_MEASURING;

  measurementStart = millis();
  lastSnapshotTime = millis();

  snapshotCount = 0;

  Serial.println("Item stable.");
  Serial.println("Starting 5-second measurement.");
}

void finishMeasurement() {

  if (snapshotCount == 0) {

    restorePreviousTare();

    showMessage(
      "Weigh failed",
      "No data",
      2000
    );

    reweighState = REWEIGH_IDLE;

    return;
  }

  float total = 0.0;

  for (int i = 0; i < snapshotCount; i++) {
    total += weightSnapshots[i];
  }

  float averageWeight = total / snapshotCount;

  currentItemWeight = averageWeight;
  activeProfileIndex = findOrCreateProfile(averageWeight);
  hasItemWeight = true;

  // Save the current item's unit weight
  prefs.putFloat("itemWeight", currentItemWeight);

  Serial.print("Average item weight: ");
  Serial.print(currentItemWeight, 1);
  Serial.println(" g");

  // -------------------------------------------------------
  // Restore the EMPTY-TRAY tare (captured before the item was
  // placed), instead of taring now while the item is still on
  // the scale. Taring here would redefine "zero" as "tray +
  // item", so removing the item afterward would read negative.
  // -------------------------------------------------------

  restorePreviousTare();

  prefs.putLong("tareOffset", previousTareOffset);

  Serial.print("Restored empty-tray offset: ");
  Serial.println(previousTareOffset);

  // -------------------------------------------------------
  // Success display
  // -------------------------------------------------------

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("Weigh successful");

  lcd.setCursor(0, 1);
  lcd.print(currentItemWeight, 1);
  lcd.print(" g");

  Serial.println("Re-weigh successful.");

  reweighState = REWEIGH_SUCCESS;
}

void restorePreviousTare() {

  scale.set_offset(previousTareOffset);

  Serial.print("Restored previous tare offset: ");
  Serial.println(previousTareOffset);
}


// ============================================================
// LCD (normal operation screens)
// ============================================================

void updateNormalLCD(float grams) {

  if (!hasItemWeight) {
    lcd.setCursor(0, 0);
    lcd.print("No product data ");
    lcd.setCursor(0, 1);
    lcd.print("Press to weigh ");
    return;
  }

  if (displayState == DISPLAY_QUANTITY) {

    float quantity = grams / currentItemWeight;
    if (quantity < 0) quantity = 0; // clamp, no negatives

    const char* statusStr;
    if (currentState == RESTOCK)        statusStr = "RSTK";
    else if (currentState == LOW_STOCK) statusStr = "LOW";
    else                                  statusStr = "OK";

    char left[12];
    snprintf(left, sizeof(left), "Qty: %.0f", quantity);

    int leftLen = strlen(left);
    int statusLen = strlen(statusStr);
    int padding = 16 - leftLen - statusLen;
    if (padding < 1) padding = 1; // safety net for unexpectedly large numbers

    char topRow[17];
    snprintf(topRow, sizeof(topRow), "%s%*s", left, padding + statusLen, statusStr);

    lcd.setCursor(0, 0);
    lcd.print(topRow);

    lcd.setCursor(0, 1);
    lcd.print("Weight:         ");

    lcd.setCursor(8, 1);
    lcd.print(currentItemWeight, 1);
    lcd.print(" g");

  } else {

    lcd.setCursor(0, 0);
    lcd.print("Total Weight:   ");

    lcd.setCursor(0, 1);
    lcd.print("                ");

    lcd.setCursor(0, 1);
    lcd.print(grams, 2);
    lcd.print(" g");
  }
}

void showMessage(
  const char* line1,
  const char* line2,
  unsigned long duration
) {

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print(line1);

  lcd.setCursor(0, 1);
  lcd.print(line2);

  messageScreenUntil = millis() + duration;

  strncpy(messageLine1, line1, 16);
  messageLine1[16] = '\0';

  strncpy(messageLine2, line2, 16);
  messageLine2[16] = '\0';
}


// ============================================================
// RGB LED
// ============================================================

void updateLED(StockState state) {

  switch (state) {

    case RESTOCK:
      // Red
      setColor(true, false, false);
      break;

    case LOW_STOCK:
      // Yellow
      setColor(false, true, false);
      break;

    case GOOD_STOCK:
      // Green
      setColor(false, false, true);
      break;

    default:
      setColor(false, false, false);
      break;
  }
}


// ============================================================
// BUZZER (non-blocking state machine)
// ============================================================

void triggerBuzzer(StockState state) {

  int beeps = 0;

  if (state == RESTOCK) beeps = 3;
  else if (state == LOW_STOCK) beeps = 1;

  if (beeps == 0) return;

  beepsRemaining = beeps;
  buzzerPhase = BUZZ_ON;
  lastBuzzerActionTime = millis();

  startBuzzer();
}

void updateBuzzerSequence() {

  if (buzzerPhase == BUZZ_IDLE) return;

  // Mute pressed mid-sequence? Kill it immediately.
  if (silenceState == MUTED) {
    stopBuzzer();
    buzzerPhase = BUZZ_IDLE;
    beepsRemaining = 0;
    return;
  }

  unsigned long elapsed = millis() - lastBuzzerActionTime;

  if (buzzerPhase == BUZZ_ON && elapsed >= BEEP_ON_TIME) {
    stopBuzzer();
    beepsRemaining--;

    if (beepsRemaining <= 0) {
      buzzerPhase = BUZZ_IDLE;
    } else {
      buzzerPhase = BUZZ_OFF;
      lastBuzzerActionTime = millis();
    }
  }
  else if (buzzerPhase == BUZZ_OFF && elapsed >= BEEP_OFF_TIME) {
    startBuzzer();
    buzzerPhase = BUZZ_ON;
    lastBuzzerActionTime = millis();
  }
}

void startBuzzer() {

  if (PASSIVE_BUZZER) {
    tone(BUZZER, 2000);
  }
  else {
    digitalWrite(BUZZER, HIGH);
  }
}


void stopBuzzer() {

  if (PASSIVE_BUZZER) {
    noTone(BUZZER);
  }
  else {
    digitalWrite(BUZZER, LOW);
  }
}


// ============================================================
// RGB COLOR CONTROL
// ============================================================

// Assumes common-cathode RGB LED.
// HIGH = channel ON.
//
// Parameters:
// red    = red channel
// yellow = red + green
// green  = green channel

void setColor(bool red, bool yellow, bool green) {

  if (yellow) {
    digitalWrite(LED_R, HIGH);
    digitalWrite(LED_G, HIGH);
    digitalWrite(LED_B, LOW);
    return;
  }

  digitalWrite(LED_R, red ? HIGH : LOW);
  digitalWrite(LED_G, green ? HIGH : LOW);
  digitalWrite(LED_B, LOW);
}


//  WEBSERVER

void setupWiFiAndServer() {

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi ");

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi connected  ");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
    delay(1500);
  } else {
    Serial.println();
    Serial.println("WiFi failed - continuing offline.");

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi failed     ");
    lcd.setCursor(0, 1);
    lcd.print("Running offline ");
    delay(1500);
  }

  // ---- Routes ----
  server.on("/status", HTTP_GET, handleStatusRequest);
  server.on("/nickname", HTTP_POST, handleNicknameUpdate);
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });
  server.enableCORS(true);
  server.begin();
  deviceBootTime = millis();
  
}

void handleStatusRequest() {

  String stateStr;
  switch (currentState) {
    case RESTOCK:   stateStr = "RSTK"; break;
    case LOW_STOCK: stateStr = "LOW";  break;
    case GOOD_STOCK: stateStr = "OK";  break;
    default:        stateStr = "UNKNOWN"; break;
  }

  float quantity = hasItemWeight ? (lastGrams / currentItemWeight) : 0;
  if (quantity < 0) quantity = 0;

  String json = "{";
  json += "\"nickname\":\"" + getDeviceNickname() + "\",";
  json += "\"mac\":\"" + getDeviceId() + "\",";
  json += "\"totalWeight\":" + String(lastGrams, 1) + ",";
  json += "\"unitWeight\":" + String(currentItemWeight, 1) + ",";
  json += "\"quantity\":" + String(quantity, 1) + ",";
  json += "\"status\":\"" + stateStr + "\",";
  json += "\"hasItemWeight\":" + String(hasItemWeight ? "true" : "false") + ",";
  json += "\"silenceState\":\"" + String(silenceState == MUTED ? "MUTED" : "UNMUTED") + "\",";
  json += "\"uptimeMs\":" + String(millis() - deviceBootTime);
  json += "}";

  server.send(200, "application/json", json);
}

void handleNicknameUpdate() {

  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Missing body");
    return;
  }

  String body = server.arg("plain"); // expects raw string, e.g. "Snack Tray A"

  if (body.length() == 0 || body.length() > 32) {
    server.send(400, "text/plain", "Invalid nickname length");
    return;
  }

  prefs.putString("nickname", body);

  Serial.print("Nickname updated to: ");
  Serial.println(body);

  server.send(200, "text/plain", "OK");
}

String getDeviceId() {
  return WiFi.macAddress();
}

String getDeviceNickname() {
  if (prefs.isKey("nickname")) {
    return prefs.getString("nickname", "Unnamed Tray");
  }
  return "Unnamed Tray";
}


// Returns true exactly once, on the tick a NEW settle is confirmed.
// Writes the confirmed weight into *outWeight when it returns true.
bool updateSettleDetector(float grams, float* outWeight) {
  settleBuffer[settleBufIndex] = grams;
  settleBufIndex = (settleBufIndex + 1) % SETTLE_BUFFER_SIZE;
  if (settleBufIndex == 0) settleBufFull = true;
  if (!settleBufFull) return false; // wait for buffer to fill once at boot

  float mn = settleBuffer[0], mx = settleBuffer[0];
  for (int i = 1; i < SETTLE_BUFFER_SIZE; i++) {
    if (settleBuffer[i] < mn) mn = settleBuffer[i];
    if (settleBuffer[i] > mx) mx = settleBuffer[i];
  }
  if ((mx - mn) > SETTLE_NOISE_FLOOR_G) return false; // still moving, not settled

  float candidate = (mn + mx) / 2.0;

  if (!haveSettledBaseline) {
    lastSettledWeight = candidate;
    haveSettledBaseline = true;
    return false; // first-ever settle is just the baseline, not an "event"
  }

  if (fabs(candidate - lastSettledWeight) < EVENT_MIN_DELTA_G) {
    return false; // basically unchanged, nothing to report
  }

  // Real change confirmed
  *outWeight = candidate;
  lastSettledWeight = candidate;
  return true;
}

// ============================================================
// PROFILE MATCHING
// Call when REWEIGH_BTN finishes measuring a single item
// (i.e. right after your existing finishMeasurement() computes
// averageWeight) — this replaces treating currentItemWeight as
// a lone global value.
// ============================================================
int findOrCreateProfile(float measuredUnitWeight) {
  int bestIdx = -1;
  float bestDiff = 1e9;

  for (int i = 0; i < MAX_PROFILES; i++) {
    if (!profiles[i].valid) continue;
    float diff = fabs(profiles[i].unitWeight - measuredUnitWeight) / profiles[i].unitWeight;
    if (diff < PROFILE_MATCH_TOL && diff < bestDiff) {
      bestDiff = diff;
      bestIdx = i;
    }
  }

  if (bestIdx >= 0) {
    profiles[bestIdx].lastSeen = millis();
    return bestIdx;
  }

  // No match -> new item type. Find a free or LRU slot.
  int freeIdx = -1;
  unsigned long oldest = ULONG_MAX;
  int oldestIdx = 0;
  for (int i = 0; i < MAX_PROFILES; i++) {
    if (!profiles[i].valid) { freeIdx = i; break; }
    if (profiles[i].lastSeen < oldest) { oldest = profiles[i].lastSeen; oldestIdx = i; }
  }
  int slot = (freeIdx >= 0) ? freeIdx : oldestIdx;

  profiles[slot].valid          = true;
  profiles[slot].unitWeight     = measuredUnitWeight;
  profiles[slot].fullQtyEstimate = 0.0; // unlearned yet — see below
  profiles[slot].lastSeen       = millis();
  return slot;
}

// ============================================================
// ASYMMETRIC LEAKY-MAX CAPACITY UPDATE
// Call this whenever updateSettleDetector() reports an ADDITION
// event (candidate > previous settled weight) AND you have an
// active profile with a known unitWeight.
// ============================================================
void updateCapacityEstimate(int profileIdx, float settledWeightGrams) {
  if (profileIdx < 0 || !profiles[profileIdx].valid) return;
  if (profiles[profileIdx].unitWeight <= 0.0) return;

  float observedQty = settledWeightGrams / profiles[profileIdx].unitWeight;

  if (profiles[profileIdx].fullQtyEstimate <= 0.0) {
    // First-ever observation for this profile: accept outright
    profiles[profileIdx].fullQtyEstimate = observedQty;
    return;
  }

  float alpha = (observedQty >= profiles[profileIdx].fullQtyEstimate)
                  ? LEAKY_MAX_ALPHA_UP
                  : LEAKY_MAX_ALPHA_DOWN;

  profiles[profileIdx].fullQtyEstimate =
      alpha * observedQty + (1.0 - alpha) * profiles[profileIdx].fullQtyEstimate;
}

// ============================================================
// QUANTIZED PERCENTAGE + STATE (replaces computeEffectiveState)
// ============================================================
StockState computeEffectiveStateAuto(float grams) {
  if (activeProfileIndex < 0 || !profiles[activeProfileIndex].valid) {
    return UNKNOWN; // no learned item yet -> nothing meaningful to say
  }

  ItemProfile &p = profiles[activeProfileIndex];
  if (p.unitWeight <= 0.0 || p.fullQtyEstimate <= 0.0) return UNKNOWN;

  int qty = (int)roundf(grams / p.unitWeight);
  if (qty < 0) qty = 0;
  int capQty = (int)roundf(p.fullQtyEstimate);
  if (capQty < 1) capQty = 1;
  if (qty > capQty) qty = capQty; // clamp — don't report >100%

  float pct = 100.0 * qty / capQty;

  // Hysteresis: only cross a boundary in the direction that
  // matches the "clear" threshold if we're already past it.
  static StockState lastAutoState = UNKNOWN;

  if (lastAutoState == RESTOCK) {
    if (pct > PCT_RESTOCK_CLEAR) lastAutoState = (pct > PCT_LOW_CLEAR) ? GOOD_STOCK : LOW_STOCK;
  } else if (lastAutoState == LOW_STOCK) {
    if (pct <= PCT_RESTOCK_ENTER) lastAutoState = RESTOCK;
    else if (pct > PCT_LOW_CLEAR) lastAutoState = GOOD_STOCK;
  } else { // GOOD_STOCK or UNKNOWN
    if (pct <= PCT_RESTOCK_ENTER) lastAutoState = RESTOCK;
    else if (pct <= PCT_LOW_ENTER) lastAutoState = LOW_STOCK;
    else lastAutoState = GOOD_STOCK;
  }

  return lastAutoState;
}
