package aether

import (
	"errors"
	"math"
	"testing"
)

func deterministicWave(count int) []float32 {
	values := make([]float32, count)
	for i := range values {
		x := float64(i)
		values[i] = float32(math.Sin(x*0.017) + 0.2*math.Cos(x*0.071))
	}
	return values
}

func TestRoundTripAndIndexedSlice(t *testing.T) {
	input := deterministicWave(16384)
	cfg := DefaultConfig()
	cfg.AbsoluteErrorBound = 0.001
	cfg.EnableIndex = true
	encoded, err := Compress(input, cfg)
	if err != nil {
		t.Fatal(err)
	}
	decoded, err := Decompress(encoded, len(input))
	if err != nil {
		t.Fatal(err)
	}
	for i := range input {
		if delta := math.Abs(float64(input[i] - decoded[i])); delta > 0.00101 {
			t.Fatalf("sample %d exceeded error bound: %g", i, delta)
		}
	}
	slice, err := DecompressSlice(encoded, 2030, 128)
	if err != nil {
		t.Fatal(err)
	}
	for i := range slice {
		if slice[i] != decoded[2030+i] {
			t.Fatalf("slice mismatch at %d", i)
		}
	}
	if ABIVersion() != 1 {
		t.Fatalf("unexpected ABI version %d", ABIVersion())
	}
	if Version() != "0.0.1" {
		t.Fatalf("unexpected version %q", Version())
	}
}

func TestCorruptionReturnsTypedError(t *testing.T) {
	var target *StatusError
	_, err := Decompress([]byte("not an aether stream"), 4)
	if !errors.As(err, &target) {
		t.Fatalf("expected StatusError, got %T: %v", err, err)
	}
	if target.Status != 3 {
		t.Fatalf("expected corrupted-stream status, got %d", target.Status)
	}
}

func BenchmarkCompress(b *testing.B) {
	input := deterministicWave(65536)
	cfg := DefaultConfig()
	b.ReportAllocs()
	b.SetBytes(int64(len(input) * 4))
	for i := 0; i < b.N; i++ {
		if _, err := Compress(input, cfg); err != nil {
			b.Fatal(err)
		}
	}
}
