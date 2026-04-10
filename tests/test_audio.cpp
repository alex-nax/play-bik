#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bink_audio_decoder.h"
#include "bink_demuxer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace nolf::formats;

static bool FileExists(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (f != nullptr)
    {
        fclose(f);
        return true;
    }
    return false;
}

static constexpr const char* FOXPC = "nolf/Movies/foxpc.bik";

TEST_CASE("bink audio — 29-bit float decode")
{
    // Construct a known 29-bit float: exponent=15, mantissa=0, sign=0
    // IEEE: (0 << 31) | ((15 + 0x7C) << 23) | 0 = (139 << 23) = exponent 139
    // IEEE exponent 139 => 2^(139-127) = 2^12 = 4096.0
    uint32_t raw = 15; // exponent = 15, mantissa = 0, sign = 0
    float expected = 4096.0f;
    uint32_t ieee = ((15 + 0x7C) << 23);
    float result;
    std::memcpy(&result, &ieee, 4);
    CHECK(result == doctest::Approx(expected));
}

TEST_CASE("bink audio — band computation 44100 Hz stereo RDFT")
{
    BinkAudioDecoder dec(44100, 2, false);
    CHECK(dec.GetFrameLen() == 4096);
    CHECK(dec.GetOverlapLen() == 256);
}

TEST_CASE("bink audio — band computation 22050 Hz mono")
{
    BinkAudioDecoder dec(22050, 1, false);
    CHECK(dec.GetFrameLen() == 1024);
    CHECK(dec.GetOverlapLen() == 64);
}

TEST_CASE("bink audio — band computation 11025 Hz mono")
{
    BinkAudioDecoder dec(11025, 1, false);
    CHECK(dec.GetFrameLen() == 512);
    CHECK(dec.GetOverlapLen() == 32);
}

TEST_CASE("bink audio — decode foxpc first audio frame" *
          doctest::skip(!FileExists(FOXPC)))
{
    std::vector<uint8_t> fileData;
    FILE* f = fopen(FOXPC, "rb");
    REQUIRE(f != nullptr);
    fseek(f, 0, SEEK_END);
    fileData.resize(static_cast<size_t>(ftell(f)));
    fseek(f, 0, SEEK_SET);
    fread(fileData.data(), 1, fileData.size(), f);
    fclose(f);

    BinkDemuxer demux;
    REQUIRE(demux.Open(std::move(fileData)));
    CHECK(demux.GetAudioTrackCount() == 1);

    auto& track = demux.GetAudioTrackInfo(0);
    CHECK(track.sampleRate == 44100);
    CHECK(track.channels == 2);

    BinkAudioDecoder dec(track.sampleRate, track.channels,
                         track.codec == BinkAudioCodec::DCT);

    BinkFrameData frame;
    REQUIRE(demux.ReadFrame(0, frame));
    REQUIRE(!frame.audioPackets.empty());
    REQUIRE(!frame.audioPackets[0].empty());

    std::vector<float> pcm;
    size_t samplesPerCh = dec.Decode(frame.audioPackets[0].data(),
                                     frame.audioPackets[0].size(), pcm);

    CHECK(samplesPerCh > 0);
    CHECK(pcm.size() == samplesPerCh * 2); // stereo

    // Check no NaN/Inf
    bool hasNaN = false;
    for (float s : pcm)
    {
        if (std::isnan(s) || std::isinf(s))
        {
            hasNaN = true;
            break;
        }
    }
    CHECK_FALSE(hasNaN);
}

TEST_CASE("bink audio — decode all foxpc audio frames" *
          doctest::skip(!FileExists(FOXPC)))
{
    std::vector<uint8_t> fileData;
    FILE* f = fopen(FOXPC, "rb");
    REQUIRE(f != nullptr);
    fseek(f, 0, SEEK_END);
    fileData.resize(static_cast<size_t>(ftell(f)));
    fseek(f, 0, SEEK_SET);
    fread(fileData.data(), 1, fileData.size(), f);
    fclose(f);

    BinkDemuxer demux;
    REQUIRE(demux.Open(std::move(fileData)));

    auto& track = demux.GetAudioTrackInfo(0);
    BinkAudioDecoder dec(track.sampleRate, track.channels,
                         track.codec == BinkAudioCodec::DCT);

    std::vector<float> allPcm;
    uint32_t frameCount = demux.GetFrameCount();
    size_t totalSamples = 0;

    for (uint32_t i = 0; i < frameCount; i++)
    {
        BinkFrameData frame;
        REQUIRE(demux.ReadFrame(i, frame));
        if (frame.audioPackets.empty() || frame.audioPackets[0].empty())
        {
            continue;
        }

        std::vector<float> pcm;
        size_t n = dec.Decode(frame.audioPackets[0].data(),
                              frame.audioPackets[0].size(), pcm);
        totalSamples += n;

        // Check no NaN/Inf in this frame
        for (float s : pcm)
        {
            if (std::isnan(s) || std::isinf(s))
            {
                FAIL("NaN/Inf in frame " << i);
                break;
            }
        }
    }

    CHECK(totalSamples > 0);

    // foxpc: 141 frames at ~15fps = ~9.4 seconds at 44100 Hz
    // expect roughly 400k+ samples per channel
    CHECK(totalSamples > 100000);
}

TEST_CASE("bink audio — RMS energy is nonzero" *
          doctest::skip(!FileExists(FOXPC)))
{
    std::vector<uint8_t> fileData;
    FILE* f = fopen(FOXPC, "rb");
    REQUIRE(f != nullptr);
    fseek(f, 0, SEEK_END);
    fileData.resize(static_cast<size_t>(ftell(f)));
    fseek(f, 0, SEEK_SET);
    fread(fileData.data(), 1, fileData.size(), f);
    fclose(f);

    BinkDemuxer demux;
    REQUIRE(demux.Open(std::move(fileData)));

    auto& track = demux.GetAudioTrackInfo(0);
    BinkAudioDecoder dec(track.sampleRate, track.channels,
                         track.codec == BinkAudioCodec::DCT);

    // Decode first 10 frames and check RMS
    std::vector<float> pcm;
    for (uint32_t i = 0; i < std::min(10u, demux.GetFrameCount()); i++)
    {
        BinkFrameData frame;
        demux.ReadFrame(i, frame);
        if (!frame.audioPackets.empty() && !frame.audioPackets[0].empty())
        {
            dec.Decode(frame.audioPackets[0].data(),
                       frame.audioPackets[0].size(), pcm);
        }
    }

    double rms = 0.0;
    for (float s : pcm)
    {
        rms += static_cast<double>(s) * s;
    }
    rms = std::sqrt(rms / static_cast<double>(pcm.size()));

    // Should be nonzero (not silence)
    CHECK(rms > 0.0001);
}
