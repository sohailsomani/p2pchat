#include "app.h"
#include "log.h"
#include <stdlib.h>

int main(int argc, char *argv[]) {
  int ret = EXIT_FAILURE;

  if (argc == 1) {
    LOG_ERROR("Usage: %s <fingerprint>", argv[0]);
    return EXIT_FAILURE;
  }

  ApplicationConfig cfg;
  const int base = 10;
  cfg.fingerprint = strtol(argv[1], NULL, base);
  struct Application *app = app_new(&cfg);
  if (app) {
    ret = app_run(app);
    app_free(app);
  }
  return ret;
}
