# Agent Instructions — play-bik

1. Read `CLAUDE.md` for build commands and architecture
2. Build: `cmake -B build && cmake --build build`
3. The output is a shared library (`libplay-bik.so`/`.dylib`/`.dll`)
4. It exports one symbol: `nolf_video_plugin_info`
