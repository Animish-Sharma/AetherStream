#ifndef AETHER_C_API_H
#define AETHER_C_API_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(AETHER_C_STATIC)
#define AETHER_API
#elif defined(AETHER_C_BUILD)
#define AETHER_API __declspec(dllexport)
#else
#define AETHER_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define AETHER_API __attribute__((visibility("default")))
#else
#define AETHER_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AETHER_C_ABI_VERSION 1u

typedef enum aether_status {
    AETHER_OK = 0,
    AETHER_ERR_INVALID_ARG = 1,
    AETHER_ERR_BUFFER_TOO_SMALL = 2,
    AETHER_ERR_CORRUPTED_STREAM = 3,
    AETHER_ERR_RATE_BUDGET_EXCEEDED = 4,
    AETHER_ERR_DEPRECATED_FORMAT = 5,
    AETHER_ERR_INTERNAL = 6
} aether_status;

typedef struct aether_config_t {
    float target_rate;
    float absolute_error_bound;
    float deadzone_factor;
    uint8_t enable_crc;
    uint8_t enable_index;
} aether_config_t;

AETHER_API aether_status aether_compress(const float* input, size_t input_count,
                                         const aether_config_t* config, uint8_t* output,
                                         size_t output_capacity, size_t* bytes_written);

AETHER_API aether_status aether_decompress(const uint8_t* input, size_t input_bytes, float* output,
                                           size_t output_capacity, size_t* samples_written);

AETHER_API aether_status aether_decompress_slice(const uint8_t* input, size_t input_bytes,
                                                 size_t start_sample, size_t count, float* output,
                                                 size_t output_capacity, size_t* samples_written);

AETHER_API uint32_t aether_c_abi_version(void);
AETHER_API const char* aether_version_string(void);
AETHER_API const char* aether_status_to_string(aether_status status);

#ifdef __cplusplus
}
#endif

#endif
