# AetherStream Wire Format v6

Status: stable AetherStream 0.0.1 interchange specification. Multi-byte integers and IEEE-754
binary32 values are serialized **little-endian**. Readers must use unaligned
loads or byte assembly; casting wire pointers to scalar pointers is forbidden.

## Compatibility

AetherStream 0.0.1 writes wire version `6`. Version 6 retains the version-5
64-byte stream header and aligned block records, and standardizes the
CRC-protected AIDX footer used by cached indexed views.

The compatibility policy is explicit:

- unindexed wire-v5 streams remain readable and may be decoded directly;
- indexed wire-v5 streams are rejected with `DeprecatedWireFormatException`
  because earlier development builds emitted incompatible length-only and
  CRC-trailer footer variants under the same version number;
- indexed v5 data must be fully decoded with its matching historical release
  and re-encoded as v6;
- versions below 5 and above 6 raise `UnsupportedWireFormatException`;
- unknown flag bits are corruption regardless of version.

Versions 2 and 3 used native-endian fields without complete metadata integrity.
Development version 4 used unaligned records and raw error varints. They require
the corresponding historical decoder before migration.

## Stream header (64 bytes)

| Offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 4 | u32 | Magic `0x41455448` (`48 54 45 41` on wire) |
| 4 | 2 | u16 | Wire version, currently `6` |
| 6 | 2 | u16 | Reserved; zero |
| 8 | 8 | u64 | Total reconstructed sample count |
| 16 | 4 | u32 | Number of blocks |
| 20 | 4 | u32 | Global flags |
| 24 | 40 | bytes | Zero padding |

Global flag bit 0 selects strict error-bounded blocks. Bit 1 declares the AIDX
random-access footer described below. Bits 2 through 31 are reserved and must be
zero. The block count must equal
`ceil(total_samples / 2048)`.

## Block record and alignment

In wire v6, every block begins at a stream-relative 64-byte boundary. A record is:

1. `body_bytes` (u32)
2. `body_bytes` bytes of block body
3. CRC32-C (u32) over the block body only
4. zero padding through the next 64-byte boundary

`body_bytes` is in `[28, 16777216]`. Padding is not covered by CRC but readers
must reject non-zero padding. The first block starts at offset 64. This gives a
formal 64-byte block alignment guarantee while all individual fields remain
safe to read unaligned.

## Block body

| Body offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 4 | u32 | Reconstructed samples, 1 through 2048 |
| 4 | 1 | u8 | Predictor: 0 constant, 1 harmonic, 2 linear |
| 5 | 1 | u8 | Quantizer: 0 rate-targeted, 1 error-bounded |
| 6 | 2 | u16 | Reserved block flags; zero |
| 8 | 4 | f32 | Harmonic `cos(theta)`; finite and within `[-0.999,0.999]` |
| 12 | 4 | f32 | GED alpha or error lattice delta |
| 16 | 4 | f32 | GED beta; `[0.2,5]` in rate mode |
| 20 | 2 | u16 | Number of reconstruction levels; zero in error mode |
| 22 | 2 | u16 | Reserved extension; zero |
| 24 | 4 | u32 | Entropy payload byte count |
| 28 | 4*K | f32[K] | Reconstruction levels in symbol order |
| 28+4*K | payload bytes | bytes | Payload described below |

The payload must consume the remainder of the block body exactly. Codebook
values and all reconstructed values must be finite.

## Predictor state

Predictor state resets at every block. Sample zero predicts zero, and sample
one predicts reconstructed sample zero. Later samples use the selected mode:

- constant: `p = previous`
- harmonic: `p = 2*cos(theta)*previous - previous_two`
- linear: `p = 2*previous - previous_two`

Encoder and decoder update state only with reconstructed samples.

## 16-way rANS payload

All integer fields below are little-endian. The payload consists of:

| Size | Meaning |
|---:|---|
| 4 | rANS magic `0x31525341` |
| 8 | Decoded symbol/token byte count |
| 2 | Active-symbol count `K`, 1 through 256 |
| 3*K | `(symbol: u8, normalized frequency: u16)` entries |
| 16*(4+4) | Final state and renormalization-word count per lane |
| variable | Lane-major little-endian u16 renormalization words |

Symbols in the sparse table are unique, frequencies are non-zero, and their
sum is exactly 4096. States are interleaved by
`position modulo 16`, use a lower bound of 65536, and must return to that lower
bound with every declared word consumed after decoding. Trailing data,
underflow, and inconsistent final states are corruption.

## Rate-targeted payload

The rANS output symbols index the serialized reconstruction-level array.
Symbol zero is the dead-zone reconstruction (`0.0`). The complete aligned block
record, including metadata, codebook, entropy table, states, payload, CRC, and
padding, is constrained by the cumulative statutory wire budget. The 64-byte
stream header is included in the whole-stream budget. If fixed framing alone
cannot fit (notably very short inputs at low rates), encoding fails with
`RateBudgetExceeded`; it never silently exceeds the requested rate.

## Error-bounded payload

`delta = 2*absolute_error`. Closed-loop residual quanta are
`q = round((sample-prediction)/delta)`. Quanta become a byte-token stream:

- tag `0`, followed by unsigned LEB128: positive run length of zero quanta
- tag `1`, followed by unsigned LEB128: non-zero zig-zag encoded signed quantum

The token byte stream is compressed by the same 16-way rANS container. LEB128
is limited to five bytes and canonical semantic constraints are enforced
(non-zero runs and non-zero tag-1 quanta). This mode guarantees
`abs(original-reconstructed) <= absolute_error` or rejects an unrepresentable
input scale.

## Random-access index footer

In wire v6, when global flag bit 1 is set, an index immediately follows the
final aligned block record. It is not padded and has this little-endian layout:

| Size | Type | Meaning |
|---:|---|---|
| 4 | u32 | Entry count, exactly equal to the stream block count |
| 4 | u32 | Index magic `0x41494458` |
| 16*N | entries | One entry per block in sample order |
| 4 | u32 | CRC32-C of the entry count, index magic, and all entries |

Each entry contains `byte_offset` (u64), `start_sample_index` (u32), and
`sample_count` (u32). Offsets are stream-relative, 64-byte aligned, strictly
increasing, and must identify the exact beginning of their corresponding block
record. Sample ranges are contiguous and cover the stream exactly. Indexed
streams are therefore limited to `2^32-1` samples.

The footer size is derived from the block count in the fixed stream header.
`IndexedStreamView` validates the header, footer CRC, and index semantics once,
then binary-searches `start_sample_index` and validates/decodes only intersecting
block records. The convenience `decompress_slice` function creates a temporary
view. Predictor state resets at block boundaries, so no preceding block is
needed. The index header, entries, and CRC count toward the statutory wire-rate
budget.

## Integrity

CRC32-C uses the reflected Castagnoli polynomial `0x82F63B78`, initial state
`0xFFFFFFFF`, and final XOR `0xFFFFFFFF`. Hardware SSE4.2 and ARM CRC paths must
produce the same result as the scalar algorithm; the check value for ASCII
`123456789` is `0xE3069283`.

Readers validate all lengths before allocation, cap incremental buffering,
reject non-finite metadata, and throw `aether::CorruptedStreamException` for
malformed wire data. Version-policy failures use the more specific deprecated
or unsupported exceptions described above.
