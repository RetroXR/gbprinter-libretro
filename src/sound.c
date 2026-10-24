#include "sound.h"

#include <math.h>
#include <string.h>

/* See sound.h: fitted to a recording of a real printer. MEASURED marks a number
 * read off it (the turn's tables from a second, independent analysis that
 * tracked the harmonics as they moved); the gains are shaped by ear and by its
 * spectrum. */

/* How long the motor coasts on after the last line before it is called
 * stopped (a little over one line, so a print's lines run together into one
 * sound instead of a line of separate bursts). */
#define MOTOR_TAIL (GBP_LINE_TICKS + GBP_LINE_TICKS / 2)

/* MEASURED: one turn of the gear takes 1.1407 s, knock to knock, give or take
 * 7 ms from one turn to the next. Its own clock, not a count of steps: the step
 * rate changes through the turn.
 *
 * The give or take is a fixed pattern (TURN_OFFSET_S, standard deviation 7 ms)
 * rather than noise, and the gear comes round to the knock whenever the motor
 * starts: so the room, which cannot hear the core, can count the turns as they
 * go and jolt the paper as each knocks (GbPrinter, which keeps these numbers
 * too). Change one and change the other. */
#define TURN_S 1.1407f
#define TURN_OFFSETS 8
static const float TURN_OFFSET_S[TURN_OFFSETS] = {
   0.004f, -0.009f, 0.002f, 0.011f, -0.005f, -0.001f, 0.008f, -0.010f
};

/* MEASURED, in TURN_PARTS parts of the turn from the knock (median turns of the
 * recording, in time):
 *
 *   TURN_STEP_HZ, the step rate every harmonic is a multiple of: near 127.8 Hz
 *   for a third of the turn, winding down to 119.4 Hz by half way and snapping
 *   back up past 129. The winding heard turn after turn is this, the pitch
 *   falling a semitone and more and recovering. (The first fit tracked each
 *   harmonic in a window too narrow to follow it down, read the harmonics
 *   leaving the window as the motor going quiet, and missed the fall.)
 *
 *   TURN_TONE_DB and TURN_RATTLE_DB, how loud the harmonics and the rattle
 *   between them are against their medians: as the pitch falls the tones give
 *   way to rattle and the whole stays about as loud. The tones' dip is taken at
 *   TURN_TONE_DEPTH of its measure, which a fast-moving pitch exaggerates. */
#define TURN_PARTS 40
static const float TURN_STEP_HZ[TURN_PARTS] = {
   127.84f, 127.92f, 127.89f, 127.69f, 127.41f, 126.98f, 127.37f, 127.78f,
   128.30f, 127.35f, 127.22f, 127.24f, 127.13f, 125.60f, 123.68f, 122.79f,
   121.95f, 121.14f, 120.49f, 119.79f, 119.44f, 120.45f, 127.88f, 129.19f,
   128.83f, 127.71f, 127.64f, 127.73f, 127.90f, 128.12f, 128.27f, 128.05f,
   127.90f, 127.82f, 127.69f, 127.64f, 127.76f, 127.69f, 127.64f, 127.71f
};
static const float TURN_TONE_DB[TURN_PARTS] = {
   0.0f, 0.4f, 0.4f, 0.4f, 0.3f, 0.0f, 0.3f, -1.4f, -2.6f, -1.7f,
   -0.6f, -0.9f, -2.5f, -5.2f, -4.4f, -3.5f, -3.6f, -4.1f, -4.3f, -5.3f,
   -5.5f, -8.2f, -9.2f, -2.2f, -1.0f, -0.6f, 0.4f, 1.1f, 1.1f, 1.1f,
   1.2f, 1.4f, 1.6f, 1.7f, 1.4f, 1.5f, 1.3f, 1.4f, 1.4f, 0.8f
};
/* (The first and last parts, which hold the knock, are taken from their
 * neighbors: the knock is its own sound.) */
static const float TURN_RATTLE_DB[TURN_PARTS] = {
   -1.0f, -0.9f, -1.8f, -2.0f, -1.2f, -0.6f, -1.2f, -0.2f, 1.6f, 1.4f,
   0.8f, 0.4f, 1.9f, 3.8f, 2.5f, 1.2f, 0.6f, 0.9f, 1.9f, 2.2f,
   1.6f, 3.1f, 3.1f, 0.6f, 1.9f, 0.2f, -1.4f, -1.5f, -1.4f, -1.7f,
   -2.1f, -2.0f, -1.9f, -1.7f, -1.6f, -1.6f, -1.8f, -1.7f, -1.7f, -1.7f
};
#define TURN_TONE_DEPTH 0.7f

/* MEASURED: which harmonics of the step ring loudest, and how loud against the
 * 10th, with three weaker ones that give the real one its texture (the 21st,
 * 29th and 41st, 7-11 dB short in the fit without them). Each rings longer
 * than a step, so the steps add up into those
 * harmonics' lines rather than a rattle, and each is tuned to its harmonic of
 * the rate now, so the lines follow the pitch as it winds down and back. */
static const float MODE_HARMONIC[GBP_SOUND_MODES] = { 9, 10, 18, 20, 24, 36, 39, 40, 21, 29, 41 };
static const float MODE_GAIN[GBP_SOUND_MODES] = {
   0.30f, 1.00f, 0.57f, 0.64f, 0.36f, 0.97f, 0.65f, 0.56f, 0.37f, 0.34f, 0.41f
};
#define MODE_DECAY_S 0.030f
/* How fast the rings die once the motor stops, seconds. */
#define STOP_DECAY_S 0.015f
#define STEP_STRIKE 0.015f

/* The rattle under the tones, 1-7 kHz: MEASURED much of the recording's energy
 * is broadband there, uneven, and falling away above 7 kHz. Four low-pass stages,
 * since two left 8-12 kHz 6 dB too bright; a gentle rise and fall with each
 * step, down to HISS_FLOOR (MEASURED weak, and not locked from one turn to the
 * next); louder as the tones give way (TURN_RATTLE_DB). */
#define HISS_HP_HZ 1000.0f
#define HISS_LP_HZ 6500.0f
#define HISS_FLOOR 0.8f
#define HISS_GAIN 0.26f
/* The step itself, a sharp tick through the rattle's band: the recording carries
 * a faint line at every harmonic of the step, not only the ones that ring. */
#define TICK_GAIN 0.4f

/* The knock once a turn: MEASURED about 3.6 ms of ring near 5 kHz, a third of
 * its energy elsewhere, some 24 dB over the motor just before it. Three rings,
 * not one: a single ring at 5 kHz put nine tenths of it there. */
static const float KNOCK_HZ[3] = { 5000.0f, 3300.0f, 7200.0f };
static const float KNOCK_MIX[3] = { 1.0f, 0.42f, 0.34f };
#define KNOCK_DECAY_S 0.0022f
#define KNOCK_STRIKE 0.9f
#define KNOCK_HISS_DECAY_S 0.0025f
#define KNOCK_HISS_GAIN 0.22f

/* The swell as the pitch winds down: MEASURED the rattle between the lines
 * rising some 6 dB in 3-4 kHz and 3 dB in 4-6 kHz, over about 0.3 to 0.6 of the
 * turn, and broad (half a kilohertz and more). Noise rung through two broad
 * bands there, faded in and out. */
static const float SWELL_HZ[2] = { 3400.0f, 4800.0f };
static const float SWELL_MIX[2] = { 1.0f, 0.55f };
#define SWELL_DECAY_S 0.0006f
#define SWELL_FROM 0.30f
#define SWELL_TO 0.60f
/* 0.09 pumped the motor 4.5 dB louder through the swell where the recording
 * stays about as loud (rendered and measured, with the rattle's own rise). */
#define SWELL_GAIN 0.03f

/* The head's tick: noise, a couple of milliseconds long, as loud as the line is
 * inked. */
#define CLICK_DECAY_S 0.0018f
/* Unverified in the recording, and much of what was left too bright above
 * 8 kHz: kept, quieter. */
#define CLICK_GAIN 0.2f

#define OUTPUT_GAIN 14000.0f

#define TWO_PI 6.28318530718f

void gbp_sound_init(gbp_sound *s)
{
   memset(s, 0, sizeof(*s));
   s->rng = 0x6B5A1C3Du;
   s->turn_s = TURN_S + TURN_OFFSET_S[0];
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
   const float stop_damp = expf(-1.0f / (STOP_DECAY_S * (float)GBP_SOUND_RATE));
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
      float rate_now;
      float step_ticks;
      int running;
      unsigned m;

      /* Lines that came out by now: each keeps the motor going and, if it
       * burned anything, ticks the head. */
      while (e < n_ev && ev[e].tick <= tick)
      {
         if (s->motor_until <= tick)
         {
            /* The motor starts on this line, the gear at its knock. */
            s->next_step = tick;
            s->turn = 0.0f;
            s->turns = 0;
            s->turn_s = TURN_S + TURN_OFFSET_S[0];
         }
         s->motor_until = ev[e].tick + MOTOR_TAIL;
         if (ev[e].ink)
         {
            s->click = 1.0f;
            s->click_gain = CLICK_GAIN * sqrtf((float)ev[e].ink / (float)GBP_WIDTH);
         }
         e++;
      }

      running = tick < s->motor_until;
      /* The gear turns while the motor runs, and knocks as it comes round; each
       * turn a little longer or shorter than the last. Stopped, it stays where it
       * stopped, until the motor starts again at the knock. */
      if (running)
      {
         if (s->turn_s <= 0.0f)
            s->turn_s = TURN_S;
         s->turn += 1.0f / (s->turn_s * (float)GBP_SOUND_RATE);
         if (s->turn >= 1.0f)
         {
            s->turn -= 1.0f;
            s->turns++;
            s->turn_s = TURN_S + TURN_OFFSET_S[s->turns % TURN_OFFSETS];
            knock = KNOCK_STRIKE;
            s->knock_hiss = 1.0f;
         }
      }
      rate_now = turn_at(TURN_STEP_HZ, s->turn);
      step_ticks = (float)GBP_SOUND_RATE * GBP_TICKS_PER_SAMPLE / rate_now;
      if (!running)
      {
         /* Stopped, the motor's rings die out quickly: left to their own 60 ms
          * the louder harmonics were still faintly sounding half a second after
          * the paper stopped. */
         for (m = 0; m < GBP_SOUND_MODES; m++)
         {
            s->mode[m][0] *= stop_damp;
            s->mode[m][1] *= stop_damp;
         }
      }
      if (running && tick >= s->next_step)
      {
         strike = STEP_STRIKE;
         s->next_step += (uint64_t)(step_ticks + 0.5f);
         if (s->next_step <= tick)
            s->next_step = tick + (uint64_t)(step_ticks + 0.5f);
      }

      v = 0.0f;
      {
         /* The tones, each tuned to its harmonic of the rate now, as loud as
          * they are at this point of the turn. */
         float tones = 0.0f;
         for (m = 0; m < GBP_SOUND_MODES; m++)
            tones += resonate(s->mode[m], strike, rate_now * MODE_HARMONIC[m], MODE_DECAY_S) * MODE_GAIN[m];
         v += tones * powf(10.0f, TURN_TONE_DEPTH * turn_at(TURN_TONE_DB, s->turn) / 20.0f);
      }
      for (m = 0; m < 3; m++)
         v += resonate(s->knock[m], knock * KNOCK_MIX[m], KNOCK_HZ[m], KNOCK_DECAY_S);
      if (s->knock_hiss > 0.001f)
      {
         v += noise(s) * s->knock_hiss * KNOCK_HISS_GAIN;
         s->knock_hiss *= knock_hiss_decay;
      }
      else
         s->knock_hiss = 0.0f;
      {
         /* Noise only while the motor turns: the rattle through its band, and
          * the swell's through its own, faded by where the gear is in its turn. */
         float n = running ? noise(s) : 0.0f;
         float swell = 0.0f;
         float in;
         if (running && s->turn > SWELL_FROM && s->turn < SWELL_TO)
         {
            float k = sinf((s->turn - SWELL_FROM) / (SWELL_TO - SWELL_FROM) * 3.14159265f);
            swell = n * k * k * SWELL_GAIN;
         }
         in = n + strike * TICK_GAIN;
         s->hiss_hp += hp_a * (in - s->hiss_hp);
         s->hiss_lp[0] += lp_a * ((in - s->hiss_hp) - s->hiss_lp[0]);
         for (m = 1; m < 4; m++)
            s->hiss_lp[m] += lp_a * (s->hiss_lp[m - 1] - s->hiss_lp[m]);
         if (running)
         {
            /* How far through this step, 0 to 1: loudest as it lands. */
            float into = 1.0f - (float)(s->next_step - tick) / step_ticks;
            float pulse = 0.5f + 0.5f * cosf(TWO_PI * into);
            v += s->hiss_lp[3] * HISS_GAIN * (HISS_FLOOR + (1.0f - HISS_FLOOR) * pulse)
               * powf(10.0f, turn_at(TURN_RATTLE_DB, s->turn) / 20.0f);
         }
         for (m = 0; m < 2; m++)
            v += resonate(s->swell[m], swell * SWELL_MIX[m], SWELL_HZ[m], SWELL_DECAY_S);
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
