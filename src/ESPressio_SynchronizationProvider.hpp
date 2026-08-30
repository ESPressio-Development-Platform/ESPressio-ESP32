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

    System::PlatformResult Give() noexcept override {
        if (_semaphore == nullptr) {
            return System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
        }
        const BaseType_t result = xSemaphoreGive(_semaphore);
        return result == pdTRUE
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
        const BaseType_t result = xSemaphoreTake(_semaphore, ticks);
        return result == pdTRUE
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

/// <summary>FreeRTOS-backed non-recursive mutex implementation for ESPressio System.</summary>
class Mutex final : public System::Synchronization::IMutex {
    SemaphoreHandle_t _semaphore = nullptr;
public:
    Mutex() : _semaphore(xSemaphoreCreateMutex()) {}
    ~Mutex() override {
        if (_semaphore != nullptr) vSemaphoreDelete(_semaphore);
    }
    bool IsAvailable() const noexcept { return _semaphore != nullptr; }
    void Lock() noexcept override {
        if (_semaphore != nullptr) (void)xSemaphoreTake(_semaphore, portMAX_DELAY);
    }
    bool TryLock() noexcept override {
        return _semaphore != nullptr && xSemaphoreTake(_semaphore, 0) == pdTRUE;
    }
    void Unlock() noexcept override {
        if (_semaphore != nullptr) (void)xSemaphoreGive(_semaphore);
    }
};

/// <summary>FreeRTOS-backed recursive mutex implementation for ESPressio System.</summary>
class RecursiveMutex final : public System::Synchronization::IRecursiveMutex {
    SemaphoreHandle_t _semaphore = nullptr;
public:
    RecursiveMutex() : _semaphore(xSemaphoreCreateRecursiveMutex()) {}
    ~RecursiveMutex() override {
        if (_semaphore != nullptr) vSemaphoreDelete(_semaphore);
    }
    bool IsAvailable() const noexcept { return _semaphore != nullptr; }
    void Lock() noexcept override {
        if (_semaphore != nullptr) (void)xSemaphoreTakeRecursive(_semaphore, portMAX_DELAY);
    }
    bool TryLock() noexcept override {
        return _semaphore != nullptr && xSemaphoreTakeRecursive(_semaphore, 0) == pdTRUE;
    }
    void Unlock() noexcept override {
        if (_semaphore != nullptr) (void)xSemaphoreGiveRecursive(_semaphore);
    }
};

/// <summary>ESP32 read/write contract implemented with one native FreeRTOS mutex.</summary>
/// <remarks>Shared and exclusive acquisition intentionally serialize on ESP32. This avoids pthread rwlock state and preserves correctness; true concurrent-reader behaviour can be introduced later behind the same System contract if measurements justify it.</remarks>
class ReadWriteLock final : public System::Synchronization::IReadWriteLock {
    SemaphoreHandle_t _semaphore = nullptr;
public:
    ReadWriteLock() : _semaphore(xSemaphoreCreateMutex()) {}
    ~ReadWriteLock() override {
        if (_semaphore != nullptr) vSemaphoreDelete(_semaphore);
    }
    bool IsAvailable() const noexcept { return _semaphore != nullptr; }
    void Lock() noexcept override {
        if (_semaphore != nullptr) (void)xSemaphoreTake(_semaphore, portMAX_DELAY);
    }
    bool TryLock() noexcept override {
        return _semaphore != nullptr && xSemaphoreTake(_semaphore, 0) == pdTRUE;
    }
    void Unlock() noexcept override {
        if (_semaphore != nullptr) (void)xSemaphoreGive(_semaphore);
    }
    void LockShared() noexcept override { Lock(); }
    bool TryLockShared() noexcept override { return TryLock(); }
    void UnlockShared() noexcept override { Unlock(); }
};

class SynchronizationProvider final : public System::Synchronization::ISynchronizationProvider {
public:
    std::unique_ptr<System::Synchronization::ISignal> CreateBinarySignal(
        bool initiallySet = false
    ) override {
        auto signal = std::make_unique<BinarySignal>(initiallySet);
        return signal->IsAvailable() ? std::move(signal) : nullptr;
    }

    std::unique_ptr<System::Synchronization::IMutex> CreateMutex() override {
        auto mutex = std::make_unique<Mutex>();
        return mutex->IsAvailable() ? std::move(mutex) : nullptr;
    }

    std::unique_ptr<System::Synchronization::IRecursiveMutex> CreateRecursiveMutex() override {
        auto mutex = std::make_unique<RecursiveMutex>();
        return mutex->IsAvailable() ? std::move(mutex) : nullptr;
    }

    std::unique_ptr<System::Synchronization::IReadWriteLock> CreateReadWriteLock() override {
        auto lock = std::make_unique<ReadWriteLock>();
        return lock->IsAvailable() ? std::move(lock) : nullptr;
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
