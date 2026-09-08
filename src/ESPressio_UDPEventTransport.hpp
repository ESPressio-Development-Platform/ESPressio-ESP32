#pragma once

#if !__has_include(<ESPressio_EventTransport.hpp>)
#error "UDPEventTransport requires ESPressio Event >= 5.6.2 < 6.0.0."
#endif

#include <array>
#include <mutex>
#include <vector>

#include <WiFiUdp.h>
#include <ESPressio_EventTransport.hpp>

#include <ESPressio_SocketTypes.hpp>
#include <ESPressio_SocketWorker.hpp>

namespace ESPressio::Sockets {

/**
 * ESPressio Memory Audit
 * Members:
 * - LocalPort (uint16_t): 2 bytes [0 bytes dynamic allocation]
 * - AllowBroadcast (bool): 1 bytes [0 bytes dynamic allocation]
 * - Worker (SocketWorkerConfig): 16 bytes [0 bytes dynamic allocation]
 * Total Memory: 20 bytes [0 bytes dynamic allocation]
 * Basis: ESP32/Xtensa ILP32 reference ABI (4-byte pointers/size_t); ESPressio stateful allocators/deleters included; ABI-sensitive STL/platform internals are identified explicitly.
 * End ESPressio Memory Audit
 */
struct UDPEventTransportConfig {
    uint16_t LocalPort = 0;
    bool AllowBroadcast = true;
    SocketWorkerConfig Worker;
};

/**
 * ESPressio Memory Audit
 * Inherited Memory Total: 40 bytes [SocketWorker: _observable: shared control block (~12+ bytes; allocate_shared may co-locate object) + object 96 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: enable_shared_from_this: embedded weak_ptr shares a control block when activated; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: shared control block (~12+ bytes; allocate_shared may co-locate object) + object 20 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: pointee: _mutex: native synchronization state may allocate platform resources lazily; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: pointee: _condition: native condition-variable state may allocate platform synchronization resources; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: _registrations: Capacity * (12 bytes) element storage; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: _bindings: Capacity * (12 bytes) element storage; SocketWorker: _observable: pointee: ThreadSafeObservable: _mutex: _owned: owned object: 4 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: _mutex: _fallback: _mutex: native synchronization state may allocate platform resources lazily; SocketWorker: _observable: pointee: ThreadSafeObservable: _notificationMutex: _owned: owned object: 4 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: _notificationMutex: _fallback: _mutex: native synchronization state may allocate platform resources lazily]
 * Members:
 * - _udp (WiFiUDP): sizeof(WiFiUDP) (target/toolchain dependent) [0 bytes dynamic allocation]
 * - _config (UDPEventTransportConfig): 20 bytes [0 bytes dynamic allocation]
 * - _destinations (std::array<SocketEndpoint, ESPRESSIO_SOCKETS_MAX_UDP_DESTINATIONS>): ESPRESSIO_SOCKETS_MAX_UDP_DESTINATIONS * (6 bytes) [0 bytes dynamic allocation]
 * - _destinationCount (std::size_t): 4 bytes [0 bytes dynamic allocation]
 * - _receiver (Event::IEventTransportReceiver*): 4 bytes [0 bytes dynamic allocation]
 * - _mutex (std::mutex): 4 bytes [native synchronization state may allocate platform resources lazily]
 * - _udpMutex (std::mutex): 4 bytes [native synchronization state may allocate platform resources lazily]
 * - _initialized (bool): 1 bytes [0 bytes dynamic allocation]
 * Total Memory: 77 bytes known/aligned storage + sizeof(WiFiUDP) (target/toolchain dependent) + ESPRESSIO_SOCKETS_MAX_UDP_DESTINATIONS * (6 bytes) [SocketWorker: _observable: shared control block (~12+ bytes; allocate_shared may co-locate object) + object 96 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: enable_shared_from_this: embedded weak_ptr shares a control block when activated; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: shared control block (~12+ bytes; allocate_shared may co-locate object) + object 20 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: pointee: _mutex: native synchronization state may allocate platform resources lazily; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: pointee: _condition: native condition-variable state may allocate platform synchronization resources; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: _registrations: Capacity * (12 bytes) element storage; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: _bindings: Capacity * (12 bytes) element storage; SocketWorker: _observable: pointee: ThreadSafeObservable: _mutex: _owned: owned object: 4 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: _mutex: _fallback: _mutex: native synchronization state may allocate platform resources lazily; SocketWorker: _observable: pointee: ThreadSafeObservable: _notificationMutex: _owned: owned object: 4 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: _notificationMutex: _fallback: _mutex: native synchronization state may allocate platform resources lazily; _mutex: native synchronization state may allocate platform resources lazily; _udpMutex: native synchronization state may allocate platform resources lazily]
 * Basis: ESP32/Xtensa ILP32 reference ABI (4-byte pointers/size_t); ESPressio stateful allocators/deleters included; ABI-sensitive STL/platform internals are identified explicitly.
 * Confidence: low; compile-time sizeof on the concrete target remains authoritative for ABI-sensitive/opaque members.
 * End ESPressio Memory Audit
 */
class UDPEventTransport final :
    public Event::IEventTransport,
    private SocketWorker {

private:
    static IPAddress NativeAddress(const IPv4Address& address) {
        return IPAddress(
            address.Octets[0],
            address.Octets[1],
            address.Octets[2],
            address.Octets[3]
        );
    }

    WiFiUDP _udp;
    UDPEventTransportConfig _config;
    std::array<SocketEndpoint, ESPRESSIO_SOCKETS_MAX_UDP_DESTINATIONS> _destinations;
    std::size_t _destinationCount = 0;
    Event::IEventTransportReceiver* _receiver = nullptr;
    mutable std::mutex _mutex;
    mutable std::mutex _udpMutex;
    bool _initialized = false;

    void OnWorkerIteration() override {
        std::vector<uint8_t> packet;
        {
            std::lock_guard<std::mutex> udpLock(_udpMutex);
            const int packetSize = _udp.parsePacket();
            if (packetSize <= 0) return;
            if (static_cast<std::size_t>(packetSize) > ESPRESSIO_SOCKETS_MAX_EVENT_PACKET_SIZE) {
                while (_udp.available()) _udp.read();
                return;
            }
            packet.resize(static_cast<std::size_t>(packetSize));
            const int received = _udp.read(packet.data(), packet.size());
            if (received <= 0 || static_cast<std::size_t>(received) != packet.size()) return;
        }

        Event::IEventTransportReceiver* receiver = nullptr;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            receiver = _receiver;
        }
        if (receiver != nullptr) {
            receiver->ReceiveEventTransportPacket(this, packet.data(), packet.size());
        }
    }

public:
    UDPEventTransport() = default;
    ~UDPEventTransport() override { Shutdown(); }

    bool Initialize(const UDPEventTransportConfig& config) {
        if (_initialized) return true;
        if (config.LocalPort == 0 || !_udp.begin(config.LocalPort)) return false;
        _config = config;
        if (!StartWorker("ESPressioUDP", config.Worker)) {
            _udp.stop();
            return false;
        }
        _initialized = true;
        return true;
    }

    bool InitializeMulticast(
        const IPv4Address& group,
        uint16_t port,
        const SocketWorkerConfig& worker = {}
    ) {
        if (_initialized) return true;
        if (port == 0 || !_udp.beginMulticast(NativeAddress(group), port)) return false;
        _config.LocalPort = port;
        _config.Worker = worker;
        if (!StartWorker("ESPressioUDPM", worker)) {
            _udp.stop();
            return false;
        }
        _initialized = true;
        return true;
    }

    void Shutdown() {
        if (!_initialized) return;
        StopWorker();
        {
            std::lock_guard<std::mutex> udpLock(_udpMutex);
            _udp.stop();
        }
        std::lock_guard<std::mutex> lock(_mutex);
        _receiver = nullptr;
        _destinationCount = 0;
        _initialized = false;
    }

    bool GetIsInitialized() const noexcept { return _initialized; }

    bool AddDestination(const SocketEndpoint& endpoint) {
        if (!endpoint.IsValid()) return false;
        std::lock_guard<std::mutex> lock(_mutex);
        for (std::size_t i = 0; i < _destinationCount; ++i) {
            if (_destinations[i] == endpoint) return true;
        }
        if (_destinationCount >= _destinations.size()) return false;
        _destinations[_destinationCount++] = endpoint;
        return true;
    }

    bool AddBroadcastDestination(
        uint16_t port,
        const IPv4Address& broadcast = IPv4Address(255, 255, 255, 255)
    ) {
        return AddDestination(SocketEndpoint(broadcast, port));
    }

    bool AddMulticastDestination(const IPv4Address& group, uint16_t port) {
        return AddDestination(SocketEndpoint(group, port));
    }

    bool RemoveDestination(const SocketEndpoint& endpoint) {
        std::lock_guard<std::mutex> lock(_mutex);
        for (std::size_t i = 0; i < _destinationCount; ++i) {
            if (_destinations[i] == endpoint) {
                for (std::size_t move = i + 1; move < _destinationCount; ++move) {
                    _destinations[move - 1] = _destinations[move];
                }
                --_destinationCount;
                return true;
            }
        }
        return false;
    }

    void ClearDestinations() {
        std::lock_guard<std::mutex> lock(_mutex);
        _destinationCount = 0;
    }

    bool Send(const Event::EventTransportPacket& packet) override {
        if (
            !_initialized || packet.Data == nullptr || packet.Size == 0 ||
            packet.Size > ESPRESSIO_SOCKETS_MAX_EVENT_PACKET_SIZE
        ) return false;

        std::array<SocketEndpoint, ESPRESSIO_SOCKETS_MAX_UDP_DESTINATIONS> destinations;
        std::size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            count = _destinationCount;
            for (std::size_t i = 0; i < count; ++i) destinations[i] = _destinations[i];
        }
        if (count == 0) return false;

        bool success = true;
        {
            std::lock_guard<std::mutex> udpLock(_udpMutex);
            for (std::size_t i = 0; i < count; ++i) {
                if (
                    !_udp.beginPacket(NativeAddress(destinations[i].Address), destinations[i].Port) ||
                    _udp.write(packet.Data, packet.Size) != packet.Size ||
                    !_udp.endPacket()
                ) {
                    success = false;
                }
            }
        }
        return success;
    }

    void SetReceiver(Event::IEventTransportReceiver* receiver) override {
        std::lock_guard<std::mutex> lock(_mutex);
        _receiver = receiver;
    }
};

} // namespace ESPressio::Sockets
