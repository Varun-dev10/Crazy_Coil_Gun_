/*

  Hardware overview:
    - 3 coil stages, each with own capacitor bank
    - Per stage: 3x 60N60 IGBTs in parallel, gate driven via gate driver IC
    - 12V LiPo -> boost converter -> ~400V cap banks
    - Bleed resistor (200W ~1k ohm) on a separate IGBT for safe discharge
    - Two momentary buttons: CHARGE and TRIGGER

  PINOUT (Arduino Nano)
  --------------------------------------------------------
  D2  - Trigger button        (INPUT_PULLUP, active LOW)
  D3  - Charge button         (INPUT_PULLUP, active LOW)
  D4  - Coil 1 IGBT gate      (HIGH = on, through gate driver)
  D5  - Coil 2 IGBT gate
  D6  - Coil 3 IGBT gate
  D7  - Bleed resistor gate   (HIGH = discharge caps)
  D8  - Boost converter EN    (HIGH = charging)
  D9  - LED: Charging         (yellow)
  D10 - LED: Ready/Armed      (green)
  D11 - LED: Firing           (red)
  D12 - LED: Error/fault      (orange or red)
  A0  - Battery voltage sense (divider: 10k/3.3k)
  A1  - Cap bank 1 voltage    (divider: 890k/10k, see notes)
  A2  - Cap bank 2 voltage
  A3  - Cap bank 3 voltage
  --------------------------------------------------------

  IMPORTANT: Read the hardware notes at the bottom before building!
  Gate drivers, HV dividers, EMI shielding all matter a lot here.
*/


//  PIN DEFS

#define PIN_TRIG        2
#define PIN_CHG_BTN     3
#define PIN_COIL1       4
#define PIN_COIL2       5
#define PIN_COIL3       6
#define PIN_BLEED       7
#define PIN_BOOST       8
#define LED_CHG         9
#define LED_RDY         10
#define LED_FIRE        11
#define LED_ERR         12

#define PIN_BAT         A0
#define PIN_CAP1        A1
#define PIN_CAP2        A2
#define PIN_CAP3        A3


//  TUNABLE PARAMS - tweak these for your build


// --- Coil pulse durations (milliseconds) ---
// Start LOW (3-5ms) and work up. Too long = heat, saturation, suck-back.
// Optimal time is when projectile is at the coil center.
#define COIL1_MS        8     // stage 1 on-time
#define COIL2_MS        7     // stage 2 (slightly less - projectile faster)
#define COIL3_MS        6     // stage 3

// --- Inter-stage delays (ms): time from coil N turning OFF -> coil N+1 ON ---
// Depends on coil spacing and expected projectile velocity between stages.
// Too short = coils fight each other, too long = projectile decelerates.
#define DLY_12          5     // delay between stage 1 and 2
#define DLY_23          4     // delay between stage 2 and 3

// --- Voltage thresholds ---
#define V_TARGET        390.0   // charge to this (volts) then stop
#define V_MIN_FIRE      350.0   // won't allow firing below this
#define V_SAG_REFILL    365.0   // re-charge if caps sag below this while ready
#define V_OVERVOLT      450.0   // emergency dump if any bank hits this

// --- Battery ---
#define V_BAT_LOW       10.5    // warn below this (3S LiPo = 3.5V/cell)

// --- Timers ---
#define CHARGE_TIMEOUT  15000   // (ms) give up charging if this long w/o hitting target
#define BLEED_MS        6000    // (ms) how long to run bleed resistor
                                // at 1000ohm, 400V cap (say 4700uF): E=~375J, P_avg=~62W -> ~6s

// --- Voltage divider calibration ---
// Battery divider: R1=10k R2=3.3k -> ratio = 13.3/3.3 = 4.030
// If your reading is off, calculate: real_v / measured_v and use that
#define BAT_DIV         4.030

// Cap banks: R1=890k R2=10k -> ratio = 900/10 = 90.0
// IMPORTANT: calibrate with a known voltage (multimeter vs serial output)
// and adjust this. A 1% error in resistors = 4.5V error at 450V.
#define CAP_DIV         90.0

// ADC ref (should be 5.0 for Nano with AREF tied to 5V, or 4.95ish in practice)
#define VREF            5.00

// Serial print rate
#define SERIAL_MS       300

// Debounce time for buttons
#define DEBOUNCE        55


//  STATE MACHINE

#define ST_IDLE         0
#define ST_CHARGING     1
#define ST_READY        2
#define ST_FIRING       3
#define ST_BLEEDING     4

int sys_state = ST_IDLE;


//  GLOBALS

float bat_v  = 0;
float cap1_v = 0;
float cap2_v = 0;
float cap3_v = 0;

// timing variables
unsigned long t_charge_start = 0;
unsigned long t_bleed_start  = 0;
unsigned long t_last_serial  = 0;
unsigned long t_step         = 0;   // timestamp for current fire sub-step

// firing sub-step index (1 through 6, 0=done)
int fstep = 0;

// debounce tracking
unsigned long t_trig_db  = 0;
unsigned long t_chg_db   = 0;


//  VOLTAGE READING HELPERS


float readBat() {
  int raw = analogRead(PIN_BAT);
  return (raw * (VREF / 1023.0)) * BAT_DIV;
}

// averages 4 readings to smooth ADC noise (HV dividers can be a bit noisy)
float readCap(int pin) {
  long sum = 0;
  for (int i = 0; i < 4; i++) {
    sum += analogRead(pin);
    delayMicroseconds(150);  // let ADC settle between reads
  }
  return (sum / 4.0) * (VREF / 1023.0) * CAP_DIV;
}

float minCap() {
  return min(cap1_v, min(cap2_v, cap3_v));
}

float maxCap() {
  return max(cap1_v, max(cap2_v, cap3_v));
}


//  LED HELPERS

void ledsOff() {
  digitalWrite(LED_CHG,  LOW);
  digitalWrite(LED_RDY,  LOW);
  digitalWrite(LED_FIRE, LOW);
  digitalWrite(LED_ERR,  LOW);
}


//  COIL / POWER HELPERS

void coilsOff() {
  // Belt and suspenders - make sure all IGBTs are off
  digitalWrite(PIN_COIL1, LOW);
  digitalWrite(PIN_COIL2, LOW);
  digitalWrite(PIN_COIL3, LOW);
}

// Call this to initiate a safe discharge from ANY state
void doBleed(const char* reason) {
  coilsOff();
  digitalWrite(PIN_BOOST, LOW);    // stop charging
  delay(5);                        // tiny gap to let IGBT fully close before switching
  digitalWrite(PIN_BLEED, HIGH);   // bleed on
  t_bleed_start = millis();
  sys_state = ST_BLEEDING;
  ledsOff();
  Serial.print("[BLEED] Starting discharge ");
  Serial.println(reason);
}


//  BUTTON HELPERS (debounced)

bool trigPressed() {
  if (digitalRead(PIN_TRIG) == LOW) {
    if (millis() - t_trig_db > DEBOUNCE) {
      t_trig_db = millis();
      return true;
    }
  }
  return false;
}

bool chgPressed() {
  if (digitalRead(PIN_CHG_BTN) == LOW) {
    if (millis() - t_chg_db > DEBOUNCE) {
      t_chg_db = millis();
      return true;
    }
  }
  return false;
}


//  SETUP

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("STARTING");


  // --- outputs ---
  pinMode(PIN_COIL1, OUTPUT);  digitalWrite(PIN_COIL1, LOW);
  pinMode(PIN_COIL2, OUTPUT);  digitalWrite(PIN_COIL2, LOW);
  pinMode(PIN_COIL3, OUTPUT);  digitalWrite(PIN_COIL3, LOW);
  pinMode(PIN_BLEED, OUTPUT);  digitalWrite(PIN_BLEED, LOW);
  pinMode(PIN_BOOST, OUTPUT);  digitalWrite(PIN_BOOST, LOW);
  pinMode(LED_CHG,   OUTPUT);  digitalWrite(LED_CHG,   LOW);
  pinMode(LED_RDY,   OUTPUT);  digitalWrite(LED_RDY,   LOW);
  pinMode(LED_FIRE,  OUTPUT);  digitalWrite(LED_FIRE,  LOW);
  pinMode(LED_ERR,   OUTPUT);  digitalWrite(LED_ERR,   LOW);

  // --- inputs ---
  pinMode(PIN_TRIG,   INPUT_PULLUP);
  pinMode(PIN_CHG_BTN, INPUT_PULLUP);

  // --- startup safety bleed ---
  // Caps could be partially charged from a previous session.
  // We MUST bleed on boot before doing anything else.
  // This is a blocking delay intentionally - safety takes priority here.
  Serial.println("[BOOT] Running startup safety discharge...");
  Serial.println("[BOOT] Do NOT touch the device during this time.");
  digitalWrite(PIN_BLEED, HIGH);

  unsigned long boot_t = millis();
  while (millis() - boot_t < BLEED_MS) {
    // flash error LED to show we're alive and working
    digitalWrite(LED_ERR, (millis() / 200) % 2);
    // Also check if caps read low early - we could exit early but
    // honestly safer to just wait the full time on boot
    if ((millis() - boot_t) % 1000 < 20) {
      Serial.print("[BOOT] Discharging... ");
      Serial.print((BLEED_MS - (millis() - boot_t)) / 1000);
      Serial.println("s remaining");
    }
    delay(10);
  }

  digitalWrite(PIN_BLEED, LOW);
  ledsOff();
  sys_state = ST_IDLE;

  Serial.println("[BOOT] Startup discharge done.");
  Serial.println("[IDLE] System ready. Press CHARGE to arm.");
  Serial.println("--------------------------------------------");
}


//  LOOP

void loop() {
  // update voltages every loop pass
  bat_v  = readBat();
  cap1_v = readCap(PIN_CAP1);
  cap2_v = readCap(PIN_CAP2);
  cap3_v = readCap(PIN_CAP3);

  // periodic serial status
  if (millis() - t_last_serial > SERIAL_MS) {
    t_last_serial = millis();
    printStatus();
  }

  // main state dispatch
  switch (sys_state) {
    case ST_IDLE:     state_idle();     break;
    case ST_CHARGING: state_charging(); break;
    case ST_READY:    state_ready();    break;
    case ST_FIRING:   state_firing();   break;
    case ST_BLEEDING: state_bleed();    break;
  }
}


//  STATE: IDLE
//  Caps discharged (or just booted), waiting for user input.

void state_idle() {
  ledsOff();

  if (chgPressed()) {
    if (bat_v < V_BAT_LOW && bat_v > 3.0) {
      // 3.0 check = sense wire probably isn't floating/disconnected
      Serial.print("[WARN] Battery voltage low: ");
      Serial.print(bat_v);
      Serial.println("V -- charging anyway but check your pack");
    }
    Serial.println("[CHARGE] Boost converter ON, charging...");
    digitalWrite(PIN_BOOST, HIGH);
    t_charge_start = millis();
    sys_state = ST_CHARGING;
    return;
  }

  if (trigPressed()) {
    Serial.println("[WARN] Trigger pressed but system not charged! Charge first.");
    // flash error LED briefly
    for (int i = 0; i < 4; i++) {
      digitalWrite(LED_ERR, HIGH); delay(80);
      digitalWrite(LED_ERR, LOW);  delay(80);
    }
  }
}


//  STATE: CHARGING
//  Boost converter running, watching cap voltages.

void state_charging() {
  // fast blink on charging LED
  digitalWrite(LED_CHG, (millis() / 150) % 2);

  // overvoltage protection - this is an emergency
  if (maxCap() > V_OVERVOLT) {
    Serial.print("[ERR] Overvoltage detected! Max cap: ");
    Serial.print(maxCap());
    Serial.println("V -- emergency dump!");
    digitalWrite(LED_ERR, HIGH);
    doBleed("OVERVOLTAGE");
    return;
  }

  // charging timeout - something is wrong if we haven't hit target
  if (millis() - t_charge_start > CHARGE_TIMEOUT) {
    Serial.println("[ERR] Charge timeout exceeded. Cap voltage may be stuck.");
    Serial.print("       Current min cap: ");
    Serial.print(minCap());
    Serial.println("V -- discharging for safety.");
    digitalWrite(LED_ERR, HIGH);
    doBleed("CHARGE TIMEOUT");
    return;
  }

  // check if all caps reached target voltage
  // using minCap() ensures ALL banks are charged, not just one
  if (minCap() >= V_TARGET) {
    digitalWrite(PIN_BOOST, LOW);
    Serial.print("[CHARGE] Target voltage reached! All caps >= ");
    Serial.print(V_TARGET);
    Serial.print("V  (");
    Serial.print(minCap());
    Serial.println("V min)");
    Serial.println("[READY] System armed. Pull trigger to fire.");
    sys_state = ST_READY;
    return;
  }

  // let user abort charge by pressing charge button again
  if (chgPressed()) {
    Serial.println("[USER] Charge aborted.");
    doBleed("USER ABORT");
    return;
  }
}


//  STATE: READY
//  Caps charged, waiting for trigger. Manages voltage sag topping.

void state_ready() {
  // solid green ready LED
  digitalWrite(LED_RDY, HIGH);
  digitalWrite(LED_CHG, LOW);

  // caps leak slowly over time - top up if they sag
  if (minCap() < V_SAG_REFILL) {
    Serial.print("[CHARGE] Voltage sagged to ");
    Serial.print(minCap());
    Serial.println("V, topping up...");
    digitalWrite(PIN_BOOST, HIGH);
    t_charge_start = millis();
    sys_state = ST_CHARGING;
    return;
  }

  // FIRE!
  if (trigPressed()) {
    // one last sanity check
    if (minCap() < V_MIN_FIRE) {
      Serial.print("[WARN] Caps too low to fire: ");
      Serial.print(minCap());
      Serial.print("V (need ");
      Serial.print(V_MIN_FIRE);
      Serial.println("V). Wait for charge.");
      return;
    }

    Serial.println("[FIRE] *** FIRING SEQUENCE INITIATED ***");
    Serial.print("[FIRE] Cap voltages: ");
    Serial.print(cap1_v, 0); Serial.print("V / ");
    Serial.print(cap2_v, 0); Serial.print("V / ");
    Serial.print(cap3_v, 0); Serial.println("V");

    // stop charging BEFORE firing - don't want boost on during discharge
    digitalWrite(PIN_BOOST, LOW);
    delay(2);  // tiny gap - let boost gate close completely

    ledsOff();
    digitalWrite(LED_FIRE, HIGH);

    fstep = 1;              // kick off the firing sub-state machine
    t_step = millis();
    sys_state = ST_FIRING;
    return;
  }

  // charge button while ready = manual discharge request
  if (chgPressed()) {
    Serial.println("[USER] Manual discharge requested.");
    ledsOff();
    doBleed("USER REQUEST");
  }
}


//  STATE: FIRING SEQUENCE
//  Non-blocking staged coil fire. Each "step" is a phase:
//
//   step 1: Coil 1 gate HIGH
//   step 2: wait COIL1_MS, then gate LOW
//   step 3: wait DLY_12, then Coil 2 gate HIGH
//   step 4: wait COIL2_MS, then gate LOW
//   step 5: wait DLY_23, then Coil 3 gate HIGH
//   step 6: wait COIL3_MS, then gate LOW, done

void state_firing() {
  unsigned long now = millis();

  switch (fstep) {

    case 1:
      Serial.println("[FIRE] >> Stage 1: Coil 1 ON");
      digitalWrite(PIN_COIL1, HIGH);
      fstep = 2;
      t_step = now;
      break;

    case 2:
      if (now - t_step >= COIL1_MS) {
        digitalWrite(PIN_COIL1, LOW);
        Serial.println("[FIRE]    Stage 1: Coil 1 OFF");
        fstep = 3;
        t_step = now;
      }
      break;

    case 3:
      // inter-stage delay 1->2
      if (now - t_step >= DLY_12) {
        Serial.println("[FIRE] >> Stage 2: Coil 2 ON");
        digitalWrite(PIN_COIL2, HIGH);
        fstep = 4;
        t_step = now;
      }
      break;

    case 4:
      if (now - t_step >= COIL2_MS) {
        digitalWrite(PIN_COIL2, LOW);
        Serial.println("[FIRE]    Stage 2: Coil 2 OFF");
        fstep = 5;
        t_step = now;
      }
      break;

    case 5:
      // inter-stage delay 2->3
      if (now - t_step >= DLY_23) {
        Serial.println("[FIRE] >> Stage 3: Coil 3 ON");
        digitalWrite(PIN_COIL3, HIGH);
        fstep = 6;
        t_step = now;
      }
      break;

    case 6:
      if (now - t_step >= COIL3_MS) {
        digitalWrite(PIN_COIL3, LOW);
        Serial.println("[FIRE]    Stage 3: Coil 3 OFF");
        Serial.println("[FIRE] *** Sequence complete ***");
        Serial.print("[FIRE] Remaining cap voltages: ");
        Serial.print(cap1_v, 0); Serial.print("V / ");
        Serial.print(cap2_v, 0); Serial.print("V / ");
        Serial.print(cap3_v, 0); Serial.println("V");
        fstep = 0;
        ledsOff();
        // after firing, caps are partially discharged. Go idle and let
        // user decide to recharge or discharge (don't auto-recharge)
        sys_state = ST_IDLE;
        Serial.println("[IDLE] Press CHARGE to rearm, or wait for bleed.");
      }
      break;

    default:
      // shouldn't land here, but if we do, make safe
      coilsOff();
      fstep = 0;
      sys_state = ST_IDLE;
      Serial.println("[ERR] Invalid fire step, resetting.");
      break;
  }
}


//  STATE: BLEEDING (discharge)
//  Runs bleed resistor until caps are confirmed safe.

void state_bleed() {
  // medium blink on error LED during discharge
  digitalWrite(LED_ERR, (millis() / 300) % 2);

  if (millis() - t_bleed_start >= BLEED_MS) {
    // check the caps actually went down
    if (minCap() > 50.0) {
      // still too high - extend the bleed
      Serial.print("[BLEED] Timer expired but cap still at ");
      Serial.print(minCap());
      Serial.println("V. Extending bleed time...");
      t_bleed_start = millis();  // reset and keep going
      return;
    }

    // safe!
    digitalWrite(PIN_BLEED, LOW);
    ledsOff();
    sys_state = ST_IDLE;
    Serial.println("[BLEED] Discharge confirmed safe.");
    Serial.print("[BLEED] Final cap voltages: ");
    Serial.print(cap1_v, 0); Serial.print("V / ");
    Serial.print(cap2_v, 0); Serial.print("V / ");
    Serial.print(cap3_v, 0); Serial.println("V");
    Serial.println("[IDLE] System safe. Press CHARGE to arm.");
  }
}


//  SERIAL STATUS PRINTER

void printStatus() {
  const char* names[] = {"IDLE", "CHARGING", "READY", "FIRING", "BLEEDING"};

  Serial.print("[STATUS] ");
  Serial.print(names[sys_state]);
  Serial.print(" | Bat: ");
  Serial.print(bat_v, 2);
  Serial.print("V");
  if (bat_v < V_BAT_LOW && bat_v > 3.0) Serial.print("(!LOW)");
  Serial.print(" | Caps: ");
  Serial.print(cap1_v, 0);
  Serial.print("V / ");
  Serial.print(cap2_v, 0);
  Serial.print("V / ");
  Serial.print(cap3_v, 0);
  Serial.print("V");

  if (sys_state == ST_CHARGING) {
    unsigned long elapsed = (millis() - t_charge_start) / 1000;
    Serial.print(" | ChgTime: ");
    Serial.print(elapsed);
    Serial.print("s / ");
    Serial.print(CHARGE_TIMEOUT / 1000);
    Serial.print("s");
  }
  if (sys_state == ST_FIRING) {
    Serial.print(" | FireStep: ");
    Serial.print(fstep);
  }
  Serial.println();
}