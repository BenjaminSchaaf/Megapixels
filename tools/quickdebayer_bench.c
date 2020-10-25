#include "quickdebayer.h"
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

#define WIDTH 2592
#define HEIGHT 1944
#define SCALE 2
#define BLACKLEVEL 0
#define BENCH_COUNT 10

double get_time()
{
    struct timeval t;
    struct timezone tzp;
    gettimeofday(&t, &tzp);
    return t.tv_sec + t.tv_usec*1e-6;
}

int main(int argc, char *argv[]) {
    srand(time(NULL));

    size_t size = WIDTH * HEIGHT * 2;
    uint8_t *buf = malloc(sizeof(uint8_t) * size);
    for (size_t i = 0; i < size; ++i) {
        buf[i] = rand();
    }

    double start = get_time();
    for (size_t i = 0; i < BENCH_COUNT; ++i) {
        uint32_t *dest = malloc(sizeof(uint32_t) * WIDTH * HEIGHT / SCALE);
        quick_debayer_bggr8(buf, (uint8_t *)dest, WIDTH, HEIGHT, SCALE, BLACKLEVEL);
        free(dest);
    }
    double end = get_time();
    printf("Benchmark took %fms per run\n", (end - start) / BENCH_COUNT * 1000);

    free(buf);
}
