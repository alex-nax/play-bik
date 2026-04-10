# Feature 117: Bink Audio Decoder

## Summary

Clean-room decoder for the Bink Audio codec used in NOLF's .bik cutscene videos. Transforms compressed frequency-domain audio frames back to PCM samples using inverse DCT-II or inverse RDFT, with overlap-add windowing for frame continuity.

## Context

- **Depends on**: F115 (Bink container demuxer — provides compressed audio packets and track metadata)
- **Depended on by**: F33 (video playback integration — feeds decoded PCM to the audio system)

NOLF ships 4 intro .bik files using Bink version 1. The container demuxer (F115) extracts per-frame audio packets with track metadata (sample rate, channel count, transform type). This feature decodes those packets to float PCM.

## Design

### Codec Overview

Bink Audio is a perceptual audio codec operating in the frequency domain. Each audio frame is transformed via either DCT-II (per-channel) or RDFT (interleaved stereo), quantized per critical band, and packed into a bitstream. Decoding reverses this: unpack coefficients, dequantize, inverse-transform, then overlap-add with the previous frame for continuity.

The codec is simple compared to MP3 or Vorbis: no Huffman tables, no psychoacoustic model on the decode side, no long/short block switching. Quantization bands correspond loosely to critical bands of human hearing.

### Transform Types

The container header's audio track flags select the transform per track:

| Flag bit 12 | Transform | Usage |
|---|---|---|
| 0 | RDFT (Real DFT) | Older Bink files, stereo interleaving |
| 1 | DCT-II | Standard mode, per-channel processing |

**DCT mode**: Each channel is transformed independently. The inverse DCT-II converts `N/2` frequency coefficients back to `N` time-domain samples per channel.

**RDFT mode**: For stereo, left and right samples are interleaved into a single buffer of length `N` before the forward transform. The inverse RDFT produces interleaved L/R samples that are then deinterleaved. For mono, RDFT behaves like a single-channel real DFT.

NOLF's .bik files use RDFT mode (flag bit 12 = 0, confirmed from container flags 0xE000). The decoder must support both modes for correctness.

### Frame Sizes

Frame size (number of output samples per channel) depends on sample rate:

| Sample rate | Frame size (samples) | Transform size (N/2 coefficients) | Overlap window |
|---|---|---|---|
| < 22050 Hz | 2048 | 1024 | 128 |
| < 44100 Hz | 4096 | 2048 | 256 |
| >= 44100 Hz | 8192 | 4096 | 512 |

The overlap window size is always `frame_size / 16`.

### Coefficient Decoding

Each audio packet contains one frame of compressed audio. The bitstream is read LSB-first. Per channel, the decoding steps are:

**Step 1 — Read initial coefficients.** Two floating-point values are read directly from the bitstream using a packed 29-bit float format:

```
bits [0..4]   = exponent (5 bits, unsigned)
bits [5..27]  = mantissa (23 bits)
bit  [28]     = sign (1 bit)
```

These reconstruct an IEEE 754 float: `sign | (exponent + 0x7C) << 23 | mantissa`. The exponent bias of `0x7C` (124) means the 5-bit range [0..31] maps to IEEE exponents [124..155], covering roughly 2^-3 to 2^28. These two values become `coefficients[0]` and `coefficients[1]`.

**Step 2 — Unpack quantizers.** The frequency spectrum is divided into critical bands. Band boundaries are computed from a table of 25 critical frequencies:

```
100, 200, 300, 400, 510, 630, 770, 920, 1080, 1270,
1480, 1720, 2000, 2320, 2700, 3150, 3700, 4400, 5300, 6400,
7700, 9500, 12000, 15500, 24500
```

Band boundary calculation:

```
bands[0] = 2  (coefficients 0 and 1 are sent directly)
for i in 1..24:
    bands[i] = critical_freq[i-1] * (frame_size / 2) / (sample_rate / 2)
bands[num_bands] = frame_size / 2
```

Bands where `bands[i] >= frame_size / 2` are discarded. For each remaining band, read an 8-bit quantizer index from the bitstream:

```
q_index = read_bits(8)
exponent = min(q_index, 95) * 0.0664
quantizer[band] = pow(10.0, exponent)
```

**Step 3 — Unpack coefficients.** Starting from coefficient index 2, iterate through bands. Within each band, coefficients are decoded in groups of 16 (Bink v1). For each group:

1. Read the bit width `w` needed to represent the largest quantized value in this group
2. For each coefficient in the group, read `w` bits as an unsigned integer
3. If the value is nonzero, read 1 sign bit
4. Dequantize: `coefficient = (signed_value * quantizer[current_band])`

Any remaining coefficients beyond the last full group in a band are zero-filled.

### Inverse Transform

**DCT mode (inverse DCT-II):** Apply an `N/2`-point inverse Type-II DCT to the coefficient array, producing `N/2` time-domain samples per channel. The standard formula applies:

```
x[n] = (2/N) * sum_{k=0}^{N/2-1} w[k] * X[k] * cos(pi * (2n+1) * k / N)
```

where `w[0] = 1/sqrt(2)`, `w[k] = 1` for `k > 0`. In practice, implement via a butterfly-based fast algorithm (split-radix or recursive) rather than the naive O(N^2) sum.

**RDFT mode (inverse real DFT):** The `N/2` coefficients represent the real-valued DFT of `N/2` real samples (or `N` interleaved stereo samples). Apply an `N/2`-point inverse RDFT. The output for stereo is interleaved L/R which must be deinterleaved into separate channel buffers.

Both transforms operate on float arrays. The implementation should use an in-place butterfly algorithm. Power-of-two sizes (1024, 2048, 4096) are guaranteed by the frame size table.

### Windowing & Overlap-Add

Consecutive frames overlap by `window_len = frame_size / 16` samples. After the inverse transform produces a full frame of samples, the first `window_len` samples are blended with the stored tail of the previous frame using a linear crossfade:

```
for i in 0..window_len-1:
    output[i] = (prev_tail[i] * (window_len - i) + current[i] * i) / window_len
```

Samples from index `window_len` to `frame_size - window_len - 1` pass through unchanged.

The last `window_len` samples of the current frame are saved into `prev_tail[]` for blending with the next frame. They are not output — the usable output per frame is `frame_size - window_len` samples (after the first frame which outputs `frame_size - window_len` with silence for the missing previous frame).

The linear crossfade (not a sine or Vorbis window) is confirmed by the MultimediaWiki documentation.

### Stereo Handling

Stereo handling depends on the transform mode:

**DCT mode**: Each channel is decoded independently. The packet contains channel 0 coefficients followed by channel 1 coefficients. Each channel has its own quantizers and coefficient data. Output is two separate float arrays.

**RDFT mode**: Left and right samples are interleaved into a single transform buffer (`L0 R0 L1 R1 ...`). The RDFT operates on this combined buffer. After inverse transform, deinterleave into separate L/R output buffers. Only one set of quantizers/coefficients is decoded (for the combined buffer).

Mono tracks always use a single channel regardless of transform mode.

### Public Interface

```cpp
namespace nolf::formats {

struct BinkAudioTrackInfo {
    uint32_t sample_rate;
    uint8_t  channels;       // 1 = mono, 2 = stereo
    bool     use_dct;        // true = DCT-II, false = RDFT
};

class BinkAudioDecoder {
public:
    explicit BinkAudioDecoder(const BinkAudioTrackInfo& info);

    // Decode one audio packet. Returns decoded PCM samples interleaved
    // (L R L R... for stereo, mono samples for mono).
    // Input: raw audio packet data from the Bink demuxer.
    // Output: float PCM samples in [-1.0, 1.0] range, appended to `out`.
    // Returns number of samples per channel decoded.
    size_t decode(const uint8_t* data, size_t size, std::vector<float>& out);

    size_t frame_size() const;
    size_t overlap_size() const;

    void reset(); // Clear overlap buffer (e.g. on seek)

private:
    BinkAudioTrackInfo m_info;
    size_t m_frameSize;
    size_t m_overlapSize;
    size_t m_numBands;
    std::vector<size_t> m_bands;          // band boundary indices
    std::vector<float> m_prevOverlap;     // per-channel overlap buffers
    std::vector<float> m_coefficients;    // working buffer
    bool m_firstFrame;
};

} // namespace nolf::formats
```

## Files to Create/Modify

| File | Action | Purpose |
|---|---|---|
| `docs/specs/feature-117-bink-audio-decoder.md` | Create | This spec |
| `src/formats/bink_audio_decoder.h` | Create | Public interface, BinkAudioDecoder class |
| `src/formats/bink_audio_decoder.cpp` | Create | Decoder implementation (bitstream, transforms, overlap) |
| `tests/test_bink_audio.cpp` | Create | Unit + integration tests |
| `cmake.toml` | Modify | Add new source files to formats library and test target |

## Acceptance Criteria

1. Decode audio packets from all 4 NOLF .bik files without errors or crashes
2. Output float PCM samples at the correct sample rate as specified by the track header
3. Handle both mono and stereo tracks correctly
4. Support both DCT and RDFT transform modes
5. Overlap-add produces glitch-free transitions between frames
6. Decoded audio is audibly correct when played back (matches original Bink player output)
7. No GPL or LGPL code — clean-room implementation only

## Test Plan

### Unit Tests

- **Bitstream reader**: Verify 29-bit float unpacking produces correct IEEE 754 values for known bit patterns
- **Band calculation**: For 22050 Hz and 44100 Hz sample rates, verify band boundaries match expected values from the critical frequency table
- **Quantizer computation**: Verify `pow(10, min(idx, 95) * 0.0664)` for boundary values (0, 95, 255)
- **Overlap-add**: Feed two synthetic frames through the overlap function, verify crossfade is linear
- **DCT round-trip**: Forward DCT-II a known signal, inverse it, verify reconstruction within float tolerance
- **RDFT round-trip**: Same for RDFT path

### Integration Tests

- **Decode real packets**: Load a .bik file via the demuxer (F115), decode first N audio frames, verify output sample count matches expected `(N * (frame_size - overlap_size))` plus first-frame adjustment
- **Waveform check**: Decode first second of audio, compute RMS energy, verify it is nonzero and within a reasonable range (not clipping, not silence)
- **Full file decode**: Decode all audio from each NOLF .bik file without assertion failures or NaN/Inf in output
- Skip gracefully when `nolf/NOLF.REZ` or .bik files are not present

### Manual Verification

- Play decoded PCM through SDL audio callback, listen for correctness against original Bink player

## Edge Cases & Risks

- **Transform implementation correctness**: The inverse DCT-II and RDFT must be numerically accurate. A naive implementation is O(N^2) and slow; a fast butterfly is needed but must be validated carefully. Consider using a well-tested public-domain FFT library (e.g. pffft or a minimal radix-2 implementation) if writing from scratch proves error-prone.
- **First frame handling**: No previous overlap buffer exists for the first frame. Initialize `prev_tail` to zeros.
- **Packet alignment**: The demuxer provides raw packet bytes; the bitstream reader must handle LSB-first bit ordering correctly.
- **Bink version differences**: NOLF uses Bink v1 which groups coefficients in sets of 16. If any .bik file uses the newer 8-group format, the decoder must detect and handle it. The version byte from the container header (`b` vs later) determines this.
- **Coefficient overflow**: Quantizer values can be large (up to `10^(95*0.0664)` = `10^6.3`). Ensure float range is sufficient (it is — float32 handles up to ~3.4e38).
- **Zero-length audio packets**: Some frames may have zero audio bytes (indicated by length=0 in the container). The decoder should return zero samples without error.
- **Sample rate mismatch**: The decoder trusts the sample rate from the container header. If the audio system expects a different rate, resampling is outside the scope of this feature.
- **Stereo RDFT deinterleaving**: Off-by-one in deinterleaving would swap or corrupt channels. Must be tested with known stereo content.

## Codec Details

### Bitstream Reader

Bink audio packets are read LSB-first (least significant bit first). A bitstream reader maintains a bit position within the packet buffer. Operations needed:

- `read_bits(n)`: Read n bits, return as uint32_t (max 25 bits at a time is sufficient)
- `read_float()`: Read 29 bits, reconstruct IEEE 754 float as described above

### Critical Frequency Table

The 25 critical frequencies in Hz (derived from psychoacoustic critical band boundaries, matching WMA's table):

```
{100, 200, 300, 400, 510, 630, 770, 920, 1080, 1270,
 1480, 1720, 2000, 2320, 2700, 3150, 3700, 4400, 5300, 6400,
 7700, 9500, 12000, 15500, 24500}
```

### Band Boundary Computation

Given `half_rate = (sample_rate + 1) / 2` and `half_frame = frame_size / 2`:

```
bands[0] = 2
for i = 1 to 24:
    b = critical_freq[i-1] * half_frame / half_rate
    if b >= half_frame: break
    bands[num_bands++] = b
bands[num_bands] = half_frame
```

Example for 22050 Hz, frame_size=2048: half_rate=11025, half_frame=1024. Band 1 = `100*1024/11025` = 9, Band 2 = `200*1024/11025` = 18, etc.

### 29-Bit Float Reconstruction

```
raw = read_bits(29)
exponent = raw & 0x1F
mantissa = (raw >> 5) & 0x7FFFFF
sign = (raw >> 28) & 1
ieee = (sign << 31) | ((exponent + 0x7C) << 23) | mantissa
return reinterpret_cast<float>(ieee)
```

The exponent bias `0x7C` (124) restricts the representable range. With 5-bit exponent [0..31], IEEE exponent = [124..155], covering magnitudes from approximately `2^{-3}` to `2^{28}`.

### Coefficient Decoding (Bink v1, group size 16)

```
coeffs[0] = read_float()
coeffs[1] = read_float()

// Unpack quantizers per band
for band in 0..num_bands-1:
    q_idx = read_bits(8)
    quant[band] = pow(10.0f, min(q_idx, 95) * 0.0664f)

// Decode coefficients
coeff_idx = 2
for band in 0..num_bands-1:
    band_end = bands[band + 1]
    while coeff_idx < band_end:
        // Read number of bits for this group of 16
        width = read_bits(4)  // 0..15 bits per value
        if width == 0:
            // All 16 coefficients in this group are zero
            for j in 0..15:
                if coeff_idx + j < band_end:
                    coeffs[coeff_idx + j] = 0.0f
        else:
            for j in 0..15:
                if coeff_idx + j < band_end:
                    val = read_bits(width)
                    if val != 0:
                        sign = read_bits(1)
                        if sign: val = -val
                    coeffs[coeff_idx + j] = val * quant[band]
                    
        coeff_idx += 16
```

### Inverse DCT-II (Fast Algorithm)

For power-of-two sizes, use a split-radix or recursive butterfly. The inverse DCT-II of size N can be computed via:

1. Reorder coefficients with appropriate scaling
2. Apply inverse FFT of size N/2 on reinterpreted data
3. Post-process with twiddle factors

Alternatively, implement Lee's fast DCT algorithm which recursively splits the DCT into two half-size problems. For N=1024, this gives O(N log N) performance.

### Inverse RDFT

The inverse real DFT of N real-valued frequency samples produces N real time-domain samples. Implement via:

1. Pack the N real coefficients into N/2 complex values
2. Apply N/2-point inverse complex FFT
3. Unpack the complex output back to N real samples using symmetry

This requires a complex FFT implementation (radix-2 butterfly) as a building block.

### Output Normalization

After the inverse transform, samples may need scaling to fit the [-1.0, 1.0] float PCM range. The transform normalization factor depends on the implementation. Typical factors:

- Inverse DCT-II: multiply by `2.0 / N` (or `sqrt(2/N)` depending on convention)
- Inverse RDFT: multiply by `1.0 / N`

The exact factor must be determined empirically by comparing decoded output against known-good reference audio. Adjust the normalization so peak amplitude approximately reaches but does not exceed 1.0.

## Additional Format Knowledge

The following tables, constants, and algorithmic details were derived from public Bink
format documentation (MultimediaWiki, Kostya's Boring Codec World). All algorithms
are described in prose and original pseudocode.

### Frame Length Calculation (Corrected)

The spec above lists frame sizes of 2048/4096/8192 samples. Those are the values from
the MultimediaWiki, but the actual calculation uses `frame_len_bits` which produces
smaller base values. The frame_len_bits determines the transform size, and for RDFT
stereo the bits value is further increased.

| Sample Rate | frame_len_bits | Base frame_len | Notes |
|---|---|---|---|
| < 22050 Hz | 9 | 512 | Smallest transform |
| 22050 to 44099 Hz | 10 | 1024 | Standard |
| >= 44100 Hz | 11 | 2048 | Largest transform |

**RDFT stereo adjustment**: For RDFT mode (non-version-b), `frame_len_bits` is
incremented by `log2(channels)`. For stereo (2 channels), this adds 1, doubling the
frame length. The internal sample rate is also multiplied by the channel count, and the
decoder internally treats the stream as mono (processing interleaved L/R as a single
buffer). This means:

| Mode | Channels | Effective frame_len (44100 Hz) |
|---|---|---|
| DCT, mono | 1 | 2048 |
| DCT, stereo | 2 | 2048 (per channel) |
| RDFT, mono | 1 | 2048 |
| RDFT, stereo | 2 | 4096 (interleaved L+R) |

**Reconciliation with MultimediaWiki values**: The wiki lists 2048/4096/8192 which are
exactly 4x the base frame_len values here. This is likely because the wiki counts total
output samples (both channels, both overlap halves) rather than the transform size.

### Overlap Length

Overlap length is always `frame_len / 16`. This produces:

| frame_len | overlap_len |
|---|---|
| 512 | 32 |
| 1024 | 64 |
| 2048 | 128 |
| 4096 (RDFT stereo) | 256 |

The usable output per frame (block_size) is `(frame_len - overlap_len) * channels`.

### Window Function: Linear Crossfade (Confirmed)

The overlap-add uses a simple linear crossfade, not a sine or Hann window. The formula
for the overlap region is:

    for i in 0 to (overlap_len * channels - 1):
        output[i] = (previous[i] * (count - i) + current[i] * i) / count

where `count = overlap_len * channels`.

On the first frame, the crossfade is skipped entirely (the current frame's samples are
used directly with no blending). A `first_frame` flag controls this; after the first
block is decoded, the flag is cleared.

The last `overlap_len` samples (per channel) of each frame are saved for blending with
the next frame's first `overlap_len` samples.

### Quantization Table Construction

The quantization table has 96 entries (indices 0 through 95). The table is precomputed
during decoder initialization using the formula:

    quant_table[i] = exp(i * 0.15289164787221953823) * root

where `root` is the transform-dependent scaling factor (see below). The constant
`0.15289164787221953823` is approximately equal to `0.0664 * ln(10)`, which converts
the base-10 formula `pow(10, i * 0.0664)` into a natural exponential. The slight
difference from the exact mathematical value (`0.0664 * ln(10)` = 0.15289165017...)
suggests the constant was chosen to match the original Bink encoder's precision.

When reading quantizer indices from the bitstream, the index is clamped:
`effective_index = min(raw_8bit_index, 95)`.

By folding the `root` scaling factor into the quantization table at init time, the
decoder avoids a per-coefficient multiply during decoding.

### Transform Scaling Factors

The scaling factor `root` depends on the transform mode:

| Transform | root formula | Purpose |
|---|---|---|
| RDFT | `2.0 / (sqrt(frame_len) * 32768.0)` | Normalizes inverse RDFT output to ~[-1, 1] |
| DCT | `frame_len / (sqrt(frame_len) * 32768.0)` = `sqrt(frame_len) / 32768.0` | Normalizes inverse DCT-III output to ~[-1, 1] |

The 32768.0 divisor converts from 16-bit PCM scale to float [-1, 1] range. These
factors are baked into the quantization table so dequantized coefficients are already
in the correct scale before the inverse transform.

### DCT Mode: Inverse DCT-III (Not DCT-II)

The inverse transform used in DCT mode is a **DCT-III** (the "inverse" of DCT-II).
Before applying the transform, the first coefficient (DC term) is divided by 0.5
(i.e., multiplied by 2). This is the standard DCT-III convention where the DC term
has a different normalization than the AC terms.

### RDFT Mode: Complex-to-Real Direction

The RDFT mode applies an inverse real FFT (complex-to-real direction). The frequency
coefficients represent a half-spectrum of a real signal, and the inverse RDFT
reconstructs the full real-valued time-domain signal.

### Critical Band Boundary Computation (Corrected)

Band boundaries are computed from the WMA critical frequency table with an important
detail: each boundary is forced to an even index using a bitmask:

    sample_rate_half = sample_rate / 2  (integer division)
    bands[0] = 2
    num_bands = 1
    for i in 1 to 24:
        boundary = (critical_freq[i-1] * frame_len / sample_rate_half) & ~1
        if boundary <= bands[num_bands - 1]:
            continue  (skip duplicate/non-increasing boundaries)
        if boundary >= frame_len:
            break
        bands[num_bands] = boundary
        num_bands += 1
    bands[num_bands] = frame_len

The `& ~1` operation clears the lowest bit, rounding down to the nearest even number.
This ensures coefficients are aligned to even indices, which may relate to the
real/imaginary pairing in RDFT mode.

The critical frequency table (25 values, shared with WMA):

| Index | Freq (Hz) | Index | Freq (Hz) | Index | Freq (Hz) |
|---|---|---|---|---|---|
| 0 | 100 | 9 | 1270 | 18 | 5300 |
| 1 | 200 | 10 | 1480 | 19 | 6400 |
| 2 | 300 | 11 | 1720 | 20 | 7700 |
| 3 | 400 | 12 | 2000 | 21 | 9500 |
| 4 | 510 | 13 | 2320 | 22 | 12000 |
| 5 | 630 | 14 | 2700 | 23 | 15500 |
| 6 | 770 | 15 | 3150 | 24 | 24500 |
| 7 | 920 | 16 | 3700 | | |
| 8 | 1080 | 17 | 4400 | | |

### Example Band Counts by Sample Rate

| Sample Rate | frame_len | Approx. num_bands |
|---|---|---|
| 11025 Hz | 512 | ~13 |
| 22050 Hz | 1024 | ~19 |
| 44100 Hz | 2048 | ~24 |

### Coefficient Decoding: Version-b vs Non-Version-b

Two distinct coefficient decoding algorithms exist depending on the Bink version.

**Version-b (and possibly version-d): Fixed 16-sample groups**

1. Read the first two coefficients as full 32-bit IEEE 754 floats (read 32 bits,
   reinterpret as float).
2. Read 8-bit quantizer index per band, build quantizer from precomputed table.
3. Starting at coefficient index 2, iterate in fixed groups of 16:
   a. Read `width` = 4 bits (0-15), the bit count per coefficient value.
   b. If `width` = 0: zero-fill the entire group of 16 coefficients.
   c. If `width` > 0: for each of the 16 coefficients in the group:
      - Read `width` bits as unsigned magnitude.
      - If magnitude != 0, read 1 sign bit. Negate if sign = 1.
      - Multiply by `quant_table[band_index]` to dequantize.
   d. Advance coefficient index by 16. Update band index as needed.
4. Continue until all coefficients up to `frame_len` are filled.

**Non-version-b (later versions): Variable groups of 8 with RLE**

1. Read the first two coefficients as 29-bit packed floats (5-bit exponent, 23-bit
   mantissa, 1-bit sign; see 29-bit float format below).
2. Read 8-bit quantizer index per band, build quantizer from precomputed table.
3. Starting at coefficient index 2, iterate with variable-length groups:
   a. Read 1 bit (RLE flag).
   b. If RLE flag = 1: read 4 bits as index into the RLE length table, multiply
      the table value by 8 to get the group span.
   c. If RLE flag = 0: group span = 8 (single group).
   d. Clamp the group endpoint to not exceed `frame_len`.
   e. Read `width` = 4 bits.
   f. If `width` = 0: zero-fill coefficients from current position to group end.
   g. If `width` > 0: for each coefficient in the group:
      - Read `width` bits as unsigned magnitude.
      - If magnitude != 0, read 1 sign bit.
      - Multiply by `quant_table[band_index]` (band index advances when
        the coefficient position crosses a band boundary).
   h. Advance to the group endpoint.
4. Continue until all coefficients up to `frame_len` are filled.

**RLE Length Table** (16 entries):

| Index | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Value | 2 | 3 | 4 | 5 | 6 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 32 | 64 |

When used as a group span, the value is multiplied by 8, so the actual coefficient
spans range from 16 to 512. Note that index 0 gives span = 16 (matching version-b's
fixed group size), while index 15 gives span = 512.

### 29-Bit Float Format (Non-Version-b)

    raw = read_29_bits_from_bitstream()
    exponent = raw & 0x1F           (bits 0-4: 5-bit unsigned exponent)
    mantissa = (raw >> 5) & 0x7FFFFF (bits 5-27: 23-bit mantissa)
    sign     = (raw >> 28) & 1       (bit 28: sign)
    ieee_bits = (sign << 31) | ((exponent + 0x7C) << 23) | mantissa

The bias offset 0x7C (124) maps the 5-bit exponent range [0, 31] to IEEE 754
exponent field values [124, 155], covering magnitudes from approximately 2^-3 to 2^28.

### 32-Bit Float Format (Version-b)

Simply read 32 bits from the bitstream and reinterpret as an IEEE 754 single-precision
float. No special packing.

### Bit Reader Convention

The bitstream is read in **little-endian** (LSB-first) order. Each byte is consumed
from least-significant bit to most-significant bit.

### Packet Structure Within a Bink Frame

Each Bink video frame contains one audio packet per audio track. The audio packet size
is stored in the Bink container's per-frame index. The packet begins with the
coefficient data for channel 0, followed by channel 1 (for DCT stereo), or a single
interleaved buffer (for RDFT stereo).

There is no sub-packet or sub-frame structure within a single audio packet; each packet
decodes to exactly one audio frame of `frame_len` samples per channel.

### First-Frame Silence Behavior

On the very first audio frame, there is no previous frame to crossfade with. The
decoder skips the overlap-add blending and outputs the current frame's samples
directly. The `prev_overlap` buffer is initialized to zeros. After the first frame is
decoded, its tail samples populate `prev_overlap` for use with the second frame.

### Output Clamping

No explicit clamping to [-1.0, 1.0] is applied after the inverse transform. The
scaling factors baked into the quantization table are designed to produce output in
approximately that range, but transient peaks may exceed it. Downstream audio
processing should handle any necessary clamping.

### NOLF-Specific Notes

NOLF ships Bink files with container revision 'i' (not 'b'). This means:
- Non-version-b coefficient decoding applies (variable groups of 8 with RLE, 29-bit initial floats)
- RDFT mode (flag bit 12 = 0, flags = 0xE000 confirmed from all 4 NOLF .bik files)
- The version-b decoding path (fixed 16-sample groups, 32-bit floats) should still be
  implemented for completeness but is NOT exercised by NOLF's .bik files
- For RDFT stereo at 44100 Hz: frame_len_bits=11+1=12, frame_len=4096 interleaved samples
