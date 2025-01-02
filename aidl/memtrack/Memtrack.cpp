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

#define ATRACE_TAG ATRACE_TAG_ALWAYS
#include <utils/Trace.h>

namespace aidl {
namespace android {
namespace hardware {
namespace memtrack {

bool getMemory_GRAPHICS_ion(int pid, int64_t* size) {
    FILE *fp = NULL;
    char line[1024];

    ATRACE_CALL();

    fp = fopen("/proc/ion/clients/clients_summary", "r");
    if (fp == NULL) {
        fp = fopen("/sys/kernel/debug/ion/clients/clients_summary", "r");
        if (fp == NULL)
            return false;
    }
    *size = 0;
    bool retval = false;
    while (1) {
        int handle_pid = 0;
        int64_t unaccounted_size = 0;
        int ret = 0;

        if (fgets(line, sizeof(line), fp) == NULL)
            break;
        ret = sscanf(line, "%*s %d %ld\n", &handle_pid, &unaccounted_size);
        if (ret == 2 && pid == handle_pid) {
            *size += unaccounted_size;
            retval = true;
        }
    }

    if (fclose(fp) != 0) {
        ALOGW("[memtrack] fclose failed");
        return false;
    }
    return retval;
}

bool getMemory_GRAPHICS_dmaheap(int pid, int64_t* size) {
    FILE *fp = NULL;
    char line[1024];
    int ret;

    ATRACE_CALL();

    fp = fopen("/proc/dma_heap/rss_pid", "w");
    if (fp == NULL)
        return false;

    ret = fprintf(fp, "pid:%d\n", pid);
    if(ret < 0)
        ALOGW("[memtrack] set rss_pid fail");

    if (fclose(fp) != 0) {
        ALOGW("[memtrack] fclose failed");
        return false;
    }

    fp = fopen("/proc/dma_heap/rss_pid", "r");
    if (fp == NULL)
        return false;

    *size = 0;
    bool retval = false;
    while (1) {
        int handle_pid = 0;
        int64_t unaccounted_size = 0;
        int ret = 0;

        if (fgets(line, sizeof(line), fp) == NULL)
            break;
        if (line[0] == '-' && line[1] == '-' && line[2] == '-')
            break;
        ret = sscanf(line, "%d %*ld %ld\n", &handle_pid, &unaccounted_size);
        if (ret == 2 && pid == handle_pid) {
            *size = unaccounted_size*1024;
            retval = true;
            break;
        }
    }

    if (fclose(fp) != 0) {
        ALOGW("[memtrack] fclose failed");
        return false;
    }
    return retval;
}

bool getMemory_GRAPHICS(int pid, int64_t* size) {

    if (getMemory_GRAPHICS_ion(pid, size) == true)
        return true;
    else if (getMemory_GRAPHICS_dmaheap(pid, size) == true)
        return true;
    else {
        return false;
    }
}


bool getMemory_GL_Mali(int pid, int64_t* size) {
    FILE *fp = NULL;
    char line[1024];

    ATRACE_CALL();

    fp = fopen("/proc/mtk_mali/gpu_memory", "r");
    if (fp == NULL) {
        fp = fopen("/proc/mali/memory_usage", "r");
        if (fp == NULL) {
            return false;
        }
    }

    while (1) {
        if (fgets(line, sizeof(line), fp) == NULL) {
            break;
        }
        if (line[0] == ' ' && line[1] == ' ') {
            int64_t gpu_mem = 0;
            unsigned int line_pid = 0;

            int ret = sscanf(line, "  %*s %ld %u\n",
                             &gpu_mem, &line_pid);
            if (2 == ret && line_pid == pid) {
                *size = gpu_mem * PAGE_SIZE;
                break;
            } else if (pid == 0) {
                *size += gpu_mem * PAGE_SIZE;
            }
        }
    }

    if (fclose(fp) != 0) {
        ALOGW("[memtrack] fclose failed");
        return false;
    }
    return true;
}

bool getMemory_GL_IMG(int pid, int64_t* size) {
    FILE *fp = NULL;
    char line[1024];
    const char * const delim = ",";
    char *saveptr = NULL;
    char *substr = NULL;

    ATRACE_CALL();

    fp = fopen("/proc/pvr/memtrack_stats", "r");
    if (fp == NULL) {
        return false;
    }

    if (fgets(line, sizeof(line), fp) == NULL) {
        ALOGW("[memtrack] /proc/pvr/memtrack_stats is abnormal");
        if (fclose(fp) != 0)
            ALOGW("[memtrack] fclose failed");
        return false;
    }

    while (1) {
        int64_t gpu_mem = 0;
        unsigned int line_pid = 0;

        if (fgets(line, sizeof(line), fp) == NULL) {
            break;
        }
        substr = strtok_r(line, delim, &saveptr);
        line_pid = atoi(substr);
        if (line_pid == pid) {
            do {
                substr = strtok_r(NULL, delim, &saveptr);
                if(substr != NULL)
                    gpu_mem += atoi(substr);
            } while (substr);
            *size = gpu_mem;
            break;
        } else if (pid == 0) {
            do {
                substr = strtok_r(NULL, delim, &saveptr);
                if(substr != NULL)
                    gpu_mem += atoi(substr);
            } while (substr);
            *size += gpu_mem;
        }
    }

    if (fclose(fp) != 0) {
        ALOGW("[memtrack] fclose failed");
        return false;
    }
    return true;
}

bool getMemory_GL(int pid, int64_t* size) {

    if (getMemory_GL_Mali(pid, size) == true)
        return true;
    else if (getMemory_GL_IMG(pid, size) == true)
        return true;
    else {
        return false;
    }

}

ndk::ScopedAStatus Memtrack::getMemory(int pid, MemtrackType type,
                                       std::vector<MemtrackRecord>* _aidl_return) {
    MemtrackRecord records = { .flags = 0, .sizeInBytes = 0};
    int64_t size = 0;
    if (pid < 0) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }
    if (type != MemtrackType::OTHER && type != MemtrackType::GL && type != MemtrackType::GRAPHICS &&
        type != MemtrackType::MULTIMEDIA && type != MemtrackType::CAMERA) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }
    if (_aidl_return == NULL)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_NULL_POINTER));

    _aidl_return->clear();

    if (type == MemtrackType::GL) {
        if (getMemory_GL(pid,&size) == false)
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_NONE));

        records.flags = MemtrackRecord::FLAG_SMAPS_UNACCOUNTED |
                        MemtrackRecord::FLAG_PRIVATE |
                        MemtrackRecord::FLAG_NONSECURE;
        records.sizeInBytes = size;
    } else if (type == MemtrackType::GRAPHICS) {
        if (getMemory_GRAPHICS(pid,&size) == false)
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_NONE));

        records.flags = MemtrackRecord::FLAG_SMAPS_UNACCOUNTED |
                        MemtrackRecord::FLAG_SHARED |
                        MemtrackRecord::FLAG_SYSTEM |
                        MemtrackRecord::FLAG_NONSECURE;
        records.sizeInBytes = size;
    }
    _aidl_return->push_back(records);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Memtrack::getGpuDeviceInfo(std::vector<DeviceInfo>* _aidl_return) {
    _aidl_return->clear();
    DeviceInfo dev_info = {.id = 0, .name = "virtio_gpu"};
    _aidl_return->emplace_back(dev_info);
    return ndk::ScopedAStatus::ok();
}

}  // namespace memtrack
}  // namespace hardware
}  // namespace android
}  // namespace aidl