/* Godot-free checks for the Game Boy Printer core.
 *
 * Two layers. The first drives printer.c directly with whole packets built here
 * from an image, and reads the paper back. The second includes the core itself
 * and plays the Game Boy against it through a mock of the link bus, so what is
 * checked there is what gambatte and mGBA would see on the wire: which byte the
 * printer has published, and that it has published it before the next clock.
 *
 * The image is built here, encoded here into tiles (and into the printer's RLE
 * when asked), and compared with the paper dot for dot -- so a fault in the
 * printer's decoding cannot hide behind the same fault in the check. It is
 * asymmetric on both axes, so a flip or a transposition shows.
 *
 * Exit status is the number of failed checks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/libretro.c"
#include "../src/printer.c"
#include "../src/sound.c"

static int failures;
static int checks;

#define CHECK(cond, ...) do { \
   checks++; \
   if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ------------------------------------------------------------------------ */
/* The picture                                                               */
/* ------------------------------------------------------------------------ */

static unsigned colour_at(unsigned x, unsigned y, unsigned seed)
{
   /* Diagonal bands with a solid block in the top-left corner only, and a
    * one-dot rule down column 3: none of it survives a mirror. */
   if (x < 24 && y < 6)
      return 3;
   if (x == 3)
      return 2;
   return ((x + 2 * y + seed) / 5) & 3;
}

/* Tiles in the printer's order: twenty to a row, 16 bytes each, a line's low
 * bitplane then its high one. */
static size_t encode_tiles(uint8_t *out, unsigned lines, unsigned seed)
{
   size_t n = 0;
   unsigned row, tile, line, x;
   for (row = 0; row < lines / 8; row++)
      for (tile = 0; tile < 20; tile++)
         for (line = 0; line < 8; line++)
         {
            uint8_t lo = 0, hi = 0;
            for (x = 0; x < 8; x++)
            {
               unsigned c = colour_at(tile * 8 + x, row * 8 + line, seed);
               lo |= (uint8_t)((c & 1) << (7 - x));
               hi |= (uint8_t)(((c >> 1) & 1) << (7 - x));
            }
            out[n++] = lo;
            out[n++] = hi;
         }
   return n;
}

/* The printer's RLE, written the obvious greedy way: runs of 2+ equal bytes as
 * runs, everything else as literals. */
static size_t rle(uint8_t *out, const uint8_t *in, size_t len)
{
   size_t i = 0, n = 0;
   while (i < len)
   {
      size_t run = 1;
      while (i + run < len && in[i + run] == in[i] && run < 129)
         run++;
      if (run >= 2)
      {
         out[n++] = (uint8_t)(0x80 | (run - 2));
         out[n++] = in[i];
         i += run;
      }
      else
      {
         size_t start = i, lit = 0;
         while (i < len && lit < 128)
         {
            if (i + 1 < len && in[i + 1] == in[i])
               break;
            i++;
            lit++;
         }
         out[n++] = (uint8_t)(lit - 1);
         memcpy(out + n, in + start, lit);
         n += lit;
      }
   }
   return n;
}

/* ------------------------------------------------------------------------ */
/* Talking to the printer directly                                           */
/* ------------------------------------------------------------------------ */

static uint64_t clock_tick;

/* Send one byte the way a Game Boy does: it reads what the printer published
 * for this byte, then the printer takes the byte. One transfer a byte. */
static uint8_t xfer(gbp_printer *p, uint8_t byte)
{
   uint8_t got = gbp_reply(p);
   gbp_receive(p, byte, clock_tick);
   clock_tick += 8192;
   gbp_run(p, clock_tick, 0);
   return got;
}

/* A whole packet. Returns the status byte; *ack gets the 0x81. */
static uint8_t packet(gbp_printer *p, uint8_t cmd, int comp, const uint8_t *data, size_t len,
      int corrupt, uint8_t *ack)
{
   uint16_t sum = (uint16_t)(cmd + (comp ? 1 : 0) + (len & 0xFF) + (len >> 8));
   size_t i;
   uint8_t a, s;

   xfer(p, 0x88);
   xfer(p, 0x33);
   xfer(p, cmd);
   xfer(p, comp ? 1 : 0);
   xfer(p, (uint8_t)(len & 0xFF));
   xfer(p, (uint8_t)(len >> 8));
   for (i = 0; i < len; i++)
   {
      sum = (uint16_t)(sum + data[i]);
      xfer(p, data[i]);
   }
   if (corrupt)
      sum ^= 0x0100;
   xfer(p, (uint8_t)(sum & 0xFF));
   xfer(p, (uint8_t)(sum >> 8));
   a = xfer(p, 0x00);
   s = xfer(p, 0x00);
   if (ack)
      *ack = a;
   return s;
}

static void wait_print(gbp_printer *p)
{
   int guard = 100000;
   while (p->job_active && guard--)
   {
      clock_tick += 8192;
      gbp_run(p, clock_tick, 0);
   }
}

/* Expected grey for colour c under palette/exposure. */
static uint8_t expect_grey(unsigned c, uint8_t palette, uint8_t exposure)
{
   return shade_grey((palette >> (c * 2)) & 3, exposure);
}

static int strip_matches(const gbp_printer *p, unsigned first, unsigned lines, unsigned seed,
      uint8_t palette, uint8_t exposure, unsigned *bad_x, unsigned *bad_y)
{
   unsigned x, y;
   for (y = 0; y < lines; y++)
      for (x = 0; x < GBP_WIDTH; x++)
         if (p->strip[first + y][x] != expect_grey(colour_at(x, y, seed), palette, exposure))
         {
            *bad_x = x;
            *bad_y = y;
            return 0;
         }
   return 1;
}

static int blank_lines(const gbp_printer *p, unsigned first, unsigned count)
{
   unsigned y, x;
   for (y = first; y < first + count; y++)
      for (x = 0; x < GBP_WIDTH; x++)
         if (p->strip[y][x] != 0xFF)
            return 0;
   return 1;
}

static uint8_t tiles[GBP_BUFFER_BYTES];
static uint8_t packed[GBP_BUFFER_BYTES * 2];

/* Load `lines` lines of picture as DATA packets of up to 0x280 bytes, close the
 * buffer with the empty one, and return the last DATA status. */
static uint8_t load_picture(gbp_printer *p, unsigned lines, unsigned seed, int compress)
{
   size_t len = encode_tiles(tiles, lines, seed);
   size_t off;
   uint8_t st = 0;
   for (off = 0; off < len; off += GBP_PACKET_BYTES)
   {
      size_t chunk = len - off < GBP_PACKET_BYTES ? len - off : GBP_PACKET_BYTES;
      if (compress)
      {
         size_t c = rle(packed, tiles + off, chunk);
         st = packet(p, GBP_CMD_DATA, 1, packed, c, 0, NULL);
      }
      else
         st = packet(p, GBP_CMD_DATA, 0, tiles + off, chunk, 0, NULL);
   }
   packet(p, GBP_CMD_DATA, 0, NULL, 0, 0, NULL);
   return st;
}

static uint8_t print_args[4];

static uint8_t send_print(gbp_printer *p, uint8_t sheets, uint8_t margins, uint8_t palette, uint8_t exposure)
{
   print_args[0] = sheets;
   print_args[1] = margins;
   print_args[2] = palette;
   print_args[3] = exposure;
   return packet(p, GBP_CMD_PRINT, 0, print_args, 4, 0, NULL);
}

/* ------------------------------------------------------------------------ */
/* Protocol                                                                  */
/* ------------------------------------------------------------------------ */

static gbp_printer P, Q;

static void test_detection(void)
{
   uint8_t ack, st;
   gbp_init(&P);
   st = packet(&P, GBP_CMD_STATUS, 0, NULL, 0, 0, &ack);
   CHECK(ack == 0x81, "detection ack 0x%02X, want 0x81", ack);
   CHECK(st == 0x00, "idle status 0x%02X, want 0x00", st);
   /* Every byte that is not one of the two trailing ones reads 0x00. */
   CHECK(gbp_reply(&P) == 0x00, "reply between packets 0x%02X", gbp_reply(&P));
}

static void test_print_sequence(int compress)
{
   uint8_t st, ack;
   unsigned bx = 0, by = 0;
   const unsigned lines = 144;

   gbp_init(&P);
   clock_tick = 0;
   st = packet(&P, GBP_CMD_INIT, 0, NULL, 0, 0, &ack);
   CHECK(ack == 0x81 && st == 0x00, "INIT -> %02X %02X", ack, st);

   st = load_picture(&P, lines, 7, compress);
   CHECK(st == GBP_STATUS_UNPRINTED, "after DATA status 0x%02X, want 0x08", st);
   CHECK(P.buffer_len == lines / 8 * GBP_TILE_ROW_BYTES, "buffer holds %u bytes", P.buffer_len);

   /* The status read back from the PRINT packet itself is the one from before
    * the print; the next poll sees it burning. */
   st = send_print(&P, 1, 0x13, 0xE4, 0x40);
   CHECK(st == GBP_STATUS_UNPRINTED, "PRINT packet status 0x%02X, want 0x08", st);
   st = packet(&P, GBP_CMD_STATUS, 0, NULL, 0, 0, NULL);
   CHECK(st == (GBP_STATUS_PRINTING | GBP_STATUS_FULL), "while printing 0x%02X, want 0x06", st);

   wait_print(&P);
   st = packet(&P, GBP_CMD_STATUS, 0, NULL, 0, 0, NULL);
   CHECK(st == GBP_STATUS_FULL, "after printing 0x%02X, want 0x04", st);

   CHECK(P.strip_lines == 16 + lines + 48, "strip %u lines, want %u", P.strip_lines, 16 + lines + 48);
   CHECK(blank_lines(&P, 0, 16), "upper margin not blank");
   CHECK(blank_lines(&P, 16 + lines, 48), "lower margin not blank");
   CHECK(strip_matches(&P, 16, lines, 7, 0xE4, 0x40, &bx, &by),
         "%s picture differs at (%u,%u): got %u want %u", compress ? "RLE" : "raw",
         bx, by, P.strip[16 + by][bx], expect_grey(colour_at(bx, by, 7), 0xE4, 0x40));

   st = packet(&P, GBP_CMD_INIT, 0, NULL, 0, 0, NULL);
   st = packet(&P, GBP_CMD_STATUS, 0, NULL, 0, 0, NULL);
   CHECK(st == 0x00, "INIT clears status, got 0x%02X", st);
}

static void test_print_takes_time(void)
{
   uint64_t start;
   gbp_init(&P);
   clock_tick = 0;
   load_picture(&P, 16, 1, 0);
   send_print(&P, 1, 0x00, 0xE4, 0x40);
   start = clock_tick;
   CHECK(P.job_active, "print did not start");
   CHECK(P.strip_lines <= 1, "print burned %u lines at once", P.strip_lines);
   wait_print(&P);
   CHECK(clock_tick - start >= (uint64_t)15 * GBP_LINE_TICKS,
         "16 lines took %llu ticks, want about %u", (unsigned long long)(clock_tick - start),
         16 * GBP_LINE_TICKS);
}

static void test_checksum_error(void)
{
   uint8_t st;
   size_t len;
   gbp_init(&P);
   clock_tick = 0;
   len = encode_tiles(tiles, 16, 3);
   st = packet(&P, GBP_CMD_DATA, 0, tiles, len, 1, NULL);
   CHECK(st & GBP_STATUS_CHECKSUM, "bad checksum not reported (0x%02X)", st);
   CHECK(P.buffer_len == 0, "bad packet's data kept (%u bytes)", P.buffer_len);
   st = packet(&P, GBP_CMD_DATA, 0, tiles, len, 0, NULL);
   CHECK(!(st & GBP_STATUS_CHECKSUM) && P.buffer_len == len, "good packet after a bad one: 0x%02X, %u",
         st, P.buffer_len);
}

static void test_print_needs_closed_buffer(void)
{
   size_t len;
   gbp_init(&P);
   clock_tick = 0;
   len = encode_tiles(tiles, 16, 3);
   packet(&P, GBP_CMD_DATA, 0, tiles, len, 0, NULL);
   send_print(&P, 1, 0, 0xE4, 0x40);
   CHECK(!P.job_active && P.strip_lines == 0, "PRINT without the empty DATA packet printed");
}

static void test_palette_and_sheets(void)
{
   unsigned bx = 0, by = 0;
   gbp_init(&P);
   clock_tick = 0;
   load_picture(&P, 16, 2, 0);
   /* Palette 0x1B is 0xE4 backwards: white prints black. Two sheets, no margins. */
   send_print(&P, 2, 0x00, 0x1B, 0x7F);
   wait_print(&P);
   CHECK(P.strip_lines == 32, "two sheets gave %u lines", P.strip_lines);
   CHECK(strip_matches(&P, 0, 16, 2, 0x1B, 0x7F, &bx, &by), "sheet 1 palette wrong at (%u,%u)", bx, by);
   CHECK(strip_matches(&P, 16, 16, 2, 0x1B, 0x7F, &bx, &by), "sheet 2 differs at (%u,%u)", bx, by);
   CHECK(expect_grey(0, 0x1B, 0x7F) == 0, "palette 0x1B colour 0 should burn black");
   CHECK(shade_grey(1, 0x00) > shade_grey(1, 0x7F), "exposure does not darken");
}

static void test_tear_and_feed(void)
{
   unsigned left;
   gbp_init(&P);
   clock_tick = 0;
   load_picture(&P, 16, 4, 0);
   send_print(&P, 1, 0x00, 0xE4, 0x40);
   wait_print(&P);
   CHECK(P.strip_lines == 16, "strip %u", P.strip_lines);

   /* The OLDEST lines go, so what stays is what was burned last. */
   left = P.strip[15][0];
   CHECK(gbp_tear(&P, 10) == 10 && P.strip_lines == 6, "tear 10 left %u", P.strip_lines);
   CHECK(P.strip[5][0] == left, "tear took the wrong end");
   CHECK(gbp_tear(&P, 100) == 6 && P.strip_lines == 0, "tear-all left %u", P.strip_lines);
   CHECK(P.tear_count == 2, "tear count %u", P.tear_count);

   gbp_run(&P, clock_tick, 1);
   gbp_run(&P, clock_tick + 10 * GBP_LINE_TICKS, 1);
   CHECK(P.strip_lines == 10, "feeding 10 line-times gave %u lines", P.strip_lines);
   CHECK(blank_lines(&P, 0, P.strip_lines), "fed paper is not blank");
   gbp_run(&P, clock_tick + 20 * GBP_LINE_TICKS, 0);
   CHECK(P.strip_lines == 10, "feed kept running after release (%u)", P.strip_lines);
}

static void test_state_round_trip(void)
{
   uint64_t mid;
   gbp_init(&P);
   clock_tick = 0;
   load_picture(&P, 144, 9, 1);
   send_print(&P, 1, 0x13, 0xE4, 0x40);
   mid = clock_tick + 40 * GBP_LINE_TICKS;
   gbp_run(&P, mid, 0);
   CHECK(P.job_active && P.strip_lines > 20, "not mid-print (%u lines)", P.strip_lines);

   /* A copy is the savestate. Both go on from here and must agree to the dot. */
   memcpy(&Q, &P, sizeof(P));
   gbp_run(&P, mid + 400 * GBP_LINE_TICKS, 0);
   gbp_run(&Q, mid + 400 * GBP_LINE_TICKS, 0);
   CHECK(memcmp(&P, &Q, sizeof(P)) == 0, "a restored printer went a different way");
   CHECK(!P.job_active && P.strip_lines == 16 + 144 + 48, "restored print ended with %u lines",
         P.strip_lines);
}

/* ------------------------------------------------------------------------ */
/* The core on a mock link bus                                               */
/* ------------------------------------------------------------------------ */

/* The mock plays a Game Boy that clocks bytes at fixed ticks, as gambatte
 * would: each CLOCK is stamped where the Game Boy's horizon puts it, and the
 * byte the Game Boy receives is the printer's last published state at or
 * before that stamp. */
typedef struct mock_msg { uint64_t tick; uint8_t buf[8]; } mock_msg;

static mock_msg mock_out[4096];   /* the printer's states */
static unsigned mock_out_n;
static mock_msg mock_in[4096];    /* the Game Boy's clocks, in tick order */
static unsigned mock_in_n, mock_in_next;
static uint64_t mock_published_safe;
static int mock_peers = 2;
static int mock_bad_stamp;

static retro_link_port_t *RETRO_CALLCONV mock_attach(unsigned port, const char *id, uint64_t rate)
{
   (void)port;
   CHECK(strcmp(id, "gb-sio") == 0 && rate == 8388608, "attached as %s @ %llu", id,
         (unsigned long long)rate);
   return (retro_link_port_t *)(uintptr_t)1;
}
static void RETRO_CALLCONV mock_detach(retro_link_port_t *h) { (void)h; }
static int RETRO_CALLCONV mock_peers_fn(retro_link_port_t *h, unsigned *count)
{
   (void)h;
   if (count)
      *count = (unsigned)mock_peers;
   return mock_peers >= 2 ? 1 : -1;
}
static bool RETRO_CALLCONV mock_send(retro_link_port_t *h, uint64_t tick, unsigned to, const void *buf, size_t len)
{
   (void)h;
   (void)to;
   if (tick < mock_published_safe)
      mock_bad_stamp++;
   if (mock_out_n < 4096 && len == 8)
   {
      mock_out[mock_out_n].tick = tick;
      memcpy(mock_out[mock_out_n].buf, buf, 8);
      mock_out_n++;
   }
   return true;
}
/* The Game Boy's messages are delivered once the printer's clock is within a
 * horizon of them -- the earliest the real bus could hand them over. */
static uint64_t mock_now;
static bool RETRO_CALLCONV mock_recv(retro_link_port_t *h, uint64_t *tick, unsigned *from, void *buf, size_t *len)
{
   (void)h;
   if (mock_in_next >= mock_in_n || mock_in[mock_in_next].tick > mock_now + 2048)
      return false;
   *tick = mock_in[mock_in_next].tick;
   *from = 0;
   memcpy(buf, mock_in[mock_in_next].buf, 8);
   *len = 8;
   mock_in_next++;
   return true;
}
static uint64_t RETRO_CALLCONV mock_advance(retro_link_port_t *h, uint64_t local, uint64_t safe, uint64_t req, uint32_t *wake)
{
   (void)h;
   CHECK(safe >= local, "safe %llu below local %llu", (unsigned long long)safe, (unsigned long long)local);
   if (safe > mock_published_safe)
      mock_published_safe = safe;
   if (wake)
      *wake = 0;
   mock_now = req > local ? req : local;
   return mock_now;
}

static bool RETRO_CALLCONV mock_environ(unsigned cmd, void *data)
{
   switch (cmd)
   {
   case RETRO_ENVIRONMENT_GET_LINK_INTERFACE:
   {
      struct retro_link_interface *i = (struct retro_link_interface *)data;
      i->attach = mock_attach;
      i->detach = mock_detach;
      i->peers = mock_peers_fn;
      i->send = mock_send;
      i->recv = mock_recv;
      i->advance = mock_advance;
      return true;
   }
   case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
   case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
   case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
      return true;
   default:
      return false;
   }
}

static unsigned frame_w, frame_h, frames_sent, frames_duped;
static uint32_t frame_px[GBP_STRIP_MAX * GBP_WIDTH];
static void RETRO_CALLCONV mock_video(const void *data, unsigned w, unsigned h, size_t pitch)
{
   (void)pitch;
   if (!data)
   {
      frames_duped++;
      return;
   }
   frames_sent++;
   frame_w = w;
   frame_h = h;
   memcpy(frame_px, data, (size_t)w * h * 4);
}
static unsigned audio_frames;
static long audio_energy;
static size_t RETRO_CALLCONV mock_audio(const int16_t *data, size_t frames)
{
   size_t i;
   audio_frames += (unsigned)frames;
   for (i = 0; i < frames * 2; i++)
      audio_energy += data[i] < 0 ? -data[i] : data[i];
   return frames;
}

static int pad_a, pad_b, pad_lx;
static void RETRO_CALLCONV mock_poll(void) {}
static int16_t RETRO_CALLCONV mock_input(unsigned port, unsigned device, unsigned index, unsigned id)
{
   (void)port;
   (void)index;
   if (device == RETRO_DEVICE_JOYPAD && id == RETRO_DEVICE_ID_JOYPAD_A)
      return (int16_t)pad_a;
   if (device == RETRO_DEVICE_JOYPAD && id == RETRO_DEVICE_ID_JOYPAD_B)
      return (int16_t)pad_b;
   if (device == RETRO_DEVICE_ANALOG && id == RETRO_DEVICE_ID_ANALOG_X)
      return (int16_t)pad_lx;
   return 0;
}

/* The printer's published byte as the Game Boy sees it at `tick`. */
static int printer_sb_at(uint64_t tick)
{
   int sb = -1;
   unsigned i;
   for (i = 0; i < mock_out_n; i++)
      if (mock_out[i].buf[0] == LS_STATE && mock_out[i].tick <= tick)
         sb = (mock_out[i].buf[3] & LS_FLAG_ARMED) ? mock_out[i].buf[2] : 0xFF;
   return sb;
}

static uint8_t script[GBP_BUFFER_BYTES * 3];
static uint8_t expected[GBP_BUFFER_BYTES * 3]; /* what the Game Boy must read */
static size_t script_n;

static void script_packet(uint8_t cmd, const uint8_t *data, size_t len, uint8_t status)
{
   size_t first = script_n;
   uint16_t sum = (uint16_t)(cmd + (len & 0xFF) + (len >> 8));
   size_t i;
   script[script_n++] = 0x88;
   script[script_n++] = 0x33;
   script[script_n++] = cmd;
   script[script_n++] = 0;
   script[script_n++] = (uint8_t)(len & 0xFF);
   script[script_n++] = (uint8_t)(len >> 8);
   for (i = 0; i < len; i++)
   {
      script[script_n++] = data[i];
      sum = (uint16_t)(sum + data[i]);
   }
   script[script_n++] = (uint8_t)(sum & 0xFF);
   script[script_n++] = (uint8_t)(sum >> 8);
   script[script_n++] = 0;
   script[script_n++] = 0;
   memset(expected + first, 0, script_n - first);
   expected[script_n - 2] = 0x81;
   expected[script_n - 1] = status;
}

static void run_frames(unsigned n)
{
   while (n--)
      retro_run();
}

/* `spacing`: ticks from one byte's start to the next. A normal-speed transfer is
 * 8192; a Game Boy Color in double speed (Super Mario Bros. Deluxe) takes 4096,
 * and its answer then has 2048 ticks to land after the clock reaches the
 * printer. */
static void test_core_on_the_wire(uint64_t spacing)
{
   uint64_t t0 = 130 * FRAME_TICKS; /* after the idle stretch below */
   size_t len, i;
   unsigned acks = 0, wrong = 0;
   unsigned bx = 0, by = 0;

   mock_out_n = mock_in_n = mock_in_next = 0;
   mock_published_safe = 0;
   mock_bad_stamp = 0;
   mock_peers = 2;
   script_n = 0;

   script_packet(GBP_CMD_INIT, NULL, 0, 0x00);
   len = encode_tiles(tiles, 16, 5);
   script_packet(GBP_CMD_DATA, tiles, len, GBP_STATUS_UNPRINTED);
   script_packet(GBP_CMD_DATA, NULL, 0, GBP_STATUS_UNPRINTED);
   print_args[0] = 1;
   print_args[1] = 0x00;
   print_args[2] = 0xE4;
   print_args[3] = 0x40;
   script_packet(GBP_CMD_PRINT, print_args, 4, GBP_STATUS_UNPRINTED);
   /* And a status poll straight after, which must see it burning. */
   script_packet(GBP_CMD_STATUS, NULL, 0, GBP_STATUS_PRINTING | GBP_STATUS_FULL);

   for (i = 0; i < script_n; i++)
   {
      mock_in[i].tick = t0 + i * spacing + 2048; /* stamped a Game Boy horizon out */
      memset(mock_in[i].buf, 0, 8);
      mock_in[i].buf[0] = LS_CLOCK;
      mock_in[i].buf[2] = script[i];
   }
   mock_in_n = (unsigned)script_n;

   retro_set_environment(mock_environ);
   retro_set_video_refresh(mock_video);
   retro_set_input_poll(mock_poll);
   retro_set_input_state(mock_input);
   retro_set_audio_sample_batch(mock_audio);
   retro_init();
   CHECK(retro_load_game(NULL), "load with no content failed");

   frames_sent = frames_duped = 0;
   audio_frames = 0;
   audio_energy = 0;
   run_frames(128);
   CHECK(audio_frames == 70224, "128 frames made %u samples, want exactly 70224 (548.625 a frame)",
         audio_frames);
   CHECK(audio_energy == 0, "an idle printer made sound (%ld)", audio_energy);
   CHECK(frame_w == 1 && frame_h == 1, "empty paper should be a 1x1 frame, got %ux%u", frame_w, frame_h);

   /* Enough frames for every byte and the print. */
   run_frames((unsigned)((t0 + script_n * spacing) / FRAME_TICKS) + 2 + 16 * GBP_LINE_TICKS / FRAME_TICKS + 4);
   CHECK(mock_in_next == mock_in_n, "the printer took %u of %u clocks", mock_in_next, mock_in_n);
   CHECK(mock_bad_stamp == 0, "%d messages stamped inside a published horizon", mock_bad_stamp);

   /* What the Game Boy read for every byte, decided by what the printer had
    * published by the moment each was clocked. */
   for (i = 0; i < script_n; i++)
   {
      int sb = printer_sb_at(mock_in[i].tick - 2048);
      if (sb == 0x81)
         acks++;
      if (sb != expected[i])
      {
         if (!wrong)
            printf("  byte %u (0x%02X) read 0x%02X, want 0x%02X\n", (unsigned)i, script[i], sb, expected[i]);
         wrong++;
      }
   }
   CHECK(acks == 5, "Game Boy read 0x81 %u times over 5 packets", acks);
   CHECK(wrong == 0, "Game Boy read %u bytes wrong", wrong);

   CHECK(frame_w == GBP_WIDTH && frame_h == 16, "paper frame %ux%u, want 160x16", frame_w, frame_h);
   CHECK(printer.strip_lines == 16 && strip_matches(&printer, 0, 16, 5, 0xE4, 0x40, &bx, &by),
         "printed through the wire, differs at (%u,%u)", bx, by);
   for (i = 0; i < 16 * GBP_WIDTH; i++)
   {
      uint32_t g = printer.strip[i / GBP_WIDTH][i % GBP_WIDTH];
      if (frame_px[i] != ((g << 16) | (g << 8) | g))
      {
         CHECK(0, "frame pixel %u is %08X, strip says %u", (unsigned)i, frame_px[i], g);
         break;
      }
   }
   CHECK(frames_duped > 0, "an unchanged strip should be a duplicated frame");

   /* Tear 10 of the 16 through the B button, with LX saying how many. */
   pad_b = 1;
   pad_lx = 10;
   run_frames(1);
   CHECK(printer.strip_lines == 6, "tear through input left %u lines", printer.strip_lines);
   run_frames(3);
   CHECK(printer.strip_lines == 6, "a held B tore again (%u)", printer.strip_lines);
   pad_b = 0;
   pad_lx = 0;
   run_frames(1);
   pad_b = 1;
   run_frames(1);
   pad_b = 0;
   CHECK(printer.strip_lines == 0, "B with LX 0 should tear everything, left %u", printer.strip_lines);

   /* FEED for a second puts out blank paper, and the motor is heard. */
   audio_energy = 0;
   pad_a = 1;
   run_frames(60);
   pad_a = 0;
   CHECK(audio_energy > 0, "feeding paper made no sound");
   run_frames(30);
   audio_energy = 0;
   run_frames(30);
   CHECK(audio_energy == 0, "the motor kept sounding after the paper stopped (%ld)", audio_energy);
   CHECK(printer.strip_lines >= 30 && printer.strip_lines <= 40, "a second of FEED gave %u lines",
         printer.strip_lines);

   /* Savestate through the libretro entry points: two runs from one state
    * agree. */
   {
      size_t n = retro_serialize_size();
      void *a = malloc(n), *b = malloc(n);
      CHECK(retro_serialize(a, n), "serialize failed");
      pad_a = 1;
      run_frames(5);
      pad_a = 0;
      CHECK(retro_serialize(b, n), "serialize failed");
      CHECK(retro_unserialize(a, n), "unserialize failed");
      pad_a = 1;
      run_frames(5);
      pad_a = 0;
      {
         void *c = malloc(n);
         retro_serialize(c, n);
         CHECK(memcmp(b, c, n) == 0, "replaying the same input from a state did not land in the same state");
         free(c);
      }
      free(a);
      free(b);
   }

   retro_unload_game();
   retro_deinit();
}

int main(void)
{
   test_detection();
   test_print_sequence(0);
   test_print_sequence(1);
   test_print_takes_time();
   test_checksum_error();
   test_print_needs_closed_buffer();
   test_palette_and_sheets();
   test_tear_and_feed();
   test_state_round_trip();
   test_core_on_the_wire(8192 + 512);
   /* Back to back in double speed, no gap at all. */
   test_core_on_the_wire(4096);

   printf("%d checks, %d failed\n", checks, failures);
   return failures;
}
