#ifndef GBPRINTER_PRINTER_H
#define GBPRINTER_PRINTER_H

#include <stdint.h>
#include <stddef.h>

/* The Game Boy Printer (MGB-007), as the Game Boy sees it down the cable.
 *
 * Pure: no libretro, no link bus, no clock of its own. Every call that can
 * change what the printer does is handed the tick it happens at, on the link
 * bus's timeline (8388608 per second), so the printer moves on emulated time
 * and nothing else -- which is what lets two netplay peers that feed it the same
 * bytes at the same ticks print the same paper.
 *
 * WHAT A PRINTER IS ON THE WIRE. It never clocks a transfer; the Game Boy does,
 * every byte of it. So the printer's only output is the byte sitting in its
 * shift register when the Game Boy clocks the next one, and that byte is decided
 * by everything received so far. gbp_reply() is that byte. It is ready the
 * moment the previous byte has been taken, which is what a real printer does
 * too and is why it can be published ahead of the clock that reads it.
 *
 * THE PACKET (Pan Docs, "Game Boy Printer"):
 *
 *   88 33 | cmd | comp | lenLo lenHi | data... | sumLo sumHi | 00 | 00
 *
 * The printer answers 0x81 to the first of the two trailing bytes and its
 * status to the second, and 0x00 to everything else. The checksum is the 16-bit
 * sum of everything from cmd to the last data byte.
 *
 * THE PAPER. What the printer burns is kept here as a strip of 160-dot lines,
 * oldest first: the length of paper hanging out of the slot that nobody has torn
 * off yet. Each dot is a grey level, 255 for bare paper. */

#define GBP_WIDTH          160

/* The printer's own RAM: 8 KiB, which Pan Docs puts at 160 x 200 dots between
 * prints. A tile row is twenty 16-byte tiles. */
#define GBP_BUFFER_BYTES   0x2000
#define GBP_TILE_ROW_BYTES (20 * 16)
#define GBP_PACKET_BYTES   0x280

/* Longest strip kept before the oldest lines are let go. A Game Boy Camera
 * photo is 144 lines plus its margins, so this is about six of them -- far
 * more paper than anyone leaves hanging out of a slot, and still a texture a
 * headset can take. */
#define GBP_STRIP_MAX      1024

/* How long one dot line takes to burn and feed, in link ticks.
 *
 * ESTIMATED, not measured: about 35 lines a second, which makes a Game Boy
 * Camera photo with its margins a little over six seconds -- what recordings of
 * the real unit show by eye. Nothing in a game depends on the figure beyond
 * "printing" staying set for a while; they poll status until it clears. */
#define GBP_LINE_TICKS     240000u

/* One unit of margin, in dot lines. ESTIMATED: Pan Docs gives the nibbles but
 * not their unit. Sixteen lines is a tile row's height, and makes the Game Boy
 * Camera's 0x13 (one unit before, three after) a short lead-in and enough tail
 * for the picture to clear the tear bar. */
#define GBP_MARGIN_LINES   16

enum
{
   GBP_CMD_INIT   = 0x01,
   GBP_CMD_PRINT  = 0x02,
   GBP_CMD_DATA   = 0x04,
   GBP_CMD_BREAK  = 0x08,
   GBP_CMD_STATUS = 0x0F
};

enum
{
   GBP_STATUS_CHECKSUM   = 0x01,
   GBP_STATUS_PRINTING   = 0x02,
   GBP_STATUS_FULL       = 0x04,
   GBP_STATUS_UNPRINTED  = 0x08,
   GBP_STATUS_PACKET_ERR = 0x10,
   GBP_STATUS_JAM        = 0x20,
   GBP_STATUS_OTHER      = 0x40,
   GBP_STATUS_BATTERY    = 0x80
};

/* Where the parser stands in a packet: what the NEXT byte will be. */
enum
{
   GBP_AT_MAGIC0 = 0,
   GBP_AT_MAGIC1,
   GBP_AT_CMD,
   GBP_AT_COMP,
   GBP_AT_LEN0,
   GBP_AT_LEN1,
   GBP_AT_DATA,
   GBP_AT_SUM0,
   GBP_AT_SUM1,
   GBP_AT_ACK,
   GBP_AT_STATUS
};

/* Everything here is plain data with no pointers, so a savestate is a copy of
 * the struct -- see gbp_serialize. Field order is the savestate layout: append
 * only, and bump GBP_STATE_VERSION when it changes. */
#define GBP_STATE_VERSION 2

/* A line of paper coming out, for the sound: when, and how many of its dots
 * were burned (none for paper only fed). Kept until the core's audio has
 * played past it. */
#define GBP_EVENTS_MAX 64

typedef struct gbp_line_event
{
   uint64_t tick;
   uint16_t ink;
   uint16_t pad[3];
} gbp_line_event;

typedef struct gbp_printer
{
   /* Packet being received. */
   uint8_t  at;
   uint8_t  command;
   uint8_t  compressed;
   uint8_t  rle_run;        /* 1 while the next byte is a run's value */
   uint16_t length;
   uint16_t remaining;
   uint16_t rle_left;       /* literal bytes still to come in this group */
   uint16_t sum;
   uint16_t sum_received;
   uint16_t staged_len;
   uint8_t  staged[GBP_BUFFER_BYTES];   /* this packet's decompressed data */
   uint8_t  packet_ok;      /* the packet's checksum matched */
   uint8_t  reply;          /* the byte the Game Boy reads on its next clock */
   uint8_t  status;

   /* The printer's RAM, and whether the empty DATA packet that has to come
    * before a PRINT has arrived. */
   uint16_t buffer_len;
   uint8_t  data_closed;
   uint8_t  buffer[GBP_BUFFER_BYTES];

   /* The print in progress. Its image is fixed when it starts, so a game that
    * sends the next picture while this one is still burning cannot change it. */
   uint8_t  job_active;
   uint8_t  job_palette;
   uint8_t  job_exposure;
   uint8_t  job_pad;
   uint16_t job_lines;       /* every line of the job, margins included */
   uint16_t job_done;        /* lines already burned */
   uint16_t job_before;      /* blank lines ahead of the image */
   uint16_t job_image;       /* image lines, all sheets */
   uint16_t job_source;      /* image lines in one sheet */
   uint16_t job_pad2;
   uint64_t job_next_tick;   /* when the next line comes out */
   uint8_t  job_pixels[GBP_BUFFER_BYTES * 4]; /* one 2-bit colour per byte */

   /* Paper being fed by the FEED button, which runs the motor without burning. */
   uint8_t  feeding;
   uint8_t  pad[7];
   uint64_t feed_next_tick;

   /* The strip of paper hanging out of the slot, oldest line first. */
   uint16_t strip_lines;
   uint16_t pad3;
   uint32_t strip_serial;    /* changes whenever the strip does */
   uint32_t tear_count;
   uint32_t pad4;
   uint8_t  strip[GBP_STRIP_MAX][GBP_WIDTH];

   /* Lines out since the sound last took them, oldest first. */
   uint32_t event_count;
   uint32_t pad5;
   gbp_line_event events[GBP_EVENTS_MAX];
} gbp_printer;

/* Hand over the lines that came out at or before `tick`, oldest first; returns
 * how many were copied into `out` (at most `max`) and drops them. */
unsigned gbp_take_events(gbp_printer *p, uint64_t tick, gbp_line_event *out, unsigned max);

void gbp_init(gbp_printer *p);

/* The byte the Game Boy will read when it next clocks the cable. */
static inline uint8_t gbp_reply(const gbp_printer *p) { return p->reply; }

/* The Game Boy clocked `byte` into the printer at `tick`. Afterwards
 * gbp_reply() is the answer to the NEXT byte. */
void gbp_receive(gbp_printer *p, uint8_t byte, uint64_t tick);

/* Run the motor up to `tick`: burn the lines of a print, or feed blank paper
 * while `feed` is held. Lines come out one per GBP_LINE_TICKS. */
void gbp_run(gbp_printer *p, uint64_t tick, int feed);

/* Tear off the oldest `lines` lines of the strip (all of it if more). Returns
 * how many went. */
unsigned gbp_tear(gbp_printer *p, unsigned lines);

/* Savestate. The struct is the state; this is here so a caller never has to
 * know that. */
size_t gbp_serialize_size(void);

#endif
