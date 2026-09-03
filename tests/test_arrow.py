import aether
import numpy as np
import pytest
from aetherstream.arrow import compress_arrow_array, decompress_arrow_buffer

pa = pytest.importorskip("pyarrow")


def test_arrow_chunked_roundtrip_without_intermediate_value_copy():
    time = np.arange(20_000, dtype=np.float32)
    values = (np.sin(time * 0.013) + 0.1 * np.cos(time * 0.071)).astype(np.float32)
    first = pa.array(values[:12_000], type=pa.float32())
    second = pa.array(values[12_000:], type=pa.float32())
    chunked = pa.chunked_array([first, second])

    # Arrow exposes both source chunks as views over their primitive buffers.
    assert first.to_numpy(zero_copy_only=True).ctypes.data == first.buffers()[1].address
    assert second.to_numpy(zero_copy_only=True).ctypes.data == second.buffers()[1].address

    encoded = compress_arrow_array(chunked, target_rate=4.0)
    decoded = decompress_arrow_buffer(encoded, len(values))
    assert isinstance(decoded, pa.Array)
    assert decoded.type == pa.float32()
    assert decoded.null_count == 0

    expected = np.concatenate(
        [
            aether.decompress(aether.compress(values[:12_000], 4.0), 12_000),
            aether.decompress(aether.compress(values[12_000:], 4.0), 8_000),
        ]
    )
    actual = decoded.to_numpy(zero_copy_only=True)
    assert actual.ctypes.data == decoded.buffers()[1].address
    np.testing.assert_array_equal(actual, expected)


def test_arrow_bridge_rejects_type_nulls_and_wrong_length():
    with pytest.raises(TypeError, match="float32"):
        compress_arrow_array(pa.array([1.0, 2.0], type=pa.float64()))
    with pytest.raises(ValueError, match="null"):
        compress_arrow_array(pa.array([1.0, None, 2.0], type=pa.float32()))

    values = pa.array(np.linspace(0, 1, 4096, dtype=np.float32))
    encoded = compress_arrow_array(values, target_rate=4.0)
    with pytest.raises(ValueError, match="length"):
        decompress_arrow_buffer(encoded, len(values) - 1)
