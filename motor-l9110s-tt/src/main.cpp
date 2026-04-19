#if __has_include(<Arduino.h>)
#include <Arduino.h>
#else
// Minimal Arduino-compatible stubs for hosts/IDEs where Arduino.h is unavailable
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#define HIGH 0x1
#define LOW 0x0
#define INPUT 0x0
#define OUTPUT 0x1
#define LED_BUILTIN 13
#define A0 0
#define A1 1
#define A2 2

inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return LOW; }
inline unsigned long millis() { return 0UL; }
inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned int) {}
inline unsigned long pulseIn(int, int, unsigned long = 1000000UL) { return 0UL; }
inline void ledcSetup(int, int, int) {}
inline void ledcAttachPin(int, int) {}
inline void ledcWrite(int, int) {}

struct SerialClass {
  void begin(int) {}
  int available() { return 0; }
  int read() { return -1; }

  void print(const char*) {}
  void println(const char*) {}

  void print(char c) { (void)c; }
  void println(char c) { (void)c; }

  void print(int v) { (void)v; }
  void println(int v) { (void)v; }

  void print(unsigned int v) { (void)v; }
  void println(unsigned int v) { (void)v; }

  void print(unsigned long v) { (void)v; }
  void println(unsigned long v) { (void)v; }

  void print(float v) { (void)v; }
  void println(float v) { (void)v; }

  void print(bool b) { (void)b; }
  void println(bool b) { (void)b; }
} Serial;
#endif

/*
  ============================================================
  FINAL MERGED MAIN.CPP
  ============================================================

  This file merges:

  1) Original motor subsystem code
     - TT motor + L9110S
     - IR obstacle sensor
     - safety gating / state machine
     - serial commands
     - debug heartbeat and state prints

  2) Ultrasonic subsystem code
     - HC-SR04 distance measurement
     - serial distance print

  3) Ultrasonic LED traffic-light subsystem
     - red / yellow / green indication
     - level thresholds for waste / litter status

  ------------------------------------------------------------
  Board:
  - Adafruit Feather ESP32 V2 / ESP32 Development Board

  Original motor subsystem wiring:
  - A0 -> L9110S input 1
  - A1 -> L9110S input 2
  - A2 -> IR obstacle OUT
  - LED_BUILTIN -> original status LED

  Ultrasonic wiring:
  - trigPin = 25
  - echoPin = 26

  Traffic-light LED pins:
  - redPin    = 27   (changed from 13 to avoid conflict with LED_BUILTIN)
  - yellowPin = 12
  - greenPin  = 14
*/

// ============================================================
// -------------------- MOTOR / IR SUBSYSTEM -------------------
// ============================================================

// -------------------- Pin Mapping --------------------
static const int PIN_IN1 = A0;      // L9110S Motor-B input 1
static const int PIN_IN2 = A1;      // L9110S Motor-B input 2
static const int PIN_IR_OUT = A2;   // IR obstacle module digital output
static const int PIN_STATUS_LED = LED_BUILTIN;

// -------------------- PWM (LEDC) Setup --------------------
static const int CH1 = 0;
static const int CH2 = 1;
static const int PWM_RES_BITS = 8;       // 0..255
static const int PWM_FREQ_HZ = 20000;    // quiet PWM

// -------------------- Motor Parameters --------------------
static const uint8_t DUTY_MAX = 220;             // adjust for torque vs noise (0..255)
static const uint8_t RAMP_STEP = 5;              // duty increment/decrement per step
static const uint32_t RAMP_STEP_DELAY_MS = 8;    // ramp smoothness

// -------------------- Timing Parameters --------------------
static const uint32_t CLEAN_RUN_MS = 3500;                   // cleaning action duration
static const uint32_t POST_STOP_MS = 800;                   // pause after motor stops
static const uint32_t COOLDOWN_MS = 3UL * 60UL * 1000UL;    // 3 minutes
static const uint32_t LEAVE_CONFIRM_MS = 1500;              // wait after "cat leaves" before cleaning
static const uint32_t DEBOUNCE_MS = 80;                     // debounce for IR transitions

// -------------------- IR Logic Parameters --------------------
static bool IR_ACTIVE_LOW = true;

// Trigger policy:
// false -> event fires when obstacle is no longer detected (cat leaves) [recommended]
// true  -> event fires when obstacle is detected (cat arrives)
static bool TRIGGER_ON_DETECT = false;

// -------------------- State Machine --------------------
enum class SysState {
  IDLE,
  CAT_PRESENT,
  WAIT_LEAVE_DELAY,
  CLEANING,
  COOLDOWN,
  STOPPED_MANUAL
};

static SysState state = SysState::IDLE;

static uint32_t t_state_enter = 0;
static uint32_t t_last_clean = 0;
static uint32_t t_last_edge_ms = 0;

// -------------------- Serial Monitoring --------------------
static uint32_t g_lastAliveMs = 0;
static bool g_lastObstacle = false;
static bool g_hasLastObstacle = false;

// ============================================================
// -------------------- ULTRASONIC SUBSYSTEM -------------------
// ============================================================

// -------------------- Ultrasonic Pins --------------------
static const int trigPin = 25;
static const int echoPin = 26;

// -------------------- Traffic Light Pins --------------------
// NOTE: redPin changed from 13 -> 27 to avoid conflict with LED_BUILTIN
static const int redPin = 27;
static const int yellowPin = 12;
static const int greenPin = 14;

// -------------------- Ultrasonic Thresholds (cm) --------------------
static const float redThreshold = 5.0f;       // distance <= 5 cm -> RED
static const float yellowThreshold = 10.0f;   // 5 < distance <= 10 cm -> YELLOW

// -------------------- Ultrasonic Sampling --------------------
static const int ULTRA_NUM_SAMPLES = 5;
static const unsigned long ULTRA_TIMEOUT_US = 30000UL;
static const unsigned long ULTRA_SAMPLE_GAP_MS = 20;
static const unsigned long ULTRA_UPDATE_PERIOD_MS = 500;

static float g_lastDistanceCm = -1.0f;
static uint32_t g_lastUltrasonicUpdateMs = 0;

// ============================================================
// -------------------- FORWARD DECLARATIONS -------------------
// ============================================================

// Motor / IR helpers
static void logStateChange(const char* nextState);
static void logMotorAction(const char* msg);
static void heartbeat();
static void logIRChangeIfAny(bool obstacleNow);

static void motorStopHard();
static void motorForwardDuty(uint8_t duty);
static void motorReverseDuty(uint8_t duty);
static void motorRampForward(uint8_t dutyTarget);
static void motorRampStopFrom(uint8_t dutyStart);

static void setState(SysState s);
static bool irObstacleDetectedRaw();
static bool irEdgeEventDebounced(bool obstacleNow);

static void printHelp();
static const char* stateName(SysState s);
static void printStatus();
static void handleSerial();

// Ultrasonic helpers
static float readDistanceSingle();
static float readDistanceAveraged(int samples);
static void setTrafficLight(bool redOn, bool yellowOn, bool greenOn);
static void updateTrafficLightFromDistance(float distance);
static const char* levelNameFromDistance(float distance);
static void updateUltrasonicMonitor(bool forcePrint = false);

// ============================================================
// -------------------- MOTOR / IR FUNCTIONS -------------------
// ============================================================

static void logStateChange(const char* nextState) {
  Serial.print("[STATE] -> ");
  Serial.println(nextState);
}

static void logMotorAction(const char* msg) {
  Serial.print("[MOTOR] ");
  Serial.println(msg);
}

static void heartbeat() {
  uint32_t now = millis();
  if (now - g_lastAliveMs >= 1000) {
    g_lastAliveMs = now;
    unsigned long b = now / 1000UL;

    Serial.print("Alive:");
    Serial.print(b);
    Serial.println(' ');
  }
}

static void logIRChangeIfAny(bool obstacleNow) {
  if (!g_hasLastObstacle) {
    g_hasLastObstacle = true;
    g_lastObstacle = obstacleNow;
    Serial.print("[IR] init -> ");
    Serial.println(obstacleNow ? "BLOCKED" : "CLEAR");
    return;
  }

  if (obstacleNow != g_lastObstacle) {
    g_lastObstacle = obstacleNow;
    Serial.print("[IR] change -> ");
    Serial.println(obstacleNow ? "BLOCKED" : "CLEAR");
  }
}

static void motorStopHard() {
  ledcWrite(CH1, 0);
  ledcWrite(CH2, 0);
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IN2, LOW);
}

static void motorForwardDuty(uint8_t duty) {
  ledcWrite(CH2, 0);
  digitalWrite(PIN_IN2, LOW);
  ledcWrite(CH1, duty);
}

static void motorReverseDuty(uint8_t duty) {
  ledcWrite(CH1, 0);
  digitalWrite(PIN_IN1, LOW);
  ledcWrite(CH2, duty);
}

static void motorRampForward(uint8_t dutyTarget) {
  logMotorAction("Soft-start (ramp up)");
  for (uint16_t d = 0; d <= dutyTarget; d += RAMP_STEP) {
    motorForwardDuty((uint8_t)d);
    delay(RAMP_STEP_DELAY_MS);
  }
  logMotorAction("Ramp up complete");
}

static void motorRampStopFrom(uint8_t dutyStart) {
  logMotorAction("Soft-stop (ramp down)");
  for (int d = dutyStart; d >= 0; d -= (int)RAMP_STEP) {
    motorForwardDuty((uint8_t)d);
    delay(RAMP_STEP_DELAY_MS);
  }
  motorStopHard();
  logMotorAction("Motor hard stop issued");
}

static void setState(SysState s) {
  state = s;
  t_state_enter = millis();

  logStateChange(
    (s == SysState::IDLE) ? "IDLE" :
    (s == SysState::CAT_PRESENT) ? "CAT_PRESENT" :
    (s == SysState::WAIT_LEAVE_DELAY) ? "WAIT_LEAVE_DELAY" :
    (s == SysState::CLEANING) ? "CLEANING" :
    (s == SysState::COOLDOWN) ? "COOLDOWN" :
    (s == SysState::STOPPED_MANUAL) ? "STOPPED_MANUAL" : "UNKNOWN"
  );
}

static bool irObstacleDetectedRaw() {
  int v = digitalRead(PIN_IR_OUT);
  return IR_ACTIVE_LOW ? (v == LOW) : (v == HIGH);
}

static bool irEdgeEventDebounced(bool obstacleNow) {
  static bool lastObstacle = false;
  uint32_t now = millis();

  if ((now - t_last_edge_ms) < DEBOUNCE_MS) {
    lastObstacle = obstacleNow;
    return false;
  }

  bool event = false;

  if (TRIGGER_ON_DETECT) {
    if (obstacleNow && !lastObstacle) event = true;
  } else {
    if (!obstacleNow && lastObstacle) event = true;
  }

  if (event) {
    t_last_edge_ms = now;
  }

  lastObstacle = obstacleNow;
  return event;
}

static void printHelp() {
  Serial.println(' ');
  Serial.println("=== Cat Litterbox Final Prototype ===");
  Serial.println("Motor / IR commands:");
  Serial.println("h : help");
  Serial.println("p : print full status");
  Serial.println("c : force cleaning now");
  Serial.println("s : emergency stop (latched)");
  Serial.println("r : reset from STOPPED back to IDLE");
  Serial.println("d : toggle trigger edge (detect <-> leave)");
  Serial.println("l : toggle IR active logic (active-low <-> active-high)");
  Serial.println("u : print ultrasonic reading now");
  Serial.println("=====================================");
}

static const char* stateName(SysState s) {
  switch (s) {
    case SysState::IDLE: return "IDLE";
    case SysState::CAT_PRESENT: return "CAT_PRESENT";
    case SysState::WAIT_LEAVE_DELAY: return "WAIT_LEAVE_DELAY";
    case SysState::CLEANING: return "CLEANING";
    case SysState::COOLDOWN: return "COOLDOWN";
    case SysState::STOPPED_MANUAL: return "STOPPED_MANUAL";
    default: return "UNKNOWN";
  }
}

static void printStatus() {
  Serial.println("--------------- FULL STATUS ---------------");

  Serial.print("MotorState=");
  Serial.println(stateName(state));

  Serial.print("IR_ACTIVE_LOW=");
  Serial.println(IR_ACTIVE_LOW ? "true" : "false");

  Serial.print("TRIGGER_ON_DETECT=");
  Serial.println(TRIGGER_ON_DETECT ? "true" : "false");

  Serial.print("obstacleDetected=");
  Serial.println(irObstacleDetectedRaw() ? "true" : "false");

  Serial.print("lastCleanAgo(ms)=");
  Serial.println((unsigned long)(millis() - t_last_clean));

  Serial.print("UltrasonicDistance(cm)=");
  Serial.println(g_lastDistanceCm);

  Serial.print("WasteLevelStatus=");
  Serial.println(levelNameFromDistance(g_lastDistanceCm));

  Serial.println("-------------------------------------------");
}

static void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') continue;

    if (c == 'h') {
      printHelp();
    } else if (c == 'p') {
      printStatus();
    } else if (c == 's') {
      Serial.println("Manual STOP (latched)");
      logMotorAction("Manual STOP requested");
      motorStopHard();
      setState(SysState::STOPPED_MANUAL);
    } else if (c == 'r') {
      Serial.println("Reset -> IDLE");
      logMotorAction("Manual reset requested");
      motorStopHard();
      setState(SysState::IDLE);
    } else if (c == 'c') {
      Serial.println("Manual CLEAN start");
      logMotorAction("Manual CLEAN requested");
      if (state != SysState::STOPPED_MANUAL) {
        setState(SysState::CLEANING);
      }
    } else if (c == 'd') {
      TRIGGER_ON_DETECT = !TRIGGER_ON_DETECT;
      Serial.print("TRIGGER_ON_DETECT toggled -> ");
      Serial.println(TRIGGER_ON_DETECT ? "true (detect event)" : "false (leave event)");
    } else if (c == 'l') {
      IR_ACTIVE_LOW = !IR_ACTIVE_LOW;
      Serial.print("IR_ACTIVE_LOW toggled -> ");
      Serial.println(IR_ACTIVE_LOW ? "true (active LOW)" : "false (active HIGH)");
    } else if (c == 'u') {
      updateUltrasonicMonitor(true);
    }
  }
}

// ============================================================
// -------------------- ULTRASONIC FUNCTIONS -------------------
// ============================================================

static float readDistanceSingle() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);

  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long duration = pulseIn(echoPin, HIGH, ULTRA_TIMEOUT_US);

  if (duration == 0) {
    return -1.0f;
  }

  float distance = duration / 58.0f;
  return distance;
}

static float readDistanceAveraged(int samples) {
  float sum = 0.0f;
  int validCount = 0;

  for (int i = 0; i < samples; i++) {
    float d = readDistanceSingle();
    if (d > 0) {
      sum += d;
      validCount++;
    }
    delay(ULTRA_SAMPLE_GAP_MS);
  }

  if (validCount == 0) {
    return -1.0f;
  }

  return sum / validCount;
}

static void setTrafficLight(bool redOn, bool yellowOn, bool greenOn) {
  digitalWrite(redPin, redOn ? HIGH : LOW);
  digitalWrite(yellowPin, yellowOn ? HIGH : LOW);
  digitalWrite(greenPin, greenOn ? HIGH : LOW);
}

static const char* levelNameFromDistance(float distance) {
  if (distance <= 0) return "INVALID";
  if (distance <= redThreshold) return "RED - Clean immediately";
  if (distance <= yellowThreshold) return "YELLOW - Cleaning recommended";
  return "GREEN - No cleaning needed";
}

static void updateTrafficLightFromDistance(float distance) {
  if (distance <= 0) {
    setTrafficLight(false, false, false);
  } else if (distance <= redThreshold) {
    setTrafficLight(true, false, false);
  } else if (distance <= yellowThreshold) {
    setTrafficLight(false, true, false);
  } else {
    setTrafficLight(false, false, true);
  }
}

static void updateUltrasonicMonitor(bool forcePrint) {
  uint32_t now = millis();
  if (!forcePrint && (now - g_lastUltrasonicUpdateMs) < ULTRA_UPDATE_PERIOD_MS) {
    return;
  }

  g_lastUltrasonicUpdateMs = now;
  g_lastDistanceCm = readDistanceAveraged(ULTRA_NUM_SAMPLES);

  updateTrafficLightFromDistance(g_lastDistanceCm);

  Serial.print("[ULTRA] Distance: ");
  Serial.print(g_lastDistanceCm);
  Serial.println(" cm");

  Serial.print("[ULTRA] Status: ");
  Serial.println(levelNameFromDistance(g_lastDistanceCm));
}

// ============================================================
// -------------------- SETUP / LOOP ---------------------------
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println(' ');
  Serial.println("[BOOT] Final merged prototype starting...");
  Serial.println("[BOOT] TT Motor + L9110S + IR + HC-SR04 + Traffic Light");
  Serial.println("[BOOT] Use 'h' for help. Monitor baud = 115200.");

  // Original motor subsystem setup
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);

  pinMode(PIN_IR_OUT, INPUT);
  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);

  ledcSetup(CH1, PWM_FREQ_HZ, PWM_RES_BITS);
  ledcSetup(CH2, PWM_FREQ_HZ, PWM_RES_BITS);
  ledcAttachPin(PIN_IN1, CH1);
  ledcAttachPin(PIN_IN2, CH2);

  motorStopHard();
  t_last_clean = 0;

  // Ultrasonic setup
  pinMode(echoPin, INPUT);
  pinMode(trigPin, OUTPUT);
  digitalWrite(trigPin, LOW);

  pinMode(redPin, OUTPUT);
  pinMode(yellowPin, OUTPUT);
  pinMode(greenPin, OUTPUT);
  setTrafficLight(false, false, false);

  setState(SysState::IDLE);
  printHelp();
  printStatus();

  logIRChangeIfAny(irObstacleDetectedRaw());
  updateUltrasonicMonitor(true);
}

void loop() {
  handleSerial();
  heartbeat();
  updateUltrasonicMonitor(false);

  if (state == SysState::STOPPED_MANUAL) {
    digitalWrite(PIN_STATUS_LED, LOW);
    motorStopHard();
    delay(10);
    return;
  }

  uint32_t now = millis();
  bool obstacleNow = irObstacleDetectedRaw();

  logIRChangeIfAny(obstacleNow);

  if (obstacleNow && state == SysState::IDLE) {
    setState(SysState::CAT_PRESENT);
    Serial.println("IR: obstacle detected -> CAT_PRESENT");
  }

  bool edgeEvent = irEdgeEventDebounced(obstacleNow);
  if (edgeEvent) {
    if (TRIGGER_ON_DETECT) {
      Serial.println("IR EVENT: DETECT");
    } else {
      Serial.println("IR EVENT: LEAVE");
    }
  }

  switch (state) {
    case SysState::IDLE: {
      digitalWrite(PIN_STATUS_LED, LOW);
      break;
    }

    case SysState::CAT_PRESENT: {
      digitalWrite(PIN_STATUS_LED, HIGH);

      if (!TRIGGER_ON_DETECT && edgeEvent) {
        Serial.println("Cat left -> starting leave-confirm delay");
        setState(SysState::WAIT_LEAVE_DELAY);
      }
      break;
    }

    case SysState::WAIT_LEAVE_DELAY: {
      digitalWrite(PIN_STATUS_LED, HIGH);

      if (obstacleNow) {
        Serial.println("Cat returned during delay -> back to CAT_PRESENT");
        setState(SysState::CAT_PRESENT);
        break;
      }

      if (t_last_clean != 0 && (now - t_last_clean) < COOLDOWN_MS) {
        Serial.println("Cooldown active -> skipping cleaning");
        setState(SysState::COOLDOWN);
        break;
      }

      if ((now - t_state_enter) >= LEAVE_CONFIRM_MS) {
        Serial.println("Leave confirmed -> start CLEANING");
        setState(SysState::CLEANING);
      }
      break;
    }

    case SysState::CLEANING: {
      digitalWrite(PIN_STATUS_LED, HIGH);

      if (obstacleNow) {
        Serial.println("Safety: obstacle detected during cleaning -> STOP");
        logMotorAction("Safety stop triggered by IR during cleaning");
        motorStopHard();
        setState(SysState::CAT_PRESENT);
        break;
      }

      logMotorAction("Cleaning cycle begin");
      motorRampForward(DUTY_MAX);

      logMotorAction("Run at DUTY_MAX");
      motorForwardDuty(DUTY_MAX);
      delay(CLEAN_RUN_MS);

      motorRampStopFrom(DUTY_MAX);
      delay(POST_STOP_MS);

      t_last_clean = millis();
      Serial.println("Cleaning complete -> COOLDOWN");
      logMotorAction("Cleaning cycle complete");
      setState(SysState::COOLDOWN);
      break;
    }

    case SysState::COOLDOWN: {
      digitalWrite(PIN_STATUS_LED, LOW);

      if (t_last_clean != 0 && (now - t_last_clean) >= COOLDOWN_MS) {
        Serial.println("Cooldown finished -> IDLE");
        setState(SysState::IDLE);
      }
      break;
    }

    case SysState::STOPPED_MANUAL:
    default:
      break;
  }

  delay(5);
}