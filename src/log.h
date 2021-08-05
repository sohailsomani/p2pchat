#pragma once

#include <stdio.h>

#define LOG_DEBUG(msg, ...)                                                    \
  do {                                                                         \
    if (getenv("P2P_DEBUG")) /* NOLINT(concurrency-mt-unsafe)  */              \
      (void)fprintf(stderr, "DEBUG: " msg "\n", __VA_ARGS__);                  \
  } while (0)

#define LOG_DEBUG0(msg)                                                        \
  do {                                                                         \
    if (getenv("P2P_DEBUG")) /* NOLINT(concurrency-mt-unsafe) */               \
      (void)fprintf(stderr, "DEBUG: " msg "\n");                               \
  } while (0)

#define LOG_INFO(msg, ...) (void)fprintf(stderr, "INFO: " msg "\n", __VA_ARGS__)
#define LOG_INFO0(msg) (void)fprintf(stderr, "INFO: " msg "\n");

#define LOG_WARNING(msg, ...)                                                  \
  (void)fprintf(stderr, "WARNING: " msg "\n", __VA_ARGS__)
#define LOG_WARNING0(msg) (void)fprintf(stderr, "WARNING: " msg "\n");

#define LOG_ERROR(msg, ...)                                                    \
  (void)fprintf(stderr, "ERROR: " msg "\n", __VA_ARGS__)
#define LOG_ERROR0(msg) (void)fprintf(stderr, "ERROR: " msg "\n");
