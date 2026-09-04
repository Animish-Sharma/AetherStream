use aetherstream::{
    abi_version, compress, decompress, decompress_slice, version, AetherError, Config,
};

fn wave(count: usize) -> Vec<f32> {
    (0..count)
        .map(|index| {
            let x = index as f32;
            (x * 0.017).sin() + 0.2 * (x * 0.071).cos()
        })
        .collect()
}

#[test]
fn bounded_roundtrip_and_native_slice_are_bit_exact() {
    let input = wave(16_777);
    let config = Config::builder()
        .absolute_error_bound(0.001)
        .enable_index(true)
        .build()
        .unwrap();
    let encoded = compress(&input, &config).unwrap();
    let decoded = decompress(&encoded, input.len()).unwrap();
    for (original, reconstructed) in input.iter().zip(&decoded) {
        assert!((original - reconstructed).abs() <= 0.00101);
    }

    let sliced = decompress_slice(&encoded, 2031, 4097).unwrap();
    for (from_slice, from_full) in sliced.iter().zip(&decoded[2031..6128]) {
        assert_eq!(from_slice.to_bits(), from_full.to_bits());
    }
    assert_eq!(abi_version(), 1);
    assert_eq!(version(), "0.0.1");
}

#[test]
fn corrupted_input_is_a_typed_error() {
    let error = decompress(b"not an aether frame", 12).unwrap_err();
    assert_eq!(error, AetherError::CorruptedStream);
}
