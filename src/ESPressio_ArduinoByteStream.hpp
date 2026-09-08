#pragma once

#if defined(ARDUINO)

#include <Arduino.h>

#include <ESPressio_ByteStream.hpp>

namespace ESPressio::ESP32Platform {

/**
 * ESPressio Memory Audit
 * Inherited Memory Total: 4 bytes [0 bytes dynamic allocation]
 * Members:
 * - _input (Stream*): 4 bytes [0 bytes dynamic allocation]
 * Total Memory: 8 bytes [0 bytes dynamic allocation]
 * Basis: ESP32/Xtensa ILP32 reference ABI (4-byte pointers/size_t); ESPressio stateful allocators/deleters included; ABI-sensitive STL/platform internals are identified explicitly.
 * End ESPressio Memory Audit
 */
class ArduinoByteInput final : public System::IO::IByteInput {
public:
    explicit ArduinoByteInput(Stream& input) noexcept : _input(&input) {}

    std::size_t Available() const noexcept override {
        return _input == nullptr
            ? 0
            : static_cast<std::size_t>(_input->available());
    }

    System::PlatformResult Read(uint8_t& value) noexcept override {
        if (_input == nullptr) {
            return System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
        }
        const int read = _input->read();
        if (read < 0) {
            return System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
        }
        value = static_cast<uint8_t>(read);
        return System::PlatformResult::Succeeded();
    }

private:
    Stream* _input;
};

/**
 * ESPressio Memory Audit
 * Inherited Memory Total: 4 bytes [0 bytes dynamic allocation]
 * Members:
 * - _output (Print*): 4 bytes [0 bytes dynamic allocation]
 * Total Memory: 8 bytes [0 bytes dynamic allocation]
 * Basis: ESP32/Xtensa ILP32 reference ABI (4-byte pointers/size_t); ESPressio stateful allocators/deleters included; ABI-sensitive STL/platform internals are identified explicitly.
 * End ESPressio Memory Audit
 */
class ArduinoByteOutput final : public System::IO::IByteOutput {
public:
    explicit ArduinoByteOutput(Print& output) noexcept : _output(&output) {}

    System::PlatformResult Write(
        const uint8_t* data,
        std::size_t size,
        std::size_t& bytesWritten
    ) noexcept override {
        bytesWritten = 0;
        if (_output == nullptr) {
            return System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
        }
        if (data == nullptr && size != 0) {
            return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
        }
        bytesWritten = size == 0 ? 0 : _output->write(data, size);
        return bytesWritten == size
            ? System::PlatformResult::Succeeded()
            : System::PlatformResult::Failed(System::PlatformStatus::HardwareFailure);
    }

private:
    Print* _output;
};

/**
 * ESPressio Memory Audit
 * Inherited Memory Total: 8 bytes [0 bytes dynamic allocation]
 * Members:
 * - _stream (Stream*): 4 bytes [0 bytes dynamic allocation]
 * Total Memory: 12 bytes [0 bytes dynamic allocation]
 * Basis: ESP32/Xtensa ILP32 reference ABI (4-byte pointers/size_t); ESPressio stateful allocators/deleters included; ABI-sensitive STL/platform internals are identified explicitly.
 * End ESPressio Memory Audit
 */
class ArduinoByteStream final : public System::IO::IByteStream {
public:
    explicit ArduinoByteStream(Stream& stream) noexcept : _stream(&stream) {}

    std::size_t Available() const noexcept override {
        return _stream == nullptr
            ? 0
            : static_cast<std::size_t>(_stream->available());
    }

    System::PlatformResult Read(uint8_t& value) noexcept override {
        if (_stream == nullptr) {
            return System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
        }
        const int read = _stream->read();
        if (read < 0) {
            return System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
        }
        value = static_cast<uint8_t>(read);
        return System::PlatformResult::Succeeded();
    }

    System::PlatformResult Write(
        const uint8_t* data,
        std::size_t size,
        std::size_t& bytesWritten
    ) noexcept override {
        bytesWritten = 0;
        if (_stream == nullptr) {
            return System::PlatformResult::Failed(System::PlatformStatus::Unavailable);
        }
        if (data == nullptr && size != 0) {
            return System::PlatformResult::Failed(System::PlatformStatus::InvalidArgument);
        }
        bytesWritten = size == 0 ? 0 : _stream->write(data, size);
        return bytesWritten == size
            ? System::PlatformResult::Succeeded()
            : System::PlatformResult::Failed(System::PlatformStatus::HardwareFailure);
    }

private:
    Stream* _stream;
};

} // namespace ESPressio::ESP32Platform

#endif
