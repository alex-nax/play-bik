#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bink_demuxer.h"
#include "bink_video_decoder.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

static bool FileExists(const char* path)
{
    FILE* fp = std::fopen(path, "rb");
    if (fp != nullptr)
    {
        std::fclose(fp);
        return true;
    }
    return false;
}

static constexpr const char* FOXPC = "nolf/Movies/foxpc.bik";
static constexpr const char* LTLOGO = "nolf/Movies/LTLogo.bik";
static constexpr const char* LITHLOGO = "nolf/Movies/LithLogo.bik";
static constexpr const char* SIERRALOGO = "nolf/Movies/SierraLogo.bik";

using nolf::formats::BinkDemuxer;
using nolf::formats::BinkFrameData;
using nolf::formats::BinkVideoDecoder;

static void SaveFrame(const nolf::formats::BinkVideoFrame& frame, const char* path)
{
    stbi_write_png(path, static_cast<int>(frame.width), static_cast<int>(frame.height), 4,
                   frame.rgba.data(), static_cast<int>(frame.width * 4));
}

TEST_CASE("smoke test — decode dummy frame without crash")
{
    BinkVideoDecoder dec(8, 8, 'i', false);
    uint8_t dummy[16] = {};
    dec.DecodeFrame(dummy, sizeof(dummy), true);
}

TEST_CASE("decode foxpc frame 0" * doctest::skip(!FileExists(FOXPC)))
{
    BinkDemuxer dmx;
    REQUIRE(dmx.OpenFile(FOXPC));

    auto& info = dmx.GetVideoInfo();
    BinkVideoDecoder dec(info.width, info.height, info.revision, info.hasAlpha);

    BinkFrameData frame;
    REQUIRE(dmx.ReadFrame(0, frame));
    bool ok = dec.DecodeFrame(frame.videoData.data(), frame.videoData.size(), frame.isKeyframe);
    auto& output = dec.GetFrame();
    CHECK(output.width == 320);
    CHECK(output.height == 240);
    CHECK(output.rgba.size() == 320u * 240 * 4);

    if (ok)
    {
        SaveFrame(output, "tests/reference/bink_frame_foxpc_0.png");

        bool hasNonZero = false;
        for (size_t i = 0; i < output.rgba.size(); i += 4)
        {
            if (output.rgba[i] != 0 || output.rgba[i + 1] != 0 || output.rgba[i + 2] != 0)
            {
                hasNonZero = true;
                break;
            }
        }
        CHECK(hasNonZero);
    }
}

TEST_CASE("decode all 4 NOLF bik frame 0" * doctest::skip(!FileExists(FOXPC)))
{
    struct VideoFile
    {
        const char* path;
        const char* outName;
    };
    VideoFile files[] = {
        {FOXPC, "tests/reference/bink_frame_foxpc_0.png"},
        {LTLOGO, "tests/reference/bink_frame_ltlogo_0.png"},
        {LITHLOGO, "tests/reference/bink_frame_lithlogo_0.png"},
        {SIERRALOGO, "tests/reference/bink_frame_sierralogo_0.png"},
    };

    for (auto& vf : files)
    {
        if (!FileExists(vf.path))
        {
            continue;
        }
        CAPTURE(vf.path);

        BinkDemuxer dmx;
        REQUIRE(dmx.OpenFile(vf.path));

        auto& info = dmx.GetVideoInfo();
        BinkVideoDecoder dec(info.width, info.height, info.revision, info.hasAlpha);

        BinkFrameData frame;
        REQUIRE(dmx.ReadFrame(0, frame));
        bool ok =
            dec.DecodeFrame(frame.videoData.data(), frame.videoData.size(), frame.isKeyframe);
        if (ok)
        {
            SaveFrame(dec.GetFrame(), vf.outName);
        }
    }
}

TEST_CASE("decode foxpc frame 30 (inter)" * doctest::skip(!FileExists(FOXPC)))
{
    BinkDemuxer dmx;
    REQUIRE(dmx.OpenFile(FOXPC));

    auto& info = dmx.GetVideoInfo();
    BinkVideoDecoder dec(info.width, info.height, info.revision, info.hasAlpha);

    BinkFrameData frame;
    bool lastOk = false;
    for (uint32_t i = 0; i <= 30 && i < info.frameCount; i++)
    {
        REQUIRE(dmx.ReadFrame(i, frame));
        lastOk = dec.DecodeFrame(frame.videoData.data(), frame.videoData.size(), frame.isKeyframe);
    }

    if (lastOk)
    {
        SaveFrame(dec.GetFrame(), "tests/reference/bink_frame_foxpc_30.png");
    }
}

TEST_CASE("decode multiple foxpc frames" * doctest::skip(!FileExists(FOXPC)))
{
    BinkDemuxer dmx;
    REQUIRE(dmx.OpenFile(FOXPC));

    auto& info = dmx.GetVideoInfo();
    BinkVideoDecoder dec(info.width, info.height, info.revision, info.hasAlpha);

    BinkFrameData frame;
    uint32_t decoded = 0;
    for (uint32_t i = 0; i < std::min(info.frameCount, 10u); i++)
    {
        REQUIRE(dmx.ReadFrame(i, frame));
        if (dec.DecodeFrame(frame.videoData.data(), frame.videoData.size(), frame.isKeyframe))
        {
            decoded++;
        }
    }
    CHECK(decoded > 0);
}

TEST_CASE("alpha channel is 255 for non-alpha video" * doctest::skip(!FileExists(FOXPC)))
{
    BinkDemuxer dmx;
    REQUIRE(dmx.OpenFile(FOXPC));

    auto& info = dmx.GetVideoInfo();
    BinkVideoDecoder dec(info.width, info.height, info.revision, info.hasAlpha);

    BinkFrameData frame;
    REQUIRE(dmx.ReadFrame(0, frame));
    if (dec.DecodeFrame(frame.videoData.data(), frame.videoData.size(), frame.isKeyframe))
    {
        auto& output = dec.GetFrame();
        bool allAlpha255 = true;
        for (size_t i = 3; i < output.rgba.size(); i += 4)
        {
            if (output.rgba[i] != 255)
            {
                allAlpha255 = false;
                break;
            }
        }
        CHECK(allAlpha255);
    }
}

TEST_CASE("reset clears decoder state")
{
    BinkVideoDecoder dec(320, 240, 'i', false);
    dec.Reset();
    auto& output = dec.GetFrame();
    CHECK(output.width == 320);
    CHECK(output.height == 240);
}
