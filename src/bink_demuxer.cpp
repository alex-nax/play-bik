#include "bink_demuxer.h"

#include <cstdio>
#include <cstring>

namespace nolf::formats
{

static uint16_t ReadLE16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

static uint32_t ReadLE32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool BinkDemuxer::Open(std::vector<uint8_t> fileData)
{
    m_data = std::move(fileData);
    m_videoInfo = {};
    m_audioTracks.clear();
    m_frames.clear();
    return Parse();
}

bool BinkDemuxer::OpenFile(const char* path)
{
    FILE* fp = std::fopen(path, "rb");
    if (!fp)
        return false;
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    if (sz <= 0)
    {
        std::fclose(fp);
        return false;
    }
    std::fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (std::fread(buf.data(), 1, buf.size(), fp) != buf.size())
    {
        std::fclose(fp);
        return false;
    }
    std::fclose(fp);
    return Open(std::move(buf));
}

bool BinkDemuxer::Parse()
{
    if (m_data.size() < 44)
        return false;

    const uint8_t* d = m_data.data();

    if (d[0] != 'B' || d[1] != 'I' || d[2] != 'K')
        return false;

    m_videoInfo.revision = static_cast<char>(d[3]);
    uint32_t fileSize = ReadLE32(d + 4) + 8;
    m_videoInfo.frameCount = ReadLE32(d + 8);
    uint32_t frameCount2 = ReadLE32(d + 16);
    m_videoInfo.width = ReadLE32(d + 20);
    m_videoInfo.height = ReadLE32(d + 24);
    m_videoInfo.fpsDividend = ReadLE32(d + 28);
    m_videoInfo.fpsDivisor = ReadLE32(d + 32);
    uint32_t videoFlags = ReadLE32(d + 36);
    uint32_t audioTrackCount = ReadLE32(d + 40);

    (void)fileSize;
    (void)frameCount2;

    m_videoInfo.hasAlpha = (videoFlags & (1u << 20)) != 0;
    m_videoInfo.grayscale = (videoFlags & (1u << 17)) != 0;

    if (m_videoInfo.frameCount == 0 || m_videoInfo.width == 0 || m_videoInfo.height == 0)
        return false;
    if (m_videoInfo.fpsDivisor == 0)
        return false;
    if (audioTrackCount > 256)
        return false;

    size_t pos = 44;
    size_t audioHeaderSize = static_cast<size_t>(audioTrackCount) * 12;
    if (pos + audioHeaderSize > m_data.size())
        return false;

    m_audioTracks.resize(audioTrackCount);

    for (uint32_t i = 0; i < audioTrackCount; i++)
    {
        m_audioTracks[i].maxPacketSize = ReadLE16(d + pos);
        m_audioTracks[i].channels = ReadLE16(d + pos + 2);
        pos += 4;
    }
    for (uint32_t i = 0; i < audioTrackCount; i++)
    {
        m_audioTracks[i].sampleRate = ReadLE16(d + pos);
        uint16_t codecFlags = ReadLE16(d + pos + 2);
        m_audioTracks[i].codec =
            (codecFlags & (1u << 12)) ? BinkAudioCodec::DCT : BinkAudioCodec::RDFT;
        pos += 4;
    }
    for (uint32_t i = 0; i < audioTrackCount; i++)
    {
        m_audioTracks[i].trackId = ReadLE32(d + pos);
        pos += 4;
    }

    uint32_t indexCount = m_videoInfo.frameCount + 1;
    size_t indexSize = static_cast<size_t>(indexCount) * 4;
    if (pos + indexSize > m_data.size())
        return false;

    m_frames.resize(m_videoInfo.frameCount);
    for (uint32_t i = 0; i < indexCount; i++)
    {
        uint32_t raw = ReadLE32(d + pos + i * 4);
        bool kf = (raw & 1) != 0;
        uint32_t offset = raw & ~1u;

        if (i < m_videoInfo.frameCount)
        {
            m_frames[i].offset = offset;
            m_frames[i].keyframe = kf;
        }

        if (i > 0)
        {
            uint32_t prevOffset = m_frames[i - 1].offset;
            if (offset < prevOffset)
                return false;
            m_frames[i - 1].size = offset - prevOffset;
        }
    }

    for (uint32_t i = 0; i < m_videoInfo.frameCount; i++)
    {
        if (m_frames[i].offset + m_frames[i].size > m_data.size())
            return false;
    }

    return true;
}

bool BinkDemuxer::IsKeyframe(uint32_t frameIndex) const
{
    if (frameIndex >= m_frames.size())
        return false;
    return m_frames[frameIndex].keyframe;
}

bool BinkDemuxer::ReadFrame(uint32_t frameIndex, BinkFrameData& out) const
{
    if (frameIndex >= m_frames.size())
        return false;

    const auto& entry = m_frames[frameIndex];
    out.frameIndex = frameIndex;
    out.isKeyframe = entry.keyframe;
    out.audioPackets.clear();
    out.audioSampleCounts.clear();
    out.videoData.clear();

    const uint8_t* frameStart = m_data.data() + entry.offset;
    size_t remaining = entry.size;
    size_t off = 0;

    uint32_t trackCount = static_cast<uint32_t>(m_audioTracks.size());
    out.audioPackets.resize(trackCount);
    out.audioSampleCounts.resize(trackCount, 0);

    for (uint32_t t = 0; t < trackCount; t++)
    {
        if (off + 4 > remaining)
            return false;
        uint32_t audioSize = ReadLE32(frameStart + off);
        off += 4;

        if (audioSize > 0)
        {
            if (off + audioSize > remaining)
                return false;

            if (audioSize >= 4)
            {
                out.audioSampleCounts[t] = ReadLE32(frameStart + off);
                out.audioPackets[t].assign(frameStart + off, frameStart + off + audioSize);
            }
            off += audioSize;
        }
    }

    if (off < remaining)
    {
        out.videoData.assign(frameStart + off, frameStart + off + (remaining - off));
    }

    return true;
}

float BinkDemuxer::GetFrameDuration() const
{
    if (m_videoInfo.fpsDividend == 0)
        return 0.0f;
    return static_cast<float>(m_videoInfo.fpsDivisor) /
           static_cast<float>(m_videoInfo.fpsDividend);
}

float BinkDemuxer::GetDuration() const
{
    return GetFrameDuration() * static_cast<float>(m_videoInfo.frameCount);
}

} // namespace nolf::formats
