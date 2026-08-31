#pragma once

#if !defined(ARDUINO_ARCH_ESP32)
#error "ESPressio_Raw80211Radio.hpp requires an ESP32 Arduino target"
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <WiFi.h>
#include <esp_err.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include <ESPressio_IRadio.hpp>

#ifndef ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH
#define ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH 4
#endif

namespace ESPressio::ESP32Platform {

/// <summary>Configuration for the ESP32 integrated Wi-Fi raw IEEE 802.11 packet-radio provider.</summary>
struct Raw80211RadioConfiguration {
    wifi_interface_t Interface = WIFI_IF_STA;
    uint8_t Channel = 0;
    bool InitializeStationModeWhenNeeded = true;
};

/// <summary>
/// ESPressio Radio concrete implemented with ESP32 raw non-QoS IEEE 802.11 data frames.
/// Driver callbacks only copy accepted frames into a bounded queue and wake RadioWorker; parsing/routing onward happens
/// outside the Espressif Wi-Fi callback context.
/// </summary>
class Raw80211Radio final : public Radio::IRadio {
private:
    static constexpr std::size_t MacBytes = 6;
    static constexpr std::size_t Dot11HeaderBytes = 24;
    static constexpr std::size_t EncapsulationBytes = 10;
    static constexpr std::size_t MaximumPayloadBytes = 270;
    static constexpr std::size_t MaximumFrameBytes = Dot11HeaderBytes + EncapsulationBytes + MaximumPayloadBytes;
    static constexpr uint8_t LlcSnap[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0xB5};
    static constexpr uint8_t RadioBssid[MacBytes] = {0x02, 0x45, 0x53, 0x50, 0x52, 0x01};

    struct ReceivedPacket {
        Radio::RadioAddress Source{};
        Radio::RadioAddress Destination{};
        uint16_t Length = 0;
        int16_t RssiDbm = 0;
        uint64_t TimestampNanoseconds = 0;
        std::array<uint8_t, MaximumPayloadBytes> Payload{};
    };

    Raw80211RadioConfiguration _configuration{};
    Radio::IRadioReceiver* _receiver = nullptr;
    std::atomic<Radio::IRadioWorkSignal*> _workSignal{nullptr};
    Radio::RadioObserverSubscriptions _observers{};
    Radio::RadioAddress _localAddress{};
    std::array<ReceivedPacket, ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH> _receiveQueue{};
    std::atomic<uint8_t> _writeIndex{0};
    std::atomic<uint8_t> _readIndex{0};
    std::atomic<bool> _started{false};
    bool _promiscuousWasEnabled = false;

    static Raw80211Radio*& CallbackInstance() noexcept {
        static Raw80211Radio* instance = nullptr;
        return instance;
    }

    static bool IsOurFrame(const uint8_t* data, std::size_t length) noexcept {
        if (data == nullptr || length < Dot11HeaderBytes + EncapsulationBytes) return false;
        const uint16_t frameControl = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8u);
        if ((frameControl & 0x00FCu) != 0x0008u) return false;
        if (std::memcmp(data + 16, RadioBssid, MacBytes) != 0) return false;
        return std::memcmp(data + Dot11HeaderBytes, LlcSnap, sizeof(LlcSnap)) == 0;
    }

    static void PromiscuousReceive(void* buffer, wifi_promiscuous_pkt_type_t type) {
        auto* self = CallbackInstance();
        if (self == nullptr || !self->_started.load(std::memory_order_acquire) || type != WIFI_PKT_DATA || buffer == nullptr) return;
        const auto* packet = static_cast<const wifi_promiscuous_pkt_t*>(buffer);
        const uint8_t* frame = packet->payload;
        const std::size_t frameLength = packet->rx_ctrl.sig_len;
        if (!IsOurFrame(frame, frameLength)) return;

        const std::size_t lengthOffset = Dot11HeaderBytes + sizeof(LlcSnap);
        const uint16_t payloadLength = static_cast<uint16_t>(frame[lengthOffset]) |
            (static_cast<uint16_t>(frame[lengthOffset + 1]) << 8u);
        if (payloadLength > MaximumPayloadBytes || Dot11HeaderBytes + EncapsulationBytes + payloadLength > frameLength) return;

        const uint8_t write = self->_writeIndex.load(std::memory_order_relaxed);
        const uint8_t next = static_cast<uint8_t>((write + 1u) % self->_receiveQueue.size());
        if (next == self->_readIndex.load(std::memory_order_acquire)) return;

        auto& queued = self->_receiveQueue[write];
        queued.Source = Radio::RadioAddress::FromBytes(frame + 10, MacBytes);
        queued.Destination = Radio::RadioAddress::FromBytes(frame + 4, MacBytes);
        queued.Length = payloadLength;
        queued.RssiDbm = packet->rx_ctrl.rssi;
        queued.TimestampNanoseconds = static_cast<uint64_t>(esp_timer_get_time()) * 1000ULL;
        if (payloadLength != 0) {
            std::memcpy(queued.Payload.data(), frame + Dot11HeaderBytes + EncapsulationBytes, payloadLength);
        }
        self->_writeIndex.store(next, std::memory_order_release);

        Radio::IRadioWorkSignal* signal = self->_workSignal.load(std::memory_order_acquire);
        if (signal != nullptr) signal->OnRadioWorkAvailable(*self);
    }

    bool ResolveLocalAddress() noexcept {
        uint8_t address[MacBytes]{};
        if (esp_wifi_get_mac(_configuration.Interface, address) != ESP_OK) return false;
        _localAddress = Radio::RadioAddress::FromBytes(address, MacBytes);
        return true;
    }

public:
    explicit Raw80211Radio(Raw80211RadioConfiguration configuration = {}) noexcept
        : _configuration(configuration) {}

    bool Start() override {
        if (_started.load(std::memory_order_acquire)) return true;
        if (CallbackInstance() != nullptr && CallbackInstance() != this) return false;

        wifi_mode_t mode = WIFI_MODE_NULL;
        const esp_err_t modeResult = esp_wifi_get_mode(&mode);
        if ((modeResult != ESP_OK || mode == WIFI_MODE_NULL) && _configuration.InitializeStationModeWhenNeeded) {
            if (!::WiFi.mode(WIFI_STA)) return false;
            mode = WIFI_MODE_STA;
        }
        if (mode == WIFI_MODE_NULL) return false;
        if (_configuration.Interface == WIFI_IF_STA && mode == WIFI_MODE_AP) return false;
        if (_configuration.Interface == WIFI_IF_AP && mode == WIFI_MODE_STA) return false;

        if (_configuration.Channel != 0 && esp_wifi_set_channel(_configuration.Channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
        if (!ResolveLocalAddress()) return false;

        bool promiscuous = false;
        if (esp_wifi_get_promiscuous(&promiscuous) == ESP_OK) _promiscuousWasEnabled = promiscuous;
        CallbackInstance() = this;
        if (esp_wifi_set_promiscuous_rx_cb(&Raw80211Radio::PromiscuousReceive) != ESP_OK) {
            CallbackInstance() = nullptr;
            return false;
        }
        if (esp_wifi_set_promiscuous(true) != ESP_OK) {
            (void)esp_wifi_set_promiscuous_rx_cb(nullptr);
            CallbackInstance() = nullptr;
            return false;
        }
        _readIndex.store(0, std::memory_order_relaxed);
        _writeIndex.store(0, std::memory_order_relaxed);
        _started.store(true, std::memory_order_release);
        _observers.NotifyStarted(*this);
        return true;
    }

    void Stop() noexcept override {
        if (!_started.exchange(false, std::memory_order_acq_rel)) return;
        if (CallbackInstance() == this) {
            (void)esp_wifi_set_promiscuous_rx_cb(nullptr);
            if (!_promiscuousWasEnabled) (void)esp_wifi_set_promiscuous(false);
            CallbackInstance() = nullptr;
        }
        _readIndex.store(0, std::memory_order_relaxed);
        _writeIndex.store(0, std::memory_order_relaxed);
        _observers.NotifyStopped(*this);
    }

    bool IsStarted() const noexcept override { return _started.load(std::memory_order_acquire); }

    Radio::RadioCapabilities Capabilities() const noexcept override {
        return {
            Radio::RadioCapability::Broadcast |
            Radio::RadioCapability::Rssi |
            Radio::RadioCapability::ChannelSelection |
            Radio::RadioCapability::DataRateSelection |
            Radio::RadioCapability::TransmitPower |
            Radio::RadioCapability::HardwareAddressing |
            Radio::RadioCapability::ReceiveTimestamp |
            Radio::RadioCapability::CarrierSense,
            static_cast<uint16_t>(MaximumPayloadBytes),
            static_cast<uint8_t>(MacBytes)
        };
    }

    Radio::RadioAddress LocalAddress() const noexcept override { return _localAddress; }

    Radio::RadioSendResult Send(
        const Radio::RadioAddress& destination,
        const uint8_t* payload,
        std::size_t payloadSize
    ) override {
        const auto complete = [&](Radio::RadioSendResult result) {
            _observers.NotifySendCompleted(*this, destination, payloadSize, result);
            return result;
        };
        if (!IsStarted()) return complete({Radio::RadioSendStatus::NotStarted, 0});
        if (!destination.IsValid() || destination.Length != MacBytes) return complete({Radio::RadioSendStatus::InvalidAddress, 0});
        if ((payload == nullptr && payloadSize != 0) || payloadSize > MaximumPayloadBytes)
            return complete({Radio::RadioSendStatus::PayloadTooLarge, 0});

        std::array<uint8_t, MaximumFrameBytes> frame{};
        frame[0] = 0x08;
        frame[1] = 0x00;
        std::memcpy(frame.data() + 4, destination.Bytes.data(), MacBytes);
        std::memcpy(frame.data() + 10, _localAddress.Bytes.data(), MacBytes);
        std::memcpy(frame.data() + 16, RadioBssid, MacBytes);
        std::memcpy(frame.data() + Dot11HeaderBytes, LlcSnap, sizeof(LlcSnap));
        const std::size_t lengthOffset = Dot11HeaderBytes + sizeof(LlcSnap);
        frame[lengthOffset] = static_cast<uint8_t>(payloadSize & 0xFFu);
        frame[lengthOffset + 1] = static_cast<uint8_t>((payloadSize >> 8u) & 0xFFu);
        if (payloadSize != 0) std::memcpy(frame.data() + Dot11HeaderBytes + EncapsulationBytes, payload, payloadSize);

        const esp_err_t result = esp_wifi_80211_tx(
            _configuration.Interface,
            frame.data(),
            static_cast<int>(Dot11HeaderBytes + EncapsulationBytes + payloadSize),
            true
        );
        if (result == ESP_OK) return complete(Radio::RadioSendResult::Accepted());
#ifdef ESP_ERR_NO_MEM
        if (result == ESP_ERR_NO_MEM) return complete({Radio::RadioSendStatus::NoMemory, static_cast<int32_t>(result)});
#endif
        return complete({Radio::RadioSendStatus::NativeFailure, static_cast<int32_t>(result)});
    }

    void SetReceiver(Radio::IRadioReceiver* receiver) noexcept override { _receiver = receiver; }
    void SetWorkSignal(Radio::IRadioWorkSignal* signal) noexcept override {
        _workSignal.store(signal, std::memory_order_release);
    }
    Radio::RadioObserverSubscriptions& Observers() noexcept override { return _observers; }

    void DrainInbound() override {
        while (true) {
            const uint8_t read = _readIndex.load(std::memory_order_relaxed);
            if (read == _writeIndex.load(std::memory_order_acquire)) return;
            const auto& queued = _receiveQueue[read];
            Radio::RadioPacketView view;
            view.Source = queued.Source;
            view.Destination = queued.Destination;
            view.Payload = queued.Length == 0 ? nullptr : queued.Payload.data();
            view.PayloadSize = queued.Length;
            view.RssiDbm = queued.RssiDbm;
            view.ReceiveTimestampNanoseconds = queued.TimestampNanoseconds;
            view.Flags = queued.Destination.IsBroadcast() ? Radio::RadioPacketFlag::Broadcast : Radio::RadioPacketFlag::None;
            if (_receiver != nullptr) _receiver->OnRadioPacket(*this, view);
            _readIndex.store(static_cast<uint8_t>((read + 1u) % _receiveQueue.size()), std::memory_order_release);
        }
    }
};

} // namespace ESPressio::ESP32Platform