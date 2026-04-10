#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nolf::formats
{

class BinkAudioDecoder
{
  public:
    BinkAudioDecoder(uint32_t sampleRate, uint8_t channels, bool useDCT);

    // Decode one audio packet to float PCM (interleaved L/R for stereo).
    // Returns number of usable samples per channel.
    size_t Decode(const uint8_t* data, size_t size, std::vector<float>& out);

    void Reset();

    size_t GetFrameLen() const { return m_frameLen; }
    size_t GetOverlapLen() const { return m_overlapLen; }
    uint32_t GetSampleRate() const { return m_sampleRate; }
    uint8_t GetChannels() const { return m_channels; }

  private:
    struct BitReader
    {
        const uint8_t* data = nullptr;
        size_t size = 0;
        size_t pos = 0;
        uint32_t bits = 0;
        int avail = 0;

        void Init(const uint8_t* d, size_t s);
        uint32_t Read(int n);
        float ReadFloat29();
    };

    void ComputeBands(uint32_t effectiveRate);
    void BuildQuantTable();
    void DecodeCoeffs(BitReader& br, float* coeffs);

    void FFT(float* data, int n, bool inverse);
    void RDFT(float* coeffs, int n);

    uint32_t m_sampleRate;
    uint8_t m_channels;
    bool m_useDCT;
    size_t m_frameLen;
    size_t m_overlapLen;
    bool m_firstFrame;
    float m_root;

    std::vector<size_t> m_bands;
    std::vector<float> m_quantTable;
    std::vector<float> m_prevOverlap;
    std::vector<float> m_coeffs;
};

} // namespace nolf::formats
