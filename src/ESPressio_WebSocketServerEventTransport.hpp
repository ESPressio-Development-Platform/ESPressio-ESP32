#pragma once

#if !__has_include(<ESPressio_EventTransport.hpp>)
#error "WebSocketServerEventTransport requires ESPressio Event >= 5.6.2 < 6.0.0."
#endif

#if !__has_include(<WebSocketsServer.h>)
#error "WebSocketServerEventTransport requires Links2004 arduinoWebSockets >= 2.3.6."
#endif

#include <memory>
#include <mutex>
#include <string>

#include <WebSocketsServer.h>
#include <ESPressio_EventTransport.hpp>

#include <ESPressio_SocketTypes.hpp>
#include <ESPressio_SocketWorker.hpp>

namespace ESPressio::Sockets {

struct WebSocketServerEventTransportConfig {
    uint16_t Port = 0;
    std::string Origin;
    std::string Protocol = "espressio";

    bool EnableHeartbeat = true;
    uint32_t PingIntervalMilliseconds = 15000;
    uint32_t PongTimeoutMilliseconds = 3000;
    uint8_t DisconnectTimeoutCount = 2;

    SocketWorkerConfig Worker;
};

class WebSocketServerEventTransport final :
    public Event::IEventTransport,
    private SocketWorker {

private:
    std::unique_ptr<WebSocketsServer> _server;
    WebSocketServerEventTransportConfig _config;
    Event::IEventTransportReceiver* _receiver = nullptr;
    mutable std::mutex _socketMutex;
    mutable std::mutex _receiverMutex;
    bool _initialized = false;

    void HandleEvent(uint8_t, WStype_t type, uint8_t* payload, std::size_t length) {
        if (
            type != WStype_BIN || payload == nullptr || length == 0 ||
            length > ESPRESSIO_SOCKETS_MAX_EVENT_PACKET_SIZE
        ) return;

        Event::IEventTransportReceiver* receiver = nullptr;
        {
            std::lock_guard<std::mutex> lock(_receiverMutex);
            receiver = _receiver;
        }
        if (receiver != nullptr) {
            receiver->ReceiveEventTransportPacket(this, payload, length);
        }
    }

    void OnWorkerIteration() override {
        std::lock_guard<std::mutex> lock(_socketMutex);
        if (_server != nullptr) _server->loop();
    }

public:
    ~WebSocketServerEventTransport() override { Shutdown(); }

    bool Initialize(const WebSocketServerEventTransportConfig& config) {
        if (_initialized) return true;
        if (config.Port == 0) return false;

        _config = config;
        _server = std::make_unique<WebSocketsServer>(
            config.Port,
            config.Origin.c_str(),
            config.Protocol.c_str()
        );

        _server->onEvent(
            [this](uint8_t client, WStype_t type, uint8_t* payload, std::size_t length) {
                HandleEvent(client, type, payload, length);
            }
        );

        if (config.EnableHeartbeat) {
            _server->enableHeartbeat(
                config.PingIntervalMilliseconds,
                config.PongTimeoutMilliseconds,
                config.DisconnectTimeoutCount
            );
        }

        _server->begin();
        if (!StartWorker("ESPressioWSS", config.Worker)) {
            _server->close();
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
            std::lock_guard<std::mutex> lock(_socketMutex);
            if (_server != nullptr) {
                _server->close();
                _server.reset();
            }
        }
        {
            std::lock_guard<std::mutex> lock(_receiverMutex);
            _receiver = nullptr;
        }
        _initialized = false;
    }

    int GetConnectedClientCount() const {
        std::lock_guard<std::mutex> lock(_socketMutex);
        return _server == nullptr ? 0 : _server->connectedClients();
    }

    bool Send(const Event::EventTransportPacket& packet) override {
        if (
            !_initialized || packet.Data == nullptr || packet.Size == 0 ||
            packet.Size > ESPRESSIO_SOCKETS_MAX_EVENT_PACKET_SIZE
        ) return false;

        std::lock_guard<std::mutex> lock(_socketMutex);
        if (_server == nullptr || _server->connectedClients() == 0) return false;
        return _server->broadcastBIN(packet.Data, packet.Size);
    }

    void SetReceiver(Event::IEventTransportReceiver* receiver) override {
        std::lock_guard<std::mutex> lock(_receiverMutex);
        _receiver = receiver;
    }
};

} // namespace ESPressio::Sockets
