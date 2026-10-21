/* Render what a print sounds like to a WAV, for listening to the synthesis
 * without a frontend: a 144-line picture with the Game Boy Camera's 0x13
 * margins, a pause, then a second of FEED.
 *
 *     render_wav out.wav
 */
#include <stdio.h>
#include <string.h>

#include "../src/printer.c"
#include "../src/sound.c"

static gbp_printer P;
static gbp_sound S;
static int16_t buf[550 * 2];
static int16_t all[32768 * 30 * 2];
static size_t all_n;

static uint64_t t;

static void xfer(uint8_t b)
{
   gbp_receive(&P, b, t);
   t += 8192 + 512;
}

static void packet(uint8_t cmd, const uint8_t *d, size_t n)
{
   uint16_t sum = (uint16_t)(cmd + (n & 0xFF) + (n >> 8));
   size_t i;
   xfer(0x88); xfer(0x33); xfer(cmd); xfer(0); xfer((uint8_t)(n & 0xFF)); xfer((uint8_t)(n >> 8));
   for (i = 0; i < n; i++) { sum = (uint16_t)(sum + d[i]); xfer(d[i]); }
   xfer((uint8_t)sum); xfer((uint8_t)(sum >> 8)); xfer(0); xfer(0);
}

static void frames(unsigned n, int feed)
{
   while (n--)
   {
      unsigned k;
      t += 140448;
      gbp_run(&P, t, feed);
      k = gbp_sound_frame(&S, &P, t, buf);
      if (all_n + k * 2 <= sizeof(all) / sizeof(all[0]))
      {
         memcpy(all + all_n, buf, k * 4);
         all_n += k * 2;
      }
   }
}

static void put32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc(v >> 24, f); }
static void put16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); }

int main(int argc, char **argv)
{
   static uint8_t tiles[GBP_PACKET_BYTES];
   uint8_t args[4] = { 1, 0x13, 0xE4, 0x40 };
   unsigned i, k;
   FILE *f;

   gbp_init(&P);
   gbp_sound_init(&S);
   frames(20, 0);
   packet(GBP_CMD_INIT, NULL, 0);
   for (k = 0; k < 9; k++)
   {
      /* Something with ink in it: diagonal bands. */
      for (i = 0; i < GBP_PACKET_BYTES; i++)
         tiles[i] = (uint8_t)(((i / 2) + k * 7) & 0x7 ? 0x0F << (i & 3) : 0xFF);
      packet(GBP_CMD_DATA, tiles, sizeof(tiles));
   }
   packet(GBP_CMD_DATA, NULL, 0);
   packet(GBP_CMD_PRINT, args, 4);
   frames(60 * 9, 0);
   frames(60, 1);
   frames(60, 0);

   f = fopen(argc > 1 ? argv[1] : "print.wav", "wb");
   if (!f)
      return 1;
   fwrite("RIFF", 1, 4, f); put32(f, (uint32_t)(36 + all_n * 2));
   fwrite("WAVEfmt ", 1, 8, f); put32(f, 16); put16(f, 1); put16(f, 2);
   put32(f, GBP_SOUND_RATE); put32(f, GBP_SOUND_RATE * 4); put16(f, 4); put16(f, 16);
   fwrite("data", 1, 4, f); put32(f, (uint32_t)(all_n * 2));
   fwrite(all, 2, all_n, f);
   fclose(f);
   printf("%u lines printed, %.1f s of sound\n", P.strip_lines, all_n / 2.0 / GBP_SOUND_RATE);
   return 0;
}
