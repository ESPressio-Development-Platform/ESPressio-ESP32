#pragma once

#if !defined(ARDUINO)
#error "ESPressio_ArduinoGPIOProvider.hpp requires the Arduino framework"
#endif

#include <Arduino.h>
#include <memory>

#include <ESPressio_GPIO.hpp>

namespace ESPressio::ESP32Platform {

class ArduinoGPIOController final : public System::GPIO::IController {
private:
    class Interrupt final : public System::GPIO::IInterrupt {
    private:
        System::GPIO::Pin _pin;
        System::GPIO::InterruptCallback _callback;
        void* _context;
        int _mode;
        bool _enabled = false;

        static void Dispatch(void* argument) {
            auto* instance = static_cast<Interrupt*>(argument);
            if (instance != nullptr && instance->_callback != nullptr) {
                instance->_callback(instance->_context);
            }
        }

    public:
        Interrupt(
            System::GPIO::Pin pin,
            System::GPIO::InterruptCallback callback,
            void* context,
            int mode
        )
            : _pin(pin),
              _callback(callback),
              _context(context),
              _mode(mode) {}

        ~Interrupt() override {
            if (_enabled) {
                detachInterrupt(static_cast<uint8_t>(_pin));
                _enabled = false;
            }
        }

        System::GPIO::Pin GetPin() const noexcept override { return _pin; }
        System::ProcessorAffinity GetAffinity() const noexcept override {
            return System::ProcessorAffinity::Any();
        }
        bool IsEnabled() const noexcept override { return _enabled; }

        System::PlatformResult Enable() noexcept override {
            if (_callback == nullptr) {
                return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
            }
#if defined(ARDUINO_ARCH_ESP32)
            attachInterruptArg(
                static_cast<uint8_t>(_pin),
                &Dispatch,
                this,
                _mode
            );
            _enabled = true;
            return System::PlatformResult::Succeeded();
#else
            return System::PlatformResult::Failed(System::PlatformStatus::Unsupported);
#endif
        }

        System::PlatformResult Disable() noexcept override {
            detachInterrupt(static_cast<uint8_t>(_pin));
            _enabled = false;
            return System::PlatformResult::Succeeded();
        }
    };

    static System::PlatformResult ResolvePinMode(
        const System::GPIO::PinConfiguration& configuration,
        uint8_t& mode
    ) noexcept {
        switch (configuration.DirectionMode) {
            case System::GPIO::Direction::Input:
                switch (configuration.PullMode) {
                    case System::GPIO::Pull::None:
                        mode = INPUT;
                        return System::PlatformResult::Succeeded();
                    case System::GPIO::Pull::Up:
                        mode = INPUT_PULLUP;
                        return System::PlatformResult::Succeeded();
                    case System::GPIO::Pull::Down:
#if defined(INPUT_PULLDOWN)
                        mode = INPUT_PULLDOWN;
                        return System::PlatformResult::Succeeded();
#else
                        return System::PlatformResult::Failed(System::PlatformStatus::Unsupported);
#endif
                    case System::GPIO::Pull::UpDown:
                        return System::PlatformResult::Failed(System::PlatformStatus::Unsupported);
                }
                break;
            case System::GPIO::Direction::Output:
                mode = OUTPUT;
                return System::PlatformResult::Succeeded();
            case System::GPIO::Direction::OpenDrain:
#if defined(OUTPUT_OPEN_DRAIN)
                mode = OUTPUT_OPEN_DRAIN;
                return System::PlatformResult::Succeeded();
#else
                return System::PlatformResult::Failed(System::PlatformStatus::Unsupported);
#endif
        }
        return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
    }

    static System::PlatformResult ResolveInterruptMode(
        System::GPIO::InterruptTrigger trigger,
        int& mode
    ) noexcept {
        switch (trigger) {
            case System::GPIO::InterruptTrigger::RisingEdge:
                mode = RISING;
                return System::PlatformResult::Succeeded();
            case System::GPIO::InterruptTrigger::FallingEdge:
                mode = FALLING;
                return System::PlatformResult::Succeeded();
            case System::GPIO::InterruptTrigger::AnyEdge:
                mode = CHANGE;
                return System::PlatformResult::Succeeded();
            case System::GPIO::InterruptTrigger::LowLevel:
#if defined(ONLOW)
                mode = ONLOW;
                return System::PlatformResult::Succeeded();
#else
                return System::PlatformResult::Failed(System::PlatformStatus::Unsupported);
#endif
            case System::GPIO::InterruptTrigger::HighLevel:
#if defined(ONHIGH)
                mode = ONHIGH;
                return System::PlatformResult::Succeeded();
#else
                return System::PlatformResult::Failed(System::PlatformStatus::Unsupported);
#endif
        }
        return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
    }

public:
    System::PlatformResult Configure(
        System::GPIO::Pin pin,
        const System::GPIO::PinConfiguration& configuration
    ) noexcept override {
        uint8_t mode = INPUT;
        const auto resolved = ResolvePinMode(configuration, mode);
        if (!resolved) return resolved;

        pinMode(static_cast<uint8_t>(pin), mode);
        if (configuration.DirectionMode != System::GPIO::Direction::Input) {
            digitalWrite(
                static_cast<uint8_t>(pin),
                configuration.InitialState == System::GPIO::State::High ? HIGH : LOW
            );
        }
        return System::PlatformResult::Succeeded();
    }

    System::PlatformResult Write(
        System::GPIO::Pin pin,
        System::GPIO::State state
    ) noexcept override {
        digitalWrite(
            static_cast<uint8_t>(pin),
            state == System::GPIO::State::High ? HIGH : LOW
        );
        return System::PlatformResult::Succeeded();
    }

    System::PlatformResult Read(
        System::GPIO::Pin pin,
        System::GPIO::State& state
    ) const noexcept override {
        state = digitalRead(static_cast<uint8_t>(pin)) == HIGH
            ? System::GPIO::State::High
            : System::GPIO::State::Low;
        return System::PlatformResult::Succeeded();
    }

    System::GPIO::InterruptCreationResult CreateInterrupt(
        System::GPIO::Pin pin,
        const System::GPIO::InterruptConfiguration& configuration,
        System::GPIO::InterruptCallback callback,
        void* context = nullptr
    ) override {
        if (callback == nullptr) {
            return {System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument)};
        }
        if (configuration.Affinity.IsSpecific()) {
            return {System::PlatformResult::Failed(System::PlatformStatus::Unsupported)};
        }

        int mode = CHANGE;
        const auto resolved = ResolveInterruptMode(configuration.Trigger, mode);
        if (!resolved) return {resolved};

        auto interrupt = std::make_unique<Interrupt>(pin, callback, context, mode);
        if (configuration.StartEnabled) {
            const auto enabled = interrupt->Enable();
            if (!enabled) return {enabled};
        }

        return {System::PlatformResult::Succeeded(), std::move(interrupt)};
    }

    bool SupportsInterrupts() const noexcept override { return true; }
    bool SupportsInterruptAffinity() const noexcept override { return false; }
};

inline ArduinoGPIOController& ArduinoGPIO() noexcept {
    static ArduinoGPIOController controller;
    return controller;
}

inline void InstallArduinoGPIOController() noexcept {
    System::GPIO::SetController(&ArduinoGPIO());
}

} // namespace ESPressio::ESP32Platform
