#include "bink_video_decoder.h"

#include "bink_tables.h"

#include <algorithm>
#include <cstring>

namespace nolf::formats
{

static uint8_t Clamp255(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// --- BitReader ---

void BinkVideoDecoder::BitReader::Init(const uint8_t* d, size_t s)
{
    data = d;
    size = s;
    pos = 0;
    bits = 0;
    avail = 0;
}

uint32_t BinkVideoDecoder::BitReader::Read(int n)
{
    uint32_t result = 0;
    int got = 0;
    while (got < n)
    {
        if (avail == 0)
        {
            if (pos + 4 <= size)
            {
                bits = static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8) |
                       (static_cast<uint32_t>(data[pos + 2]) << 16) |
                       (static_cast<uint32_t>(data[pos + 3]) << 24);
                pos += 4;
            }
            else
            {
                bits = 0;
                pos = size;
            }
            avail = 32;
        }
        int take = std::min(n - got, avail);
        uint32_t mask = (take >= 32) ? 0xFFFFFFFFu : ((1u << take) - 1);
        result |= (bits & mask) << got;
        bits >>= take;
        avail -= take;
        got += take;
    }
    return result;
}

uint32_t BinkVideoDecoder::BitReader::Read1() { return Read(1); }

void BinkVideoDecoder::BitReader::Align32()
{
    avail = 0;
    bits = 0;
}

// --- Constructor / Reset ---

BinkVideoDecoder::BinkVideoDecoder(uint32_t width, uint32_t height, char codec_revision,
                                   bool has_alpha)
    : m_width(width), m_height(height), m_revision(codec_revision), m_hasAlpha(has_alpha),
      m_swapPlanes(codec_revision >= 'h'), m_colLastVal(0)
{
    uint32_t yw = (width + 7u) & ~7u;
    uint32_t yh = (height + 7u) & ~7u;
    uint32_t cw = ((width + 15u) & ~15u) / 2;
    uint32_t ch = ((height + 15u) & ~15u) / 2;

    InitPlane(m_cur.y, yw, yh, 0);
    InitPlane(m_cur.u, cw, ch, 128);
    InitPlane(m_cur.v, cw, ch, 128);
    InitPlane(m_prev.y, yw, yh, 0);
    InitPlane(m_prev.u, cw, ch, 128);
    InitPlane(m_prev.v, cw, ch, 128);

    if (m_hasAlpha)
    {
        InitPlane(m_cur.a, yw, yh, 0);
        InitPlane(m_prev.a, yw, yh, 0);
    }

    m_output.width = width;
    m_output.height = height;
    m_output.rgba.resize(static_cast<size_t>(width) * height * 4);

    int bw = static_cast<int>((width + 7) >> 3);
    int blocks = bw * static_cast<int>((height + 7) >> 3);
    for (auto& b : m_bundles)
    {
        b.data.resize(static_cast<size_t>(blocks) * 64);
    }
}

void BinkVideoDecoder::InitPlane(Plane& p, uint32_t w, uint32_t h, uint8_t fill)
{
    p.width = w;
    p.height = h;
    p.data.resize(static_cast<size_t>(w) * h, fill);
}

void BinkVideoDecoder::Reset()
{
    // Bink internal: Y=0 (black), U=128 V=128 (neutral chroma)
    std::fill(m_prev.y.data.begin(), m_prev.y.data.end(), 0);
    std::fill(m_prev.u.data.begin(), m_prev.u.data.end(), 128);
    std::fill(m_prev.v.data.begin(), m_prev.v.data.end(), 128);
    std::fill(m_cur.y.data.begin(), m_cur.y.data.end(), 0);
    std::fill(m_cur.u.data.begin(), m_cur.u.data.end(), 128);
    std::fill(m_cur.v.data.begin(), m_cur.v.data.end(), 128);
    if (m_hasAlpha)
    {
        std::fill(m_prev.a.data.begin(), m_prev.a.data.end(), 0);
        std::fill(m_cur.a.data.begin(), m_cur.a.data.end(), 0);
    }
}

// --- Huffman ---

static int IntLog2(int v)
{
    int r = 0;
    while (v > 1)
    {
        v >>= 1;
        r++;
    }
    return r;
}

void BinkVideoDecoder::InitLengths(int width, int bw)
{
    width = (width + 7) & ~7;
    m_bundles[SRC_BLOCK_TYPES].lenBits = IntLog2((width >> 3) + 511) + 1;
    m_bundles[SRC_SUB_BLOCK_TYPES].lenBits = IntLog2((width >> 4) + 511) + 1;
    m_bundles[SRC_COLORS].lenBits = IntLog2(bw * 64 + 511) + 1;
    m_bundles[SRC_INTRA_DC].lenBits = IntLog2((width >> 3) + 511) + 1;
    m_bundles[SRC_INTER_DC].lenBits = IntLog2((width >> 3) + 511) + 1;
    m_bundles[SRC_X_OFF].lenBits = IntLog2((width >> 3) + 511) + 1;
    m_bundles[SRC_Y_OFF].lenBits = IntLog2((width >> 3) + 511) + 1;
    m_bundles[SRC_PATTERN].lenBits = IntLog2((bw << 3) + 511) + 1;
    m_bundles[SRC_RUN].lenBits = IntLog2(bw * 48 + 511) + 1;
}

void BinkVideoDecoder::ReadTree(BitReader& br, HuffTree& tree)
{
    tree.vlcNum = static_cast<int>(br.Read(4));
    if (tree.vlcNum == 0)
    {
        for (int i = 0; i < 16; i++)
        {
            tree.syms[i] = static_cast<uint8_t>(i);
        }
        return;
    }

    if (br.Read1() != 0)
    {
        // Explicit symbol list
        uint8_t used[16] = {};
        int len = static_cast<int>(br.Read(3));
        for (int i = 0; i <= len; i++)
        {
            tree.syms[i] = static_cast<uint8_t>(br.Read(4));
            used[tree.syms[i]] = 1;
        }
        for (int i = 0; i < 16 && len < 15; i++)
        {
            if (used[i] == 0)
            {
                tree.syms[++len] = static_cast<uint8_t>(i);
            }
        }
    }
    else
    {
        // Bit-controlled merge shuffle (matches FFmpeg's merge function)
        uint8_t tmp1[16], tmp2[16];
        uint8_t* in = tmp1;
        uint8_t* out = tmp2;
        for (int i = 0; i < 16; i++)
        {
            in[i] = static_cast<uint8_t>(i);
        }
        int len = static_cast<int>(br.Read(2));
        for (int i = 0; i <= len; i++)
        {
            int sz = 1 << i;
            for (int t = 0; t < 16; t += sz << 1)
            {
                uint8_t* src1 = in + t;
                uint8_t* src2 = in + t + sz;
                int s1 = sz, s2 = sz;
                if (t + sz > 16)
                {
                    s1 = 16 - t;
                    s2 = 0;
                }
                else if (t + (sz << 1) > 16)
                {
                    s2 = 16 - t - sz;
                }
                int o = t;
                while (s1 > 0 && s2 > 0)
                {
                    if (br.Read1() == 0)
                    {
                        out[o++] = *src1++;
                        s1--;
                    }
                    else
                    {
                        out[o++] = *src2++;
                        s2--;
                    }
                }
                while (s1-- > 0)
                {
                    out[o++] = *src1++;
                }
                while (s2-- > 0)
                {
                    out[o++] = *src2++;
                }
            }
            std::swap(in, out);
        }
        std::memcpy(tree.syms, in, 16);
    }
}

uint8_t BinkVideoDecoder::GetHuff(BitReader& br, const HuffTree& tree)
{
    int vlc = tree.vlcNum;
    if (vlc == 0)
    {
        return tree.syms[br.Read(4)];
    }
    // Build lookup: accumulate bits one at a time and check for matches
    uint32_t code = 0;
    for (int len = 1; len <= 8; len++)
    {
        code |= br.Read1() << (len - 1);
        for (int i = 0; i < 16; i++)
        {
            if (kTreeLens[vlc][i] == len && kTreeBits[vlc][i] == code)
            {
                return tree.syms[i];
            }
        }
    }
    return 0;
}

// --- Bundle reading ---

int BinkVideoDecoder::ReadBundleCount(BitReader& br, Bundle& b)
{
    b.curDec = b.data.data() + (b.curDec - b.data.data()); // keep offset
    if (b.curDec == nullptr || b.curDec > b.curPtr)
    {
        return 0;
    }
    int t = static_cast<int>(br.Read(b.lenBits));
    if (t == 0)
    {
        b.curDec = nullptr;
        return 0;
    }
    return t;
}

void BinkVideoDecoder::ReadBlockTypes(BitReader& br, Bundle& b)
{
    if (b.curDec == nullptr || b.curDec > b.curPtr)
    {
        return;
    }
    int t = static_cast<int>(br.Read(b.lenBits));
    if (t == 0)
    {
        b.curDec = nullptr;
        return;
    }
    uint8_t* end = b.curDec + t;
    if (br.Read1() != 0)
    {
        uint8_t v = static_cast<uint8_t>(br.Read(4));
        std::memset(b.curDec, v, static_cast<size_t>(t));
        b.curDec += t;
    }
    else
    {
        int last = 0;
        while (b.curDec < end)
        {
            int v = GetHuff(br, b.tree);
            if (v < 12)
            {
                last = v;
                *b.curDec++ = static_cast<uint8_t>(v);
            }
            else
            {
                int run = kRLERunLens[v - 12];
                std::memset(b.curDec, static_cast<uint8_t>(last), static_cast<size_t>(run));
                b.curDec += run;
            }
        }
    }
}

void BinkVideoDecoder::ReadRuns(BitReader& br, Bundle& b)
{
    if (b.curDec == nullptr || b.curDec > b.curPtr)
    {
        return;
    }
    int t = static_cast<int>(br.Read(b.lenBits));
    if (t == 0)
    {
        b.curDec = nullptr;
        return;
    }
    uint8_t* end = b.curDec + t;
    if (br.Read1() != 0)
    {
        uint8_t v = static_cast<uint8_t>(br.Read(4));
        std::memset(b.curDec, v, static_cast<size_t>(t));
        b.curDec += t;
    }
    else
    {
        while (b.curDec < end)
        {
            *b.curDec++ = GetHuff(br, b.tree);
        }
    }
}

void BinkVideoDecoder::ReadMotionValues(BitReader& br, Bundle& b)
{
    if (b.curDec == nullptr || b.curDec > b.curPtr)
    {
        return;
    }
    int t = static_cast<int>(br.Read(b.lenBits));
    if (t == 0)
    {
        b.curDec = nullptr;
        return;
    }
    uint8_t* end = b.curDec + t;
    if (br.Read1() != 0)
    {
        int v = static_cast<int>(br.Read(4));
        if (v != 0)
        {
            int sign = -static_cast<int>(br.Read1());
            v = (v ^ sign) - sign;
        }
        std::memset(b.curDec, static_cast<uint8_t>(v), static_cast<size_t>(t));
        b.curDec += t;
    }
    else
    {
        while (b.curDec < end)
        {
            int v = GetHuff(br, b.tree);
            if (v != 0)
            {
                int sign = -static_cast<int>(br.Read1());
                v = (v ^ sign) - sign;
            }
            *b.curDec++ = static_cast<uint8_t>(v);
        }
    }
}

void BinkVideoDecoder::ReadPatterns(BitReader& br, Bundle& b)
{
    if (b.curDec == nullptr || b.curDec > b.curPtr)
    {
        return;
    }
    int t = static_cast<int>(br.Read(b.lenBits));
    if (t == 0)
    {
        b.curDec = nullptr;
        return;
    }
    uint8_t* end = b.curDec + t;
    while (b.curDec < end)
    {
        int v = GetHuff(br, b.tree);
        v |= GetHuff(br, b.tree) << 4;
        *b.curDec++ = static_cast<uint8_t>(v);
    }
}

void BinkVideoDecoder::ReadColors(BitReader& br, Bundle& b)
{
    if (b.curDec == nullptr || b.curDec > b.curPtr)
    {
        return;
    }
    int t = static_cast<int>(br.Read(b.lenBits));
    if (t == 0)
    {
        b.curDec = nullptr;
        return;
    }
    uint8_t* end = b.curDec + t;
    if (br.Read1() != 0)
    {
        m_colLastVal = GetHuff(br, m_colHigh[m_colLastVal]);
        int v = GetHuff(br, b.tree);
        v = (m_colLastVal << 4) | v;
        if (m_revision < 'i')
        {
            int sign = static_cast<int8_t>(v) >> 7;
            v = ((v & 0x7F) ^ sign) - sign;
            v += 0x80;
        }
        std::memset(b.curDec, static_cast<uint8_t>(v), static_cast<size_t>(t));
        b.curDec += t;
    }
    else
    {
        while (b.curDec < end)
        {
            m_colLastVal = GetHuff(br, m_colHigh[m_colLastVal]);
            int v = GetHuff(br, b.tree);
            v = (m_colLastVal << 4) | v;
            if (m_revision < 'i')
            {
                int sign = static_cast<int8_t>(v) >> 7;
                v = ((v & 0x7F) ^ sign) - sign;
                v += 0x80;
            }
            *b.curDec++ = static_cast<uint8_t>(v);
        }
    }
}

void BinkVideoDecoder::ReadDCs(BitReader& br, Bundle& b, int startBits, bool hasSgn)
{
    if (b.curDec == nullptr || b.curDec > b.curPtr)
    {
        return;
    }
    int t = static_cast<int>(br.Read(b.lenBits));
    if (t == 0)
    {
        b.curDec = nullptr;
        return;
    }
    auto* dst = reinterpret_cast<int16_t*>(b.curDec);
    int v = static_cast<int>(br.Read(startBits - (hasSgn ? 1 : 0)));
    if (v != 0 && hasSgn)
    {
        int sign = -static_cast<int>(br.Read1());
        v = (v ^ sign) - sign;
    }
    *dst++ = static_cast<int16_t>(v);
    for (int i = 1; i < t; i += 8)
    {
        int len2 = std::min(t - i, 8);
        int bsize = static_cast<int>(br.Read(4));
        if (bsize != 0)
        {
            for (int j = 0; j < len2; j++)
            {
                int v2 = static_cast<int>(br.Read(bsize));
                if (v2 != 0)
                {
                    int sign = -static_cast<int>(br.Read1());
                    v2 = (v2 ^ sign) - sign;
                }
                v += v2;
                *dst++ = static_cast<int16_t>(v);
            }
        }
        else
        {
            for (int j = 0; j < len2; j++)
            {
                *dst++ = static_cast<int16_t>(v);
            }
        }
    }
    b.curDec = reinterpret_cast<uint8_t*>(dst);
}

uint8_t BinkVideoDecoder::GetVal(int id) { return *m_bundles[id].curPtr++; }

int8_t BinkVideoDecoder::GetMotionVal(int id)
{
    return static_cast<int8_t>(*m_bundles[id].curPtr++);
}

int16_t BinkVideoDecoder::GetDCVal(int id)
{
    int16_t v;
    std::memcpy(&v, m_bundles[id].curPtr, 2);
    m_bundles[id].curPtr += 2;
    return v;
}

// --- IDCT ---

static constexpr int32_t kA1 = 2896;
static constexpr int32_t kA2 = 2217;
static constexpr int32_t kA3 = 3784;
static constexpr int32_t kA4 = -5352;

static int32_t MUL(int32_t x, int32_t y) { return (x * y) >> 11; }

void BinkVideoDecoder::IDCTCol(int32_t* col)
{
    int32_t a0 = col[0] + col[32];
    int32_t a1 = col[0] - col[32];
    int32_t a2 = col[16] + col[48];
    int32_t a3 = MUL(kA1, col[16] - col[48]);
    int32_t a4 = col[40] + col[24];
    int32_t a5 = col[40] - col[24];
    int32_t a6 = col[8] + col[56];
    int32_t a7 = col[8] - col[56];
    int32_t b0 = a4 + a6;
    int32_t b1 = MUL(kA3, a5 + a7);
    int32_t b2 = MUL(kA4, a5) - b0 + b1;
    int32_t b3 = MUL(kA1, a6 - a4) - b2;
    int32_t b4 = MUL(kA2, a7) + b3 - b1;
    col[0] = a0 + a2 + b0;
    col[8] = a1 + a3 - a2 + b2;
    col[16] = a1 - a3 + a2 + b3;
    col[24] = a0 - a2 - b4;
    col[32] = a0 - a2 + b4;
    col[40] = a1 - a3 + a2 - b3;
    col[48] = a1 + a3 - a2 - b2;
    col[56] = a0 + a2 - b0;
}

void BinkVideoDecoder::IDCTRow(int32_t* row)
{
    int32_t a0 = row[0] + row[4];
    int32_t a1 = row[0] - row[4];
    int32_t a2 = row[2] + row[6];
    int32_t a3 = MUL(kA1, row[2] - row[6]);
    int32_t a4 = row[5] + row[3];
    int32_t a5 = row[5] - row[3];
    int32_t a6 = row[1] + row[7];
    int32_t a7 = row[1] - row[7];
    int32_t b0 = a4 + a6;
    int32_t b1 = MUL(kA3, a5 + a7);
    int32_t b2 = MUL(kA4, a5) - b0 + b1;
    int32_t b3 = MUL(kA1, a6 - a4) - b2;
    int32_t b4 = MUL(kA2, a7) + b3 - b1;
    row[0] = (a0 + a2 + b0 + 127) >> 8;
    row[1] = (a1 + a3 - a2 + b2 + 127) >> 8;
    row[2] = (a1 - a3 + a2 + b3 + 127) >> 8;
    row[3] = (a0 - a2 - b4 + 127) >> 8;
    row[4] = (a0 - a2 + b4 + 127) >> 8;
    row[5] = (a1 - a3 + a2 - b3 + 127) >> 8;
    row[6] = (a1 + a3 - a2 - b2 + 127) >> 8;
    row[7] = (a0 + a2 - b0 + 127) >> 8;
}

void BinkVideoDecoder::IDCT(int32_t* block)
{
    for (int i = 0; i < 8; i++)
    {
        IDCTCol(block + i);
    }
    for (int i = 0; i < 8; i++)
    {
        IDCTRow(block + i * 8);
    }
}

void BinkVideoDecoder::IDCTPut(int32_t block[64], uint8_t* dst, int stride, int coordmap[64])
{
    IDCT(block);
    for (int i = 0; i < 64; i++)
    {
        dst[coordmap[i]] = Clamp255(block[i]);
    }
}

void BinkVideoDecoder::IDCTAdd(int32_t block[64], uint8_t* dst, int stride, int coordmap[64])
{
    IDCT(block);
    for (int i = 0; i < 64; i++)
    {
        dst[coordmap[i]] = Clamp255(dst[coordmap[i]] + block[i]);
    }
}

// --- DCT Coefficient Reading ---

int BinkVideoDecoder::ReadDCTCoeffs(BitReader& br, int32_t block[64], int coefIdx[64],
                                    int& coefCount)
{
    int coefList[128], modeList[128];
    int listStart = 64, listEnd = 64;
    coefCount = 0;

    coefList[listEnd] = 4;
    modeList[listEnd++] = 0;
    coefList[listEnd] = 24;
    modeList[listEnd++] = 0;
    coefList[listEnd] = 44;
    modeList[listEnd++] = 0;
    coefList[listEnd] = 1;
    modeList[listEnd++] = 3;
    coefList[listEnd] = 2;
    modeList[listEnd++] = 3;
    coefList[listEnd] = 3;
    modeList[listEnd++] = 3;

    int nBits = static_cast<int>(br.Read(4)) - 1;
    for (int bits = nBits; bits >= 0; bits--)
    {
        int listPos = listStart;
        while (listPos < listEnd)
        {
            if ((modeList[listPos] == 0 && coefList[listPos] == 0) || br.Read1() == 0)
            {
                listPos++;
                continue;
            }
            int ccoef = coefList[listPos];
            int mode = modeList[listPos];
            switch (mode)
            {
            case 0:
                coefList[listPos] = ccoef + 4;
                modeList[listPos] = 1;
                // fallthrough
            case 2:
                if (mode == 2)
                {
                    coefList[listPos] = 0;
                    modeList[listPos++] = 0;
                }
                for (int i = 0; i < 4; i++, ccoef++)
                {
                    if (br.Read1() != 0)
                    {
                        coefList[--listStart] = ccoef;
                        modeList[listStart] = 3;
                    }
                    else
                    {
                        int t;
                        if (bits == 0)
                        {
                            t = 1 - (static_cast<int>(br.Read1()) << 1);
                        }
                        else
                        {
                            t = static_cast<int>(br.Read(bits)) | (1 << bits);
                            int sign = -static_cast<int>(br.Read1());
                            t = (t ^ sign) - sign;
                        }
                        block[kBinkScan[ccoef]] = t;
                        coefIdx[coefCount++] = ccoef;
                    }
                }
                break;
            case 1:
                modeList[listPos] = 2;
                for (int i = 0; i < 3; i++)
                {
                    ccoef += 4;
                    coefList[listEnd] = ccoef;
                    modeList[listEnd++] = 2;
                }
                break;
            case 3:
            {
                int t;
                if (bits == 0)
                {
                    t = 1 - (static_cast<int>(br.Read1()) << 1);
                }
                else
                {
                    t = static_cast<int>(br.Read(bits)) | (1 << bits);
                    int sign = -static_cast<int>(br.Read1());
                    t = (t ^ sign) - sign;
                }
                block[kBinkScan[ccoef]] = t;
                coefIdx[coefCount++] = ccoef;
                coefList[listPos] = 0;
                modeList[listPos++] = 0;
                break;
            }
            }
        }
    }

    return static_cast<int>(br.Read(4)); // quantizer index
}

void BinkVideoDecoder::UnquantizeDCTCoeffs(int32_t block[64], const int32_t quant[64],
                                            int coefCount, int coefIdx[64])
{
    block[0] = (block[0] * quant[0]) >> 11;
    for (int i = 0; i < coefCount; i++)
    {
        int idx = coefIdx[i];
        block[kBinkScan[idx]] = (block[kBinkScan[idx]] * quant[idx]) >> 11;
    }
}

void BinkVideoDecoder::ReadResidue(BitReader& br, int16_t block[64], int masksCount)
{
    int coefList[128], modeList[128];
    int listStart = 64, listEnd = 64;
    int nzCoeff[64];
    int nzCoeffCount = 0;

    coefList[listEnd] = 4;
    modeList[listEnd++] = 0;
    coefList[listEnd] = 24;
    modeList[listEnd++] = 0;
    coefList[listEnd] = 44;
    modeList[listEnd++] = 0;
    coefList[listEnd] = 0;
    modeList[listEnd++] = 2;

    int mask = 1 << static_cast<int>(br.Read(3));
    while (mask != 0)
    {
        for (int i = 0; i < nzCoeffCount; i++)
        {
            if (br.Read1() == 0)
            {
                continue;
            }
            if (block[nzCoeff[i]] < 0)
            {
                block[nzCoeff[i]] -= static_cast<int16_t>(mask);
            }
            else
            {
                block[nzCoeff[i]] += static_cast<int16_t>(mask);
            }
            masksCount--;
            if (masksCount < 0)
            {
                return;
            }
        }
        int listPos = listStart;
        while (listPos < listEnd)
        {
            if ((modeList[listPos] == 0 && coefList[listPos] == 0) || br.Read1() == 0)
            {
                listPos++;
                continue;
            }
            int ccoef = coefList[listPos];
            int mode = modeList[listPos];
            switch (mode)
            {
            case 0:
                coefList[listPos] = ccoef + 4;
                modeList[listPos] = 1;
                // fallthrough
            case 2:
                if (mode == 2)
                {
                    coefList[listPos] = 0;
                    modeList[listPos++] = 0;
                }
                for (int i = 0; i < 4; i++, ccoef++)
                {
                    if (br.Read1() != 0)
                    {
                        coefList[--listStart] = ccoef;
                        modeList[listStart] = 3;
                    }
                    else
                    {
                        nzCoeff[nzCoeffCount++] = kBinkScan[ccoef];
                        int sign = -static_cast<int>(br.Read1());
                        block[kBinkScan[ccoef]] =
                            static_cast<int16_t>((mask ^ sign) - sign);
                        masksCount--;
                        if (masksCount < 0)
                        {
                            return;
                        }
                    }
                }
                break;
            case 1:
                modeList[listPos] = 2;
                for (int i = 0; i < 3; i++)
                {
                    ccoef += 4;
                    coefList[listEnd] = ccoef;
                    modeList[listEnd++] = 2;
                }
                break;
            case 3:
                nzCoeff[nzCoeffCount++] = kBinkScan[ccoef];
                {
                    int sign = -static_cast<int>(br.Read1());
                    block[kBinkScan[ccoef]] =
                        static_cast<int16_t>((mask ^ sign) - sign);
                }
                coefList[listPos] = 0;
                modeList[listPos++] = 0;
                masksCount--;
                if (masksCount < 0)
                {
                    return;
                }
                break;
            default:
                break;
            }
        }
        mask >>= 1;
    }
}

// --- Block Decoder ---

const uint8_t* BinkVideoDecoder::SafeMotionSrc(const uint8_t* prev, int prevStride)
{
    int xoff = GetMotionVal(SRC_X_OFF);
    int yoff = GetMotionVal(SRC_Y_OFF);
    const uint8_t* src = prev + yoff * prevStride + xoff;
    if (src < m_refStart || src > m_refEnd)
    {
        return prev;
    }
    return src;
}

void BinkVideoDecoder::DecodeBlock(BitReader& br, uint8_t* dst, const uint8_t* prev, int stride,
                                   int prevStride, int blockType, int coordmap[64],
                                   int32_t* dctBuf)
{
    switch (blockType)
    {
    case 0: // Skip
        for (int r = 0; r < 8; r++)
        {
            std::memcpy(dst + r * stride, prev + r * prevStride, 8);
        }
        break;

    case 2: // Motion
    {
        const uint8_t* src = SafeMotionSrc(prev, prevStride);
        for (int r = 0; r < 8; r++)
        {
            std::memcpy(dst + r * stride, src + r * prevStride, 8);
        }
        break;
    }

    case 3: // Run
    {
        const uint8_t* scan = kPatterns[br.Read(4)];
        int i = 0;
        do
        {
            int run = GetVal(SRC_RUN) + 1;
            i += run;
            if (i > 64)
            {
                break;
            }
            if (br.Read1() != 0)
            {
                uint8_t v = GetVal(SRC_COLORS);
                for (int j = 0; j < run; j++)
                {
                    dst[coordmap[*scan++]] = v;
                }
            }
            else
            {
                for (int j = 0; j < run; j++)
                {
                    dst[coordmap[*scan++]] = GetVal(SRC_COLORS);
                }
            }
        } while (i < 63);
        if (i == 63)
        {
            dst[coordmap[*scan]] = GetVal(SRC_COLORS);
        }
        break;
    }

    case 4: // Motion + Residue
    {
        const uint8_t* src = SafeMotionSrc(prev, prevStride);
        for (int r = 0; r < 8; r++)
        {
            std::memcpy(dst + r * stride, src + r * prevStride, 8);
        }
        int16_t residue[64] = {};
        int masks = static_cast<int>(br.Read(7));
        ReadResidue(br, residue, masks);
        for (int r = 0; r < 8; r++)
        {
            for (int c = 0; c < 8; c++)
            {
                dst[r * stride + c] = static_cast<uint8_t>(
                    dst[r * stride + c] + residue[r * 8 + c]);
            }
        }
        break;
    }

    case 5: // Intra DCT
    {
        std::memset(dctBuf, 0, 64 * sizeof(int32_t));
        dctBuf[0] = GetDCVal(SRC_INTRA_DC);
        int coefIdx[64], coefCount;
        int qi = ReadDCTCoeffs(br, dctBuf, coefIdx, coefCount);
        UnquantizeDCTCoeffs(dctBuf, kBinkIntraQuant[qi], coefCount, coefIdx);
        IDCTPut(dctBuf, dst, stride, coordmap);
        break;
    }

    case 6: // Fill
    {
        uint8_t v = GetVal(SRC_COLORS);
        for (int r = 0; r < 8; r++)
        {
            std::memset(dst + r * stride, v, 8);
        }
        break;
    }

    case 7: // Inter DCT
    {
        const uint8_t* src = SafeMotionSrc(prev, prevStride);
        for (int r = 0; r < 8; r++)
        {
            std::memcpy(dst + r * stride, src + r * prevStride, 8);
        }
        std::memset(dctBuf, 0, 64 * sizeof(int32_t));
        dctBuf[0] = GetDCVal(SRC_INTER_DC);
        int coefIdx[64], coefCount;
        int qi = ReadDCTCoeffs(br, dctBuf, coefIdx, coefCount);
        UnquantizeDCTCoeffs(dctBuf, kBinkInterQuant[qi], coefCount, coefIdx);
        IDCTAdd(dctBuf, dst, stride, coordmap);
        break;
    }

    case 8: // Pattern
    {
        uint8_t c0 = GetVal(SRC_COLORS);
        uint8_t c1 = GetVal(SRC_COLORS);
        for (int r = 0; r < 8; r++)
        {
            uint8_t pat = GetVal(SRC_PATTERN);
            for (int c = 0; c < 8; c++)
            {
                dst[r * stride + c] = (pat & (1 << c)) ? c1 : c0;
            }
        }
        break;
    }

    case 9: // Raw
        for (int i = 0; i < 64; i++)
        {
            dst[coordmap[i]] = GetVal(SRC_COLORS);
        }
        break;

    default:
        for (int r = 0; r < 8; r++)
        {
            std::memset(dst + r * stride, 0, 8);
        }
        break;
    }
}

// --- Plane Decode ---

bool BinkVideoDecoder::DecodePlane(BitReader& br, Plane& cur, const Plane& prev, bool isChroma)
{
    int bw = static_cast<int>(cur.width) / 8;
    int bh = static_cast<int>(cur.height) / 8;
    int stride = static_cast<int>(cur.width);
    int prevStride = static_cast<int>(prev.width);
    int width = static_cast<int>(isChroma ? m_width >> 1 : m_width);

    InitLengths(std::max(width, 8), bw);

    // Read trees for each bundle
    for (int i = 0; i < kBundleCount; i++)
    {
        if (i == SRC_COLORS)
        {
            for (int j = 0; j < 16; j++)
            {
                ReadTree(br, m_colHigh[j]);
            }
            m_colLastVal = 0;
        }
        if (i != SRC_INTRA_DC && i != SRC_INTER_DC)
        {
            ReadTree(br, m_bundles[i].tree);
        }
        std::fill(m_bundles[i].data.begin(), m_bundles[i].data.end(), 0);
        m_bundles[i].curDec = m_bundles[i].data.data();
        m_bundles[i].curPtr = m_bundles[i].data.data();
    }
    m_refStart = prev.data.data();
    m_refEnd = prev.data.data() + (static_cast<size_t>(bh - 1) * 8 * prevStride + (bw - 1) * 8);

    int coordmap[64];
    for (int i = 0; i < 64; i++)
    {
        coordmap[i] = (i & 7) + (i >> 3) * stride;
    }

    int32_t dctBuf[64];

    for (int by = 0; by < bh; by++)
    {
        ReadBlockTypes(br, m_bundles[SRC_BLOCK_TYPES]);
        ReadBlockTypes(br, m_bundles[SRC_SUB_BLOCK_TYPES]);
        ReadColors(br, m_bundles[SRC_COLORS]);
        ReadPatterns(br, m_bundles[SRC_PATTERN]);
        ReadMotionValues(br, m_bundles[SRC_X_OFF]);
        ReadMotionValues(br, m_bundles[SRC_Y_OFF]);
        ReadDCs(br, m_bundles[SRC_INTRA_DC], 11, false);
        ReadDCs(br, m_bundles[SRC_INTER_DC], 11, true);
        ReadRuns(br, m_bundles[SRC_RUN]);

        uint8_t* dst = cur.data.data() + by * 8 * stride;
        const uint8_t* prevRow = prev.data.data() + by * 8 * prevStride;

        for (int bx = 0; bx < bw; bx++, dst += 8, prevRow += 8)
        {
            int blk = GetVal(SRC_BLOCK_TYPES);
            // Scaled blocks on odd row/col are already filled by the previous 16x16
            if (((by & 1) || (bx & 1)) && blk == 1)
            {
                bx++;
                dst += 8;
                prevRow += 8;
                continue;
            }
            if (blk == 1)
            {
                int sub = GetVal(SRC_SUB_BLOCK_TYPES);
                if (sub == 6)
                {
                    // FILL at 16x16: write directly
                    uint8_t v = GetVal(SRC_COLORS);
                    for (int r = 0; r < std::min(16, bh * 8 - by * 8); r++)
                    {
                        std::memset(dst + r * stride, v, std::min(16, bw * 8 - bx * 8));
                    }
                }
                else if (sub == 3 || sub == 5 || sub == 8 || sub == 9)
                {
                    // Valid sub-types: RUN(3), INTRA(5), PATTERN(8), RAW(9)
                    uint8_t ublock[64] = {};
                    int tmpCoord[64];
                    for (int i = 0; i < 64; i++)
                    {
                        tmpCoord[i] = (i & 7) + (i >> 3) * 8;
                    }
                    DecodeBlock(br, ublock, prevRow, 8, prevStride, sub, tmpCoord, dctBuf);
                    int maxR = std::min(16, bh * 8 - by * 8);
                    int maxC = std::min(16, bw * 8 - bx * 8);
                    for (int r = 0; r < 8; r++)
                    {
                        for (int c = 0; c < 8; c++)
                        {
                            uint8_t v = ublock[r * 8 + c];
                            if (r * 2 < maxR && c * 2 < maxC)
                            {
                                dst[(r * 2) * stride + c * 2] = v;
                            }
                            if (r * 2 < maxR && c * 2 + 1 < maxC)
                            {
                                dst[(r * 2) * stride + c * 2 + 1] = v;
                            }
                            if (r * 2 + 1 < maxR && c * 2 < maxC)
                            {
                                dst[(r * 2 + 1) * stride + c * 2] = v;
                            }
                            if (r * 2 + 1 < maxR && c * 2 + 1 < maxC)
                            {
                                dst[(r * 2 + 1) * stride + c * 2 + 1] = v;
                            }
                        }
                    }
                }
                bx++;
                dst += 8;
                prevRow += 8;
            }
            else
            {
                DecodeBlock(br, dst, prevRow, stride, prevStride, blk, coordmap, dctBuf);
            }
        }
    }

    // Align to 32-bit boundary for next plane
    size_t bitCount = br.pos * 8 - static_cast<size_t>(br.avail);
    if (bitCount & 0x1F)
    {
        int skip = 32 - static_cast<int>(bitCount & 0x1F);
        br.Read(skip);
    }

    return true;
}

// --- Frame Decode ---

bool BinkVideoDecoder::DecodeFrame(const uint8_t* data, size_t size, bool is_keyframe)
{
    if (is_keyframe)
    {
        Reset();
    }

    BitReader br;
    br.Init(data, size);

    // FFmpeg plane order: [alpha if present], then Y, U, V
    // Revision 'i': 32-bit plane size prefix before alpha group and before Y group
    if (m_hasAlpha)
    {
        if (m_revision >= 'i')
        {
            br.Read(32); // alpha plane data size — skip, decode sequentially
        }
        DecodePlane(br, m_cur.a, m_prev.a, false);
    }

    if (m_revision >= 'i')
    {
        br.Read(32); // plane data offset — skip, decode sequentially
    }

    size_t totalBits = size * 8;
    auto bitsLeft = [&]() -> int {
        return static_cast<int>(totalBits) - static_cast<int>(br.pos * 8 - br.avail);
    };

    // Y plane
    DecodePlane(br, m_cur.y, m_prev.y, false);

    // Chroma planes — swap_planes (revision >= 'h') puts V before U in bitstream
    if (m_swapPlanes)
    {
        if (bitsLeft() > 32)
        {
            DecodePlane(br, m_cur.v, m_prev.v, true);
        }
        if (bitsLeft() > 32)
        {
            DecodePlane(br, m_cur.u, m_prev.u, true);
        }
    }
    else
    {
        if (bitsLeft() > 32)
        {
            DecodePlane(br, m_cur.u, m_prev.u, true);
        }
        if (bitsLeft() > 32)
        {
            DecodePlane(br, m_cur.v, m_prev.v, true);
        }
    }

    ConvertYUVToRGBA();
    std::swap(m_cur, m_prev);
    return true;
}

// --- YUV to RGBA ---

void BinkVideoDecoder::ConvertYUVToRGBA()
{
    const uint8_t* yp = m_cur.y.data.data();
    const uint8_t* up = m_cur.u.data.data();
    const uint8_t* vp = m_cur.v.data.data();
    const uint8_t* ap = m_hasAlpha ? m_cur.a.data.data() : nullptr;
    uint8_t* out = m_output.rgba.data();
    int ystride = static_cast<int>(m_cur.y.width);
    int cstride = static_cast<int>(m_cur.u.width);

    for (uint32_t y = 0; y < m_height; y++)
    {
        for (uint32_t x = 0; x < m_width; x++)
        {
            int Y = yp[y * ystride + x];
            int U = up[(y / 2) * cstride + (x / 2)];
            int V = vp[(y / 2) * cstride + (x / 2)];
            // Full-range BT.601 (Bink internal format)
            int R = (256 * Y + 359 * (V - 128) + 128) >> 8;
            int G = (256 * Y - 88 * (U - 128) - 183 * (V - 128) + 128) >> 8;
            int B = (256 * Y + 454 * (U - 128) + 128) >> 8;
            size_t idx = (static_cast<size_t>(y) * m_width + x) * 4;
            out[idx + 0] = Clamp255(R);
            out[idx + 1] = Clamp255(G);
            out[idx + 2] = Clamp255(B);
            out[idx + 3] = ap != nullptr ? ap[y * ystride + x] : 255;
        }
    }
}

} // namespace nolf::formats
