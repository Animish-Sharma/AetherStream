// Package aether provides native AetherStream compression through its stable C ABI.
package aether

/*
#cgo CFLAGS: -I${SRCDIR}/../../../include
#cgo linux LDFLAGS: -L${SRCDIR}/../../../build -laether_c -Wl,-rpath,${SRCDIR}/../../../build
#cgo darwin LDFLAGS: -L${SRCDIR}/../../../build -laether_c -Wl,-rpath,${SRCDIR}/../../../build
#cgo windows LDFLAGS: -L${SRCDIR}/../../../build -L${SRCDIR}/../../../build/Release -laether_c
#include "aether/c_api.h"
*/
import "C"

import (
	"fmt"
	"math"
	"unsafe"
)

// Config controls compression. CRC validation is always enabled by wire format v6.
type Config struct {
	TargetRate         float32
	AbsoluteErrorBound float32
	DeadzoneFactor     float32
	EnableCRC          bool
	EnableIndex        bool
}

// DefaultConfig returns the native codec defaults.
func DefaultConfig() Config {
	return Config{TargetRate: 4, DeadzoneFactor: 0.3, EnableCRC: true}
}

// Status is a stable C ABI result code.
type Status int

const (
	StatusOK                 Status = 0
	StatusInvalidArgument    Status = 1
	StatusBufferTooSmall     Status = 2
	StatusCorruptedStream    Status = 3
	StatusRateBudgetExceeded Status = 4
	StatusDeprecatedFormat   Status = 5
	StatusInternal           Status = 6
)

// StatusError is returned for errors reported by the C ABI.
type StatusError struct {
	Status Status
	Text   string
}

func (e *StatusError) Error() string {
	return fmt.Sprintf("aetherstream: %s (status %d)", e.Text, e.Status)
}

func statusError(status C.aether_status) error {
	if status == C.AETHER_OK {
		return nil
	}
	return &StatusError{Status: Status(status), Text: C.GoString(C.aether_status_to_string(status))}
}

func boolByte(value bool) C.uint8_t {
	if value {
		return 1
	}
	return 0
}

// Compress encodes samples without copying the input slice.
func Compress(samples []float32, cfg Config) ([]byte, error) {
	if len(samples) == 0 {
		return nil, &StatusError{Status: StatusInvalidArgument, Text: "input is empty"}
	}
	if cfg.TargetRate < 2 || cfg.TargetRate > 8 || math.IsNaN(float64(cfg.TargetRate)) ||
		cfg.AbsoluteErrorBound < 0 || math.IsNaN(float64(cfg.AbsoluteErrorBound)) ||
		math.IsInf(float64(cfg.AbsoluteErrorBound), 0) || cfg.DeadzoneFactor < 0 ||
		math.IsNaN(float64(cfg.DeadzoneFactor)) || math.IsInf(float64(cfg.DeadzoneFactor), 0) {
		return nil, &StatusError{Status: StatusInvalidArgument, Text: "invalid configuration"}
	}

	native := C.aether_config_t{
		target_rate:          C.float(cfg.TargetRate),
		absolute_error_bound: C.float(cfg.AbsoluteErrorBound),
		deadzone_factor:      C.float(cfg.DeadzoneFactor),
		enable_crc:           boolByte(cfg.EnableCRC),
		enable_index:         boolByte(cfg.EnableIndex),
	}
	capacity := 0
	maxInt := int(^uint(0) >> 1)
	if cfg.AbsoluteErrorBound == 0 {
		capacity = int(math.Ceil(float64(len(samples)) * float64(cfg.TargetRate) / 8.0))
	} else if len(samples) <= (maxInt-65536)/16 {
		capacity = len(samples)*16 + 65536
	} else {
		return nil, &StatusError{Status: StatusInvalidArgument, Text: "input is too large"}
	}
	if capacity < 1 {
		capacity = 1
	}
	output := make([]byte, capacity)
	var written C.size_t
	status := C.aether_compress(
		(*C.float)(unsafe.Pointer(unsafe.SliceData(samples))),
		C.size_t(len(samples)),
		&native,
		(*C.uint8_t)(unsafe.Pointer(unsafe.SliceData(output))),
		C.size_t(len(output)),
		&written,
	)
	if status == C.AETHER_ERR_BUFFER_TOO_SMALL {
		if uint64(written) > uint64(maxInt) {
			return nil, &StatusError{Status: StatusInternal, Text: "output is too large"}
		}
		output = make([]byte, int(written))
		status = C.aether_compress(
			(*C.float)(unsafe.Pointer(unsafe.SliceData(samples))), C.size_t(len(samples)), &native,
			(*C.uint8_t)(unsafe.Pointer(unsafe.SliceData(output))), C.size_t(len(output)), &written,
		)
	}
	if err := statusError(status); err != nil {
		return nil, err
	}
	return output[:int(written)], nil
}

// Decompress decodes a complete frame and verifies its sample count.
func Decompress(data []byte, expectedSamples int) ([]float32, error) {
	if len(data) == 0 || expectedSamples < 0 {
		return nil, &StatusError{Status: StatusInvalidArgument, Text: "invalid input length"}
	}
	capacity := expectedSamples
	if capacity == 0 {
		capacity = 1
	}
	output := make([]float32, capacity)
	var written C.size_t
	status := C.aether_decompress(
		(*C.uint8_t)(unsafe.Pointer(unsafe.SliceData(data))), C.size_t(len(data)),
		(*C.float)(unsafe.Pointer(unsafe.SliceData(output))), C.size_t(expectedSamples), &written,
	)
	if err := statusError(status); err != nil {
		return nil, err
	}
	if uint64(written) != uint64(expectedSamples) {
		return nil, &StatusError{Status: StatusInvalidArgument, Text: "unexpected sample count"}
	}
	return output[:expectedSamples], nil
}

// DecompressSlice decodes a range from an indexed frame.
func DecompressSlice(data []byte, start int, count int) ([]float32, error) {
	if len(data) == 0 || start < 0 || count < 0 {
		return nil, &StatusError{Status: StatusInvalidArgument, Text: "invalid slice range"}
	}
	capacity := count
	if capacity == 0 {
		capacity = 1
	}
	output := make([]float32, capacity)
	var written C.size_t
	status := C.aether_decompress_slice(
		(*C.uint8_t)(unsafe.Pointer(unsafe.SliceData(data))), C.size_t(len(data)),
		C.size_t(start), C.size_t(count),
		(*C.float)(unsafe.Pointer(unsafe.SliceData(output))), C.size_t(count), &written,
	)
	if err := statusError(status); err != nil {
		return nil, err
	}
	return output[:count], nil
}

// ABIVersion returns the stable native ABI generation.
func ABIVersion() uint32 { return uint32(C.aether_c_abi_version()) }

// Version returns the native library version.
func Version() string { return C.GoString(C.aether_version_string()) }
