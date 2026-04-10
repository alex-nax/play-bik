# CLAUDE.md — play-bik

## Project

Bink Video 1 decoder plugin for the NOLF Improved sourceport. Builds as a dynamically-linked shared library implementing the `NolfVideoPlugin` C-ABI interface. Loaded at runtime by the main project via dlopen/LoadLibrary.

## Build Commands

```bash
cmake -B build && cmake --build build                              # Build shared library
cmake -B build -DPLAY_BIK_BUILD_TESTS=ON && cmake --build build   # Build with tests
cd build && ctest --output-on-failure                              # Run tests
```

Build system uses **cmkr** (`cmake.toml` → auto-generates `CMakeLists.txt`).

## Development Workflow

1. **Orient**: Read `claude-progress.md` and `features.json`
2. **Spec**: Write `docs/specs/feature-NNN.md` before implementation
3. **Test**: Write failing tests first
4. **Implement**: Make tests pass
5. **Record**: Update `claude-progress.md`, commit

## Architecture

```
src/
├── bink_demuxer.*          # Bink 1 container parser
├── bink_video_decoder.*    # Bink Video 1 decoder (10 block types, IDCT, YUV→RGBA)
├── bink_audio_decoder.*    # Bink Audio decoder (RDFT, overlap-add)
├── bink_tables.*           # VLC and quantization tables
├── bink_quant_tables.inc   # Generated quant table data
└── plugin.cpp              # NolfVideoPlugin C-ABI wrapper

include/
└── nolf_video_plugin.h     # Plugin interface header (from main project)

tests/
├── test_demuxer.cpp        # Container parsing tests
├── test_video.cpp          # Video decoder tests (needs stb_image_write)
└── test_audio.cpp          # Audio decoder tests

docs/specs/
├── feature-116-bink-video-decoder.md   # Video decoder spec + FFmpeg comparison
├── feature-117-bink-audio-decoder.md   # Audio decoder spec
└── feature-117-bink-audio-comparison.md # Audio FFmpeg comparison
```

## Key Reference

| What | Where |
|------|-------|
| Feature tracking | `features.json` |
| Session history | `claude-progress.md` |
| Video decoder spec | `docs/specs/feature-116-bink-video-decoder.md` |
| Audio decoder spec | `docs/specs/feature-117-bink-audio-decoder.md` |
| FFmpeg comparison (video) | `docs/specs/feature-116-bink-video-decoder.md` (section "FFmpeg Comparison") |
| FFmpeg comparison (audio) | `docs/specs/feature-117-bink-audio-comparison.md` |

## License

LGPL-2.1 — developed referencing FFmpeg's Bink decoders. Must be dynamically linked by the main MIT/BSD project.

## Known Issues

- Audio output peak ~3700x too high (normalization scale mismatch between our 1/N-normalized FFT and FFmpeg's unnormalized RDFT with scale=0.5)
- DCT audio mode not implemented (NOLF only uses RDFT)
