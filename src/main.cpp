#include <Arduino.h>           // Include the core Arduino framework libraries
#include <Wire.h>              // Include the I2C communication library for LCD control
#include <LiquidCrystal_I2C.h> // Include the library for I2C Liquid Crystal Displays
#include <Keypad.h>            // Include the matrix keypad interface library

// --- PIN CONFIGURATION ---
const int POT1_PIN = 34;       // Analog input pin for Potentiometer 1 (Motor 1 speed trim)
const int POT2_PIN = 35;       // Analog input pin for Potentiometer 2 (Motor 2 speed trim)
const int SWITCH_PIN = 4;      // Digital input pin for the motor selection toggle switch

const int STEP_PIN_1 = 18;     // Digital output pin for Motor 1 STEP signal (A4988 driver)
const int DIR_PIN_1  = 19;     // Digital output pin for Motor 1 DIRECTION signal (A4988 driver)
const int STEP_PIN_2 = 23;     // Digital output pin for Motor 2 STEP signal (A4988 driver)
const int DIR_PIN_2  = 16;     // Digital output pin for Motor 2 DIRECTION signal (GPIO16 replaces bootstrap GPIO5)

// --- 4x4 MEMBRANE KEYPAD CONFIGURATION ---
const byte ROWS = 4;           // Define the number of rows on the keypad matrix
const byte COLS = 4;           // Define the number of columns on the keypad matrix
char keys[ROWS][COLS] = {      // Define the key map layout corresponding to the physical matrix
  {'1','2','3','A'},           // Row 0 key definitions (Numbers 1-3, Command A -> START)
  {'4','5','6','B'},           // Row 1 key definitions (Numbers 4-6, Command B -> STOP)
  {'7','8','9','C'},           // Row 2 key definitions (Numbers 7-9, Command C -> CLEAR)
  {'*','0','#','D'}            // Row 3 key definitions (Backspace, Zero, Enter, Command D -> TOGGLE DIR)
};                             // End of keys matrix definition

byte rowPins[ROWS] = {13, 14, 27, 26}; // ESP32 GPIO pins connected to the keypad row lines
byte colPins[COLS] = {25, 33, 32, 17}; // ESP32 GPIO pins connected to the keypad column lines

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS); // Initialize Keypad instance with layout and pins
LiquidCrystal_I2C lcd(0x27, 20, 4);                                    // Initialize LCD instance at I2C address 0x27, 20x4 characters

// --- PROCESS VARIABLES ---
int baseV1 = 500;              // Target base frequency for Motor 1 in Hz (Steps per second)
int baseV2 = 500;              // Target base frequency for Motor 2 in Hz (Steps per second)
String inputBuffer = "";       // String buffer to accumulate numeric characters from the keypad

bool targetDir1 = true;        // Hardware direction state for Motor 1 (true = CW / HIGH, false = CCW / LOW)
bool targetDir2 = true;        // Hardware direction state for Motor 2 (true = CW / HIGH, false = CCW / LOW)
bool bufferDir  = true;        // Staged/pending direction state for the currently selected motor inside the editor

bool systemRunning = false;    // Global execution flag tracking machine state (false=STOP, true=RUN)
bool prevRunning   = false;    // Flag storing previous execution state to detect positive edge trigger
int prevSwitchState = -1;      // Variable to track changes in the hardware toggle switch state

// --- ACCELERATION / DECELERATION RAMP ---
int currentV1 = 0;             // Instantaneous velocity profile step for Motor 1 (Hz)
int currentV2 = 0;             // Instantaneous velocity profile step for Motor 2 (Hz)
const int RAMP_STEP        = 10; // Velocity delta increment per ramp execution interval (Hz)
const int RAMP_INTERVAL_MS = 5;  // Fixed millisecond interval separating velocity ramp step updates
unsigned long lastRampTime = 0; // Timestamp tracking the last millisecond execution of the ramp logic

// --- NON-BLOCKING TIMERS ---
unsigned long lastDisplayUpdate = 0; // Timestamp tracking the last millisecond update of the LCD interface
unsigned long lastStepTime1     = 0; // Timestamp tracking the exact microsecond execution of Motor 1's last step
unsigned long lastStepTime2     = 0; // Timestamp tracking the exact microsecond execution of Motor 2's last step

// Converts step frequency (Hz) to a non-blocking pulse period interval in microseconds
unsigned long speedToInterval(int speedHz) {
    if (speedHz <= 0) return 0;      // Return zero interval if target velocity is non-positive to stall motor
    return 1000000UL / speedHz;      // Calculate microsecond period using Unsigned Long literal to prevent overflow
}                                    // End of speedToInterval function

// ─────────────────────────────────────────────────────────────────────────────
void setup() {                       // Hardware and peripheral initialization phase
    Serial.begin(115200);            // Open hardware serial port at high-speed 115200 baud for debugging

    pinMode(POT1_PIN,   INPUT);      // Configure Potentiometer 1 pin as high-impedance analog input
    pinMode(POT2_PIN,   INPUT);      // Configure Potentiometer 2 pin as high-impedance analog input
    pinMode(SWITCH_PIN, INPUT_PULLUP); // Configure toggle switch pin with internal pull-up resistor active

    pinMode(STEP_PIN_1, OUTPUT);     // Configure Motor 1 step line as digital push-pull output
    pinMode(DIR_PIN_1,  OUTPUT);     // Configure Motor 1 direction line as digital push-pull output
    pinMode(STEP_PIN_2, OUTPUT);     // Configure Motor 2 step line as digital push-pull output
    pinMode(DIR_PIN_2,  OUTPUT);     // Configure Motor 2 direction line as digital push-pull output

    digitalWrite(DIR_PIN_1, targetDir1 ? HIGH : LOW); // Write default rotational direction to Motor 1 driver hardware
    digitalWrite(DIR_PIN_2, targetDir2 ? LOW : HIGH); // HARDWARE FIX: Inverted Motor 2 pin mapping to synchronize direction

    lcd.init();                      // Wake up and execute internal initialization routine for the I2C LCD
    lcd.backlight();                 // Enable the LED backlight power rail on the LCD module

    Wire.setClock(400000);           // Increase I2C bus speed to 400kHz to minimize blocking overhead during step cycles

    lcd.setCursor(0, 0); lcd.print("V1:      |T:    |    "); // Render static template framework for Line 0 (Speed, Trim, Direction)
    lcd.setCursor(0, 1); lcd.print("V2:      |T:    |    "); // Render static template framework for Line 1 (Speed, Trim, Direction)
    lcd.setCursor(0, 2); lcd.print("Input:              "); // Clear template line 2 to prevent double brackets
    lcd.setCursor(0, 3); lcd.print("STOP| Ed: ----------"); // Render static template framework for Line 3 (System Status + selection)
}                                    // End of setup function

// ─────────────────────────────────────────────────────────────────────────────
void loop() {                        // Main cyclical program execution thread
    char key         = keypad.getKey();         // Poll the keypad matrix controller for asynchronous key press events
    int  switchState = digitalRead(SWITCH_PIN); // Read physical toggle switch state (LOW = Motor 1, HIGH = Motor 2)

    // ── SWITCH STATE MONITORING & BUFFER SYNC ─────────────────────────────
    if (switchState != prevSwitchState) {       // Evaluate if the operator flipped the physical toggle switch
        bufferDir = (switchState == LOW) ? targetDir1 : targetDir2; // Sync staged buffer direction variable to selected motor's active state
        prevSwitchState = switchState;          // Update switch state memory flag to catch subsequent hardware state changes
    }                                           // End of switch state synchronization context

    // ── KEYPAD CONTROLLER LOGIC ──────────────────────────────────────────
    if (key) {                                  // Evaluate if a valid key press event occurred during polling
        if (key >= '0' && key <= '9') {         // Verify if the incoming character resides within numeric ranges
            if (inputBuffer.length() < 4) inputBuffer += key; // Append numeric character if buffer is under 4-digit limit
        }                                       // End of numeric validation filter
        else if (key == 'D') {                  // Intercept command key 'D' designated to handle pending direction inversion
            bufferDir = !bufferDir;             // Electronically toggle the pending directional state inside the editor buffer
        }                                       // End of direction toggle processing
        else if (key == '#') {                  // Intercept hash key designated as ENTER command to submit staged parameters
            if (inputBuffer.length() > 0) {     // Ensure input buffer contains numeric data prior to executing memory commit
                int val = inputBuffer.toInt();  // Parse raw buffer string data into signed integer frequency metric
                if (switchState == LOW) baseV1 = val; // Apply target parsed metric to Motor 1 if toggle points LOW
                else                   baseV2 = val; // Apply target parsed metric to Motor 2 if toggle points HIGH
                inputBuffer = "";               // Flush input string buffer to receive subsequent inputs cleanly
            }                                   // End of active numeric buffer condition
            
            if (switchState == LOW) {           // Execute parameters processing branch dedicated to Motor 1 hardware profile
                targetDir1 = bufferDir;         // Transfer staged buffer directional parameter into actual active variable
                digitalWrite(DIR_PIN_1, targetDir1 ? HIGH : LOW); // Write the standard physical logic state to DIR_1 hardware pin
            } else {                            // Intercept parameters processing branch dedicated to Motor 2 hardware profile
                targetDir2 = bufferDir;         // Transfer staged buffer directional parameter into actual active variable
                digitalWrite(DIR_PIN_2, targetDir2 ? LOW : HIGH); // HARDWARE FIX: Inverted output matching software abstraction layer
            }                                   // End of directional hardware assignment filter
        }                                       // End of ENTER command processing
        else if (key == '*') {                  // Intercept asterisk key designated as BACKSPACE command
            if (inputBuffer.length() > 0)       // Confirm buffer is populated prior to attempting deletion cycle
                inputBuffer.remove(inputBuffer.length() - 1); // Delete terminal character from string buffer sequence
        }                                       // End of BACKSPACE command processing
        else if (key == 'C') {                  // Intercept alpha key 'C' designated as absolute CLEAR command
            inputBuffer = "";                   // Instantly wipe and reinitialize the input buffer contents
        }                                       // End of CLEAR command processing
        else if (key == 'A') {                  // Intercept alpha key 'A' designated as system START directive
            systemRunning = true;               // Set global run-state machine flag to enable motor driver tasks
        }                                       // End of START command processing
        else if (key == 'B') {                  // Intercept alpha key 'B' designated as system STOP directive
            systemRunning = false;              // Reset global run-state machine flag to halt motor driver tasks
        }                                       // End of STOP command processing
    }                                           // End of keypad processing condition

    // ── RUNTIME TIMER SYNCHRONIZATION ─────────────────────────────────────
    if (systemRunning && !prevRunning) {         // Monitor positive edge switch to detect the exact moment system triggers RUN
        unsigned long now = micros();            // Capture stable snapshot reference of internal system time counter
        lastStepTime1 = now;                     // Synchronize Motor 1 scheduler timer to current time snapshot
        lastStepTime2 = now;                     // Synchronize Motor 2 scheduler timer to current time snapshot
        lastRampTime  = millis();                // Synchronize acceleration profile timer to millisecond clock
    }                                           // End of positive edge conditional synchronization
    prevRunning = systemRunning;                // Cache state flag to compute differential transitions on future passes

    // ── POTENTIOMETER ANALOG SAMPLING ──────────────────────────────────────
    int pot1Raw = analogRead(POT1_PIN);         // Sample 12-bit ADC voltage channel mapped to Potentiometer 1 (0-4095)
    int pot2Raw = analogRead(POT2_PIN);         // Sample 12-bit ADC voltage channel mapped to Potentiometer 2 (0-4095)
    int trim1   = map(pot1Raw, 0, 4095, -5, 5); // Map raw 12-bit values linearly to integer trim percentage (-5% to +5%)
    int trim2   = map(pot2Raw, 0, 4095, -5, 5); // Map raw 12-bit values linearly to integer trim percentage (-5% to +5%)

    // ── TARGET VELOCITY CALCULATION ────────────────────────────────────────
    int finalV1 = constrain(baseV1 + (baseV1 * trim1 / 100), 0, 9999); // Compute and clamp absolute target velocity for Motor 1
    int finalV2 = constrain(baseV2 + (baseV2 * trim2 / 100), 0, 9999); // Compute and clamp absolute target velocity for Motor 2

    // ── ACCELERATION / DECELERATION PROFILER ───────────────────────────────
    if (systemRunning) {                        // Execute step-wise velocity profiles only when machine state is active
        if (millis() - lastRampTime >= (unsigned long)RAMP_INTERVAL_MS) { // Check if velocity profiling cycle interval has expired
            lastRampTime = millis();            // Reset the millisecond timestamp baseline tracking the profiling loop
            currentV1 = (currentV1 < finalV1) ? min(currentV1 + RAMP_STEP, finalV1)   // Accelerate or cap Motor 1 profile speed
                                               : max(currentV1 - RAMP_STEP, finalV1);  // Decelerate or floor Motor 1 profile speed
            currentV2 = (currentV2 < finalV2) ? min(currentV2 + RAMP_STEP, finalV2)   // Accelerate or cap Motor 2 profile speed
                                               : max(currentV2 - RAMP_STEP, finalV2);  // Decelerate or floor Motor 2 profile speed
        }                                       // End of profiling tick interval processing
    } else {                                    // Intercept inactive machine states to reset acceleration structures
        currentV1 = 0;                          // Reset current operational step frequency of Motor 1 instantly to zero
        currentV2 = 0;                          // Reset current operational step frequency of Motor 2 instantly to zero
    }                                           // End of run-state velocity profiling check

    unsigned long stepInterval1 = speedToInterval(currentV1); // Convert current ramp speed into microsecond phase ticks for Motor 1
    unsigned long stepInterval2 = speedToInterval(currentV2); // Convert current ramp speed into microsecond phase ticks for Motor 2

    // ── STEP PULSE GENERATION SCHEDULER (NON-BLOCKING) ─────────────────────
    if (systemRunning) {                        // Authorize low-level stepper driver execution blocks under execution state
        unsigned long currentMicros = micros(); // Capture high-resolution microsecond timer update for task tracking

        // MOTOR 1 STEP EXECUTION TASK
        if (stepInterval1 > 0 && (currentMicros - lastStepTime1 >= stepInterval1)) { // Evaluate if phase period for Motor 1 has completed
            if (currentMicros - lastStepTime1 > stepInterval1 * 2UL) {               // Detect overflow latency exceeding double execution bounds
                lastStepTime1 = currentMicros - stepInterval1;                       // Drop accumulative backlog and bind scheduler to clear latency
            } else {                                                                 // Normal scheduling flow with no execution backlog present
                lastStepTime1 += stepInterval1;                                      // Advance phase timing boundary precisely without temporal drift
            }                                                                        // End of backlog protection filter evaluation
            digitalWrite(STEP_PIN_1, HIGH);     // Assert high state on Motor 1 driver step line to trigger internal driver circuit
            delayMicroseconds(10);              // Maintain pulse shape width duration for safe driver input registration
            digitalWrite(STEP_PIN_1, LOW);      // Clear step line back to low state to complete square wave generation pass
        }                                       // End of Motor 1 scheduler check

        // MOTOR 2 STEP EXECUTION TASK
        currentMicros = micros();               // Force-refresh timer reference to exclude delayMicroseconds overhead from Motor 1 pass
        if (stepInterval2 > 0 && (currentMicros - lastStepTime2 >= stepInterval2)) { // Evaluate if phase period for Motor 2 has completed
            if (currentMicros - lastStepTime2 > stepInterval2 * 2UL) {               // Detect overflow latency exceeding double execution bounds
                lastStepTime2 = currentMicros - stepInterval2;                       // Drop accumulative backlog and bind scheduler to clear latency
            } else {                                                                 // Normal scheduling flow with no execution backlog present
                lastStepTime2 += stepInterval2;                                      // Advance phase timing boundary precisely without temporal drift
            }                                                                        // End of backlog protection filter evaluation
            digitalWrite(STEP_PIN_2, HIGH);     // Assert high state on Motor 2 driver step line to trigger internal driver circuit
            delayMicroseconds(10);              // Maintain pulse shape width duration for safe driver input registration
            digitalWrite(STEP_PIN_2, LOW);      // Clear step line back to low state to complete square wave generation pass
        }                                       // End of Motor 2 scheduler check
    }                                           // End of non-blocking step engine logic block

    // ── GRAPHICAL LCD HMI DISPLAY TASK (150MS REFRESH RATE) ────────────────
    if (millis() - lastDisplayUpdate > 150) {   // Assess if display window refresh rate delay has elapsed
        lastDisplayUpdate = millis();           // Reset display refresh timer milestone tracking structure

        // LINE 0 INTERFACE: Motor 1 Operational State
        lcd.setCursor(3, 0);                    // Relocate I2C text layout cursor boundary to numerical slot position on Line 0
        lcd.print(baseV1);                      // Transmit numerical base velocity string representation to LCD screen
        if      (baseV1 < 10)   lcd.print("   "); // Pad single-digit string values cleanly with three blank trailing characters
        else if (baseV1 < 100)  lcd.print("  ");  // Pad double-digit string values cleanly with two blank trailing characters
        else if (baseV1 < 1000) lcd.print(" ");   // Pad triple-digit string values cleanly with one blank trailing character

        lcd.setCursor(11, 0);                   // Relocate text cursor layout boundary to trim slot position on Line 0
        if (trim1 >= 0) lcd.print("+");         // Append positive operational indicator character if mapped value is positive or zero
        lcd.print(trim1);                       // Transmit raw computed numerical integer trim metric to display screen
        lcd.print("%");                         // Append standard dimensional percentage indicator array block

        lcd.setCursor(16, 0);                   // Shift cursor focus towards the dedicated hardware direction output zone
        lcd.print(targetDir1 ? "CW  " : "CCW "); // Render alphanumeric tag reflecting active physical directional status

        // LINE 1 INTERFACE: Motor 2 Operational State
        lcd.setCursor(3, 1);                    // Relocate I2C text layout cursor boundary to numerical slot position on Line 1
        lcd.print(baseV2);                      // Transmit numerical base velocity string representation to LCD screen
        if      (baseV2 < 10)   lcd.print("   "); // Pad single-digit string values cleanly with three blank trailing characters
        else if (baseV2 < 100)  lcd.print("  ");  // Pad double-digit string values cleanly with two blank trailing characters
        else if (baseV2 < 1000) lcd.print(" ");   // Pad triple-digit string values cleanly with one blank trailing character

        lcd.setCursor(11, 1);                   // Relocate text cursor layout boundary to trim slot position on Line 1
        if (trim2 >= 0) lcd.print("+");         // Append positive operational indicator character if mapped value is positive or zero
        lcd.print(trim2);                       // Transmit raw computed numerical integer trim metric to display screen
        lcd.print("%");                         // Append standard dimensional percentage indicator array block

        lcd.setCursor(16, 1);                   // Shift cursor focus towards the dedicated hardware direction output zone
        lcd.print(targetDir2 ? "CW  " : "CCW "); // Render alphanumeric tag reflecting active physical directional status

        // LINE 2 INTERFACE: Active Buffer Status Feedback
        lcd.setCursor(7, 2);                    // Relocate text cursor layout boundary to standard buffer display field on Line 2
        lcd.print(inputBuffer);                 // Transmit currently accumulated temporary string buffer payload to screen
        lcd.print("    ");                      // Force render blank tail array block to wipe historical character residue

        lcd.setCursor(15, 2);                   // Target the custom visual slot configured to preview buffered selection changes
        lcd.print(bufferDir ? "[CW] " : "[CCW]"); // String length limited to exactly 5 chars. No memory overflow to line 1!

        // LINE 3 INTERFACE: Machine Status Frame Layout
        lcd.setCursor(0, 3);                    // Relocate layout interface cursor boundary to starting index point of Line 3
        lcd.print(systemRunning ? "RUN " : "STOP"); // Evaluate execution status state and render matching state tag text string
        lcd.print("| Ed: ");                    // Append standard text layout divider delimiter array element structure
        lcd.print((switchState == LOW) ? "Motor 1" : "Motor 2"); // Evaluate physical switch state and print selected target tag
    }                                           // End of display interface update execution block
}                                               // End of main loop execution loop context pass