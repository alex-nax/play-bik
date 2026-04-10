# Feature 116: Bink Video Decoder

## Summary

Clean-room Bink Video 1 decoder that transforms compressed frame data (extracted by F115's container demuxer) into RGBA pixel buffers. Covers all block types used in NOLF's four intro .bik files (all revision 'i').

## Context

- **Depends on**: F115 (Bink container demuxer)
- **Depended on by**: F33 (video playback)
- **FFmpeg reference**: `libavcodec/bink.c`, `binkdsp.c`, `binkdata.h` (behavioral reference only)
- **Our implementation**: `src/formats/bink_video_decoder.cpp` (~1155 lines)

## Design

### Public Interface

```cpp
class BinkVideoDecoder {
    BinkVideoDecoder(uint32_t width, uint32_t height, char codec_revision, bool has_alpha);
    bool DecodeFrame(const uint8_t* data, size_t size, bool is_keyframe);
    const BinkVideoFrame& GetFrame() const;
    void Reset();
};
```

### Codec Architecture

Bink 1 is a block-based YUV 4:2:0 codec:
- **Planes**: Y (full res), U/V (half res), optional Alpha (full res)
- **Blocks**: 8x8 pixel blocks, 10 block types (0–9)
- **Bundles**: 9 data streams refilled per row of blocks
- **Huffman**: 16 predefined VLC tables with merge-sort symbol reordering
- **DCT**: Custom 8-point IDCT, fixed-point (>>11 multiply, >>8 row output)
- **Quantization**: 16 intra + 16 inter matrices

### Frame Decode Flow

1. If alpha: skip 32-bit field, decode alpha plane
2. Skip 32-bit field (revision >= 'i')
3. Decode Y, U, V planes sequentially (U/V swapped for revision >= 'h')
4. Convert YUV to RGBA

### Block Types

| Type | Name | Description |
|------|------|-------------|
| 0 | SKIP | Copy from previous frame |
| 1 | SCALED | Decode 8x8 sub-block, upscale 2x to 16x16 |
| 2 | MOTION | Copy 8x8 from prev + (xoff, yoff) |
| 3 | RUN | Pattern-scan with run-length coded colors |
| 4 | RESIDUE | Motion copy + list-based residue correction |
| 5 | INTRA | DCT intra (DC from bundle + AC coefficients) |
| 6 | FILL | Single color fill |
| 7 | INTER | Motion copy + DCT inter correction |
| 8 | PATTERN | 2-color pattern fill (8 bytes from bundle) |
| 9 | RAW | 64 raw color values |

## Files to Create/Modify

| File | Action | Description |
|------|--------|-------------|
| `src/formats/bink_video_decoder.cpp` | Modify | Fix bugs identified below |
| `src/formats/bink_video_decoder.h` | Modify | Add int16_t residue buffer |
| `docs/specs/feature-116-bink-video-decoder.md` | Replace | This spec |

## Acceptance Criteria

- All 4 NOLF .bik files decode all frames without crash
- Keyframes produce visually correct output
- Inter-frames show correct motion compensation and color
- No chroma corruption, blue/green tint, or block artifacts
- Visual verification via `nolf-bink-player` tool

## Test Plan

- foxpc.bik: frame 0 (keyframe), frame 30 (inter), frame 100 (late inter)
- LTLogo.bik: frame 0, frame 130
- LithLogo.bik: frame 0
- SierraLogo.bik: frame 0 (800x400, non-standard dims)
- All files: decode every frame without crash

---

## FFmpeg Comparison — Bug Analysis

Step-by-step comparison of `bink_video_decoder.cpp` against FFmpeg `bink.c` (master, April 2026). Each function is compared with the FFmpeg equivalent and differences are categorized.

---

### 1. Frame-Level Decode (`DecodeFrame` vs `decode_frame`)

#### 1.1 Plane offset field — revision 'i' [CRITICAL BUG]

**FFmpeg** (line 1280):
```c
if (c->version >= 'i')
    skip_bits_long(&gb, 32);   // just SKIP, not used
// ... decode Y, U, V sequentially in a for-loop ...
```
The 32-bit field is read and discarded. All three planes decode sequentially from the same bitstream.

**Ours** (line 1072–1092):
```cpp
planeDataOffset = br.Read(32);
DecodePlane(br, m_cur.y, m_prev.y, false);  // Y
if (m_revision >= 'i' && planeDataOffset > 0) {
    br.pos = planeDataOffset;  // SEEK to byte offset
    br.avail = 0;
}
DecodePlane(br, m_cur.u, m_prev.u, true);   // U
```

**Bug**: We interpret the 32-bit field as a byte offset and seek to it before decoding chroma. FFmpeg just skips it. This corrupts the bitstream position, causing all chroma data (U/V planes) to be read from the wrong position.

**Impact**: Primary cause of the blue/green tint and color corruption on all frames.

**Fix**: Remove `planeDataOffset` logic entirely. Just `br.Read(32)` (skip) and decode all planes sequentially.

#### 1.2 Plane iteration order [OK — matches]

FFmpeg: plane 0→Y, plane 1→U or V (swap for >= 'h'), plane 2→V or U.
Ours: Same logic with `m_swapPlanes`.

#### 1.3 Bits-exhausted check [OK — matches]

FFmpeg: `if (get_bits_count(&gb) >= bits_count) break;`
Ours: `if (bitsLeft() > 32)` before each chroma plane. Equivalent intent.

---

### 2. Plane Decode (`DecodePlane` vs `bink_decode_plane`)

#### 2.1 Chroma bw/bh calculation [OK — matches]

FFmpeg: `bw = is_chroma ? (width + 15) >> 4 : (width + 7) >> 3`
Ours: `bw = cur.width / 8` where `cur.width = ((width + 15) & ~15) / 2`. Same result.

#### 2.2 init_lengths [OK — matches]

Both use identical formulas: `av_log2((width >> 3) + 511) + 1` for BLOCK_TYPES, etc.

#### 2.3 Bundle tree reading order [OK — matches]

Both: read color high trees (×16), then trees for non-DC bundles. Reset cur_dec = cur_ptr = data.

#### 2.4 coordmap [OK — matches]

Both: `coordmap[i] = (i & 7) + (i >> 3) * stride`

#### 2.5 Per-row bundle refill order [OK — matches]

Both: block_types, sub_block_types, colors, patterns, x_off, y_off, intra_dc, inter_dc, runs.

#### 2.6 32-bit alignment after plane [OK — matches]

Both: skip remaining bits in current 32-bit word.

---

### 3. Bundle Reading Functions

#### 3.1 CHECK_READ_VAL / ReadBundleCount [OK — matches]

FFmpeg: `if (!b->cur_dec || (b->cur_dec > b->cur_ptr)) return 0;`
Ours: Same check in each Read* function.

#### 3.2 read_runs / ReadRuns [OK — matches]

Both: flag bit → memset or per-element Huffman decode.

#### 3.3 read_motion_values / ReadMotionValues [OK — matches]

Both: flag bit → memset (with sign) or per-element with sign. Same sign pattern: `sign = -get_bits1(); v = (v ^ sign) - sign`.

#### 3.4 read_block_types / ReadBlockTypes [OK — matches]

Both: flag bit → memset or Huffman with RLE (values 12–15 map to runs 4, 8, 12, 32).

#### 3.5 read_patterns / ReadPatterns [OK — matches]

Both: `v = GET_HUFF | (GET_HUFF << 4)`.

#### 3.6 read_colors / ReadColors [OK — matches]

Both: two-level Huffman (high nibble from `col_high[lastval]`, low from bundle tree). Revision < 'i' sign extension logic matches.

#### 3.7 read_dcs / ReadDCs [OK — matches]

Both: delta-coded int16_t. First value from `start_bits`. Groups of 8 with 4-bit bsize. Same sign handling.

---

### 4. Huffman / VLC

#### 4.1 read_tree / ReadTree [OK — matches]

Both: 4-bit vlc_num. Identity for 0. Explicit list (flag=1) or merge-sort (flag=0).

#### 4.2 Merge function [OK — matches]

Both: bit-controlled interleave of two halves. Our boundary handling matches.

#### 4.3 GetHuff [MINOR DIFFERENCE]

FFmpeg uses pre-built VLC tables (`get_vlc2`) for O(1) lookup.
Ours: linear scan over `kTreeBits`/`kTreeLens` arrays (O(n) per symbol, n≤16).

**Impact**: Performance only. Correctness verified by matching tables.

---

### 5. DCT Coefficient Reading (`ReadDCTCoeffs` vs `read_dct_coeffs`)

#### 5.1 List initialization [OK — matches]

Both: `{4,0}, {24,0}, {44,0}, {1,3}, {2,3}, {3,3}` starting at list position 64.

#### 5.2 Bit precision loop [OK — matches]

Both: `bits = get_bits(4) - 1`, decrementing to 0.

#### 5.3 Mode 0/1/2/3 handling [OK — matches]

Mode 0→split+promote, mode 1→create sub-bands, mode 2→split+clear, mode 3→single coeff.

#### 5.4 Coefficient value reading [OK — matches]

Both: `t = get_bits(bits) | (1 << bits)` with sign. At bits=0: `t = 1 - (get_bits1() << 1)`.

#### 5.5 Quantizer index [OK — matches]

Both: 4-bit quant_idx read after coefficient scan.

---

### 6. Unquantize (`UnquantizeDCTCoeffs` vs `unquantize_dct_coeffs`)

[OK — matches]

Both: `block[0] = (block[0] * quant[0]) >> 11`, then `block[scan[idx]] = (block[scan[idx]] * quant[idx]) >> 11` for each non-zero coeff.

---

### 7. IDCT (`IDCTCol`/`IDCTRow` vs `IDCT_TRANSFORM`)

#### 7.1 Constants [OK — matches]

Both: A1=2896, A2=2217, A3=3784, A4=-5352. `MUL(x,y) = (x*y) >> 11`.

#### 7.2 Column transform [OK — matches]

Both use identical butterfly structure: a0–a7, b0–b4, 8 outputs.

#### 7.3 Row transform [OK — matches]

Both: same butterfly with `(result + 127) >> 8` rounding.

#### 7.4 DC-only optimization [MINOR]

FFmpeg has a shortcut: if all AC coefficients in a column are zero, replicate DC. Ours always does full transform.

**Impact**: Performance only.

---

### 8. ReadResidue (`ReadResidue` vs `read_residue`) [CRITICAL BUG]

**FFmpeg** (line 757–836):
```c
// List-based tree walk, same structure as read_dct_coeffs
coef_list[list_end] =  4; mode_list[list_end++] = 0;
coef_list[list_end] = 24; mode_list[list_end++] = 0;
coef_list[list_end] = 44; mode_list[list_end++] = 0;
coef_list[list_end] =  0; mode_list[list_end++] = 2;  // NOTE: {0,2} not {1,3}

for (mask = 1 << get_bits(gb, 3); mask; mask >>= 1) {
    // Refine existing non-zero coefficients: ±mask
    for (i = 0; i < nz_coeff_count; i++) {
        if (!get_bits1(gb)) continue;
        if (block[nz_coeff[i]] < 0)
            block[nz_coeff[i]] -= mask;
        else
            block[nz_coeff[i]] += mask;
        masks_count--;
        if (masks_count < 0) return 0;
    }
    // Tree walk to place new coefficients at ±mask
    list_pos = list_start;
    while (list_pos < list_end) {
        // ... same mode 0/1/2/3 structure ...
        // Values placed as: (mask ^ sign) - sign  (i.e. +mask or -mask)
        // into block[bink_scan[ccoef]]
        // Tracked in nz_coeff[] for future refinement
    }
}
```

Key details:
- Uses `int16_t block[64]` (not int32_t)
- Initial list: `{4,0}, {24,0}, {44,0}, {0,2}` (different from DCT's `{1,3},{2,3},{3,3}`)
- Mask starts at `1 << get_bits(3)` (power of 2), halves each pass
- `masks_count` is decremented per coefficient; early exit when < 0
- Existing coefficients are refined by ±mask each pass (convergent bit-plane coding)
- Values placed via `bink_scan[]`

**Ours** (line 723–755):
```cpp
int bits = br.Read(4);  // WRONG: should be Read(3), and used as 1<<bits
for (int mask = 0; mask < maskCount; mask++) {
    // Refine existing (close, but uses (1^s)-s instead of sign-dependent ±1)
    uint32_t maskedBits = br.Read(bits);  // WRONG: flat bit mask, not tree walk
    for (int i = 0; i < 64; i++) {
        if ((maskedBits >> (i & 31)) & 1)  // WRONG: linear scan, not list-based
            nz_coeff[nz_coeff_count++] = kBinkScan[i];
    }
}
```

**Problems**:
1. No list-based tree walk — uses flat bit mask instead of mode 0/1/2/3 structure
2. Reads 4 bits for mask init (`Read(4)`) instead of 3 bits (`1 << Read(3)`)
3. No mask halving (power-of-2 convergence)
4. No `masks_count` tracking or early exit
5. Coefficient placement doesn't use the tree walk at all
6. Uses int32_t instead of int16_t

**Impact**: Block artifacts on any frame containing RESIDUE blocks (type 4). These are motion-compensated blocks with small corrections — common in inter-frames.

**Fix**: Complete rewrite using list-based tree walk. See "Corrected ReadResidue" section below.

---

### 9. Block Decoding

#### 9.1 SKIP (type 0) [OK — matches]

Both: copy 8 rows of 8 bytes from prev.

#### 9.2 SCALED (type 1) [MINOR BUG — sub-type validation]

FFmpeg: Only allows sub-types RUN(3), INTRA(5), FILL(6), PATTERN(8), RAW(9). Returns error for others (SKIP, MOTION, RESIDUE, INTER are invalid as sub-types).

Ours: Passes any sub-type to `DecodeBlock`, which could mishandle MOTION/RESIDUE/INTER with wrong prev data in the ublock context.

#### 9.3 MOTION (type 2) [OK — matches]

Both: `prev + xoff + yoff * stride`, bounds check, memcpy.

#### 9.4 RUN (type 3) [MINOR BUG — loop termination]

FFmpeg: `do { ... } while (i < 63); if (i == 63) fill_last;`
Ours: `do { ... } while (i < 64);` with silent clamp on i > 64.

FFmpeg handles the 64th pixel as a special case outside the run loop.

#### 9.5 RESIDUE (type 4) [CRITICAL — see Bug 2 above]

#### 9.6 INTRA (type 5) [OK — matches]

Both: DC from bundle, read_dct_coeffs, unquantize, idct_put.

#### 9.7 FILL (type 6) [OK — matches]

Both: single color, memset 8 rows.

#### 9.8 INTER (type 7) [OK — matches]

Both: motion copy, DC from bundle, read_dct_coeffs, unquantize, idct_add.

#### 9.9 PATTERN (type 8) [OK — matches]

Both: 2 colors, 8 pattern bytes, bit-indexed fill (LSB first).

#### 9.10 RAW (type 9) [OK — matches]

Both: 64 color values from bundle.

---

### 10. YUV→RGB Conversion

**FFmpeg**: Outputs YUV420P planes. Marks revision < 'k' as MPEG range (limited: Y 16–235).

**Ours**: Full-range BT.601 conversion:
```
R = (256*Y + 359*(V-128) + 128) >> 8     // = Y + 1.402*(V-128)
G = (256*Y - 88*(U-128) - 183*(V-128) + 128) >> 8
B = (256*Y + 454*(U-128) + 128) >> 8
```

**Status**: Undetermined. Need to test MPEG-range coefficients after fixing bugs 1 and 2, then compare visually. The init fill of Y=0 for black (not Y=16) suggests full range may be correct for the internal codec, with FFmpeg's MPEG tag being a hint for downstream consumers rather than a statement about the codec's internal range.

---

## Corrected ReadResidue (pseudocode)

```
ReadResidue(br, int16_t block[64], int masks_count):
    coef_list[128], mode_list[128]
    list_start = 64, list_end = 64
    nz_coeff[64], nz_coeff_count = 0

    // Different initial list from read_dct_coeffs
    push {4, mode=0}, {24, mode=0}, {44, mode=0}, {0, mode=2}

    mask = 1 << br.Read(3)    // NOTE: 3 bits, not 4
    while mask > 0:
        // Refine existing non-zero coefficients
        for each nz_coeff[i]:
            if br.Read1():
                if block[nz_coeff[i]] < 0: block[nz_coeff[i]] -= mask
                else:                      block[nz_coeff[i]] += mask
                masks_count--
                if masks_count < 0: return

        // Tree walk to place new coefficients
        list_pos = list_start
        while list_pos < list_end:
            if (mode==0 && coef==0) or !br.Read1(): skip
            switch mode:
                case 0: promote to mode 1, fallthrough to case 2
                case 2: clear entry, for 4 children:
                    if br.Read1(): prepend as mode 3
                    else:
                        nz_coeff[count++] = bink_scan[ccoef]
                        sign = -br.Read1()
                        block[bink_scan[ccoef]] = (mask ^ sign) - sign
                        masks_count--; if < 0: return
                case 1: create 3 sub-bands, promote to mode 2
                case 3:
                    nz_coeff[count++] = bink_scan[ccoef]
                    sign = -br.Read1()
                    block[bink_scan[ccoef]] = (mask ^ sign) - sign
                    clear entry; masks_count--; if < 0: return
        mask >>= 1   // halve mask each pass
```

## Priority Fix Order

1. **BUG 1**: Plane offset seek → skip (primary chroma corruption cause)
2. **BUG 2 + 3**: Rewrite ReadResidue with list-based tree walk using int16_t
3. **BUG 5**: RUN_BLOCK loop termination
4. **BUG 8**: Scaled block sub-type validation
5. **BUG 6**: YUV color range (test after other fixes)
