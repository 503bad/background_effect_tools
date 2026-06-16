#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct bg_effect;

/* All selectable effects, in UI order. */
const struct bg_effect *const *bg_registry(size_t *count);

/* Index of `id` in the registry, or -1. */
int bg_registry_index(const char *id);

#ifdef __cplusplus
}
#endif
