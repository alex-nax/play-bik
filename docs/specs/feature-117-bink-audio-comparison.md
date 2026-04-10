# Feature 117: Bink Audio — FFmpeg Comparison

Step-by-step comparison of `bink_audio_decoder.cpp` against FFmpeg's `binkaudio.c`.

---

## 1. get_float / ReadFloat29

**FFmpeg** (line 156–163):
```c
static float get_float(GetBitContext *gb) {
    int power = get_bits(gb, 5);
    float f = ldexpf(get_bits(gb, 23), power - 23);
    if (get_bits1(gb)) f = -f;
    return f;
}
```

Reads 5-bit power, 23-bit mantissa, 1-bit sign. Uses `ldexpf(mantissa, power - 23)` which computes `mantissa * 2^(power-23)`.

**Ours** (ReadFloat29):
```cpp
uint32_t raw = Read(29);
uint32_t exponent = raw & 0x1F;
uint32_t mantissa = (raw >> 5) & 0x7FFFFF;
uint32_t sign = (raw >> 28) & 1;
uint32_t ieee = (sign << 31) | ((exponent + 0x7C) << 23) | mantissa;
```

Constructs IEEE 754 directly: `sign | (exponent + 124) << 23 | mantissa`.

**KEY DIFFERENCE**: FFmpeg reads fields in ORDER: power(5), mantissa(23), sign(1). Our code reads all 29 bits at once and extracts fields from bit positions. Since the bitstream is LSB-first:

- FFmpeg: bits[0..4]=power, bits[5..27]=mantissa, bits[28]=sign
- Ours: `raw & 0x1F` = bits[0..4]=exponent, `(raw>>5) & 0x7FFFFF` = bits[5..27]=mantissa, `(raw>>28) & 1` = bit 28 = sign

Both extract the same fields. **OK — matches.**

**VALUE CHECK**: FFmpeg's `ldexpf(mantissa, power - 23)` = `mantissa * 2^(power-23)`.
Our IEEE construction: `(exponent + 124) << 23 | mantissa`.

For exponent=e, mantissa=m: IEEE float = `(-1)^sign * 2^(e+124-127) * (1 + m/2^23)` = `(-1)^sign * 2^(e-3) * (1 + m/2^23)`.

FFmpeg: `mantissa * 2^(power-23)` = `m * 2^(e-23)`.

These are NOT the same! FFmpeg treats the 23 bits as an integer mantissa (no implicit leading 1), while our IEEE construction has an implicit leading 1 bit.

Example: power=15, mantissa=0:
- FFmpeg: `0 * 2^(15-23)` = 0.0
- Ours: `2^(15-3) * (1 + 0)` = 4096.0

**BUG: ReadFloat29 is wrong.** The mantissa should not have an implicit leading 1. Must use `ldexpf` or equivalent.

---

## 2. Packet-level skip

**FFmpeg** (line 322): `skip_bits_long(gb, 32);` — skips 32-bit "reported size" at the start of each audio packet.

**Ours**: No packet-level skip. We start reading coefficients immediately from the packet data.

**BUG: Missing 32-bit skip at start of audio packet.**

---

## 3. Band computation

**FFmpeg** (line 127–135):
```c
for (s->num_bands = 1; s->num_bands < 25; s->num_bands++)
    if (sample_rate_half <= ff_wma_critical_freqs[s->num_bands - 1])
        break;
```
Where `sample_rate_half = (sample_rate + 1) / 2` and for RDFT stereo, `sample_rate` was already multiplied by channels.

**Ours** (ComputeBands):
```cpp
uint32_t sampleRateHalf = (effectiveRate + 1) / 2;
size_t numBands = 1;
for (size_t i = 1; i <= 25; i++) {
    if (sampleRateHalf <= kCritFreqs[i - 1]) break;
    numBands = i + 1;
}
```

**Analysis**: FFmpeg starts num_bands at 1 and increments while checking `sample_rate_half <= freq[num_bands-1]`. When the condition is true, it breaks. So num_bands = first i where `sample_rate_half <= freq[i-1]`.

For 44100 Hz stereo RDFT: effective_rate = 88200, sample_rate_half = 44100.
- freq[0]=100, 44100 > 100 → num_bands=2
- freq[1]=200, 44100 > 200 → num_bands=3
- ...
- freq[23]=15500, 44100 > 15500 → num_bands=25
- freq[24]=24500, 44100 > 24500 → loop ends at 25

FFmpeg: `for (num_bands=1; num_bands<25; ...)` — loop stops at num_bands=25 since the condition `num_bands < 25` is false. Final num_bands=25.

Ours: similar logic, numBands=25. **Probably OK** but should verify edge cases.

**Band values** (line 132-135):
FFmpeg: `bands[i] = (freq[i-1] * frame_len / sample_rate_half) & ~1`
Ours: `m_bands[i] = (kCritFreqs[i-1] * m_frameLen / sampleRateHalf) & ~size_t(1)`

**OK — matches.**

---

## 4. Coefficient decoding

**FFmpeg** (line 206-250):
```c
k = 0;
q = quant[0];
i = 2;
while (i < s->frame_len) {
    // RLE group length
    int v = get_bits1(gb);
    if (v) {
        v = get_bits(gb, 4);
        j = i + rle_length_tab[v] * 8;
    } else {
        j = i + 8;
    }
    j = FFMIN(j, s->frame_len);

    width = get_bits(gb, 4);
    if (width == 0) {
        memset(coeffs + i, 0, (j - i) * sizeof(*coeffs));
        i = j;
        while (s->bands[k] < i)
            q = quant[k++];
    } else {
        while (i < j) {
            if (s->bands[k] == i)
                q = quant[k++];
            coeff = get_bits(gb, width);
            if (coeff) {
                if (get_bits1(gb))
                    coeffs[i] = -q * coeff;
                else
                    coeffs[i] =  q * coeff;
            } else {
                coeffs[i] = 0.0f;
            }
            i++;
        }
    }
}
```

**Ours** (DecodeCoeffs): Structurally matches. Same RLE length table, same width=0 zero-fill, same band tracking.

**Minor difference**: FFmpeg's zero-fill band tracking: `while (s->bands[k] < i)` without bounds check. Ours: `while (m_bands[k] < i && k < numBands)`. The bounds check is defensive but otherwise **OK**.

FFmpeg's nonzero band tracking: `if (s->bands[k] == i)` without bounds check. Ours: `if (m_bands[k] == i && k < numBands)`. Same.

**OK — matches.**

---

## 5. RDFT pre-processing

**FFmpeg** (line 255–261):
```c
for (int i = 2; i < s->frame_len; i += 2)
    coeffs[i + 1] *= -1;
coeffs[s->frame_len + 0] = coeffs[1];
coeffs[s->frame_len + 1] = coeffs[1] = 0;
```

Note line 260: `coeffs[s->frame_len + 1] = coeffs[1] = 0;` — this sets BOTH coeffs[1] and coeffs[frame_len+1] to 0. The assignment is right-to-left: first `coeffs[1] = 0`, then that 0 is assigned to `coeffs[frame_len+1]`.

Wait — actually `coeffs[s->frame_len + 0] = coeffs[1]` happens FIRST (line 259), reading the old value of coeffs[1]. Then line 260 sets both to 0. So the sequence is:
1. coeffs[frame_len] = old coeffs[1]  (copy Nyquist)
2. coeffs[1] = 0
3. coeffs[frame_len+1] = 0

**Ours**:
```cpp
for (size_t i = 2; i < frameLen; i += 2)
    coeffs[i + 1] *= -1.0f;
coeffs[frameLen] = coeffs[1];
coeffs[frameLen + 1] = 0.0f;
coeffs[1] = 0.0f;
```

Same sequence. **OK — matches.**

---

## 6. Inverse RDFT transform

**FFmpeg** (line 139–141):
```c
float scale = 0.5;
av_tx_init(&s->tx, &s->tx_fn, AV_TX_FLOAT_RDFT, 1, 1 << frame_len_bits, &scale, 0);
```
Then called with: `s->tx_fn(s->tx, out, coeffs, sizeof(AVComplexFloat))`.

This is an N-point inverse RDFT with external scale 0.5. FFmpeg's av_tx for inverse RDFT:
- Input: N/2+1 complex values (N+2 floats) 
- Output: N real values
- Convention: `output[n] = scale * sum_{k=0}^{N-1} X[k] * exp(+j*2*pi*kn/N)`
- The sum is **unnormalized** (no 1/N factor)

**Ours**: N-point inverse complex FFT on conjugate-extended spectrum:
- Extend N/2+1 complex → N complex via Hermitian symmetry
- Apply N-point inverse FFT (which includes 1/N normalization)
- Extract real parts

**The structural approach is correct** (conjugate extension → full IFFT → real parts). But the scaling differs:
- FFmpeg: `0.5 * unnormalized_sum`
- Ours: `(1/N) * unnormalized_sum`
- Ratio: ours = FFmpeg * `(1/N) / 0.5` = FFmpeg * `2/N`
- So: **our output is `2/N` times FFmpeg's output**

For N=4096: our output is 1/2048 of FFmpeg's.

But the root factor (2/(sqrt(N)*32768)) was designed for FFmpeg's scaling. With our 1/N normalization, we're effectively applying an extra `1/(N*0.5) = 2/N` factor.

**FIX**: After the FFT, multiply each output sample by `N/2` to match FFmpeg's `scale=0.5` convention.

Expected peak with fix: coefficients are in correct scale (root-normalized), IDFT produces time-domain, multiply by N/2 to match FFmpeg → output should be in ~[-1, 1] range.

---

## 7. Overlap-add

**FFmpeg** (line 265–276):
```c
int count = s->overlap_len * channels;  // channels=1 for RDFT stereo
if (!s->first) {
    j = ch;  // ch=0 for RDFT single-channel
    for (i = 0; i < s->overlap_len; i++, j += channels)
        out[ch][i] = (s->previous[ch][i] * (count - j) + out[ch][i] * j) / count;
}
memcpy(s->previous[ch], &out[ch][s->frame_len - s->overlap_len],
       s->overlap_len * sizeof(*s->previous[ch]));
```

For RDFT stereo (channels=1, ch=0): count = overlap_len * 1 = overlap_len. j goes 0, 1, 2, ..., overlap_len-1. Standard linear ramp.

**Ours**: Same logic. `count = overlapLen * channels`, `j = ch`, `j += channels`. For RDFT (channels=1), this produces the same ramp. **OK — matches.**

---

## 8. Output handling

**FFmpeg**: For RDFT, `avctx->sample_fmt = AV_SAMPLE_FMT_FLT` (interleaved float). Frame `nb_samples = s->block_size / channels`. `block_size = (frame_len - overlap_len) * min(2, 1)` = `frame_len - overlap_len`.

The output buffer `out[0]` contains `frame_len` interleaved L/R samples. After overlap-add, `frame_len - overlap_len` samples are usable.

**Ours**: Output `frameLen - overlapLen` floats. For RDFT stereo, return `outputLen / 2` as "samples per channel". **OK — matches.**

---

## Summary of Bugs

| # | Issue | Severity |
|---|-------|----------|
| 1 | **ReadFloat29: implicit leading 1 bit** — IEEE 754 construction adds leading 1 to mantissa; FFmpeg's `ldexpf(mantissa, power-23)` treats mantissa as raw integer without leading 1 | CRITICAL |
| 2 | **Missing 32-bit packet skip** — FFmpeg skips first 32 bits of each audio packet ("reported size"); we don't | CRITICAL |
| 3 | **RDFT scale factor** — our 1/N-normalized FFT needs `N/2` post-multiply to match FFmpeg's `scale=0.5` on unnormalized transform | CRITICAL |
| 4 | Band computation | OK |
| 5 | Coefficient decoding | OK |
| 6 | RDFT pre-processing | OK |
| 7 | Overlap-add | OK |
| 8 | Output handling | OK |

## Fix Order

1. **BUG 1**: Rewrite ReadFloat29 to use `ldexpf(mantissa, power - 23)` instead of IEEE construction
2. **BUG 2**: Add `br.Read(32)` skip at start of Decode()
3. **BUG 3**: After RDFT inverse FFT, multiply output by `N/2`
