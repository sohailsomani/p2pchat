#pragma once

#include <stdint.h>

#define CAST(target, value) ((target)value)
#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))

typedef uint16_t fingerprint_t;
