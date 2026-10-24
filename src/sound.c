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

/* MEASURED: which harmonics of the step ring loudest (9, 10, 18, 20, 24, 36,
 * 40), and roughly how loud against each other. Each rings longer than a step,
 * so the steps add up into those harmonics' lines rather than a rattle. */
static const float MODE_HARMONIC[GBP_SOUND_MODES] = { 9, 10, 18, 20, 24, 36, 40 };
static const float MODE_GAIN[GBP_SOUND_MODES] = { 0.30f, 1.00f, 0.50f, 0.50f, 0.25f, 0.70f, 0.40f };
#define MODE_DECAY_S 0.060f
#define STEP_STRIKE 0.012f

/* The hiss under the tones, 1-7 kHz (MEASURED: most of the recording's energy
 * is broadband there), half steady and half a pulse with each step. */
#define HISS_HP_HZ 1000.0f
#define HISS_LP_HZ 12000.0f
#define HISS_FLOOR 0.85f
#define HISS_DECAY_S 0.004f
#define HISS_GAIN 0.20f
/* The step itself, a sharp tick through the hiss's band: MEASURED, the
 * recording carries a faint line at every harmonic of the step, not only the
 * ones the case rings. */
#define TICK_GAIN 3.0f

/* The knock once a turn: MEASURED a 3 ms ring near 5 kHz, ten times the motor. */
#define KNOCK_HZ 5000.0f
#define KNOCK_DECAY_S 0.0012f
#define KNOCK_STRIKE 2.5f
/* MEASURED, too: the knock is heard right across the spectrum, so with the
 * ring comes a burst of noise as short. */
#define KNOCK_HISS_DECAY_S 0.0015f
#define KNOCK_HISS_GAIN 0.25f

/* The swell half way round a turn: MEASURED 3-4 kHz, from about 0.4 to 0.75 of
 * the turn. Noise rung through a broad band there, faded in and out. */
#define SWELL_HZ 3300.0f
#define SWELL_DECAY_S 0.004f
#define SWELL_FROM 0.40f
#define SWELL_TO 0.75f
#define SWELL_GAIN 0.03f

/* The head's tick: noise, a couple of milliseconds long, as loud as the line is
 * inked. */
#define CLICK_DECAY_S 0.0018f
#define CLICK_GAIN 0.35f

#define OUTPUT_GAIN 9000.0f

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

unsigned gbp_sound_frame(gbp_sound *s, gbp_printer *p, uint64_t end_tick, int16_t *out)
{
   gbp_line_event ev[GBP_EVENTS_MAX];
   unsigned n_ev = gbp_take_events(p, end_tick, ev, GBP_EVENTS_MAX);
   unsigned e = 0;
   unsigned frames = 0;
   const float hiss_decay = expf(-1.0f / (HISS_DECAY_S * (float)GBP_SOUND_RATE));
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
      if (running && tick >= s->next_step)
      {
         strike = STEP_STRIKE;
         s->steps++;
         s->hiss = 1.0f;
         if (s->steps % STEPS_PER_TURN == 0)
         {
            knock = KNOCK_STRIKE;
            s->knock_hiss = 1.0f;
         }
         s->next_step += STEP_TICKS;
         if (s->next_step <= tick)
            s->next_step = tick + STEP_TICKS;
      }

      v = 0.0f;
      for (m = 0; m < GBP_SOUND_MODES; m++)
         v += resonate(s->mode[m], strike, STEP_HZ * MODE_HARMONIC[m], MODE_DECAY_S) * MODE_GAIN[m];
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
         float turn = ((float)(s->steps % STEPS_PER_TURN)
            + (float)(tick + STEP_TICKS - s->next_step) / (float)STEP_TICKS) / (float)STEPS_PER_TURN;
         float swell = 0.0f;
         if (running && turn > SWELL_FROM && turn < SWELL_TO)
         {
            float k = sinf((turn - SWELL_FROM) / (SWELL_TO - SWELL_FROM) * 3.14159265f);
            swell = n * k * k * SWELL_GAIN;
         }
         float in = n + strike * TICK_GAIN;
         s->hiss_hp += hp_a * (in - s->hiss_hp);
         s->hiss_lp += lp_a * ((in - s->hiss_hp) - s->hiss_lp);
         v += s->hiss_lp * HISS_GAIN * (running ? HISS_FLOOR + (1.0f - HISS_FLOOR) * s->hiss : 0.0f);
         s->hiss *= hiss_decay;
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
