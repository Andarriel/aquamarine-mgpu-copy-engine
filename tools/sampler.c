// Minimal perf_event_open CPU sampler: samples user-space IPs + callchains of a pid,
// prints raw addresses; symbolization done by a python script using /proc/pid/maps.
#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

static long perf_event_open(struct perf_event_attr* a, pid_t pid, int cpu, int gfd, unsigned long flags) {
    return syscall(__NR_perf_event_open, a, pid, cpu, gfd, flags);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s pid seconds [callchain_depth]\n", argv[0]); return 1; }
    pid_t pid = atoi(argv[1]);
    int secs = atoi(argv[2]);
    int depth = argc > 3 ? atoi(argv[3]) : 0;

    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_SOFTWARE;
    attr.config = PERF_COUNT_SW_CPU_CLOCK;
    attr.sample_period = 250000; // 250us of on-cpu time per sample
    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | (depth ? PERF_SAMPLE_CALLCHAIN : 0);
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.disabled = 1;
    attr.inherit = 0;
    if (depth) attr.sample_max_stack = depth;

    int fd = perf_event_open(&attr, pid, -1, -1, PERF_FLAG_FD_CLOEXEC);
    if (fd < 0) { perror("perf_event_open"); return 1; }

    size_t pages = 1 + 256; // 1MB ring
    size_t mmlen = pages * 4096;
    void* base = mmap(NULL, mmlen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }
    struct perf_event_mmap_page* meta = base;
    uint8_t* data = (uint8_t*)base + 4096;
    size_t dlen = mmlen - 4096;

    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint64_t tail = 0;
    long nsamples = 0, lost = 0;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= secs) break;
        __sync_synchronize();
        uint64_t head = meta->data_head;
        __sync_synchronize();
        while (tail < head) {
            struct perf_event_header h;
            // copy header (may wrap)
            for (size_t i = 0; i < sizeof(h); i++) ((uint8_t*)&h)[i] = data[(tail + i) % dlen];
            uint8_t buf[8192];
            size_t sz = h.size < sizeof(buf) ? h.size : sizeof(buf);
            for (size_t i = 0; i < sz; i++) buf[i] = data[(tail + i) % dlen];
            if (h.type == PERF_RECORD_SAMPLE) {
                uint64_t* p = (uint64_t*)(buf + sizeof(h));
                uint64_t ip = *p++;
                uint32_t pidv = (uint32_t)(*p & 0xffffffff), tid = (uint32_t)(*p >> 32);
                p++;
                (void)pidv;
                printf("S %u %lx", tid, (unsigned long)ip);
                if (depth) {
                    uint64_t nr = *p++;
                    for (uint64_t i = 0; i < nr && i < 64; i++) printf(" %lx", (unsigned long)p[i]);
                }
                printf("\n");
                nsamples++;
            } else if (h.type == PERF_RECORD_LOST) {
                lost++;
            }
            tail += h.size;
        }
        __sync_synchronize();
        meta->data_tail = tail;
        usleep(2000);
    }
    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
    fprintf(stderr, "samples=%ld lost=%ld\n", nsamples, lost);
    return 0;
}
