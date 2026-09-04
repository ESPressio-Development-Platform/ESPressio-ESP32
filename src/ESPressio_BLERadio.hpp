#pragma once

#if !defined(ARDUINO_ARCH_ESP32)
#error "ESPressio_BLERadio.hpp requires an ESP32 Arduino target"
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sdkconfig.h>

#if defined(CONFIG_BT_BLE_ENABLED) && CONFIG_BT_BLE_ENABLED && \
    defined(CONFIG_BT_BLUEDROID_ENABLED) && CONFIG_BT_BLUEDROID_ENABLED

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_err.h>
#include <esp_gap_ble_api.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include <ESPressio_IRadio.hpp>

#ifndef ESPRESSIO_ESP32_BLE_RADIO_RX_QUEUE_DEPTH
#define ESPRESSIO_ESP32_BLE_RADIO_RX_QUEUE_DEPTH 16
#endif

#ifndef ESPRESSIO_ESP32_BLE_RADIO_TX_QUEUE_DEPTH
// A ring keeps one slot empty, so 65 slots permit 64 queued physical packets.
#define ESPRESSIO_ESP32_BLE_RADIO_TX_QUEUE_DEPTH 65
#endif

namespace ESPressio::ESP32Platform {

/// <summary>Configuration for the ESP32 integrated Bluetooth Low Energy ESPressio Radio concrete.</summary>
/// <remarks>
/// The baseline bearer uses legacy non-connectable advertising and passive scanning so it remains applicable to
/// original ESP32-class BLE hardware. ManufacturerCompanyIdentifier identifies the manufacturer-specific advertising
/// namespace used by all peers in the same Radio deployment. The default 0xFFFF value is the Bluetooth SIG testing
/// identifier and should be replaced with an appropriately assigned identifier for production products.
/// </remarks>
struct BLERadioConfiguration {
    /// <summary>Bluetooth SIG company identifier carried by the manufacturer-specific ESPressio Radio advertisement.</summary>
    uint16_t ManufacturerCompanyIdentifier = 0xFFFFu;
    /// <summary>Minimum BLE advertising interval in 0.625 ms units.</summary>
    uint16_t AdvertisingIntervalMinimum = 0x0020u;
    /// <summary>Maximum BLE advertising interval in 0.625 ms units.</summary>
    uint16_t AdvertisingIntervalMaximum = 0x0020u;
    /// <summary>Passive scan interval in 0.625 ms units.</summary>
    uint16_t ScanInterval = 0x0050u;
    /// <summary>Passive scan window in 0.625 ms units; must not exceed ScanInterval.</summary>
    uint16_t ScanWindow = 0x0040u;
    /// <summary>
    /// Minimum time for which one queued physical Radio packet remains the active advertisement before advancing to
    /// the next packet. It should comfortably exceed one configured advertising interval plus BLE advertising delay.
    /// </summary>
    uint32_t TransmissionDwellMilliseconds = 40u;
};

/// <summary>
/// ESPressio Radio concrete implemented over ESP32 Bluetooth Low Energy legacy advertising/scanning.
/// </summary>
/// <remarks>
/// BLE is used strictly as an opaque Radio bearer. This type does not implement Bluetooth Mesh, ESPressio-Mesh routing,
/// admission, identities, primitive semantics, authentication, or serialization.
///
/// Receive callbacks perform only bounded frame recognition/copying into provider-owned storage and wake RadioWorker;
/// packet delivery and observer notification happen later from DrainInbound().
///
/// The ESP-IDF Bluedroid GAP callback is process-global. Consequently one BLERadio instance owns the GAP callback while
/// started and must be the application component coordinating BLE GAP use. GATT profiles may coexist, but an independent
/// component that replaces the global GAP callback would conflict with this provider.
///
/// This initial implementation intentionally does not advertise RadioCapability::ReceiveTimestamp. The Bluedroid GAP
/// scan callback does not expose a demonstrated near-RF receive timestamp, so using its callback time as a precision
/// clock-synchronization observation would overstate timing quality. A timestamp capability should only be added after
/// controller/driver-level characterization proves sufficient uncertainty margin for the required sub-millisecond sync.
/// </remarks>
class BLERadio final : public Radio::IRadio {
private:
    static constexpr std::size_t AddressBytes = 6;
    static constexpr uint8_t ManufacturerSpecificType = 0xFFu;
    static constexpr uint8_t FrameMarker = 0xE5u;
    static constexpr std::size_t AdvertisementBytesMaximum = 31;
    static constexpr std::size_t ManufacturerPrefixBytes = 3;
    static constexpr std::size_t LinkEnvelopeBytes = ManufacturerPrefixBytes + AddressBytes;
    static constexpr std::size_t AdvertisingStructureBytes = 2;
    static constexpr std::size_t MaximumPayloadBytes = AdvertisementBytesMaximum - AdvertisingStructureBytes - LinkEnvelopeBytes;
    static constexpr uint16_t MaximumLogicalTransferBytes = 256;

    static_assert(MaximumPayloadBytes == 20, "BLE legacy advertising payload calculation changed unexpectedly");
    static_assert(ESPRESSIO_ESP32_BLE_RADIO_RX_QUEUE_DEPTH > 1, "BLE RX queue depth must be at least two");
    static_assert(ESPRESSIO_ESP32_BLE_RADIO_RX_QUEUE_DEPTH <= 255, "BLE RX queue depth must fit its indices");
    static_assert(ESPRESSIO_ESP32_BLE_RADIO_TX_QUEUE_DEPTH > 1, "BLE TX queue depth must be at least two");
    static_assert(ESPRESSIO_ESP32_BLE_RADIO_TX_QUEUE_DEPTH <= 255, "BLE TX queue depth must fit its indices");

    struct ReceivedPacket {
        Radio::RadioAddress Source{};
        Radio::RadioAddress Destination{};
        uint8_t Length = 0;
        int16_t RssiDbm = 0;
        std::array<uint8_t, MaximumPayloadBytes> Payload{};
    };

    struct TransmitPacket {
        Radio::RadioAddress Destination{};
        uint8_t Length = 0;
        std::array<uint8_t, MaximumPayloadBytes> Payload{};
    };

    BLERadioConfiguration _configuration{};
    Radio::IRadioReceiver* _receiver = nullptr;
    std::atomic<Radio::IRadioWorkSignal*> _workSignal{nullptr};
    Radio::RadioObserverSubscriptions _observers{};
    Radio::RadioAddress _localAddress{};
    std::array<ReceivedPacket, ESPRESSIO_ESP32_BLE_RADIO_RX_QUEUE_DEPTH> _receiveQueue{};
    std::atomic<uint8_t> _receiveWriteIndex{0};
    std::atomic<uint8_t> _receiveReadIndex{0};
    std::array<TransmitPacket, ESPRESSIO_ESP32_BLE_RADIO_TX_QUEUE_DEPTH> _transmitQueue{};
    uint8_t _transmitWriteIndex = 0;
    uint8_t _transmitReadIndex = 0;
    portMUX_TYPE _transmitMux = portMUX_INITIALIZER_UNLOCKED;
    std::array<uint8_t, AdvertisementBytesMaximum> _activeAdvertisement{};
    esp_timer_handle_t _transmitTimer = nullptr;
    esp_ble_adv_params_t _advertisingParameters{};
    esp_ble_scan_params_t _scanParameters{};
    std::atomic<bool> _started{false};
    std::atomic<bool> _scanActive{false};
    std::atomic<bool> _advertisingActive{false};
    std::atomic<bool> _transmitCycleActive{false};
    bool _controllerInitializedByUs = false;
    bool _controllerEnabledByUs = false;
    bool _bluedroidInitializedByUs = false;
    bool _bluedroidEnabledByUs = false;

    static BLERadio*& CallbackInstance() noexcept { static BLERadio* instance = nullptr; return instance; }

    static bool IsBroadcastAddress(const uint8_t* address) noexcept {
        if (address == nullptr) return false;
        for (std::size_t i = 0; i < AddressBytes; ++i) if (address[i] != 0xFFu) return false;
        return true;
    }

    static void GapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* parameter) {
        BLERadio* self = CallbackInstance();
        if (self == nullptr || parameter == nullptr) return;
        self->HandleGapEvent(event, *parameter);
    }

    static void TransmitTimerCallback(void* context) {
        auto* self = static_cast<BLERadio*>(context);
        if (self == nullptr || !self->_started.load(std::memory_order_acquire)) return;
        self->AdvanceTransmitQueue();
    }

    void ConfigureNativeParameters() noexcept {
        std::memset(&_advertisingParameters, 0, sizeof(_advertisingParameters));
        _advertisingParameters.adv_int_min = _configuration.AdvertisingIntervalMinimum;
        _advertisingParameters.adv_int_max = _configuration.AdvertisingIntervalMaximum;
        _advertisingParameters.adv_type = ADV_TYPE_NONCONN_IND;
        _advertisingParameters.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
        _advertisingParameters.channel_map = ADV_CHNL_ALL;
        _advertisingParameters.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
        std::memset(&_scanParameters, 0, sizeof(_scanParameters));
        _scanParameters.scan_type = BLE_SCAN_TYPE_PASSIVE;
        _scanParameters.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
        _scanParameters.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
        _scanParameters.scan_interval = _configuration.ScanInterval;
        _scanParameters.scan_window = _configuration.ScanWindow;
        _scanParameters.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE;
    }

    bool ResolveLocalAddress() noexcept {
        uint8_t address[AddressBytes]{};
        if (esp_read_mac(address, ESP_MAC_BT) != ESP_OK) return false;
        _localAddress = Radio::RadioAddress::FromBytes(address, static_cast<uint8_t>(AddressBytes));
        return _localAddress.IsValid();
    }

    bool EnsureBluetoothStack() noexcept {
        esp_bt_controller_status_t controllerStatus = esp_bt_controller_get_status();
        if (controllerStatus == ESP_BT_CONTROLLER_STATUS_IDLE) {
            esp_bt_controller_config_t controllerConfiguration = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
            if (esp_bt_controller_init(&controllerConfiguration) != ESP_OK) return false;
            _controllerInitializedByUs = true;
            controllerStatus = esp_bt_controller_get_status();
        }
        if (controllerStatus == ESP_BT_CONTROLLER_STATUS_INITED) {
            if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) return false;
            _controllerEnabledByUs = true;
            controllerStatus = esp_bt_controller_get_status();
        }
        if (controllerStatus != ESP_BT_CONTROLLER_STATUS_ENABLED) return false;
        esp_bluedroid_status_t bluedroidStatus = esp_bluedroid_get_status();
        if (bluedroidStatus == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
            if (esp_bluedroid_init() != ESP_OK) return false;
            _bluedroidInitializedByUs = true;
            bluedroidStatus = esp_bluedroid_get_status();
        }
        if (bluedroidStatus == ESP_BLUEDROID_STATUS_INITIALIZED) {
            if (esp_bluedroid_enable() != ESP_OK) return false;
            _bluedroidEnabledByUs = true;
            bluedroidStatus = esp_bluedroid_get_status();
        }
        return bluedroidStatus == ESP_BLUEDROID_STATUS_ENABLED;
    }

    void ReleaseOwnedBluetoothStack() noexcept {
        if (_bluedroidEnabledByUs && esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) (void)esp_bluedroid_disable();
        if (_bluedroidInitializedByUs && esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) (void)esp_bluedroid_deinit();
        if (_controllerEnabledByUs && esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) (void)esp_bt_controller_disable();
        if (_controllerInitializedByUs && esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) (void)esp_bt_controller_deinit();
        _bluedroidEnabledByUs = false;
        _bluedroidInitializedByUs = false;
        _controllerEnabledByUs = false;
        _controllerInitializedByUs = false;
    }

    bool CreateTransmitTimer() noexcept {
        if (_transmitTimer != nullptr) return true;
        esp_timer_create_args_t arguments{};
        arguments.callback = &BLERadio::TransmitTimerCallback;
        arguments.arg = this;
        arguments.dispatch_method = ESP_TIMER_TASK;
        arguments.name = "espr_ble_tx";
        return esp_timer_create(&arguments, &_transmitTimer) == ESP_OK;
    }

    void DestroyTransmitTimer() noexcept {
        if (_transmitTimer == nullptr) return;
        (void)esp_timer_stop(_transmitTimer);
        (void)esp_timer_delete(_transmitTimer);
        _transmitTimer = nullptr;
    }

    bool EnqueueTransmit(const Radio::RadioAddress& destination, const uint8_t* payload, std::size_t payloadSize) noexcept {
        bool accepted = false;
        portENTER_CRITICAL(&_transmitMux);
        const uint8_t next = static_cast<uint8_t>((_transmitWriteIndex + 1u) % ESPRESSIO_ESP32_BLE_RADIO_TX_QUEUE_DEPTH);
        if (next != _transmitReadIndex) {
            auto& packet = _transmitQueue[_transmitWriteIndex];
            packet.Destination = destination;
            packet.Length = static_cast<uint8_t>(payloadSize);
            if (payloadSize != 0) std::memcpy(packet.Payload.data(), payload, payloadSize);
            _transmitWriteIndex = next;
            accepted = true;
        }
        portEXIT_CRITICAL(&_transmitMux);
        return accepted;
    }

    bool DequeueTransmit(TransmitPacket& packet) noexcept {
        bool available = false;
        portENTER_CRITICAL(&_transmitMux);
        if (_transmitReadIndex != _transmitWriteIndex) {
            packet = _transmitQueue[_transmitReadIndex];
            _transmitReadIndex = static_cast<uint8_t>((_transmitReadIndex + 1u) % ESPRESSIO_ESP32_BLE_RADIO_TX_QUEUE_DEPTH);
            available = true;
        }
        portEXIT_CRITICAL(&_transmitMux);
        return available;
    }

    void ClearTransmitQueue() noexcept {
        portENTER_CRITICAL(&_transmitMux);
        _transmitReadIndex = 0;
        _transmitWriteIndex = 0;
        portEXIT_CRITICAL(&_transmitMux);
    }

    void BuildAdvertisement(const TransmitPacket& packet) noexcept {
        _activeAdvertisement.fill(0);
        const std::size_t manufacturerDataBytes = ManufacturerPrefixBytes + AddressBytes + packet.Length;
        _activeAdvertisement[0] = static_cast<uint8_t>(1u + manufacturerDataBytes);
        _activeAdvertisement[1] = ManufacturerSpecificType;
        _activeAdvertisement[2] = static_cast<uint8_t>(_configuration.ManufacturerCompanyIdentifier & 0xFFu);
        _activeAdvertisement[3] = static_cast<uint8_t>((_configuration.ManufacturerCompanyIdentifier >> 8u) & 0xFFu);
        _activeAdvertisement[4] = FrameMarker;
        std::memcpy(_activeAdvertisement.data() + 5, packet.Destination.Bytes.data(), AddressBytes);
        if (packet.Length != 0) std::memcpy(_activeAdvertisement.data() + 5 + AddressBytes, packet.Payload.data(), packet.Length);
    }

    std::size_t ActiveAdvertisementLength(const TransmitPacket& packet) const noexcept {
        return AdvertisingStructureBytes + ManufacturerPrefixBytes + AddressBytes + packet.Length;
    }

    void ArmTransmitDwell() noexcept {
        if (_transmitTimer == nullptr || !_started.load(std::memory_order_acquire)) return;
        (void)esp_timer_stop(_transmitTimer);
        const uint64_t dwellMicroseconds = static_cast<uint64_t>(_configuration.TransmissionDwellMilliseconds) * 1000ULL;
        if (esp_timer_start_once(_transmitTimer, dwellMicroseconds) != ESP_OK) AdvanceTransmitQueue();
    }

    void AdvanceTransmitQueue() noexcept {
        if (!_started.load(std::memory_order_acquire)) return;
        TransmitPacket packet;
        while (DequeueTransmit(packet)) {
            BuildAdvertisement(packet);
            const esp_err_t result = esp_ble_gap_config_adv_data_raw(_activeAdvertisement.data(), static_cast<uint32_t>(ActiveAdvertisementLength(packet)));
            if (result == ESP_OK) return;
        }
        if (_advertisingActive.load(std::memory_order_acquire)) {
            if (esp_ble_gap_stop_advertising() == ESP_OK) return;
            _advertisingActive.store(false, std::memory_order_release);
        }
        _transmitCycleActive.store(false, std::memory_order_release);
        bool expected = false;
        portENTER_CRITICAL(&_transmitMux);
        const bool hasQueuedWork = _transmitReadIndex != _transmitWriteIndex;
        portEXIT_CRITICAL(&_transmitMux);
        if (hasQueuedWork && _transmitCycleActive.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) AdvanceTransmitQueue();
    }

    void QueueReceivedAdvertisement(const esp_ble_gap_cb_param_t::ble_scan_result_evt_param& scan) noexcept {
        if (scan.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT || scan.adv_data_len == 0) return;
        if (std::memcmp(scan.bda, _localAddress.Bytes.data(), AddressBytes) == 0) return;
        const uint8_t* data = scan.ble_adv;
        std::size_t offset = 0;
        const std::size_t total = scan.adv_data_len;
        while (offset < total) {
            const uint8_t fieldLength = data[offset];
            if (fieldLength == 0) return;
            const std::size_t fieldEnd = offset + 1u + fieldLength;
            if (fieldEnd > total || fieldLength < 10u) return;
            if (data[offset + 1u] == ManufacturerSpecificType) {
                const uint8_t* manufacturer = data + offset + 2u;
                const uint16_t companyIdentifier = static_cast<uint16_t>(manufacturer[0]) | (static_cast<uint16_t>(manufacturer[1]) << 8u);
                if (companyIdentifier == _configuration.ManufacturerCompanyIdentifier && manufacturer[2] == FrameMarker) {
                    const uint8_t* destination = manufacturer + ManufacturerPrefixBytes;
                    if (std::memcmp(destination, _localAddress.Bytes.data(), AddressBytes) != 0 && !IsBroadcastAddress(destination)) return;
                    const std::size_t payloadSize = static_cast<std::size_t>(fieldLength) - 10u;
                    if (payloadSize > MaximumPayloadBytes) return;
                    const uint8_t write = _receiveWriteIndex.load(std::memory_order_relaxed);
                    const uint8_t next = static_cast<uint8_t>((write + 1u) % ESPRESSIO_ESP32_BLE_RADIO_RX_QUEUE_DEPTH);
                    if (next == _receiveReadIndex.load(std::memory_order_acquire)) return;
                    auto& packet = _receiveQueue[write];
                    packet.Source = Radio::RadioAddress::FromBytes(scan.bda, static_cast<uint8_t>(AddressBytes));
                    packet.Destination = Radio::RadioAddress::FromBytes(destination, static_cast<uint8_t>(AddressBytes));
                    packet.Length = static_cast<uint8_t>(payloadSize);
                    packet.RssiDbm = static_cast<int16_t>(scan.rssi);
                    if (payloadSize != 0) std::memcpy(packet.Payload.data(), manufacturer + ManufacturerPrefixBytes + AddressBytes, payloadSize);
                    _receiveWriteIndex.store(next, std::memory_order_release);
                    Radio::IRadioWorkSignal* signal = _workSignal.load(std::memory_order_acquire);
                    if (signal != nullptr) signal->OnRadioWorkAvailable(*this);
                    return;
                }
            }
            offset = fieldEnd;
        }
    }

    void HandleGapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t& parameter) noexcept {
        switch (event) {
            case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
                if (_started.load(std::memory_order_acquire) && parameter.scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS && esp_ble_gap_start_scanning(0) == ESP_OK) {}
                break;
            case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
                _scanActive.store(parameter.scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS, std::memory_order_release);
                break;
            case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
                _scanActive.store(false, std::memory_order_release);
                break;
            case ESP_GAP_BLE_SCAN_RESULT_EVT:
                if (_started.load(std::memory_order_acquire)) QueueReceivedAdvertisement(parameter.scan_rst);
                break;
            case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
                if (parameter.adv_data_raw_cmpl.status != ESP_BT_STATUS_SUCCESS) { AdvanceTransmitQueue(); break; }
                if (_advertisingActive.load(std::memory_order_acquire)) ArmTransmitDwell();
                else if (esp_ble_gap_start_advertising(&_advertisingParameters) != ESP_OK) AdvanceTransmitQueue();
                break;
            case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
                if (parameter.adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                    _advertisingActive.store(true, std::memory_order_release);
                    ArmTransmitDwell();
                } else {
                    _advertisingActive.store(false, std::memory_order_release);
                    AdvanceTransmitQueue();
                }
                break;
            case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
                _advertisingActive.store(false, std::memory_order_release);
                if (_started.load(std::memory_order_acquire)) AdvanceTransmitQueue();
                else _transmitCycleActive.store(false, std::memory_order_release);
                break;
            default: break;
        }
    }

public:
    /// <summary>Creates a BLE Radio with legacy-advertising/scanning configuration.</summary>
    explicit BLERadio(BLERadioConfiguration configuration = {}) noexcept : _configuration(configuration) {}
    BLERadio(const BLERadio&) = delete;
    BLERadio& operator=(const BLERadio&) = delete;

    /// <summary>Starts BLE scanning and prepares bounded asynchronous advertising transmission.</summary>
    bool Start() override {
        if (_started.load(std::memory_order_acquire)) return true;
        if (CallbackInstance() != nullptr && CallbackInstance() != this) return false;
        if (_configuration.ScanWindow == 0 || _configuration.ScanInterval == 0 || _configuration.ScanWindow > _configuration.ScanInterval || _configuration.TransmissionDwellMilliseconds == 0) return false;
        if (!ResolveLocalAddress()) return false;
        if (!EnsureBluetoothStack()) { ReleaseOwnedBluetoothStack(); return false; }
        if (!CreateTransmitTimer()) { ReleaseOwnedBluetoothStack(); return false; }
        ConfigureNativeParameters();
        _receiveReadIndex.store(0, std::memory_order_relaxed);
        _receiveWriteIndex.store(0, std::memory_order_relaxed);
        ClearTransmitQueue();
        _scanActive.store(false, std::memory_order_relaxed);
        _advertisingActive.store(false, std::memory_order_relaxed);
        _transmitCycleActive.store(false, std::memory_order_relaxed);
        CallbackInstance() = this;
        if (esp_ble_gap_register_callback(&BLERadio::GapCallback) != ESP_OK) {
            CallbackInstance() = nullptr; DestroyTransmitTimer(); ReleaseOwnedBluetoothStack(); return false;
        }
        _started.store(true, std::memory_order_release);
        if (esp_ble_gap_set_scan_params(&_scanParameters) != ESP_OK) {
            _started.store(false, std::memory_order_release); CallbackInstance() = nullptr; DestroyTransmitTimer(); ReleaseOwnedBluetoothStack(); return false;
        }
        _observers.NotifyStarted(*this);
        return true;
    }

    /// <summary>Stops scanning/advertising and releases Bluetooth stack state created by this provider.</summary>
    void Stop() noexcept override {
        if (!_started.exchange(false, std::memory_order_acq_rel)) return;
        if (_transmitTimer != nullptr) (void)esp_timer_stop(_transmitTimer);
        (void)esp_ble_gap_stop_scanning();
        (void)esp_ble_gap_stop_advertising();
        _scanActive.store(false, std::memory_order_release);
        _advertisingActive.store(false, std::memory_order_release);
        _transmitCycleActive.store(false, std::memory_order_release);
        ClearTransmitQueue();
        _receiveReadIndex.store(_receiveWriteIndex.load(std::memory_order_acquire), std::memory_order_release);
        if (CallbackInstance() == this) CallbackInstance() = nullptr;
        DestroyTransmitTimer();
        ReleaseOwnedBluetoothStack();
        _observers.NotifyStopped(*this);
    }

    bool IsStarted() const noexcept override { return _started.load(std::memory_order_acquire); }

    Radio::RadioCapabilities Capabilities() const noexcept override {
        return {Radio::RadioCapability::Broadcast | Radio::RadioCapability::Rssi | Radio::RadioCapability::HardwareAddressing,
                static_cast<uint16_t>(MaximumPayloadBytes), static_cast<uint8_t>(AddressBytes), MaximumLogicalTransferBytes};
    }

    Radio::RadioAddress LocalAddress() const noexcept override { return _localAddress; }

    /// <summary>Queues one bounded opaque Radio packet for repeated legacy BLE advertising before the next queued packet is sent.</summary>
    Radio::RadioSendResult Send(const Radio::RadioAddress& destination, const uint8_t* payload, std::size_t payloadSize) override {
        const auto complete = [&](Radio::RadioSendResult result) {
            _observers.NotifySendAttempted(*this, destination, payloadSize, result);
            return result;
        };
        if (!IsStarted()) return complete({Radio::RadioSendStatus::NotStarted, 0});
        if (!destination.IsValid() || destination.Length != AddressBytes) return complete({Radio::RadioSendStatus::InvalidAddress, 0});
        if ((payload == nullptr && payloadSize != 0) || payloadSize > MaximumPayloadBytes) return complete({Radio::RadioSendStatus::PayloadTooLarge, 0});
        if (!EnqueueTransmit(destination, payload, payloadSize)) return complete({Radio::RadioSendStatus::Busy, 0});
        bool expected = false;
        if (_transmitCycleActive.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) AdvanceTransmitQueue();
        return complete(Radio::RadioSendResult::Accepted());
    }

    void SetReceiver(Radio::IRadioReceiver* receiver) noexcept override { _receiver = receiver; }
    void SetWorkSignal(Radio::IRadioWorkSignal* signal) noexcept override { _workSignal.store(signal, std::memory_order_release); }
    Radio::RadioObserverSubscriptions& Observers() noexcept override { return _observers; }

    void DrainInbound() override {
        while (true) {
            const uint8_t read = _receiveReadIndex.load(std::memory_order_relaxed);
            if (read == _receiveWriteIndex.load(std::memory_order_acquire)) return;
            const auto& packet = _receiveQueue[read];
            Radio::RadioPacketView view;
            view.Source = packet.Source;
            view.Destination = packet.Destination;
            view.Payload = packet.Length == 0 ? nullptr : packet.Payload.data();
            view.PayloadSize = packet.Length;
            view.RssiDbm = packet.RssiDbm;
            view.ReceiveTimestampNanoseconds = 0;
            view.Flags = packet.Destination.IsBroadcast() ? Radio::RadioPacketFlag::Broadcast : Radio::RadioPacketFlag::None;
            _observers.NotifyPacketReceived(*this, view);
            if (_receiver != nullptr) _receiver->OnRadioPacket(*this, view);
            _receiveReadIndex.store(static_cast<uint8_t>((read + 1u) % ESPRESSIO_ESP32_BLE_RADIO_RX_QUEUE_DEPTH), std::memory_order_release);
        }
    }
};

} // namespace ESPressio::ESP32Platform

#endif // CONFIG_BT_BLE_ENABLED && CONFIG_BT_BLUEDROID_ENABLED
