#include "printer.h"

#include <string.h>

void gbp_init(gbp_printer *p)
{
   memset(p, 0, sizeof(*p));
   p->at = GBP_AT_MAGIC0;
   p->reply = 0x00;
}

size_t gbp_serialize_size(void)
{
   return sizeof(gbp_printer);
}

/* ------------------------------------------------------------------------ */
/* The strip                                                                 */
/* ------------------------------------------------------------------------ */

static void note_line(gbp_printer *p, const uint8_t *line, uint64_t tick)
{
   gbp_line_event *e;
   unsigned x, ink = 0;
   for (x = 0; x < GBP_WIDTH; x++)
      if (line[x] < 0xFF)
         ink++;
   if (p->event_count >= GBP_EVENTS_MAX)
   {
      /* Nobody is listening (a frontend that never asks for sound): keep the
       * newest, which is what anyone starting to listen would want. */
      memmove(&p->events[0], &p->events[1], sizeof(p->events[0]) * (GBP_EVENTS_MAX - 1));
      p->event_count = GBP_EVENTS_MAX - 1;
   }
   e = &p->events[p->event_count++];
   memset(e, 0, sizeof(*e));
   e->tick = tick;
   e->ink = (uint16_t)ink;
}

unsigned gbp_take_events(gbp_printer *p, uint64_t tick, gbp_line_event *out, unsigned max)
{
   unsigned n = 0;
   while (n < p->event_count && n < max && p->events[n].tick <= tick)
   {
      out[n] = p->events[n];
      n++;
   }
   if (n)
   {
      memmove(&p->events[0], &p->events[n], sizeof(p->events[0]) * (p->event_count - n));
      p->event_count -= n;
   }
   return n;
}

static void strip_push(gbp_printer *p, const uint8_t *line, uint64_t tick)
{
   note_line(p, line, tick);
   if (p->strip_lines >= GBP_STRIP_MAX)
   {
      /* The oldest paper falls off the end. Nobody tore it, and a strip this
       * long is lying on the table anyway; what matters is that the newest line,
       * the one at the print head, is never the one lost. */
      memmove(p->strip[0], p->strip[1], (size_t)(GBP_STRIP_MAX - 1) * GBP_WIDTH);
      p->strip_lines = GBP_STRIP_MAX - 1;
   }
   memcpy(p->strip[p->strip_lines], line, GBP_WIDTH);
   p->strip_lines++;
   p->strip_serial++;
}

static void strip_push_blank(gbp_printer *p, uint64_t tick)
{
   uint8_t line[GBP_WIDTH];
   memset(line, 0xFF, sizeof(line));
   strip_push(p, line, tick);
}

unsigned gbp_tear(gbp_printer *p, unsigned lines)
{
   if (lines > p->strip_lines)
      lines = p->strip_lines;
   if (lines == 0)
      return 0;

   /* The OLDEST lines go: they are the ones furthest out of the slot, and the
    * room tears what it could see. Anything burned since it looked is still
    * nearer the head and stays on the roll. */
   memmove(p->strip[0], p->strip[lines], (size_t)(p->strip_lines - lines) * GBP_WIDTH);
   p->strip_lines = (uint16_t)(p->strip_lines - lines);
   p->strip_serial++;
   p->tear_count++;
   return lines;
}

/* ------------------------------------------------------------------------ */
/* Printing                                                                  */
/* ------------------------------------------------------------------------ */

/* Grey for a palette shade (0 lightest .. 3 darkest), with the exposure byte
 * applied. Exposure sets how long the head burns: Nintendo's manual gives 0x00
 * as -25% and 0x7F as +25%, with the Game Boy Camera's 0x40 the middle. */
static uint8_t shade_grey(unsigned shade, unsigned exposure)
{
   static const unsigned base[4] = { 255, 170, 85, 0 };
   unsigned ink = 255 - base[shade & 3];
   unsigned e = exposure & 0x7F;

   /* ink * (0.75 + 0.5 * e / 127), in integers. */
   ink = ink * (381 + 2 * e) / 508;
   if (ink > 255)
      ink = 255;
   return (uint8_t)(255 - ink);
}

/* Unpack the printer's RAM, which holds tiles in the order they were sent --
 * twenty to a row, 16 bytes each, two bitplanes interleaved a line at a time --
 * into one colour per dot. Only whole tile rows count. */
static uint16_t unpack_tiles(gbp_printer *p)
{
   unsigned rows = p->buffer_len / GBP_TILE_ROW_BYTES;
   unsigned lines = rows * 8;
   unsigned b;

   for (b = 0; b < rows * GBP_TILE_ROW_BYTES; b += 2)
   {
      unsigned tile = b / 16;
      unsigned y = (tile / 20) * 8 + (b % 16) / 2;
      unsigned x0 = (tile % 20) * 8;
      uint8_t lo = p->buffer[b];
      uint8_t hi = p->buffer[b + 1];
      unsigned x;

      for (x = 0; x < 8; x++)
      {
         unsigned bit = 7 - x;
         p->job_pixels[y * GBP_WIDTH + x0 + x] =
            (uint8_t)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
      }
   }
   return (uint16_t)lines;
}

static void start_print(gbp_printer *p, uint64_t tick)
{
   unsigned sheets = p->staged[0];
   unsigned before = (p->staged[1] >> 4) & 0x0F;
   unsigned after = p->staged[1] & 0x0F;
   unsigned source;

   p->job_palette = p->staged[2];
   p->job_exposure = p->staged[3];
   source = unpack_tiles(p);

   /* "Number of sheets: 0 means line feed only." A sheet is one burn of the
    * buffer; more than one repeats it. */
   p->job_source = (uint16_t)source;
   p->job_image = (uint16_t)(sheets * source);
   p->job_before = (uint16_t)(before * GBP_MARGIN_LINES);
   p->job_lines = (uint16_t)(p->job_before + p->job_image + after * GBP_MARGIN_LINES);
   p->job_done = 0;
   p->job_next_tick = tick + GBP_LINE_TICKS;
   p->job_active = p->job_lines > 0;

   /* The buffer is consumed by the print: what was waiting has been taken. */
   p->buffer_len = 0;
   p->data_closed = 0;
   p->status &= (uint8_t)~GBP_STATUS_UNPRINTED;
   p->status |= GBP_STATUS_FULL;
   if (p->job_active)
      p->status |= GBP_STATUS_PRINTING;
}

static void burn_line(gbp_printer *p, uint64_t tick)
{
   uint8_t line[GBP_WIDTH];
   unsigned n = p->job_done;

   if (n < p->job_before || n >= (unsigned)p->job_before + p->job_image || p->job_source == 0)
      memset(line, 0xFF, sizeof(line));
   else
   {
      unsigned y = (n - p->job_before) % p->job_source;
      unsigned x;
      for (x = 0; x < GBP_WIDTH; x++)
      {
         unsigned colour = p->job_pixels[y * GBP_WIDTH + x];
         unsigned shade = (p->job_palette >> (colour * 2)) & 3;
         line[x] = shade_grey(shade, p->job_exposure);
      }
   }

   strip_push(p, line, tick);
   p->job_done++;
}

void gbp_run(gbp_printer *p, uint64_t tick, int feed)
{
   while (p->job_active && p->job_next_tick <= tick)
   {
      burn_line(p, p->job_next_tick);
      p->job_next_tick += GBP_LINE_TICKS;
      if (p->job_done >= p->job_lines)
      {
         p->job_active = 0;
         p->status &= (uint8_t)~GBP_STATUS_PRINTING;
      }
   }

   /* FEED runs the motor for as long as it is held, and does nothing while a
    * print is going: the motor is already running. */
   if (feed && !p->job_active)
   {
      if (!p->feeding)
      {
         p->feeding = 1;
         p->feed_next_tick = tick + GBP_LINE_TICKS;
      }
      while (p->feed_next_tick <= tick)
      {
         strip_push_blank(p, p->feed_next_tick);
         p->feed_next_tick += GBP_LINE_TICKS;
      }
   }
   else
      p->feeding = 0;
}

/* ------------------------------------------------------------------------ */
/* The packet                                                                */
/* ------------------------------------------------------------------------ */

static void stage(gbp_printer *p, uint8_t value)
{
   if (p->staged_len < GBP_BUFFER_BYTES)
      p->staged[p->staged_len++] = value;
}

/* One byte of a DATA packet's body. With compression on, the body is groups:
 * a control byte with bit 7 set is a run of (n & 0x7F) + 2 copies of the byte
 * after it; with bit 7 clear, (n & 0x7F) + 1 literal bytes follow. */
static void body_byte(gbp_printer *p, uint8_t value)
{
   if (!p->compressed)
   {
      stage(p, value);
      return;
   }

   if (p->rle_run)
   {
      unsigned n = p->rle_left;
      while (n--)
         stage(p, value);
      p->rle_run = 0;
      p->rle_left = 0;
   }
   else if (p->rle_left > 0)
   {
      stage(p, value);
      p->rle_left--;
   }
   else if (value & 0x80)
   {
      p->rle_run = 1;
      p->rle_left = (uint16_t)((value & 0x7F) + 2);
   }
   else
      p->rle_left = (uint16_t)((value & 0x7F) + 1);
}

/* The packet's checksum is in. Act on it -- except a PRINT, which starts only
 * once the status byte has been read, so that the status a game reads back from
 * its own PRINT packet is the one from before the print, as on the hardware. */
static void packet_done(gbp_printer *p)
{
   p->packet_ok = p->sum == p->sum_received;
   if (!p->packet_ok)
   {
      p->status |= GBP_STATUS_CHECKSUM;
      return;
   }
   p->status &= (uint8_t)~GBP_STATUS_CHECKSUM;

   switch (p->command)
   {
   case GBP_CMD_INIT:
      /* Clears the RAM, and with it everything the status says about it. A
       * print already burning carries on: the motor does not stop for this. */
      p->buffer_len = 0;
      p->data_closed = 0;
      p->status &= (uint8_t)~(GBP_STATUS_FULL | GBP_STATUS_UNPRINTED);
      break;

   case GBP_CMD_DATA:
      if (p->length == 0)
         p->data_closed = 1;
      else
      {
         unsigned room = GBP_BUFFER_BYTES - p->buffer_len;
         unsigned n = p->staged_len;
         if (n > room)
         {
            n = room;
            p->status |= GBP_STATUS_FULL;
         }
         memcpy(p->buffer + p->buffer_len, p->staged, n);
         p->buffer_len = (uint16_t)(p->buffer_len + n);
         p->data_closed = 0;
      }
      if (p->buffer_len > 0)
         p->status |= GBP_STATUS_UNPRINTED;
      break;

   case GBP_CMD_BREAK:
      /* Stops the print: what is not burned yet is not burned. */
      p->job_active = 0;
      p->status &= (uint8_t)~GBP_STATUS_PRINTING;
      break;

   default:
      break;
   }
}

void gbp_receive(gbp_printer *p, uint8_t byte, uint64_t tick)
{
   uint8_t next_reply = 0x00;

   switch (p->at)
   {
   case GBP_AT_MAGIC0:
      if (byte == 0x88)
         p->at = GBP_AT_MAGIC1;
      break;

   case GBP_AT_MAGIC1:
      /* A second 0x88 is a packet starting one byte late, not garbage. */
      if (byte == 0x33)
         p->at = GBP_AT_CMD;
      else if (byte != 0x88)
         p->at = GBP_AT_MAGIC0;
      break;

   case GBP_AT_CMD:
      p->command = byte;
      p->sum = byte;
      p->staged_len = 0;
      p->rle_run = 0;
      p->rle_left = 0;
      p->at = GBP_AT_COMP;
      break;

   case GBP_AT_COMP:
      p->compressed = byte & 1;
      p->sum = (uint16_t)(p->sum + byte);
      p->at = GBP_AT_LEN0;
      break;

   case GBP_AT_LEN0:
      p->length = byte;
      p->sum = (uint16_t)(p->sum + byte);
      p->at = GBP_AT_LEN1;
      break;

   case GBP_AT_LEN1:
      p->length = (uint16_t)(p->length | (byte << 8));
      p->sum = (uint16_t)(p->sum + byte);
      p->remaining = p->length;
      p->at = p->length ? GBP_AT_DATA : GBP_AT_SUM0;
      break;

   case GBP_AT_DATA:
      p->sum = (uint16_t)(p->sum + byte);
      body_byte(p, byte);
      if (--p->remaining == 0)
         p->at = GBP_AT_SUM0;
      break;

   case GBP_AT_SUM0:
      p->sum_received = byte;
      p->at = GBP_AT_SUM1;
      break;

   case GBP_AT_SUM1:
      p->sum_received = (uint16_t)(p->sum_received | (byte << 8));
      packet_done(p);
      p->at = GBP_AT_ACK;
      /* The first trailing byte is answered 0x81: "a printer is here". */
      next_reply = 0x81;
      break;

   case GBP_AT_ACK:
      p->at = GBP_AT_STATUS;
      next_reply = p->status;
      break;

   case GBP_AT_STATUS:
      /* A PRINT is started only with the buffer closed by an empty DATA packet;
       * without one the hardware ignores it, and so does this. */
      if (p->packet_ok && p->command == GBP_CMD_PRINT && p->staged_len >= 4 && p->data_closed)
         start_print(p, tick);
      p->at = GBP_AT_MAGIC0;
      break;

   default:
      p->at = GBP_AT_MAGIC0;
      break;
   }

   p->reply = next_reply;
}
