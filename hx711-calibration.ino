#include "HX711.h"

const int LOADCELL_DOUT_PIN = 2;
const int LOADCELL_SCK_PIN = 3;

HX711 scale;

void setup() {
  Serial.begin(57600);
  delay(1000);
  
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  
  Serial.println("Clearing/Tares the scale...");
  scale.set_scale(); // Set scale to 1
  scale.tare();      // Reset the scale to 0
  
  Serial.println("Place your known weight on the scale now.");
  delay(5000); // Wait 5 seconds for you to put the weight
  
  long reading = scale.get_units(10); // Get average of 10 readings
  Serial.print("Reading divided by known weight gives factor: ");
  
  // CHANGE 100.0 BELOW TO YOUR EXACT KNOWN WEIGHT (e.g., grams or ounces)
  float known_weight = 100.0; 
  float calibration_factor = (reading / known_weight);
  
  Serial.println(calibration_factor);
  Serial.print("Use this in your main sketch: scale.set_scale(");
  Serial.print(calibration_factor);
  Serial.println(");");
}

void loop() {
  // Nothing to do here during calibration
}
