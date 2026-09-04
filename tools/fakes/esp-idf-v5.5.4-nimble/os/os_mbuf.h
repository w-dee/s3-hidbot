#pragma once

#ifndef S3_HIDBOT_U76A_NIMBLE_FAKE
#error "The U7.6A NimBLE fake is test-only"
#endif

#include <stdbool.h>
#include <stdint.h>

// The leading fields mirror the v5.5.4 os_mbuf concepts used by this test.
// Test-only trailing fields provide deterministic flatten fault injection;
// production code can observe them only through the real NimBLE API surface.
struct os_mbuf {
    uint8_t *om_data;
    uint8_t om_flags;
    uint8_t om_pkthdr_len;
    uint16_t om_len;
    void *om_omp;
    struct os_mbuf *om_next;

    uint16_t test_packet_length;
    int test_flatten_status;
    bool test_override_flatten_length;
    uint16_t test_flatten_length;
};

// v5.5.4 reads the head packet header's total omp_len through this macro.
#define OS_MBUF_PKTLEN(mbuf) ((mbuf)->test_packet_length)

#ifdef __cplusplus
extern "C" {
#endif

int os_mbuf_append(struct os_mbuf *mbuf, const void *data, uint16_t length);

#ifdef __cplusplus
}
#endif
