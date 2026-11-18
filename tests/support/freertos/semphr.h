#pragma once
#include <cassert>
using SemaphoreHandle_t = bool *;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new bool(false); }
inline int xSemaphoreTake(SemaphoreHandle_t lock, int) {
    if (*lock) return 0;
    *lock = true;
    return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t lock) {
    assert(*lock);
    *lock = false;
}
