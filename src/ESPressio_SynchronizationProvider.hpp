#pragma once

#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <ESPressio_Synchronization.hpp>

namespace ESPressio::ESP32Platform {

class BinarySignal final : public System::Synchronization::ISignal {
private:
    SemaphoreHandle_t _semaphore = nullptr;

public:
    explicit BinarySignal(bool initiallySet) {
        _semaphore = xSemaphoreCreateBinary();
        if (_semaphore != nullptr && initiallySet) xSemaphoreGive(_semaphore);
    }

    ~BinarySignal() override {
        if (_semaphore != nullptr) {
            vSemaphoreDelete(_semaphore);
            _semaphore = nullptr;
        }
    }

    bool IsAvailable() const noexcept { return _semaphore != nullptr; }

    System::PlatformResult Give() noexcept override {
        if (_semaphore == nullptr) {
            return System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
        }
        return xSemaphoreGive(_semaphore) == pdTRUE
            ? System::PlatformResult::Succeeded()
            : System::PlatformResult::Failed(System::PlatformStatus::Busy);
    }

    System::PlatformResult GiveFromInterrupt() noexcept override {
        if (_semaphore == nullptr) {
            return System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
        }
        BaseType_t higherPriorityTaskWoken = pdFALSE;
        const BaseType_t result = xSemaphoreGiveFromISR(_semaphore, &higherPriorityTaskWoken);
        if (higherPriorityTaskWoken == pdTRUE) portYIELD_FROM_ISR();
        return result == pdTRUE
            ? System::PlatformResult::Succeeded()
            : System::PlatformResult::Failed(System::PlatformStatus::Busy);
    }

    System::PlatformResult Wait(uint32_t timeoutMilliseconds) noexcept override {
        if (_semaphore == nullptr) {
            return System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
        }
        TickType_t ticks = portMAX_DELAY;
        if (timeoutMilliseconds != System::Synchronization::WaitForever) {
            ticks = pdMS_TO_TICKS(timeoutMilliseconds);
            if (timeoutMilliseconds != 0 && ticks == 0) ticks = 1;
        }
        return xSemaphoreTake(_semaphore, ticks) == pdTRUE
            ? System::PlatformResult::Succeeded()
            : System::PlatformResult::Failed(System::PlatformStatus::Timeout);
    }

    System::PlatformResult Reset() noexcept override {
        if (_semaphore == nullptr) {
            return System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
        }
        while (xSemaphoreTake(_semaphore, 0) == pdTRUE) {}
        return System::PlatformResult::Succeeded();
    }
};

class SynchronizationProvider final : public System::Synchronization::ISynchronizationProvider {
public:
    std::unique_ptr<System::Synchronization::ISignal> CreateBinarySignal(
        bool initiallySet = false
    ) override {
        auto signal = std::make_unique<BinarySignal>(initiallySet);
        return signal->IsAvailable() ? std::move(signal) : nullptr;
    }
};

inline SynchronizationProvider& GetSynchronizationProvider() noexcept {
    static SynchronizationProvider provider;
    return provider;
}

inline void InstallSynchronizationProvider() noexcept {
    System::Synchronization::SetProvider(&GetSynchronizationProvider());
}

} // namespace ESPressio::ESP32Platform
