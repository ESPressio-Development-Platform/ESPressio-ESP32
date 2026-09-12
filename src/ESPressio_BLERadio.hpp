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

#include <ESPressio_IRadio.hpp>

#ifndef ESPRESSIO_ESP32_BLE_RADIO_RX_QUEUE_DEPTH
#define ESPRESSIO_ESP32_BLE_RADIO_RX_QUEUE_DEPTH 16
#endif

namespace ESPressio::ESP32Platform {

inline constexpr std::uint32_t ESP32BLERadioContentionDomain = 0x4553424CU; // "ESBL"

struct BLERadioConfiguration {
    uint16_t ManufacturerCompanyIdentifier = 0xFFFFu;
    uint16_t AdvertisingIntervalMinimum = 0x0020u;
    uint16_t AdvertisingIntervalMaximum = 0x0020u;
    uint16_t ScanInterval = 0x0050u;
    uint16_t ScanWindow = 0x0040u;
    uint32_t TransmissionDwellMilliseconds = 40u;
};

/// <summary>Managed ESP32 BLE legacy-advertising Radio provider.</summary>
/// <remarks>
/// Legacy advertisements are physically broadcast, so the target envelope carries only company-id + ESPressio marker
/// before the opaque Radio physical packet. The old redundant six-byte destination is removed: this restores a 26-byte
/// payload budget, enough for the locked RadioTransport v3 15-byte prefix plus a six-byte source address. The provider
/// therefore rejects non-broadcast destinations rather than pretending that legacy advertising provides unicast.
/// Exactly one TX campaign may be outstanding. Successful GAP setup/start followed by the configured finite dwell and
/// advertising stop establishes TransmissionCompletion without peer acknowledgement. GAP/timer callbacks only latch
/// terminal state and wake infrastructure; the Radio domain service quantum publishes the correlated completion.
/// Bluedroid scan callbacks have no characterized provider-proximate RX timestamp, so K1/K2 timestamp certification is
/// intentionally unavailable.
/// </remarks>
class BLERadio final : public Radio::IRadio {
private:
    static constexpr std::size_t AddressBytes=6;
    static constexpr uint8_t ManufacturerSpecificType=0xFFu;
    static constexpr uint8_t FrameMarker=0xE5u;
    static constexpr std::size_t AdvertisementBytesMaximum=31;
    static constexpr std::size_t ManufacturerPrefixBytes=3; // company-id + marker
    static constexpr std::size_t AdvertisingStructureBytes=2; // length + AD type
    static constexpr std::size_t MaximumPayloadBytes=
        AdvertisementBytesMaximum-AdvertisingStructureBytes-ManufacturerPrefixBytes;
    static constexpr uint16_t MaximumLogicalTransferBytes=1275; // 255 * (26 - 15 - 6)

    static_assert(MaximumPayloadBytes==26,"BLE legacy target envelope must expose 26 opaque bytes");
    static_assert(MaximumPayloadBytes>=21,"BLE legacy target envelope must carry v3 header + six-byte source");
    static_assert(ESPRESSIO_ESP32_BLE_RADIO_RX_QUEUE_DEPTH>1);
    static_assert(ESPRESSIO_ESP32_BLE_RADIO_RX_QUEUE_DEPTH<=255);

    struct ReceivedPacket final {
        Radio::RadioAddress Source{};
        uint8_t Length{0};
        int16_t RssiDbm{0};
        std::array<uint8_t,MaximumPayloadBytes> Payload{};
    };

    BLERadioConfiguration _configuration{};
    Radio::IRadioReceiver* _receiver{nullptr};
    std::atomic<Radio::IRadioRuntimeSink*> _runtimeSink{nullptr};
    Radio::RadioAddress _localAddress{};
    std::array<ReceivedPacket,ESPRESSIO_ESP32_BLE_RADIO_RX_QUEUE_DEPTH> _receiveQueue{};
    std::atomic<uint8_t> _receiveWriteIndex{0};
    std::atomic<uint8_t> _receiveReadIndex{0};
    std::atomic<std::uint32_t> _acceptedPackets{0};
    std::atomic<std::uint32_t> _droppedPackets{0};

    std::array<uint8_t,AdvertisementBytesMaximum> _activeAdvertisement{};
    esp_timer_handle_t _transmitTimer{nullptr};
    esp_ble_adv_params_t _advertisingParameters{};
    esp_ble_scan_params_t _scanParameters{};
    std::atomic<bool> _started{false};
    std::atomic<bool> _scanActive{false};
    std::atomic<bool> _advertisingActive{false};
    std::atomic<bool> _completionRequested{false};
    std::atomic<std::uint32_t> _nextTransmissionHandle{1};
    std::atomic<std::uint32_t> _pendingTransmissionHandle{0};
    std::atomic<bool> _terminalCompletionPending{false};
    std::atomic<std::uint8_t> _terminalTransmission{
        static_cast<std::uint8_t>(Radio::RadioTransmissionCompletion::Unknown)};

    bool _controllerInitializedByUs{false};
    bool _controllerEnabledByUs{false};
    bool _bluedroidInitializedByUs{false};
    bool _bluedroidEnabledByUs{false};

    static BLERadio*& CallbackInstance() noexcept {static BLERadio* instance=nullptr;return instance;}

    static void GapCallback(esp_gap_ble_cb_event_t event,esp_ble_gap_cb_param_t* parameter) {
        auto* self=CallbackInstance();
        if(self&&parameter) self->HandleGapEvent(event,*parameter);
    }
    static void TransmitTimerCallback(void* context) {
        auto* self=static_cast<BLERadio*>(context);
        if(!self||!self->_started.load(std::memory_order_acquire)) return;
        self->_completionRequested.store(true,std::memory_order_release);
        if(esp_ble_gap_stop_advertising()!=ESP_OK) self->LatchTerminal(false);
    }

    static std::uint32_t NextHandle(std::atomic<std::uint32_t>& counter) noexcept {
        auto value=counter.fetch_add(1,std::memory_order_relaxed);
        if(value==0) value=counter.fetch_add(1,std::memory_order_relaxed);
        return value==0?1:value;
    }

    void ConfigureNativeParameters() noexcept {
        std::memset(&_advertisingParameters,0,sizeof(_advertisingParameters));
        _advertisingParameters.adv_int_min=_configuration.AdvertisingIntervalMinimum;
        _advertisingParameters.adv_int_max=_configuration.AdvertisingIntervalMaximum;
        _advertisingParameters.adv_type=ADV_TYPE_NONCONN_IND;
        _advertisingParameters.own_addr_type=BLE_ADDR_TYPE_PUBLIC;
        _advertisingParameters.channel_map=ADV_CHNL_ALL;
        _advertisingParameters.adv_filter_policy=ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
        std::memset(&_scanParameters,0,sizeof(_scanParameters));
        _scanParameters.scan_type=BLE_SCAN_TYPE_PASSIVE;
        _scanParameters.own_addr_type=BLE_ADDR_TYPE_PUBLIC;
        _scanParameters.scan_filter_policy=BLE_SCAN_FILTER_ALLOW_ALL;
        _scanParameters.scan_interval=_configuration.ScanInterval;
        _scanParameters.scan_window=_configuration.ScanWindow;
        _scanParameters.scan_duplicate=BLE_SCAN_DUPLICATE_DISABLE;
    }

    bool ResolveLocalAddress() noexcept {
        uint8_t address[AddressBytes]{};
        if(esp_read_mac(address,ESP_MAC_BT)!=ESP_OK) return false;
        _localAddress=Radio::RadioAddress::FromBytes(address,static_cast<uint8_t>(AddressBytes));
        return _localAddress.IsValid();
    }

    bool EnsureBluetoothStack() noexcept {
        auto controllerStatus=esp_bt_controller_get_status();
        if(controllerStatus==ESP_BT_CONTROLLER_STATUS_IDLE){
            esp_bt_controller_config_t configuration=BT_CONTROLLER_INIT_CONFIG_DEFAULT();
            if(esp_bt_controller_init(&configuration)!=ESP_OK) return false;
            _controllerInitializedByUs=true;
            controllerStatus=esp_bt_controller_get_status();
        }
        if(controllerStatus==ESP_BT_CONTROLLER_STATUS_INITED){
            if(esp_bt_controller_enable(ESP_BT_MODE_BLE)!=ESP_OK) return false;
            _controllerEnabledByUs=true;
            controllerStatus=esp_bt_controller_get_status();
        }
        if(controllerStatus!=ESP_BT_CONTROLLER_STATUS_ENABLED) return false;
        auto bluedroidStatus=esp_bluedroid_get_status();
        if(bluedroidStatus==ESP_BLUEDROID_STATUS_UNINITIALIZED){
            if(esp_bluedroid_init()!=ESP_OK) return false;
            _bluedroidInitializedByUs=true;
            bluedroidStatus=esp_bluedroid_get_status();
        }
        if(bluedroidStatus==ESP_BLUEDROID_STATUS_INITIALIZED){
            if(esp_bluedroid_enable()!=ESP_OK) return false;
            _bluedroidEnabledByUs=true;
            bluedroidStatus=esp_bluedroid_get_status();
        }
        return bluedroidStatus==ESP_BLUEDROID_STATUS_ENABLED;
    }

    void ReleaseOwnedBluetoothStack() noexcept {
        if(_bluedroidEnabledByUs&&esp_bluedroid_get_status()==ESP_BLUEDROID_STATUS_ENABLED)(void)esp_bluedroid_disable();
        if(_bluedroidInitializedByUs&&esp_bluedroid_get_status()==ESP_BLUEDROID_STATUS_INITIALIZED)(void)esp_bluedroid_deinit();
        if(_controllerEnabledByUs&&esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_ENABLED)(void)esp_bt_controller_disable();
        if(_controllerInitializedByUs&&esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_INITED)(void)esp_bt_controller_deinit();
        _bluedroidEnabledByUs=_bluedroidInitializedByUs=_controllerEnabledByUs=_controllerInitializedByUs=false;
    }

    bool CreateTransmitTimer() noexcept {
        if(_transmitTimer) return true;
        esp_timer_create_args_t arguments{};
        arguments.callback=&BLERadio::TransmitTimerCallback;
        arguments.arg=this;
        arguments.dispatch_method=ESP_TIMER_TASK;
        arguments.name="espr_ble_tx";
        return esp_timer_create(&arguments,&_transmitTimer)==ESP_OK;
    }
    void DestroyTransmitTimer() noexcept {
        if(!_transmitTimer) return;
        (void)esp_timer_stop(_transmitTimer);
        (void)esp_timer_delete(_transmitTimer);
        _transmitTimer=nullptr;
    }

    void BuildAdvertisement(const uint8_t* payload,std::size_t payloadSize) noexcept {
        _activeAdvertisement.fill(0);
        const std::size_t manufacturerDataBytes=ManufacturerPrefixBytes+payloadSize;
        _activeAdvertisement[0]=static_cast<uint8_t>(1u+manufacturerDataBytes);
        _activeAdvertisement[1]=ManufacturerSpecificType;
        _activeAdvertisement[2]=static_cast<uint8_t>(_configuration.ManufacturerCompanyIdentifier&0xFFu);
        _activeAdvertisement[3]=static_cast<uint8_t>((_configuration.ManufacturerCompanyIdentifier>>8u)&0xFFu);
        _activeAdvertisement[4]=FrameMarker;
        if(payloadSize) std::memcpy(_activeAdvertisement.data()+5,payload,payloadSize);
    }

    std::size_t ActiveAdvertisementLength(std::size_t payloadSize) const noexcept {
        return AdvertisingStructureBytes+ManufacturerPrefixBytes+payloadSize;
    }

    void ArmTransmitDwell() noexcept {
        if(!_transmitTimer||!_started.load(std::memory_order_acquire)) return;
        (void)esp_timer_stop(_transmitTimer);
        const auto us=static_cast<uint64_t>(_configuration.TransmissionDwellMilliseconds)*1000ULL;
        if(esp_timer_start_once(_transmitTimer,us)!=ESP_OK) LatchTerminal(false);
    }

    void LatchTerminal(bool success) noexcept {
        if(_pendingTransmissionHandle.load(std::memory_order_acquire)==0) return;
        _terminalTransmission.store(static_cast<std::uint8_t>(success
            ?Radio::RadioTransmissionCompletion::Completed:Radio::RadioTransmissionCompletion::Failed),
            std::memory_order_relaxed);
        _terminalCompletionPending.store(true,std::memory_order_release);
        _completionRequested.store(false,std::memory_order_release);
        if(auto* sink=_runtimeSink.load(std::memory_order_acquire)) sink->TransmitReadinessChanged(*this);
    }

    void PublishPendingTransmissionCompletion() noexcept {
        if(!_terminalCompletionPending.exchange(false,std::memory_order_acq_rel)) return;
        const auto handleValue=_pendingTransmissionHandle.exchange(0,std::memory_order_acq_rel);
        if(handleValue==0) return;
        const auto terminal=static_cast<Radio::RadioTransmissionCompletion>(
            _terminalTransmission.load(std::memory_order_acquire));
        const auto evidence=terminal==Radio::RadioTransmissionCompletion::Completed
            ?Radio::RadioDirectLinkEvidence::CompletedWithoutPeerAcknowledgement()
            :Radio::RadioDirectLinkEvidence::Failed();
        if(auto* sink=_runtimeSink.load(std::memory_order_acquire)){
            sink->TransmissionResolved(*this,{handleValue},evidence);
            sink->TransmitReadinessChanged(*this);
        }
    }

    void QueueReceivedAdvertisement(const esp_ble_gap_cb_param_t::ble_scan_result_evt_param& scan) noexcept {
        if(scan.search_evt!=ESP_GAP_SEARCH_INQ_RES_EVT||scan.adv_data_len==0) return;
        if(std::memcmp(scan.bda,_localAddress.Bytes.data(),AddressBytes)==0) return;
        const uint8_t* data=scan.ble_adv;
        std::size_t offset=0;
        const std::size_t total=scan.adv_data_len;
        while(offset<total){
            const uint8_t fieldLength=data[offset];
            if(fieldLength==0) return;
            const std::size_t fieldEnd=offset+1u+fieldLength;
            if(fieldEnd>total||fieldLength<4u) return;
            if(data[offset+1u]==ManufacturerSpecificType){
                const uint8_t* manufacturer=data+offset+2u;
                const uint16_t company=static_cast<uint16_t>(manufacturer[0])|
                    (static_cast<uint16_t>(manufacturer[1])<<8u);
                if(company==_configuration.ManufacturerCompanyIdentifier&&manufacturer[2]==FrameMarker){
                    const std::size_t payloadSize=static_cast<std::size_t>(fieldLength)-4u;
                    if(payloadSize>MaximumPayloadBytes) return;
                    const uint8_t write=_receiveWriteIndex.load(std::memory_order_relaxed);
                    const uint8_t next=static_cast<uint8_t>((write+1u)%_receiveQueue.size());
                    if(next==_receiveReadIndex.load(std::memory_order_acquire)){
                        _droppedPackets.fetch_add(1,std::memory_order_relaxed);return;
                    }
                    auto& packet=_receiveQueue[write];
                    packet.Source=Radio::RadioAddress::FromBytes(scan.bda,static_cast<uint8_t>(AddressBytes));
                    packet.Length=static_cast<uint8_t>(payloadSize);
                    packet.RssiDbm=static_cast<int16_t>(scan.rssi);
                    if(payloadSize) std::memcpy(packet.Payload.data(),manufacturer+ManufacturerPrefixBytes,payloadSize);
                    _receiveWriteIndex.store(next,std::memory_order_release);
                    _acceptedPackets.fetch_add(1,std::memory_order_relaxed);
                    if(auto* sink=_runtimeSink.load(std::memory_order_acquire)) sink->InboundAvailable(*this);
                    return;
                }
            }
            offset=fieldEnd;
        }
    }

    void HandleGapEvent(esp_gap_ble_cb_event_t event,esp_ble_gap_cb_param_t& parameter) noexcept {
        switch(event){
            case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
                if(_started.load(std::memory_order_acquire)&&parameter.scan_param_cmpl.status==ESP_BT_STATUS_SUCCESS)
                    (void)esp_ble_gap_start_scanning(0);
                break;
            case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
                _scanActive.store(parameter.scan_start_cmpl.status==ESP_BT_STATUS_SUCCESS,std::memory_order_release);
                if(auto* sink=_runtimeSink.load(std::memory_order_acquire)) sink->LifecycleAvailabilityChanged(*this);
                break;
            case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
                _scanActive.store(false,std::memory_order_release);break;
            case ESP_GAP_BLE_SCAN_RESULT_EVT:
                if(_started.load(std::memory_order_acquire)) QueueReceivedAdvertisement(parameter.scan_rst);break;
            case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
                if(parameter.adv_data_raw_cmpl.status!=ESP_BT_STATUS_SUCCESS){LatchTerminal(false);break;}
                if(esp_ble_gap_start_advertising(&_advertisingParameters)!=ESP_OK) LatchTerminal(false);
                break;
            case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
                if(parameter.adv_start_cmpl.status==ESP_BT_STATUS_SUCCESS){
                    _advertisingActive.store(true,std::memory_order_release);ArmTransmitDwell();
                }else{_advertisingActive.store(false,std::memory_order_release);LatchTerminal(false);}
                break;
            case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
                _advertisingActive.store(false,std::memory_order_release);
                if(_completionRequested.load(std::memory_order_acquire))
                    LatchTerminal(parameter.adv_stop_cmpl.status==ESP_BT_STATUS_SUCCESS);
                break;
            default: break;
        }
    }

public:
    explicit BLERadio(BLERadioConfiguration configuration={}) noexcept:_configuration(configuration) {}
    BLERadio(const BLERadio&)=delete;
    BLERadio& operator=(const BLERadio&)=delete;

    bool Start() override {
        if(_started.load(std::memory_order_acquire)) return true;
        if(CallbackInstance()!=nullptr&&CallbackInstance()!=this) return false;
        if(_configuration.ScanWindow==0||_configuration.ScanInterval==0||
           _configuration.ScanWindow>_configuration.ScanInterval||_configuration.TransmissionDwellMilliseconds==0)
            return false;
        if(!ResolveLocalAddress()||!EnsureBluetoothStack()||!CreateTransmitTimer()){
            DestroyTransmitTimer();ReleaseOwnedBluetoothStack();return false;
        }
        ConfigureNativeParameters();
        _receiveReadIndex.store(0,std::memory_order_relaxed);
        _receiveWriteIndex.store(0,std::memory_order_relaxed);
        _pendingTransmissionHandle.store(0,std::memory_order_relaxed);
        _terminalCompletionPending.store(false,std::memory_order_relaxed);
        _completionRequested.store(false,std::memory_order_relaxed);
        _scanActive.store(false,std::memory_order_relaxed);
        _advertisingActive.store(false,std::memory_order_relaxed);
        CallbackInstance()=this;
        if(esp_ble_gap_register_callback(&BLERadio::GapCallback)!=ESP_OK){
            CallbackInstance()=nullptr;DestroyTransmitTimer();ReleaseOwnedBluetoothStack();return false;
        }
        _started.store(true,std::memory_order_release);
        if(esp_ble_gap_set_scan_params(&_scanParameters)!=ESP_OK){Stop();return false;}
        if(auto* sink=_runtimeSink.load(std::memory_order_acquire)) sink->LifecycleAvailabilityChanged(*this);
        return true;
    }

    void Stop() noexcept override {
        if(!_started.exchange(false,std::memory_order_acq_rel)) return;
        if(_transmitTimer)(void)esp_timer_stop(_transmitTimer);
        (void)esp_ble_gap_stop_scanning();
        (void)esp_ble_gap_stop_advertising();
        _scanActive.store(false,std::memory_order_release);
        _advertisingActive.store(false,std::memory_order_release);
        _completionRequested.store(false,std::memory_order_release);
        _terminalCompletionPending.store(false,std::memory_order_release);
        _pendingTransmissionHandle.store(0,std::memory_order_release);
        _receiveReadIndex.store(_receiveWriteIndex.load(std::memory_order_acquire),std::memory_order_release);
        if(CallbackInstance()==this) CallbackInstance()=nullptr;
        DestroyTransmitTimer();ReleaseOwnedBluetoothStack();
        if(auto* sink=_runtimeSink.load(std::memory_order_acquire)) sink->LifecycleAvailabilityChanged(*this);
    }

    bool IsStarted() const noexcept override {return _started.load(std::memory_order_acquire);}
    Radio::RadioCapabilities Capabilities() const noexcept override {
        return {Radio::RadioCapability::Broadcast|Radio::RadioCapability::Rssi|Radio::RadioCapability::HardwareAddressing,
                static_cast<uint16_t>(MaximumPayloadBytes),static_cast<uint8_t>(AddressBytes),MaximumLogicalTransferBytes};
    }
    Radio::RadioAddress LocalAddress() const noexcept override {return _localAddress;}
    Radio::RadioContentionDomainId ContentionDomain() const noexcept override {return {ESP32BLERadioContentionDomain};}
    Radio::RadioProviderResourceProfile ProviderResources() const noexcept override {
        return {static_cast<std::uint16_t>(ESPRESSIO_ESP32_BLE_RADIO_RX_QUEUE_DEPTH-1),
                static_cast<std::uint16_t>(ESPRESSIO_ESP32_BLE_RADIO_RX_QUEUE_DEPTH-1),1,0};
    }
    bool IsTransmitReady() const noexcept override {
        return IsStarted()&&_pendingTransmissionHandle.load(std::memory_order_acquire)==0;
    }
    Radio::RadioTransmissionCost EstimateTransmissionCost(
        const Radio::RadioAddress&,std::size_t payloadBytes,const Radio::RadioServiceProfile&) const noexcept override {
        const auto fairness=static_cast<std::uint64_t>(_configuration.TransmissionDwellMilliseconds)*1000ULL+payloadBytes;
        return {fairness?fairness:1,0,Radio::RadioCostEstimateQuality::RelativeOnly};
    }

    Radio::RadioSendResult Send(
        const Radio::RadioAddress& destination,const uint8_t* payload,std::size_t payloadSize) noexcept override {
        if(!IsStarted()) return {Radio::RadioSendStatus::NotStarted,0};
        if(!destination.IsValid()||destination.Length!=AddressBytes||!destination.IsBroadcast())
            return {Radio::RadioSendStatus::InvalidAddress,0};
        if((!payload&&payloadSize!=0)||payloadSize>MaximumPayloadBytes)
            return {Radio::RadioSendStatus::PayloadTooLarge,0};
        if(_pendingTransmissionHandle.load(std::memory_order_acquire)!=0)
            return {Radio::RadioSendStatus::Busy,0};
        const auto handleValue=NextHandle(_nextTransmissionHandle);
        BuildAdvertisement(payload,payloadSize);
        _terminalCompletionPending.store(false,std::memory_order_release);
        _completionRequested.store(false,std::memory_order_release);
        _pendingTransmissionHandle.store(handleValue,std::memory_order_release);
        const auto result=esp_ble_gap_config_adv_data_raw(
            _activeAdvertisement.data(),static_cast<uint32_t>(ActiveAdvertisementLength(payloadSize)));
        if(result!=ESP_OK){
            std::uint32_t expected=handleValue;
            (void)_pendingTransmissionHandle.compare_exchange_strong(expected,0,std::memory_order_acq_rel);
            return {Radio::RadioSendStatus::NativeFailure,static_cast<int32_t>(result)};
        }
        return Radio::RadioSendResult::Accepted({},Radio::RadioTransmissionHandle{handleValue});
    }

    void SetReceiver(Radio::IRadioReceiver* receiver) noexcept override {_receiver=receiver;}
    void SetRuntimeSink(Radio::IRadioRuntimeSink* sink) noexcept override {_runtimeSink.store(sink,std::memory_order_release);}

    Radio::ManagedRadioIngressServiceResult ServiceInbound(std::size_t maximumPackets=0) noexcept override {
        PublishPendingTransmissionCompletion();
        const auto initialRead=_receiveReadIndex.load(std::memory_order_relaxed);
        const auto initialWrite=_receiveWriteIndex.load(std::memory_order_acquire);
        const auto depth=initialWrite>=initialRead?static_cast<std::size_t>(initialWrite-initialRead)
            :_receiveQueue.size()-static_cast<std::size_t>(initialRead-initialWrite);
        if(depth==0) return {};
        const auto requested=maximumPackets==0?_receiveQueue.size()-1:maximumPackets;
        const auto count=depth<requested?depth:requested;
        auto read=initialRead;
        for(std::size_t i=0;i<count;++i){
            const auto& packet=_receiveQueue[read];
            const auto broadcast=Radio::RadioAddress::Broadcast(static_cast<uint8_t>(AddressBytes));
            Radio::RadioPacketView view{packet.Source,broadcast,packet.Length?packet.Payload.data():nullptr,
                packet.Length,packet.RssiDbm,0,Radio::RadioPacketFlag::Broadcast};
            const Radio::RadioReceiveTimestampEvidence timestamp{};
            if(_receiver) _receiver->OnRadioPacket(*this,view,timestamp);
            read=static_cast<uint8_t>((read+1u)%_receiveQueue.size());
            _receiveReadIndex.store(read,std::memory_order_release);
        }
        return {static_cast<std::uint32_t>(count),read!=_receiveWriteIndex.load(std::memory_order_acquire)};
    }

    std::uint64_t AcceptedIngressPackets() const noexcept {return _acceptedPackets.load(std::memory_order_relaxed);}
    std::uint64_t DroppedIngressPackets() const noexcept {return _droppedPackets.load(std::memory_order_relaxed);}
};

} // namespace ESPressio::ESP32Platform

#endif
