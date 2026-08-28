#pragma once

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_HTTPD_WS_SUPPORT)

#if !__has_include(<ESPressio_WebSocketEndpoint.hpp>)
#error "ESPressio ESP32 WebSocket endpoint provider requires ESPressio-Web."
#endif

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>

#include <esp_err.h>
#include <esp_http_server.h>

#include <ESPressio_Memory.hpp>
#include <ESPressio_WebSocketEndpoint.hpp>

#include "ESPressio_HttpServerPlatform.hpp"

#ifndef ESPRESSIO_ESP32_WEBSOCKET_MAX_MESSAGE_BYTES
#define ESPRESSIO_ESP32_WEBSOCKET_MAX_MESSAGE_BYTES 65536
#endif

namespace ESPressio::Web {

namespace Detail {

inline WebResult ESP32WebSocketResult(esp_err_t result) noexcept {
    if (result == ESP_OK) return WebResult::Success();
    switch (result) {
        case ESP_ERR_INVALID_ARG:
            return WebResult::Failure(WebError::InvalidConfiguration, result);
        case ESP_ERR_NO_MEM:
            return WebResult::Failure(WebError::ResourceExhausted, result);
        case ESP_ERR_INVALID_STATE:
            return WebResult::Failure(WebError::InvalidState, result);
        case ESP_ERR_NOT_FOUND:
            return WebResult::Failure(WebError::Closed, result);
        default:
            return WebResult::Failure(WebError::ConnectionFailure, result);
    }
}

inline uint16_t ReadNetworkUInt16(const uint8_t* data) noexcept {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8U) |
        static_cast<uint16_t>(data[1])
    );
}

inline WebSocketFrameType ESP32WebSocketFrameType(httpd_ws_type_t type) noexcept {
    switch (type) {
        case HTTPD_WS_TYPE_TEXT: return WebSocketFrameType::Text;
        case HTTPD_WS_TYPE_BINARY: return WebSocketFrameType::Binary;
        case HTTPD_WS_TYPE_PING: return WebSocketFrameType::Ping;
        case HTTPD_WS_TYPE_PONG: return WebSocketFrameType::Pong;
        case HTTPD_WS_TYPE_CLOSE: return WebSocketFrameType::Close;
        default: return WebSocketFrameType::Binary;
    }
}

} // namespace Detail

class ESP32WebSocketConnection final : public IWebSocketConnection {
private:
    using ByteBuffer = System::Memory::Vector<
        uint8_t,
        System::Memory::MemoryPolicy::ExternalPreferred
    >;
    using WorkingString = System::Memory::String<
        System::Memory::MemoryPolicy::ExternalPreferred
    >;

    struct SendOperation final {
        System::Memory::IMemoryProvider* AllocationProvider = nullptr;
        httpd_handle_t Server = nullptr;
        int Socket = -1;
        bool CloseAfterSend = false;
        ByteBuffer Payload;
        httpd_ws_frame_t Frame{};
    };

    using SendOperationAllocator = System::Memory::Allocator<
        SendOperation,
        System::Memory::MemoryPolicy::ExternalPreferred
    >;

public:
    ESP32WebSocketConnection(
        httpd_handle_t server,
        int socket,
        IWebSocketEndpointPlatformSink* activitySink
    ) : _server(server),
        _socket(socket),
        _id(static_cast<WebSocketConnectionId>(static_cast<uint32_t>(socket))),
        _activitySink(activitySink) {}

    WebSocketConnectionId Id() const noexcept override { return _id; }

    bool IsOpen() const noexcept override {
        return _open.load(std::memory_order_acquire) &&
               _server != nullptr &&
               httpd_ws_get_fd_info(_server, _socket) == HTTPD_WS_CLIENT_WEBSOCKET;
    }

    void SetActivitySink(IWebSocketEndpointPlatformSink* sink) noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        _activitySink = sink;
    }

    WebResult SendBinary(const uint8_t* data, std::size_t size) override {
        if (data == nullptr || size == 0) {
            return WebResult::Failure(WebError::InvalidConfiguration);
        }
        return QueueFrame(HTTPD_WS_TYPE_BINARY, data, size, false);
    }

    WebResult SendText(std::string_view text) override {
        return QueueFrame(
            HTTPD_WS_TYPE_TEXT,
            reinterpret_cast<const uint8_t*>(text.data()),
            text.size(),
            false
        );
    }

    WebResult Close(const WebSocketCloseReason& reason = {}) override {
        bool expected = true;
        if (!_open.compare_exchange_strong(
                expected,
                false,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return WebResult::Success();
        }

        {
            std::lock_guard<std::mutex> lock(_mutex);
            _closeCode = reason.Code;
            _closeReason.assign(reason.Reason.begin(), reason.Reason.end());
        }

        NotifyActivity({
            WebSocketActivityKind::CloseRequested,
            _id,
            WebSocketFrameType::Close,
            reason.Reason.size(),
            WebResult::Success(),
            reason.Code,
            reason.Reason
        });

        const std::size_t reasonBytes = std::min<std::size_t>(reason.Reason.size(), 123);
        ByteBuffer payload(2 + reasonBytes);
        payload[0] = static_cast<uint8_t>((reason.Code >> 8U) & 0xffU);
        payload[1] = static_cast<uint8_t>(reason.Code & 0xffU);
        if (reasonBytes != 0) {
            std::memcpy(payload.data() + 2, reason.Reason.data(), reasonBytes);
        }

        const auto result = QueueOwnedFrame(
            HTTPD_WS_TYPE_CLOSE,
            std::move(payload),
            true,
            true
        );
        if (!result && _server != nullptr) {
            (void)httpd_sess_trigger_close(_server, _socket);
        }
        return result;
    }

    void MarkClosed() noexcept {
        _open.store(false, std::memory_order_release);
    }

    WebSocketCloseReason CloseReason() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return {
            _closeCode,
            std::string_view(_closeReason.data(), _closeReason.size())
        };
    }

    WebResult RememberPeerClose(const uint8_t* payload, std::size_t size) {
        std::lock_guard<std::mutex> lock(_mutex);
        _open.store(false, std::memory_order_release);
        _closeCode = size >= 2 && payload != nullptr ? Detail::ReadNetworkUInt16(payload) : 1000;
        _closeReason.clear();
        if (size > 2 && payload != nullptr) {
            _closeReason.assign(
                reinterpret_cast<const char*>(payload + 2),
                reinterpret_cast<const char*>(payload + size)
            );
        }
        return WebResult::Success();
    }

    WebResult AccumulateFragment(
        httpd_ws_type_t type,
        bool final,
        ByteBuffer&& payload,
        httpd_ws_type_t& completedType,
        ByteBuffer& completedPayload,
        bool& completed
    ) {
        completed = false;
        std::lock_guard<std::mutex> lock(_mutex);
        const std::size_t size = payload.size();

        if (type == HTTPD_WS_TYPE_TEXT || type == HTTPD_WS_TYPE_BINARY) {
            if (_fragmenting) {
                return WebResult::Failure(WebError::ProtocolError);
            }
            if (final) {
                completedType = type;
                completedPayload = std::move(payload);
                completed = true;
                return WebResult::Success();
            }
            if (size > ESPRESSIO_ESP32_WEBSOCKET_MAX_MESSAGE_BYTES) {
                return WebResult::Failure(WebError::ResourceExhausted);
            }
            _fragmenting = true;
            _fragmentType = type;
            _fragmentBuffer = std::move(payload);
            return WebResult::Success();
        }

        if (type != HTTPD_WS_TYPE_CONTINUE || !_fragmenting) {
            return WebResult::Failure(WebError::ProtocolError);
        }

        if (_fragmentBuffer.size() + size > ESPRESSIO_ESP32_WEBSOCKET_MAX_MESSAGE_BYTES) {
            _fragmenting = false;
            _fragmentBuffer.clear();
            return WebResult::Failure(WebError::ResourceExhausted);
        }
        if (size != 0) {
            _fragmentBuffer.insert(
                _fragmentBuffer.end(),
                payload.begin(),
                payload.end()
            );
        }
        if (final) {
            completedType = _fragmentType;
            completedPayload = std::move(_fragmentBuffer);
            _fragmentBuffer.clear();
            _fragmenting = false;
            completed = true;
        }
        return WebResult::Success();
    }

private:
    static SendOperation* AllocateSendOperation() {
        SendOperationAllocator allocator;
        SendOperation* operation = allocator.allocate(1);
        try {
            ::new (static_cast<void*>(operation)) SendOperation();
        } catch (...) {
            allocator.deallocate(operation, 1);
            throw;
        }
        operation->AllocationProvider = allocator.Provider();
        return operation;
    }

    static void DestroySendOperation(SendOperation* operation) noexcept {
        if (operation == nullptr) return;
        auto* provider = operation->AllocationProvider;
        operation->~SendOperation();
        if (provider != nullptr) {
            provider->Deallocate(
                operation,
                sizeof(SendOperation),
                alignof(SendOperation),
                System::Memory::MemoryPolicy::ExternalPreferred
            );
        }
    }

    static void SendCompleted(esp_err_t, int, void* argument) {
        auto* operation = static_cast<SendOperation*>(argument);
        if (operation == nullptr) return;
        const bool closeAfterSend = operation->CloseAfterSend;
        const auto server = operation->Server;
        const int socket = operation->Socket;
        DestroySendOperation(operation);
        if (closeAfterSend && server != nullptr) {
            (void)httpd_sess_trigger_close(server, socket);
        }
    }

    void NotifyActivity(const WebSocketActivity& activity) const {
        IWebSocketEndpointPlatformSink* sink = nullptr;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            sink = _activitySink;
        }
        if (sink != nullptr) sink->OnPlatformWebSocketActivity(activity);
    }

    WebResult QueueFrame(
        httpd_ws_type_t type,
        const uint8_t* data,
        std::size_t size,
        bool closeAfterSend
    ) {
        if (!_open.load(std::memory_order_acquire) || _server == nullptr) {
            const auto result = WebResult::Failure(WebError::Closed);
            NotifyActivity({
                WebSocketActivityKind::SendFailed,
                _id,
                Detail::ESP32WebSocketFrameType(type),
                size,
                result,
                0,
                "connection closed before send"
            });
            return result;
        }
        ByteBuffer payload;
        if (size != 0) {
            if (data == nullptr) return WebResult::Failure(WebError::InvalidConfiguration);
            payload.assign(data, data + size);
        }
        return QueueOwnedFrame(type, std::move(payload), closeAfterSend, false);
    }

    WebResult QueueOwnedFrame(
        httpd_ws_type_t type,
        ByteBuffer&& payload,
        bool closeAfterSend,
        bool allowClosed
    ) {
        const auto payloadBytes = payload.size();
        if ((!allowClosed && !_open.load(std::memory_order_acquire)) || _server == nullptr) {
            const auto result = WebResult::Failure(WebError::Closed);
            NotifyActivity({WebSocketActivityKind::SendFailed, _id,
                Detail::ESP32WebSocketFrameType(type), payloadBytes, result, 0,
                "connection unavailable"});
            return result;
        }
        if (httpd_ws_get_fd_info(_server, _socket) != HTTPD_WS_CLIENT_WEBSOCKET) {
            const auto result = WebResult::Failure(WebError::Closed);
            NotifyActivity({WebSocketActivityKind::SendFailed, _id,
                Detail::ESP32WebSocketFrameType(type), payloadBytes, result, 0,
                "HTTPD session is not a websocket"});
            return result;
        }

        SendOperation* operation = nullptr;
        try {
            operation = AllocateSendOperation();
        } catch (const std::bad_alloc&) {
            const auto result = WebResult::Failure(WebError::ResourceExhausted);
            NotifyActivity({WebSocketActivityKind::SendFailed, _id,
                Detail::ESP32WebSocketFrameType(type), payloadBytes, result, 0,
                "send operation allocation failed"});
            return result;
        } catch (...) {
            const auto result = WebResult::Failure(WebError::PlatformFailure);
            NotifyActivity({WebSocketActivityKind::SendFailed, _id,
                Detail::ESP32WebSocketFrameType(type), payloadBytes, result, 0,
                "send operation construction failed"});
            return result;
        }

        operation->Server = _server;
        operation->Socket = _socket;
        operation->CloseAfterSend = closeAfterSend;
        operation->Payload = std::move(payload);
        operation->Frame.final = true;
        operation->Frame.fragmented = false;
        operation->Frame.type = type;
        operation->Frame.payload = operation->Payload.empty()
            ? nullptr
            : operation->Payload.data();
        operation->Frame.len = operation->Payload.size();

        const auto queued = httpd_ws_send_data_async(
            _server,
            _socket,
            &operation->Frame,
            &SendCompleted,
            operation
        );
        if (queued != ESP_OK) {
            const auto result = Detail::ESP32WebSocketResult(queued);
            DestroySendOperation(operation);
            NotifyActivity({WebSocketActivityKind::SendFailed, _id,
                Detail::ESP32WebSocketFrameType(type), payloadBytes, result, 0,
                "httpd_ws_send_data_async rejected frame"});
            return result;
        }

        NotifyActivity({
            WebSocketActivityKind::SendQueued,
            _id,
            Detail::ESP32WebSocketFrameType(type),
            payloadBytes,
            WebResult::Success(),
            0,
            "frame queued to HTTPD"
        });
        return WebResult::Success();
    }

    httpd_handle_t _server = nullptr;
    int _socket = -1;
    WebSocketConnectionId _id = 0;
    std::atomic<bool> _open{true};
    mutable std::mutex _mutex;
    IWebSocketEndpointPlatformSink* _activitySink = nullptr;
    uint16_t _closeCode = 1006;
    WorkingString _closeReason;
    bool _fragmenting = false;
    httpd_ws_type_t _fragmentType = HTTPD_WS_TYPE_BINARY;
    ByteBuffer _fragmentBuffer;
};

class ESP32WebSocketEndpointPlatform final : public IWebSocketEndpointPlatform {
private:
    class BindingState final : public IESP32HttpWebSocketBinding {
    private:
        using WorkingString = System::Memory::String<
            System::Memory::MemoryPolicy::ExternalPreferred
        >;
        using ByteBuffer = System::Memory::Vector<
            uint8_t,
            System::Memory::MemoryPolicy::ExternalPreferred
        >;
        using ConnectionPtr = std::shared_ptr<ESP32WebSocketConnection>;
        using ConnectionList = System::Memory::Vector<
            ConnectionPtr,
            System::Memory::MemoryPolicy::ExternalPreferred
        >;

    public:
        void SetSink(IWebSocketEndpointPlatformSink* sink) {
            std::lock_guard<std::mutex> lock(_mutex);
            _sink = sink;
            for (const auto& connection : _connections) {
                if (connection) connection->SetActivitySink(sink);
            }
        }

        WebResult Prepare(const WebSocketEndpointConfiguration& configuration) {
            if (configuration.Path.empty() || configuration.Path.front() != '/') {
                return WebResult::Failure(WebError::InvalidConfiguration);
            }
            std::lock_guard<std::mutex> lock(_mutex);
            if (_active) return WebResult::Failure(WebError::AlreadyRunning);
            _path.assign(configuration.Path.begin(), configuration.Path.end());
            _protocol.assign(configuration.Protocol.begin(), configuration.Protocol.end());
            return WebResult::Success();
        }

        void Activate() noexcept { _active.store(true, std::memory_order_release); }
        void Deactivate() noexcept { _active.store(false, std::memory_order_release); }
        bool IsActive() const noexcept override { return _active.load(std::memory_order_acquire); }
        const char* PathCString() const noexcept override { return _path.c_str(); }
        const char* ProtocolCString() const noexcept override { return _protocol.c_str(); }

        std::size_t ConnectionCount() const noexcept {
            std::lock_guard<std::mutex> lock(_mutex);
            return _connections.size();
        }

        WebResult BroadcastBinary(const uint8_t* data, std::size_t size) {
            if (data == nullptr || size == 0) {
                return WebResult::Failure(WebError::InvalidConfiguration);
            }
            const auto connections = SnapshotConnections();
            WebResult finalResult = WebResult::Success();
            for (const auto& connection : connections) {
                if (!connection) continue;
                const auto result = connection->SendBinary(data, size);
                if (!result) finalResult = result;
            }
            return finalResult;
        }

        WebResult BroadcastText(std::string_view text) {
            const auto connections = SnapshotConnections();
            WebResult finalResult = WebResult::Success();
            for (const auto& connection : connections) {
                if (!connection) continue;
                const auto result = connection->SendText(text);
                if (!result) finalResult = result;
            }
            return finalResult;
        }

        WebResult CloseAll(const WebSocketCloseReason& reason) {
            const auto connections = SnapshotConnections();
            WebResult finalResult = WebResult::Success();
            for (const auto& connection : connections) {
                if (!connection) continue;
                const auto result = connection->Close(reason);
                if (!result) finalResult = result;
            }
            return finalResult;
        }

        esp_err_t HandleWebSocketRequest(httpd_req_t& request) override {
            if (!IsActive()) return ESP_FAIL;

            const int socket = httpd_req_to_sockfd(&request);
            if (socket < 0) return ESP_FAIL;
            const auto id = static_cast<WebSocketConnectionId>(static_cast<uint32_t>(socket));

            if (request.method == HTTP_GET) {
                auto* sink = SnapshotSink();
                if (sink != nullptr) {
                    sink->OnPlatformWebSocketActivity({
                        WebSocketActivityKind::UpgradeRequested,
                        id,
                        WebSocketFrameType::Text,
                        0,
                        WebResult::Success(),
                        0,
                        "HTTPD websocket upgrade request"
                    });
                }

                ConnectionPtr connection;
                bool created = false;
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    connection = FindConnectionLocked(socket);
                    if (!connection) {
                        connection = System::Memory::MakeShared<
                            ESP32WebSocketConnection,
                            System::Memory::MemoryPolicy::ExternalPreferred
                        >(request.handle, socket, _sink);
                        _connections.push_back(connection);
                        created = true;
                    }
                    sink = _sink;
                }
                if (sink != nullptr && created) {
                    sink->OnPlatformWebSocketActivity({
                        WebSocketActivityKind::ConnectionCreated,
                        connection->Id(),
                        WebSocketFrameType::Text,
                        0,
                        WebResult::Success(),
                        0,
                        "ESP32 websocket connection object created"
                    });
                }
                if (sink != nullptr) sink->OnPlatformWebSocketConnected(*connection);
                return ESP_OK;
            }

            auto connection = FindConnection(socket);
            if (!connection) {
                auto* sink = SnapshotSink();
                if (sink != nullptr) {
                    sink->OnPlatformWebSocketActivity({
                        WebSocketActivityKind::ReceiveFailed, id, WebSocketFrameType::Binary, 0,
                        WebResult::Failure(WebError::NotFound), 0,
                        "frame arrived without tracked websocket connection"
                    });
                }
                return ESP_FAIL;
            }

            httpd_ws_frame_t frame{};
            auto received = httpd_ws_recv_frame(&request, &frame, 0);
            if (received != ESP_OK) {
                NotifyActivity(*connection, {
                    WebSocketActivityKind::ReceiveFailed,
                    connection->Id(),
                    WebSocketFrameType::Binary,
                    0,
                    Detail::ESP32WebSocketResult(received),
                    0,
                    "failed reading websocket frame header"
                });
                return received;
            }

            NotifyActivity(*connection, {
                WebSocketActivityKind::FrameHeaderReceived,
                connection->Id(),
                Detail::ESP32WebSocketFrameType(frame.type),
                frame.len,
                WebResult::Success(),
                0,
                frame.final ? "final frame header" : "fragmented frame header"
            });

            if (frame.len > ESPRESSIO_ESP32_WEBSOCKET_MAX_MESSAGE_BYTES) {
                NotifyActivity(*connection, {
                    WebSocketActivityKind::ProtocolError,
                    connection->Id(),
                    Detail::ESP32WebSocketFrameType(frame.type),
                    frame.len,
                    WebResult::Failure(WebError::ResourceExhausted),
                    1009,
                    "websocket message exceeds configured maximum"
                });
                (void)connection->Close({1009, "message too large"});
                return ESP_OK;
            }

            ByteBuffer payload(frame.len);
            frame.payload = payload.empty() ? nullptr : payload.data();
            received = httpd_ws_recv_frame(&request, &frame, frame.len);
            if (received != ESP_OK) {
                NotifyActivity(*connection, {
                    WebSocketActivityKind::ReceiveFailed,
                    connection->Id(),
                    Detail::ESP32WebSocketFrameType(frame.type),
                    frame.len,
                    Detail::ESP32WebSocketResult(received),
                    0,
                    "failed reading websocket frame payload"
                });
                return received;
            }

            NotifyActivity(*connection, {
                WebSocketActivityKind::FramePayloadReceived,
                connection->Id(),
                Detail::ESP32WebSocketFrameType(frame.type),
                payload.size(),
                WebResult::Success(),
                0,
                "websocket frame payload received"
            });

            if (frame.type == HTTPD_WS_TYPE_PING) {
                NotifyActivity(*connection, {
                    WebSocketActivityKind::PingReceived, connection->Id(), WebSocketFrameType::Ping,
                    payload.size(), WebResult::Success(), 0, "ping received; replying pong"
                });
                frame.type = HTTPD_WS_TYPE_PONG;
                return httpd_ws_send_frame(&request, &frame);
            }
            if (frame.type == HTTPD_WS_TYPE_PONG) {
                NotifyActivity(*connection, {
                    WebSocketActivityKind::PongReceived, connection->Id(), WebSocketFrameType::Pong,
                    payload.size(), WebResult::Success(), 0, "pong received"
                });
                return ESP_OK;
            }
            if (frame.type == HTTPD_WS_TYPE_CLOSE) {
                const uint16_t closeCode = payload.size() >= 2
                    ? Detail::ReadNetworkUInt16(payload.data())
                    : 1000;
                NotifyActivity(*connection, {
                    WebSocketActivityKind::PeerCloseReceived,
                    connection->Id(),
                    WebSocketFrameType::Close,
                    payload.size(),
                    WebResult::Success(),
                    closeCode,
                    "peer close frame received"
                });
                (void)connection->RememberPeerClose(payload.data(), payload.size());
                httpd_ws_frame_t closeFrame{};
                closeFrame.final = true;
                closeFrame.type = HTTPD_WS_TYPE_CLOSE;
                closeFrame.payload = payload.empty() ? nullptr : payload.data();
                closeFrame.len = payload.size();
                (void)httpd_ws_send_frame(&request, &closeFrame);
                (void)httpd_sess_trigger_close(request.handle, socket);
                return ESP_OK;
            }

            if (!frame.final &&
                (frame.type == HTTPD_WS_TYPE_TEXT || frame.type == HTTPD_WS_TYPE_BINARY)) {
                NotifyActivity(*connection, {
                    WebSocketActivityKind::FragmentStarted,
                    connection->Id(),
                    Detail::ESP32WebSocketFrameType(frame.type),
                    payload.size(),
                    WebResult::Success(),
                    0,
                    "fragmented websocket message started"
                });
            }

            httpd_ws_type_t completedType = HTTPD_WS_TYPE_BINARY;
            ByteBuffer completedPayload;
            bool completed = false;
            const auto accumulated = connection->AccumulateFragment(
                frame.type,
                frame.final,
                std::move(payload),
                completedType,
                completedPayload,
                completed
            );
            if (!accumulated) {
                const uint16_t code = accumulated.Error == WebError::ResourceExhausted
                    ? 1009
                    : 1002;
                NotifyActivity(*connection, {
                    WebSocketActivityKind::ProtocolError,
                    connection->Id(),
                    Detail::ESP32WebSocketFrameType(frame.type),
                    frame.len,
                    accumulated,
                    code,
                    "invalid websocket fragmentation sequence"
                });
                (void)connection->Close({code, "invalid websocket message"});
                return ESP_OK;
            }
            if (!completed) return ESP_OK;

            if (frame.type == HTTPD_WS_TYPE_CONTINUE || !frame.final) {
                NotifyActivity(*connection, {
                    WebSocketActivityKind::FragmentCompleted,
                    connection->Id(),
                    Detail::ESP32WebSocketFrameType(completedType),
                    completedPayload.size(),
                    WebResult::Success(),
                    0,
                    "fragmented websocket message completed"
                });
            }

            auto* sink = SnapshotSink();
            if (sink == nullptr) return ESP_OK;

            if (completedType == HTTPD_WS_TYPE_TEXT) {
                sink->OnPlatformWebSocketText(
                    *connection,
                    std::string_view(
                        reinterpret_cast<const char*>(completedPayload.data()),
                        completedPayload.size()
                    )
                );
            } else if (completedType == HTTPD_WS_TYPE_BINARY) {
                sink->OnPlatformWebSocketBinary(
                    *connection,
                    completedPayload.data(),
                    completedPayload.size()
                );
            }
            return ESP_OK;
        }

        void OnHttpSessionClosed(int socket) override {
            ConnectionPtr connection;
            IWebSocketEndpointPlatformSink* sink = nullptr;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                auto iterator = std::find_if(
                    _connections.begin(),
                    _connections.end(),
                    [&](const ConnectionPtr& candidate) {
                        return candidate &&
                               candidate->Id() == static_cast<WebSocketConnectionId>(
                                   static_cast<uint32_t>(socket)
                               );
                    }
                );
                if (iterator == _connections.end()) return;
                connection = *iterator;
                _connections.erase(iterator);
                sink = _sink;
            }

            connection->MarkClosed();
            if (sink != nullptr) {
                const auto reason = connection->CloseReason();
                sink->OnPlatformWebSocketActivity({
                    WebSocketActivityKind::SessionClosed,
                    connection->Id(),
                    WebSocketFrameType::Close,
                    reason.Reason.size(),
                    WebResult::Success(),
                    reason.Code,
                    "HTTPD websocket session closed"
                });
                sink->OnPlatformWebSocketDisconnected(connection->Id(), reason);
            }
        }

    private:
        ConnectionList SnapshotConnections() const {
            std::lock_guard<std::mutex> lock(_mutex);
            return _connections;
        }

        IWebSocketEndpointPlatformSink* SnapshotSink() const noexcept {
            std::lock_guard<std::mutex> lock(_mutex);
            return _sink;
        }

        void NotifyActivity(
            const ESP32WebSocketConnection&,
            const WebSocketActivity& activity
        ) const {
            auto* sink = SnapshotSink();
            if (sink != nullptr) sink->OnPlatformWebSocketActivity(activity);
        }

        ConnectionPtr FindConnection(int socket) const {
            std::lock_guard<std::mutex> lock(_mutex);
            return FindConnectionLocked(socket);
        }

        ConnectionPtr FindConnectionLocked(int socket) const {
            const auto id = static_cast<WebSocketConnectionId>(static_cast<uint32_t>(socket));
            for (const auto& connection : _connections) {
                if (connection && connection->Id() == id) return connection;
            }
            return {};
        }

        std::atomic<bool> _active{false};
        mutable std::mutex _mutex;
        IWebSocketEndpointPlatformSink* _sink = nullptr;
        WorkingString _path;
        WorkingString _protocol;
        ConnectionList _connections;
    };

    static std::shared_ptr<BindingState> MakeBindingState(
        IWebSocketEndpointPlatformSink* sink
    ) {
        auto state = System::Memory::MakeShared<
            BindingState,
            System::Memory::MemoryPolicy::ExternalPreferred
        >();
        state->SetSink(sink);
        return state;
    }

public:
    explicit ESP32WebSocketEndpointPlatform(ESP32HttpServerPlatform& httpPlatform)
        : _httpPlatform(httpPlatform),
          _state(MakeBindingState(nullptr)) {}

    ~ESP32WebSocketEndpointPlatform() override {
        (void)Unbind();
        _state->SetSink(nullptr);
    }

    ESP32WebSocketEndpointPlatform(const ESP32WebSocketEndpointPlatform&) = delete;
    ESP32WebSocketEndpointPlatform& operator=(const ESP32WebSocketEndpointPlatform&) = delete;

    void SetSink(IWebSocketEndpointPlatformSink* sink) override {
        _sink = sink;
        _state->SetSink(sink);
    }

    WebResult Bind(const WebSocketEndpointConfiguration& configuration) override {
        if (IsBound()) return WebResult::Failure(WebError::AlreadyRunning);
        NotifyActivity({
            WebSocketActivityKind::BindRequested,
            0,
            WebSocketFrameType::Text,
            0,
            WebResult::Success(),
            0,
            configuration.Path
        });

        auto result = _state->Prepare(configuration);
        if (!result) {
            NotifyActivity({WebSocketActivityKind::ProtocolError, 0, WebSocketFrameType::Text,
                0, result, 0, "websocket endpoint prepare failed"});
            return result;
        }

        _state->Activate();
        result = _httpPlatform.AddWebSocketBinding(_state);
        if (!result) {
            _state->Deactivate();
            NotifyActivity({WebSocketActivityKind::ProtocolError, 0, WebSocketFrameType::Text,
                0, result, 0, "HTTPD websocket binding registration failed"});
            return result;
        }

        NotifyActivity({
            WebSocketActivityKind::Bound,
            0,
            WebSocketFrameType::Text,
            0,
            WebResult::Success(),
            0,
            configuration.Path
        });
        return WebResult::Success();
    }

    WebResult Unbind() override {
        auto retired = _state;
        if (!retired->IsActive()) {
            _httpPlatform.ReleaseWebSocketBinding(retired);
            return WebResult::Success();
        }

        const auto closeResult = retired->CloseAll({1001, "endpoint unbound"});
        retired->Deactivate();
        _httpPlatform.ReleaseWebSocketBinding(retired);

        // Never reactivate a BindingState that may still be retained by the
        // running native HTTP server as a handler user_ctx. A later Bind uses a
        // fresh state and therefore cannot transiently re-enable a stale route.
        _state = MakeBindingState(_sink);
        NotifyActivity({
            WebSocketActivityKind::Unbound,
            0,
            WebSocketFrameType::Close,
            0,
            closeResult,
            1001,
            "websocket endpoint unbound"
        });
        return closeResult;
    }

    bool IsBound() const noexcept override { return _state->IsActive(); }
    std::size_t ConnectionCount() const noexcept override { return _state->ConnectionCount(); }

    WebResult BroadcastBinary(const uint8_t* data, std::size_t size) override {
        return IsBound()
            ? _state->BroadcastBinary(data, size)
            : WebResult::Failure(WebError::InvalidState);
    }

    WebResult BroadcastText(std::string_view text) override {
        return IsBound()
            ? _state->BroadcastText(text)
            : WebResult::Failure(WebError::InvalidState);
    }

    WebResult CloseAll(const WebSocketCloseReason& reason = {}) override {
        return _state->CloseAll(reason);
    }

private:
    void NotifyActivity(const WebSocketActivity& activity) const {
        if (_sink != nullptr) _sink->OnPlatformWebSocketActivity(activity);
    }

    ESP32HttpServerPlatform& _httpPlatform;
    IWebSocketEndpointPlatformSink* _sink = nullptr;
    std::shared_ptr<BindingState> _state;
};

} // namespace ESPressio::Web

#endif // ARDUINO_ARCH_ESP32 && CONFIG_HTTPD_WS_SUPPORT
