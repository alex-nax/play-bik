#pragma once

#include <cstdint>
#include <vector>

namespace nolf::formats
{

struct BinkVideoFrame
{
    std::vector<uint8_t> rgba;
    uint32_t width;
    uint32_t height;
};

class BinkVideoDecoder
{
  public:
    BinkVideoDecoder(uint32_t width, uint32_t height, char codec_revision, bool has_alpha);

    bool DecodeFrame(const uint8_t* data, size_t size, bool is_keyframe);
    const BinkVideoFrame& GetFrame() const { return m_output; }
    void Reset();

  private:
    struct Plane
    {
        std::vector<uint8_t> data;
        uint32_t width;
        uint32_t height;
    };

    struct FrameBuffer
    {
        Plane y, u, v, a;
    };

    struct BitReader
    {
        const uint8_t* data = nullptr;
        size_t size = 0;
        size_t pos = 0;
        uint32_t bits = 0;
        int avail = 0;

        void Init(const uint8_t* d, size_t s);
        uint32_t Read(int n);
        uint32_t Read1();
        void Align32();
    };

    struct HuffTree
    {
        uint8_t syms[16];
        int vlcNum;
    };

    static constexpr int kBundleCount = 9;

    enum BundleId
    {
        SRC_BLOCK_TYPES = 0,
        SRC_SUB_BLOCK_TYPES,
        SRC_COLORS,
        SRC_PATTERN,
        SRC_X_OFF,
        SRC_Y_OFF,
        SRC_INTRA_DC,
        SRC_INTER_DC,
        SRC_RUN,
    };

    struct Bundle
    {
        std::vector<uint8_t> data;
        uint8_t* curDec = nullptr;
        uint8_t* curPtr = nullptr;
        int lenBits = 0;
        HuffTree tree;
    };

    void InitPlane(Plane& p, uint32_t w, uint32_t h, uint8_t fill = 0);
    void InitLengths(int width, int bw);
    void ReadTree(BitReader& br, HuffTree& tree);
    uint8_t GetHuff(BitReader& br, const HuffTree& tree);

    int ReadBundleCount(BitReader& br, Bundle& b);
    void ReadBlockTypes(BitReader& br, Bundle& b);
    void ReadColors(BitReader& br, Bundle& b);
    void ReadPatterns(BitReader& br, Bundle& b);
    void ReadMotionValues(BitReader& br, Bundle& b);
    void ReadDCs(BitReader& br, Bundle& b, int startBits, bool hasSgn);
    void ReadRuns(BitReader& br, Bundle& b);

    uint8_t GetVal(int id);
    int8_t GetMotionVal(int id);
    int16_t GetDCVal(int id);

    bool DecodePlane(BitReader& br, Plane& cur, const Plane& prev, bool isChroma);
    const uint8_t* SafeMotionSrc(const uint8_t* prev, int prevStride);
    void DecodeBlock(BitReader& br, uint8_t* dst, const uint8_t* prev, int stride,
                     int prevStride, int blockType, int coordmap[64], int32_t* dctBuf);

    int ReadDCTCoeffs(BitReader& br, int32_t block[64], int coefIdx[64], int& coefCount);
    void UnquantizeDCTCoeffs(int32_t block[64], const int32_t quant[64], int coefCount,
                             int coefIdx[64]);
    void ReadResidue(BitReader& br, int16_t block[64], int masksCount);
    void IDCT(int32_t* block);
    void IDCTCol(int32_t* col);
    void IDCTRow(int32_t* row);
    void IDCTPut(int32_t block[64], uint8_t* dst, int stride, int coordmap[64]);
    void IDCTAdd(int32_t block[64], uint8_t* dst, int stride, int coordmap[64]);
    void ConvertYUVToRGBA();

    uint32_t m_width;
    uint32_t m_height;
    char m_revision;
    bool m_hasAlpha;
    bool m_swapPlanes;

    FrameBuffer m_cur;
    FrameBuffer m_prev;
    BinkVideoFrame m_output;

    Bundle m_bundles[kBundleCount];
    HuffTree m_colHigh[16];
    int m_colLastVal;

    const uint8_t* m_refStart = nullptr;
    const uint8_t* m_refEnd = nullptr;
};

} // namespace nolf::formats
