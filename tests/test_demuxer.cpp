#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bink_demuxer.h"

#include <cmath>
#include <cstdio>

static bool FileExists(const char* path)
{
    FILE* fp = std::fopen(path, "rb");
    if (fp)
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

using nolf::formats::BinkAudioCodec;
using nolf::formats::BinkDemuxer;
using nolf::formats::BinkFrameData;

TEST_CASE("reject empty data")
{
    BinkDemuxer dmx;
    std::vector<uint8_t> empty;
    CHECK_FALSE(dmx.Open(std::move(empty)));
}

TEST_CASE("reject truncated header")
{
    BinkDemuxer dmx;
    std::vector<uint8_t> small(20, 0);
    small[0] = 'B';
    small[1] = 'I';
    small[2] = 'K';
    CHECK_FALSE(dmx.Open(std::move(small)));
}

TEST_CASE("reject wrong signature")
{
    BinkDemuxer dmx;
    std::vector<uint8_t> bad(64, 0);
    bad[0] = 'R';
    bad[1] = 'I';
    bad[2] = 'F';
    bad[3] = 'F';
    CHECK_FALSE(dmx.Open(std::move(bad)));
}

TEST_CASE("reject BIK2 signature")
{
    BinkDemuxer dmx;
    std::vector<uint8_t> bik2(64, 0);
    bik2[0] = 'K';
    bik2[1] = 'B';
    bik2[2] = '2';
    CHECK_FALSE(dmx.Open(std::move(bik2)));
}

TEST_CASE("parse foxpc.bik header" * doctest::skip(!FileExists(FOXPC)))
{
    BinkDemuxer dmx;
    REQUIRE(dmx.OpenFile(FOXPC));

    auto& info = dmx.GetVideoInfo();
    CHECK(info.width == 320);
    CHECK(info.height == 240);
    CHECK(info.frameCount == 141);
    CHECK(info.revision == 'i');
    CHECK(info.fpsDividend == 15);
    CHECK(info.fpsDivisor == 1);
    CHECK_FALSE(info.hasAlpha);
    CHECK_FALSE(info.grayscale);
    CHECK(dmx.GetFrameCount() == 141);
}

TEST_CASE("parse LTLogo.bik header" * doctest::skip(!FileExists(LTLOGO)))
{
    BinkDemuxer dmx;
    REQUIRE(dmx.OpenFile(LTLOGO));

    auto& info = dmx.GetVideoInfo();
    CHECK(info.width == 640);
    CHECK(info.height == 480);
    CHECK(info.frameCount == 260);
    CHECK(info.revision == 'i');
    CHECK(info.fpsDividend == 600);
    CHECK(info.fpsDivisor == 41);
}

TEST_CASE("parse SierraLogo.bik header" * doctest::skip(!FileExists(SIERRALOGO)))
{
    BinkDemuxer dmx;
    REQUIRE(dmx.OpenFile(SIERRALOGO));

    auto& info = dmx.GetVideoInfo();
    CHECK(info.width == 800);
    CHECK(info.height == 400);
    CHECK(info.frameCount == 201);
}

TEST_CASE("audio track info" * doctest::skip(!FileExists(FOXPC)))
{
    BinkDemuxer dmx;
    REQUIRE(dmx.OpenFile(FOXPC));

    CHECK(dmx.GetAudioTrackCount() == 1);
    auto& track = dmx.GetAudioTrackInfo(0);
    CHECK(track.sampleRate == 44100);
    CHECK(track.channels == 2);
    CHECK(track.codec == BinkAudioCodec::RDFT);
}

TEST_CASE("frame 0 is keyframe" * doctest::skip(!FileExists(FOXPC)))
{
    BinkDemuxer dmx;
    REQUIRE(dmx.OpenFile(FOXPC));
    CHECK(dmx.IsKeyframe(0));
}

TEST_CASE("read frame 0" * doctest::skip(!FileExists(FOXPC)))
{
    BinkDemuxer dmx;
    REQUIRE(dmx.OpenFile(FOXPC));

    BinkFrameData frame;
    REQUIRE(dmx.ReadFrame(0, frame));
    CHECK(frame.frameIndex == 0);
    CHECK(frame.isKeyframe);
    CHECK_FALSE(frame.videoData.empty());
    CHECK(frame.audioPackets.size() == 1);
    CHECK_FALSE(frame.audioPackets[0].empty());
    CHECK(frame.audioSampleCounts[0] > 0);
}

TEST_CASE("read frame 1 is not keyframe" * doctest::skip(!FileExists(FOXPC)))
{
    BinkDemuxer dmx;
    REQUIRE(dmx.OpenFile(FOXPC));

    BinkFrameData frame;
    REQUIRE(dmx.ReadFrame(1, frame));
    CHECK_FALSE(frame.isKeyframe);
    CHECK_FALSE(frame.videoData.empty());
}

TEST_CASE("read all foxpc frames" * doctest::skip(!FileExists(FOXPC)))
{
    BinkDemuxer dmx;
    REQUIRE(dmx.OpenFile(FOXPC));

    BinkFrameData frame;
    for (uint32_t i = 0; i < dmx.GetFrameCount(); i++)
    {
        REQUIRE(dmx.ReadFrame(i, frame));
        CHECK_FALSE(frame.videoData.empty());
    }
}

TEST_CASE("audio prefill on frame 0" * doctest::skip(!FileExists(FOXPC)))
{
    BinkDemuxer dmx;
    REQUIRE(dmx.OpenFile(FOXPC));

    BinkFrameData f0, f1;
    REQUIRE(dmx.ReadFrame(0, f0));
    REQUIRE(dmx.ReadFrame(1, f1));
    CHECK(f0.audioSampleCounts[0] > f1.audioSampleCounts[0]);
}

TEST_CASE("duration calculation" * doctest::skip(!FileExists(FOXPC)))
{
    BinkDemuxer dmx;
    REQUIRE(dmx.OpenFile(FOXPC));

    float duration = dmx.GetDuration();
    CHECK(std::fabs(duration - 9.4f) < 0.1f);
}

TEST_CASE("out of range frame returns false")
{
    BinkDemuxer dmx;
    BinkFrameData frame;
    CHECK_FALSE(dmx.ReadFrame(999999, frame));
    CHECK_FALSE(dmx.IsKeyframe(999999));
}

TEST_CASE("all four NOLF bik files parse" * doctest::skip(!FileExists(FOXPC)))
{
    const char* files[] = {FOXPC, LTLOGO, LITHLOGO, SIERRALOGO};
    for (const char* path : files)
    {
        if (!FileExists(path))
        {
            continue;
        }
        BinkDemuxer dmx;
        CAPTURE(path);
        REQUIRE(dmx.OpenFile(path));
        CHECK(dmx.GetVideoInfo().frameCount > 0);
        CHECK(dmx.GetAudioTrackCount() == 1);
        CHECK(dmx.GetAudioTrackInfo(0).sampleRate == 44100);

        BinkFrameData frame;
        REQUIRE(dmx.ReadFrame(0, frame));
        CHECK_FALSE(frame.videoData.empty());
    }
}
