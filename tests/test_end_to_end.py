import aether
import aetherstream
import numpy as np
import pytest


def test_roundtrip_quality_and_arbitrary_length():
    time = np.arange(10_137, dtype=np.float32) / 1000.0
    samples = (0.3 * np.sin(3.0 * time) + np.sin(17.0 * time) + 0.02 * np.cos(79.0 * time)).astype(
        np.float32
    )

    encoded = aether.compress(samples, target_rate=4.0, deadzone=0.3)
    reconstructed = aether.decompress(encoded, len(samples))

    assert reconstructed.dtype == np.float32
    assert reconstructed.shape == samples.shape
    assert np.isfinite(reconstructed).all()
    assert np.mean((samples - reconstructed) ** 2) < 0.05


def test_crc_rejects_corruption():
    samples = np.linspace(-1.0, 1.0, 4096, dtype=np.float32)
    encoded = bytearray(aether.compress(samples, 3.0))
    # Offset 68 is the first CRC-protected block-body byte. The stream tail
    # may be alignment padding and is validated separately by native tests.
    encoded[68] ^= 0x40

    with pytest.raises(aetherstream.CorruptedStreamError, match="CRC32-C"):
        aether.decompress(bytes(encoded), len(samples))


def test_ged_api():
    random = np.random.default_rng(4)
    residuals = random.normal(0.0, 3.0, 200_000).astype(np.float32)
    alpha, beta = aether.estimate_ged(residuals)

    assert abs(beta - 2.0) < 0.12
    assert abs(alpha - 3.0 * np.sqrt(2.0)) < 0.12


def test_error_bounded_and_fragmented_streaming():
    random = np.random.default_rng(19)
    samples = (np.cumsum(random.normal(0, 0.01, 20_003)) + np.sin(np.arange(20_003) * 0.13)).astype(
        np.float32
    )
    encoded = aether.compress(samples, absolute_error=0.005)
    reconstructed = aether.decompress(encoded, len(samples))
    assert np.max(np.abs(samples - reconstructed)) <= 0.005001

    encoder = aether.StreamEncoder(target_rate=3.0)
    wire = bytearray()
    for begin in range(0, len(samples), 137):
        wire += encoder.feed(samples[begin : begin + 137])
    wire += encoder.flush()

    decoder = aether.StreamDecoder()
    pieces = []
    for begin in range(0, len(wire), 83):
        piece = decoder.feed(bytes(wire[begin : begin + 83]))
        if len(piece):
            pieces.append(piece)
    decoder.flush()
    streamed = np.concatenate(pieces)
    batch = aether.decompress(aether.compress(samples, 3.0), len(samples))
    np.testing.assert_array_equal(streamed, batch)


def test_rejects_multidimensional_input():
    with pytest.raises(ValueError, match="one-dimensional"):
        aether.compress(np.zeros((2, 4), dtype=np.float32))


def test_exact_wire_rate_and_infeasible_short_budget():
    time = np.arange(32_768, dtype=np.float32)
    samples = (np.sin(time * 0.013) + 0.02 * np.cos(time * 0.19)).astype(np.float32)
    for rate in (2.0, 3.0, 4.0):
        encoded = aether.compress(samples, target_rate=rate)
        assert len(encoded) * 8 <= len(samples) * rate
        assert len(aether.decompress(encoded, len(samples))) == len(samples)

    with pytest.raises(aetherstream.RateBudgetError, match="rate budget"):
        aether.compress(np.zeros(8, dtype=np.float32), target_rate=2.0)
    assert issubclass(aetherstream.RateBudgetError, aetherstream.StreamError)


def test_indexed_random_access_matches_full_decode():
    time = np.arange(15_777, dtype=np.float32)
    samples = (np.sin(time * 0.021) + 0.2 * np.cos(time * 0.17)).astype(np.float32)
    encoded = aetherstream.compress(samples, target_rate=4.0, enable_index=True)
    full = aetherstream.decompress(encoded, len(samples))
    view = aetherstream.IndexedStreamView(encoded)
    assert view.sample_count == len(samples)
    assert view.block_count == (len(samples) + 2047) // 2048
    for start, count in ((0, 1), (2031, 80), (4096, 4097), (15_700, 77), (15_777, 0)):
        sliced = aetherstream.decompress_slice(encoded, start, count)
        cached = view.decompress_slice(start, count)
        np.testing.assert_array_equal(sliced, full[start : start + count])
        np.testing.assert_array_equal(cached, sliced)


def test_wire_v6_and_v5_migration_policy():
    samples = np.linspace(-1.0, 1.0, 4096, dtype=np.float32)
    unindexed = bytearray(aetherstream.compress(samples))
    assert int.from_bytes(unindexed[4:6], "little") == 6
    unindexed[4:6] = (5).to_bytes(2, "little")
    assert len(aetherstream.decompress(bytes(unindexed), len(samples))) == len(samples)

    indexed = bytearray(aetherstream.compress(samples, enable_index=True))
    indexed[4:6] = (5).to_bytes(2, "little")
    with pytest.raises(
        aetherstream.DeprecatedWireFormatError,
        match="Wire format v5 indexed footers are deprecated",
    ):
        aetherstream.decompress(bytes(indexed), len(samples))
    with pytest.raises(aetherstream.DeprecatedWireFormatError):
        aetherstream.IndexedStreamView(bytes(indexed))
    assert issubclass(
        aetherstream.DeprecatedWireFormatError,
        aetherstream.CorruptedStreamError,
    )
    assert issubclass(aetherstream.CorruptedStreamError, aetherstream.StreamError)

    extended_header = bytearray(unindexed)
    extended_header[6:8] = (1).to_bytes(2, "little")
    with pytest.raises(aetherstream.CorruptedStreamError, match="header extension"):
        aetherstream.decompress(bytes(extended_header), len(samples))
    with pytest.raises(aetherstream.CorruptedStreamError, match="header extension"):
        aetherstream.StreamDecoder().feed(bytes(extended_header))

    for version in (4, 7):
        unsupported = bytearray(unindexed)
        unsupported[4:6] = version.to_bytes(2, "little")
        with pytest.raises(aetherstream.UnsupportedWireFormatError):
            aetherstream.decompress(bytes(unsupported), len(samples))


def test_adaptive_tail_trigger_and_distortion_guard():
    random = np.random.default_rng(41)
    samples = random.normal(0.0, 0.02, 16_384).astype(np.float32)
    samples[7000:7006] = np.array([8.0, -7.0, 9.0, -8.0, 7.0, -9.0], dtype=np.float32)
    diagnostics = aether._analyze_impulsive_blocks(samples)
    assert diagnostics["triggered_blocks"] >= 1

    adaptive = aether.decompress(
        aether.compress(samples, target_rate=4.0, adaptive_tail=True), len(samples)
    )
    baseline = aether.decompress(
        aether.compress(samples, target_rate=4.0, adaptive_tail=False), len(samples)
    )
    adaptive_mse = np.mean((samples.astype(np.float64) - adaptive) ** 2)
    baseline_mse = np.mean((samples.astype(np.float64) - baseline) ** 2)
    assert adaptive_mse <= baseline_mse


def test_error_mode_entropy_codes_zero_runs():
    samples = np.zeros(100_000, dtype=np.float32)
    samples[::10_000] = 1.0
    encoded = aether.compress(samples, absolute_error=0.001)
    reconstructed = aether.decompress(encoded, len(samples))
    assert np.max(np.abs(samples - reconstructed)) <= 0.001001
    assert len(encoded) < samples.nbytes // 20
