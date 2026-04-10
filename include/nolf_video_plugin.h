/*
 * nolf_video_plugin.h — Video decoder plugin C-ABI interface
 *
 * This header defines the stable ABI that video decoder plugins must implement.
 * Plugins are loaded at runtime via dlopen/LoadLibrary. No C++ types cross the
 * boundary — only plain C structs, function pointers, and primitive types.
 *
 * To create a plugin: implement all functions in NolfVideoPlugin and export
 * a function named "nolf_video_plugin_info" that returns a pointer to a
 * static NolfVideoPlugin struct.
 */

#ifndef NOLF_VIDEO_PLUGIN_H
#define NOLF_VIDEO_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define NOLF_VIDEO_EXPORT __declspec(dllexport)
#else
#define NOLF_VIDEO_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NolfVideoDecoder NolfVideoDecoder;

typedef struct
{
    uint32_t width;
    uint32_t height;
    uint32_t frame_count;
    float fps;
    uint32_t audio_sample_rate;
    uint8_t audio_channels;
    uint8_t has_audio;
    uint8_t has_alpha;
    uint8_t reserved;
} NolfVideoInfo;

typedef struct
{
    const char* name;
    const char* version;
    const char* extensions; /* semicolon-separated, e.g. "bik;bk2" */

    NolfVideoDecoder* (*open)(const uint8_t* data, size_t size);
    void (*close)(NolfVideoDecoder* dec);
    int (*get_info)(NolfVideoDecoder* dec, NolfVideoInfo* out);

    /* Decode next video frame to RGBA. Returns 0 on success, -1 on error/EOF. */
    int (*decode_video)(NolfVideoDecoder* dec, uint8_t* rgba, size_t rgba_size);

    /* Decode audio for current frame. Returns samples per channel written.
     * Output is interleaved float PCM (L R L R...) for stereo. */
    size_t (*decode_audio)(NolfVideoDecoder* dec, float* pcm, size_t max_floats);

    int (*seek)(NolfVideoDecoder* dec, uint32_t frame_index);
    void (*reset)(NolfVideoDecoder* dec);
} NolfVideoPlugin;

#define NOLF_VIDEO_PLUGIN_SYMBOL "nolf_video_plugin_info"
typedef const NolfVideoPlugin* (*NolfVideoPluginFn)(void);

#ifdef __cplusplus
}
#endif

#endif /* NOLF_VIDEO_PLUGIN_H */
