#pragma once

#if !__has_include(<ESPressio_EventTransport.hpp>)
#error "TLSEventTransport requires ESPressio Event >= 5.6.2 < 6.0.0."
#endif

#include <array>
#include <mutex>
#include <string>
#include <vector>

#include <WiFiClientSecure.h>
#include <ESPressio_SystemPlatformClock.hpp>
#include <ESPressio_EventTransport.hpp>

#include <ESPressio_SocketEventFrame.hpp>
#include <ESPressio_SocketStreamHelpers.hpp>
#include <ESPressio_SocketWorker.hpp>

namespace ESPressio::Sockets {

/**
 * ESPressio Memory Audit
 * Members:
 * - Host (std::string): 24 bytes [Capacity + 1 bytes when capacity exceeds 15-byte SSO]
 * - Port (uint16_t): 2 bytes [0 bytes dynamic allocation]
 * - CACertificate (char*): 4 bytes [0 bytes dynamic allocation]
 * - ClientCertificate (char*): 4 bytes [0 bytes dynamic allocation]
 * - ClientPrivateKey (char*): 4 bytes [0 bytes dynamic allocation]
 * - Insecure (bool): 1 bytes [0 bytes dynamic allocation]
 * - ReconnectIntervalMilliseconds (uint32_t): 4 bytes [0 bytes dynamic allocation]
 * - Worker (SocketWorkerConfig): 16 bytes [0 bytes dynamic allocation]
 * Total Memory: 64 bytes [Host: Capacity + 1 bytes when capacity exceeds 15-byte SSO]
 * Basis: ESP32/Xtensa ILP32 reference ABI (4-byte pointers/size_t); ESPressio stateful allocators/deleters included; ABI-sensitive STL/platform internals are identified explicitly.
 * Confidence: medium; compile-time sizeof on the concrete target remains authoritative for ABI-sensitive/opaque members.
 * End ESPressio Memory Audit
 */
struct TLSEventTransportConfig {
    std::string Host;
    uint16_t Port = 0;

    const char* CACertificate = nullptr;
    const char* ClientCertificate = nullptr;
    const char* ClientPrivateKey = nullptr;

    bool Insecure = false;

    uint32_t ReconnectIntervalMilliseconds = 2000;
    SocketWorkerConfig Worker;
};

/**
 * ESPressio Memory Audit
 * Inherited Memory Total: 40 bytes [SocketWorker: _observable: shared control block (~12+ bytes; allocate_shared may co-locate object) + object 96 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: enable_shared_from_this: embedded weak_ptr shares a control block when activated; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: shared control block (~12+ bytes; allocate_shared may co-locate object) + object 20 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: pointee: _mutex: native synchronization state may allocate platform resources lazily; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: pointee: _condition: native condition-variable state may allocate platform synchronization resources; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: _registrations: Capacity * (12 bytes) element storage; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: _bindings: Capacity * (12 bytes) element storage; SocketWorker: _observable: pointee: ThreadSafeObservable: _mutex: _owned: owned object: 4 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: _mutex: _fallback: _mutex: native synchronization state may allocate platform resources lazily; SocketWorker: _observable: pointee: ThreadSafeObservable: _notificationMutex: _owned: owned object: 4 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: _notificationMutex: _fallback: _mutex: native synchronization state may allocate platform resources lazily]
 * Members:
 * - _client (WiFiClientSecure): sizeof(WiFiClientSecure) (target/toolchain dependent) [0 bytes dynamic allocation]
 * - _config (TLSEventTransportConfig): 64 bytes [Host: Capacity + 1 bytes when capacity exceeds 15-byte SSO]
 * - _decoder (SocketEventFrameDecoder): 12 bytes [_buffer: Capacity * (1 bytes) element storage]
 * - _receiver (Event::IEventTransportReceiver*): 4 bytes [0 bytes dynamic allocation]
 * - _clientMutex (std::mutex): 4 bytes [native synchronization state may allocate platform resources lazily]
 * - _receiverMutex (std::mutex): 4 bytes [native synchronization state may allocate platform resources lazily]
 * - _lastConnectAttempt (uint32_t): 4 bytes [0 bytes dynamic allocation]
 * - _initialized (bool): 1 bytes [0 bytes dynamic allocation]
 * Total Memory: 133 bytes known/aligned storage + sizeof(WiFiClientSecure) (target/toolchain dependent) [SocketWorker: _observable: shared control block (~12+ bytes; allocate_shared may co-locate object) + object 96 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: enable_shared_from_this: embedded weak_ptr shares a control block when activated; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: shared control block (~12+ bytes; allocate_shared may co-locate object) + object 20 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: pointee: _mutex: native synchronization state may allocate platform resources lazily; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: pointee: _condition: native condition-variable state may allocate platform synchronization resources; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: _registrations: Capacity * (12 bytes) element storage; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: _bindings: Capacity * (12 bytes) element storage; SocketWorker: _observable: pointee: ThreadSafeObservable: _mutex: _owned: owned object: 4 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: _mutex: _fallback: _mutex: native synchronization state may allocate platform resources lazily; SocketWorker: _observable: pointee: ThreadSafeObservable: _notificationMutex: _owned: owned object: 4 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: _notificationMutex: _fallback: _mutex: native synchronization state may allocate platform resources lazily; _config: Host: Capacity + 1 bytes when capacity exceeds 15-byte SSO; _decoder: _buffer: Capacity * (1 bytes) element storage; _clientMutex: native synchronization state may allocate platform resources lazily; _receiverMutex: native synchronization state may allocate platform resources lazily]
 * Basis: ESP32/Xtensa ILP32 reference ABI (4-byte pointers/size_t); ESPressio stateful allocators/deleters included; ABI-sensitive STL/platform internals are identified explicitly.
 * Confidence: low; compile-time sizeof on the concrete target remains authoritative for ABI-sensitive/opaque members.
 * End ESPressio Memory Audit
 */
class TLSEventTransport final :
    public Event::IEventTransport,
    private SocketWorker {

private:
    WiFiClientSecure _client;
    TLSEventTransportConfig _config;
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

    void ConfigureSecurityLocked() {
        if (_config.Insecure) {
            _client.setInsecure();
            return;
        }

        if (_config.CACertificate != nullptr) {
            _client.setCACert(_config.CACertificate);
        }

        if (
            _config.ClientCertificate != nullptr &&
            _config.ClientPrivateKey != nullptr
        ) {
            _client.setCertificate(_config.ClientCertificate);
            _client.setPrivateKey(_config.ClientPrivateKey);
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
        ConfigureSecurityLocked();
        return _client.connect(_config.Host.c_str(), _config.Port);
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
    ~TLSEventTransport() override { Shutdown(); }

    bool Initialize(const TLSEventTransportConfig& config) {
        if (_initialized) return true;
        if (config.Host.empty() || config.Port == 0) return false;

        _config = config;
        if (!StartWorker("ESPressioTLS", config.Worker)) return false;
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
