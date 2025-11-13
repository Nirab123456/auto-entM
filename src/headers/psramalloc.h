#include <esp_heap_caps.h>
#include <cstdint>
#include <cstdio>

template <typename T>
T* allocate_in_parse(size_t N, const char* label = nullptr)
{
    size_t bytes = N * sizeof(T);
    T* ptr = nullptr;

    ptr = reinterpret_cast<T*>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );

    if (ptr)
    {
        if (label)
        {
            Serial.printf("PsRamAllocator::allocate_in_parse::%s: %u bytes\n",label,(unsigned)bytes, (void*)ptr);
            return ptr;
        }
        
    }
    
    // failed
    Serial.printf("PsRamAllocator::allocate_in_parse::%s:Parse ram allocation failed");
    return nullptr;
}