#pragma once

#include <cstdint>
#include <vector>

namespace nolf::formats
{

enum class BinkAudioCodec : uint8_t
{
    RDFT = 0,
    DCT = 1,
};

struct BinkAudioTrackInfo
{
    uint32_t sampleRate;
    uint16_t channels;
    BinkAudioCodec codec;
    uint16_t maxPacketSize;
    uint32_t trackId;
};

struct BinkVideoInfo
{
    uint32_t width;
    uint32_t height;
    uint32_t frameCount;
    uint32_t fpsDividend;
    uint32_t fpsDivisor;
    char revision;
    bool hasAlpha;
    bool grayscale;
};

struct BinkFrameData
{
    uint32_t frameIndex;
    bool isKeyframe;
    std::vector<std::vector<uint8_t>> audioPackets;
    std::vector<uint32_t> audioSampleCounts;
    std::vector<uint8_t> videoData;
};

class BinkDemuxer
{
  public:
    bool Open(std::vector<uint8_t> fileData);
    bool OpenFile(const char* path);

    const BinkVideoInfo& GetVideoInfo() const { return m_videoInfo; }
    uint32_t GetAudioTrackCount() const { return static_cast<uint32_t>(m_audioTracks.size()); }
    const BinkAudioTrackInfo& GetAudioTrackInfo(uint32_t trackIndex) const
    {
        return m_audioTracks[trackIndex];
    }

    uint32_t GetFrameCount() const { return static_cast<uint32_t>(m_frames.size()); }
    bool IsKeyframe(uint32_t frameIndex) const;
    bool ReadFrame(uint32_t frameIndex, BinkFrameData& out) const;

    float GetFrameDuration() const;
    float GetDuration() const;

  private:
    bool Parse();

    std::vector<uint8_t> m_data;
    BinkVideoInfo m_videoInfo{};
    std::vector<BinkAudioTrackInfo> m_audioTracks;

    struct FrameEntry
    {
        uint32_t offset;
        uint32_t size;
        bool keyframe;
    };
    std::vector<FrameEntry> m_frames;
};

} // namespace nolf::formats
