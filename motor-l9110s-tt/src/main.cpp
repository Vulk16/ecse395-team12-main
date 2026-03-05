// ===============================
// TT Motor Controller (ESP32 + L9110S)
// Features:
// - Manual cleaning cycle trigger
// - Safety gating: cat-present blocks motor
// - Emergency stop (latched until released)
// - Soft start/stop via PWM ramp
// - Non-blocking state machine (millis-based)
// - Serial logging for testing
// ===============================

#include <Arduino.h>

// ---------- L9110S Control Pins (Channel A) ----------
static const int PIN_IA1 = 26; // L9110S IA1
static const int PIN_IA2 = 27; // L9110S IA2

// ---------- Inputs (active-low with internal pullups) ----------
static const int PIN_MANUAL_BTN  = 33; // to GND when pressed
static const int PIN_ESTOP_BTN   = 32; // to GND when pressed
static const int PIN_CAT_PRESENT = 23; // to GND when cat is present (switch closed)

// ---------- PWM (LEDC) ----------
static const int PWM_CH_IA1 = 0;
static const int PWM_CH_IA2 = 1;
static const int PWM_FREQ   = 20000; // 20 kHz reduces audible whine
static const int PWM_RES    = 8;     // 0..255

// ---------- Cleaning Cycle Parameters ----------
static const uint8_t PWM_TARGET_FWD = 210;  // 0..255 (tune for torque/noise)
static const uint8_t PWM_TARGET_REV = 180;  // reverse speed
static const uint32_t RAMP_MS       = 1200; // soft start/stop duration
static const uint32_t RUN_FWD_MS    = 5000; // forward run time
static const uint32_t PAUSE_MS      = 800;  // pause between directions
static const uint32_t RUN_REV_MS    = 2000; // reverse run time
static const uint32_t COOLDOWN_MS   = 4000; // lockout after cycle

// ---------- Debounce ----------
static const uint32_t DEBOUNCE_MS = 35;
static uint32_t tLastPress = 0;
static bool manualPrev = HIGH;

// ---------- State Machine ----------
enum class State {
  IDLE,
  RAMP_UP_FWD,
  RUN_FWD,
  PAUSE,
  RAMP_UP_REV,
  RUN_REV,
  RAMP_DOWN,
  COOLDOWN,
  ESTOP_LATCHED
};

static State state = State::IDLE;
static uint32_t tEnter = 0;

// Track current direction and duty for ramp-down
enum class Dir { STOP, FWD, REV };
static Dir currentDir = Dir::STOP;
static uint8_t currentDuty = 0;

// ---------- Input helpers (active-low) ----------
static bool catPresent()   { return digitalRead(PIN_CAT_PRESENT) == LOW; }
static bool estopPressed() { return digitalRead(PIN_ESTOP_BTN) == LOW; }

// ---------- Logging ----------
static void logLine(const char* msg) {
  Serial.print("[LOG] ");
  Serial.print(msg);
  Serial.print(" | cat=");
  Serial.print(catPresent() ? "YES" : "NO");
  Serial.print(" estop=");
  Serial.print(estopPressed() ? "PRESSED" : "OK");
  Serial.print(" dir=");
  if (currentDir == Dir::FWD) Serial.print("FWD");
  else if (currentDir == Dir::REV) Serial.print("REV");
  else Serial.print("STOP");
  Serial.print(" duty=");
  Serial.println(currentDuty);
}

static void enterState(State s, const char* msg) {
  state = s;
  tEnter = millis();
  logLine(msg);
}

// ---------- Safety gate ----------
static bool safeToRun() {
  if (estopPressed()) return false;
  if (catPresent())   return false;
  return true;
}

// ---------- PWM write for L9110S ----------
// L9110S direction control:
// - Forward: IA1 = PWM, IA2 = LOW
// - Reverse: IA1 = LOW, IA2 = PWM
// - Stop:    IA1 = LOW, IA2 = LOW
static void motorStop() {
  ledcWrite(PWM_CH_IA1, 0);
  ledcWrite(PWM_CH_IA2, 0);
  digitalWrite(PIN_IA1, LOW);
  digitalWrite(PIN_IA2, LOW);
  currentDir = Dir::STOP;
  currentDuty = 0;
}

static void motorForward(uint8_t duty) {
  // Ensure reverse channel is off
  ledcWrite(PWM_CH_IA2, 0);
  digitalWrite(PIN_IA2, LOW);

  // Apply PWM on IA1
  ledcWrite(PWM_CH_IA1, duty);
  currentDir = Dir::FWD;
  currentDuty = duty;
}

static void motorReverse(uint8_t duty) {
  // Ensure forward channel is off
  ledcWrite(PWM_CH_IA1, 0);
  digitalWrite(PIN_IA1, LOW);

  // Apply PWM on IA2
  ledcWrite(PWM_CH_IA2, duty);
  currentDir = Dir::REV;
  currentDuty = duty;
}

// ---------- Ramp helper ----------
static uint8_t ramp(uint8_t startDuty, uint8_t endDuty, uint32_t elapsed, uint32_t total) {
  if (total == 0) return endDuty;
  if (elapsed >= total) return endDuty;
  float a = (float)elapsed / (float)total;
  float d = (1.0f - a) * startDuty + a * endDuty;
  if (d < 0) d = 0;
  if (d > 255) d = 255;
  return (uint8_t)(d + 0.5f);
}

// ---------- Manual press edge detect ----------
static bool manualPressEdge() {
  bool now = digitalRead(PIN_MANUAL_BTN); // HIGH idle, LOW pressed
  bool edge = false;

  if (manualPrev == HIGH && now == LOW) {
    uint32_t t = millis();
    if (t - tLastPress > DEBOUNCE_MS) {
      edge = true;
      tLastPress = t;
    }
  }
  manualPrev = now;
  return edge;
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_IA1, OUTPUT);
  pinMode(PIN_IA2, OUTPUT);

  pinMode(PIN_MANUAL_BTN, INPUT_PULLUP);
  pinMode(PIN_ESTOP_BTN,  INPUT_PULLUP);
  pinMode(PIN_CAT_PRESENT, INPUT_PULLUP);

  // Setup PWM channels for IA1 and IA2
  ledcSetup(PWM_CH_IA1, PWM_FREQ, PWM_RES);
  ledcSetup(PWM_CH_IA2, PWM_FREQ, PWM_RES);
  ledcAttachPin(PIN_IA1, PWM_CH_IA1);
  ledcAttachPin(PIN_IA2, PWM_CH_IA2);

  motorStop();
  enterState(State::IDLE, "BOOT -> IDLE");
}

void loop() {
  // ---------- E-stop always wins ----------
  if (estopPressed() && state != State::ESTOP_LATCHED) {
    motorStop();
    enterState(State::ESTOP_LATCHED, "E-STOP LATCHED (motor stopped)");
  }

  if (state == State::ESTOP_LATCHED) {
    // Stay latched until E-stop released
    if (!estopPressed()) {
      enterState(State::IDLE, "E-STOP released -> IDLE");
    }
    return;
  }

  // ---------- Cat enters while running: stop immediately ----------
  if (catPresent()) {
    if (state != State::IDLE && state != State::COOLDOWN) {
      motorStop();
      enterState(State::COOLDOWN, "Cat detected -> immediate stop -> COOLDOWN");
    }
  }

  // ---------- Manual trigger ----------
  if (manualPressEdge()) {
    if (state == State::IDLE && safeToRun()) {
      enterState(State::RAMP_UP_FWD, "Manual trigger -> RAMP_UP_FWD");
    } else {
      if (state != State::IDLE) Serial.println("[INFO] Manual trigger ignored: not IDLE");
      if (!safeToRun())        Serial.println("[INFO] Manual trigger blocked: cat present or E-stop");
    }
  }

  // ---------- State machine ----------
  uint32_t now = millis();
  uint32_t dt  = now - tEnter;

  switch (state) {
    case State::IDLE: {
      motorStop();
      break;
    }

    case State::RAMP_UP_FWD: {
      if (!safeToRun()) {
        motorStop();
        enterState(State::COOLDOWN, "Unsafe during ramp -> COOLDOWN");
        break;
      }
      uint8_t duty = ramp(0, PWM_TARGET_FWD, dt, RAMP_MS);
      motorForward(duty);
      if (dt >= RAMP_MS) enterState(State::RUN_FWD, "RAMP_UP_FWD -> RUN_FWD");
      break;
    }

    case State::RUN_FWD: {
      if (!safeToRun()) {
        motorStop();
        enterState(State::COOLDOWN, "Unsafe during RUN_FWD -> COOLDOWN");
        break;
      }
      motorForward(PWM_TARGET_FWD);
      if (dt >= RUN_FWD_MS) {
        motorStop();
        enterState(State::PAUSE, "RUN_FWD done -> PAUSE");
      }
      break;
    }

    case State::PAUSE: {
      motorStop();
      if (dt >= PAUSE_MS) {
        if (safeToRun()) enterState(State::RAMP_UP_REV, "PAUSE -> RAMP_UP_REV");
        else enterState(State::COOLDOWN, "Unsafe after pause -> COOLDOWN");
      }
      break;
    }

    case State::RAMP_UP_REV: {
      if (!safeToRun()) {
        motorStop();
        enterState(State::COOLDOWN, "Unsafe during REV ramp -> COOLDOWN");
        break;
      }
      uint8_t duty = ramp(0, PWM_TARGET_REV, dt, RAMP_MS);
      motorReverse(duty);
      if (dt >= RAMP_MS) enterState(State::RUN_REV, "RAMP_UP_REV -> RUN_REV");
      break;
    }

    case State::RUN_REV: {
      if (!safeToRun()) {
        motorStop();
        enterState(State::COOLDOWN, "Unsafe during RUN_REV -> COOLDOWN");
        break;
      }
      motorReverse(PWM_TARGET_REV);
      if (dt >= RUN_REV_MS) enterState(State::RAMP_DOWN, "RUN_REV done -> RAMP_DOWN");
      break;
    }

    case State::RAMP_DOWN: {
      // Ramp down smoothly in the current direction
      uint8_t duty = ramp(currentDuty, 0, dt, RAMP_MS);
      if (currentDir == Dir::REV) motorReverse(duty);
      else motorForward(duty);

      if (dt >= RAMP_MS) {
        motorStop();
        enterState(State::COOLDOWN, "RAMP_DOWN -> COOLDOWN");
      }
      break;
    }

    case State::COOLDOWN: {
      motorStop();
      if (dt >= COOLDOWN_MS) enterState(State::IDLE, "COOLDOWN done -> IDLE");
      break;
    }

    default:
      motorStop();
      enterState(State::IDLE, "Unknown state -> IDLE");
      break;
  }
}