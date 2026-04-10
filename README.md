# play-bik

Bink Video 1 decoder plugin for [NOLF Improved](https://github.com/alex-nax/nolf-improved). Builds as a dynamically-linked shared library implementing the `NolfVideoPlugin` C-ABI interface.

## Features

- **Bink 1 demuxer** — parses container headers, frame index, audio track metadata
- **Video decoder** — all 10 block types, custom IDCT, motion compensation, YUV-to-RGBA conversion
- **Audio decoder** — RDFT inverse transform, overlap-add windowing, stereo support
- **Plugin interface** — single exported C symbol `nolf_video_plugin_info` for runtime loading via dlopen/LoadLibrary

## Supported Files

All NOLF .bik files (revision 'i'):
- foxpc.bik (320x240, 15fps, 141 frames)
- LTLogo.bik (640x480, 14.63fps, 260 frames)
- LithLogo.bik (640x480, 15fps, 272 frames)
- SierraLogo.bik (800x400, 27.8fps, 201 frames)

## Build

```bash
cmake -B build && cmake --build build
```

Output: `libplay-bik.dylib` (macOS), `libplay-bik.so` (Linux), `play-bik.dll` (Windows)

### Build with tests

```bash
cmake -B build -DPLAY_BIK_BUILD_TESTS=ON
cmake --build build
cd build && ctest --output-on-failure
```

## Usage

The host application loads the shared library at runtime and calls `nolf_video_plugin_info()` to get the plugin vtable:

```c
#include "nolf_video_plugin.h"
#include <dlfcn.h>

void* handle = dlopen("libplay-bik.dylib", RTLD_LAZY);
NolfVideoPluginFn fn = dlsym(handle, "nolf_video_plugin_info");
const NolfVideoPlugin* plugin = fn();

NolfVideoDecoder* dec = plugin->open(file_data, file_size);
NolfVideoInfo info;
plugin->get_info(dec, &info);

uint8_t* rgba = malloc(info.width * info.height * 4);
while (plugin->decode_video(dec, rgba, info.width * info.height * 4) == 0) {
    // display rgba frame
    float pcm[48000];
    size_t samples = plugin->decode_audio(dec, pcm, 48000);
    // play pcm audio
}
plugin->close(dec);
dlclose(handle);
```

## License

LGPL-2.1 — this library was developed referencing FFmpeg's Bink decoders. It must be dynamically linked (not statically linked) by MIT/BSD projects.
