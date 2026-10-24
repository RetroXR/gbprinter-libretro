#ifndef GBPRINTER_SOUND_H
#define GBPRINTER_SOUND_H

#include <stdint.h>

#include "printer.h"

/* What the printer sounds like while paper is coming out.
 *
 * SYNTHESISED, and fitted to a recording of a real MGB-007 printing (a
 * fifteen-second print found online, 44.1 kHz, measured twice over, the second
 * time independently): what is measured from it is marked so in sound.c, the
 * rest is shaped by ear. Three sources:
 *
 *   The paper motor. A stepper whose harmonics -- the 9th, 10th, 18th, 20th,
 *   24th, 36th, 39th and 40th stand out, around 1.2, 2.4, 3.1 and 4.6-5.1 kHz --
 *   ring over a rattle across 1-7 kHz. It runs while paper moves, burned or only
 *   fed, on its own clock rather than the lines'.
 *
 *   The gear's turn, once every 1.14 s: a knock (a ring near 5 kHz with noise
 *   across the spectrum), the step rate holding near 127.8 Hz for a third of the
 *   turn, then winding down to 119.5 Hz and snapping back up past 129 -- the
 *   pitch falling a semitone and more and recovering, turn after turn, while the
 *   tones give way to rattle and the whole stays about as loud.
 *
 *   The head. Burning a line fires the heating elements of every dot it inks,
 *   which ticks; a dark line louder than a pale one, and a blank one not at all.
 *   Not separable in the recording; kept from the first synthesis.
 *
 * Time is the link bus's, as everything in this core is, so the sound is the
 * same on every netplay peer and a savestate carries it. 32768 samples a second
 * is exactly 256 link ticks a sample, and a Game Boy frame exactly 548.625
 * samples. */

#define GBP_SOUND_RATE 32768
#define GBP_TICKS_PER_SAMPLE 256

/* The case's resonances a motor step rings (sound.c). */
#define GBP_SOUND_MODES 8

typedef struct gbp_sound
{
   uint64_t sample;          /* samples made since load */
   uint64_t motor_until;     /* tick the motor stops at, unless a line comes */
   uint64_t next_step;       /* tick of the next motor step */
   uint32_t rng;
   float turn;               /* where the gear is in its turn, 0 to 1 from the knock */
   float turn_s;             /* how long this turn takes, seconds */
   uint32_t turns;           /* turns since the motor started */
   float mode[GBP_SOUND_MODES][2]; /* the motor's harmonics, two samples of history each */
   float swell[2][2];        /* the two bands the mid-turn swell rings */
   float knock[2];           /* the once-a-turn knock */
   float knock_hiss;         /* its burst of noise, decaying */
   float hiss_hp;            /* the rattle's band filter: high-pass state */
   float hiss_lp[4];         /* and its four low-pass stages */
   float click;              /* the head's tick, decaying */
   float click_gain;
} gbp_sound;

void gbp_sound_init(gbp_sound *s);

/* Make the samples for one Game Boy frame ending at `end_tick`, interleaved
 * stereo into `out`, from the lines `p` put out. Returns how many frames of
 * stereo were written; `out` must hold 550 of them. */
unsigned gbp_sound_frame(gbp_sound *s, gbp_printer *p, uint64_t end_tick, int16_t *out);

#endif
