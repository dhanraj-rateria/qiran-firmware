#ifndef QIRAN_TYPES_H
#define QIRAN_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    QIRAN_OK = 0,
    QIRAN_ERR_PARAM,
    QIRAN_ERR_STATE,
    QIRAN_ERR_TIMEOUT,
    QIRAN_ERR_HARDWARE,
    QIRAN_ERR_RANGE,
    QIRAN_ERR_INTEGRITY,
    QIRAN_ERR_BUSY,
    QIRAN_ERR_UNSUPPORTED
} qiran_status_t;

typedef enum {
    QIRAN_HEALTH_HEALTHY = 0,
    QIRAN_HEALTH_FAILED
} qiran_health_t;

typedef enum {
    QIRAN_PAYLOAD_NON_OPERATIONAL = 0,
    QIRAN_PAYLOAD_OPERATIONAL
} qiran_payload_status_t;

/* Compile-time assertion usable at file scope in C99. */
#define QIRAN_STATIC_ASSERT(cond, tag) \
    typedef char qiran_static_assert_##tag[(cond) ? 1 : -1]

#define QIRAN_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define QIRAN_UNUSED(x) ((void)(x))

#endif
