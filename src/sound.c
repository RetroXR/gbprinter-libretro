#include "sound.h"

#include <math.h>
#include <string.h>

/* See sound.h: fitted to a recording of a real printer. MEASURED marks a number
 * read off it; the gains are shaped by ear and by its spectrum. */

/* How long the motor coasts on after the last line before it is called
 * stopped (a little over one line, so a print's lines run together into one
 * sound instead of a line of separate bursts). */
#define MOTOR_TAIL (GBP_LINE_TICKS + GBP_LINE_TICKS / 2)

/* MEASURED: the stepper's rate, the fundamental every tone is a harmonic of,
 * and the steps in one turn of the gear (1.14 s), which knocks once a turn. */
#define STEP_HZ 127.7f
#define STEP_TICKS ((uint64_t)((float)GBP_SOUND_RATE * GBP_TICKS_PER_SAMPLE / STEP_HZ + 0.5f))
#define STEPS_PER_TURN 146

/* MEASURED: the motor through one turn of the gear, in TURN_PARTS parts from
 * the knock, averaged over twelve turns of the recording: how loud its tones
 * are (dB) and how fast it steps (Hz). A little after a third of the way round
 * its tones fall away by 8 dB for a quarter of a second and come back: the
 * winding heard turn after turn, and why a motor held at one level sounded
 * flat. The dip is deepened by TURN_DEPTH, the averaging having blurred one
 * that runs 10-15 dB in any single turn. */
#define TURN_PARTS 40
static const float TURN_LEVEL_DB[TURN_PARTS] = {
   0.2f, 0.6f, 0.8f, 0.1f, 0.1f, -0.8f, -2.6f, -2.2f, -0.6f, -0.2f,
   -1.4f, -4.2f, -8.3f, -9.3f, -8.2f, -7.1f, -7.1f, -7.9f, -7.9f, -8.2f,
   -8.0f, -7.0f, -4.2f, -1.8f, -0.1f, 0.9f, 1.2f, 1.0f, 0.8f, 0.8f,
   1.1f, 1.5f, 1.4f, 1.3f, 1.4f, 1.5f, 1.5f, 1.3f, 1.7f, 0.5f
};
static const float TURN_STEP_HZ[TURN_PARTS] = {
   127.99f, 127.79f, 127.68f, 127.30f, 127.16f, 127.56f, 128.01f, 127.95f,
   127.41f, 127.29f, 127.31f, 126.96f, 127.12f, 127.63f, 127.82f, 127.71f,
   127.31f, 127.24f, 127.64f, 127.69f, 127.63f, 127.97f, 128.38f, 128.20f,
   127.75f, 127.66f, 127.77f, 127.89f, 128.09f, 128.14f, 128.07f, 127.95f,
   127.90f, 127.77f, 127.65f, 127.70f, 127.68f, 127.63f, 127.72f, 127.88f
};
#define TURN_DEPTH 1.25f

/* MEASURED: which harmonics of the step ring loudest (9, 10, 18, 20, 24, 36,
 * 40), and roughly how loud against each other. Each rings longer than a step,
 * so the steps add up into those harmonics' lines rather than a rattle. */
static const float MODE_HARMONIC[GBP_SOUND_MODES] = { 9, 10, 18, 20, 24, 36, 40 };
static const float MODE_GAIN[GBP_SOUND_MODES] = { 0.30f, 1.00f, 0.50f, 0.50f, 0.25f, 0.70f, 0.40f };
#define MODE_DECAY_S 0.060f
#define STEP_STRIKE 0.0075f

/* The rattle under the tones, 1-6 kHz (MEASURED: much of the recording's energy
 * is broadband there, and it swells and falls with each step, about 1.8 times;
 * its top falls away past 6 kHz). Two low-pass stages, and a smooth rise and
 * fall over each step down to HISS_FLOOR of its peak: one gentle stage up to
 * 12 kHz, steady, was a bright even wash that sounded like white noise, and a
 * sharp hit each step that decayed was a spike the recording does not have. */
#define HISS_HP_HZ 1000.0f
#define HISS_LP_HZ 6000.0f
#define HISS_FLOOR 0.72f
#define HISS_GAIN 0.26f
/* The step itself, a sharp tick through the hiss's band: MEASURED, the
 * recording carries a faint line at every harmonic of the step, not only the
 * ones the case rings. */
#define TICK_GAIN 0.4f

/* The knock once a turn: MEASURED a 3 ms ring near 5 kHz, ten times the motor. */
#define KNOCK_HZ 5000.0f
#define KNOCK_DECAY_S 0.0012f
#define KNOCK_STRIKE 1.8f
/* MEASURED, too: the knock is heard right across the spectrum, so with the
 * ring comes a burst of noise as short. */
#define KNOCK_HISS_DECAY_S 0.0015f
#define KNOCK_HISS_GAIN 0.18f

/* The swell in the turn: MEASURED 3-4 kHz, from about 0.3 to 0.6 of
 * the turn, MEASURED where the motor's tones dip (TURN_LEVEL_DB). Noise rung
 * through a broad band there, faded in and out. */
#define SWELL_HZ 3300.0f
#define SWELL_DECAY_S 0.004f
#define SWELL_FROM 0.30f
#define SWELL_TO 0.60f
#define SWELL_GAIN 0.03f

/* The head's tick: noise, a couple of milliseconds long, as loud as the line is
 * inked. */
#define CLICK_DECAY_S 0.0018f
#define CLICK_GAIN 0.35f

#define OUTPUT_GAIN 14000.0f

#define TWO_PI 6.28318530718f

void gbp_sound_init(gbp_sound *s)
{
   memset(s, 0, sizeof(*s));
   s->rng = 0x6B5A1C3Du;
}

static float noise(gbp_sound *s)
{
   /* xorshift32: deterministic, and carried in the savestate. */
   uint32_t x = s->rng;
   x ^= x << 13;
   x ^= x >> 17;
   x ^= x << 5;
   s->rng = x;
   return ((float)(x & 0xFFFF) / 32768.0f) - 1.0f;
}

/* One sample of a two-pole resonator at `hz` decaying over `decay_s`.
 *
 * Struck with an impulse of x, such a filter rings at about x / sin(w), which for
 * a low one is many times the strike: scaled by sin(w) so each rings at the size
 * it is struck, whatever its pitch. */
static float resonate(float *y, float x, float hz, float decay_s)
{
   const float w = TWO_PI * hz / (float)GBP_SOUND_RATE;
   const float r = expf(-1.0f / (decay_s * (float)GBP_SOUND_RATE));
   const float c = 2.0f * r * cosf(w);
   float out = c * y[0] - r * r * y[1] + x * sinf(w);
   y[1] = y[0];
   y[0] = out;
   return out;
}

/* `table` (TURN_PARTS values round a turn) at `turn`, 0 to 1 from the knock. */
static float turn_at(const float *table, float turn)
{
   float at = turn * (float)TURN_PARTS;
   int i = (int)at;
   float f = at - (float)i;
   i %= TURN_PARTS;
   return table[i] + (table[(i + 1) % TURN_PARTS] - table[i]) * f;
}

unsigned gbp_sound_frame(gbp_sound *s, gbp_printer *p, uint64_t end_tick, int16_t *out)
{
   gbp_line_event ev[GBP_EVENTS_MAX];
   unsigned n_ev = gbp_take_events(p, end_tick, ev, GBP_EVENTS_MAX);
   unsigned e = 0;
   unsigned frames = 0;
   const float knock_hiss_decay = expf(-1.0f / (KNOCK_HISS_DECAY_S * (float)GBP_SOUND_RATE));
   const float hp_a = 1.0f - expf(-TWO_PI * HISS_HP_HZ / (float)GBP_SOUND_RATE);
   const float lp_a = 1.0f - expf(-TWO_PI * HISS_LP_HZ / (float)GBP_SOUND_RATE);
   const float click_decay = expf(-1.0f / (CLICK_DECAY_S * (float)GBP_SOUND_RATE));
   const uint64_t behind = (uint64_t)1100 * GBP_TICKS_PER_SAMPLE;

   /* Never more than two frames behind the clock. The core moves both a frame
    * at a time and this never happens there; a caller that jumped the clock
    * would otherwise leave the sound trailing for ever, a frame's worth at a
    * time, and every line it was meant to sound long gone. */
   if (end_tick > behind && s->sample * GBP_TICKS_PER_SAMPLE < end_tick - behind)
      s->sample = (end_tick - behind / 2) / GBP_TICKS_PER_SAMPLE;

   while ((s->sample + 1) * GBP_TICKS_PER_SAMPLE <= end_tick && frames < 550)
   {
      uint64_t tick = s->sample * GBP_TICKS_PER_SAMPLE;
      float strike = 0.0f;
      float knock = 0.0f;
      float v;
      int running;
      unsigned m;
      float turn;
      uint64_t step_ticks;

      /* Lines that came out by now: each keeps the motor going and, if it
       * burned anything, ticks the head. */
      while (e < n_ev && ev[e].tick <= tick)
      {
         if (s->motor_until <= tick)
            s->next_step = tick;   /* the motor starts on this line */
         s->motor_until = ev[e].tick + MOTOR_TAIL;
         if (ev[e].ink)
         {
            s->click = 1.0f;
            s->click_gain = CLICK_GAIN * sqrtf((float)ev[e].ink / (float)GBP_WIDTH);
         }
         e++;
      }

      running = tick < s->motor_until;
      /* Where the gear is in its turn: the steps it has made, and how far into
       * the next. Stopped, it stays where it stopped -- worked out from a step
       * long past it ran on, and the rings, retuned every sample, never died
       * away to silence. */
      turn = (float)(s->steps % STEPS_PER_TURN) / (float)STEPS_PER_TURN;
      if (running && tick + STEP_TICKS >= s->next_step)
         turn += (float)(tick + STEP_TICKS - s->next_step) / (float)STEP_TICKS / (float)STEPS_PER_TURN;
      if (turn >= 1.0f)
         turn -= 1.0f;
      if (running && tick >= s->next_step)
      {
         strike = STEP_STRIKE;
         s->steps++;
         if (s->steps % STEPS_PER_TURN == 0)
         {
            knock = KNOCK_STRIKE;
            s->knock_hiss = 1.0f;
         }
         /* Each step as long as the motor steps at this point of the turn. */
         step_ticks = (uint64_t)((float)GBP_SOUND_RATE * GBP_TICKS_PER_SAMPLE
            / turn_at(TURN_STEP_HZ, turn) + 0.5f);
         s->next_step += step_ticks;
         if (s->next_step <= tick)
            s->next_step = tick + step_ticks;
      }

      v = 0.0f;
      {
         /* The tones, as loud as the motor is at this point of its turn, each
          * tuned to its harmonic of the rate the motor steps at now: tuned to the
          * mean rate, a harmonic slid off its ring and back as the rate wandered,
          * and swung 15 dB the recording never does. */
         float tones = 0.0f;
         const float rate_now = turn_at(TURN_STEP_HZ, turn);
         for (m = 0; m < GBP_SOUND_MODES; m++)
            tones += resonate(s->mode[m], strike, rate_now * MODE_HARMONIC[m], MODE_DECAY_S) * MODE_GAIN[m];
         v += tones * powf(10.0f, TURN_DEPTH * turn_at(TURN_LEVEL_DB, turn) / 20.0f);
      }
      v += resonate(s->knock, knock, KNOCK_HZ, KNOCK_DECAY_S);
      if (s->knock_hiss > 0.001f)
      {
         v += noise(s) * s->knock_hiss * KNOCK_HISS_GAIN;
         s->knock_hiss *= knock_hiss_decay;
      }
      else
         s->knock_hiss = 0.0f;
      {
         /* Noise only while the motor turns: the hiss through its band, and the
          * swell's through its own, faded by where the gear is in its turn. */
         float n = running ? noise(s) : 0.0f;
         float swell = 0.0f;
         if (running && turn > SWELL_FROM && turn < SWELL_TO)
         {
            float k = sinf((turn - SWELL_FROM) / (SWELL_TO - SWELL_FROM) * 3.14159265f);
            swell = n * k * k * SWELL_GAIN;
         }
         float in = n + strike * TICK_GAIN;
         s->hiss_hp += hp_a * (in - s->hiss_hp);
         s->hiss_lp += lp_a * ((in - s->hiss_hp) - s->hiss_lp);
         s->hiss_lp2 += lp_a * (s->hiss_lp - s->hiss_lp2);
         if (running)
         {
            /* How far through this step, 0 to 1: loudest as it lands. */
            float into = (float)(tick + STEP_TICKS - s->next_step) / (float)STEP_TICKS;
            float pulse = 0.5f + 0.5f * cosf(TWO_PI * into);
            v += s->hiss_lp2 * HISS_GAIN * (HISS_FLOOR + (1.0f - HISS_FLOOR) * pulse);
         }
         v += resonate(s->swell, swell, SWELL_HZ, SWELL_DECAY_S);
      }
      if (s->click > 0.001f)
      {
         v += noise(s) * s->click * s->click_gain;
         s->click *= click_decay;
      }
      else
         s->click = 0.0f;

      {
         float f = v * OUTPUT_GAIN;
         int16_t smp;
         if (f > 32767.0f)
            f = 32767.0f;
         else if (f < -32768.0f)
            f = -32768.0f;
         smp = (int16_t)f;
         out[frames * 2] = smp;
         out[frames * 2 + 1] = smp;
      }
      frames++;
      s->sample++;
   }

   /* Anything stamped past this frame is the next frame's. */
   if (e < n_ev)
   {
      unsigned k;
      for (k = n_ev; k > e; k--)
      {
         /* Put them back at the front of the printer's queue, oldest first. */
         if (p->event_count < GBP_EVENTS_MAX)
         {
            memmove(&p->events[1], &p->events[0], sizeof(p->events[0]) * p->event_count);
            p->events[0] = ev[k - 1];
            p->event_count++;
         }
      }
   }
   return frames;
}
