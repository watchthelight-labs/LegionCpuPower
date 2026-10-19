// <wtl> project: legioncpupower | file: tools/spin.c | author: watchthelight </wtl>
// spin <threads> <seconds>: integer work on N threads for S seconds; prints total and per-thread Mops/s (the throughput
// number cpuload.py compares before and after a CPU power fix).
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static volatile int stop;
static double secs;
typedef struct { uint64_t ops; } res_t;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void *work(void *arg) {
    res_t *r = arg; uint64_t x = (uintptr_t)arg | 1, n = 0;
    while (!stop) {
        for (int i = 0; i < 4096; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; x = x * 0x9E3779B97F4A7C15ull + i; }
        n += 4096;
    }
    r->ops = n + (x & 1);
    return NULL;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1; secs = argc > 2 ? atof(argv[2]) : 10;
    pthread_t *t = calloc(n, sizeof *t); res_t *r = calloc(n, sizeof *r);
    double t0 = now();
    for (int i = 0; i < n; i++) pthread_create(&t[i], NULL, work, &r[i]);
    while (now() - t0 < secs) { struct timespec s = {0, 50000000}; nanosleep(&s, NULL); }
    stop = 1; double dt = now() - t0; uint64_t tot = 0;
    for (int i = 0; i < n; i++) { pthread_join(t[i], NULL); tot += r[i].ops; }
    printf("threads %d seconds %.1f total_mops %.1f per_thread_mops %.1f\n", n, dt, tot / dt / 1e6, tot / dt / 1e6 / n);
    return 0;
}
