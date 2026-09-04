#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "aether/c_api.h"

#define AETHER_COMPILE_ASSERT(name, expression) typedef char name[(expression) ? 1 : -1]
AETHER_COMPILE_ASSERT(aether_status_abi_changed, AETHER_OK == 0 && AETHER_ERR_INTERNAL == 6);
AETHER_COMPILE_ASSERT(aether_config_size_changed, sizeof(aether_config_t) == 16);
AETHER_COMPILE_ASSERT(aether_crc_offset_changed, offsetof(aether_config_t, enable_crc) == 12);
AETHER_COMPILE_ASSERT(aether_index_offset_changed, offsetof(aether_config_t, enable_index) == 13);

int main(void) {
    const size_t count = 8192;
    float* input = (float*)malloc(count * sizeof(float));
    float* decoded = (float*)malloc(count * sizeof(float));
    assert(input != NULL && decoded != NULL);
    for (size_t i = 0; i < count; ++i)
        input[i] = sinf((float)i * 0.017f) + 0.1f * cosf((float)i * 0.071f);

    const aether_config_t config = {4.0f, 0.001f, 0.3f, 1, 1};
    uint8_t probe = 0;
    size_t encoded_size = 0;
    assert(aether_compress(input, count, &config, &probe, 0, &encoded_size) ==
           AETHER_ERR_BUFFER_TOO_SMALL);
    assert(encoded_size > 64);

    uint8_t* encoded = (uint8_t*)malloc(encoded_size);
    assert(encoded != NULL);
    size_t bytes_written = 0;
    assert(aether_compress(input, count, &config, encoded, encoded_size, &bytes_written) ==
           AETHER_OK);
    assert(bytes_written == encoded_size);

    size_t samples_written = 0;
    assert(aether_decompress(encoded, encoded_size, decoded, count, &samples_written) == AETHER_OK);
    assert(samples_written == count);
    for (size_t i = 0; i < count; ++i) assert(fabsf(input[i] - decoded[i]) <= 0.00101f);

    float slice[97];
    assert(aether_decompress_slice(encoded, encoded_size, 2040, 97, slice, 97, &samples_written) ==
           AETHER_OK);
    assert(samples_written == 97);
    assert(memcmp(slice, decoded + 2040, sizeof(slice)) == 0);

    encoded[68] ^= 1;
    assert(aether_decompress(encoded, encoded_size, decoded, count, &samples_written) ==
           AETHER_ERR_CORRUPTED_STREAM);
    assert(aether_c_abi_version() == AETHER_C_ABI_VERSION);
    assert(strcmp(aether_version_string(), "0.0.1") == 0);

    free(encoded);
    free(decoded);
    free(input);
    return 0;
}
