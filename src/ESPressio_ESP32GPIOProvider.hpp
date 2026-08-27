#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include <driver/gpio.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <ESPressio_GPIO.hpp>
#include "ESPressio_ESP32ClockProvider.hpp"

namespace ESPressio::ESP32Platform {

class ESP32GPIOController final : public System::GPIO::IController {
private:
    class Interrupt final : public System::GPIO::IInterrupt {
    private:
        System::GPIO::Pin _pin;
        System::ProcessorAffinity _affinity;
        System::GPIO::InterruptCallback _callback;
        void* _context;
        bool _enabled = false;
        bool _registered = false;

        static void IRAM_ATTR Dispatch(void* argument) {
            auto* instance = static_cast<Interrupt*>(argument);
            if (instance != nullptr && instance->_callback != nullptr) {
                instance->_callback(instance->_context);
            }
        }

    public:
        Interrupt(
            System::GPIO::Pin pin,
            System::ProcessorAffinity affinity,
            System::GPIO::InterruptCallback callback,
            void* context
        )
            : _pin(pin),
              _affinity(affinity),
              _callback(callback),
              _context(context) {}

        ~Interrupt() override {
            if (_registered) {
                (void)gpio_intr_disable(static_cast<gpio_num_t>(_pin));
                (void)gpio_isr_handler_remove(static_cast<gpio_num_t>(_pin));
                _registered = false;
                _enabled = false;
            }
        }

        System::PlatformResult Register(bool startEnabled) noexcept {
            const esp_err_t added = gpio_isr_handler_add(
                static_cast<gpio_num_t>(_pin),
                &Dispatch,
                this
            );
            if (added != ESP_OK) {
                return PlatformResultFromEspError(added);
            }
            _registered = true;

            if (startEnabled) {
                return Enable();
            }
            (void)gpio_intr_disable(static_cast<gpio_num_t>(_pin));
            return System::PlatformResult::Succeeded();
        }

        System::GPIO::Pin GetPin() const noexcept override {
            return _pin;
        }

        System::ProcessorAffinity GetAffinity() const noexcept override {
            return _affinity;
        }

        bool IsEnabled() const noexcept override {
            return _enabled;
        }

        System::PlatformResult Enable() noexcept override {
            if (!_registered) {
                return System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
            }
            const auto result = gpio_intr_enable(static_cast<gpio_num_t>(_pin));
            if (result == ESP_OK) {
                _enabled = true;
            }
            return PlatformResultFromEspError(result);
        }

        System::PlatformResult Disable() noexcept override {
            if (!_registered) {
                return System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
            }
            const auto result = gpio_intr_disable(static_cast<gpio_num_t>(_pin));
            if (result == ESP_OK) {
                _enabled = false;
            }
            return PlatformResultFromEspError(result);
        }
    };

    struct InstallContext {
        SemaphoreHandle_t Completed = nullptr;
        esp_err_t Result = ESP_FAIL;
    };

    inline static std::mutex _serviceMutex;
    inline static bool _serviceInstalled = false;
    inline static int _serviceCore = -1;

    static void InstallServiceTask(void* argument) {
        auto* context = static_cast<InstallContext*>(argument);
        if (context != nullptr) {
            context->Result = gpio_install_isr_service(0);
            xSemaphoreGive(context->Completed);
        }
        vTaskDelete(nullptr);
    }

    static System::PlatformResult EnsureInterruptService(
        System::ProcessorAffinity affinity
    ) {
        std::lock_guard<std::mutex> lock(_serviceMutex);

        if (_serviceInstalled) {
            if (affinity.IsSpecific() && _serviceCore != affinity.Processor) {
                return System::PlatformResult::Failed(System::PlatformStatus::Conflict);
            }
            return System::PlatformResult::Succeeded();
        }

        esp_err_t result = ESP_FAIL;
        int installedCore = xPortGetCoreID();

        if (!affinity.IsSpecific() || affinity.Processor == installedCore) {
            result = gpio_install_isr_service(0);
            installedCore = xPortGetCoreID();
        } else {
            SemaphoreHandle_t completed = xSemaphoreCreateBinary();
            if (completed == nullptr) {
                return System::PlatformResult::Failed(System::PlatformStatus::OutOfMemory);
            }

            InstallContext context;
            context.Completed = completed;

            TaskHandle_t installer = nullptr;
            const BaseType_t created = xTaskCreatePinnedToCore(
                &InstallServiceTask,
                "ESPressioGPIOISR",
                2048,
                &context,
                2,
                &installer,
                affinity.Processor
            );
            if (created != pdPASS || installer == nullptr) {
                vSemaphoreDelete(completed);
                return System::PlatformResult::Failed(System::PlatformStatus::OutOfMemory);
            }

            if (xSemaphoreTake(completed, portMAX_DELAY) != pdTRUE) {
                vSemaphoreDelete(completed);
                return System::PlatformResult::Failed(System::PlatformStatus::Timeout);
            }
            result = context.Result;
            installedCore = affinity.Processor;
            vSemaphoreDelete(completed);
        }

        if (result == ESP_OK || result == ESP_ERR_INVALID_STATE) {
            _serviceInstalled = true;
            _serviceCore = installedCore;
            return System::PlatformResult::Succeeded();
        }
        return PlatformResultFromEspError(result);
    }

    static gpio_int_type_t NativeInterruptType(System::GPIO::InterruptTrigger trigger) noexcept {
        switch (trigger) {
            case System::GPIO::InterruptTrigger::RisingEdge:
                return GPIO_INTR_POSEDGE;
            case System::GPIO::InterruptTrigger::FallingEdge:
                return GPIO_INTR_NEGEDGE;
            case System::GPIO::InterruptTrigger::AnyEdge:
                return GPIO_INTR_ANYEDGE;
            case System::GPIO::InterruptTrigger::LowLevel:
                return GPIO_INTR_LOW_LEVEL;
            case System::GPIO::InterruptTrigger::HighLevel:
                return GPIO_INTR_HIGH_LEVEL;
        }
        return GPIO_INTR_DISABLE;
    }

public:
    System::PlatformResult Configure(
        System::GPIO::Pin pin,
        const System::GPIO::PinConfiguration& configuration
    ) noexcept override {
        const auto nativePin = static_cast<gpio_num_t>(pin);
        if (!GPIO_IS_VALID_GPIO(nativePin)) {
            return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
        }

        gpio_config_t native{};
        native.pin_bit_mask = 1ULL << static_cast<uint64_t>(pin);
        native.intr_type = GPIO_INTR_DISABLE;

        switch (configuration.DirectionMode) {
            case System::GPIO::Direction::Input:
                native.mode = GPIO_MODE_INPUT;
                break;
            case System::GPIO::Direction::Output:
                if (!GPIO_IS_VALID_OUTPUT_GPIO(nativePin)) {
                    return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
                }
                native.mode = GPIO_MODE_OUTPUT;
                break;
            case System::GPIO::Direction::OpenDrain:
                if (!GPIO_IS_VALID_OUTPUT_GPIO(nativePin)) {
                    return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
                }
                native.mode = GPIO_MODE_OUTPUT_OD;
                break;
        }

        native.pull_up_en =
            configuration.PullMode == System::GPIO::Pull::Up ||
            configuration.PullMode == System::GPIO::Pull::UpDown
                ? GPIO_PULLUP_ENABLE
                : GPIO_PULLUP_DISABLE;
        native.pull_down_en =
            configuration.PullMode == System::GPIO::Pull::Down ||
            configuration.PullMode == System::GPIO::Pull::UpDown
                ? GPIO_PULLDOWN_ENABLE
                : GPIO_PULLDOWN_DISABLE;

        auto result = PlatformResultFromEspError(gpio_config(&native));
        if (!result) {
            return result;
        }

        if (configuration.DirectionMode != System::GPIO::Direction::Input) {
            result = Write(pin, configuration.InitialState);
        }
        return result;
    }

    System::PlatformResult Write(
        System::GPIO::Pin pin,
        System::GPIO::State state
    ) noexcept override {
        const auto nativePin = static_cast<gpio_num_t>(pin);
        if (!GPIO_IS_VALID_OUTPUT_GPIO(nativePin)) {
            return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
        }
        return PlatformResultFromEspError(
            gpio_set_level(nativePin, state == System::GPIO::State::High ? 1 : 0)
        );
    }

    System::PlatformResult Read(
        System::GPIO::Pin pin,
        System::GPIO::State& state
    ) const noexcept override {
        const auto nativePin = static_cast<gpio_num_t>(pin);
        if (!GPIO_IS_VALID_GPIO(nativePin)) {
            return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
        }
        state = gpio_get_level(nativePin) != 0
            ? System::GPIO::State::High
            : System::GPIO::State::Low;
        return System::PlatformResult::Succeeded();
    }

    System::GPIO::InterruptHandle CreateInterrupt(
        System::GPIO::Pin pin,
        const System::GPIO::InterruptConfiguration& configuration,
        System::GPIO::InterruptCallback callback,
        void* context = nullptr
    ) override {
        const auto nativePin = static_cast<gpio_num_t>(pin);
        if (!GPIO_IS_VALID_GPIO(nativePin) || callback == nullptr) {
            return nullptr;
        }

        const auto service = EnsureInterruptService(configuration.Affinity);
        if (!service) {
            return nullptr;
        }

        if (gpio_set_intr_type(nativePin, NativeInterruptType(configuration.Trigger)) != ESP_OK) {
            return nullptr;
        }
        (void)gpio_intr_disable(nativePin);

        auto interrupt = std::make_unique<Interrupt>(
            pin,
            configuration.Affinity,
            callback,
            context
        );
        if (!interrupt->Register(configuration.StartEnabled)) {
            return nullptr;
        }
        return interrupt;
    }

    bool SupportsInterrupts() const noexcept override {
        return true;
    }

    bool SupportsInterruptAffinity() const noexcept override {
        return true;
    }
};

inline ESP32GPIOController& GPIOController() noexcept {
    static ESP32GPIOController controller;
    return controller;
}

inline void InstallGPIOController() noexcept {
    System::GPIO::SetController(&GPIOController());
}

} // namespace ESPressio::ESP32Platform
