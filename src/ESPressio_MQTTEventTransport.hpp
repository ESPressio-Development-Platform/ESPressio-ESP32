#pragma once

#if !__has_include(<ESPressio_EventTransport.hpp>)
#error "MQTTEventTransport requires ESPressio Event >= 5.6.2 < 6.0.0."
#endif

#if !__has_include(<PubSubClient.h>)
#error "MQTTEventTransport requires PubSubClient >= 2.8."
#endif

#include <memory>
#include <mutex>
#include <string>

#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ESPressio_Clock.hpp>
#include <ESPressio_EventTransport.hpp>

#include <ESPressio_SocketTypes.hpp>
#include <ESPressio_SocketWorker.hpp>

namespace ESPressio::Sockets {

struct MQTTEventTransportConfig {
    std::string Host;
    uint16_t Port = 1883;

    std::string ClientID;
    std::string Username;
    std::string Password;

    std::string OutboundTopic = "espressio/events/out";
    std::string InboundTopic = "espressio/events/in";

    uint8_t SubscribeQoS = 0;
    bool RetainOutbound = false;

    bool Secure = false;
    bool Insecure = false;
    const char* CACertificate = nullptr;
    const char* ClientCertificate = nullptr;
    const char* ClientPrivateKey = nullptr;

    uint16_t BufferSize = 4096;
    uint16_t KeepAliveSeconds = 15;
    uint16_t SocketTimeoutSeconds = 15;

    uint32_t ReconnectIntervalMilliseconds = 2000;

    SocketWorkerConfig Worker;
};

class MQTTEventTransport final :
    public Event::IEventTransport,
    private SocketWorker {

private:
    WiFiClient _plainClient;
    WiFiClientSecure _secureClient;
    std::unique_ptr<PubSubClient> _mqtt;
    MQTTEventTransportConfig _config;
    Event::IEventTransportReceiver* _receiver = nullptr;
    mutable std::mutex _mqttMutex;
    mutable std::mutex _receiverMutex;
    uint32_t _lastConnectAttempt = 0;
    bool _initialized = false;

    static uint32_t NowMilliseconds() noexcept {
        return static_cast<uint32_t>(
            System::Clock::Monotonic().NowNanoseconds() / 1000000ULL
        );
    }

    void ConfigureTLS() {
        if (!_config.Secure) return;

        if (_config.Insecure) {
            _secureClient.setInsecure();
            return;
        }

        if (_config.CACertificate != nullptr) {
            _secureClient.setCACert(_config.CACertificate);
        }

        if (
            _config.ClientCertificate != nullptr &&
            _config.ClientPrivateKey != nullptr
        ) {
            _secureClient.setCertificate(_config.ClientCertificate);
            _secureClient.setPrivateKey(_config.ClientPrivateKey);
        }
    }

    void ReceiveMQTT(char* topic, uint8_t* payload, unsigned int length) {
        if (
            topic == nullptr || payload == nullptr || length == 0 ||
            _config.InboundTopic != topic ||
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

    bool EnsureConnectedLocked() {
        if (_mqtt != nullptr && _mqtt->connected()) return true;

        const uint32_t now = NowMilliseconds();
        if (now - _lastConnectAttempt < _config.ReconnectIntervalMilliseconds) {
            return false;
        }
        _lastConnectAttempt = now;

        if (_mqtt == nullptr) return false;

        bool connected = false;
        if (!_config.Username.empty()) {
            connected = _mqtt->connect(
                _config.ClientID.c_str(),
                _config.Username.c_str(),
                _config.Password.c_str()
            );
        } else {
            connected = _mqtt->connect(_config.ClientID.c_str());
        }

        if (connected && !_config.InboundTopic.empty()) {
            connected = _mqtt->subscribe(
                _config.InboundTopic.c_str(),
                _config.SubscribeQoS
            );
        }
        return connected;
    }

    void OnWorkerIteration() override {
        std::lock_guard<std::mutex> lock(_mqttMutex);
        if (EnsureConnectedLocked() && _mqtt != nullptr) _mqtt->loop();
    }

public:
    ~MQTTEventTransport() override { Shutdown(); }

    bool Initialize(const MQTTEventTransportConfig& config) {
        if (_initialized) return true;
        if (
            config.Host.empty() || config.Port == 0 || config.ClientID.empty() ||
            config.OutboundTopic.empty() || config.InboundTopic.empty()
        ) return false;

        _config = config;
        if (_config.Secure) {
            ConfigureTLS();
            _mqtt = std::make_unique<PubSubClient>(_secureClient);
        } else {
            _mqtt = std::make_unique<PubSubClient>(_plainClient);
        }

        _mqtt->setServer(config.Host.c_str(), config.Port);
        _mqtt->setBufferSize(config.BufferSize);
        _mqtt->setKeepAlive(config.KeepAliveSeconds);
        _mqtt->setSocketTimeout(config.SocketTimeoutSeconds);
        _mqtt->setCallback(
            [this](char* topic, uint8_t* payload, unsigned int length) {
                ReceiveMQTT(topic, payload, length);
            }
        );

        if (!StartWorker("ESPressioMQTT", config.Worker)) {
            _mqtt.reset();
            return false;
        }

        _initialized = true;
        return true;
    }

    void Shutdown() {
        if (!_initialized) return;
        StopWorker();
        {
            std::lock_guard<std::mutex> lock(_mqttMutex);
            if (_mqtt != nullptr) {
                _mqtt->disconnect();
                _mqtt.reset();
            }
            _plainClient.stop();
            _secureClient.stop();
        }
        {
            std::lock_guard<std::mutex> lock(_receiverMutex);
            _receiver = nullptr;
        }
        _initialized = false;
    }

    bool GetIsConnected() const {
        std::lock_guard<std::mutex> lock(_mqttMutex);
        return _mqtt != nullptr && _mqtt->connected();
    }

    bool Send(const Event::EventTransportPacket& packet) override {
        if (
            !_initialized || packet.Data == nullptr || packet.Size == 0 ||
            packet.Size > ESPRESSIO_SOCKETS_MAX_EVENT_PACKET_SIZE ||
            packet.Size > _config.BufferSize
        ) return false;

        std::lock_guard<std::mutex> lock(_mqttMutex);
        if (!EnsureConnectedLocked() || _mqtt == nullptr) return false;

        return _mqtt->publish(
            _config.OutboundTopic.c_str(),
            packet.Data,
            static_cast<unsigned int>(packet.Size),
            _config.RetainOutbound
        );
    }

    void SetReceiver(Event::IEventTransportReceiver* receiver) override {
        std::lock_guard<std::mutex> lock(_receiverMutex);
        _receiver = receiver;
    }
};

} // namespace ESPressio::Sockets
