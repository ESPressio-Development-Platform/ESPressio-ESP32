#pragma once

#include <cstdint>
#include <memory>

#include <esp_err.h>
#include <esp_timer.h>

#if __has_include(<driver/gptimer.h>)
#include <driver/gptimer.h>
#define ESPRESSIO_ESP32_HAS_GPTIMER 1
#else
#define ESPRESSIO_ESP32_HAS_GPTIMER 0
#endif

#include <ESPressio_SystemClock.hpp>

namespace ESPressio::ESP32Platform {

inline System::PlatformResult PlatformResultFromEspError(esp_err_t error) noexcept {
    if (error == ESP_OK) return System::PlatformResult::Succeeded();

    System::PlatformStatus status = System::PlatformStatus::HardwareFailure;
    switch (error) {
        case ESP_ERR_INVALID_ARG: status = System::PlatformStatus::InvalidArgument; break;
        case ESP_ERR_NO_MEM: status = System::PlatformStatus::OutOfMemory; break;
#ifdef ESP_ERR_NOT_SUPPORTED
        case ESP_ERR_NOT_SUPPORTED: status = System::PlatformStatus::Unsupported; break;
#endif
        case ESP_ERR_INVALID_STATE: status = System::PlatformStatus::Conflict; break;
#ifdef ESP_ERR_TIMEOUT
        case ESP_ERR_TIMEOUT: status = System::PlatformStatus::Timeout; break;
#endif
        default: break;
    }
    return System::PlatformResult::Failed(status, static_cast<int32_t>(error));
}

class MonotonicClock final : public System::Clock::IMonotonicClock {
public:
    uint64_t NowNanoseconds() const noexcept override {
        return static_cast<uint64_t>(esp_timer_get_time()) * 1000ULL;
    }

    uint64_t ResolutionNanoseconds() const noexcept override { return 1000ULL; }
    bool IsInterruptSafe() const noexcept override { return true; }
};

#if ESPRESSIO_ESP32_HAS_GPTIMER

class HighResolutionCounter final : public System::Clock::IHighResolutionCounter {
private:
    gptimer_handle_t _timer = nullptr;
    uint64_t _resolutionHz = 0;
    System::PlatformResult _initializationResult =
        System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
    bool _enabled = false;
    bool _running = false;

public:
    explicit HighResolutionCounter(uint64_t requestedResolutionHz) {
        if (requestedResolutionHz == 0 || requestedResolutionHz > UINT32_MAX) {
            _initializationResult = System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
            return;
        }

        gptimer_config_t configuration{};
        configuration.clk_src = GPTIMER_CLK_SRC_DEFAULT;
        configuration.direction = GPTIMER_COUNT_UP;
        configuration.resolution_hz = static_cast<uint32_t>(requestedResolutionHz);

        esp_err_t result = gptimer_new_timer(&configuration, &_timer);
        if (result != ESP_OK) {
            _initializationResult = PlatformResultFromEspError(result);
            _timer = nullptr;
            return;
        }

        result = gptimer_enable(_timer);
        if (result != ESP_OK) {
            _initializationResult = PlatformResultFromEspError(result);
            gptimer_del_timer(_timer);
            _timer = nullptr;
            return;
        }

        _enabled = true;
        _resolutionHz = requestedResolutionHz;
        _initializationResult = System::PlatformResult::Succeeded();
    }

    ~HighResolutionCounter() override {
        if (_timer == nullptr) return;
        if (_running) {
            (void)gptimer_stop(_timer);
            _running = false;
        }
        if (_enabled) {
            (void)gptimer_disable(_timer);
            _enabled = false;
        }
        (void)gptimer_del_timer(_timer);
        _timer = nullptr;
    }

    System::PlatformResult Start() noexcept override {
        if (_timer == nullptr) return _initializationResult;
        if (_running) return System::PlatformResult::Succeeded();
        const auto result = gptimer_start(_timer);
        if (result == ESP_OK) _running = true;
        return PlatformResultFromEspError(result);
    }

    System::PlatformResult Stop() noexcept override {
        if (_timer == nullptr) return _initializationResult;
        if (!_running) return System::PlatformResult::Succeeded();
        const auto result = gptimer_stop(_timer);
        if (result == ESP_OK) _running = false;
        return PlatformResultFromEspError(result);
    }

    System::PlatformResult Reset() noexcept override {
        if (_timer == nullptr) return _initializationResult;
        return PlatformResultFromEspError(gptimer_set_raw_count(_timer, 0));
    }

    System::PlatformResult Read(uint64_t& count) const noexcept override {
        if (_timer == nullptr) {
            count = 0;
            return _initializationResult;
        }
        return PlatformResultFromEspError(gptimer_get_raw_count(_timer, &count));
    }

    uint64_t ResolutionHz() const noexcept override { return _resolutionHz; }
    bool IsAvailable() const noexcept override {
        return _timer != nullptr && static_cast<bool>(_initializationResult);
    }
    bool IsInterruptSafe() const noexcept override { return true; }
    System::PlatformResult InitializationResult() const noexcept override { return _initializationResult; }
};

#endif

class HighResolutionCounterProvider final : public System::Clock::IHighResolutionCounterProvider {
public:
    std::unique_ptr<System::Clock::IHighResolutionCounter> Create(
        uint64_t requestedResolutionHz
    ) override {
#if ESPRESSIO_ESP32_HAS_GPTIMER
        return std::make_unique<HighResolutionCounter>(requestedResolutionHz);
#else
        (void)requestedResolutionHz;
        return nullptr;
#endif
    }
};

inline MonotonicClock& GetMonotonicClock() noexcept {
    static MonotonicClock clock;
    return clock;
}

inline HighResolutionCounterProvider& GetHighResolutionCounterProvider() noexcept {
    static HighResolutionCounterProvider provider;
    return provider;
}

inline bool HasGPTimerHighResolutionCounter() noexcept {
    return ESPRESSIO_ESP32_HAS_GPTIMER != 0;
}

inline void InstallClockProviders() noexcept {
    System::Clock::SetMonotonicClock(&GetMonotonicClock());
    System::Clock::SetHighResolutionCounterProvider(&GetHighResolutionCounterProvider());
}

} // namespace ESPressio::ESP32Platform
