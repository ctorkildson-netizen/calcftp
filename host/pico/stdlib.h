/* the bits of the Pico SDK the server uses, for a PC */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
typedef uint64_t absolute_time_t;
static inline absolute_time_t get_absolute_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
static inline absolute_time_t make_timeout_time_ms(unsigned ms) { return get_absolute_time() + ms * 1000ull; }
static inline bool time_reached(absolute_time_t t) { return get_absolute_time() >= t; }
static inline void sleep_ms(unsigned ms) { usleep(ms * 1000); }
