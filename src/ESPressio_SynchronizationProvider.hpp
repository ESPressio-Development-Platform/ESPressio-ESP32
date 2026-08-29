#pragma once

#include <memory>

#ifdef ESPRESSIO_ESP32_SYNCHRONIZATION_DIAGNOSTICS
    #include <esp_rom_sys.h>
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <ESPressio_Synchronization.hpp>

namespace ESPressio::ESP32Platform {

class BinarySignal final : public System::Synchronization::ISignal {
private:
    SemaphoreHandle_t _semaphore = nullptr;

#ifdef ESPRESSIO_ESP32_SYNCHRONIZATION_DIAGNOSTICS
    /// <summary>Emits a low-overhead ROM-console diagnostic without entering newlib stdio locks.</summary>
    /// <remarks>This path is intended for constrained worker-stack diagnostics and deliberately avoids printf/fflush from newlib, which can consume enough stack to perturb or overflow small FreeRTOS workers.</remarks>
    void Trace(const char* phase) const noexcept {
        esp_rom_printf(
            "[ESPressio ESP32][BinarySignal] phase=%s signal=%p semaphore=%p currentTask=%p\n",
            phase != nullptr ? phase : "unknown",
            static_cast<const void*>(this),
            static_cast<void*>(_semaphore),
            static_cast<void*>(xTaskGetCurrentTaskHandle())
        );
    }
#endif

public:
    /// <summary>Creates a FreeRTOS-backed binary signal.</summary>
    explicit BinarySignal(bool initiallySet) {
        _semaphore = xSemaphoreCreateBinary();
        if (_semaphore != nullptr && initiallySet) xSemaphoreGive(_semaphore);
#ifdef ESPRESSIO_ESP32_SYNCHRONIZATION_DIAGNOSTICS
        Trace("created");
#endif
    }

    /// <summary>Releases the native FreeRTOS semaphore owned by this signal.</summary>
    ~BinarySignal() override {
#ifdef ESPRESSIO_ESP32_SYNCHRONIZATION_DIAGNOSTICS
        Trace("destroying");
#endif
        if (_semaphore != nullptr) {
            vSemaphoreDelete(_semaphore);
            _semaphore = nullptr;
        }
    }

    /// <summary>Indicates whether the native FreeRTOS semaphore was created successfully.</summary>
    bool IsAvailable() const noexcept { return _semaphore != nullptr; }

    /// <summary>Sets the binary signal from task context.</summary>
    System::PlatformResult Give() noexcept override {
        if (_semaphore == nullptr) {
            return System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
        }
#ifdef ESPRESSIO_ESP32_SYNCHRONIZATION_DIAGNOSTICS
        Trace("give-begin");
#endif
        const BaseType_t result = xSemaphoreGive(_semaphore);
#ifdef ESPRESSIO_ESP32_SYNCHRONIZATION_DIAGNOSTICS
        Trace(result == pdTRUE ? "give-end-success" : "give-end-busy");
#endif
        return result == pdTRUE
            ? System::PlatformResult::Succeeded()
            : System::PlatformResult::Failed(System::PlatformStatus::Busy);
    }

    /// <summary>Sets the binary signal from interrupt context.</summary>
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

    /// <summary>Waits for the binary signal to become set, optionally with a timeout.</summary>
    System::PlatformResult Wait(uint32_t timeoutMilliseconds) noexcept override {
        if (_semaphore == nullptr) {
            return System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
        }
        TickType_t ticks = portMAX_DELAY;
        if (timeoutMilliseconds != System::Synchronization::WaitForever) {
            ticks = pdMS_TO_TICKS(timeoutMilliseconds);
            if (timeoutMilliseconds != 0 && ticks == 0) ticks = 1;
        }
#ifdef ESPRESSIO_ESP32_SYNCHRONIZATION_DIAGNOSTICS
        Trace("wait-begin");
#endif
        const BaseType_t result = xSemaphoreTake(_semaphore, ticks);
#ifdef ESPRESSIO_ESP32_SYNCHRONIZATION_DIAGNOSTICS
        Trace(result == pdTRUE ? "wait-end-success" : "wait-end-timeout");
#endif
        return result == pdTRUE
            ? System::PlatformResult::Succeeded()
            : System::PlatformResult::Failed(System::PlatformStatus::Timeout);
    }

    /// <summary>Clears any currently set state without blocking.</summary>
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
    /// <summary>Creates a FreeRTOS-backed binary signal.</summary>
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
