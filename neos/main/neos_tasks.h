/*
 * The task list, the kill button behind it, and background services.
 *
 * Three things that look separate and are not. A service is a task, so it
 * appears in the list; the list is the only place a service can be stopped
 * from, so the kill button has to know which rows are services; and a service
 * keeps its app's ELF image loaded, so whatever decides when an image may be
 * freed is also whatever counts services. Splitting them across files would be
 * three files sharing one piece of state.
 *
 * The app-facing half - neos_tasks(), neos_task_kill(), neos_service_start()
 * and the reasoning behind each - is in neos_api.h. What is here is what the
 * firmware needs and apps must not have.
 */
#pragma once

#include <stdbool.h>

#include "esp_elf.h"

#include "neos_api.h"

/**
 * Hand an app's relocated image over to its services.
 *
 * Called by the app runner once main() has returned and neos_service_count() is
 * not zero. The image is copied into a keeper slot and @p elf is zeroed, so the
 * caller's esp_elf_deinit() becomes the no-op it needs to be rather than
 * freeing the pages the services are executing out of. The keeper frees the
 * image when the last service holding it has gone.
 *
 * False if there is no free slot, which leaves @p elf untouched and the caller
 * holding a decision: freeing an image under a live service is a jump into
 * whatever the allocator does with those pages next, so the services have to be
 * stopped instead.
 */
bool neos_service_adopt_image(esp_elf_t *elf);

/**
 * Stop every service and wait for them to be gone.
 *
 * The way out of the case above, and of a card being pulled from under one.
 * Cooperative first, the way neos_task_kill() is, then deleted outright.
 */
void neos_service_stop_all(void);
