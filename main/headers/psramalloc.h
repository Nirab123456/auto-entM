#pragma once 
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cstdint>
#include <cstddef>
#include <stdlib.h>

static const char *psmTAG = "psramalloc";

template<typename T>
static inline T* AllocPSRamArray(size_t N, const char* label = nullptr)
{
    if (N == 0)
    {
        return nullptr;
    }
    size_t bytes = N* sizeof(T);
    size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (label) {
        ESP_LOGD(psmTAG, "AllocPSRamArray::%s->Allocating:%llu bytes (free %llu)\n",
                      label, (unsigned long long)bytes, (unsigned long long)free_ps);
    }

    T* ptr = reinterpret_cast<T*>(heap_caps_calloc(N, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));


    if (ptr) {
        if (label) ESP_LOGD(psmTAG, "AllocPSRamArray::%s->Allocated:%llu bytes at %p\n", label,
                                 (unsigned long long)bytes, (void*)ptr);
    } else {
        if (label) ESP_LOGD(psmTAG, "AllocPSRamArray::%s->Allocation FAILED (%llu bytes)\n", label,
                                 (unsigned long long)bytes);
    }
    
    return ptr;
}

template <typename T>
static inline T* AllocDmaArray(size_t N, const char* label = nullptr)
{
    if (N == 0)
    {
        return nullptr;
    }
    size_t bytes = N* sizeof(T);
    T* ptr = reinterpret_cast<T*>(heap_caps_calloc(N, sizeof(T), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    return ptr;
    
}

template <typename T>
static inline void FreeCaps(T* p)
{
    if(!p)
    {
        return;
    }
    heap_caps_free(reinterpret_cast<void*>(p));
}