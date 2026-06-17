//#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <android/log.h>
#include <pthread.h>
#include "perf.h"
#ifndef __NR_perf_event_open       // 没有定义就手动定义一下
#define __NR_perf_event_open 241   // arm64
#endif
#define LOG_TAG "PerfBP"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
static long perf_event_open(
        struct perf_event_attr *attr,
        pid_t pid,
        int cpu,
        int group_fd,
        unsigned long flags)
{
    return syscall(
            __NR_perf_event_open,
            attr,
            pid,
            cpu,
            group_fd,
            flags
    );
}

typedef void (*sample_cb)(uint32_t pid, uint32_t tid, uint64_t *regs, int reg_count);

typedef struct {
    int fd;
    void *mmap_addr;
    size_t mmap_size;
    int backtrace;
} PerfMapC;

static uint64_t sample_regs_user_mask(void) {
    /*
     * arm64 perf regs:
     * X0-X30, SP, PC, PSTATE
     * 通常可以先只采 PC/SP/X29/X30，减少输出。
     */
   /* return (1ULL << 29) |  // X29 FP
           (1ULL << 30) |   // LR
           (1ULL << 31) |   // SP
           (1ULL << 32);    // PC*/
   // 所有寄存器
   // Need to print all Registers.
    uint64_t mask = 0;
    for (int i = 0; i <= 32; i++) {
        mask |= (1ULL << i);
    }

    return mask;
}

int perfmap_new(PerfMapC *pm,
                uint32_t bp_type,
                uint64_t addr,
                uint64_t len,
                pid_t pid,
                int buf_order,
                int backtrace) {
    memset(pm, 0, sizeof(*pm));
    pm->fd = -1;

    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));

    attr.type = PERF_TYPE_BREAKPOINT;
    attr.size = sizeof(attr);
    attr.sample_period = 1;
    attr.wakeup_events = 1;

    attr.bp_type = bp_type;     // HW_BREAKPOINT_R / W / RW / X
    attr.bp_addr = addr;
    attr.bp_len = len;

    attr.precise_ip = 2;

    attr.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_REGS_USER;
    if (backtrace) {
        attr.sample_type |= PERF_SAMPLE_CALLCHAIN;
        attr.exclude_callchain_kernel = 1;
    }

    attr.sample_regs_user = sample_regs_user_mask();

    int fd = (int)perf_event_open(
            &attr,
            pid,        // 目标 pid；当前进程可用 0
            -1,         // 任意 CPU
            -1,
            PERF_FLAG_FD_CLOEXEC
    );

    if (fd < 0) {
        fprintf(stderr, "perf_event_open failed: %s\n", strerror(errno));
        LOGE("perf_event_open failed: %s\n",strerror(errno));
        return -1;
    }

    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    size_t mmap_size = (1 + (1u << buf_order)) * page_size;

    void *base = mmap(NULL, mmap_size,
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED,
                      fd,
                      0);

    if (base == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    struct perf_event_mmap_page *meta =
            (struct perf_event_mmap_page *)base;

    if (meta->compat_version != 0) {
        fprintf(stderr, "unsupported mmap_page version\n");
        munmap(base, mmap_size);
        close(fd);
        return -1;
    }

    pm->fd = fd;
    pm->mmap_addr = base;
    pm->mmap_size = mmap_size;
    pm->backtrace = backtrace;
    return 0;
}

static uint64_t read_u64_ring(uint8_t *data,
                              uint64_t data_size,
                              uint64_t pos) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        ((uint8_t *)&v)[i] = data[(pos + i) % data_size];
    }
    return v;
}

static uint32_t read_u32_ring(uint8_t *data,
                              uint64_t data_size,
                              uint64_t pos) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        ((uint8_t *)&v)[i] = data[(pos + i) % data_size];
    }
    return v;
}

static void read_bytes_ring(uint8_t *dst,
                            uint8_t *data,
                            uint64_t data_size,
                            uint64_t pos,
                            size_t len) {
    for (size_t i = 0; i < len; i++) {
        dst[i] = data[(pos + i) % data_size];
    }
}

int perfmap_loop(PerfMapC *pm, sample_cb cb) {
    struct perf_event_mmap_page *meta =
            (struct perf_event_mmap_page *)pm->mmap_addr;

    uint8_t *data = (uint8_t *)pm->mmap_addr + meta->data_offset;
    uint64_t data_size = meta->data_size;

    uint64_t tail = meta->data_tail;

    struct pollfd pfd;
    pfd.fd = pm->fd;
    pfd.events = POLLIN;

    while (1) {
        int r = poll(&pfd, 1, -1); // timeout: Infinite
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "poll failed: %s\n", strerror(errno));
            return -1;
        }
        if(r == 0){ continue;}

        uint64_t head = meta->data_head;
        __sync_synchronize();

        while (tail < head) {
            struct perf_event_header hdr;
            read_bytes_ring((uint8_t *)&hdr, data, data_size,
                            tail % data_size, sizeof(hdr));

            uint64_t off = tail + sizeof(hdr);

            if (hdr.type == PERF_RECORD_SAMPLE) {
                uint32_t pid = read_u32_ring(data, data_size, off % data_size);
                off += 4;

                uint32_t tid = read_u32_ring(data, data_size, off % data_size);
                off += 4;

                if (pm->backtrace) {
                    uint64_t nr = read_u64_ring(data, data_size, off % data_size);
                    off += 8;

                    LOGD("callchain count: %llu\n",
                           (unsigned long long)nr);

                    for (uint64_t i = 0; i < nr; i++) {
                        uint64_t ip = read_u64_ring(data, data_size, off % data_size);
                        off += 8;
                        LOGD("  bt[%llu] = 0x%llx\n",
                               (unsigned long long)i,
                               (unsigned long long)ip);
                    }
                }

                /*
                 * PERF_SAMPLE_REGS_USER 格式：
                 * u64 abi;
                 * u64 regs[n];
                 *
                 * Rust 代码里的 offset += 8 就是在跳过 abi。
                 */
                uint64_t abi = read_u64_ring(data, data_size, off % data_size);
                off += 8;
                (void)abi;
                const uint32_t reg_counts = 33;
                uint64_t regs[reg_counts];
                for (int i = 0; i < reg_counts; i++) {
                    regs[i] = read_u64_ring(data, data_size, off % data_size);
                    off += 8;
                }

                if (cb) {
                    cb(pid, tid, regs, reg_counts);
                }

            } else if (hdr.type == PERF_RECORD_LOST) {
                uint64_t id = read_u64_ring(data, data_size, off % data_size);
                off += 8;
                uint64_t lost = read_u64_ring(data, data_size, off % data_size);
                LOGD("lost events: id=%llu lost=%llu\n",
                       (unsigned long long)id,
                       (unsigned long long)lost);
            } else {
                LOGD("unknown perf record type: %u\n", hdr.type);
            }

            tail += hdr.size;
            meta->data_tail = tail;
        }
    }

    return 0;
}

void perfmap_close(PerfMapC *pm) {
    if (pm->mmap_addr && pm->mmap_addr != MAP_FAILED) {
        munmap(pm->mmap_addr, pm->mmap_size);
    }
    if (pm->fd >= 0) {
        close(pm->fd);
    }
}

// 使用
static void on_sample(uint32_t pid, uint32_t tid, uint64_t *regs, int reg_count) {
 /*   LOGD("hit pid=%u tid=%u\n", pid, tid);
    LOGD("x29/fp = 0x%llx\n", (unsigned long long)regs[0]);
    LOGD("lr     = 0x%llx\n", (unsigned long long)regs[1]);
    LOGD("sp     = 0x%llx\n", (unsigned long long)regs[2]);
    LOGD("pc     = 0x%llx\n", (unsigned long long)regs[3]);
*/
    LOGD("hit pid=%u tid=%u\n", pid, tid);
    for (int i = 0; i <= 28; i++) {
        LOGI("x%-2d = 0x%016llx",
             i,
             (unsigned long long)regs[i]);
    }

    LOGI("fp = 0x%016llx",
         (unsigned long long)regs[29]);

    LOGI("lr = 0x%016llx",
         (unsigned long long)regs[30]);

    LOGI("sp = 0x%016llx",
         (unsigned long long)regs[31]);

    LOGI("pc = 0x%016llx",
         (unsigned long long)regs[32]);
}

void test_breakpoint(uint64_t target_addr) {
    PerfMapC pm;

    int ret = perfmap_new(
            &pm,
            HW_BREAKPOINT_X,   // 执行断点
            target_addr,
            4,
            0,                 // 当前进程；监控其他 pid 填目标 pid
            4,            // ring buffer 页数：1 + 16 pages
            0             // 是否采 callchain
    );

    if (ret != 0) return;

    perfmap_loop(&pm, on_sample);
    perfmap_close(&pm);
}
//
// 后台线程，避免卡死.
// Create background thread to avoid freeze.
static PerfMapC g_pm;
static bool isStartedMonitor = false;   // 判断是否开启了监控，不要多次开启
static void *perf_thread_main(void *arg) {
    perfmap_loop(&g_pm, on_sample);
    return NULL;
}
void start_perf(uint64_t target_addr, uint32_t bp_Type) { // HW_BREAKPOINT_X
    if(!isStartedMonitor){
        if (perfmap_new(&g_pm,
                        bp_Type,
                        target_addr,
                        4, /* 如果是内存的话，可以是HW_BREAKPOINT_LEN_4 */
                        0,
                        4,
                        0) != 0) {
            LOGE("perfmap_new failed");
            return;
        }

        pthread_t tid;
        pthread_create(&tid, NULL, perf_thread_main, NULL);
        pthread_detach(tid);
        isStartedMonitor = true;
    }else{
        return;
    }
    return;
}