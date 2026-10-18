/*
 * Clarion CRX87RM C-Bus AUX emulator - release - Christine Jordan
 * Arduino Nano or Uno
 *
 * Emulates a C-Bus CD changer.
 *
 *   D2 - SCK (DIN 2)   D3 - SOI (DIN 1)   D4 - SRQ (DIN 4)   D5 - AC (DIN 11)
 *   Audio: 3.5mm jack -> DIN 7/8/6 (L/R/AGND).
 */

#define SCK_BIT _BV(2)
#define SOI_BIT _BV(3)
#define SRQ_BIT _BV(4)
#define AC_BIT  _BV(5)

#define SCK_IS_HIGH  (PIND & SCK_BIT)
#define SOI_IS_HIGH  (PIND & SOI_BIT)
#define AC_IS_LOW    (!(PIND & AC_BIT))  // low = head is powering the changer

// Open-collector: OUTPUT drives low (PORTD bit cleared), INPUT releases.
#define SOI_DRIVE_LOW  (DDRD |=  SOI_BIT)
#define SOI_RELEASE    (DDRD &= ~SOI_BIT)
#define SRQ_ASSERT     (DDRD |=  SRQ_BIT)
#define SRQ_RELEASE    (DDRD &= ~SRQ_BIT)

// Bus timing (measured on the CRX87RM): SCK 3.6µs period, data changes
// after each falling edge and is read on the rising edge. Bytes are clocked
// by polling with interrupts off. This transport is tested... change with care... will break shit
#define HANDSHAKE_HI      64     // ack: SOI high this many clocks...
#define HANDSHAKE_LO       3     // ...then low this many

#define T1_US             16     // Timer1 at clk/256 (millis() stops with IRQs off)
#define MS_TO_T1(ms)      ((uint16_t)((ms) * 1000UL / T1_US))
#define SCK_IDLE_T1       2      // steady-high SCK = gap between bursts
#define SEND_TIMEOUT_MS   20

static inline uint16_t t1() { return TCNT1; }

// Millisecond clock from Timer1; must be called at least once per second.
static uint32_t bus_ms() {
  static uint16_t last = 0;
  static uint32_t us   = 0;
  const uint16_t now = t1();
  us  += (uint32_t)(uint16_t)(now - last) * T1_US;
  last = now;
  return us / 1000;
}

// ---- Low-level helpers ----

// Tight edge wait; gives up after ~80µs.
static inline bool wait_sck(bool high) {
  uint8_t n = 0;
  if (high) { while (!(PIND & SCK_BIT)) if (--n == 0) return false; }
  else      { while (  PIND & SCK_BIT ) if (--n == 0) return false; }
  return true;
}

static inline bool wait_falling_edge() {
  return wait_sck(true) && wait_sck(false);
}

// First falling edge of a burst, with a long deadline checked every 256 polls.
static inline bool wait_first_fall(uint16_t t0, uint16_t limit) {
  uint8_t n = 0;
  while (PIND & SCK_BIT)
    if (++n == 0 && (uint16_t)(t1() - t0) >= limit) return false;
  return true;
}

// Wait for a gap between bursts. Returns ticks left, or 0 on timeout.
static uint16_t wait_idle(uint16_t t0, uint16_t limit) {
  uint16_t idle_since = t1();
  for (;;) {
    const uint16_t now = t1(), used = now - t0;
    if (used >= limit) return 0;
    if (!SCK_IS_HIGH) idle_since = now;
    else if ((uint16_t)(now - idle_since) >= SCK_IDLE_T1) return limit - used;
  }
}

// Post-byte acknowledge. SOI released on entry and exit.
static bool handshake() {
  bool ok = true;
  for (uint8_t i = 0; i < HANDSHAKE_HI && ok; i++) ok = wait_falling_edge();
  if (ok) {
    SOI_DRIVE_LOW;
    for (uint8_t i = 0; i < HANDSHAKE_LO && ok; i++) ok = wait_falling_edge();
  }
  SOI_RELEASE;
  return ok;
}

// ---- Byte transport ----

static void cbus_init() {
  PORTD &= ~(SOI_BIT | SRQ_BIT);
  SOI_RELEASE;
  SRQ_RELEASE;
  DDRD  &= ~(SCK_BIT | AC_BIT);
  PORTD |=  AC_BIT;                // pull-up on AC

  TCCR1A = 0;                      // Timer1 free-running, clk/256
  TCCR1B = _BV(CS12);
  TIMSK1 = 0;
}

// Send one byte. SOI stays released until the head starts clocking.
static bool cbus_send(uint8_t b) {
  noInterrupts();
  uint16_t first = wait_idle(t1(), MS_TO_T1(SEND_TIMEOUT_MS));
  const uint16_t t0 = t1();
  bool ok = first != 0 && wait_first_fall(t0, first);
  // Shift a copy and test bit 7 — a variable shift is too slow here.
  uint8_t d = b;
  for (uint8_t i = 0; i < 8 && ok; i++) {
    if (i) {
      ok = wait_sck(false);
      if (!ok) break;
    }
    if (d & 0x80) SOI_RELEASE;
    else          SOI_DRIVE_LOW;
    d <<= 1;
    ok = wait_sck(true);
  }
  if (ok) ok = wait_sck(false);    // hold bit 0 until the next falling edge
  SOI_RELEASE;
  ok = ok && handshake();
  interrupts();
  return ok;
}

// Byte followed by its complement.
static bool cbus_send_pair(uint8_t b) {
  return cbus_send(b) && cbus_send(~b);
}

// Receive one byte (bits read on rising edges); false on timeout.
static bool cbus_recv(uint8_t *out, uint16_t timeout_ms) {
  noInterrupts();
  uint16_t first = wait_idle(t1(), MS_TO_T1(timeout_ms));
  const uint16_t t0 = t1();
  bool ok = first != 0;
  uint8_t b = 0;
  for (uint8_t i = 0; i < 8 && ok; i++) {
    ok = (i == 0) ? wait_first_fall(t0, first) : wait_sck(false);
    ok = ok && wait_sck(true);
    b = (b << 1) | (SOI_IS_HIGH ? 1 : 0);
  }
  ok = ok && handshake();
  interrupts();
  *out = b;
  return ok;
}

// Echo + length + data (length and data sent with complements).
static bool send_response(uint8_t echo, const uint8_t *data, uint8_t len) {
  if (!cbus_send(echo) || !cbus_send_pair(len)) return false;
  for (uint8_t i = 0; i < len; i++)
    if (!cbus_send_pair(data[i])) return false;
  return true;
}

// ---- Changer state ----

enum CbusState { WAIT_POWER, IDLE, PLAY, RUNNING };

static CbusState state       = WAIT_POWER;
static uint32_t  last_srq_ms = 0;
static uint16_t  play_secs   = 0;

// Next play-sequence message. Kept across resets - the head may keep
// polling 11 for the rest of the sequence after resetting the bus.
#define SEQ_NONE 0xFF
static uint8_t seq_phase = SEQ_NONE;
static bool    playing   = false;

static uint8_t chg_status     = 0x00;    // 00 ready, 05 starting, 01 playing
static uint8_t disc           = 1;
static bool    play_requested = false;   // set when the head loads a disc

// ---- Command replies (from a CDC634 capture, srwalter/cbus-emulator and the C-Bus notes) ----

struct CmdReply { uint8_t cmd, len, data[4]; };

static const CmdReply REPLIES[] = {
  { 0x01, 0, {} },
  { 0x41, 4, { 0x1A, 0x55, 0x55, 0x54 } },  // magazine: disc 1 loaded, 2-6 not loaded
  { 0x4C, 2, { 0x00, 0x00 } },
  { 0x3E, 3, { 0x00, 0x01, 0x07 } },        // read TOC
  { 0x5B, 3, { 0x00, 0x01, 0x01 } },        // play
  { 0x5C, 3, { 0x00, 0x01, 0x06 } },
  { 0x70, 0, {} },
  { 0x81, 0, {} },                          // select track 1
};

// Status queries — always 00 01 1A.
static const uint8_t STATUS_CMDS[] = { 0x09, 0x45, 0x50, 0x51, 0x56, 0x59 };

static uint8_t bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

// Elapsed time: 10 mm ss (BCD).
static void time_msg(uint8_t *m) {
  m[0] = 0x10;
  m[1] = bcd((play_secs / 60) % 100);
  m[2] = bcd(play_secs % 60);
}

// Play sequence sent in answer to 11 polls after a disc load.
// Returns the message length, or 0 when complete.
static uint8_t play_msg(uint8_t phase, uint8_t *m) {
  const uint8_t d3 = 0x30 | disc, d4 = 0x40 | disc;
  uint8_t len = 3;
  switch (phase) {
    case 0:         m[0] = 0x00; m[1] = 0x01; m[2] = 0x04; break;
    case 1: case 3: m[0] = d4;   m[1] = 0x12; m[2] = 0x01; break;
    case 2: case 4: m[0] = d3;   m[1] = 0x43; m[2] = 0x31; break;
    case 5:         m[0] = d3;   m[1] = 0x01; len = 2;     break;
    case 6:         m[0] = 0x00; m[1] = 0x01; m[2] = 0x08; break;  // audio on
    case 7:         m[0] = 0x00; m[1] = 0x01; m[2] = 0x01; break;  // playing
    case 8:         m[0] = 0x10; m[1] = 0x00; m[2] = 0x00; break;  // time 0:00
    default: return 0;
  }
  return len;
}

static void start_running() {
  seq_phase   = SEQ_NONE;
  playing     = true;
  chg_status  = 0x01;
  state       = RUNNING;
  last_srq_ms = bus_ms();
  play_secs   = 0;
}

// Answer to an 11 poll: next play message, else the time, else status.
static uint8_t next_message(uint8_t *m) {
  if (seq_phase != SEQ_NONE) {
    const uint8_t len = play_msg(seq_phase++, m);
    uint8_t peek[3];
    if (play_msg(seq_phase, peek) == 0) start_running();
    return len;
  }
  if (playing) {
    if (state != RUNNING) { state = RUNNING; last_srq_ms = bus_ms(); }
    time_msg(m);
    return 3;
  }
  m[0] = 0x00; m[1] = 0x01; m[2] = chg_status;
  return 3;
}

// Reply to one command. Returns true for a bus reset (F7/FB) or garbage (FF).
static bool dispatch(uint8_t cmd) {
  const uint8_t hi = cmd & 0xF0, lo = cmd & 0x0F;
  switch (cmd) {
    case 0xFF:
      return true;             // out of sync - don't answer
    case 0xF7:
    case 0xFB:
      cbus_send(cmd);          // reset: echo only
      return true;
    case 0x00:
      cbus_send(0xF7);         // service poll
      return false;
    case 0x11: {               // message poll
      uint8_t m[3];
      send_response(0x11, m, next_message(m));
      return false;
    }
  }
  if (hi == 0x60 && lo >= 1 && lo <= 6) {        // start disc x
    disc = lo;
    send_response(cmd, nullptr, 0);
    return false;
  }
  if (hi == 0xE0 && lo >= 1 && lo <= 6) {        // load disc x
    disc = lo;
    const uint8_t r[] = { (uint8_t)(0x40 | lo), 0xFF, 0xFF };
    send_response(cmd, r, 3);
    play_requested = true;
    seq_phase      = 0;
    playing        = false;
    chg_status     = 0x05;
    return false;
  }
  for (uint8_t c : STATUS_CMDS)
    if (c == cmd) {
      const uint8_t r[] = { 0x00, 0x01, 0x1A };
      send_response(cmd, r, 3);
      return false;
    }
  for (const CmdReply &r : REPLIES)
    if (r.cmd == cmd) { send_response(cmd, r.data, r.len); return false; }

  const uint8_t disc_ready[] = { 0x00, 0x01, 0x00 };   // unknown command
  send_response(cmd, disc_ready, 3);
  return false;
}

// Request service: SRQ low, then the head's 00 / F7 exchange.
static bool do_srq_handshake() {
  SRQ_ASSERT;
  uint8_t b;
  if (!cbus_recv(&b, 100) || b != 0x00) { SRQ_RELEASE; return false; }
  cbus_send(0xF7);
  if (!cbus_recv(&b, 100) || b != 0xF7) { SRQ_RELEASE; return false; }
  SRQ_RELEASE;      // SRQ rises before the echo
  cbus_send(0xF7);
  return true;
}

// Stop requesting service after a reset; play-sequence progress is kept.
static void reset_to_idle() {
  SRQ_RELEASE;
  state          = IDLE;
  play_requested = false;
}

// ---- Main ----

void setup() {
  cbus_init();
}

void loop() {
  uint8_t cmd;
  const uint32_t now_ms = bus_ms();

  // Head switched off (AC high for 200ms): start over. The Nano itself
  // stays powered from unswitched 12V.
  static uint32_t ac_high_since = 0;
  if (state != WAIT_POWER && !AC_IS_LOW) {
    if (!ac_high_since) ac_high_since = now_ms ? now_ms : 1;
    else if (now_ms - ac_high_since >= 200) {
      SRQ_RELEASE;
      SOI_RELEASE;
      state = WAIT_POWER;
      seq_phase = SEQ_NONE; playing = false; play_requested = false;
      chg_status = 0x00;
      ac_high_since = 0;
    }
  } else {
    ac_high_since = 0;
  }

  switch (state) {

    case WAIT_POWER:            // ignore the bus until the head powers us
      if (AC_IS_LOW) state = IDLE;
      break;

    case IDLE:                  // answer setup/select commands until a disc load
      if (cbus_recv(&cmd, 50) && dispatch(cmd)) play_requested = false;
      if (play_requested) {
        play_requested = false;
        state = PLAY;
      }
      break;

    case PLAY: {                // send the play sequence via SRQ
      if (cbus_recv(&cmd, 10)) {
        if (dispatch(cmd) && cmd != 0xF7) reset_to_idle();   // F7 is part of SRQ handling
        break;
      }
      if (!do_srq_handshake()) break;
      if (!cbus_recv(&cmd, 100)) break;
      if (cmd != 0x11) { dispatch(cmd); break; }

      uint8_t m[3];
      send_response(0x11, m, next_message(m));
      break;
    }

    case RUNNING:               // playing: time update every second
      if (cbus_recv(&cmd, 5)) {
        if (dispatch(cmd) && cmd != 0xF7) reset_to_idle();
        break;
      }
      if (now_ms - last_srq_ms >= 1000) {
        last_srq_ms = now_ms;
        play_secs++;
        if (do_srq_handshake() && cbus_recv(&cmd, 100) && cmd == 0x11) {
          uint8_t m[3];
          send_response(0x11, m, next_message(m));
        }
      }
      break;
  }
}
