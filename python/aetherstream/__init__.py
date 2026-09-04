"""Public Python API for AetherStream."""

from aether import (
    CorruptedStreamError,
    DeprecatedWireFormatError,
    IndexedStreamView,
    RateBudgetError,
    StreamDecoder,
    StreamEncoder,
    StreamError,
    UnsupportedWireFormatError,
    __version__,
    compress,
    decompress,
    decompress_slice,
    estimate_ged,
)

__all__ = [
    "CorruptedStreamError",
    "DeprecatedWireFormatError",
    "IndexedStreamView",
    "RateBudgetError",
    "StreamDecoder",
    "StreamEncoder",
    "StreamError",
    "UnsupportedWireFormatError",
    "__version__",
    "compress",
    "decompress",
    "decompress_slice",
    "estimate_ged",
]
