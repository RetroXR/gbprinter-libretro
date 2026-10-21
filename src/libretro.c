/* A Game Boy Printer as a libretro core.
 *
 * There is no CPU here and no game. The printer is a peripheral on the far end
 * of a Game Boy link cable, and the cable is the frontend's link bus
 * (RETRO_ENVIRONMENT_GET_LINK_INTERFACE): this core joins the "gb-sio-1" wire
 * that gambatte and mGBA already speak to each other, and answers the Game Boy
 * the way a printer would. Neither Game Boy core knows or needs to know that the
 * machine on the other end of its cable is not another Game Boy.
 *
 * What it shows is the paper: the video frame is the strip hanging out of the
 * slot, one row per dot line, oldest at the top. A frontend that wants the paper
 * draws the frame on it; one that does not can ignore it.
 *
 * Its two buttons are input, so that a netplay session replays them like any
 * other press:
 *
 *   A    FEED: runs the motor while held, feeding blank paper.
 *   B    TEAR: on the press, tears off the oldest LX lines of the strip (left
 *        stick X, as a plain count), or all of it when LX is 0. A frontend that
 *        captured the strip to keep what was torn says how much it captured, so
 *        a line burned between its look and the press is never thrown away.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <libretro.h>

#include "link_interface.h"
#include "printer.h"
#include "sound.h"

#define CORE_NAME    "GB Printer"
#define CORE_VERSION "1.0.0"

/* ------------------------------------------------------------------------ */
/* The wire                                                                  */
/* ------------------------------------------------------------------------ */

/* What gambatte's and mGBA's Game Boy drivers call it, and must: peers whose
 * protocol ids differ are never joined. */
#define LINK_PROTOCOL "gb-sio-1"

/* Twice the Game Boy's 4.194304 MHz, as both Game Boy drivers count. */
#define LINK_CLOCK_RATE ((uint64_t)8388608)

/* One Game Boy video frame, 70224 cycles, in link ticks. The printer runs
 * exactly one per retro_run so that its frames are the Game Boy's: a netplay
 * session steps every machine in a group one frame at a time, and a machine
 * whose frame was a different length of emulated time would drift on the bus
 * against its partner until one of them waited on the other for ever. */
#define FRAME_TICKS ((uint64_t)140448)

/* How the printer keeps time with a Game Boy on the wire.
 *
 * GRAIN is how far it asks to run between rendezvous, a quarter of a normal
 * transfer as the Game Boy drivers use. HORIZON is what it promises: nothing it
 * says lands before now + HORIZON. REPLY is how long after the clock that asked
 * for it the answer to the next byte lands.
 *
 * All three are 2048, and REPLY may not be more than HORIZON's worth past the
 * clock: a Game Boy Color in double speed -- Super Mario Bros. Deluxe -- takes a
 * byte in 4096 ticks, not 8192, and its clock reaches the printer a Game Boy
 * horizon (2048) after it started, so an answer has 2048 ticks to land. Promising
 * further ahead than that (4096 was tried, for netplay) made every answer late
 * and the game report Error 02.
 *
 * It is enough because the printer only ever publishes a horizon past a tick once
 * it has answered every clock up to that tick: the answer to a clock at t is sent
 * before the printer promises anything beyond t + 2048. */
#define LINK_GRAIN ((uint64_t)2048)
#define LINK_REPLY ((uint64_t)2048)
#define LINK_HORIZON ((uint64_t)2048)

#define LINK_REANNOUNCE_GRAINS 8
#define LINK_PENDING_MAX 16

/* Bus message, byte for byte what the Game Boy drivers send:
 * [0] type, [1] sender's bus index, [2] serial byte, [3] flags, [4..7] zero. */
enum { LS_STATE = 1, LS_CLOCK = 2 };
enum { LS_FLAG_ARMED = 1, LS_FLAG_FAST = 2 };
#define LS_MSG_SIZE 8

typedef struct pending_msg
{
   uint64_t tick;
   uint8_t type;
   uint8_t sb;
   uint8_t flags;
   uint8_t pad[5];
} pending_msg;

/* Everything about the wire that a savestate has to carry. The handle and the
 * interface are not in it: they belong to this process, not to the machine. */
typedef struct link_state
{
   uint64_t now;
   uint64_t reannounce_until;
   uint64_t promised;        /* the furthest horizon published */
   uint32_t pending_count;
   uint8_t last_sb;
   uint8_t last_flags;
   uint8_t published;
   uint8_t pad;
   pending_msg pending[LINK_PENDING_MAX];
} link_state;

static struct retro_link_interface link_iface;
static int link_have_iface;
static retro_link_port_t *link_handle;
static int link_anchored;
static int link_self_id;
static unsigned link_peers;
static link_state wire;

/* ------------------------------------------------------------------------ */
/* Core state                                                                */
/* ------------------------------------------------------------------------ */

#define STATE_MAGIC 0x52504247u /* "GBPR" */

typedef struct core_state
{
   uint32_t magic;
   uint32_t version;
   uint32_t size;
   uint8_t prev_tear;
   uint8_t pad[3];
   link_state wire;
   gbp_sound sound;
   gbp_printer printer;
} core_state;

static gbp_printer printer;
static gbp_sound sound;
static int16_t sound_buf[550 * 2];
static uint8_t prev_tear;

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static uint32_t framebuffer[GBP_STRIP_MAX * GBP_WIDTH];
static uint32_t sent_serial;
static int sent_any;

static void log_msg(enum retro_log_level level, const char *fmt, ...)
{
   char buf[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   if (log_cb)
      log_cb(level, "[GB Printer] %s", buf);
}

/* ------------------------------------------------------------------------ */
/* Link                                                                      */
/* ------------------------------------------------------------------------ */

/* Rendezvous: publish where the printer stands and what it promises, and ask
 * to run to `request`. */
static uint64_t link_advance(uint64_t request, uint32_t *wake)
{
   uint64_t safe = wire.now + LINK_HORIZON;
   if (safe > wire.promised)
      wire.promised = safe;
   link_anchored = 1;
   return link_iface.advance(link_handle, wire.now, safe, request, wake);
}

static void link_send(uint64_t tick, uint8_t type, uint8_t sb, uint8_t flags)
{
   uint8_t msg[LS_MSG_SIZE];

   /* Nothing goes out before the first rendezvous: that is what anchors the
    * origin the bus measures a tick from. See gambatte's LinkSerial::wire.
    * Asking for where we already stand is granted at once. */
   if (!link_anchored)
      link_advance(wire.now, NULL);

   /* Never inside a promise already made: a peer may have run up to it. */
   if (tick < wire.promised)
      tick = wire.promised;

   memset(msg, 0, sizeof(msg));
   msg[0] = type;
   msg[1] = (uint8_t)link_self_id;
   msg[2] = sb;
   msg[3] = flags;
   link_iface.send(link_handle, tick, RETRO_LINK_BROADCAST, msg, sizeof(msg));
}

/* Say what is in the shift register: always armed, always the reply to the
 * next byte. A printer is never the end that drives the clock, so unlike a
 * Game Boy it has no moment at which it is not waiting to be clocked. */
static void link_publish(uint64_t at)
{
   uint8_t sb = gbp_reply(&printer);
   uint8_t flags = LS_FLAG_ARMED;

   if (!link_handle || link_peers < 2)
      return;
   if (wire.published && wire.last_sb == sb && wire.last_flags == flags)
      return;

   wire.last_sb = sb;
   wire.last_flags = flags;
   wire.published = 1;
   link_send(at + LINK_REPLY, LS_STATE, sb, flags);
}

static void link_refresh_peers(void)
{
   unsigned was = link_peers;
   unsigned count = 0;
   int id;

   if (!link_handle)
      return;

   id = link_iface.peers(link_handle, &count);
   if (id < 0)
   {
      link_self_id = 0;
      link_peers = 0;
   }
   else if (count > 2)
   {
      /* A Game Boy cable joins two. Carrying a third machine's bytes would be a
       * transfer no real cable makes, so the printer goes quiet, exactly as the
       * Game Boy drivers do. */
      link_self_id = 0;
      link_peers = 0;
   }
   else
   {
      link_self_id = id;
      link_peers = count;
   }

   if (link_peers != was)
   {
      /* A cable moved: forget anything in flight and say who we are again for a
       * while, because a state sent while the other end was being re-seated is
       * dropped on the way. A deadline on this machine's own clock, not "until
       * answered", so two netplay peers decide the same. */
      wire.pending_count = 0;
      wire.published = 0;
      link_anchored = 0;
      wire.reannounce_until = wire.now + LINK_REANNOUNCE_GRAINS * LINK_GRAIN;
      log_msg(RETRO_LOG_INFO, "%u machine(s) on the wire, this one is %d\n", link_peers, link_self_id);
   }
}

static void link_pump(void)
{
   uint8_t buf[64];
   uint64_t tick;
   unsigned from;
   size_t len = sizeof(buf);

   while (link_iface.recv(link_handle, &tick, &from, buf, &len))
   {
      if (len == LS_MSG_SIZE && buf[0] == LS_CLOCK)
      {
         /* Only clocks matter: what the Game Boy holds in ITS register is read
          * by nobody here. */
         if (wire.pending_count < LINK_PENDING_MAX)
         {
            pending_msg *m = &wire.pending[wire.pending_count++];
            memset(m, 0, sizeof(*m));
            m->tick = tick;
            m->type = buf[0];
            m->sb = buf[2];
            m->flags = buf[3];
         }
         else
            log_msg(RETRO_LOG_WARN, "inbox full, dropping a clock the horizon should have made impossible\n");
      }
      len = sizeof(buf);
   }
}

/* Run the printer to `tick`, taking every clock stamped at or before it at its
 * own tick. Each byte is handed to the printer, and the answer to the next one
 * is published a horizon after the clock that asked for it. */
static void link_run_to(uint64_t tick, int feed)
{
   while (wire.pending_count > 0 && wire.pending[0].tick <= tick)
   {
      pending_msg m = wire.pending[0];
      memmove(&wire.pending[0], &wire.pending[1], sizeof(pending_msg) * (wire.pending_count - 1));
      wire.pending_count--;

      if (m.tick > wire.now)
         wire.now = m.tick;
      gbp_run(&printer, wire.now, feed);

      if (link_peers >= 2)
      {
         gbp_receive(&printer, m.sb, wire.now);
         link_publish(wire.now);
      }
   }

   if (tick > wire.now)
      wire.now = tick;
   gbp_run(&printer, wire.now, feed);
}

static void link_frame(int feed)
{
   uint64_t end = wire.now + FRAME_TICKS;

   if (!link_handle)
   {
      wire.now = end;
      gbp_run(&printer, wire.now, feed);
      return;
   }

   while (wire.now < end)
   {
      uint64_t want;
      uint64_t grant;
      uint32_t wake = RETRO_LINK_WAKE_NONE;

      link_refresh_peers();

      if (link_peers >= 2 && wire.now < wire.reannounce_until)
         wire.published = 0;
      link_publish(wire.now);

      want = wire.now + LINK_GRAIN;
      if (want > end)
         want = end;

      grant = link_advance(want, &wake);
      link_pump();

      if (grant == RETRO_LINK_UNBOUNDED || grant > end)
         grant = end;
      /* Never stand still: a grant at where we already are is a wake-up, and
       * asking again is how it is answered. */
      if (grant <= wire.now)
      {
         link_run_to(wire.now, feed);
         continue;
      }
      link_run_to(grant, feed);
   }

   /* Say where this frame ended, before the frame ends. Asking for where we
    * already stand is granted at once, so this never waits; what it does is
    * move the printer's published horizon to the frame's edge plus one.
    *
    * Without it the last promise of the frame sits wherever the last grain fell,
    * up to a grain short of the edge -- 140448 is not a multiple of 2048 -- and a
    * Game Boy whose own last rendezvous falls in that gap asks for a horizon the
    * printer never published until its NEXT frame starts. In free running that
    * is a pause; in a netplay session that holds every core at one frame (a
    * cable landing mid-game, NetplaySession._all_cores_at_link_boundary) it is a
    * deadlock, because the printer's next frame never comes.
    *
    * That holds with gambatte, whose frames start where the printer's do on the
    * bus. mGBA first calls in after a cable is made at its next timing event, up
    * to a grain into its frame, so its frame edges can sit before the printer's;
    * held at one frame WHILE cabled (a lead pulled mid-session), the printer can
    * then wait on it for ever. Making that impossible needs a promise further
    * ahead than a double-speed answer allows -- see LINK_HORIZON. */
   if (link_anchored)
      link_advance(wire.now, NULL);
}

static void link_attach(void)
{
   if (!link_have_iface || link_handle)
      return;
   link_handle = link_iface.attach(0, LINK_PROTOCOL, LINK_CLOCK_RATE);
   if (!link_handle)
   {
      log_msg(RETRO_LOG_WARN, "the frontend refused link port 0\n");
      return;
   }
   link_anchored = 0;
   link_peers = 0;
   link_self_id = 0;
   wire.published = 0;
   wire.pending_count = 0;
   link_refresh_peers();
   log_msg(RETRO_LOG_INFO, "attached to %s\n", LINK_PROTOCOL);
}

static void link_detach(void)
{
   if (!link_handle)
      return;
   link_iface.detach(link_handle);
   link_handle = NULL;
   link_peers = 0;
   link_anchored = 0;
}

/* ------------------------------------------------------------------------ */
/* Video                                                                     */
/* ------------------------------------------------------------------------ */

static void present(void)
{
   unsigned lines = printer.strip_lines;
   unsigned y, x;

   if (sent_any && sent_serial == printer.strip_serial)
   {
      video_cb(NULL, GBP_WIDTH, lines ? lines : 1, GBP_WIDTH * sizeof(uint32_t));
      return;
   }
   sent_any = 1;
   sent_serial = printer.strip_serial;

   if (lines == 0)
   {
      /* No paper out. A 1x1 frame says so unmistakably: a strip, however short,
       * is always 160 dots wide. */
      framebuffer[0] = 0x00FFFFFFu;
      video_cb(framebuffer, 1, 1, sizeof(uint32_t));
      return;
   }

   for (y = 0; y < lines; y++)
   {
      const uint8_t *src = printer.strip[y];
      uint32_t *dst = framebuffer + (size_t)y * GBP_WIDTH;
      for (x = 0; x < GBP_WIDTH; x++)
      {
         uint32_t g = src[x];
         dst[x] = (g << 16) | (g << 8) | g;
      }
   }
   video_cb(framebuffer, GBP_WIDTH, lines, GBP_WIDTH * sizeof(uint32_t));
}

/* ------------------------------------------------------------------------ */
/* libretro                                                                  */
/* ------------------------------------------------------------------------ */

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb)
{
   bool no_game = true;
   struct retro_log_callback logging;

   environ_cb = cb;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
      log_cb = logging.log;
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API void retro_init(void)
{
   memset(&link_iface, 0, sizeof(link_iface));
   link_have_iface = environ_cb(RETRO_ENVIRONMENT_GET_LINK_INTERFACE, &link_iface)
      || environ_cb(RETRO_ENVIRONMENT_GET_LINK_INTERFACE_FINAL, &link_iface);
   if (!link_have_iface)
      log_msg(RETRO_LOG_WARN, "the frontend has no link bus; nothing can print\n");
   gbp_init(&printer);
}

RETRO_API void retro_deinit(void)
{
   link_detach();
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name = CORE_NAME;
   info->library_version = CORE_VERSION;
   info->valid_extensions = "";
   info->need_fullpath = false;
   info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->timing.fps = 4194304.0 / 70224.0;
   /* The motor and the head (sound.h). Exactly 548.625 samples a frame. */
   info->timing.sample_rate = GBP_SOUND_RATE;
   info->geometry.base_width = GBP_WIDTH;
   info->geometry.base_height = 144;
   info->geometry.max_width = GBP_WIDTH;
   info->geometry.max_height = GBP_STRIP_MAX;
   info->geometry.aspect_ratio = 0.0f;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

RETRO_API void retro_reset(void)
{
   /* Switching a printer off and on clears its RAM and its parser; the paper
    * already out of the slot stays where it is. */
   uint16_t lines = printer.strip_lines;
   uint32_t serial = printer.strip_serial;
   uint32_t tears = printer.tear_count;
   static uint8_t strip[GBP_STRIP_MAX][GBP_WIDTH];

   memcpy(strip, printer.strip, sizeof(strip));
   gbp_init(&printer);
   memcpy(printer.strip, strip, sizeof(strip));
   printer.strip_lines = lines;
   printer.strip_serial = serial + 1;
   printer.tear_count = tears;
   wire.published = 0;
}

RETRO_API void retro_run(void)
{
   int feed, tear;
   int lx;

   input_poll_cb();
   feed = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) != 0;
   tear = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B) != 0;
   lx = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);

   if (tear && !prev_tear)
      gbp_tear(&printer, lx > 0 ? (unsigned)lx : GBP_STRIP_MAX);
   prev_tear = (uint8_t)tear;

   link_frame(feed);
   present();
   {
      unsigned n = gbp_sound_frame(&sound, &printer, wire.now, sound_buf);
      if (n && audio_batch_cb)
         audio_batch_cb(sound_buf, n);
   }
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Feed" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Tear" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Lines to tear" },
      { 0, 0, 0, 0, NULL }
   };
   (void)game;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_msg(RETRO_LOG_ERROR, "XRGB8888 is not supported\n");
      return false;
   }
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   gbp_init(&printer);
   gbp_sound_init(&sound);
   memset(&wire, 0, sizeof(wire));
   prev_tear = 0;
   sent_any = 0;
   link_attach();
   return true;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

RETRO_API void retro_unload_game(void)
{
   link_detach();
}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API size_t retro_serialize_size(void)
{
   return sizeof(core_state);
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
   core_state *s = (core_state *)data;
   if (size < sizeof(core_state))
      return false;
   memset(s, 0, sizeof(*s));
   s->magic = STATE_MAGIC;
   s->version = GBP_STATE_VERSION;
   s->size = (uint32_t)sizeof(core_state);
   s->prev_tear = prev_tear;
   s->wire = wire;
   s->sound = sound;
   s->printer = printer;
   return true;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
   const core_state *s = (const core_state *)data;
   if (size < sizeof(core_state) || s->magic != STATE_MAGIC
         || s->version != GBP_STATE_VERSION || s->size != sizeof(core_state))
      return false;
   prev_tear = s->prev_tear;
   wire = s->wire;
   sound = s->sound;
   printer = s->printer;
   /* The other end has been told nothing about this state yet. */
   wire.published = 0;
   sent_any = 0;
   return true;
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

RETRO_API void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}
