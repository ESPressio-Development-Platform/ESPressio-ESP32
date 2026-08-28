#pragma once

#ifdef ARDUINO_ARCH_ESP32

#if !__has_include(<ESPressio_HttpServer.hpp>)
#error "ESPressio ESP32 HTTP provider requires ESPressio-Web."
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

#include <esp_err.h>
#include <esp_http_server.h>

#include <ESPressio_HttpServer.hpp>
#include <ESPressio_Memory.hpp>

#ifndef ESPRESSIO_ESP32_WEB_MAX_HEADER_NAME_LENGTH
#define ESPRESSIO_ESP32_WEB_MAX_HEADER_NAME_LENGTH 128
#endif

namespace ESPressio::Web {
namespace Detail {

inline WebResult ESP32HttpResult(esp_err_t result) noexcept {
    if (result == ESP_OK) return WebResult::Success();
    switch (result) {
        case ESP_ERR_INVALID_ARG:
            return WebResult::Failure(WebError::InvalidConfiguration, result);
        case ESP_ERR_NO_MEM:
        case ESP_ERR_HTTPD_ALLOC_MEM:
        case ESP_ERR_HTTPD_HANDLERS_FULL:
            return WebResult::Failure(WebError::ResourceExhausted, result);
        case ESP_ERR_NOT_FOUND:
            return WebResult::Failure(WebError::NotFound, result);
        case ESP_ERR_HTTPD_INVALID_REQ:
            return WebResult::Failure(WebError::Closed, result);
        case ESP_ERR_HTTPD_RESP_SEND:
            return WebResult::Failure(WebError::ConnectionFailure, result);
        case ESP_ERR_HTTPD_RESP_HDR:
        case ESP_ERR_HTTPD_RESULT_TRUNC:
            return WebResult::Failure(WebError::ProtocolError, result);
        default:
            return WebResult::Failure(WebError::PlatformFailure, result);
    }
}

inline HttpMethod FromESP32HttpMethod(httpd_method_t method) noexcept {
    switch (method) {
        case HTTP_GET: return HttpMethod::Get;
        case HTTP_HEAD: return HttpMethod::Head;
        case HTTP_POST: return HttpMethod::Post;
        case HTTP_PUT: return HttpMethod::Put;
        case HTTP_PATCH: return HttpMethod::Patch;
        case HTTP_DELETE: return HttpMethod::Delete;
        case HTTP_OPTIONS: return HttpMethod::Options;
        case HTTP_CONNECT: return HttpMethod::Connect;
        case HTTP_TRACE: return HttpMethod::Trace;
        default: return HttpMethod::Any;
    }
}

inline const char* ESP32HttpStatusText(HttpStatus status) noexcept {
    switch (status) {
        case HttpStatus::Continue: return "100 Continue";
        case HttpStatus::SwitchingProtocols: return "101 Switching Protocols";
        case HttpStatus::Ok: return "200 OK";
        case HttpStatus::Created: return "201 Created";
        case HttpStatus::Accepted: return "202 Accepted";
        case HttpStatus::NoContent: return "204 No Content";
        case HttpStatus::MovedPermanently: return "301 Moved Permanently";
        case HttpStatus::Found: return "302 Found";
        case HttpStatus::NotModified: return "304 Not Modified";
        case HttpStatus::BadRequest: return "400 Bad Request";
        case HttpStatus::Unauthorized: return "401 Unauthorized";
        case HttpStatus::Forbidden: return "403 Forbidden";
        case HttpStatus::NotFound: return "404 Not Found";
        case HttpStatus::MethodNotAllowed: return "405 Method Not Allowed";
        case HttpStatus::RequestTimeout: return "408 Request Timeout";
        case HttpStatus::Conflict: return "409 Conflict";
        case HttpStatus::LengthRequired: return "411 Length Required";
        case HttpStatus::PayloadTooLarge: return "413 Payload Too Large";
        case HttpStatus::UnsupportedMediaType: return "415 Unsupported Media Type";
        case HttpStatus::TooManyRequests: return "429 Too Many Requests";
        case HttpStatus::InternalServerError: return "500 Internal Server Error";
        case HttpStatus::NotImplemented: return "501 Not Implemented";
        case HttpStatus::BadGateway: return "502 Bad Gateway";
        case HttpStatus::ServiceUnavailable: return "503 Service Unavailable";
        case HttpStatus::GatewayTimeout: return "504 Gateway Timeout";
        default: return "500 Internal Server Error";
    }
}

inline bool HeaderNameEquals(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto a = static_cast<unsigned char>(left[i]);
        const auto b = static_cast<unsigned char>(right[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

inline bool IsFramingHeader(std::string_view name) noexcept {
    return HeaderNameEquals(name, "Content-Length") ||
           HeaderNameEquals(name, "Transfer-Encoding") ||
           HeaderNameEquals(name, "Connection");
}

inline bool CopyHeaderName(
    std::string_view name,
    std::array<char, ESPRESSIO_ESP32_WEB_MAX_HEADER_NAME_LENGTH + 1>& destination
) noexcept {
    if (name.empty() || name.size() > ESPRESSIO_ESP32_WEB_MAX_HEADER_NAME_LENGTH) {
        return false;
    }
    std::memcpy(destination.data(), name.data(), name.size());
    destination[name.size()] = '\0';
    return true;
}

} // namespace Detail

class ESP32HttpRequestPlatform final : public IHttpRequestPlatform {
public:
    explicit ESP32HttpRequestPlatform(httpd_req_t& request) noexcept
        : _request(request) {
        const std::string_view target(request.uri == nullptr ? "" : request.uri);
        const auto query = target.find('?');
        _path = query == std::string_view::npos ? target : target.substr(0, query);
        _query = query == std::string_view::npos
            ? std::string_view{}
            : target.substr(query + 1);
    }

    HttpMethod Method() const noexcept override {
        return Detail::FromESP32HttpMethod(
            static_cast<httpd_method_t>(_request.method)
        );
    }

    std::string_view Path() const noexcept override { return _path; }
    std::string_view QueryString() const noexcept override { return _query; }

    std::optional<std::size_t> ContentLength() const noexcept override {
        return _request.content_len == 0
            ? std::nullopt
            : std::optional<std::size_t>(_request.content_len);
    }

    bool HasHeader(std::string_view name) const noexcept override {
        std::array<char, ESPRESSIO_ESP32_WEB_MAX_HEADER_NAME_LENGTH + 1> field{};
        if (!Detail::CopyHeaderName(name, field)) return false;

        if (httpd_req_get_hdr_value_len(
                const_cast<httpd_req_t*>(&_request),
                field.data()) != 0) {
            return true;
        }

        char value[1]{};
        return httpd_req_get_hdr_value_str(
            const_cast<httpd_req_t*>(&_request),
            field.data(),
            value,
            sizeof(value)
        ) == ESP_OK;
    }

    std::size_t HeaderValueLength(std::string_view name) const noexcept override {
        std::array<char, ESPRESSIO_ESP32_WEB_MAX_HEADER_NAME_LENGTH + 1> field{};
        if (!Detail::CopyHeaderName(name, field)) return 0;
        return httpd_req_get_hdr_value_len(
            const_cast<httpd_req_t*>(&_request),
            field.data()
        );
    }

    WebResult ReadHeader(
        std::string_view name,
        char* destination,
        std::size_t capacity,
        std::size_t& bytesWritten
    ) const override {
        bytesWritten = 0;
        if (destination == nullptr || capacity == 0) {
            return WebResult::Failure(WebError::InvalidConfiguration);
        }

        std::array<char, ESPRESSIO_ESP32_WEB_MAX_HEADER_NAME_LENGTH + 1> field{};
        if (!Detail::CopyHeaderName(name, field)) {
            return WebResult::Failure(WebError::InvalidConfiguration);
        }

        const auto length = httpd_req_get_hdr_value_len(
            const_cast<httpd_req_t*>(&_request),
            field.data()
        );
        if (length > capacity) {
            return WebResult::Failure(WebError::ResourceExhausted);
        }

        System::Memory::Vector<
            char,
            System::Memory::MemoryPolicy::ExternalPreferred
        > scratch(length + 1);
        const auto result = httpd_req_get_hdr_value_str(
            const_cast<httpd_req_t*>(&_request),
            field.data(),
            scratch.data(),
            scratch.size()
        );
        if (result != ESP_OK) return Detail::ESP32HttpResult(result);

        if (length != 0) std::memcpy(destination, scratch.data(), length);
        bytesWritten = length;
        return WebResult::Success();
    }

    HttpReadResult ReadBody(uint8_t* destination, std::size_t capacity) override {
        if (destination == nullptr || capacity == 0) {
            return {WebResult::Failure(WebError::InvalidConfiguration), 0, false};
        }
        if (_bodyRead >= _request.content_len) {
            return {WebResult::Success(), 0, true};
        }

        const auto remaining = _request.content_len - _bodyRead;
        const auto requested = std::min<std::size_t>(capacity, remaining);
        const int received = httpd_req_recv(
            &_request,
            reinterpret_cast<char*>(destination),
            requested
        );
        if (received > 0) {
            _bodyRead += static_cast<std::size_t>(received);
            return {
                WebResult::Success(),
                static_cast<std::size_t>(received),
                _bodyRead >= _request.content_len
            };
        }
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            return {WebResult::Failure(WebError::ConnectionFailure, received), 0, false};
        }
        if (received == 0) {
            return {WebResult::Failure(WebError::Closed), 0, false};
        }
        return {WebResult::Failure(WebError::ConnectionFailure, received), 0, false};
    }

private:
    httpd_req_t& _request;
    std::string_view _path;
    std::string_view _query;
    std::size_t _bodyRead = 0;
};

class ESP32HttpResponsePlatform final : public IHttpResponsePlatform {
private:
    struct Header final {
        System::Memory::String<System::Memory::MemoryPolicy::ExternalPreferred> Name;
        System::Memory::String<System::Memory::MemoryPolicy::ExternalPreferred> Value;
    };
    using HeaderList = System::Memory::Vector<
        Header,
        System::Memory::MemoryPolicy::ExternalPreferred
    >;
    using WorkingString = System::Memory::String<
        System::Memory::MemoryPolicy::ExternalPreferred
    >;

public:
    ESP32HttpResponsePlatform(httpd_req_t& request, bool keepAlive)
        : _request(request), _keepAlive(keepAlive) {}

    WebResult SetStatus(HttpStatus status) override {
        if (_begun) return WebResult::Failure(WebError::InvalidState);
        _status = status;
        return WebResult::Success();
    }

    WebResult SetHeader(std::string_view name, std::string_view value) override {
        if (_begun || name.empty()) {
            return WebResult::Failure(WebError::InvalidState);
        }
        if (Detail::IsFramingHeader(name)) {
            return WebResult::Failure(WebError::InvalidConfiguration);
        }
        if (Detail::HeaderNameEquals(name, "Content-Type")) {
            _contentType.assign(value.begin(), value.end());
            return WebResult::Success();
        }
        for (auto& header : _headers) {
            if (Detail::HeaderNameEquals(
                    std::string_view(header.Name.data(), header.Name.size()), name)) {
                header.Value.assign(value.begin(), value.end());
                return WebResult::Success();
            }
        }
        Header header;
        header.Name.assign(name.begin(), name.end());
        header.Value.assign(value.begin(), value.end());
        _headers.push_back(std::move(header));
        return WebResult::Success();
    }

    WebResult Begin(std::optional<std::size_t> contentLength) override {
        if (_begun) return WebResult::Failure(WebError::InvalidState);
        _expectedLength = contentLength;
        _knownLengthFraming = contentLength.has_value();

        if (_knownLengthFraming) {
            const auto result = SendKnownLengthHeaders(*contentLength);
            if (!result) return result;
            _begun = true;
            return WebResult::Success();
        }

        auto result = httpd_resp_set_status(
            &_request,
            Detail::ESP32HttpStatusText(_status)
        );
        if (result != ESP_OK) return Detail::ESP32HttpResult(result);

        if (!_contentType.empty()) {
            result = httpd_resp_set_type(&_request, _contentType.c_str());
            if (result != ESP_OK) return Detail::ESP32HttpResult(result);
        }
        for (const auto& header : _headers) {
            result = httpd_resp_set_hdr(
                &_request,
                header.Name.c_str(),
                header.Value.c_str()
            );
            if (result != ESP_OK) return Detail::ESP32HttpResult(result);
        }
        if (!_keepAlive) {
            result = httpd_resp_set_hdr(&_request, "Connection", "close");
            if (result != ESP_OK) return Detail::ESP32HttpResult(result);
        }
        _begun = true;
        return WebResult::Success();
    }

    WebResult Write(const uint8_t* data, std::size_t size) override {
        if (!_begun || _completed || data == nullptr || size == 0) {
            return WebResult::Failure(WebError::InvalidState);
        }
        if (_expectedLength.has_value() &&
            _bytesWritten + size > *_expectedLength) {
            return WebResult::Failure(WebError::ProtocolError);
        }

        if (static_cast<httpd_method_t>(_request.method) == HTTP_HEAD) {
            _bytesWritten += size;
            return WebResult::Success();
        }

        WebResult result;
        if (_knownLengthFraming) {
            result = SendRaw(
                reinterpret_cast<const char*>(data),
                size
            );
        } else {
            const auto native = httpd_resp_send_chunk(
                &_request,
                reinterpret_cast<const char*>(data),
                size
            );
            result = native == ESP_OK
                ? WebResult::Success()
                : Detail::ESP32HttpResult(native);
        }
        if (!result) return result;
        _bytesWritten += size;
        return WebResult::Success();
    }

    WebResult Complete() override {
        if (!_begun || _completed) {
            return _completed
                ? WebResult::Success()
                : WebResult::Failure(WebError::InvalidState);
        }
        if (_expectedLength.has_value() &&
            _bytesWritten != *_expectedLength) {
            return WebResult::Failure(WebError::ProtocolError);
        }

        if (!_knownLengthFraming) {
            const auto result = httpd_resp_send_chunk(&_request, nullptr, 0);
            if (result != ESP_OK) return Detail::ESP32HttpResult(result);
        }
        _completed = true;
        return WebResult::Success();
    }

    void Abort() noexcept override { _completed = true; }

private:
    WebResult SendRaw(const char* data, std::size_t size) {
        if (data == nullptr && size != 0) {
            return WebResult::Failure(WebError::InvalidConfiguration);
        }
        while (size != 0) {
            const int sent = httpd_send(&_request, data, size);
            if (sent <= 0) {
                return WebResult::Failure(WebError::ConnectionFailure, sent);
            }
            data += sent;
            size -= static_cast<std::size_t>(sent);
        }
        return WebResult::Success();
    }

    WebResult SendKnownLengthHeaders(std::size_t contentLength) {
        WorkingString headers;
        headers.reserve(128 + _contentType.size() + (_headers.size() * 32));
        headers.append("HTTP/1.1 ");
        headers.append(Detail::ESP32HttpStatusText(_status));
        headers.append("\r\nContent-Length: ");

        char lengthText[32]{};
        const int lengthCharacters = std::snprintf(
            lengthText,
            sizeof(lengthText),
            "%llu",
            static_cast<unsigned long long>(contentLength)
        );
        if (lengthCharacters <= 0 ||
            static_cast<std::size_t>(lengthCharacters) >= sizeof(lengthText)) {
            return WebResult::Failure(WebError::PlatformFailure);
        }
        headers.append(lengthText, static_cast<std::size_t>(lengthCharacters));
        headers.append("\r\n");

        if (!_contentType.empty()) {
            headers.append("Content-Type: ");
            headers.append(_contentType);
            headers.append("\r\n");
        }
        for (const auto& header : _headers) {
            headers.append(header.Name);
            headers.append(": ");
            headers.append(header.Value);
            headers.append("\r\n");
        }
        if (!_keepAlive) {
            headers.append("Connection: close\r\n");
        }
        headers.append("\r\n");
        return SendRaw(headers.data(), headers.size());
    }

    httpd_req_t& _request;
    bool _keepAlive = true;
    bool _begun = false;
    bool _completed = false;
    bool _knownLengthFraming = false;
    HttpStatus _status = HttpStatus::Ok;
    std::optional<std::size_t> _expectedLength;
    std::size_t _bytesWritten = 0;
    WorkingString _contentType;
    HeaderList _headers;
};

#ifdef CONFIG_HTTPD_WS_SUPPORT
class IESP32HttpWebSocketBinding {
public:
    virtual ~IESP32HttpWebSocketBinding() = default;
    virtual bool IsActive() const noexcept = 0;
    virtual const char* PathCString() const noexcept = 0;
    virtual const char* ProtocolCString() const noexcept = 0;
    virtual esp_err_t HandleWebSocketRequest(httpd_req_t& request) = 0;
    virtual void OnHttpSessionClosed(int socket) = 0;
};

using ESP32HttpWebSocketBindingPtr = std::shared_ptr<IESP32HttpWebSocketBinding>;
#endif

class ESP32HttpServerPlatform final : public IHttpServerPlatform {
public:
    ESP32HttpServerPlatform() = default;
    ~ESP32HttpServerPlatform() override { Reset(); }

    ESP32HttpServerPlatform(const ESP32HttpServerPlatform&) = delete;
    ESP32HttpServerPlatform& operator=(const ESP32HttpServerPlatform&) = delete;

    WebCapabilities Capabilities() const noexcept override {
        WebCapabilities capabilities =
            ToCapabilities(WebCapability::Http) |
            ToCapabilities(WebCapability::ChunkedResponses) |
            ToCapabilities(WebCapability::PersistentConnections);
#ifdef CONFIG_HTTPD_WS_SUPPORT
        capabilities |= ToCapabilities(WebCapability::WebSocketServer);
#endif
        return capabilities;
    }

    WebResult Initialize(
        const HttpServerConfiguration& configuration,
        IHttpRequestDispatcher& dispatcher
    ) override {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_server != nullptr) return WebResult::Failure(WebError::InvalidState);
        if (configuration.TransportMode != HttpTransportMode::Plain) {
            return WebResult::Failure(WebError::Unsupported);
        }
        _configuration = configuration;
        _dispatcher = &dispatcher;
        return WebResult::Success();
    }

#ifdef CONFIG_HTTPD_WS_SUPPORT
    WebResult AddWebSocketBinding(const ESP32HttpWebSocketBindingPtr& binding) {
        if (!binding || !binding->IsActive() || binding->PathCString() == nullptr ||
            binding->PathCString()[0] != '/') {
            return WebResult::Failure(WebError::InvalidConfiguration);
        }

        std::lock_guard<std::mutex> lock(_mutex);
        if (_server != nullptr) return WebResult::Failure(WebError::InvalidState);

        for (const auto& existing : _webSocketBindings) {
            if (!existing || !existing->IsActive()) continue;
            if (existing.get() == binding.get()) return WebResult::Success();
            if (std::strcmp(existing->PathCString(), binding->PathCString()) == 0) {
                return WebResult::Failure(WebError::InvalidConfiguration);
            }
        }
        _webSocketBindings.push_back(binding);
        return WebResult::Success();
    }

    void ReleaseWebSocketBinding(const ESP32HttpWebSocketBindingPtr& binding) noexcept {
        if (!binding) return;
        std::lock_guard<std::mutex> lock(_mutex);
        if (_server != nullptr) return;
        _webSocketBindings.erase(
            std::remove_if(
                _webSocketBindings.begin(),
                _webSocketBindings.end(),
                [&](const ESP32HttpWebSocketBindingPtr& candidate) {
                    return !candidate || candidate.get() == binding.get() || !candidate->IsActive();
                }
            ),
            _webSocketBindings.end()
        );
    }
#endif

    WebResult Start() override {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_server != nullptr) return WebResult::Failure(WebError::AlreadyRunning);
        if (_dispatcher == nullptr) return WebResult::Failure(WebError::InvalidState);

#ifdef CONFIG_HTTPD_WS_SUPPORT
        _webSocketBindings.erase(
            std::remove_if(
                _webSocketBindings.begin(),
                _webSocketBindings.end(),
                [](const ESP32HttpWebSocketBindingPtr& binding) {
                    return !binding || !binding->IsActive();
                }
            ),
            _webSocketBindings.end()
        );
#endif

        httpd_config_t native = HTTPD_DEFAULT_CONFIG();
        native.server_port = _configuration.Port;
        native.max_open_sockets = static_cast<unsigned>(std::min<std::size_t>(
            _configuration.MaximumConnections + 3,
            0xffffu
        ));
        native.max_uri_handlers = static_cast<uint16_t>(
            SupportedMethodCount
#ifdef CONFIG_HTTPD_WS_SUPPORT
            + _webSocketBindings.size()
#endif
        );
        native.uri_match_fn = httpd_uri_match_wildcard;
        native.lru_purge_enable = true;
#ifdef CONFIG_HTTPD_WS_SUPPORT
        native.global_user_ctx = this;
        native.global_user_ctx_free_fn = &IgnoreGlobalContextFree;
        native.close_fn = &HandleSessionClosed;
#endif

        const auto started = httpd_start(&_server, &native);
        if (started != ESP_OK) {
            _server = nullptr;
            return Detail::ESP32HttpResult(started);
        }

#ifdef CONFIG_HTTPD_WS_SUPPORT
        for (const auto& binding : _webSocketBindings) {
            httpd_uri_t handler{};
            handler.uri = binding->PathCString();
            handler.method = HTTP_GET;
            handler.handler = &DispatchWebSocketRequest;
            handler.user_ctx = binding.get();
            handler.is_websocket = true;
            handler.handle_ws_control_frames = true;
            const char* protocol = binding->ProtocolCString();
            handler.supported_subprotocol =
                protocol != nullptr && protocol[0] != '\0' ? protocol : nullptr;

            const auto registered = httpd_register_uri_handler(_server, &handler);
            if (registered != ESP_OK) {
                (void)httpd_stop(_server);
                _server = nullptr;
                return Detail::ESP32HttpResult(registered);
            }
        }
#endif

        for (const auto method : SupportedMethods) {
            httpd_uri_t handler{};
            handler.uri = "/*";
            handler.method = method;
            handler.handler = &DispatchRequest;
            handler.user_ctx = this;
            const auto registered = httpd_register_uri_handler(_server, &handler);
            if (registered != ESP_OK) {
                (void)httpd_stop(_server);
                _server = nullptr;
                return Detail::ESP32HttpResult(registered);
            }
        }
        return WebResult::Success();
    }

    WebResult Stop() override {
        httpd_handle_t server = nullptr;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_server == nullptr) return WebResult::Success();
            server = _server;
            _server = nullptr;
        }
        const auto stopped = httpd_stop(server);
#ifdef CONFIG_HTTPD_WS_SUPPORT
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _webSocketBindings.erase(
                std::remove_if(
                    _webSocketBindings.begin(),
                    _webSocketBindings.end(),
                    [](const ESP32HttpWebSocketBindingPtr& binding) {
                        return !binding || !binding->IsActive();
                    }
                ),
                _webSocketBindings.end()
            );
        }
#endif
        return Detail::ESP32HttpResult(stopped);
    }

    void Reset() noexcept override {
        (void)Stop();
        std::lock_guard<std::mutex> lock(_mutex);
        _dispatcher = nullptr;
        _configuration = {};
#ifdef CONFIG_HTTPD_WS_SUPPORT
        _webSocketBindings.erase(
            std::remove_if(
                _webSocketBindings.begin(),
                _webSocketBindings.end(),
                [](const ESP32HttpWebSocketBindingPtr& binding) {
                    return !binding || !binding->IsActive();
                }
            ),
            _webSocketBindings.end()
        );
#endif
    }

private:
    inline static constexpr std::array<httpd_method_t, 9> SupportedMethods = {
        HTTP_GET,
        HTTP_HEAD,
        HTTP_POST,
        HTTP_PUT,
        HTTP_PATCH,
        HTTP_DELETE,
        HTTP_OPTIONS,
        HTTP_CONNECT,
        HTTP_TRACE
    };
    inline static constexpr std::size_t SupportedMethodCount = SupportedMethods.size();

    static esp_err_t DispatchRequest(httpd_req_t* request) {
        if (request == nullptr || request->user_ctx == nullptr) return ESP_FAIL;
        return static_cast<ESP32HttpServerPlatform*>(request->user_ctx)
            ->HandleRequest(*request);
    }

#ifdef CONFIG_HTTPD_WS_SUPPORT
    static esp_err_t DispatchWebSocketRequest(httpd_req_t* request) {
        if (request == nullptr || request->user_ctx == nullptr) return ESP_FAIL;
        auto* binding = static_cast<IESP32HttpWebSocketBinding*>(request->user_ctx);
        return binding->IsActive()
            ? binding->HandleWebSocketRequest(*request)
            : ESP_FAIL;
    }

    static void IgnoreGlobalContextFree(void*) {}

    static void HandleSessionClosed(httpd_handle_t handle, int socket) {
        auto* server = static_cast<ESP32HttpServerPlatform*>(
            httpd_get_global_user_ctx(handle)
        );
        if (server != nullptr) server->NotifySessionClosed(socket);
        close(socket);
    }

    void NotifySessionClosed(int socket) {
        System::Memory::Vector<
            ESP32HttpWebSocketBindingPtr,
            System::Memory::MemoryPolicy::ExternalPreferred
        > bindings;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            bindings = _webSocketBindings;
        }
        for (const auto& binding : bindings) {
            if (binding) binding->OnHttpSessionClosed(socket);
        }
    }
#endif

    esp_err_t HandleRequest(httpd_req_t& request) {
        IHttpRequestDispatcher* dispatcher = nullptr;
        bool keepAlive = true;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            dispatcher = _dispatcher;
            keepAlive = _configuration.KeepAlive;
        }
        if (dispatcher == nullptr) return ESP_FAIL;

        ESP32HttpRequestPlatform requestPlatform(request);
        ESP32HttpResponsePlatform responsePlatform(request, keepAlive);
        return dispatcher->Dispatch(requestPlatform, responsePlatform)
            ? ESP_OK
            : ESP_FAIL;
    }

    mutable std::mutex _mutex;
    httpd_handle_t _server = nullptr;
    IHttpRequestDispatcher* _dispatcher = nullptr;
    HttpServerConfiguration _configuration{};
#ifdef CONFIG_HTTPD_WS_SUPPORT
    System::Memory::Vector<
        ESP32HttpWebSocketBindingPtr,
        System::Memory::MemoryPolicy::ExternalPreferred
    > _webSocketBindings;
#endif
};

} // namespace ESPressio::Web

#endif // ARDUINO_ARCH_ESP32
