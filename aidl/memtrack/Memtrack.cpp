/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Memtrack.h"
#include <android-base/logging.h>
#include <log/log.h>
#include <string.h>
#include <cstdio>
#include <vector>

namespace aidl {
namespace android {
namespace hardware {
namespace memtrack {

namespace {

bool openFile(const char* path, const char* mode, FILE** fp) {
    *fp = fopen(path, mode);
    if (!*fp) return false;
    return true;
}

bool readFileLines(FILE* fp, std::function<bool(const char*)> processLine) {
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        if (!processLine(line)) break;
    }
    return fclose(fp) == 0;
}

bool parseIonClients(int pid, int64_t* size) {
    FILE* fp;
    if (!openFile("/proc/ion/clients/clients_summary", "r", &fp) &&
        !openFile("/sys/kernel/debug/ion/clients/clients_summary", "r", &fp)) {
        return false;
    }

    *size = 0;
    bool found = readFileLines(fp, [pid, size](const char* line) {
        int handle_pid;
        int64_t unaccounted_size;
        if (sscanf(line, "%*s %d %ld", &handle_pid, &unaccounted_size) == 2 && pid == handle_pid) {
            *size += unaccounted_size;
            return true;
        }
        return true;
    });
    return found;
}

bool parseDmaHeap(int pid, int64_t* size) {
    FILE* fp;
    if (!openFile("/proc/dma_heap/rss_pid", "w", &fp)) return false;

    if (fprintf(fp, "pid:%d\n", pid) < 0 || fclose(fp) != 0) return false;

    if (!openFile("/proc/dma_heap/rss_pid", "r", &fp)) return false;

    *size = 0;
    return readFileLines(fp, [pid, size](const char* line) {
        if (line[0] == '-' && line[1] == '-' && line[2] == '-') return false;

        int handle_pid;
        int64_t unaccounted_size;
        if (sscanf(line, "%d %*ld %ld", &handle_pid, &unaccounted_size) == 2 && pid == handle_pid) {
            *size = unaccounted_size * 1024;
            return false;
        }
        return true;
    });
}

bool parseGpuMemory(const char* path, int pid, int64_t* size) {
    FILE* fp;
    if (!openFile(path, "r", &fp)) return false;

    *size = 0;
    return readFileLines(fp, [pid, size](const char* line) {
        int line_pid;
        int64_t gpu_mem;

        if (sscanf(line, "  %*s %ld %u", &gpu_mem, &line_pid) == 2) {
            if (line_pid == pid || pid == 0) {
                *size += gpu_mem * PAGE_SIZE;
                return pid == 0;
            }
        }
        return true;
    });
}

}  // namespace

bool getMemory_GRAPHICS(int pid, int64_t* size) {
    return parseIonClients(pid, size) || parseDmaHeap(pid, size);
}

bool getMemory_GL(int pid, int64_t* size) {
    return parseGpuMemory("/proc/mtk_mali/gpu_memory", pid, size) ||
           parseGpuMemory("/proc/mali/memory_usage", pid, size);
}

ndk::ScopedAStatus Memtrack::getMemory(int pid, MemtrackType type,
                                       std::vector<MemtrackRecord>* _aidl_return) {
    if (pid < 0 || !_aidl_return) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }

    _aidl_return->clear();

    int64_t size = 0;
    bool success = (type == MemtrackType::GL) ? getMemory_GL(pid, &size)
                  : (type == MemtrackType::GRAPHICS) ? getMemory_GRAPHICS(pid, &size)
                  : false;

    if (!success) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_NONE));
    }

    MemtrackRecord record = {
        .flags = (type == MemtrackType::GL)
                     ? (MemtrackRecord::FLAG_SMAPS_UNACCOUNTED |
                        MemtrackRecord::FLAG_PRIVATE |
                        MemtrackRecord::FLAG_NONSECURE)
                     : (MemtrackRecord::FLAG_SMAPS_UNACCOUNTED |
                        MemtrackRecord::FLAG_SHARED |
                        MemtrackRecord::FLAG_SYSTEM |
                        MemtrackRecord::FLAG_NONSECURE),
        .sizeInBytes = size
    };
    _aidl_return->push_back(record);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Memtrack::getGpuDeviceInfo(std::vector<DeviceInfo>* _aidl_return) {
    _aidl_return->clear();
    _aidl_return->emplace_back(DeviceInfo{.id = 0, .name = "virtio_gpu"});
    return ndk::ScopedAStatus::ok();
}

}  // namespace memtrack
}  // namespace hardware
}  // namespace android
}  // namespace aidl