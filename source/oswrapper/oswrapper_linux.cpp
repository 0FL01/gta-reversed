// mad-sa Linux native track (R1 skeleton).
// POSIX implementation of the OS abstraction declared in `oswrapper.h`.
// Windows track (`oswrapper_win.cpp`) is untouched; this file is compiled
// only into the native `mad-sa-linux` executable (never into the ASI/DLL).
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <thread>
#include <unistd.h>

// --- Basic RE types (mirrors `source/Base.h`, standalone-safe) ---
using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;

#ifndef __stdcall
#define __stdcall
#endif

#include "oswrapper.h"

namespace {
char s_BasePath[1024] = { 0 };

void BuildPath(const char* file, char* out, size_t outSize) {
    assert(file && out);
    if (s_BasePath[0] == '\0') {
        (void)snprintf(out, outSize, "%s", file);
        return;
    }
    size_t baseLen = strlen(s_BasePath);
    bool needSep = s_BasePath[baseLen - 1] != '/';
    (void)snprintf(out, outSize, "%s%s%s", s_BasePath, needSep ? "/" : "", file);
}
} // namespace

void OS_DebugOut(const char* msg) {
    if (!msg) {
        return;
    }
    (void)fputs(msg, stderr);
    (void)fputc('\n', stderr);
}

void OS_DebugBreak() {
    (void)raise(SIGTRAP);
}

const char* OS_FileGetArchiveName(int32 archive) {
    (void)archive;
    return nullptr;
}

int32 OS_FileSize(void* file) {
    assert(file);
    auto* f = static_cast<FILE*>(file);
    long cur = ftell(f);
    (void)fseek(f, 0, SEEK_END);
    long size = ftell(f);
    (void)fseek(f, cur, SEEK_SET);
    return static_cast<int32>(size);
}

int32 OS_FileOpen(OSFileDataArea dataArea, void** pfile, const char* file, OSFileAccessType access) {
    (void)dataArea;
    assert(pfile && file);
    const char* mode = "rb";
    if (access == FILE_ACCESS_WRITE) {
        mode = "wb";
    } else if (access == FILE_ACCESS_EXISTING) {
        mode = "rb+";
    }
    char path[2048];
    BuildPath(file, path, sizeof(path));
    FILE* f = fopen(path, mode);
    *pfile = f;
    return f ? 0 : 1;
}

int32 OS_FileClose(void* file) {
    assert(file);
    return fclose(static_cast<FILE*>(file));
}

int32 OS_FileDelete(const char* file) {
    assert(file);
    char path[2048];
    BuildPath(file, path, sizeof(path));
    return ::remove(path);
}

int32 OS_FileRead(void* file, void* dest, int32 nDestSize) {
    assert(file && dest && nDestSize >= 0);
    size_t got = fread(dest, 1, static_cast<size_t>(nDestSize), static_cast<FILE*>(file));
    return got == static_cast<size_t>(nDestSize) ? 0 : 3;
}

int32 OS_FileGetPosition(void* file) {
    assert(file);
    return static_cast<int32>(ftell(static_cast<FILE*>(file)));
}

void OS_FileSetPosition(void* file, int32 nPos) {
    assert(file);
    (void)fseek(static_cast<FILE*>(file), nPos, SEEK_SET);
}

int32 OS_FileWrite(void* file, const void* src, int32 nSrcSize) {
    assert(file && src && nSrcSize >= 0);
    size_t put = fwrite(src, 1, static_cast<size_t>(nSrcSize), static_cast<FILE*>(file));
    return put == static_cast<size_t>(nSrcSize) ? 0 : 3;
}

void* OS_MutexCreate(const char* name) {
    (void)name;
    auto* m = new pthread_mutex_t{};
    (void)pthread_mutex_init(m, nullptr);
    return m;
}

void OS_MutexObtain(void* mutex) {
    assert(mutex);
    (void)pthread_mutex_lock(static_cast<pthread_mutex_t*>(mutex));
}

void OS_MutexRelease(void* mutex) {
    assert(mutex);
    (void)pthread_mutex_unlock(static_cast<pthread_mutex_t*>(mutex));
}

void OS_MutexDelete(void* mutex) {
    if (!mutex) {
        return;
    }
    auto* m = static_cast<pthread_mutex_t*>(mutex);
    (void)pthread_mutex_destroy(m);
    delete m;
}

void* OS_SemaphoreCreate(int32 iMaximumCount, const char* name) {
    (void)iMaximumCount;
    (void)name;
    auto* s = new sem_t{};
    (void)sem_init(s, 0, 0);
    return s;
}

bool OS_SemaphorePost(void* semaphore) {
    assert(semaphore);
    return sem_post(static_cast<sem_t*>(semaphore)) == 0;
}

void OS_SemaphoreWait(void* semaphore) {
    assert(semaphore);
    (void)sem_wait(static_cast<sem_t*>(semaphore));
}

void OS_SemaphoreDelete(void* semaphore) {
    if (!semaphore) {
        return;
    }
    auto* s = static_cast<sem_t*>(semaphore);
    (void)sem_destroy(s);
    delete s;
}

void OS_SetFilePathOffset(const char* path) {
    if (!path) {
        s_BasePath[0] = '\0';
        return;
    }
    (void)snprintf(s_BasePath, sizeof(s_BasePath), "%s", path);
}

namespace {
struct ThreadStart {
    OS_ThreadRoutine fn = nullptr;
    void* param = nullptr;
};

void* ThreadTrampoline(void* p) {
    auto* s = static_cast<ThreadStart*>(p);
    OS_ThreadRoutine fn = s->fn;
    void* param = s->param;
    delete s;
    uint32 result = fn(param);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(result));
}
} // namespace

void* OS_ThreadLaunch(OS_ThreadRoutine pfnStart, void* pParam, uint32 nFlags, const char* name, void* unk, OSThreadPriority nPriority) {
    (void)nFlags;
    (void)name;
    (void)unk;
    (void)nPriority;
    assert(pfnStart);
    auto* handle = new pthread_t{};
    auto* start = new ThreadStart{ pfnStart, pParam };
    int rc = pthread_create(handle, nullptr, ThreadTrampoline, start);
    assert(rc == 0);
    if (rc != 0) {
        delete start;
        delete handle;
        return nullptr;
    }
    return handle;
}

void OS_ThreadSleep(uint32 nMSec) {
    std::this_thread::sleep_for(std::chrono::milliseconds(nMSec));
}

void OS_ThreadClose(void* thread) {
    if (!thread) {
        return;
    }
    auto* h = static_cast<pthread_t*>(thread);
    (void)pthread_detach(*h);
    delete h;
}

void OS_ThreadWait(void* thread) {
    assert(thread);
    auto* h = static_cast<pthread_t*>(thread);
    (void)pthread_join(*h, nullptr);
}

void OS_ThreadResume(void* thread) {
    (void)thread;
}

uint64 GetOSWPerformanceTime() {
    struct timespec ts = {};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64>(ts.tv_sec) * 1000000000ULL + static_cast<uint64>(ts.tv_nsec);
}

int64 GetOSWPerformanceFrequency() {
    return 1000000000LL;
}

double OS_TimeAccurate() {
    return static_cast<double>(GetOSWPerformanceTime()) / 1000000000.0;
}

uint32 OS_TimeMS() {
    return static_cast<uint32>(GetOSWPerformanceTime() / 1000000ULL);
}
