#include "nolf_video_plugin.h"

#include "bink_audio_decoder.h"
#include "bink_demuxer.h"
#include "bink_video_decoder.h"

#include <cstring>
#include <vector>

struct NolfVideoDecoder
{
    nolf::formats::BinkDemuxer demuxer;
    nolf::formats::BinkVideoDecoder* video = nullptr;
    nolf::formats::BinkAudioDecoder* audio = nullptr;
    uint32_t currentFrame = 0;
    uint32_t totalFrames = 0;
    std::vector<float> audioBuf;
};

static NolfVideoDecoder* bik_open(const uint8_t* data, size_t size)
{
    auto* dec = new (std::nothrow) NolfVideoDecoder();
    if (dec == nullptr)
    {
        return nullptr;
    }

    std::vector<uint8_t> buf(data, data + size);
    if (!dec->demuxer.Open(std::move(buf)))
    {
        delete dec;
        return nullptr;
    }

    auto& info = dec->demuxer.GetVideoInfo();
    dec->video = new nolf::formats::BinkVideoDecoder(
        info.width, info.height, info.revision, info.hasAlpha);
    dec->totalFrames = dec->demuxer.GetFrameCount();

    if (dec->demuxer.GetAudioTrackCount() > 0)
    {
        auto& track = dec->demuxer.GetAudioTrackInfo(0);
        dec->audio = new nolf::formats::BinkAudioDecoder(
            track.sampleRate, track.channels,
            track.codec == nolf::formats::BinkAudioCodec::DCT);
    }

    return dec;
}

static void bik_close(NolfVideoDecoder* dec)
{
    if (dec == nullptr)
    {
        return;
    }
    delete dec->audio;
    delete dec->video;
    delete dec;
}

static int bik_get_info(NolfVideoDecoder* dec, NolfVideoInfo* out)
{
    if (dec == nullptr || out == nullptr)
    {
        return -1;
    }
    auto& info = dec->demuxer.GetVideoInfo();
    out->width = info.width;
    out->height = info.height;
    out->frame_count = info.frameCount;
    out->fps = static_cast<float>(info.fpsDividend) / info.fpsDivisor;
    out->has_alpha = info.hasAlpha ? 1 : 0;

    if (dec->demuxer.GetAudioTrackCount() > 0)
    {
        auto& track = dec->demuxer.GetAudioTrackInfo(0);
        out->audio_sample_rate = track.sampleRate;
        out->audio_channels = track.channels;
        out->has_audio = 1;
    }
    else
    {
        out->audio_sample_rate = 0;
        out->audio_channels = 0;
        out->has_audio = 0;
    }
    out->reserved = 0;
    return 0;
}

static int bik_decode_video(NolfVideoDecoder* dec, uint8_t* rgba, size_t rgba_size)
{
    if (dec == nullptr || dec->currentFrame >= dec->totalFrames)
    {
        return -1;
    }

    nolf::formats::BinkFrameData fd;
    if (!dec->demuxer.ReadFrame(dec->currentFrame, fd))
    {
        return -1;
    }

    if (!dec->video->DecodeFrame(fd.videoData.data(), fd.videoData.size(),
                                 fd.isKeyframe))
    {
        return -1;
    }

    auto& frame = dec->video->GetFrame();
    size_t copySize = frame.rgba.size();
    if (copySize > rgba_size)
    {
        copySize = rgba_size;
    }
    std::memcpy(rgba, frame.rgba.data(), copySize);

    // Decode audio for this frame into internal buffer
    dec->audioBuf.clear();
    if (dec->audio != nullptr && !fd.audioPackets.empty() &&
        !fd.audioPackets[0].empty())
    {
        dec->audio->Decode(fd.audioPackets[0].data(),
                           fd.audioPackets[0].size(), dec->audioBuf);
    }

    dec->currentFrame++;
    return 0;
}

static size_t bik_decode_audio(NolfVideoDecoder* dec, float* pcm,
                               size_t max_floats)
{
    if (dec == nullptr || dec->audioBuf.empty())
    {
        return 0;
    }
    size_t toCopy = dec->audioBuf.size();
    if (toCopy > max_floats)
    {
        toCopy = max_floats;
    }
    std::memcpy(pcm, dec->audioBuf.data(), toCopy * sizeof(float));

    uint8_t channels = 1;
    if (dec->demuxer.GetAudioTrackCount() > 0)
    {
        channels = dec->demuxer.GetAudioTrackInfo(0).channels;
    }
    return (channels > 1) ? toCopy / channels : toCopy;
}

static int bik_seek(NolfVideoDecoder* dec, uint32_t frame_index)
{
    if (dec == nullptr || frame_index >= dec->totalFrames)
    {
        return -1;
    }
    dec->currentFrame = frame_index;
    dec->video->Reset();
    if (dec->audio != nullptr)
    {
        dec->audio->Reset();
    }
    // Decode all frames up to the target for correct inter-frame state
    for (uint32_t i = 0; i < frame_index; i++)
    {
        nolf::formats::BinkFrameData fd;
        dec->demuxer.ReadFrame(i, fd);
        dec->video->DecodeFrame(fd.videoData.data(), fd.videoData.size(),
                                fd.isKeyframe);
    }
    return 0;
}

static void bik_reset(NolfVideoDecoder* dec)
{
    if (dec == nullptr)
    {
        return;
    }
    dec->currentFrame = 0;
    dec->video->Reset();
    if (dec->audio != nullptr)
    {
        dec->audio->Reset();
    }
}

extern "C" NOLF_VIDEO_EXPORT const NolfVideoPlugin* nolf_video_plugin_info()
{
    static const NolfVideoPlugin plugin = {
        "play-bik",
        "1.0.0",
        "bik",
        bik_open,
        bik_close,
        bik_get_info,
        bik_decode_video,
        bik_decode_audio,
        bik_seek,
        bik_reset,
    };
    return &plugin;
}
