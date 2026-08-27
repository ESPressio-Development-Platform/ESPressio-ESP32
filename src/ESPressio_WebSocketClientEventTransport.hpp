#pragma once

#if !__has_include(<ESPressio_EventTransport.hpp>)
#error "WebSocketClientEventTransport requires ESPressio Event >= 5.6.2 < 6.0.0."
#endif

#if !__has_include(<WebSocketsClient.h>)
#error "WebSocketClientEventTransport requires Links2004 arduinoWebSockets >= 2.3.6."
#endif

#include <mutex>
#include <string>

#include <WebSocketsClient.h>
#include <ESPressio_EventTransport.hpp>

#include <ESPressio_SocketTypes.hpp>
#include <ESPressio_SocketWorker.hpp>

namespace ESPressio::Sockets {

struct WebSocketClientEventTransportConfig {
    std::string Host;
    uint16_t Port = 0;
    std::string Path = "/";
    std::string Protocol = "espressio";

    bool Secure = false;
    const char* CACertificate = nullptr;

    uint32_t ReconnectIntervalMilliseconds = 2000;

    bool EnableHeartbeat = true;
    uint32_t PingIntervalMilliseconds = 15000;
    uint32_t PongTimeoutMilliseconds = 3000;
    uint8_t DisconnectTimeoutCount = 2;

    SocketWorkerConfig Worker;
};

class WebSocketClientEventTransport final :
    public Event::IEventTransport,
    private SocketWorker {

private:
    WebSocketsClient _webSocket;
    WebSocketClientEventTransportConfig _config;
    Event::IEventTransportReceiver* _receiver = nullptr;
    mutable std::mutex _socketMutex;
    mutable std::mutex _receiverMutex;
    bool _initialized = false;

    void HandleEvent(WStype_t type, uint8_t* payload, std::size_t length) {
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
        _webSocket.loop();
    }

public:
    ~WebSocketClientEventTransport() override { Shutdown(); }

    bool Initialize(const WebSocketClientEventTransportConfig& config) {
        if (_initialized) return true;
        if (config.Host.empty() || config.Port == 0) return false;

        _config = config;
        _webSocket.onEvent(
            [this](WStype_t type, uint8_t* payload, std::size_t length) {
                HandleEvent(type, payload, length);
            }
        );
        _webSocket.setReconnectInterval(config.ReconnectIntervalMilliseconds);

        if (config.EnableHeartbeat) {
            _webSocket.enableHeartbeat(
                config.PingIntervalMilliseconds,
                config.PongTimeoutMilliseconds,
                config.DisconnectTimeoutCount
            );
        }

        if (config.Secure) {
            _webSocket.beginSslWithCA(
                config.Host.c_str(),
                config.Port,
                config.Path.c_str(),
                config.CACertificate,
                config.Protocol.c_str()
            );
        } else {
            _webSocket.begin(
                config.Host.c_str(),
                config.Port,
                config.Path.c_str(),
                config.Protocol.c_str()
            );
        }

        if (!StartWorker("ESPressioWSC", config.Worker)) {
            _webSocket.disconnect();
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
            _webSocket.disconnect();
        }
        {
            std::lock_guard<std::mutex> lock(_receiverMutex);
            _receiver = nullptr;
        }
        _initialized = false;
    }

    bool GetIsConnected() const {
        std::lock_guard<std::mutex> lock(_socketMutex);
        return const_cast<WebSocketsClient&>(_webSocket).isConnected();
    }

    bool Send(const Event::EventTransportPacket& packet) override {
        if (
            !_initialized || packet.Data == nullptr || packet.Size == 0 ||
            packet.Size > ESPRESSIO_SOCKETS_MAX_EVENT_PACKET_SIZE
        ) return false;

        std::lock_guard<std::mutex> lock(_socketMutex);
        if (!_webSocket.isConnected()) return false;
        return _webSocket.sendBIN(packet.Data, packet.Size);
    }

    void SetReceiver(Event::IEventTransportReceiver* receiver) override {
        std::lock_guard<std::mutex> lock(_receiverMutex);
        _receiver = receiver;
    }
};

} // namespace ESPressio::Sockets
