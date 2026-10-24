#ifndef GBPRINTER_SOUND_H
#define GBPRINTER_SOUND_H

#include <stdint.h>

#include "printer.h"

/* What the printer sounds like while paper is coming out.
 *
 * SYNTHESISED, and fitted to a recording of a real MGB-007 printing (a
 * fifteen-second print found online, 44.1 kHz): what is measured from it is
 * marked so below, the rest is shaped by ear. Three sources:
 *
 *   The paper motor. A stepper, heard stepping 127.7 times a second (every
 *   tone in the recording is a harmonic of that), each step ringing the case's
 *   resonances -- the 9th, 10th, 18th, 20th, 24th, 36th and 40th harmonics
 *   stand out, around 1.2, 2.4, 3.1 and 4.6-5.1 kHz -- over a hiss across
 *   1-7 kHz that pulses with the steps. Its own clock, not the lines': it runs
 *   while paper moves, burned or only fed.
 *
 *   The gear's turn. Every 146 steps (1.14 s) something in the gear train
 *   knocks, a 3 ms ring near 5 kHz about ten times the motor's level, and
 *   half way round the turn a swell of 3-4 kHz rises and falls: the winding
 *   that comes round and round while a print comes out.
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
#define GBP_SOUND_MODES 7

typedef struct gbp_sound
{
   uint64_t sample;          /* samples made since load */
   uint64_t motor_until;     /* tick the motor stops at, unless a line comes */
   uint64_t next_step;       /* tick of the next motor step */
   uint32_t rng;
   uint32_t steps;           /* motor steps since load: where the gear is in its turn */
   float mode[GBP_SOUND_MODES][2]; /* the case's resonances, two samples of history each */
   float swell[2];           /* the band the mid-turn swell rings */
   float knock[2];           /* the once-a-turn knock */
   float knock_hiss;         /* its burst of noise, decaying */
   float hiss_hp;            /* the hiss's band filter: high-pass state */
   float hiss_lp;            /* and low-pass state */
   float hiss_lp2;           /* and its second low-pass stage */
   float click;              /* the head's tick, decaying */
   float click_gain;
} gbp_sound;

void gbp_sound_init(gbp_sound *s);

/* Make the samples for one Game Boy frame ending at `end_tick`, interleaved
 * stereo into `out`, from the lines `p` put out. Returns how many frames of
 * stereo were written; `out` must hold 550 of them. */
unsigned gbp_sound_frame(gbp_sound *s, gbp_printer *p, uint64_t end_tick, int16_t *out);

#endif
