#pragma once

#ifdef ARDUINO_ARCH_ESP32

#if !__has_include(<ESPressio_Dns.hpp>)
#error "ESPressio ESP32 DNS provider requires ESPressio-Web."
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

extern "C" {
#include <lwip/err.h>
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>
#include <lwip/priv/tcpip_priv.h>
#include <lwip/udp.h>
}

#include <ESPressio_Dns.hpp>
#include <ESPressio_Memory.hpp>

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

inline WebError DnsLwipError(err_t error) noexcept {
    return error == ERR_MEM ? WebError::ResourceExhausted : WebError::ConnectionFailure;
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
private:
    using Buffer = System::Memory::Vector<
        uint8_t,
        System::Memory::MemoryPolicy::ExternalPreferred
    >;

public:
    ESP32DnsResponsePlatform(
        udp_pcb* pcb,
        const ip_addr_t& remoteAddress,
        uint16_t remotePort,
        const uint8_t* request,
        std::size_t requestSize,
        std::size_t questionEnd
    ) : _pcb(pcb), _remoteAddress(remoteAddress), _remotePort(remotePort) {
        if (_pcb == nullptr || request == nullptr || requestSize < 12 ||
            questionEnd > requestSize || questionEnd > ESPRESSIO_ESP32_DNS_MAX_PACKET_BYTES) {
            _valid = false;
            return;
        }

        try {
            _buffer.resize(questionEnd);
        } catch (...) {
            _valid = false;
            return;
        }
        std::memcpy(_buffer.data(), request, questionEnd);

        const uint16_t queryFlags = Detail::ReadDns16(request + 2);
        const uint16_t responseFlags = static_cast<uint16_t>(
            0x8000u | 0x0400u | (queryFlags & 0x7900u)
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
        flags = static_cast<uint16_t>((flags & 0xfff0u) | (static_cast<uint8_t>(code) & 0x0fu));
        Detail::WriteDns16(_buffer.data() + 2, flags);
        return WebResult::Success();
    }

    WebResult AddAddressAnswer(const DnsAddress& address, uint32_t ttlSeconds) override {
        if (!_valid || _completed) return WebResult::Failure(WebError::InvalidState);

        const uint16_t type = address.Family == DnsAddressFamily::IPv4
            ? static_cast<uint16_t>(DnsRecordType::A)
            : static_cast<uint16_t>(DnsRecordType::Aaaa);
        const std::size_t addressLength = address.Family == DnsAddressFamily::IPv4 ? 4 : 16;
        const std::size_t required = 12 + addressLength;
        if (_buffer.size() + required > ESPRESSIO_ESP32_DNS_MAX_PACKET_BYTES) {
            return WebResult::Failure(WebError::ResourceExhausted);
        }

        const std::size_t start = _buffer.size();
        try {
            _buffer.resize(start + required);
        } catch (...) {
            return WebResult::Failure(WebError::ResourceExhausted);
        }

        auto* output = _buffer.data() + start;
        std::size_t cursor = 0;
        output[cursor++] = 0xc0;
        output[cursor++] = 0x0c;
        Detail::WriteDns16(output + cursor, type);
        cursor += 2;
        Detail::WriteDns16(output + cursor, static_cast<uint16_t>(DnsRecordClass::Internet));
        cursor += 2;
        Detail::WriteDns32(output + cursor, ttlSeconds);
        cursor += 4;
        Detail::WriteDns16(output + cursor, static_cast<uint16_t>(addressLength));
        cursor += 2;
        std::memcpy(output + cursor, address.Bytes.data(), addressLength);

        ++_answerCount;
        Detail::WriteDns16(_buffer.data() + 6, _answerCount);
        return WebResult::Success();
    }

    WebResult Complete() override {
        if (!_valid || _completed) {
            return _completed ? WebResult::Success() : WebResult::Failure(WebError::InvalidState);
        }

        pbuf* packet = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(_buffer.size()), PBUF_RAM);
        if (packet == nullptr) return WebResult::Failure(WebError::ResourceExhausted);
        const err_t copied = pbuf_take(packet, _buffer.data(), static_cast<u16_t>(_buffer.size()));
        if (copied != ERR_OK) {
            pbuf_free(packet);
            return WebResult::Failure(Detail::DnsLwipError(copied), copied);
        }

        const err_t sent = udp_sendto(_pcb, packet, &_remoteAddress, _remotePort);
        pbuf_free(packet);
        if (sent != ERR_OK) return WebResult::Failure(Detail::DnsLwipError(sent), sent);
        _completed = true;
        return WebResult::Success();
    }

    void Abort() noexcept override { _completed = true; }

private:
    udp_pcb* _pcb = nullptr;
    ip_addr_t _remoteAddress{};
    uint16_t _remotePort = 0;
    Buffer _buffer;
    uint16_t _answerCount = 0;
    bool _valid = true;
    bool _completed = false;
};

class ESP32DnsServerPlatform final : public IDnsServerPlatform {
private:
    using Buffer = System::Memory::Vector<
        uint8_t,
        System::Memory::MemoryPolicy::ExternalPreferred
    >;

    struct ApiCall final {
        tcpip_api_call_data Call{};
        ESP32DnsServerPlatform* Owner = nullptr;
        uint16_t Port = 0;
        err_t Result = ERR_OK;
    };

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
        if (_listening.load(std::memory_order_acquire)) {
            return WebResult::Failure(WebError::InvalidState);
        }
        _configuration = configuration;
        _dispatcher.store(&dispatcher, std::memory_order_release);
        return WebResult::Success();
    }

    WebResult Start() override {
        if (_listening.load(std::memory_order_acquire)) {
            return WebResult::Failure(WebError::AlreadyRunning);
        }
        if (_dispatcher.load(std::memory_order_acquire) == nullptr) {
            return WebResult::Failure(WebError::InvalidState);
        }

        ApiCall call;
        call.Owner = this;
        call.Port = _configuration.Port;
        const err_t invoked = tcpip_api_call(&StartOnTcpipThread, &call.Call);
        const err_t result = invoked == ERR_OK ? call.Result : invoked;
        if (result != ERR_OK) {
            return WebResult::Failure(Detail::DnsLwipError(result), result);
        }
        _listening.store(true, std::memory_order_release);
        return WebResult::Success();
    }

    WebResult Stop() override {
        if (!_listening.exchange(false, std::memory_order_acq_rel)) {
            return WebResult::Success();
        }

        ApiCall call;
        call.Owner = this;
        const err_t invoked = tcpip_api_call(&StopOnTcpipThread, &call.Call);
        const err_t result = invoked == ERR_OK ? call.Result : invoked;
        return result == ERR_OK
            ? WebResult::Success()
            : WebResult::Failure(Detail::DnsLwipError(result), result);
    }

    void Reset() noexcept override {
        (void)Stop();
        _dispatcher.store(nullptr, std::memory_order_release);
        _configuration = {};
    }

private:
    static err_t StartOnTcpipThread(tcpip_api_call_data* data) {
        auto& call = *reinterpret_cast<ApiCall*>(data);
        auto* owner = call.Owner;
        if (owner == nullptr) return ERR_ARG;

        if (owner->_pcb != nullptr) {
            udp_recv(owner->_pcb, nullptr, nullptr);
            udp_remove(owner->_pcb);
            owner->_pcb = nullptr;
        }

        owner->_pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
        if (owner->_pcb == nullptr) {
            call.Result = ERR_MEM;
            return ERR_OK;
        }

        call.Result = udp_bind(owner->_pcb, IP_ANY_TYPE, call.Port);
        if (call.Result != ERR_OK) {
            udp_remove(owner->_pcb);
            owner->_pcb = nullptr;
            return ERR_OK;
        }

        udp_recv(owner->_pcb, &ReceivePacket, owner);
        return ERR_OK;
    }

    static err_t StopOnTcpipThread(tcpip_api_call_data* data) {
        auto& call = *reinterpret_cast<ApiCall*>(data);
        auto* owner = call.Owner;
        if (owner == nullptr) return ERR_ARG;
        if (owner->_pcb != nullptr) {
            udp_recv(owner->_pcb, nullptr, nullptr);
            udp_remove(owner->_pcb);
            owner->_pcb = nullptr;
        }
        call.Result = ERR_OK;
        return ERR_OK;
    }

    static void SendFormatError(
        udp_pcb* pcb,
        const ip_addr_t& remoteAddress,
        uint16_t remotePort,
        const uint8_t* request,
        std::size_t requestSize
    ) noexcept {
        if (pcb == nullptr || request == nullptr || requestSize < 2) return;
        std::array<uint8_t, 12> response{};
        response[0] = request[0];
        response[1] = request[1];
        Detail::WriteDns16(
            response.data() + 2,
            static_cast<uint16_t>(0x8000u | 0x0400u |
                static_cast<uint8_t>(DnsResponseCode::FormatError))
        );

        pbuf* packet = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(response.size()), PBUF_RAM);
        if (packet == nullptr) return;
        if (pbuf_take(packet, response.data(), static_cast<u16_t>(response.size())) == ERR_OK) {
            (void)udp_sendto(pcb, packet, &remoteAddress, remotePort);
        }
        pbuf_free(packet);
    }

    static void ReceivePacket(
        void* argument,
        udp_pcb* pcb,
        pbuf* packet,
        const ip_addr_t* remoteAddress,
        u16_t remotePort
    ) {
        auto* owner = static_cast<ESP32DnsServerPlatform*>(argument);
        if (packet == nullptr) return;

        const auto releasePacket = [&]() { pbuf_free(packet); };
        if (owner == nullptr || pcb == nullptr || remoteAddress == nullptr ||
            !owner->_listening.load(std::memory_order_acquire)) {
            releasePacket();
            return;
        }

        IDnsRequestDispatcher* dispatcher = owner->_dispatcher.load(std::memory_order_acquire);
        if (dispatcher == nullptr) {
            releasePacket();
            return;
        }

        const std::size_t size = packet->tot_len;
        if (size < 12 || size > ESPRESSIO_ESP32_DNS_MAX_PACKET_BYTES) {
            releasePacket();
            return;
        }

        Buffer bytes;
        try {
            bytes.resize(size);
        } catch (...) {
            releasePacket();
            return;
        }
        if (pbuf_copy_partial(packet, bytes.data(), static_cast<u16_t>(size), 0) != size) {
            releasePacket();
            return;
        }
        releasePacket();

        const uint16_t flags = Detail::ReadDns16(bytes.data() + 2);
        const uint16_t questionCount = Detail::ReadDns16(bytes.data() + 4);
        if ((flags & 0x8000u) != 0 || questionCount != 1) {
            SendFormatError(pcb, *remoteAddress, remotePort, bytes.data(), bytes.size());
            return;
        }

        std::array<char, 254> name{};
        std::size_t nameLength = 0;
        std::size_t encodedNameEnd = 0;
        if (!Detail::DecodeDnsName(
                bytes.data(),
                bytes.size(),
                12,
                name,
                nameLength,
                encodedNameEnd) ||
            encodedNameEnd + 4 > bytes.size()) {
            SendFormatError(pcb, *remoteAddress, remotePort, bytes.data(), bytes.size());
            return;
        }

        const uint16_t type = Detail::ReadDns16(bytes.data() + encodedNameEnd);
        const uint16_t recordClass = Detail::ReadDns16(bytes.data() + encodedNameEnd + 2);
        const std::size_t questionEnd = encodedNameEnd + 4;

        ESP32DnsRequestPlatform request(
            std::string_view(name.data(), nameLength),
            type,
            recordClass
        );
        ESP32DnsResponsePlatform response(
            pcb,
            *remoteAddress,
            remotePort,
            bytes.data(),
            bytes.size(),
            questionEnd
        );
        if (!response.Valid()) return;
        (void)dispatcher->Dispatch(request, response);
    }

    DnsServerConfiguration _configuration{};
    std::atomic<IDnsRequestDispatcher*> _dispatcher{nullptr};
    std::atomic<bool> _listening{false};
    udp_pcb* _pcb = nullptr;
};

} // namespace ESPressio::Web

#endif // ARDUINO_ARCH_ESP32
