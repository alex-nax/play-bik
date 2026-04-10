# Agent Instructions — play-bik

## Quick Start

1. Run `pwd` to confirm working directory
2. Read `claude-progress.md` for recent session history
3. Read `features.json` to find the next task
4. Build: `cmake -B build && cmake --build build`
5. The output is a shared library (`libplay-bik.so`/`.dylib`/`.dll`)
6. It exports one C symbol: `nolf_video_plugin_info`

## Key Rules

- **ALWAYS write a spec** in `docs/specs/` before implementation
- **ALWAYS write tests** before implementation
- Keep each source file under 1000 lines
- Commit frequently with descriptive messages

## Reference

- Specs and FFmpeg comparisons: `docs/specs/`
- Feature tracking: `features.json`
- Build config: `cmake.toml`
