#include "sound.h"

#include <math.h>
#include <string.h>

/* See sound.h: every number here is an estimate. */

/* Motor steps per dot line, and how long the motor coasts on after the last
 * line before it is called stopped (a little over one line, so a print's lines
 * run together into one buzz instead of a line of separate bursts). */
#define STEPS_PER_LINE 4
#define MOTOR_TAIL (GBP_LINE_TICKS + GBP_LINE_TICKS / 2)

/* The two resonators each step strikes: a bright plate and a duller body. */
#define PLATE_HZ 1150.0f
#define PLATE_DECAY_S 0.004f
#define BODY_HZ 310.0f
#define BODY_DECAY_S 0.010f
#define STEP_STRIKE 0.55f

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
 * the low body is seventeen times the strike: scaled by sin(w) so each rings at
 * the size it is struck, whatever its pitch. */
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
   const uint64_t step_ticks = GBP_LINE_TICKS / STEPS_PER_LINE;
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
      float v;

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

      if (tick < s->motor_until && tick >= s->next_step)
      {
         strike = STEP_STRIKE;
         s->next_step += step_ticks;
         if (s->next_step <= tick)
            s->next_step = tick + step_ticks;
      }

      v = resonate(s->plate, strike, PLATE_HZ, PLATE_DECAY_S) * 0.6f
         + resonate(s->body, strike, BODY_HZ, BODY_DECAY_S) * 0.4f;
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
