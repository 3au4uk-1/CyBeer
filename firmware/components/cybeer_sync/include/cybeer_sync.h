#pragma once

#include "cybeer_storage.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cybeer_sync_init(void);
esp_err_t cybeer_sync_enqueue_run(const cybeer_run_t *run);
esp_err_t cybeer_sync_enqueue_participant(const char *device_id, const char *name);

#ifdef __cplusplus
}
#endif
