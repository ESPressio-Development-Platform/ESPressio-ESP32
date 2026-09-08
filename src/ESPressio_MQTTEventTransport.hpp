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
#include <ESPressio_SystemPlatformClock.hpp>
#include <ESPressio_EventTransport.hpp>

#include <ESPressio_SocketTypes.hpp>
#include <ESPressio_SocketWorker.hpp>

namespace ESPressio::Sockets {

/**
 * ESPressio Memory Audit
 * Members:
 * - Host (std::string): 24 bytes [Capacity + 1 bytes when capacity exceeds 15-byte SSO]
 * - Port (uint16_t): 2 bytes [0 bytes dynamic allocation]
 * - ClientID (std::string): 24 bytes [Capacity + 1 bytes when capacity exceeds 15-byte SSO]
 * - Username (std::string): 24 bytes [Capacity + 1 bytes when capacity exceeds 15-byte SSO]
 * - Password (std::string): 24 bytes [Capacity + 1 bytes when capacity exceeds 15-byte SSO]
 * - OutboundTopic (std::string): 24 bytes [Capacity + 1 bytes when capacity exceeds 15-byte SSO]
 * - InboundTopic (std::string): 24 bytes [Capacity + 1 bytes when capacity exceeds 15-byte SSO]
 * - SubscribeQoS (uint8_t): 1 bytes [0 bytes dynamic allocation]
 * - RetainOutbound (bool): 1 bytes [0 bytes dynamic allocation]
 * - Secure (bool): 1 bytes [0 bytes dynamic allocation]
 * - Insecure (bool): 1 bytes [0 bytes dynamic allocation]
 * - CACertificate (char*): 4 bytes [0 bytes dynamic allocation]
 * - ClientCertificate (char*): 4 bytes [0 bytes dynamic allocation]
 * - ClientPrivateKey (char*): 4 bytes [0 bytes dynamic allocation]
 * - BufferSize (uint16_t): 2 bytes [0 bytes dynamic allocation]
 * - KeepAliveSeconds (uint16_t): 2 bytes [0 bytes dynamic allocation]
 * - SocketTimeoutSeconds (uint16_t): 2 bytes [0 bytes dynamic allocation]
 * - ReconnectIntervalMilliseconds (uint32_t): 4 bytes [0 bytes dynamic allocation]
 * - Worker (SocketWorkerConfig): 16 bytes [0 bytes dynamic allocation]
 * Total Memory: 192 bytes [Host: Capacity + 1 bytes when capacity exceeds 15-byte SSO; ClientID: Capacity + 1 bytes when capacity exceeds 15-byte SSO; Username: Capacity + 1 bytes when capacity exceeds 15-byte SSO; Password: Capacity + 1 bytes when capacity exceeds 15-byte SSO; OutboundTopic: Capacity + 1 bytes when capacity exceeds 15-byte SSO; InboundTopic: Capacity + 1 bytes when capacity exceeds 15-byte SSO]
 * Basis: ESP32/Xtensa ILP32 reference ABI (4-byte pointers/size_t); ESPressio stateful allocators/deleters included; ABI-sensitive STL/platform internals are identified explicitly.
 * Confidence: medium; compile-time sizeof on the concrete target remains authoritative for ABI-sensitive/opaque members.
 * End ESPressio Memory Audit
 */
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

/**
 * ESPressio Memory Audit
 * Inherited Memory Total: 40 bytes [SocketWorker: _observable: shared control block (~12+ bytes; allocate_shared may co-locate object) + object 96 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: enable_shared_from_this: embedded weak_ptr shares a control block when activated; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: shared control block (~12+ bytes; allocate_shared may co-locate object) + object 20 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: pointee: _mutex: native synchronization state may allocate platform resources lazily; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: pointee: _condition: native condition-variable state may allocate platform synchronization resources; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: _registrations: Capacity * (12 bytes) element storage; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: _bindings: Capacity * (12 bytes) element storage; SocketWorker: _observable: pointee: ThreadSafeObservable: _mutex: _owned: owned object: 4 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: _mutex: _fallback: _mutex: native synchronization state may allocate platform resources lazily; SocketWorker: _observable: pointee: ThreadSafeObservable: _notificationMutex: _owned: owned object: 4 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: _notificationMutex: _fallback: _mutex: native synchronization state may allocate platform resources lazily]
 * Members:
 * - _plainClient (WiFiClient): sizeof(WiFiClient) (target/toolchain dependent) [0 bytes dynamic allocation]
 * - _secureClient (WiFiClientSecure): sizeof(WiFiClientSecure) (target/toolchain dependent) [0 bytes dynamic allocation]
 * - _mqtt (std::unique_ptr<PubSubClient>): 4 bytes [owned object: sizeof(PubSubClient) (target/toolchain dependent)]
 * - _config (MQTTEventTransportConfig): 192 bytes [Host: Capacity + 1 bytes when capacity exceeds 15-byte SSO; ClientID: Capacity + 1 bytes when capacity exceeds 15-byte SSO; Username: Capacity + 1 bytes when capacity exceeds 15-byte SSO; Password: Capacity + 1 bytes when capacity exceeds 15-byte SSO; OutboundTopic: Capacity + 1 bytes when capacity exceeds 15-byte SSO; InboundTopic: Capacity + 1 bytes when capacity exceeds 15-byte SSO]
 * - _receiver (Event::IEventTransportReceiver*): 4 bytes [0 bytes dynamic allocation]
 * - _mqttMutex (std::mutex): 4 bytes [native synchronization state may allocate platform resources lazily]
 * - _receiverMutex (std::mutex): 4 bytes [native synchronization state may allocate platform resources lazily]
 * - _lastConnectAttempt (uint32_t): 4 bytes [0 bytes dynamic allocation]
 * - _initialized (bool): 1 bytes [0 bytes dynamic allocation]
 * Total Memory: 253 bytes known/aligned storage + sizeof(WiFiClient) (target/toolchain dependent) + sizeof(WiFiClientSecure) (target/toolchain dependent) [SocketWorker: _observable: shared control block (~12+ bytes; allocate_shared may co-locate object) + object 96 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: enable_shared_from_this: embedded weak_ptr shares a control block when activated; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: shared control block (~12+ bytes; allocate_shared may co-locate object) + object 20 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: pointee: _mutex: native synchronization state may allocate platform resources lazily; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: pointee: _condition: native condition-variable state may allocate platform synchronization resources; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: _registrations: Capacity * (12 bytes) element storage; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: _bindings: Capacity * (12 bytes) element storage; SocketWorker: _observable: pointee: ThreadSafeObservable: _mutex: _owned: owned object: 4 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: _mutex: _fallback: _mutex: native synchronization state may allocate platform resources lazily; SocketWorker: _observable: pointee: ThreadSafeObservable: _notificationMutex: _owned: owned object: 4 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: _notificationMutex: _fallback: _mutex: native synchronization state may allocate platform resources lazily; _mqtt: owned object: sizeof(PubSubClient) (target/toolchain dependent); _config: Host: Capacity + 1 bytes when capacity exceeds 15-byte SSO; _config: ClientID: Capacity + 1 bytes when capacity exceeds 15-byte SSO; _config: Username: Capacity + 1 bytes when capacity exceeds 15-byte SSO; _config: Password: Capacity + 1 bytes when capacity exceeds 15-byte SSO; _config: OutboundTopic: Capacity + 1 bytes when capacity exceeds 15-byte SSO; _config: InboundTopic: Capacity + 1 bytes when capacity exceeds 15-byte SSO; _mqttMutex: native synchronization state may allocate platform resources lazily; _receiverMutex: native synchronization state may allocate platform resources lazily]
 * Basis: ESP32/Xtensa ILP32 reference ABI (4-byte pointers/size_t); ESPressio stateful allocators/deleters included; ABI-sensitive STL/platform internals are identified explicitly.
 * Confidence: low; compile-time sizeof on the concrete target remains authoritative for ABI-sensitive/opaque members.
 * End ESPressio Memory Audit
 */
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
