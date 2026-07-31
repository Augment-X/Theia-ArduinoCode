/*
  Control Board - Arduino Nano
  - 3-Channel LED PWM control on hardware PWM pins (D3, D5, D6)
  - Fan PWM control on hardware PWM pin (D9)
  - Thermal regulation (Fan cooling / LED heating)
  - Overheat safety shutoff (+10°C above target)
  - Power-on safety delay (5s)
  - Production line ON/OFF toggle switch (D8)
  - Camera trigger with 100% LED strobe flash (D10, D11, D12)
  - I2C LCD Display status monitor (A4/A5)
*/

#include <Wire.h> 
#include <LiquidCrystal_I2C.h>

// --- PIN CONFIGURATION ---
const int PIN_NTC_1 = A0;         // Analog input for 1st NTC sensor
const int PIN_NTC_2 = A1;         // Analog input for 2nd NTC sensor

// 3 Independent PWM LED Channels (D3, D5, D6)
const int NUM_LED_CHANNELS = 3;
const int PIN_LEDS[NUM_LED_CHANNELS] = {3, 5, 6}; 

const int PIN_FAN = 9;            // PWM digital output for Fan (D9)
const int PIN_STATUS_LED = 7;     // External status indicator LED (D7)
const int PIN_BUTTON_ON_OFF = 8;  // Production line ON/OFF switch (D8)

// Camera Triggers
const int PIN_CAM_1 = 10;         // Camera 1 Trigger (D10)
const int PIN_CAM_2 = 11;         // Camera 2 Trigger (D11)
const int PIN_CAM_3 = 12;         // Camera 3 Trigger (D12)

// LCD I2C Configuration (Default address 0x27, 16 columns, 2 lines)
LiquidCrystal_I2C lcd(0x27, 16, 2); 

// --- ADJUSTABLE PARAMETERS ---
const float TEMPERATURE_CIBLE = 25.0;       // Target system temperature (°C)
const float BANDE_REGULATION = 4.0;        // Proportional regulation band (°C)
const float SEUIL_SECURITE_MAX = 10.0;     // Overheat offset threshold (°C)

const unsigned long START_DELAY_MS = 5000;    // Boot delay before enabling system (ms)
const unsigned long CAMERA_INTERVAL_MS = 3000; // Time interval between camera triggers (ms)
const unsigned long FLASH_DURATION_MS = 100;   // Duration of pulse trigger and LED strobe (ms)

// --- TIMING & STATE VARIABLES ---
unsigned long previousMillisTemp = 0;       // Timer for thermal check loop
const long tempInterval = 1000;             // Thermal sampling interval (1 second)

unsigned long previousMillisCam = 0;        // Timer for camera trigger loop
unsigned long flashStartMillis = 0;         // Start time of active strobe flash

bool systemReady = false;         // Set to true after initial 5s safety boot delay
bool isFlashing = false;          // True when a strobe flash cycle is running
bool alerteSurchauffe = false;     // Safety flag for critical temperature
int puissanceLedsRegul = 0;       // Calculated heating PWM value (0-255)
float currentTemperature = 0.0;   // Measured system average temperature

// Helper function to update all 3 LED PWM outputs simultaneously
void setAllLeds(int value) {
  for (int i = 0; i < NUM_LED_CHANNELS; i++) {
    analogWrite(PIN_LEDS[i], value);
  }
}

void setup() {
  Serial.begin(9600);             
  
  // Configure Outputs
  pinMode(PIN_FAN, OUTPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);
  
  for (int i = 0; i < NUM_LED_CHANNELS; i++) {
    pinMode(PIN_LEDS[i], OUTPUT);
  }

  pinMode(PIN_BUTTON_ON_OFF, INPUT_PULLUP); // Switch connected to GND
  pinMode(PIN_CAM_1, OUTPUT);
  pinMode(PIN_CAM_2, OUTPUT);
  // pinMode(PIN_CAM_3, OUTPUT);

  // Force all outputs OFF on boot
  analogWrite(PIN_FAN, 0);
  digitalWrite(PIN_STATUS_LED, LOW);
  setAllLeds(0);
  digitalWrite(PIN_CAM_1, LOW);
  digitalWrite(PIN_CAM_2, LOW);

  // Initialize LCD display
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("SYSTEM BOOTING...");
  lcd.setCursor(0, 1);
  lcd.print("Wait 5s...");

  Serial.println("System starting. Waiting for boot safety delay...");
}

void loop() {
  unsigned long currentMillis = millis();   

  // 1. BOOT SAFETY DELAY
  if (!systemReady) {
    if (currentMillis >= START_DELAY_MS) {
      systemReady = true;                   
      lcd.clear();
      Serial.println("--- System Ready and Active ---");
    }
    return;                                 
  }

  // Read production line switch status (LOW = RUN/Active, HIGH = STANDBY/Idle)
  bool ligneActive = (digitalRead(PIN_BUTTON_ON_OFF) == LOW);

  // Drive system status LED
  digitalWrite(PIN_STATUS_LED, ligneActive ? HIGH : LOW);

  // 2. THERMAL CONTROL & SAFETY REGULATION (Every 1s)
  if (currentMillis - previousMillisTemp >= tempInterval) {
    previousMillisTemp = currentMillis;     

    // Read and average NTC sensors
    int rawAverage = (analogRead(PIN_NTC_1) + analogRead(PIN_NTC_2)) / 2;
    currentTemperature = conversionNTC(rawAverage); 
    float error = currentTemperature - TEMPERATURE_CIBLE; 

    int puissanceFan = 0;                   
    puissanceLedsRegul = 0;                 

    // --- OVERHEAT SAFETY CHECK ---
    if (currentTemperature > (TEMPERATURE_CIBLE + SEUIL_SECURITE_MAX)) {
      alerteSurchauffe = true;              
      Serial.println("!!! OVERHEAT ALERT !!!");
    } else {
      alerteSurchauffe = false;             
    }

    if (alerteSurchauffe) {
      // Emergency Cooling Mode
      puissanceFan = 255;
      puissanceLedsRegul = 0;
    } 
    else {
      // Proportional Regulation
      if (error > 0) {                      // Too hot -> Ramp up cooling
        float ratio = error / BANDE_REGULATION;
        if (ratio > 1.0) ratio = 1.0;       
        puissanceFan = ratio * 255;         
      } 
      else if (error < 0) {                 // Too cold -> Ramp up heating via LEDs
        float ratio = (-error) / BANDE_REGULATION;
        if (ratio > 1.0) ratio = 1.0;       
        puissanceLedsRegul = ratio * 255;   
      }
    }

    // Apply Fan PWM
    analogWrite(PIN_FAN, puissanceFan);
    
    // Apply Heating PWM to all 3 LED channels (if not currently flashing)
    if (!isFlashing) {
      setAllLeds(puissanceLedsRegul);
    }

    // --- REFRESH LCD ---
    lcd.setCursor(0, 0);
    lcd.print("Temp: ");
    lcd.print(currentTemperature, 1); 
    lcd.print((char)223);             // Degree '°' symbol
    lcd.print("C  ");                 

    lcd.setCursor(0, 1);
    if (alerteSurchauffe) {
      lcd.print("!! OVERHEAT !!  ");
    } else if (ligneActive) {
      lcd.print("Status: RUNNING ");
    } else {
      lcd.print("Status: STANDBY ");
    }

    // Serial debug telemetry
    Serial.print("Temp: "); Serial.print(currentTemperature);
    if (alerteSurchauffe) Serial.print(" [ALERT]");
    Serial.println(" degC");
  }

  // 3. CAMERA TRIGGER & STROBE FLASH CONTROL
  if (ligneActive) {
    if (currentMillis - previousMillisCam >= CAMERA_INTERVAL_MS) {
      if (!isFlashing && !alerteSurchauffe) {
        previousMillisCam = currentMillis;  
        flashStartMillis = currentMillis;   
        isFlashing = true;                  

        // Burst all 3 LED zones to 100% brightness
        setAllLeds(255);

        // Send high trigger pulse to camera optocouplers/inputs
        digitalWrite(PIN_CAM_1, HIGH);
        digitalWrite(PIN_CAM_2, HIGH);
        digitalWrite(PIN_CAM_3, HIGH);
        
        Serial.println("CAMERA TRIGGER + TRIPLE LED FLASH!");
      }
    }
  }

  // 4. END PULSE AND RESTORE PWM
  if (isFlashing) {
    if (currentMillis - flashStartMillis >= FLASH_DURATION_MS) {
      isFlashing = false;                    

      // End camera trigger pulse
      digitalWrite(PIN_CAM_1, LOW);
      digitalWrite(PIN_CAM_2, LOW);
      digitalWrite(PIN_CAM_3, LOW);

      // Return LEDs to calculated background heating level
      setAllLeds(puissanceLedsRegul);
      
      Serial.println("Trigger pulse completed.");
    }
  }
}

// Linearized temperature conversion for NTC
float conversionNTC(int rawValue) {
  return (1023 - rawValue) * 0.1; 
}