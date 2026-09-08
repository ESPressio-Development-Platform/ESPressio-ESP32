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

/**
 * ESPressio Memory Audit
 * Members:
 * - Port (uint16_t): 2 bytes [0 bytes dynamic allocation]
 * - MaximumClients (std::size_t): 4 bytes [0 bytes dynamic allocation]
 * - Worker (SocketWorkerConfig): 16 bytes [0 bytes dynamic allocation]
 * Total Memory: 24 bytes [0 bytes dynamic allocation]
 * Basis: ESP32/Xtensa ILP32 reference ABI (4-byte pointers/size_t); ESPressio stateful allocators/deleters included; ABI-sensitive STL/platform internals are identified explicitly.
 * End ESPressio Memory Audit
 */
struct TCPServerEventTransportConfig {
    uint16_t Port = 0;
    std::size_t MaximumClients = ESPRESSIO_SOCKETS_MAX_TCP_CLIENTS;
    SocketWorkerConfig Worker;
};

/**
 * ESPressio Memory Audit
 * Inherited Memory Total: 40 bytes [SocketWorker: _observable: shared control block (~12+ bytes; allocate_shared may co-locate object) + object 96 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: enable_shared_from_this: embedded weak_ptr shares a control block when activated; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: shared control block (~12+ bytes; allocate_shared may co-locate object) + object 20 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: pointee: _mutex: native synchronization state may allocate platform resources lazily; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: pointee: _condition: native condition-variable state may allocate platform synchronization resources; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: _registrations: Capacity * (12 bytes) element storage; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: _bindings: Capacity * (12 bytes) element storage; SocketWorker: _observable: pointee: ThreadSafeObservable: _mutex: _owned: owned object: 4 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: _mutex: _fallback: _mutex: native synchronization state may allocate platform resources lazily; SocketWorker: _observable: pointee: ThreadSafeObservable: _notificationMutex: _owned: owned object: 4 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: _notificationMutex: _fallback: _mutex: native synchronization state may allocate platform resources lazily]
 * Members:
 * - _server (std::unique_ptr<WiFiServer>): 4 bytes [owned object: sizeof(WiFiServer) (target/toolchain dependent)]
 * - _clients (std::array<ClientState, ESPRESSIO_SOCKETS_MAX_TCP_CLIENTS>): ESPRESSIO_SOCKETS_MAX_TCP_CLIENTS * (13 bytes known/aligned storage + sizeof(WiFiClient) (target/toolchain dependent)) [elements: Decoder: _buffer: Capacity * (1 bytes) element storage]
 * - _config (TCPServerEventTransportConfig): 24 bytes [0 bytes dynamic allocation]
 * - _receiver (Event::IEventTransportReceiver*): 4 bytes [0 bytes dynamic allocation]
 * - _clientsMutex (std::mutex): 4 bytes [native synchronization state may allocate platform resources lazily]
 * - _receiverMutex (std::mutex): 4 bytes [native synchronization state may allocate platform resources lazily]
 * - _initialized (bool): 1 bytes [0 bytes dynamic allocation]
 * Total Memory: 81 bytes known/aligned storage + ESPRESSIO_SOCKETS_MAX_TCP_CLIENTS * (13 bytes known/aligned storage + sizeof(WiFiClient) (target/toolchain dependent)) [SocketWorker: _observable: shared control block (~12+ bytes; allocate_shared may co-locate object) + object 96 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: enable_shared_from_this: embedded weak_ptr shares a control block when activated; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: shared control block (~12+ bytes; allocate_shared may co-locate object) + object 20 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: pointee: _mutex: native synchronization state may allocate platform resources lazily; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: IUntypedObservable: IObservable: _lifetimeControl: pointee: _condition: native condition-variable state may allocate platform synchronization resources; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: _registrations: Capacity * (12 bytes) element storage; SocketWorker: _observable: pointee: ThreadSafeObservable: Observable: _bindings: Capacity * (12 bytes) element storage; SocketWorker: _observable: pointee: ThreadSafeObservable: _mutex: _owned: owned object: 4 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: _mutex: _fallback: _mutex: native synchronization state may allocate platform resources lazily; SocketWorker: _observable: pointee: ThreadSafeObservable: _notificationMutex: _owned: owned object: 4 bytes; SocketWorker: _observable: pointee: ThreadSafeObservable: _notificationMutex: _fallback: _mutex: native synchronization state may allocate platform resources lazily; _server: owned object: sizeof(WiFiServer) (target/toolchain dependent); _clients: elements: Decoder: _buffer: Capacity * (1 bytes) element storage; _clientsMutex: native synchronization state may allocate platform resources lazily; _receiverMutex: native synchronization state may allocate platform resources lazily]
 * Basis: ESP32/Xtensa ILP32 reference ABI (4-byte pointers/size_t); ESPressio stateful allocators/deleters included; ABI-sensitive STL/platform internals are identified explicitly.
 * Confidence: low; compile-time sizeof on the concrete target remains authoritative for ABI-sensitive/opaque members.
 * End ESPressio Memory Audit
 */
class TCPServerEventTransport final :
    public Event::IEventTransport,
    private SocketWorker {
private:
/**
 * ESPressio Memory Audit
 * Members:
 * - Client (WiFiClient): sizeof(WiFiClient) (target/toolchain dependent) [0 bytes dynamic allocation]
 * - Decoder (SocketEventFrameDecoder): 12 bytes [_buffer: Capacity * (1 bytes) element storage]
 * - Active (bool): 1 bytes [0 bytes dynamic allocation]
 * Total Memory: 13 bytes known/aligned storage + sizeof(WiFiClient) (target/toolchain dependent) [Decoder: _buffer: Capacity * (1 bytes) element storage]
 * Basis: ESP32/Xtensa ILP32 reference ABI (4-byte pointers/size_t); ESPressio stateful allocators/deleters included; ABI-sensitive STL/platform internals are identified explicitly.
 * Confidence: low; compile-time sizeof on the concrete target remains authoritative for ABI-sensitive/opaque members.
 * End ESPressio Memory Audit
 */
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
