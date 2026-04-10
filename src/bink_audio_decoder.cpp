#include "bink_audio_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nolf::formats
{

static constexpr uint32_t kCritFreqs[25] = {100,  200,  300,  400,  510,  630,  770,
                                            920,  1080, 1270, 1480, 1720, 2000, 2320,
                                            2700, 3150, 3700, 4400, 5300, 6400, 7700,
                                            9500, 12000, 15500, 24500};

static constexpr uint32_t kRLELengths[16] = {2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64};

static constexpr float kPI = 3.14159265358979323846f;

// --- BitReader ---

void BinkAudioDecoder::BitReader::Init(const uint8_t* d, size_t s)
{
    data = d;
    size = s;
    pos = 0;
    bits = 0;
    avail = 0;
}

uint32_t BinkAudioDecoder::BitReader::Read(int n)
{
    uint32_t result = 0;
    int got = 0;
    while (got < n)
    {
        if (avail == 0)
        {
            if (pos + 4 <= size)
            {
                bits = static_cast<uint32_t>(data[pos]) |
                       (static_cast<uint32_t>(data[pos + 1]) << 8) |
                       (static_cast<uint32_t>(data[pos + 2]) << 16) |
                       (static_cast<uint32_t>(data[pos + 3]) << 24);
                pos += 4;
            }
            else if (pos < size)
            {
                bits = 0;
                for (size_t i = 0; i < size - pos; i++)
                {
                    bits |= static_cast<uint32_t>(data[pos + i]) << (i * 8);
                }
                pos = size;
            }
            else
            {
                bits = 0;
            }
            avail = 32;
        }
        int take = std::min(n - got, avail);
        uint32_t mask = (take >= 32) ? 0xFFFFFFFFu : ((1u << take) - 1);
        result |= (bits & mask) << got;
        bits >>= (take & 31);
        avail -= take;
        got += take;
    }
    return result;
}

float BinkAudioDecoder::BitReader::ReadFloat29()
{
    int power = static_cast<int>(Read(5));
    float f = std::ldexp(static_cast<float>(Read(23)), power - 23);
    if (Read(1) != 0)
    {
        f = -f;
    }
    return f;
}

// --- Constructor ---

BinkAudioDecoder::BinkAudioDecoder(uint32_t sampleRate, uint8_t channels, bool useDCT)
    : m_sampleRate(sampleRate), m_channels(channels), m_useDCT(useDCT), m_firstFrame(true)
{
    int frameLenBits;
    if (sampleRate < 22050)
    {
        frameLenBits = 9;
    }
    else if (sampleRate < 44100)
    {
        frameLenBits = 10;
    }
    else
    {
        frameLenBits = 11;
    }

    uint32_t effectiveRate = sampleRate;
    if (!m_useDCT && m_channels == 2)
    {
        effectiveRate = sampleRate * m_channels;
        frameLenBits += 1;
    }

    m_frameLen = static_cast<size_t>(1) << frameLenBits;
    m_overlapLen = m_frameLen / 16;

    ComputeBands(effectiveRate);
    BuildQuantTable();

    m_prevOverlap.resize(m_frameLen, 0.0f);
    // +2 extra floats for Nyquist rearrangement
    m_coeffs.resize(m_frameLen + 2, 0.0f);
}

void BinkAudioDecoder::ComputeBands(uint32_t effectiveRate)
{
    uint32_t sampleRateHalf = (effectiveRate + 1) / 2;

    // Determine number of bands (FFmpeg: break when Nyquist <= critical freq)
    size_t numBands = 1;
    for (size_t i = 1; i <= 25; i++)
    {
        if (sampleRateHalf <= kCritFreqs[i - 1])
        {
            break;
        }
        numBands = i + 1;
    }
    if (numBands > 25)
    {
        numBands = 25;
    }

    m_bands.resize(numBands + 1);
    m_bands[0] = 2;
    for (size_t i = 1; i < numBands; i++)
    {
        m_bands[i] = (kCritFreqs[i - 1] * m_frameLen / sampleRateHalf) & ~static_cast<size_t>(1);
    }
    m_bands[numBands] = m_frameLen;
}

void BinkAudioDecoder::BuildQuantTable()
{
    float root;
    if (m_useDCT)
    {
        root = std::sqrt(static_cast<float>(m_frameLen)) / 32768.0f;
    }
    else
    {
        root = 2.0f / (std::sqrt(static_cast<float>(m_frameLen)) * 32768.0f);
    }
    m_root = root;

    m_quantTable.resize(96);
    for (int i = 0; i < 96; i++)
    {
        m_quantTable[i] = std::exp(static_cast<float>(i) * 0.15289164787221953823f) * root;
    }
}

// --- Coefficient decoding (non-version-b, matching FFmpeg's decode_block) ---

void BinkAudioDecoder::DecodeCoeffs(BitReader& br, float* coeffs)
{
    // First two coefficients scaled by root
    coeffs[0] = br.ReadFloat29() * m_root;
    coeffs[1] = br.ReadFloat29() * m_root;

    // Read per-band quantizer indices
    size_t numBands = m_bands.size() - 1;
    std::vector<float> quant(numBands);
    for (size_t i = 0; i < numBands; i++)
    {
        uint32_t qIdx = br.Read(8);
        quant[i] = m_quantTable[std::min(qIdx, 95u)];
    }

    // Parse coefficients (matching FFmpeg's band tracking)
    size_t k = 0;
    float q = quant[0];
    size_t i = 2;

    while (i < m_frameLen)
    {
        size_t j;
        if (br.Read(1) != 0)
        {
            uint32_t rleIdx = br.Read(4);
            j = i + kRLELengths[rleIdx] * 8;
        }
        else
        {
            j = i + 8;
        }
        j = std::min(j, m_frameLen);

        uint32_t width = br.Read(4);
        if (width == 0)
        {
            std::memset(coeffs + i, 0, (j - i) * sizeof(float));
            i = j;
            while (m_bands[k] < i && k < numBands)
            {
                q = quant[k++];
            }
        }
        else
        {
            while (i < j)
            {
                if (m_bands[k] == i && k < numBands)
                {
                    q = quant[k++];
                }
                uint32_t coeff = br.Read(static_cast<int>(width));
                if (coeff != 0)
                {
                    uint32_t sign = br.Read(1);
                    coeffs[i] = (sign != 0) ? -q * static_cast<float>(coeff)
                                            : q * static_cast<float>(coeff);
                }
                else
                {
                    coeffs[i] = 0.0f;
                }
                i++;
            }
        }
    }
}

// --- FFT (radix-2 Cooley-Tukey, in-place, interleaved real/imag) ---

void BinkAudioDecoder::FFT(float* data, int n, bool inverse)
{
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; i++)
    {
        int bit = n >> 1;
        while ((j & bit) != 0)
        {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j)
        {
            std::swap(data[2 * i], data[2 * j]);
            std::swap(data[2 * i + 1], data[2 * j + 1]);
        }
    }

    float sign = inverse ? 1.0f : -1.0f;
    for (int len = 2; len <= n; len <<= 1)
    {
        float angle = sign * 2.0f * kPI / static_cast<float>(len);
        float wRe = std::cos(angle);
        float wIm = std::sin(angle);

        for (int i = 0; i < n; i += len)
        {
            float curRe = 1.0f, curIm = 0.0f;
            int half = len / 2;
            for (int j = 0; j < half; j++)
            {
                int u = i + j;
                int v = i + j + half;
                float tRe = data[2 * v] * curRe - data[2 * v + 1] * curIm;
                float tIm = data[2 * v] * curIm + data[2 * v + 1] * curRe;
                data[2 * v] = data[2 * u] - tRe;
                data[2 * v + 1] = data[2 * u + 1] - tIm;
                data[2 * u] += tRe;
                data[2 * u + 1] += tIm;
                float newRe = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = newRe;
            }
        }
    }
}

// --- Inverse RDFT ---

void BinkAudioDecoder::RDFT(float* coeffs, int n)
{
    // coeffs[0..n+1] has n/2+1 complex values (half-spectrum, after rearrangement).
    // Build full N complex spectrum using Hermitian symmetry, apply N-point IFFT.
    int halfN = n / 2;
    std::vector<float> z(static_cast<size_t>(n) * 2, 0.0f);

    // Copy half-spectrum: Z[k] = coeffs[2k] + j*coeffs[2k+1]
    for (int k = 0; k <= halfN; k++)
    {
        z[static_cast<size_t>(2 * k)] = coeffs[2 * k];
        z[static_cast<size_t>(2 * k + 1)] = coeffs[2 * k + 1];
    }

    // Mirror: Z[N-k] = conj(Z[k])
    for (int k = 1; k < halfN; k++)
    {
        z[static_cast<size_t>(2 * (n - k))] = z[static_cast<size_t>(2 * k)];
        z[static_cast<size_t>(2 * (n - k) + 1)] = -z[static_cast<size_t>(2 * k + 1)];
    }

    // N-point inverse FFT (includes 1/N normalization)
    FFT(z.data(), n, true);

    // Our FFT divides by N; FFmpeg uses unnormalized * scale(0.5).
    // To match: multiply by N * 0.5 = N/2.
    float scale = static_cast<float>(n) * 0.5f;
    for (int i = 0; i < n; i++)
    {
        coeffs[i] = z[static_cast<size_t>(2 * i)] * scale;
    }
}

// --- Decode ---

void BinkAudioDecoder::Reset()
{
    std::fill(m_prevOverlap.begin(), m_prevOverlap.end(), 0.0f);
    m_firstFrame = true;
}

size_t BinkAudioDecoder::Decode(const uint8_t* data, size_t size, std::vector<float>& out)
{
    if (size == 0)
    {
        return 0;
    }

    BitReader br;
    br.Init(data, size);

    // Skip 32-bit "reported size" at start of packet (matches FFmpeg)
    br.Read(32);

    size_t channelsToProcess = m_useDCT ? m_channels : 1u;
    size_t frameLen = m_frameLen;
    size_t overlapLen = m_overlapLen;
    size_t totalPerCh = 0;

    // Decode all frames in the packet (a packet may contain multiple frames)
    size_t totalBits = size * 8;
    while ((br.pos * 8 - static_cast<size_t>(br.avail)) + 32 < totalBits)
    {
        std::vector<float> decoded(frameLen + 2, 0.0f);

        for (size_t ch = 0; ch < channelsToProcess; ch++)
        {
            float* coeffs = decoded.data();
            DecodeCoeffs(br, coeffs);

            if (m_useDCT)
            {
                coeffs[0] /= 0.5f;
                // STUB(F117) — inverse DCT-III not implemented
            }
            else
            {
                for (size_t i = 2; i < frameLen; i += 2)
                {
                    coeffs[i + 1] *= -1.0f;
                }
                coeffs[frameLen] = coeffs[1];
                coeffs[frameLen + 1] = 0.0f;
                coeffs[1] = 0.0f;

                RDFT(coeffs, static_cast<int>(frameLen));
            }
        }

        // Overlap-add
        size_t channels = m_useDCT ? m_channels : 1u;
        for (size_t ch = 0; ch < channels; ch++)
        {
            if (!m_firstFrame)
            {
                size_t count = overlapLen * channels;
                size_t j = ch;
                for (size_t i = 0; i < overlapLen; i++, j += channels)
                {
                    decoded[i] = (m_prevOverlap[i] * static_cast<float>(count - j) +
                                  decoded[i] * static_cast<float>(j)) /
                                 static_cast<float>(count);
                }
            }
            std::memcpy(m_prevOverlap.data(),
                         decoded.data() + frameLen - overlapLen,
                         overlapLen * sizeof(float));
        }
        m_firstFrame = false;

        // Append usable samples
        size_t outputLen = frameLen - overlapLen;
        size_t startIdx = out.size();
        out.resize(startIdx + outputLen);
        std::memcpy(out.data() + startIdx, decoded.data(), outputLen * sizeof(float));

        if (!m_useDCT && m_channels == 2)
        {
            totalPerCh += outputLen / 2;
        }
        else
        {
            totalPerCh += outputLen;
        }

        // Align to 32-bit boundary between frames
        size_t bitCount = br.pos * 8 - static_cast<size_t>(br.avail);
        if ((bitCount & 0x1F) != 0)
        {
            br.Read(32 - static_cast<int>(bitCount & 0x1F));
        }
    }

    return totalPerCh;
}

} // namespace nolf::formats
