# gbprinter-libretro

A Game Boy Printer (MGB-007) as a libretro core, for [retroXR](https://github.com/RetroXR).

There is no CPU here and no game. The printer joins the frontend-hosted link bus
(`RETRO_ENVIRONMENT_GET_LINK_INTERFACE`, environment 94, proposed upstream in
libretro/RetroArch#19454) on the `gb-sio` wire that the retroXR builds of
**gambatte** and **mGBA** already speak. A Game Boy cabled to it prints exactly as
it would to the real printer; neither Game Boy core knows the far end is not
another Game Boy.

- **Video:** the paper strip hanging out of the slot, 160 dots wide, one row per
  dot line, oldest at the top. No paper is a 1x1 frame. A frame is sent only when
  the strip changes.
- **Audio:** 32768 Hz, the motor and the head while paper moves, silence
  otherwise (`src/sound.h`). Synthesised from the mechanism, not recorded:
  `render_wav out.wav` renders a print to listen to.
- **Input (port 0):** A = FEED (while held), B = TEAR on the press, tearing the
  oldest *LX* lines (left stick X as a plain count, or everything when 0).
- **Timing:** exactly one Game Boy frame (140448 link ticks) per `retro_run`, at
  the Game Boy's 59.7275 fps, so a netplay session can step it alongside the
  Game Boy it is cabled to.
- **Savestates:** complete (parser, RAM, print job, strip, wire).

Written from Pan Docs' printer chapter. The print speed and margin unit are
estimates; see `src/printer.h`.

## Build

```sh
cmake -S . -B build && cmake --build build --config Release
ctest --test-dir build -C Release      # Godot-free protocol + wire test
```

Android: add `-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake
-DANDROID_ABI=arm64-v8a -G Ninja`. Releases are cut by pushing a
`retroxr-gbprinter-libretro-vN` tag (`.github/workflows/retroxr-release.yml`).

## License

MIT. `include/libretro.h` is the libretro API header under its own MIT-style
notice; `src/link_interface.h` is kept byte-identical (bar its include guard) to
the copies in libretro-godot, gambatte and mGBA.
