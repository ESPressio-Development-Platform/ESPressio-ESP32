#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <ESPressio_Queue.hpp>

namespace ESPressio::ESP32Platform {

class MessageQueue final : public System::Queue::IMessageQueue {
private:
    QueueHandle_t _queue = nullptr;
    std::size_t _elementSize = 0;
    std::size_t _capacity = 0;

    static TickType_t Ticks(uint32_t timeoutMilliseconds) noexcept {
        if (timeoutMilliseconds == System::Synchronization::WaitForever) return portMAX_DELAY;
        TickType_t ticks = pdMS_TO_TICKS(timeoutMilliseconds);
        if (timeoutMilliseconds != 0 && ticks == 0) ticks = 1;
        return ticks;
    }

public:
    MessageQueue(std::size_t elementSize, std::size_t capacity)
        : _elementSize(elementSize), _capacity(capacity) {
        if (elementSize == 0 || capacity == 0) return;
        _queue = xQueueCreate(
            static_cast<UBaseType_t>(capacity),
            static_cast<UBaseType_t>(elementSize)
        );
    }

    ~MessageQueue() override {
        if (_queue != nullptr) {
            vQueueDelete(_queue);
            _queue = nullptr;
        }
    }

    bool IsAvailable() const noexcept { return _queue != nullptr; }

    System::PlatformResult Send(const void* item, uint32_t timeoutMilliseconds = 0) noexcept override {
        if (_queue == nullptr || item == nullptr) {
            return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
        }
        return xQueueSend(_queue, item, Ticks(timeoutMilliseconds)) == pdTRUE
            ? System::PlatformResult::Succeeded()
            : System::PlatformResult::Failed(
                timeoutMilliseconds == 0 ? System::PlatformStatus::Busy : System::PlatformStatus::Timeout
            );
    }

    System::PlatformResult SendFromInterrupt(const void* item) noexcept override {
        if (_queue == nullptr || item == nullptr) {
            return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
        }
        BaseType_t higherPriorityTaskWoken = pdFALSE;
        const BaseType_t result = xQueueSendFromISR(_queue, item, &higherPriorityTaskWoken);
        if (higherPriorityTaskWoken == pdTRUE) portYIELD_FROM_ISR();
        return result == pdTRUE
            ? System::PlatformResult::Succeeded()
            : System::PlatformResult::Failed(System::PlatformStatus::Busy);
    }

    System::PlatformResult Receive(
        void* item,
        uint32_t timeoutMilliseconds = System::Synchronization::WaitForever
    ) noexcept override {
        if (_queue == nullptr || item == nullptr) {
            return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
        }
        return xQueueReceive(_queue, item, Ticks(timeoutMilliseconds)) == pdTRUE
            ? System::PlatformResult::Succeeded()
            : System::PlatformResult::Failed(
                timeoutMilliseconds == 0 ? System::PlatformStatus::Busy : System::PlatformStatus::Timeout
            );
    }

    System::PlatformResult Reset() noexcept override {
        if (_queue == nullptr) {
            return System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
        }
        return xQueueReset(_queue) == pdPASS
            ? System::PlatformResult::Succeeded()
            : System::PlatformResult::Failed(System::PlatformStatus::HardwareFailure);
    }

    std::size_t ElementSize() const noexcept override { return _elementSize; }
    std::size_t Capacity() const noexcept override { return _capacity; }
    std::size_t Size() const noexcept override {
        return _queue != nullptr ? static_cast<std::size_t>(uxQueueMessagesWaiting(_queue)) : 0;
    }
};

class QueueProvider final : public System::Queue::IQueueProvider {
public:
    std::unique_ptr<System::Queue::IMessageQueue> Create(
        std::size_t elementSize,
        std::size_t capacity
    ) override {
        auto queue = std::make_unique<MessageQueue>(elementSize, capacity);
        return queue->IsAvailable() ? std::move(queue) : nullptr;
    }
};

inline QueueProvider& GetQueueProvider() noexcept {
    static QueueProvider provider;
    return provider;
}

inline void InstallQueueProvider() noexcept {
    System::Queue::SetProvider(&GetQueueProvider());
}

} // namespace ESPressio::ESP32Platform
