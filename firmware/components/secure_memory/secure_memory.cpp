#include "secure_memory/secure_memory.hpp"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "mbedtls/platform_util.h"
#else
#include <malloc.h>
#endif

namespace secure_memory {

void zero(void *storage, std::size_t length) {
    if (storage == nullptr) {
        return;
    }
#ifdef ESP_PLATFORM
    mbedtls_platform_zeroize(storage, length);
#else
    volatile unsigned char *bytes =
        static_cast<volatile unsigned char *>(storage);
    while (length-- > 0) {
        *bytes++ = 0;
    }
#endif
}

void zero_allocation(void *storage) {
    if (storage == nullptr) {
        return;
    }
#ifdef ESP_PLATFORM
    const std::size_t length = heap_caps_get_allocated_size(storage);
#else
    const std::size_t length = malloc_usable_size(storage);
#endif
    zero(storage, length);
}

}  // namespace secure_memory
