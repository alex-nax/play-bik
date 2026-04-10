# CLAUDE.md — play-bik

## Project

Bink video/audio decoder plugin for the NOLF Improved sourceport. Builds as a dynamically-linked shared library implementing the `NolfVideoPlugin` C-ABI interface. Loaded at runtime by the main project via dlopen/LoadLibrary.

## Build Commands

```bash
cmake -B build && cmake --build build                     # Build shared library
cmake -B build -DPLAY_BIK_BUILD_TESTS=ON && cmake --build build  # Build with tests
cd build && ctest --output-on-failure                       # Run tests
```

Build system uses **cmkr** (`cmake.toml` → auto-generates `CMakeLists.txt`).

## Architecture

- `src/bink_demuxer.*` — Bink 1 container parser (frame index, audio track info)
- `src/bink_video_decoder.*` — Bink Video 1 decoder (DCT, motion compensation, YUV→RGBA)
- `src/bink_audio_decoder.*` — Bink Audio decoder (RDFT inverse, overlap-add)
- `src/bink_tables.*` — Precomputed quantization and VLC tables
- `src/plugin.cpp` — NolfVideoPlugin C-ABI wrapper
- `include/nolf_video_plugin.h` — Plugin interface header (from main project)

## License

LGPL — this library was developed referencing FFmpeg's LGPL Bink decoders. It is dynamically linked (not statically linked) by the main MIT/BSD project.
