#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

struct TestQueue {
    size_t capacity, itemSize;
    std::deque<std::vector<uint8_t>> items;
};
using QueueHandle_t = TestQueue *;
inline QueueHandle_t xQueueCreate(size_t count, size_t size) {
    return new TestQueue{count, size, {}};
}
inline void xQueueReset(QueueHandle_t queue) { queue->items.clear(); }
inline int xQueueSend(QueueHandle_t queue, const void *value, int) {
    if (queue->items.size() == queue->capacity) return 0;
    const auto *bytes = static_cast<const uint8_t *>(value);
    queue->items.emplace_back(bytes, bytes + queue->itemSize);
    return pdTRUE;
}
inline void xQueueOverwrite(QueueHandle_t queue, const void *value) {
    assert(queue->capacity == 1);
    xQueueReset(queue);
    assert(xQueueSend(queue, value, 0) == pdTRUE);
}
inline int xQueueReceive(QueueHandle_t queue, void *value, int) {
    if (queue->items.empty()) return 0;
    std::memcpy(value, queue->items.front().data(), queue->itemSize);
    queue->items.pop_front();
    return pdTRUE;
}
inline size_t uxQueueMessagesWaiting(QueueHandle_t queue) { return queue->items.size(); }
