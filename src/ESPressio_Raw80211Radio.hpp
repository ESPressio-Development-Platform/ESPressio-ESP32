#pragma once

#if !defined(ARDUINO_ARCH_ESP32)
#error "ESPressio_Raw80211Radio.hpp requires an ESP32 Arduino target"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

#include <WiFi.h>
#include <esp_err.h>
#include <esp_wifi.h>

#include <ESPressio_IRadio.hpp>
#include <ESPressio_Memory.hpp>
#include <ESPressio_RadioControl.hpp>
#include <ESPressio_Synchronization.hpp>
#include <ESPressio_SystemPlatformClock.hpp>

#include "ESPressio_Raw80211WiFiBootstrap.hpp"
#include "ESPressio_WiFiPhyCoordinator.hpp"

#ifndef ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH
#define ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH 4
#endif

#ifndef ESPRESSIO_ESP32_RAW_RADIO_CONTROL_RX_QUEUE_DEPTH
#define ESPRESSIO_ESP32_RAW_RADIO_CONTROL_RX_QUEUE_DEPTH 4
#endif

#ifndef ESPRESSIO_ESP32_RAW_RADIO_LEAN_WIFI_BOOTSTRAP
#define ESPRESSIO_ESP32_RAW_RADIO_LEAN_WIFI_BOOTSTRAP 0
#endif

namespace ESPressio::ESP32Platform {

/// <summary>Compile-time contract: Raw80211 RX evidence uses System::Clock::Monotonic's domain.</summary>
inline constexpr bool Raw80211ReceiveTimestampUsesSystemMonotonic = true;

/// <summary>Whether this build uses the opt-in raw-only lean ESP-IDF Wi-Fi bootstrap when Raw80211 owns driver startup.</summary>
inline constexpr bool Raw80211LeanWiFiBootstrapEnabled =
    ESPRESSIO_ESP32_RAW_RADIO_LEAN_WIFI_BOOTSTRAP != 0;

/// <summary>Configuration for the ESP32 integrated Wi-Fi raw IEEE 802.11 packet-radio provider.</summary>

struct Raw80211RadioConfiguration {
    wifi_interface_t Interface = WIFI_IF_STA;

    /// <summary>
    /// Preferred raw-radio channel. Zero follows the effective shared Wi-Fi PHY channel. A non-zero value may constrain
    /// the PHY only while ordinary ESPressio Wi-Fi is inactive; when Wi-Fi owns a different channel this radio becomes
    /// temporarily unavailable rather than retuning/disrupting the shared PHY.
    /// </summary>
    uint8_t Channel = 0;

    bool InitializeStationModeWhenNeeded = true;
};

/// <summary>Diagnostic evidence for the most recently worker-serviced Raw80211 receive timestamp.</summary>

struct Raw80211ReceiveTimestampStatistics final {
    std::uint64_t SampleNumber{0U};
    std::uint32_t AlignmentResetCount{0U};
    std::uint32_t AlignmentSampleCount{0U};
    bool AlignmentFrozen{false};
    bool AlignmentResetOnThisSample{false};
    std::uint32_t RawWiFiTimestampMicroseconds{0U};
    std::uint64_t CallbackMonotonicTimestampNanoseconds{0U};
    std::uint64_t ExtendedWiFiTimestampMicroseconds{0U};
    std::int64_t ObservedCallbackLagMicroseconds{0};
    std::int64_t SelectedAlignmentMicroseconds{0};
    std::uint64_t MappedMonotonicTimestampNanoseconds{0U};
};

/// <summary>
/// ESPressio Radio concrete implemented with ESP32 raw non-QoS IEEE 802.11 data frames.
/// </summary>
/// <remarks>
/// The ESP-IDF Wi-Fi driver-task callback only validates/copies accepted frames, maps the provider RX timestamp, asks an
/// injected generic ingress classifier for Standard vs Control urgency, and wakes the corresponding worker. The concrete
/// never understands clock synchronization or another Radio control protocol. Control and standard packets occupy
/// independent bounded queues so ordinary transfer backlog cannot delay a time-critical control packet before worker
/// scheduling.
///
/// Queue service is cooperative: one worker invocation consumes only a finite snapshot-derived packet quantum and then
/// returns to the scheduler. Concurrent producer arrivals cannot extend the current pass indefinitely. PHY availability
/// is a lifecycle-published atomic cache; no packet send/service path interrogates the Wi-Fi driver for channel state.
///
/// When ESPRESSIO_ESP32_RAW_RADIO_LEAN_WIFI_BOOTSTRAP is enabled and no Wi-Fi driver exists yet, the provider starts an
/// ESP-IDF station driver with the bounded Raw80211-only memory profile instead of Arduino's general-purpose WiFi station
/// lifecycle. This is intended only for compositions that do not later attach ordinary Wi-Fi/LwIP networking. If Wi-Fi
/// already owns the PHY, Raw80211 always joins that existing lifecycle unchanged.
///
/// ESP-IDF's private-epoch RX timestamp is aligned to System monotonic during a finite warm-up window. The best observed
/// callback-lag alignment is then frozen until a driver timestamp discontinuity or provider restart. This deliberately
/// avoids the old rolling-minimum mapping whose epoch could move throughout a clock-synchronization session.
/// </remarks>

class Raw80211Radio final : public Radio::IRadio, public Radio::IRadioPrioritizedIngress {
private:
    static constexpr std::size_t MacBytes = 6;
    static constexpr std::size_t Dot11HeaderBytes = 24;
    static constexpr std::size_t EncapsulationBytes = 10;
    static constexpr std::size_t MaximumPayloadBytes = 270;
    static constexpr uint16_t MaximumLogicalTransferBytes = 4096;
    static constexpr std::size_t MaximumFrameBytes = Dot11HeaderBytes + EncapsulationBytes + MaximumPayloadBytes;
    static constexpr uint8_t LlcSnap[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0xB5};
    static constexpr uint8_t RadioBssid[MacBytes] = {0x02, 0x45, 0x53, 0x50, 0x52, 0x01};
    static constexpr std::uint32_t ReceiveTimestampAlignmentSamples = 32U;

    static_assert(ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH > 1,
                  "Raw radio RX queue depth must be at least two");
    static_assert(ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH <= 255,
                  "Raw radio RX queue depth must fit its indices");
    static_assert(ESPRESSIO_ESP32_RAW_RADIO_CONTROL_RX_QUEUE_DEPTH > 1,
                  "Raw radio control RX queue depth must be at least two");
    static_assert(ESPRESSIO_ESP32_RAW_RADIO_CONTROL_RX_QUEUE_DEPTH <= 255,
                  "Raw radio control RX queue depth must fit its indices");


struct TimestampMapping final {
        Raw80211ReceiveTimestampStatistics Statistics{};
    };


struct ReceivedPacket {
        Radio::RadioAddress Source{};
        Radio::RadioAddress Destination{};
        uint16_t Length = 0;
        int16_t RssiDbm = 0;
        uint64_t TimestampNanoseconds = 0;
        Raw80211ReceiveTimestampStatistics TimestampEvidence{};
        std::array<uint8_t, MaximumPayloadBytes> Payload{};
    };

    using ReceiveQueue = System::Memory::Vector<
        ReceivedPacket,
        System::Memory::MemoryPolicy::ExternalPreferred
    >;

    Raw80211RadioConfiguration _configuration{};
    Radio::IRadioReceiver* _receiver = nullptr;
    Radio::IRadioReceiver* _controlReceiver = nullptr;
    std::atomic<Radio::IRadioWorkSignal*> _workSignal{nullptr};
    std::atomic<Radio::IRadioWorkSignal*> _controlWorkSignal{nullptr};
    std::atomic<Radio::IRadioIngressClassifier*> _ingressClassifier{nullptr};
    Radio::RadioObserverSubscriptions _observers{};
    Radio::RadioAddress _localAddress{};

    ReceiveQueue _receiveQueue{};
    ReceiveQueue _controlReceiveQueue{};
    std::atomic<uint8_t> _writeIndex{0};
    std::atomic<uint8_t> _readIndex{0};
    std::atomic<uint8_t> _controlWriteIndex{0};
    std::atomic<uint8_t> _controlReadIndex{0};

    // These diagnostics are mutated from the ESP-IDF Wi-Fi driver task. Keep them 32-bit to avoid unnecessary wider
    // atomic operations on the 32-bit target; the public statistics snapshot widens them to its diagnostic-width fields.
    std::atomic<std::uint32_t> _standardAcceptedPackets{0U};
    std::atomic<std::uint32_t> _standardDroppedPackets{0U};
    std::atomic<std::uint32_t> _standardHighWatermark{0U};
    std::atomic<std::uint32_t> _controlAcceptedPackets{0U};
    std::atomic<std::uint32_t> _controlDroppedPackets{0U};
    std::atomic<std::uint32_t> _controlHighWatermark{0U};

    std::atomic<bool> _started{false};
    bool _promiscuousWasEnabled = false;
    bool _phyRegistered = false;
    Raw80211WiFiBootstrap _leanWiFiBootstrap{};

    // Single-writer state: only the process-global ESP-IDF Raw80211 callback mutates the mapping below.
    bool _hasReceiveTimestampAlignment = false;
    bool _receiveTimestampAlignmentFrozen = false;
    uint32_t _lastWiFiReceiveTimestampMicroseconds = 0U;
    uint64_t _extendedWiFiReceiveTimestampMicroseconds = 0U;
    int64_t _selectedReceiveAlignmentMicroseconds = 0;
    uint32_t _receiveTimestampAlignmentCount = 0U;
    uint32_t _receiveTimestampAlignmentResetCount = 0U;
    uint64_t _receiveTimestampSampleNumber = 0U;

    // Published only after receiver processing, never from the Wi-Fi callback. Diagnostic readers cannot delay T2/T4.
    mutable System::Synchronization::Mutex _timestampStatisticsMutex;
    Raw80211ReceiveTimestampStatistics _lastStandardTimestampStatistics{};
    Raw80211ReceiveTimestampStatistics _lastControlTimestampStatistics{};

    static std::atomic<Raw80211Radio*>& CallbackInstance() noexcept {
        static std::atomic<Raw80211Radio*> instance{nullptr};
        return instance;
    }

    static bool IsOurFrame(const uint8_t* data, std::size_t length) noexcept {
        if (data == nullptr || length < Dot11HeaderBytes + EncapsulationBytes) return false;
        const uint16_t frameControl = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8u);
        if ((frameControl & 0x00FCu) != 0x0008u) return false;
        if (std::memcmp(data + 16, RadioBssid, MacBytes) != 0) return false;
        return std::memcmp(data + Dot11HeaderBytes, LlcSnap, sizeof(LlcSnap)) == 0;
    }

    static bool IsBroadcastMac(const uint8_t* address) noexcept {
        if (address == nullptr) return false;
        for (std::size_t i = 0; i < MacBytes; ++i) {
            if (address[i] != 0xFFu) return false;
        }
        return true;
    }

    static std::uint32_t QueueDepth(
        std::uint8_t read,
        std::uint8_t write,
        std::size_t size
    ) noexcept {
        if (size == 0U) return 0U;
        return write >= read
            ? static_cast<std::uint32_t>(write - read)
            : static_cast<std::uint32_t>(size - static_cast<std::size_t>(read - write));
    }

    static void UpdateHighWatermark(
        std::atomic<std::uint32_t>& target,
        std::uint32_t depth
    ) noexcept {
        auto current = target.load(std::memory_order_relaxed);
        while (depth > current &&
               !target.compare_exchange_weak(
                   current, depth, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }

    bool Enqueue(
        ReceiveQueue& queue,
        std::atomic<std::uint8_t>& writeIndex,
        std::atomic<std::uint8_t>& readIndex,
        std::atomic<std::uint32_t>& acceptedPackets,
        std::atomic<std::uint32_t>& droppedPackets,
        std::atomic<std::uint32_t>& highWatermark,
        const std::uint8_t* frame,
        std::uint16_t payloadLength,
        std::int16_t rssiDbm,
        const Raw80211ReceiveTimestampStatistics& timestampEvidence
    ) noexcept {
        if (queue.empty()) {
            droppedPackets.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        const auto write = writeIndex.load(std::memory_order_relaxed);
        const auto next = static_cast<std::uint8_t>((write + 1U) % queue.size());
        const auto read = readIndex.load(std::memory_order_acquire);
        if (next == read) {
            droppedPackets.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }

        auto& queued = queue[write];
        queued.Source = Radio::RadioAddress::FromBytes(frame + 10, MacBytes);
        queued.Destination = Radio::RadioAddress::FromBytes(frame + 4, MacBytes);
        queued.Length = payloadLength;
        queued.RssiDbm = rssiDbm;
        queued.TimestampNanoseconds = timestampEvidence.MappedMonotonicTimestampNanoseconds;
        queued.TimestampEvidence = timestampEvidence;
        if (payloadLength != 0U) {
            std::memcpy(
                queued.Payload.data(),
                frame + Dot11HeaderBytes + EncapsulationBytes,
                payloadLength);
        }
        writeIndex.store(next, std::memory_order_release);
        acceptedPackets.fetch_add(1U, std::memory_order_relaxed);
        UpdateHighWatermark(highWatermark, QueueDepth(read, next, queue.size()));
        return true;
    }

    Radio::RadioIngressServiceResult ServiceQueue(
        ReceiveQueue& queue,
        std::atomic<std::uint8_t>& readIndex,
        std::atomic<std::uint8_t>& writeIndex,
        Radio::IRadioReceiver* receiver,
        std::size_t maximumPackets,
        bool controlQueue
    ) {
        if (queue.empty()) return {};

        const auto initialRead = readIndex.load(std::memory_order_relaxed);
        const auto initialWrite = writeIndex.load(std::memory_order_acquire);
        const auto initialDepth = QueueDepth(initialRead, initialWrite, queue.size());
        if (initialDepth == 0U) return {};

        // Hot-path availability is an atomic cache read only. No Wi-Fi mutex or ESP-IDF query is reachable here.
        if (!SharedWiFiPhy().CachedRawAccess()) {
            readIndex.store(initialWrite, std::memory_order_release);
            return {};
        }

        const std::size_t finiteDefault = queue.size() > 1U ? queue.size() - 1U : 1U;
        const std::size_t requested = maximumPackets == 0U ? finiteDefault : maximumPackets;
        const std::size_t toProcess = std::min<std::size_t>(requested, initialDepth);

        auto read = initialRead;
        std::uint32_t processed = 0U;
        for (std::size_t index = 0U; index < toProcess; ++index) {
            const auto& queued = queue[read];
            Radio::RadioPacketView view;
            view.Source = queued.Source;
            view.Destination = queued.Destination;
            view.Payload = queued.Length == 0U ? nullptr : queued.Payload.data();
            view.PayloadSize = queued.Length;
            view.RssiDbm = queued.RssiDbm;
            view.ReceiveTimestampNanoseconds = queued.TimestampNanoseconds;
            view.Flags = queued.Destination.IsBroadcast()
                ? Radio::RadioPacketFlag::Broadcast
                : Radio::RadioPacketFlag::None;

            if (receiver != nullptr) receiver->OnRadioPacket(*this, view);

            // Publish timestamp diagnostics only after protocol processing so diagnostics can never delay T2/T4 work.
            {
                std::lock_guard<System::Synchronization::Mutex> lock(_timestampStatisticsMutex);
                if (controlQueue) _lastControlTimestampStatistics = queued.TimestampEvidence;
                else _lastStandardTimestampStatistics = queued.TimestampEvidence;
            }

            read = static_cast<std::uint8_t>((read + 1U) % queue.size());
            readIndex.store(read, std::memory_order_release);
            ++processed;
        }

        const auto latestWrite = writeIndex.load(std::memory_order_acquire);
        return {processed, read != latestWrite};
    }

    Raw80211ReceiveTimestampStatistics MapReceiveTimestamp(
        uint32_t wifiTimestampMicroseconds,
        uint64_t callbackTimestampNanoseconds
    ) noexcept {
        const uint64_t callbackMicroseconds = callbackTimestampNanoseconds / 1000ULL;
        bool resetAlignment = false;

        if (!_hasReceiveTimestampAlignment) {
            _hasReceiveTimestampAlignment = true;
            _lastWiFiReceiveTimestampMicroseconds = wifiTimestampMicroseconds;
            _extendedWiFiReceiveTimestampMicroseconds = wifiTimestampMicroseconds;
            resetAlignment = true;
        } else {
            const uint32_t elapsed = wifiTimestampMicroseconds - _lastWiFiReceiveTimestampMicroseconds;
            _lastWiFiReceiveTimestampMicroseconds = wifiTimestampMicroseconds;
            if (elapsed > 0x80000000UL) {
                // Treat a large backwards discontinuity as driver/timestamp-epoch restart, not normal uint32 wrap.
                _extendedWiFiReceiveTimestampMicroseconds = wifiTimestampMicroseconds;
                resetAlignment = true;
            } else {
                _extendedWiFiReceiveTimestampMicroseconds += elapsed;
            }
        }

        const int64_t observedCallbackLag =
            static_cast<int64_t>(callbackMicroseconds) -
            static_cast<int64_t>(_extendedWiFiReceiveTimestampMicroseconds);

        if (resetAlignment) {
            _receiveTimestampAlignmentCount = 0U;
            _receiveTimestampAlignmentFrozen = false;
            _selectedReceiveAlignmentMicroseconds = observedCallbackLag;
            ++_receiveTimestampAlignmentResetCount;
        }

        if (!_receiveTimestampAlignmentFrozen) {
            if (_receiveTimestampAlignmentCount == 0U ||
                observedCallbackLag < _selectedReceiveAlignmentMicroseconds) {
                _selectedReceiveAlignmentMicroseconds = observedCallbackLag;
            }
            if (_receiveTimestampAlignmentCount < ReceiveTimestampAlignmentSamples) {
                ++_receiveTimestampAlignmentCount;
            }
            if (_receiveTimestampAlignmentCount >= ReceiveTimestampAlignmentSamples) {
                _receiveTimestampAlignmentFrozen = true;
            }
        }

        const int64_t mappedMicroseconds =
            static_cast<int64_t>(_extendedWiFiReceiveTimestampMicroseconds) +
            _selectedReceiveAlignmentMicroseconds;

        std::uint64_t mappedNanoseconds = 1U;
        if (mappedMicroseconds > 0) {
            if (static_cast<uint64_t>(mappedMicroseconds) >
                std::numeric_limits<uint64_t>::max() / 1000ULL) {
                mappedNanoseconds = std::numeric_limits<uint64_t>::max();
            } else {
                mappedNanoseconds = static_cast<uint64_t>(mappedMicroseconds) * 1000ULL;
            }
        }

        Raw80211ReceiveTimestampStatistics statistics{};
        statistics.SampleNumber = ++_receiveTimestampSampleNumber;
        statistics.AlignmentResetCount = _receiveTimestampAlignmentResetCount;
        statistics.AlignmentSampleCount = _receiveTimestampAlignmentCount;
        statistics.AlignmentFrozen = _receiveTimestampAlignmentFrozen;
        statistics.AlignmentResetOnThisSample = resetAlignment;
        statistics.RawWiFiTimestampMicroseconds = wifiTimestampMicroseconds;
        statistics.CallbackMonotonicTimestampNanoseconds = callbackTimestampNanoseconds;
        statistics.ExtendedWiFiTimestampMicroseconds = _extendedWiFiReceiveTimestampMicroseconds;
        statistics.ObservedCallbackLagMicroseconds = observedCallbackLag;
        statistics.SelectedAlignmentMicroseconds = _selectedReceiveAlignmentMicroseconds;
        statistics.MappedMonotonicTimestampNanoseconds = mappedNanoseconds;
        return statistics;
    }

    static void PromiscuousReceive(void* buffer, wifi_promiscuous_pkt_type_t type) {
        auto* self = CallbackInstance().load(std::memory_order_acquire);
        if (self == nullptr ||
            !self->_started.load(std::memory_order_acquire) ||
            type != WIFI_PKT_DATA || buffer == nullptr) return;

        const auto* packet = static_cast<const wifi_promiscuous_pkt_t*>(buffer);
        const uint8_t* frame = packet->payload;
        const std::size_t frameLength = packet->rx_ctrl.sig_len;
        if (!IsOurFrame(frame, frameLength)) return;

        const uint8_t* destinationMac = frame + 4;
        if (std::memcmp(destinationMac, self->_localAddress.Bytes.data(), MacBytes) != 0 &&
            !IsBroadcastMac(destinationMac)) return;

        const std::size_t lengthOffset = Dot11HeaderBytes + sizeof(LlcSnap);
        const uint16_t payloadLength = static_cast<uint16_t>(frame[lengthOffset]) |
            (static_cast<uint16_t>(frame[lengthOffset + 1]) << 8u);
        if (payloadLength > MaximumPayloadBytes ||
            Dot11HeaderBytes + EncapsulationBytes + payloadLength > frameLength) return;

        const auto* payload = frame + Dot11HeaderBytes + EncapsulationBytes;
        auto ingressClass = Radio::RadioIngressClass::Standard;
        if (auto* classifier = self->_ingressClassifier.load(std::memory_order_acquire)) {
            ingressClass = classifier->ClassifyInbound(*self, payload, payloadLength);
        }

        const uint64_t callbackTimestampNanoseconds =
            System::Clock::Monotonic().NowNanoseconds();
        const auto timestampEvidence = self->MapReceiveTimestamp(
            packet->rx_ctrl.timestamp,
            callbackTimestampNanoseconds);

        if (ingressClass == Radio::RadioIngressClass::Control) {
            if (!self->Enqueue(
                    self->_controlReceiveQueue,
                    self->_controlWriteIndex,
                    self->_controlReadIndex,
                    self->_controlAcceptedPackets,
                    self->_controlDroppedPackets,
                    self->_controlHighWatermark,
                    frame,
                    payloadLength,
                    packet->rx_ctrl.rssi,
                    timestampEvidence)) return;
            if (auto* signal = self->_controlWorkSignal.load(std::memory_order_acquire)) {
                signal->OnRadioWorkAvailable(*self);
            }
            return;
        }

        if (!self->Enqueue(
                self->_receiveQueue,
                self->_writeIndex,
                self->_readIndex,
                self->_standardAcceptedPackets,
                self->_standardDroppedPackets,
                self->_standardHighWatermark,
                frame,
                payloadLength,
                packet->rx_ctrl.rssi,
                timestampEvidence)) return;
        if (auto* signal = self->_workSignal.load(std::memory_order_acquire)) {
            signal->OnRadioWorkAvailable(*self);
        }
    }

    bool ResolveLocalAddress() noexcept {
        uint8_t address[MacBytes]{};
        if (esp_wifi_get_mac(_configuration.Interface, address) != ESP_OK) return false;
        _localAddress = Radio::RadioAddress::FromBytes(address, MacBytes);
        return true;
    }

    void ReleasePhyRegistration() noexcept {
        if (!_phyRegistered) return;
        SharedWiFiPhy().ReleaseRawAccess();
        _phyRegistered = false;
    }

    void ReleaseOwnedWiFiBootstrap() noexcept {
#if ESPRESSIO_ESP32_RAW_RADIO_LEAN_WIFI_BOOTSTRAP
        _leanWiFiBootstrap.Shutdown();
#endif
    }

public:
    explicit Raw80211Radio(Raw80211RadioConfiguration configuration = {}) noexcept
        : _configuration(configuration) {}

    bool Start() override {
        if (_started.load(std::memory_order_acquire)) return true;
        if (CallbackInstance().load(std::memory_order_acquire) != nullptr) return false;

        try {
            if (_receiveQueue.size() != ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH) {
                _receiveQueue.resize(ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH);
            }
            if (_controlReceiveQueue.size() != ESPRESSIO_ESP32_RAW_RADIO_CONTROL_RX_QUEUE_DEPTH) {
                _controlReceiveQueue.resize(ESPRESSIO_ESP32_RAW_RADIO_CONTROL_RX_QUEUE_DEPTH);
            }
        } catch (...) {
            return false;
        }

        wifi_mode_t mode = WIFI_MODE_NULL;
        const esp_err_t modeResult = esp_wifi_get_mode(&mode);
        if ((modeResult != ESP_OK || mode == WIFI_MODE_NULL) && _configuration.InitializeStationModeWhenNeeded) {
#if ESPRESSIO_ESP32_RAW_RADIO_LEAN_WIFI_BOOTSTRAP
            const auto bootstrap = _leanWiFiBootstrap.Initialize();
            if (!bootstrap || esp_wifi_get_mode(&mode) != ESP_OK) {
                ReleaseOwnedWiFiBootstrap();
                return false;
            }
#else
            if (!::WiFi.mode(WIFI_STA)) return false;
            mode = WIFI_MODE_STA;
#endif
        }
        if (mode == WIFI_MODE_NULL) {
            ReleaseOwnedWiFiBootstrap();
            return false;
        }
        if (_configuration.Interface == WIFI_IF_STA && mode == WIFI_MODE_AP) {
            ReleaseOwnedWiFiBootstrap();
            return false;
        }
        if (_configuration.Interface == WIFI_IF_AP && mode == WIFI_MODE_STA) {
            ReleaseOwnedWiFiBootstrap();
            return false;
        }

        const auto phyAccess = SharedWiFiPhy().RegisterRawAccess(_configuration.Channel, true);
        if (!phyAccess) {
            ReleaseOwnedWiFiBootstrap();
            return false;
        }
        _phyRegistered = true;

        if (!ResolveLocalAddress()) {
            ReleasePhyRegistration();
            ReleaseOwnedWiFiBootstrap();
            return false;
        }

        Raw80211Radio* expectedOwner = nullptr;
        if (!CallbackInstance().compare_exchange_strong(
                expectedOwner, this, std::memory_order_acq_rel, std::memory_order_acquire)) {
            ReleasePhyRegistration();
            ReleaseOwnedWiFiBootstrap();
            return false;
        }

        bool promiscuous = false;
        if (esp_wifi_get_promiscuous(&promiscuous) == ESP_OK) _promiscuousWasEnabled = promiscuous;
        if (esp_wifi_set_promiscuous_rx_cb(&Raw80211Radio::PromiscuousReceive) != ESP_OK) {
            expectedOwner = this;
            (void)CallbackInstance().compare_exchange_strong(
                expectedOwner, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
            ReleasePhyRegistration();
            ReleaseOwnedWiFiBootstrap();
            return false;
        }
        if (esp_wifi_set_promiscuous(true) != ESP_OK) {
            (void)esp_wifi_set_promiscuous_rx_cb(nullptr);
            expectedOwner = this;
            (void)CallbackInstance().compare_exchange_strong(
                expectedOwner, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
            ReleasePhyRegistration();
            ReleaseOwnedWiFiBootstrap();
            return false;
        }

        _readIndex.store(0U, std::memory_order_relaxed);
        _writeIndex.store(0U, std::memory_order_relaxed);
        _controlReadIndex.store(0U, std::memory_order_relaxed);
        _controlWriteIndex.store(0U, std::memory_order_relaxed);
        _hasReceiveTimestampAlignment = false;
        _receiveTimestampAlignmentFrozen = false;
        _lastWiFiReceiveTimestampMicroseconds = 0U;
        _extendedWiFiReceiveTimestampMicroseconds = 0U;
        _selectedReceiveAlignmentMicroseconds = 0;
        _receiveTimestampAlignmentCount = 0U;
        _receiveTimestampAlignmentResetCount = 0U;
        _receiveTimestampSampleNumber = 0U;
        {
            std::lock_guard<System::Synchronization::Mutex> lock(_timestampStatisticsMutex);
            _lastStandardTimestampStatistics = {};
            _lastControlTimestampStatistics = {};
        }
        _started.store(true, std::memory_order_release);
        _observers.NotifyStarted(*this);
        return true;
    }

    void Stop() noexcept override {
        if (!_started.exchange(false, std::memory_order_acq_rel)) return;
        if (CallbackInstance().load(std::memory_order_acquire) == this) {
            (void)esp_wifi_set_promiscuous_rx_cb(nullptr);
            if (!_promiscuousWasEnabled) (void)esp_wifi_set_promiscuous(false);
            Raw80211Radio* expectedOwner = this;
            (void)CallbackInstance().compare_exchange_strong(
                expectedOwner, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
        }
        _readIndex.store(0U, std::memory_order_relaxed);
        _writeIndex.store(0U, std::memory_order_relaxed);
        _controlReadIndex.store(0U, std::memory_order_relaxed);
        _controlWriteIndex.store(0U, std::memory_order_relaxed);
        ReleasePhyRegistration();
        ReleaseOwnedWiFiBootstrap();
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
            static_cast<uint8_t>(MacBytes),
            MaximumLogicalTransferBytes
        };
    }

    Radio::RadioAddress LocalAddress() const noexcept override { return _localAddress; }

    Radio::RadioSendResult Send(
        const Radio::RadioAddress& destination,
        const uint8_t* payload,
        std::size_t payloadSize
    ) override {
        const auto complete = [&](Radio::RadioSendResult result) {
            _observers.NotifySendAttempted(*this, destination, payloadSize, result);
            return result;
        };
        if (!IsStarted()) return complete({Radio::RadioSendStatus::NotStarted, 0});
        if (!SharedWiFiPhy().CachedRawAccess()) return complete({Radio::RadioSendStatus::Busy, 0});
        if (!destination.IsValid() || destination.Length != MacBytes)
            return complete({Radio::RadioSendStatus::InvalidAddress, 0});
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
        if (payloadSize != 0) {
            std::memcpy(frame.data() + Dot11HeaderBytes + EncapsulationBytes, payload, payloadSize);
        }

        const esp_err_t result = esp_wifi_80211_tx(
            _configuration.Interface,
            frame.data(),
            static_cast<int>(Dot11HeaderBytes + EncapsulationBytes + payloadSize),
            true);
        if (result == ESP_OK) return complete(Radio::RadioSendResult::Accepted());
#ifdef ESP_ERR_NO_MEM
        if (result == ESP_ERR_NO_MEM)
            return complete({Radio::RadioSendStatus::NoMemory, static_cast<int32_t>(result)});
#endif
        return complete({Radio::RadioSendStatus::NativeFailure, static_cast<int32_t>(result)});
    }

    void SetReceiver(Radio::IRadioReceiver* receiver) noexcept override { _receiver = receiver; }
    void SetWorkSignal(Radio::IRadioWorkSignal* signal) noexcept override {
        _workSignal.store(signal, std::memory_order_release);
    }
    Radio::RadioObserverSubscriptions& Observers() noexcept override { return _observers; }

    void SetIngressClassifier(Radio::IRadioIngressClassifier* classifier) noexcept override {
        _ingressClassifier.store(classifier, std::memory_order_release);
    }
    void SetControlReceiver(Radio::IRadioReceiver* receiver) noexcept override {
        _controlReceiver = receiver;
    }
    void SetControlWorkSignal(Radio::IRadioWorkSignal* signal) noexcept override {
        _controlWorkSignal.store(signal, std::memory_order_release);
    }

    void DrainInbound() override {
        (void)ServiceInbound(0U);
    }

    Radio::RadioIngressServiceResult ServiceInbound(std::size_t maximumPackets = 0U) override {
        return ServiceQueue(
            _receiveQueue,
            _readIndex,
            _writeIndex,
            _receiver,
            maximumPackets,
            false);
    }

    void DrainControlInbound() override {
        (void)ServiceControlInbound(0U);
    }

    Radio::RadioIngressServiceResult ServiceControlInbound(std::size_t maximumPackets = 0U) override {
        return ServiceQueue(
            _controlReceiveQueue,
            _controlReadIndex,
            _controlWriteIndex,
            _controlReceiver,
            maximumPackets,
            true);
    }

    Radio::RadioIngressQueueStatistics StandardIngressStatistics() const noexcept override {
        const auto read = _readIndex.load(std::memory_order_acquire);
        const auto write = _writeIndex.load(std::memory_order_acquire);
        return {
            static_cast<std::uint64_t>(_standardAcceptedPackets.load(std::memory_order_relaxed)),
            static_cast<std::uint64_t>(_standardDroppedPackets.load(std::memory_order_relaxed)),
            QueueDepth(read, write, _receiveQueue.size()),
            _standardHighWatermark.load(std::memory_order_relaxed),
            _receiveQueue.empty() ? 0U : static_cast<std::uint32_t>(_receiveQueue.size() - 1U)
        };
    }

    Radio::RadioIngressQueueStatistics ControlIngressStatistics() const noexcept override {
        const auto read = _controlReadIndex.load(std::memory_order_acquire);
        const auto write = _controlWriteIndex.load(std::memory_order_acquire);
        return {
            static_cast<std::uint64_t>(_controlAcceptedPackets.load(std::memory_order_relaxed)),
            static_cast<std::uint64_t>(_controlDroppedPackets.load(std::memory_order_relaxed)),
            QueueDepth(read, write, _controlReceiveQueue.size()),
            _controlHighWatermark.load(std::memory_order_relaxed),
            _controlReceiveQueue.empty() ? 0U : static_cast<std::uint32_t>(_controlReceiveQueue.size() - 1U)
        };
    }

    Raw80211ReceiveTimestampStatistics StandardReceiveTimestampStatistics() const noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_timestampStatisticsMutex);
        return _lastStandardTimestampStatistics;
    }

    Raw80211ReceiveTimestampStatistics ControlReceiveTimestampStatistics() const noexcept {
        std::lock_guard<System::Synchronization::Mutex> lock(_timestampStatisticsMutex);
        return _lastControlTimestampStatistics;
    }
};

} // namespace ESPressio::ESP32Platform
