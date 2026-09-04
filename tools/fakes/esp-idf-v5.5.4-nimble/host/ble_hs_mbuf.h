#pragma once

#ifndef S3_HIDBOT_U76A_NIMBLE_FAKE
#error "The U7.6A NimBLE fake is test-only"
#endif

#include <stdint.h>

struct os_mbuf;

#ifdef __cplusplus
extern "C" {
#endif

int ble_hs_mbuf_to_flat(const struct os_mbuf *mbuf, void *flat,
                        uint16_t maximum_length, uint16_t *copied_length);

#ifdef __cplusplus
}
#endif
