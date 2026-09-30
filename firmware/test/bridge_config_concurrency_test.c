#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "bridge_config.h"

static bridge_config_t a, b;
static atomic_int failures;

static void *write_config(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 20000; i++)
        if (!bridge_config_set((i & 1u) ? &a : &b)) atomic_fetch_add(&failures, 1);
    return NULL;
}

static void *read_config(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 20000; i++) {
        bridge_config_t cfg;
        bridge_config_get(&cfg);
        if (!bridge_config_validate(&cfg) ||
            (memcmp(&cfg, &a, sizeof cfg) != 0 && memcmp(&cfg, &b, sizeof cfg) != 0))
            atomic_fetch_add(&failures, 1);
    }
    return NULL;
}

int main(void)
{
    bridge_config_defaults(&a);
    b = a;
    a.left_deadzone = 123;
    a.right_deadzone = 456;
    b.left_deadzone = 789;
    b.right_deadzone = 321;
    b.button_map[BRIDGE_SRC_C] = BRIDGE_BUTTON_DISABLED;
    bridge_config_finalize(&a);
    bridge_config_finalize(&b);
    if (!bridge_config_set(&a)) return 1;

    pthread_t writer, reader;
    if (pthread_create(&writer, NULL, write_config, NULL) != 0 ||
        pthread_create(&reader, NULL, read_config, NULL) != 0) return 2;
    pthread_join(writer, NULL);
    pthread_join(reader, NULL);
    printf("bridge config concurrency: %s\n", atomic_load(&failures) ? "FAIL" : "PASS");
    return atomic_load(&failures) != 0;
}
