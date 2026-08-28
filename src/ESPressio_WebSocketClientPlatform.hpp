#pragma once

#if defined(ARDUINO_ARCH_ESP32) && \
    __has_include(<ESPressio_WebSocketClient.hpp>) && \
    __has_include(<esp_websocket_client.h>)

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>

#include <esp_err.h>
#include <esp_tls.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ESPressio_Memory.hpp>
#include <ESPressio_WebSocketClient.hpp>

#ifndef ESPRESSIO_ESP32_WEBSOCKET_CLIENT_MAX_MESSAGE_BYTES
#define ESPRESSIO_ESP32_WEBSOCKET_CLIENT_MAX_MESSAGE_BYTES 65536
#endif

namespace ESPressio::Web {
namespace Detail {

inline WebResult ESP32WebSocketClientResult(esp_err_t result) noexcept {
    if (result == ESP_OK) return WebResult::Success();
    switch (result) {
        case ESP_ERR_INVALID_ARG:
            return WebResult::Failure(WebError::InvalidConfiguration, result);
        case ESP_ERR_NO_MEM:
            return WebResult::Failure(WebError::ResourceExhausted, result);
        case ESP_ERR_INVALID_STATE:
            return WebResult::Failure(WebError::InvalidState, result);
        default:
            return WebResult::Failure(WebError::ConnectionFailure, result);
    }
}

inline uint16_t ESP32ReadNetworkUInt16(const uint8_t* data) noexcept {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8U) |
        static_cast<uint16_t>(data[1])
    );
}

} // namespace Detail

class ESP32WebSocketClientPlatform final : public IWebSocketClientPlatform {
private:
    using WorkingString = System::Memory::String<
        System::Memory::MemoryPolicy::ExternalPreferred
    >;
    using ByteBuffer = System::Memory::Vector<
        uint8_t,
        System::Memory::MemoryPolicy::ExternalPreferred
    >;

    struct NativeCredential final {
        ByteBuffer Bytes;
        WebCredentialEncoding Encoding = WebCredentialEncoding::Pem;

        void Clear() {
            Bytes.clear();
            Encoding = WebCredentialEncoding::Pem;
        }

        bool Empty() const noexcept { return Bytes.empty(); }
        const char* Data() const noexcept {
            return Bytes.empty()
                ? nullptr
                : reinterpret_cast<const char*>(Bytes.data());
        }

        std::size_t NativeLength() const noexcept {
            return Encoding == WebCredentialEncoding::Der ? Bytes.size() : 0;
        }

        WebResult Assign(const WebCredentialView& credential) {
            Clear();
            if (credential.Empty()) return WebResult::Success();
            if (!credential.Valid()) {
                return WebResult::Failure(WebError::InvalidConfiguration);
            }

            Encoding = credential.Encoding;
            if (credential.Encoding == WebCredentialEncoding::Der) {
                Bytes.assign(credential.Data, credential.Data + credential.Size);
                return WebResult::Success();
            }

            std::size_t logicalSize = credential.Size;
            if (logicalSize != 0 && credential.Data[logicalSize - 1] == 0) {
                --logicalSize;
            }
            for (std::size_t index = 0; index < logicalSize; ++index) {
                if (credential.Data[index] == 0) {
                    Clear();
                    return WebResult::Failure(WebError::InvalidConfiguration);
                }
            }
            Bytes.assign(credential.Data, credential.Data + logicalSize);
            Bytes.push_back(0);
            return WebResult::Success();
        }
    };

    class Connection final : public IWebSocketConnection {
    public:
        Connection(ESP32WebSocketClientPlatform& owner, WebSocketConnectionId id)
            : _owner(owner), _id(id) {}

        WebSocketConnectionId Id() const noexcept override { return _id; }
        bool IsOpen() const noexcept override {
            return _open.load(std::memory_order_acquire);
        }

        WebResult SendBinary(const uint8_t* data, std::size_t size) override {
            return _owner.SendBinary(*this, data, size);
        }

        WebResult SendText(std::string_view text) override {
            return _owner.SendText(*this, text);
        }

        WebResult Close(const WebSocketCloseReason& reason = {}) override {
            return _owner.CloseConnection(*this, reason);
        }

        void SetOpen(bool open) noexcept {
            _open.store(open, std::memory_order_release);
        }

    private:
        ESP32WebSocketClientPlatform& _owner;
        WebSocketConnectionId _id = 0;
        std::atomic<bool> _open{false};
    };

public:
    ESP32WebSocketClientPlatform() = default;
    ~ESP32WebSocketClientPlatform() override { Reset(); }

    ESP32WebSocketClientPlatform(const ESP32WebSocketClientPlatform&) = delete;
    ESP32WebSocketClientPlatform& operator=(const ESP32WebSocketClientPlatform&) = delete;

    void SetSink(IWebSocketClientPlatformSink* sink) override {
        std::lock_guard<std::mutex> lock(_mutex);
        _sink = sink;
    }

    WebResult Connect(const WebSocketClientConfiguration& configuration) override {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_client != nullptr || _disconnecting) {
            return WebResult::Failure(WebError::InvalidState);
        }

        const auto policyValidation = ValidateNativePolicy(configuration);
        if (!policyValidation) return policyValidation;

        auto credentialResult = PrepareCredentials(configuration);
        if (!credentialResult) return credentialResult;

        WorkingString host(configuration.Host.begin(), configuration.Host.end());
        WorkingString path(configuration.Path.begin(), configuration.Path.end());
        WorkingString protocol(configuration.Protocol.begin(), configuration.Protocol.end());
        WorkingString headers;
        auto headerResult = BuildNativeHeaders(configuration, headers);
        if (!headerResult) {
            ClearCredentials();
            return headerResult;
        }

        esp_websocket_client_config_t native{};
        native.host = host.c_str();
        native.port = configuration.Port;
        native.path = path.c_str();
        native.subprotocol = protocol.empty() ? nullptr : protocol.c_str();
        native.headers = headers.empty() ? nullptr : headers.c_str();
        native.transport = configuration.Transport == WebTransportMode::Tls
            ? WEBSOCKET_TRANSPORT_OVER_SSL
            : WEBSOCKET_TRANSPORT_OVER_TCP;
        native.disable_auto_reconnect = !configuration.Policy.AutomaticReconnect;
        native.ping_interval_sec = configuration.Policy.PingIntervalMilliseconds == 0
            ? 0
            : configuration.Policy.PingIntervalMilliseconds / 1000U;
        native.pingpong_timeout_sec = configuration.Policy.PongTimeoutMilliseconds == 0
            ? 0
            : static_cast<int>(configuration.Policy.PongTimeoutMilliseconds / 1000U);
        native.keep_alive_enable = configuration.Policy.TcpKeepAlive;
        if (configuration.Policy.TcpKeepAlive) {
            native.keep_alive_idle = static_cast<int>(configuration.Policy.TcpKeepAliveIdleSeconds);
            native.keep_alive_interval = static_cast<int>(configuration.Policy.TcpKeepAliveIntervalSeconds);
            native.keep_alive_count = static_cast<int>(configuration.Policy.TcpKeepAliveProbeCount);
        }

        if (configuration.Transport == WebTransportMode::Tls) {
            if (configuration.Tls.ServerTrust == WebTlsServerTrustMode::PlatformTrust) {
#if defined(CONFIG_ESP_TLS_USING_MBEDTLS)
                if (esp_tls_get_global_ca_store() == nullptr) {
                    ClearCredentials();
                    return WebResult::Failure(WebError::Unsupported);
                }
                native.use_global_ca_store = true;
#else
                ClearCredentials();
                return WebResult::Failure(WebError::Unsupported);
#endif
            } else {
                native.cert_pem = _serverCa.Data();
                native.cert_len = _serverCa.NativeLength();
            }

            if (!_clientCertificate.Empty()) {
                native.client_cert = _clientCertificate.Data();
                native.client_cert_len = _clientCertificate.NativeLength();
                native.client_key = _clientPrivateKey.Data();
                native.client_key_len = _clientPrivateKey.NativeLength();
            }
        }

        auto* client = esp_websocket_client_init(&native);
        if (client == nullptr) {
            ClearCredentials();
            return WebResult::Failure(WebError::PlatformFailure);
        }

        const auto registered = esp_websocket_register_events(
            client,
            WEBSOCKET_EVENT_ANY,
            &NativeEventHandler,
            this
        );
        if (registered != ESP_OK) {
            (void)esp_websocket_client_destroy(client);
            ClearCredentials();
            return Detail::ESP32WebSocketClientResult(registered);
        }

        _client = client;
        _connection = System::Memory::MakeShared<
            Connection,
            System::Memory::MemoryPolicy::ExternalPreferred
        >(*this, NextConnectionId());
        _connected = false;
        _disconnectNotified = true;
        _eventTask = nullptr;
        _incomingFrame.clear();
        _incomingOpcode = 0;
        _requestedCloseCode = 1000;
        _requestedCloseReason.clear();
        _peerCloseCode = 1000;
        _peerCloseReason.clear();
        _sendTimeoutTicks = MillisecondsToTicks(configuration.Policy.NetworkTimeoutMilliseconds);

        const auto started = esp_websocket_client_start(client);
        if (started != ESP_OK) {
            _client = nullptr;
            _connection.reset();
            (void)esp_websocket_client_destroy(client);
            ClearCredentials();
            return Detail::ESP32WebSocketClientResult(started);
        }
        return WebResult::Success();
    }

    WebResult Disconnect(const WebSocketCloseReason& reason = {}) override {
        esp_websocket_client_handle_t client = nullptr;
        bool wasConnected = false;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_client == nullptr) return WebResult::Success();
            if (_disconnecting) return WebResult::Failure(WebError::InvalidState);
            if (_eventTask != nullptr && _eventTask == xTaskGetCurrentTaskHandle()) {
                // IDF 4.4 explicitly forbids clean close/stop from its event task.
                return WebResult::Failure(WebError::InvalidState);
            }

            _disconnecting = true;
            client = _client;
            wasConnected = _connected;
            _requestedCloseCode = reason.Code;
            _requestedCloseReason.assign(reason.Reason.begin(), reason.Reason.end());
        }

        esp_err_t stopped = ESP_OK;
        if (esp_websocket_client_is_connected(client)) {
            const std::size_t reasonBytes = std::min<std::size_t>(reason.Reason.size(), 123);
            stopped = esp_websocket_client_close_with_code(
                client,
                reason.Code,
                reasonBytes == 0 ? nullptr : reason.Reason.data(),
                static_cast<int>(reasonBytes),
                _sendTimeoutTicks
            );
        } else {
            stopped = esp_websocket_client_stop(client);
            // A naturally stopped client can report ESP_FAIL here; destroy is
            // still the authoritative cleanup operation below.
        }

        const auto destroyed = esp_websocket_client_destroy(client);

        IWebSocketClientPlatformSink* sink = nullptr;
        std::shared_ptr<Connection> connection;
        WorkingString fallbackReason;
        bool notifyFallback = false;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_client == client) _client = nullptr;
            _connected = false;
            if (_connection) _connection->SetOpen(false);
            connection = _connection;
            _eventTask = nullptr;
            _incomingFrame.clear();
            _incomingOpcode = 0;
            ClearCredentials();
            _disconnecting = false;

            if (wasConnected && !_disconnectNotified) {
                _disconnectNotified = true;
                sink = _sink;
                fallbackReason = _requestedCloseReason;
                notifyFallback = sink != nullptr;
            }
        }

        if (notifyFallback) {
            sink->OnPlatformWebSocketClientDisconnected({
                reason.Code,
                std::string_view(fallbackReason.data(), fallbackReason.size())
            });
        }

        if (destroyed != ESP_OK) return Detail::ESP32WebSocketClientResult(destroyed);
        if (stopped != ESP_OK && wasConnected) {
            return Detail::ESP32WebSocketClientResult(stopped);
        }
        return WebResult::Success();
    }

    bool IsConnected() const noexcept override {
        std::lock_guard<std::mutex> lock(_mutex);
        return _connected;
    }

    IWebSocketConnection* Connection() noexcept override {
        std::lock_guard<std::mutex> lock(_mutex);
        return _connected && _connection ? _connection.get() : nullptr;
    }

    void Reset() noexcept {
        (void)Disconnect({1001, "client reset"});
        std::lock_guard<std::mutex> lock(_mutex);
        _sink = nullptr;
        if (_connection) _connection->SetOpen(false);
        _connection.reset();
        ClearCredentials();
    }

private:
    static constexpr uint8_t OpcodeContinuation = 0x0;
    static constexpr uint8_t OpcodeText = 0x1;
    static constexpr uint8_t OpcodeBinary = 0x2;
    static constexpr uint8_t OpcodeClose = 0x8;
    static constexpr uint8_t OpcodePing = 0x9;
    static constexpr uint8_t OpcodePong = 0xA;
    static constexpr uint32_t NativeNetworkTimeoutMilliseconds = 10000;
    static constexpr uint32_t NativeReconnectDelayMilliseconds = 10000;
    static constexpr std::size_t MaximumHandshakeHeaderCount = 64;

    static WebSocketConnectionId NextConnectionId() noexcept {
        auto id = _nextConnectionId.fetch_add(1, std::memory_order_relaxed);
        if (id == 0) id = _nextConnectionId.fetch_add(1, std::memory_order_relaxed);
        return id;
    }

    static TickType_t MillisecondsToTicks(uint32_t milliseconds) noexcept {
        TickType_t ticks = pdMS_TO_TICKS(milliseconds);
        if (milliseconds != 0 && ticks == 0) ticks = 1;
        return ticks;
    }

    static char LowerAscii(char value) noexcept {
        return value >= 'A' && value <= 'Z'
            ? static_cast<char>(value + ('a' - 'A'))
            : value;
    }

    static bool HeaderNameEquals(std::string_view left, std::string_view right) noexcept {
        if (left.size() != right.size()) return false;
        for (std::size_t index = 0; index < left.size(); ++index) {
            if (LowerAscii(left[index]) != LowerAscii(right[index])) return false;
        }
        return true;
    }

    static bool IsReservedHandshakeHeader(std::string_view name) noexcept {
        return HeaderNameEquals(name, "Host") ||
               HeaderNameEquals(name, "Connection") ||
               HeaderNameEquals(name, "Upgrade") ||
               HeaderNameEquals(name, "Sec-WebSocket-Key") ||
               HeaderNameEquals(name, "Sec-WebSocket-Version") ||
               HeaderNameEquals(name, "Sec-WebSocket-Protocol");
    }

    static bool HeaderIsSafe(const WebClientHeader& header) noexcept {
        if (header.Name.empty() || IsReservedHandshakeHeader(header.Name)) return false;
        for (const unsigned char character : header.Name) {
            if (character <= 0x20 || character >= 0x7f || character == ':') return false;
        }
        return header.Value.find('\r') == std::string_view::npos &&
               header.Value.find('\n') == std::string_view::npos;
    }

    static WebResult ValidateNativePolicy(
        const WebSocketClientConfiguration& configuration
    ) noexcept {
        if (configuration.Host.empty() || configuration.Port == 0 ||
            configuration.Path.empty() || configuration.Path.front() != '/') {
            return WebResult::Failure(WebError::InvalidConfiguration);
        }
        if (configuration.Policy.NetworkTimeoutMilliseconds !=
            NativeNetworkTimeoutMilliseconds) {
            return WebResult::Failure(WebError::Unsupported);
        }
        if (configuration.Policy.AutomaticReconnect &&
            configuration.Policy.ReconnectDelayMilliseconds !=
                NativeReconnectDelayMilliseconds) {
            return WebResult::Failure(WebError::Unsupported);
        }
        if (configuration.Policy.ReconnectAfterCleanClose) {
            return WebResult::Failure(WebError::Unsupported);
        }
        if ((configuration.Policy.PingIntervalMilliseconds % 1000U) != 0 ||
            (configuration.Policy.PongTimeoutMilliseconds % 1000U) != 0) {
            return WebResult::Failure(WebError::Unsupported);
        }
        if (configuration.Policy.TcpKeepAlive &&
            (configuration.Policy.TcpKeepAliveIdleSeconds == 0 ||
             configuration.Policy.TcpKeepAliveIntervalSeconds == 0 ||
             configuration.Policy.TcpKeepAliveProbeCount == 0 ||
             configuration.Policy.TcpKeepAliveIdleSeconds > static_cast<uint32_t>(INT_MAX) ||
             configuration.Policy.TcpKeepAliveIntervalSeconds > static_cast<uint32_t>(INT_MAX) ||
             configuration.Policy.TcpKeepAliveProbeCount > static_cast<uint32_t>(INT_MAX))) {
            return WebResult::Failure(WebError::InvalidConfiguration);
        }
        return WebResult::Success();
    }

    WebResult PrepareCredentials(const WebSocketClientConfiguration& configuration) {
        ClearCredentials();
        if (configuration.Transport != WebTransportMode::Tls) {
            if (!configuration.Tls.ServerCertificateAuthority.Empty() ||
                !configuration.Tls.ClientCertificate.Empty() ||
                !configuration.Tls.ClientPrivateKey.Empty()) {
                return WebResult::Failure(WebError::InvalidConfiguration);
            }
            return WebResult::Success();
        }

        const auto tlsValidation = configuration.Tls.Validate();
        if (!tlsValidation) return tlsValidation;

        if (configuration.Tls.ServerTrust == WebTlsServerTrustMode::CertificateAuthority) {
            auto result = _serverCa.Assign(configuration.Tls.ServerCertificateAuthority);
            if (!result) return result;
        }
        auto result = _clientCertificate.Assign(configuration.Tls.ClientCertificate);
        if (!result) {
            ClearCredentials();
            return result;
        }
        result = _clientPrivateKey.Assign(configuration.Tls.ClientPrivateKey);
        if (!result) {
            ClearCredentials();
            return result;
        }
        return WebResult::Success();
    }

    void ClearCredentials() noexcept {
        _serverCa.Clear();
        _clientCertificate.Clear();
        _clientPrivateKey.Clear();
    }

    static WebResult BuildNativeHeaders(
        const WebSocketClientConfiguration& configuration,
        WorkingString& headers
    ) {
        headers.clear();
        if (configuration.Headers == nullptr) return WebResult::Success();

        const std::size_t count = configuration.Headers->Count();
        if (count > MaximumHandshakeHeaderCount) {
            return WebResult::Failure(WebError::ResourceExhausted);
        }

        for (std::size_t index = 0; index < count; ++index) {
            WebClientHeader header;
            if (!configuration.Headers->Header(index, header) || !HeaderIsSafe(header)) {
                return WebResult::Failure(WebError::InvalidConfiguration);
            }
            const std::size_t lineBytes = header.Name.size() + header.Value.size() + 4;
            if (lineBytes > configuration.Policy.MaximumHandshakeHeaderBytes ||
                headers.size() > configuration.Policy.MaximumHandshakeHeaderBytes - lineBytes) {
                return WebResult::Failure(WebError::ResourceExhausted);
            }
            headers.append(header.Name.data(), header.Name.size());
            headers.append(": ");
            headers.append(header.Value.data(), header.Value.size());
            headers.append("\r\n");
        }
        return WebResult::Success();
    }

    WebResult SendBinary(Connection& connection, const uint8_t* data, std::size_t size) {
        if (data == nullptr || size == 0 || size > static_cast<std::size_t>(INT_MAX)) {
            return WebResult::Failure(WebError::InvalidConfiguration);
        }
        esp_websocket_client_handle_t client = nullptr;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_connected || !_connection || _connection.get() != &connection ||
                _client == nullptr) {
                return WebResult::Failure(WebError::Closed);
            }
            client = _client;
        }
        const int sent = esp_websocket_client_send_bin(
            client,
            reinterpret_cast<const char*>(data),
            static_cast<int>(size),
            _sendTimeoutTicks
        );
        return sent == static_cast<int>(size)
            ? WebResult::Success()
            : WebResult::Failure(WebError::ConnectionFailure, sent);
    }

    WebResult SendText(Connection& connection, std::string_view text) {
        if (text.empty() || text.size() > static_cast<std::size_t>(INT_MAX)) {
            return text.empty()
                ? WebResult::Failure(WebError::Unsupported)
                : WebResult::Failure(WebError::InvalidConfiguration);
        }
        esp_websocket_client_handle_t client = nullptr;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_connected || !_connection || _connection.get() != &connection ||
                _client == nullptr) {
                return WebResult::Failure(WebError::Closed);
            }
            client = _client;
        }
        const int sent = esp_websocket_client_send_text(
            client,
            text.data(),
            static_cast<int>(text.size()),
            _sendTimeoutTicks
        );
        return sent == static_cast<int>(text.size())
            ? WebResult::Success()
            : WebResult::Failure(WebError::ConnectionFailure, sent);
    }

    WebResult CloseConnection(Connection& connection, const WebSocketCloseReason& reason) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_connection || _connection.get() != &connection) {
                return WebResult::Failure(WebError::Closed);
            }
        }
        return Disconnect(reason);
    }

    static void NativeEventHandler(
        void* argument,
        esp_event_base_t,
        int32_t eventId,
        void* eventData
    ) {
        auto* self = static_cast<ESP32WebSocketClientPlatform*>(argument);
        if (self == nullptr) return;
        self->HandleNativeEvent(
            static_cast<esp_websocket_event_id_t>(eventId),
            static_cast<esp_websocket_event_data_t*>(eventData)
        );
    }

    void HandleNativeEvent(
        esp_websocket_event_id_t eventId,
        esp_websocket_event_data_t* eventData
    ) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_eventTask == nullptr) _eventTask = xTaskGetCurrentTaskHandle();
        }

        switch (eventId) {
            case WEBSOCKET_EVENT_CONNECTED:
                HandleConnected();
                break;
            case WEBSOCKET_EVENT_DISCONNECTED:
                HandleDisconnected(false);
                break;
            case WEBSOCKET_EVENT_CLOSED:
                HandleDisconnected(true);
                break;
            case WEBSOCKET_EVENT_DATA:
                if (eventData != nullptr) HandleData(*eventData);
                break;
            case WEBSOCKET_EVENT_ERROR:
            default:
                break;
        }
    }

    void HandleConnected() {
        IWebSocketClientPlatformSink* sink = nullptr;
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_client == nullptr || !_connection) return;
            _connected = true;
            _disconnectNotified = false;
            _connection->SetOpen(true);
            _peerCloseCode = 1000;
            _peerCloseReason.clear();
            connection = _connection;
            sink = _sink;
        }
        if (sink != nullptr) sink->OnPlatformWebSocketClientConnected(*connection);
    }

    void HandleDisconnected(bool clean) {
        IWebSocketClientPlatformSink* sink = nullptr;
        WorkingString reason;
        uint16_t code = clean ? 1000 : 1006;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _connected = false;
            if (_connection) _connection->SetOpen(false);
            if (_disconnectNotified) return;
            _disconnectNotified = true;
            sink = _sink;

            if (clean) {
                if (!_peerCloseReason.empty() || _peerCloseCode != 1000) {
                    code = _peerCloseCode;
                    reason = _peerCloseReason;
                } else {
                    code = _requestedCloseCode;
                    reason = _requestedCloseReason;
                }
            } else {
                reason = "connection lost";
            }
        }
        if (sink != nullptr) {
            sink->OnPlatformWebSocketClientDisconnected({
                code,
                std::string_view(reason.data(), reason.size())
            });
        }
    }

    void HandleData(const esp_websocket_event_data_t& event) {
        if (event.payload_len < 0 || event.payload_offset < 0 || event.data_len < 0) return;
        const auto payloadLength = static_cast<std::size_t>(event.payload_len);
        const auto offset = static_cast<std::size_t>(event.payload_offset);
        const auto dataLength = static_cast<std::size_t>(event.data_len);
        if (payloadLength > ESPRESSIO_ESP32_WEBSOCKET_CLIENT_MAX_MESSAGE_BYTES ||
            offset > payloadLength || dataLength > payloadLength - offset) {
            return;
        }

        const uint8_t opcode = event.op_code;
        if (opcode == OpcodePing || opcode == OpcodePong) return;

        if (opcode == OpcodeContinuation) {
            // IDF 4.4 strips the FIN bit from its public DATA event and exposes
            // no equivalent accessor. Multi-frame RFC6455 fragmentation cannot
            // therefore be reconstructed faithfully at this abstraction layer.
            // Drop continuation frames rather than deliver corrupted messages.
            return;
        }

        if (offset == 0 && dataLength == payloadLength) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(event.data_ptr);
            if (opcode == OpcodeClose) {
                RememberPeerClose(bytes, dataLength);
                return;
            }
            if (opcode == OpcodeText || opcode == OpcodeBinary) {
                DispatchPayload(opcode, bytes, dataLength);
            }
            return;
        }

        ByteBuffer completed;
        uint8_t completedOpcode = 0;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (offset == 0) {
                _incomingFrame.clear();
                _incomingFrame.resize(payloadLength);
                _incomingOpcode = opcode;
            }
            if (_incomingFrame.size() != payloadLength || _incomingOpcode != opcode ||
                (dataLength != 0 && event.data_ptr == nullptr)) {
                _incomingFrame.clear();
                _incomingOpcode = 0;
                return;
            }
            if (dataLength != 0) {
                std::memcpy(
                    _incomingFrame.data() + offset,
                    event.data_ptr,
                    dataLength
                );
            }
            if (offset + dataLength == payloadLength) {
                completedOpcode = _incomingOpcode;
                completed = std::move(_incomingFrame);
                _incomingFrame.clear();
                _incomingOpcode = 0;
            }
        }

        if (completedOpcode == 0) return;
        if (completedOpcode == OpcodeClose) {
            RememberPeerClose(completed.data(), completed.size());
        } else if (completedOpcode == OpcodeText || completedOpcode == OpcodeBinary) {
            DispatchPayload(completedOpcode, completed.data(), completed.size());
        }
    }

    void DispatchPayload(uint8_t opcode, const uint8_t* data, std::size_t size) {
        IWebSocketClientPlatformSink* sink = nullptr;
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_connected || !_connection) return;
            sink = _sink;
            connection = _connection;
        }
        if (sink == nullptr) return;

        if (opcode == OpcodeText) {
            sink->OnPlatformWebSocketClientText(
                *connection,
                size == 0
                    ? std::string_view{}
                    : std::string_view(reinterpret_cast<const char*>(data), size)
            );
        } else {
            sink->OnPlatformWebSocketClientBinary(*connection, data, size);
        }
    }

    void RememberPeerClose(const uint8_t* payload, std::size_t size) {
        std::lock_guard<std::mutex> lock(_mutex);
        _peerCloseCode = size >= 2 && payload != nullptr
            ? Detail::ESP32ReadNetworkUInt16(payload)
            : 1000;
        _peerCloseReason.clear();
        if (size > 2 && payload != nullptr) {
            _peerCloseReason.assign(
                reinterpret_cast<const char*>(payload + 2),
                reinterpret_cast<const char*>(payload + size)
            );
        }
    }

    inline static std::atomic<WebSocketConnectionId> _nextConnectionId{1};

    mutable std::mutex _mutex;
    esp_websocket_client_handle_t _client = nullptr;
    IWebSocketClientPlatformSink* _sink = nullptr;
    std::shared_ptr<Connection> _connection;
    bool _connected = false;
    bool _disconnecting = false;
    bool _disconnectNotified = true;
    TaskHandle_t _eventTask = nullptr;
    TickType_t _sendTimeoutTicks = pdMS_TO_TICKS(NativeNetworkTimeoutMilliseconds);

    NativeCredential _serverCa;
    NativeCredential _clientCertificate;
    NativeCredential _clientPrivateKey;

    ByteBuffer _incomingFrame;
    uint8_t _incomingOpcode = 0;

    uint16_t _requestedCloseCode = 1000;
    WorkingString _requestedCloseReason;
    uint16_t _peerCloseCode = 1000;
    WorkingString _peerCloseReason;
};

} // namespace ESPressio::Web

#endif // ARDUINO_ARCH_ESP32 && ESPressio-Web && esp_websocket_client
