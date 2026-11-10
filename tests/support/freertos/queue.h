#pragma once
#include <cassert>
#include <cstdint>
struct TestQueue { uint8_t value = 0; bool pending = false; };
using QueueHandle_t = TestQueue *;
inline QueueHandle_t xQueueCreate(int count, size_t size) {
    assert(count == 1 && size == 1);
    return new TestQueue;
}
inline void xQueueReset(QueueHandle_t queue) { queue->pending = false; }
inline void xQueueOverwrite(QueueHandle_t queue, const uint8_t *value) {
    queue->value = *value;
    queue->pending = true;
}
inline int xQueueReceive(QueueHandle_t queue, uint8_t *value, int) {
    if (!queue->pending) return 0;
    *value = queue->value;
    queue->pending = false;
    return pdTRUE;
}
