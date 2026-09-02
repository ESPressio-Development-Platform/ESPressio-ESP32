#pragma once

#if !__has_include(<ESPressio_EventTransport.hpp>)
#error "TCPClientEventTransport requires ESPressio Event >= 5.6.2 < 6.0.0."
#endif

#include <array>
#include <mutex>
#include <string>
#include <vector>

#include <WiFiClient.h>
#include <ESPressio_SystemPlatformClock.hpp>
#include <ESPressio_EventTransport.hpp>

#include <ESPressio_SocketEventFrame.hpp>
#include <ESPressio_SocketStreamHelpers.hpp>
#include <ESPressio_SocketWorker.hpp>

namespace ESPressio::Sockets {

struct TCPClientEventTransportConfig {
    std::string Host;
    uint16_t Port = 0;
    uint32_t ReconnectIntervalMilliseconds = 2000;
    SocketWorkerConfig Worker;
};

class TCPClientEventTransport final :
    public Event::IEventTransport,
    private SocketWorker {

private:
    WiFiClient _client;
    TCPClientEventTransportConfig _config;
    SocketEventFrameDecoder _decoder;
    Event::IEventTransportReceiver* _receiver = nullptr;
    mutable std::mutex _clientMutex;
    mutable std::mutex _receiverMutex;
    uint32_t _lastConnectAttempt = 0;
    bool _initialized = false;

    static uint32_t NowMilliseconds() noexcept {
        return static_cast<uint32_t>(
            System::Clock::Monotonic().NowNanoseconds() / 1000000ULL
        );
    }

    void DispatchPacket(const uint8_t* data, std::size_t size) {
        Event::IEventTransportReceiver* receiver = nullptr;
        {
            std::lock_guard<std::mutex> lock(_receiverMutex);
            receiver = _receiver;
        }
        if (receiver != nullptr) {
            receiver->ReceiveEventTransportPacket(this, data, size);
        }
    }

    bool EnsureConnectedLocked() {
        if (_client.connected()) return true;

        const uint32_t now = NowMilliseconds();
        if (now - _lastConnectAttempt < _config.ReconnectIntervalMilliseconds) {
            return false;
        }

        _lastConnectAttempt = now;
        _client.stop();
        _decoder.Reset();
        return _client.connect(_config.Host.c_str(), _config.Port);
    }

    void OnWorkerIteration() override {
        std::vector<std::vector<uint8_t>> completed;
        {
            std::lock_guard<std::mutex> lock(_clientMutex);
            if (!EnsureConnectedLocked()) return;

            std::array<uint8_t, 512> buffer{};
            while (_client.available()) {
                const int count = _client.read(buffer.data(), buffer.size());
                if (count <= 0) break;

                const bool valid = _decoder.Push(
                    buffer.data(),
                    static_cast<std::size_t>(count),
                    [&](const uint8_t* data, std::size_t size) {
                        completed.emplace_back(data, data + size);
                    }
                );
                if (!valid) {
                    _client.stop();
                    break;
                }
            }
        }

        for (const auto& packet : completed) {
            DispatchPacket(packet.data(), packet.size());
        }
    }

public:
    ~TCPClientEventTransport() override { Shutdown(); }

    bool Initialize(const TCPClientEventTransportConfig& config) {
        if (_initialized) return true;
        if (config.Host.empty() || config.Port == 0) return false;

        _config = config;
        if (!StartWorker("ESPressioTCPC", config.Worker)) return false;
        _initialized = true;
        return true;
    }

    void Shutdown() {
        if (!_initialized) return;
        StopWorker();
        {
            std::lock_guard<std::mutex> lock(_clientMutex);
            _client.stop();
            _decoder.Reset();
        }
        {
            std::lock_guard<std::mutex> lock(_receiverMutex);
            _receiver = nullptr;
        }
        _initialized = false;
    }

    bool GetIsConnected() {
        std::lock_guard<std::mutex> lock(_clientMutex);
        return _client.connected();
    }

    bool Send(const Event::EventTransportPacket& packet) override {
        if (
            !_initialized ||
            packet.Data == nullptr ||
            packet.Size == 0 ||
            packet.Size > ESPRESSIO_SOCKETS_MAX_EVENT_PACKET_SIZE
        ) return false;

        const auto frame = BuildSocketEventFrame(packet.Data, packet.Size);
        if (frame.empty()) return false;

        std::lock_guard<std::mutex> lock(_clientMutex);
        if (!EnsureConnectedLocked()) return false;
        if (!WriteAll(_client, frame.data(), frame.size())) {
            _client.stop();
            return false;
        }
        return true;
    }

    void SetReceiver(Event::IEventTransportReceiver* receiver) override {
        std::lock_guard<std::mutex> lock(_receiverMutex);
        _receiver = receiver;
    }
};

} // namespace ESPressio::Sockets
