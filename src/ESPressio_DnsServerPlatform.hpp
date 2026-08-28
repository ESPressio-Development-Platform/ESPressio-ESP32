#pragma once

#ifdef ARDUINO_ARCH_ESP32

#if !__has_include(<ESPressio_Dns.hpp>)
#error "ESPressio ESP32 DNS provider requires ESPressio-Web."
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>

#include <AsyncUDP.h>
#include <ESPressio_Dns.hpp>

#ifndef ESPRESSIO_ESP32_DNS_MAX_PACKET_BYTES
#define ESPRESSIO_ESP32_DNS_MAX_PACKET_BYTES 512
#endif

namespace ESPressio::Web {
namespace Detail {

inline uint16_t ReadDns16(const uint8_t* data) noexcept {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8) |
        static_cast<uint16_t>(data[1])
    );
}

inline uint32_t ReadDns32(const uint8_t* data) noexcept {
    return
        (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) |
        static_cast<uint32_t>(data[3]);
}

inline void WriteDns16(uint8_t* data, uint16_t value) noexcept {
    data[0] = static_cast<uint8_t>((value >> 8) & 0xffu);
    data[1] = static_cast<uint8_t>(value & 0xffu);
}

inline void WriteDns32(uint8_t* data, uint32_t value) noexcept {
    data[0] = static_cast<uint8_t>((value >> 24) & 0xffu);
    data[1] = static_cast<uint8_t>((value >> 16) & 0xffu);
    data[2] = static_cast<uint8_t>((value >> 8) & 0xffu);
    data[3] = static_cast<uint8_t>(value & 0xffu);
}

inline bool DecodeDnsName(
    const uint8_t* packet,
    std::size_t packetSize,
    std::size_t start,
    std::array<char, 254>& output,
    std::size_t& outputLength,
    std::size_t& encodedEnd
) noexcept {
    outputLength = 0;
    encodedEnd = start;
    std::size_t cursor = start;
    std::size_t jumps = 0;
    bool jumped = false;

    while (cursor < packetSize) {
        const uint8_t length = packet[cursor];
        if (length == 0) {
            if (!jumped) encodedEnd = cursor + 1;
            return outputLength <= 253;
        }

        if ((length & 0xc0u) == 0xc0u) {
            if (cursor + 1 >= packetSize || ++jumps > 8) return false;
            const uint16_t pointer = static_cast<uint16_t>(
                ((static_cast<uint16_t>(length & 0x3fu)) << 8) |
                packet[cursor + 1]
            );
            if (pointer >= packetSize) return false;
            if (!jumped) encodedEnd = cursor + 2;
            jumped = true;
            cursor = pointer;
            continue;
        }

        if ((length & 0xc0u) != 0 || length > 63) return false;
        ++cursor;
        if (cursor + length > packetSize) return false;

        if (outputLength != 0) {
            if (outputLength >= output.size() - 1) return false;
            output[outputLength++] = '.';
        }
        if (outputLength + length > 253) return false;
        std::memcpy(output.data() + outputLength, packet + cursor, length);
        outputLength += length;
        cursor += length;
        if (!jumped) encodedEnd = cursor;
    }
    return false;
}

} // namespace Detail

class ESP32DnsRequestPlatform final : public IDnsRequestPlatform {
public:
    ESP32DnsRequestPlatform(
        std::string_view name,
        uint16_t type,
        uint16_t recordClass
    ) noexcept : _name(name),
        _type(static_cast<DnsRecordType>(type)),
        _class(static_cast<DnsRecordClass>(recordClass)) {}

    std::string_view Name() const noexcept override { return _name; }
    DnsRecordType Type() const noexcept override { return _type; }
    DnsRecordClass Class() const noexcept override { return _class; }

private:
    std::string_view _name;
    DnsRecordType _type;
    DnsRecordClass _class;
};

class ESP32DnsResponsePlatform final : public IDnsResponsePlatform {
public:
    ESP32DnsResponsePlatform(
        AsyncUDPPacket& packet,
        const uint8_t* request,
        std::size_t requestSize,
        std::size_t questionEnd
    ) noexcept : _packet(packet) {
        if (requestSize < 12 || questionEnd > requestSize || questionEnd > _buffer.size()) {
            _valid = false;
            return;
        }
        std::memcpy(_buffer.data(), request, questionEnd);
        _size = questionEnd;

        const uint16_t queryFlags = Detail::ReadDns16(request + 2);
        const uint16_t responseFlags = static_cast<uint16_t>(
            0x8000u |                 // QR: response
            0x0400u |                 // AA: authoritative
            (queryFlags & 0x7900u)    // opcode + recursion desired
        );
        Detail::WriteDns16(_buffer.data() + 2, responseFlags);
        Detail::WriteDns16(_buffer.data() + 4, 1);
        Detail::WriteDns16(_buffer.data() + 6, 0);
        Detail::WriteDns16(_buffer.data() + 8, 0);
        Detail::WriteDns16(_buffer.data() + 10, 0);
    }

    bool Valid() const noexcept { return _valid; }

    WebResult SetResponseCode(DnsResponseCode code) override {
        if (!_valid || _completed) return WebResult::Failure(WebError::InvalidState);
        uint16_t flags = Detail::ReadDns16(_buffer.data() + 2);
        flags = static_cast<uint16_t>(
            (flags & 0xfff0u) | (static_cast<uint8_t>(code) & 0x0fu)
        );
        Detail::WriteDns16(_buffer.data() + 2, flags);
        return WebResult::Success();
    }

    WebResult AddAddressAnswer(
        const DnsAddress& address,
        uint32_t ttlSeconds
    ) override {
        if (!_valid || _completed) return WebResult::Failure(WebError::InvalidState);

        const uint16_t type = address.Family == DnsAddressFamily::IPv4
            ? static_cast<uint16_t>(DnsRecordType::A)
            : static_cast<uint16_t>(DnsRecordType::Aaaa);
        const std::size_t addressLength = address.Family == DnsAddressFamily::IPv4 ? 4 : 16;
        const std::size_t required = 12 + addressLength;
        if (_size + required > _buffer.size()) {
            return WebResult::Failure(WebError::ResourceExhausted);
        }

        // NAME: compression pointer to the original QNAME at DNS offset 12.
        _buffer[_size++] = 0xc0;
        _buffer[_size++] = 0x0c;
        Detail::WriteDns16(_buffer.data() + _size, type);
        _size += 2;
        Detail::WriteDns16(
            _buffer.data() + _size,
            static_cast<uint16_t>(DnsRecordClass::Internet)
        );
        _size += 2;
        Detail::WriteDns32(_buffer.data() + _size, ttlSeconds);
        _size += 4;
        Detail::WriteDns16(
            _buffer.data() + _size,
            static_cast<uint16_t>(addressLength)
        );
        _size += 2;
        std::memcpy(_buffer.data() + _size, address.Bytes.data(), addressLength);
        _size += addressLength;

        ++_answerCount;
        Detail::WriteDns16(_buffer.data() + 6, _answerCount);
        return WebResult::Success();
    }

    WebResult Complete() override {
        if (!_valid || _completed) {
            return _completed
                ? WebResult::Success()
                : WebResult::Failure(WebError::InvalidState);
        }
        const auto written = _packet.write(_buffer.data(), _size);
        if (written != _size) {
            return WebResult::Failure(WebError::ConnectionFailure);
        }
        _completed = true;
        return WebResult::Success();
    }

    void Abort() noexcept override { _completed = true; }

private:
    AsyncUDPPacket& _packet;
    std::array<uint8_t, ESPRESSIO_ESP32_DNS_MAX_PACKET_BYTES> _buffer{};
    std::size_t _size = 0;
    uint16_t _answerCount = 0;
    bool _valid = true;
    bool _completed = false;
};

class ESP32DnsServerPlatform final : public IDnsServerPlatform {
public:
    ESP32DnsServerPlatform() = default;
    ~ESP32DnsServerPlatform() override { Reset(); }

    ESP32DnsServerPlatform(const ESP32DnsServerPlatform&) = delete;
    ESP32DnsServerPlatform& operator=(const ESP32DnsServerPlatform&) = delete;

    WebCapabilities Capabilities() const noexcept override {
        return ToCapabilities(WebCapability::Dns);
    }

    WebResult Initialize(
        const DnsServerConfiguration& configuration,
        IDnsRequestDispatcher& dispatcher
    ) override {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_listening) return WebResult::Failure(WebError::InvalidState);
        _configuration = configuration;
        _dispatcher = &dispatcher;
        return WebResult::Success();
    }

    WebResult Start() override {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_listening) return WebResult::Failure(WebError::AlreadyRunning);
        if (_dispatcher == nullptr) return WebResult::Failure(WebError::InvalidState);

        _udp.onPacket([this](AsyncUDPPacket& packet) { HandlePacket(packet); });
        if (!_udp.listen(_configuration.Port)) {
            return WebResult::Failure(WebError::ConnectionFailure, _udp.lastErr());
        }
        _listening = true;
        return WebResult::Success();
    }

    WebResult Stop() override {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_listening) return WebResult::Success();
        _listening = false;
        _udp.close();
        return WebResult::Success();
    }

    void Reset() noexcept override {
        (void)Stop();
        std::lock_guard<std::mutex> lock(_mutex);
        _dispatcher = nullptr;
        _configuration = {};
    }

private:
    static void SendFormatError(AsyncUDPPacket& packet) noexcept {
        if (packet.length() < 2) return;
        std::array<uint8_t, 12> response{};
        const auto* request = packet.data();
        response[0] = request[0];
        response[1] = request[1];
        Detail::WriteDns16(
            response.data() + 2,
            static_cast<uint16_t>(0x8000u | 0x0400u |
                static_cast<uint8_t>(DnsResponseCode::FormatError))
        );
        (void)packet.write(response.data(), response.size());
    }

    void HandlePacket(AsyncUDPPacket& packet) {
        IDnsRequestDispatcher* dispatcher = nullptr;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_listening) return;
            dispatcher = _dispatcher;
        }
        if (dispatcher == nullptr) return;

        const auto* bytes = packet.data();
        const std::size_t size = packet.length();
        if (bytes == nullptr || size < 12 || size > ESPRESSIO_ESP32_DNS_MAX_PACKET_BYTES) {
            SendFormatError(packet);
            return;
        }

        const uint16_t flags = Detail::ReadDns16(bytes + 2);
        const uint16_t questionCount = Detail::ReadDns16(bytes + 4);
        if ((flags & 0x8000u) != 0 || questionCount != 1) {
            SendFormatError(packet);
            return;
        }

        std::array<char, 254> name{};
        std::size_t nameLength = 0;
        std::size_t encodedNameEnd = 0;
        if (!Detail::DecodeDnsName(
                bytes,
                size,
                12,
                name,
                nameLength,
                encodedNameEnd) ||
            encodedNameEnd + 4 > size) {
            SendFormatError(packet);
            return;
        }

        const uint16_t type = Detail::ReadDns16(bytes + encodedNameEnd);
        const uint16_t recordClass = Detail::ReadDns16(bytes + encodedNameEnd + 2);
        const std::size_t questionEnd = encodedNameEnd + 4;

        ESP32DnsRequestPlatform request(
            std::string_view(name.data(), nameLength),
            type,
            recordClass
        );
        ESP32DnsResponsePlatform response(packet, bytes, size, questionEnd);
        if (!response.Valid()) {
            SendFormatError(packet);
            return;
        }
        (void)dispatcher->Dispatch(request, response);
    }

    mutable std::mutex _mutex;
    AsyncUDP _udp;
    DnsServerConfiguration _configuration{};
    IDnsRequestDispatcher* _dispatcher = nullptr;
    bool _listening = false;
};

} // namespace ESPressio::Web

#endif // ARDUINO_ARCH_ESP32
