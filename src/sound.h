#ifndef GBPRINTER_SOUND_H
#define GBPRINTER_SOUND_H

#include <stdint.h>

#include "printer.h"

/* What the printer sounds like while paper is coming out.
 *
 * SYNTHESISED, and from the mechanism rather than a recording: nothing published
 * describes the MGB-007's sound, and there was no unit to record. Two sources:
 *
 *   The paper motor. A small stepper advancing the paper a few steps a dot line,
 *   so while it runs there is a buzz at the step rate -- four steps a line at
 *   ~35 lines a second is ~140 Hz -- each step ringing the mechanism like a small
 *   struck plate. It runs whenever paper moves, burned or only fed.
 *
 *   The head. Burning a line fires the heating elements of every dot it inks,
 *   which ticks; a dark line louder than a pale one, and a blank one not at all.
 *
 * Every figure below is an estimate meant to be replaced the day a real unit is
 * recorded (the hardware-sfx skill is the way in): the core's job is to make
 * sound at the moments the paper moves, which it does exactly.
 *
 * Time is the link bus's, as everything in this core is, so the sound is the
 * same on every netplay peer and a savestate carries it. 32768 samples a second
 * is exactly 256 link ticks a sample, and a Game Boy frame exactly 548.625
 * samples. */

#define GBP_SOUND_RATE 32768
#define GBP_TICKS_PER_SAMPLE 256

typedef struct gbp_sound
{
   uint64_t sample;          /* samples made since load */
   uint64_t motor_until;     /* tick the motor stops at, unless a line comes */
   uint64_t next_step;       /* tick of the next motor step */
   uint32_t rng;
   uint32_t pad;
   float plate[2];           /* the ringing resonator, two samples of history */
   float body[2];            /* the low one */
   float click;              /* the head's tick, decaying */
   float click_gain;
} gbp_sound;

void gbp_sound_init(gbp_sound *s);

/* Make the samples for one Game Boy frame ending at `end_tick`, interleaved
 * stereo into `out`, from the lines `p` put out. Returns how many frames of
 * stereo were written; `out` must hold 550 of them. */
unsigned gbp_sound_frame(gbp_sound *s, gbp_printer *p, uint64_t end_tick, int16_t *out);

#endif
