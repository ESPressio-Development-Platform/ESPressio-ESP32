#pragma once

#if !__has_include(<ESPressio_EventTransport.hpp>)
#error "TCPServerEventTransport requires ESPressio Event >= 5.6.2 < 6.0.0."
#endif

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <vector>

#include <WiFiServer.h>
#include <ESPressio_EventTransport.hpp>
#include <ESPressio_SocketEventFrame.hpp>
#include <ESPressio_SocketStreamHelpers.hpp>
#include <ESPressio_SocketWorker.hpp>

namespace ESPressio::Sockets {

struct TCPServerEventTransportConfig {
    uint16_t Port = 0;
    std::size_t MaximumClients = ESPRESSIO_SOCKETS_MAX_TCP_CLIENTS;
    SocketWorkerConfig Worker;
};

class TCPServerEventTransport final :
    public Event::IEventTransport,
    private SocketWorker {
private:
    struct ClientState {
        WiFiClient Client;
        SocketEventFrameDecoder Decoder;
        bool Active = false;
    };

    std::unique_ptr<WiFiServer> _server;
    std::array<ClientState, ESPRESSIO_SOCKETS_MAX_TCP_CLIENTS> _clients;
    TCPServerEventTransportConfig _config;
    Event::IEventTransportReceiver* _receiver = nullptr;
    mutable std::mutex _clientsMutex;
    mutable std::mutex _receiverMutex;
    bool _initialized = false;

    void DispatchPacket(const uint8_t* data, std::size_t size) {
        Event::IEventTransportReceiver* receiver = nullptr;
        {
            std::lock_guard<std::mutex> lock(_receiverMutex);
            receiver = _receiver;
        }
        if (receiver != nullptr) receiver->ReceiveEventTransportPacket(this, data, size);
    }

    void AcceptClientsLocked() {
        if (_server == nullptr) return;
        WiFiClient incoming = _server->available();
        if (!incoming) return;

        const std::size_t limit = std::min(_config.MaximumClients, _clients.size());
        for (std::size_t i = 0; i < limit; ++i) {
            if (!_clients[i].Active || !_clients[i].Client.connected()) {
                _clients[i].Client.stop();
                _clients[i].Client = incoming;
                _clients[i].Decoder.Reset();
                _clients[i].Active = true;
                return;
            }
        }
        incoming.stop();
    }

    void OnWorkerIteration() override {
        std::vector<std::vector<uint8_t>> completed;
        {
            std::lock_guard<std::mutex> lock(_clientsMutex);
            AcceptClientsLocked();
            const std::size_t limit = std::min(_config.MaximumClients, _clients.size());
            std::array<uint8_t, 512> buffer{};

            for (std::size_t i = 0; i < limit; ++i) {
                auto& state = _clients[i];
                if (!state.Active) continue;
                if (!state.Client.connected()) {
                    state.Client.stop();
                    state.Decoder.Reset();
                    state.Active = false;
                    continue;
                }

                while (state.Client.available()) {
                    const int count = state.Client.read(buffer.data(), buffer.size());
                    if (count <= 0) break;
                    const bool valid = state.Decoder.Push(
                        buffer.data(),
                        static_cast<std::size_t>(count),
                        [&](const uint8_t* data, std::size_t size) {
                            completed.emplace_back(data, data + size);
                        }
                    );
                    if (!valid) {
                        state.Client.stop();
                        state.Active = false;
                        break;
                    }
                }
            }
        }

        for (const auto& packet : completed) DispatchPacket(packet.data(), packet.size());
    }

public:
    ~TCPServerEventTransport() override { Shutdown(); }

    bool Initialize(const TCPServerEventTransportConfig& config) {
        if (_initialized) return true;
        if (config.Port == 0 || config.MaximumClients == 0) return false;

        _config = config;
        _server = std::make_unique<WiFiServer>(config.Port);
        _server->begin();
        _server->setNoDelay(true);

        if (!StartWorker("ESPressioTCPS", config.Worker)) {
            _server->end();
            _server.reset();
            return false;
        }

        _initialized = true;
        return true;
    }

    void Shutdown() {
        if (!_initialized) return;
        StopWorker();
        {
            std::lock_guard<std::mutex> lock(_clientsMutex);
            for (auto& state : _clients) {
                state.Client.stop();
                state.Decoder.Reset();
                state.Active = false;
            }
            if (_server != nullptr) {
                _server->end();
                _server.reset();
            }
        }
        {
            std::lock_guard<std::mutex> lock(_receiverMutex);
            _receiver = nullptr;
        }
        _initialized = false;
    }

    std::size_t GetConnectedClientCount() {
        std::lock_guard<std::mutex> lock(_clientsMutex);
        std::size_t count = 0;
        for (auto& state : _clients) {
            if (state.Active && state.Client.connected()) ++count;
        }
        return count;
    }

    bool Send(const Event::EventTransportPacket& packet) override {
        if (
            !_initialized || packet.Data == nullptr || packet.Size == 0 ||
            packet.Size > ESPRESSIO_SOCKETS_MAX_EVENT_PACKET_SIZE
        ) return false;

        const auto frame = BuildSocketEventFrame(packet.Data, packet.Size);
        if (frame.empty()) return false;

        std::lock_guard<std::mutex> lock(_clientsMutex);
        bool hadClient = false;
        bool success = true;
        for (auto& state : _clients) {
            if (!state.Active || !state.Client.connected()) continue;
            hadClient = true;
            if (!WriteAll(state.Client, frame.data(), frame.size())) {
                state.Client.stop();
                state.Active = false;
                success = false;
            }
        }
        return hadClient && success;
    }

    void SetReceiver(Event::IEventTransportReceiver* receiver) override {
        std::lock_guard<std::mutex> lock(_receiverMutex);
        _receiver = receiver;
    }
};

} // namespace ESPressio::Sockets
