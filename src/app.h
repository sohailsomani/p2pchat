#pragma once

#include "types.h"

typedef struct {
  fingerprint_t fingerprint;
} ApplicationConfig;

struct Application *app_new(ApplicationConfig * cfg);
void app_free(struct Application *app);

int app_run(struct Application *app);
