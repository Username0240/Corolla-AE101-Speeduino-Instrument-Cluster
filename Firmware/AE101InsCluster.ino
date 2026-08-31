/*
 * ==============================================================================
 * Toyota Corolla AE101 Digital Cluster & Gauge Driver Firmware
 * ==============================================================================
 * Platform:          Arduino Nano (ATmega328P)
 * Target Vehicle:    Toyota Corolla AE101 (Speeduino ECU + Hall VSS)
 * 
 * Key Features:
 * - Air-Core Tachometer & Speedometer PWM Vector Control via Custom Interpolation
 * - Wear-Leveled EEPROM Ring Buffer for Odometer & Trip Storage
 * - Dual I2C OLED Display Driving (Trip & Total Mileage Readouts)
 * - Interrupt-Driven VSS & Tachometer Signal Frequency Capture
 * - Stop-Detection Deferral to Prevent Excessive EEPROM Write Cycles
 * 
 * Required External Libraries:
 * - TimerOne (by Jesse Tane, Jerome Desrochers, Michael Polli, et al.)
 * - SSD1306Ascii (by Bill Greiman)
 * 
 * Built-In Libraries Used:
 * - Wire, EEPROM, avr/interrupt.h, avr/pgmspace.h
 * ==============================================================================
 */

#include <TimerOne.h>
#include <avr/interrupt.h> 
#include <EEPROM.h>        
#include <Wire.h>
#include <avr/pgmspace.h>       
#include "SSD1306Ascii.h"       
#include "SSD1306AsciiWire.h"   

const byte VSS_PIN = A1;  
const byte TACH_PIN = A2; 
const byte TRIP_RST = 10;

// --- WEAR LEVELING & EEPROM RING BUFFER CONFIG ---
struct MileageData {
  unsigned long odoKm;
  float tripKm;
  unsigned long writeSeq; // Incremental counter to identify newest slot
};

const int EEPROM_START_ADDR = 10;
const int NUM_EEPROM_SLOTS  = 10; 
int activeSlotIndex         = 0;
unsigned long maxWriteSeq   = 0;

// Hardware pulse counters
volatile unsigned long vssInterruptCounter = 0;

volatile unsigned long lastTachTimeMicros = 0;
volatile unsigned long tachIntervalMicros = 0;
volatile bool newTachPulse = false;

volatile unsigned long lastVssTimeMicros = 0;
volatile unsigned long vssIntervalMicros = 0;
volatile bool newVssPulse = false;

volatile byte lastPortCState = 0;
unsigned long lastCalcTime = 0;

float engineRPM = 0.0;
float vehicleSpeedKmh = 0.0;
float totalDistanceKm = 0.0;
float tripDistanceKm = 0.0; 
unsigned long totalRotations = 0;

unsigned long lastOdoPrinted = 99999999; 
float lastTripPrinted = -1.0;

// Saved Baseline Distance Markers
unsigned long savedWholeKilometers = 0; 
unsigned long lastSavedKmMarker = 0;
float savedTripKm = 0.0;

// Trip Reset Tracking
unsigned long tripBasePulses = 0;
bool lastTripBtnState = HIGH;

// Deferred EEPROM Saving & Stop Detection
bool unsavedDataPending = false;
bool wasVehicleMoving   = false;
unsigned long stoppedStartTime = 0;

const float TIRE_DIAMETER_METERS = 0.5048; //first test = 0.5961
const float TIRE_CIRCUMFERENCE_METERS = TIRE_DIAMETER_METERS * 3.141592; 

unsigned long lastNeedleUpdate = 0;

// Motor Pins
const int totalPins = 8;
const int pins[totalPins] = { 8, 9, 7, 6, 2, 3, 4, 5 }; 
volatile int pwmValues[totalPins] = {0, 0, 0, 0, 0, 0, 0, 0};
volatile int pwmCounter = 0;

const int rpmPoints[] PROGMEM =          { 0,      250,     500,      750,    1000,    1250,    1500,    1750,   2000,    2250,  2500,   2750,   3000,   3250,  3500,   3750,   4000,    4250,  4500,   4750,    5000,   5250,    5500,     5750,   6000,    6250,    6500,    6750,     7000,    7250,   7500,    7750,    8000 };
const float rpmAnglePoints[] PROGMEM = { -98.00 , -87.33 , -82.42 , -76.18 , -69.04 , -62.79 , -55.14 , -49.40 ,-40.03 , -29.21,-19.95 ,-7.90 ,  3.26 ,  12.09 ,21.55 , 30.48 , 44.76 ,  56.36 ,65.29 , 75.10 ,  84.03 , 92.51 , 103.22 ,  113.03 , 122.85 , 135.35 , 142.49 , 147.39 , 154.09 , 160.34 , 168.82 , 179.52 , 189.34};
const int rpmNumPoints = 33;

const int spdPoints[] PROGMEM =          { 0,        10,     20,    30,    40,    50,    60,    70,    80,   90,     100,    110,   120,    130,    140,  150,      160,    170,    180,   190,    200,     210,   220,  230, 240};
const float spdAnglePoints[] PROGMEM = { -37 ,     -28.06 ,-20.93 ,-8.27, 1.63,  13.88, 27.55, 43.10, 53.00,65.25 , 72.79 , 83.70, 101.54, 111.90, 130.38,144.42 , 156.23, 162.85, 174.58,189.72, 198.10, 216.05, 220.25, 255, 282.4};
const int spdNumPoints = 25;

SSD1306AsciiWire oled1;
SSD1306AsciiWire oled2;

void commitToEEPROM() {
  activeSlotIndex = (activeSlotIndex + 1) % NUM_EEPROM_SLOTS;
  maxWriteSeq++;

  MileageData dataToSave;
  dataToSave.odoKm = (unsigned long)totalDistanceKm;
  dataToSave.tripKm = tripDistanceKm;
  dataToSave.writeSeq = maxWriteSeq;

  int targetAddr = EEPROM_START_ADDR + (activeSlotIndex * sizeof(MileageData));
  EEPROM.put(targetAddr, dataToSave);

  savedWholeKilometers = dataToSave.odoKm;
  lastSavedKmMarker = savedWholeKilometers;
  savedTripKm = dataToSave.tripKm;
  
  // Baseline pulse count resets to zero relative to newly saved state
  noInterrupts();
  vssInterruptCounter = 0;
  interrupts();
  tripBasePulses = 0;

  unsavedDataPending = false;
}

void loadFromEEPROM() {
  activeSlotIndex = 0;
  maxWriteSeq = 0;
  MileageData newestData = {0, 0.0f, 0};
  bool foundValid = false;

  for (int i = 0; i < NUM_EEPROM_SLOTS; i++) {
    MileageData slotData;
    int addr = EEPROM_START_ADDR + (i * sizeof(MileageData));
    EEPROM.get(addr, slotData);

    if (slotData.writeSeq != 0xFFFFFFFF && slotData.writeSeq > maxWriteSeq) {
      maxWriteSeq = slotData.writeSeq;
      activeSlotIndex = i;
      newestData = slotData;
      foundValid = true;
    }
  }

  if (!foundValid || isnan(newestData.tripKm)) {
    savedWholeKilometers = 0;
    savedTripKm = 0.0f;
    maxWriteSeq = 0;
    activeSlotIndex = 0;
  } else {
    savedWholeKilometers = newestData.odoKm;
    savedTripKm = newestData.tripKm;
  }

  lastSavedKmMarker = savedWholeKilometers;
  totalDistanceKm = (float)savedWholeKilometers;
  tripDistanceKm = savedTripKm;
}

void sweep() {
  for (int per = 0; per <= 100; per++) {
    int x = map(per, 0, 100, 0, 8000);
    int y = map(per, 0, 100, 0, 240);
    setAngleMotor1(rpm_angle((float)x));
    setAngleMotor2(spd_angle((float)y));
    delay(20);
  }
  delay(2000);
  for (int per = 100; per >= 0; per--) {
    int x = map(per, 0, 100, 0, 8000);
    int y = map(per, 0, 100, 0, 240);
    setAngleMotor1(rpm_angle((float)x));
    setAngleMotor2(spd_angle((float)y));
    delay(4);
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; } 

  Wire.begin();
  Wire.setClock(400000L); 

  oled1.begin(&SH1106_128x64, 0x3C);
  oled1.setFont(ZevvPeep8x16); 
  oled1.set2X(); 
  oled1.clear();

  oled2.begin(&SH1106_128x64, 0x3D);
  oled2.setFont(ZevvPeep8x16); 
  oled2.set2X(); 
  oled2.clear();
  oled1.ssd1306WriteCmd(SSD1306_SEGREMAP);   
  oled1.ssd1306WriteCmd(SSD1306_COMSCANINC);  

  for (int i = 0; i < totalPins; i++) {
    pinMode(pins[i], OUTPUT);
  }

  pinMode(VSS_PIN, INPUT_PULLUP);
  pinMode(TACH_PIN, INPUT_PULLUP);
  pinMode(TRIP_RST, INPUT_PULLUP);
  pinMode(A0, INPUT_PULLUP);

  // Load newest active EEPROM slot across ring buffer
  loadFromEEPROM();
  // Start at this value
  // totalDistanceKm = 285000.0;
  // tripDistanceKm = 0.0;
  // commitToEEPROM();


  lastPortCState = PINC;

  cli(); 
  PCICR |= (1 << PCIE1);    
  PCMSK1 |= (1 << PCINT9);  
  PCMSK1 |= (1 << PCINT10); 
  sei(); 

  Timer1.initialize(60); 
  Timer1.attachInterrupt(softwarePWM_ISR);

  oled1.setCursor(2, 4); 
  oled1.print("COROLLA");

  oled2.setCursor(2, 0);
  oled2.print(" AE101"); 

  sweep();
  lastCalcTime = millis();
}

void softwarePWM_ISR() {
  pwmCounter++;
  if (pwmCounter >= 64) pwmCounter = 0; 
  
  int val0 = pwmValues[0]; 
  int val1 = pwmValues[1]; 
  int val2 = pwmValues[2]; 
  int val3 = pwmValues[3]; 
  
  int val4 = pwmValues[4]; 
  int val5 = pwmValues[5]; 
  int val6 = pwmValues[6]; 
  int val7 = pwmValues[7]; 

  byte portD_mask = 0;
  if (pwmCounter < val4) portD_mask |= (1 << PD2); 
  if (pwmCounter < val5) portD_mask |= (1 << PD3); 
  if (pwmCounter < val6) portD_mask |= (1 << PD4); 
  if (pwmCounter < val7) portD_mask |= (1 << PD5); 
  if (pwmCounter < val3) portD_mask |= (1 << PD6); 
  if (pwmCounter < val2) portD_mask |= (1 << PD7); 
  PORTD = (PORTD & 0x03) | (portD_mask & 0xFC);

  byte portB_mask = 0;
  if (pwmCounter < val0) portB_mask |= (1 << PB0); 
  if (pwmCounter < val1) portB_mask |= (1 << PB1); 
  PORTB = (PORTB & 0xFC) | (portB_mask & 0x03);
}

ISR(PCINT1_vect) {
  byte currentPortState = PINC; 
  unsigned long currentMicros = micros();
  byte changedBits = currentPortState ^ lastPortCState;

  if (changedBits & (1 << DDC2)) {
    if (currentPortState & (1 << DDC2)) { 
      if (lastTachTimeMicros > 0) {
        tachIntervalMicros = currentMicros - lastTachTimeMicros;
        newTachPulse = true;
      }
      lastTachTimeMicros = currentMicros;
    }
  }

  if (changedBits & (1 << DDC1)) {
    if (currentPortState & (1 << DDC1)) { 
      vssInterruptCounter++; 
      if (lastVssTimeMicros > 0) {
        vssIntervalMicros = currentMicros - lastVssTimeMicros;
        newVssPulse = true;
      }
      lastVssTimeMicros = currentMicros;
    }
  }
  lastPortCState = currentPortState;
}

void setAngleMotor1(float angleDeg) {
  float rad = angleDeg * PI / 180.0;
  float sinVal = sin(rad), cosVal = cos(rad);
  pwmValues[0] = (sinVal > 0) ? (int)(sinVal * 63) : 0; 
  pwmValues[1] = (sinVal < 0) ? (int)(-sinVal * 63) : 0; 
  pwmValues[2] = (cosVal > 0) ? (int)(cosVal * 63) : 0; 
  pwmValues[3] = (cosVal < 0) ? (int)(-cosVal * 63) : 0; 
}

void setAngleMotor2(float angleDeg) {
  float rad = angleDeg * PI / 180.0;
  float sinVal = sin(rad), cosVal = cos(rad);
  pwmValues[4] = (sinVal > 0) ? (int)(sinVal * 63) : 0; 
  pwmValues[5] = (sinVal < 0) ? (int)(-sinVal * 63) : 0; 
  pwmValues[6] = (cosVal > 0) ? (int)(cosVal * 63) : 0; 
  pwmValues[7] = (cosVal < 0) ? (int)(-cosVal * 63) : 0; 
}

float rpm_angle(float rpm) {
  int p0 = pgm_read_word(&rpmPoints[0]);
  if (rpm <= (float)p0) return pgm_read_float(&rpmAnglePoints[0]);
  int pLast = pgm_read_word(&rpmPoints[rpmNumPoints - 1]);
  if (rpm >= (float)pLast) return pgm_read_float(&rpmAnglePoints[rpmNumPoints - 1]);
  for (int i = 0; i < rpmNumPoints - 1; i++) {
    int pi = pgm_read_word(&rpmPoints[i]);
    int pi1 = pgm_read_word(&rpmPoints[i + 1]);
    if (rpm >= (float)pi && rpm <= (float)pi1) {
      float t = (rpm - (float)pi) / (float)(pi1 - pi);
      float angleStart = pgm_read_float(&rpmAnglePoints[i]);
      float angleEnd = pgm_read_float(&rpmAnglePoints[i + 1]);
      return angleStart + t * (angleEnd - angleStart);
    }
  }
  return pgm_read_float(&rpmAnglePoints[0]);
}

float spd_angle(float spd) {
  int p0 = pgm_read_word(&spdPoints[0]);
  if (spd <= (float)p0) return pgm_read_float(&spdAnglePoints[0]);
  int pLast = pgm_read_word(&spdPoints[spdNumPoints - 1]);
  if (spd >= (float)pLast) return pgm_read_float(&spdAnglePoints[spdNumPoints - 1]);
  for (int i = 0; i < spdNumPoints - 1; i++) {
    int pi = pgm_read_word(&spdPoints[i]);
    int pi1 = pgm_read_word(&spdPoints[i + 1]);
    if (spd >= (float)pi && spd <= (float)pi1) {
      float t = (spd - (float)pi) / (float)(pi1 - pi);
      float angleStart = pgm_read_float(&spdAnglePoints[i]);
      float angleEnd = pgm_read_float(&spdAnglePoints[i + 1]);
      return angleStart + t * (angleEnd - angleStart);
    }
  }
  return pgm_read_float(&spdAnglePoints[0]);
}

void loop() {
  unsigned long timelapse = millis();

// --- Main Metrics Calculation Loop (Every 50ms) ---
  if (timelapse - lastCalcTime >= 50) {
    lastCalcTime = timelapse;

    unsigned long tachSnapshot = 0;
    unsigned long vssSnapshot = 0;
    unsigned long lastTachUpdate = 0;
    unsigned long lastVssUpdate = 0;
    unsigned long currentVssPulses = 0;
    unsigned long loopMicros = 0; 
    bool freshTach = false;
    bool freshVss = false;

    noInterrupts();
    loopMicros = micros(); 
    
    tachSnapshot = tachIntervalMicros;
    vssSnapshot = vssIntervalMicros;
    lastTachUpdate = lastTachTimeMicros;
    lastVssUpdate = lastVssTimeMicros;
    currentVssPulses = vssInterruptCounter;
    
    freshTach = newTachPulse;
    freshVss = newVssPulse;
    newTachPulse = false;
    newVssPulse = false;
    interrupts();

    // --- RPM Calculation ---
    if (loopMicros < lastTachUpdate || (loopMicros - lastTachUpdate) > 750000) {
      engineRPM = 0.0; 
    } else if (freshTach && tachSnapshot > 0) {
      float rawRPM = (1000000.0 / (float)tachSnapshot) * 60.0 / 2.0;
      engineRPM = (rawRPM * 0.40) + (engineRPM * 0.60);
    }

    // --- Speed Calculation ---
    if (loopMicros < lastVssUpdate || (loopMicros - lastVssUpdate) > 1500000) {
      vehicleSpeedKmh = 0.0; 
    } else if (freshVss && vssSnapshot > 0) {
      float tireRPS = (1000000.0 / (float)vssSnapshot) / 4.0;
      float rawSpeed = (tireRPS * TIRE_CIRCUMFERENCE_METERS * 3600.0) / 1000.0;
      vehicleSpeedKmh = (rawSpeed * 0.40) + (vehicleSpeedKmh * 0.60);
    }

    // --- Distance Calculation ---
    totalRotations = (currentVssPulses / 4);
    float currentSessionDistanceKm = (totalRotations * TIRE_CIRCUMFERENCE_METERS) / 1000.0;
    
    totalDistanceKm = (float)lastSavedKmMarker + currentSessionDistanceKm; 

    unsigned long tripPulses = 0;
    if (currentVssPulses >= tripBasePulses) {
      tripPulses = currentVssPulses - tripBasePulses;
    }
    float tripSessionDistanceKm = ((tripPulses / 4) * TIRE_CIRCUMFERENCE_METERS) / 1000.0;
    tripDistanceKm = savedTripKm + tripSessionDistanceKm;

    if (currentSessionDistanceKm >= 0.10) {
      unsavedDataPending = true;
    }

    // --- Trip Reset Button (Pin 10, Active LOW) ---
    bool currentTripBtnState = digitalRead(TRIP_RST);
    if (lastTripBtnState == HIGH && currentTripBtnState == LOW) {
      tripBasePulses = currentVssPulses;
      savedTripKm = 0.0;
      tripDistanceKm = 0.0;
      lastTripPrinted = -1.0; 
      commitToEEPROM(); // Immediately save trip reset state once
    }
    lastTripBtnState = currentTripBtnState;

    // --- Stop-Detection Logic (Single EEPROM Write on Transition to Stop) ---
    if (vehicleSpeedKmh > .8) {
      wasVehicleMoving = true;
      stoppedStartTime = 0;
    } else {
      if (wasVehicleMoving) {
        if (stoppedStartTime == 0) {
          stoppedStartTime = timelapse; // Record moment vehicle stopped
        } else if (timelapse - stoppedStartTime >= 2000) { // Confirmed 2 sec zero speed
          if (unsavedDataPending) {
            commitToEEPROM(); // Single write call
          }
          wasVehicleMoving = false; // Disarm until vehicle drives again
          stoppedStartTime = 0;
        }
      }
    }

    // --- Screen Rendering ---
    if (abs(tripDistanceKm - lastTripPrinted) >= 0.1) {
      lastTripPrinted = tripDistanceKm;
      oled2.setCursor(2, 0); 
      oled2.print(tripDistanceKm, 1);
      oled2.print(" km    "); 
    }

    unsigned long wholeKm = (unsigned long)totalDistanceKm;
    if (wholeKm != lastOdoPrinted) {
      lastOdoPrinted = wholeKm;
      oled1.setCursor(2, 4); 
      if (wholeKm < 100000)  oled1.print("0");
      if (wholeKm < 10000)   oled1.print("0");
      if (wholeKm < 1000)    oled1.print("0");
      if (wholeKm < 100)     oled1.print("0");
      if (wholeKm < 10)      oled1.print("0");
      oled1.print(wholeKm);
      oled1.print("km        ");
    }
  }

  // --- Active Needle Processing (Every 25ms) ---
  if (timelapse - lastNeedleUpdate >= 25) {
    lastNeedleUpdate = timelapse;

    Serial.print("RPM: ");
    Serial.print(engineRPM);
    Serial.print("\tSpeed: ");
    Serial.print(vehicleSpeedKmh);
    Serial.print("\tTrip: ");
    Serial.print(tripDistanceKm);
    Serial.print("\tUnsaved: ");
    Serial.println(unsavedDataPending ? "YES" : "NO");
    
    setAngleMotor1(rpm_angle(engineRPM)); 
    setAngleMotor2(spd_angle(vehicleSpeedKmh)); 
  }
}