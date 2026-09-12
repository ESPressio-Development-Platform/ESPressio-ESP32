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

#include <WiFi.h>
#include <esp_err.h>
#include <esp_wifi.h>

#include <ESPressio_IRadio.hpp>
#include <ESPressio_SystemPlatformClock.hpp>

#include "ESPressio_Raw80211WiFiBootstrap.hpp"
#include "ESPressio_WiFiPhyCoordinator.hpp"

#ifndef ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH
#define ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH 8
#endif

#ifndef ESPRESSIO_ESP32_RAW_RADIO_LEAN_WIFI_BOOTSTRAP
#define ESPRESSIO_ESP32_RAW_RADIO_LEAN_WIFI_BOOTSTRAP 0
#endif

namespace ESPressio::ESP32Platform {

inline constexpr bool Raw80211ReceiveTimestampUsesSystemMonotonic = true;
inline constexpr bool Raw80211LeanWiFiBootstrapEnabled =
    ESPRESSIO_ESP32_RAW_RADIO_LEAN_WIFI_BOOTSTRAP != 0;

struct Raw80211RadioConfiguration {
    wifi_interface_t Interface = WIFI_IF_STA;
    uint8_t Channel = 0;
    bool InitializeStationModeWhenNeeded = true;
};

struct Raw80211ReceiveTimestampStatistics final {
    std::uint64_t SampleNumber{0};
    std::uint32_t AlignmentResetCount{0};
    std::uint32_t AlignmentSampleCount{0};
    bool AlignmentFrozen{false};
    bool AlignmentResetOnThisSample{false};
    std::uint32_t RawWiFiTimestampMicroseconds{0};
    std::uint64_t CallbackMonotonicTimestampNanoseconds{0};
    std::uint64_t ExtendedWiFiTimestampMicroseconds{0};
    std::int64_t ObservedCallbackLagMicroseconds{0};
    std::int64_t SelectedAlignmentMicroseconds{0};
    std::uint64_t MappedMonotonicTimestampNanoseconds{0};
};

/// <summary>Managed ESP32 raw IEEE 802.11 provider for the Radio R3 runtime.</summary>
/// <remarks>
/// Ingress uses one compile-time fixed SPSC ring and one finite service quantum. The Wi-Fi driver callback copies only
/// provider bytes and timestamp evidence then coalesces the Radio runtime wake; it never invokes family/application code.
/// TX admission is correlated with a provider-local tag embedded in the private LLC envelope. ESP-IDF's raw TX callback
/// latches terminal completion in the Wi-Fi task, and the Radio domain service quantum publishes it only after R3 has had
/// the opportunity to install the returned deferred handle. Receive timestamp mapping is useful historical evidence but
/// remains Estimated rather than falsely claiming the conservative finite bound required for certified K1/K2 Clock use.
/// </remarks>
class Raw80211Radio final : public Radio::IRadio, public IRawWiFiPhyAccessObserver {
private:
    static constexpr std::size_t MacBytes = 6;
    static constexpr std::size_t Dot11HeaderBytes = 24;
    static constexpr std::size_t LlcBytes = 8;
    static constexpr std::size_t LengthBytes = 2;
    static constexpr std::size_t CorrelationBytes = 4;
    static constexpr std::size_t EncapsulationBytes = LlcBytes + LengthBytes + CorrelationBytes;
    static constexpr std::size_t MaximumPayloadBytes = 270;
    static constexpr uint16_t MaximumLogicalTransferBytes = 4096;
    static constexpr std::size_t MaximumFrameBytes = Dot11HeaderBytes + EncapsulationBytes + MaximumPayloadBytes;
    static constexpr uint8_t LlcSnap[LlcBytes] = {0xAA,0xAA,0x03,0x00,0x00,0x00,0x88,0xB5};
    static constexpr uint8_t RadioBssid[MacBytes] = {0x02,0x45,0x53,0x50,0x52,0x01};
    static constexpr std::uint32_t ReceiveTimestampAlignmentSamples = 32U;

    static_assert(ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH > 1,
                  "Raw80211 fixed RX ring requires at least two slots");
    static_assert(ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH <= 255,
                  "Raw80211 fixed RX ring indices are uint8_t");

    struct ReceivedPacket final {
        Radio::RadioAddress Source{};
        Radio::RadioAddress Destination{};
        uint16_t Length{0};
        int16_t RssiDbm{0};
        Radio::RadioReceiveTimestampEvidence Timestamp{};
        Raw80211ReceiveTimestampStatistics Diagnostics{};
        std::array<uint8_t,MaximumPayloadBytes> Payload{};
    };

    Raw80211RadioConfiguration _configuration{};
    Radio::IRadioReceiver* _receiver{nullptr};
    std::atomic<Radio::IRadioRuntimeSink*> _runtimeSink{nullptr};
    Radio::RadioAddress _localAddress{};
    std::array<ReceivedPacket,ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH> _receiveQueue{};
    std::atomic<uint8_t> _writeIndex{0};
    std::atomic<uint8_t> _readIndex{0};
    std::atomic<std::uint32_t> _acceptedPackets{0};
    std::atomic<std::uint32_t> _droppedPackets{0};
    std::atomic<std::uint32_t> _highWatermark{0};
    std::atomic<bool> _started{false};
    bool _promiscuousWasEnabled{false};
    bool _phyRegistered{false};
    Raw80211WiFiBootstrap _leanWiFiBootstrap{};

    bool _hasReceiveTimestampAlignment{false};
    bool _receiveTimestampAlignmentFrozen{false};
    uint32_t _lastWiFiReceiveTimestampMicroseconds{0};
    uint64_t _extendedWiFiReceiveTimestampMicroseconds{0};
    int64_t _selectedReceiveAlignmentMicroseconds{0};
    uint32_t _receiveTimestampAlignmentCount{0};
    uint32_t _receiveTimestampAlignmentResetCount{0};
    uint64_t _receiveTimestampSampleNumber{0};
    Raw80211ReceiveTimestampStatistics _lastTimestampStatistics{};

    std::atomic<std::uint32_t> _nextTransmissionHandle{1};
    std::atomic<std::uint32_t> _pendingTransmissionHandle{0};
    std::atomic<bool> _terminalCompletionPending{false};
    std::atomic<std::uint8_t> _terminalTransmission{
        static_cast<std::uint8_t>(Radio::RadioTransmissionCompletion::Unknown)};

    static std::atomic<Raw80211Radio*>& CallbackInstance() noexcept {
        static std::atomic<Raw80211Radio*> instance{nullptr};
        return instance;
    }

    static bool IsOurFrame(const uint8_t* data,std::size_t length) noexcept {
        if (!data || length < Dot11HeaderBytes + EncapsulationBytes) return false;
        const uint16_t frameControl = static_cast<uint16_t>(data[0]) |
            (static_cast<uint16_t>(data[1]) << 8u);
        if ((frameControl & 0x00FCu) != 0x0008u) return false;
        if (std::memcmp(data + 16,RadioBssid,MacBytes) != 0) return false;
        return std::memcmp(data + Dot11HeaderBytes,LlcSnap,sizeof(LlcSnap)) == 0;
    }

    static bool IsBroadcastMac(const uint8_t* address) noexcept {
        if (!address) return false;
        for (std::size_t i=0;i<MacBytes;++i) if (address[i] != 0xFFu) return false;
        return true;
    }

    static std::uint32_t QueueDepth(std::uint8_t read,std::uint8_t write) noexcept {
        constexpr auto size = ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH;
        return write >= read ? static_cast<std::uint32_t>(write-read)
                             : static_cast<std::uint32_t>(size-static_cast<std::size_t>(read-write));
    }

    static void StoreU32(uint8_t* target,std::uint32_t value) noexcept {
        for(std::size_t i=0;i<4;++i) target[i]=static_cast<uint8_t>((value>>(8u*i))&0xFFu);
    }
    static std::uint32_t LoadU32(const uint8_t* source) noexcept {
        std::uint32_t value=0;
        for(std::size_t i=0;i<4;++i) value|=static_cast<std::uint32_t>(source[i])<<(8u*i);
        return value;
    }

    void UpdateHighWatermark(std::uint32_t depth) noexcept {
        auto current=_highWatermark.load(std::memory_order_relaxed);
        while(depth>current && !_highWatermark.compare_exchange_weak(
            current,depth,std::memory_order_relaxed,std::memory_order_relaxed)) {}
    }

    Raw80211ReceiveTimestampStatistics MapReceiveTimestamp(
        uint32_t wifiTimestampMicroseconds,uint64_t callbackTimestampNanoseconds) noexcept {
        const uint64_t callbackMicroseconds=callbackTimestampNanoseconds/1000ULL;
        bool resetAlignment=false;
        if(!_hasReceiveTimestampAlignment){
            _hasReceiveTimestampAlignment=true;
            _lastWiFiReceiveTimestampMicroseconds=wifiTimestampMicroseconds;
            _extendedWiFiReceiveTimestampMicroseconds=wifiTimestampMicroseconds;
            resetAlignment=true;
        }else{
            const uint32_t elapsed=wifiTimestampMicroseconds-_lastWiFiReceiveTimestampMicroseconds;
            _lastWiFiReceiveTimestampMicroseconds=wifiTimestampMicroseconds;
            if(elapsed>0x80000000UL){
                _extendedWiFiReceiveTimestampMicroseconds=wifiTimestampMicroseconds;
                resetAlignment=true;
            }else _extendedWiFiReceiveTimestampMicroseconds+=elapsed;
        }
        const int64_t observedCallbackLag=static_cast<int64_t>(callbackMicroseconds)-
            static_cast<int64_t>(_extendedWiFiReceiveTimestampMicroseconds);
        if(resetAlignment){
            _receiveTimestampAlignmentCount=0;
            _receiveTimestampAlignmentFrozen=false;
            _selectedReceiveAlignmentMicroseconds=observedCallbackLag;
            ++_receiveTimestampAlignmentResetCount;
        }
        if(!_receiveTimestampAlignmentFrozen){
            if(_receiveTimestampAlignmentCount==0 || observedCallbackLag<_selectedReceiveAlignmentMicroseconds)
                _selectedReceiveAlignmentMicroseconds=observedCallbackLag;
            if(_receiveTimestampAlignmentCount<ReceiveTimestampAlignmentSamples) ++_receiveTimestampAlignmentCount;
            if(_receiveTimestampAlignmentCount>=ReceiveTimestampAlignmentSamples) _receiveTimestampAlignmentFrozen=true;
        }
        const int64_t mappedMicroseconds=static_cast<int64_t>(_extendedWiFiReceiveTimestampMicroseconds)+
            _selectedReceiveAlignmentMicroseconds;
        std::uint64_t mappedNanoseconds=1;
        if(mappedMicroseconds>0){
            const auto value=static_cast<std::uint64_t>(mappedMicroseconds);
            mappedNanoseconds=value>std::numeric_limits<uint64_t>::max()/1000ULL
                ?std::numeric_limits<uint64_t>::max():value*1000ULL;
        }
        Raw80211ReceiveTimestampStatistics result{};
        result.SampleNumber=++_receiveTimestampSampleNumber;
        result.AlignmentResetCount=_receiveTimestampAlignmentResetCount;
        result.AlignmentSampleCount=_receiveTimestampAlignmentCount;
        result.AlignmentFrozen=_receiveTimestampAlignmentFrozen;
        result.AlignmentResetOnThisSample=resetAlignment;
        result.RawWiFiTimestampMicroseconds=wifiTimestampMicroseconds;
        result.CallbackMonotonicTimestampNanoseconds=callbackTimestampNanoseconds;
        result.ExtendedWiFiTimestampMicroseconds=_extendedWiFiReceiveTimestampMicroseconds;
        result.ObservedCallbackLagMicroseconds=observedCallbackLag;
        result.SelectedAlignmentMicroseconds=_selectedReceiveAlignmentMicroseconds;
        result.MappedMonotonicTimestampNanoseconds=mappedNanoseconds;
        return result;
    }

    bool Enqueue(
        const std::uint8_t* frame,std::uint16_t payloadLength,std::int16_t rssiDbm,
        const Raw80211ReceiveTimestampStatistics& diagnostics) noexcept {
        const auto write=_writeIndex.load(std::memory_order_relaxed);
        const auto next=static_cast<std::uint8_t>((write+1U)%_receiveQueue.size());
        const auto read=_readIndex.load(std::memory_order_acquire);
        if(next==read){_droppedPackets.fetch_add(1,std::memory_order_relaxed);return false;}
        auto& queued=_receiveQueue[write];
        queued.Source=Radio::RadioAddress::FromBytes(frame+10,MacBytes);
        queued.Destination=Radio::RadioAddress::FromBytes(frame+4,MacBytes);
        queued.Length=payloadLength;
        queued.RssiDbm=rssiDbm;
        queued.Diagnostics=diagnostics;
        queued.Timestamp={};
        queued.Timestamp.ProviderCaptureCoordinate=diagnostics.ExtendedWiFiTimestampMicroseconds*1000ULL;
        queued.Timestamp.MonotonicNanoseconds=diagnostics.MappedMonotonicTimestampNanoseconds;
        queued.Timestamp.ContinuityGeneration=diagnostics.AlignmentResetCount==0?1:diagnostics.AlignmentResetCount;
        queued.Timestamp.Source=Radio::RadioTimestampCaptureSource::Driver;
        queued.Timestamp.Quality=Radio::RadioTimestampQuality::Estimated;
        queued.Timestamp.HasCaptureModel=false;
        if(payloadLength) std::memcpy(queued.Payload.data(),frame+Dot11HeaderBytes+EncapsulationBytes,payloadLength);
        _writeIndex.store(next,std::memory_order_release);
        _acceptedPackets.fetch_add(1,std::memory_order_relaxed);
        UpdateHighWatermark(QueueDepth(read,next));
        return true;
    }

    static void PromiscuousReceive(void* buffer,wifi_promiscuous_pkt_type_t type) {
        auto* self=CallbackInstance().load(std::memory_order_acquire);
        if(!self||!self->_started.load(std::memory_order_acquire)||type!=WIFI_PKT_DATA||!buffer) return;
        const auto* packet=static_cast<const wifi_promiscuous_pkt_t*>(buffer);
        const auto* frame=packet->payload;
        const std::size_t frameLength=packet->rx_ctrl.sig_len;
        if(!IsOurFrame(frame,frameLength)) return;
        const auto* destination=frame+4;
        if(std::memcmp(destination,self->_localAddress.Bytes.data(),MacBytes)!=0&&!IsBroadcastMac(destination)) return;
        const std::size_t lengthOffset=Dot11HeaderBytes+LlcBytes;
        const uint16_t payloadLength=static_cast<uint16_t>(frame[lengthOffset])|
            (static_cast<uint16_t>(frame[lengthOffset+1])<<8u);
        if(payloadLength>MaximumPayloadBytes||Dot11HeaderBytes+EncapsulationBytes+payloadLength>frameLength) return;
        const auto now=System::Clock::Monotonic().NowNanoseconds();
        const auto diagnostics=self->MapReceiveTimestamp(packet->rx_ctrl.timestamp,now);
        if(!self->Enqueue(frame,payloadLength,packet->rx_ctrl.rssi,diagnostics)) return;
        if(auto* sink=self->_runtimeSink.load(std::memory_order_acquire)) sink->InboundAvailable(*self);
    }

    static void TransmitDone(const esp_80211_tx_info_t* info) {
        auto* self=CallbackInstance().load(std::memory_order_acquire);
        if(!self||!info||!self->_started.load(std::memory_order_acquire)) return;
        const auto pending=self->_pendingTransmissionHandle.load(std::memory_order_acquire);
        if(pending==0||!info->data) return;
        const auto minimumBody=EncapsulationBytes;
        if(info->data_len<minimumBody) return;
        const auto* frame=info->data;
        if(std::memcmp(frame+10,self->_localAddress.Bytes.data(),MacBytes)!=0||
           std::memcmp(frame+16,RadioBssid,MacBytes)!=0||
           std::memcmp(frame+Dot11HeaderBytes,LlcSnap,sizeof(LlcSnap))!=0) return;
        const auto callbackHandle=LoadU32(frame+Dot11HeaderBytes+LlcBytes+LengthBytes);
        if(callbackHandle!=pending) return;
        const auto terminal=info->tx_status==WIFI_SEND_SUCCESS
            ?Radio::RadioTransmissionCompletion::Completed
            :Radio::RadioTransmissionCompletion::Failed;
        self->_terminalTransmission.store(static_cast<std::uint8_t>(terminal),std::memory_order_relaxed);
        self->_terminalCompletionPending.store(true,std::memory_order_release);
        if(auto* sink=self->_runtimeSink.load(std::memory_order_acquire)) sink->TransmitReadinessChanged(*self);
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

    bool ResolveLocalAddress() noexcept {
        uint8_t address[MacBytes]{};
        if(esp_wifi_get_mac(_configuration.Interface,address)!=ESP_OK) return false;
        _localAddress=Radio::RadioAddress::FromBytes(address,MacBytes);
        return true;
    }

    void ReleasePhyRegistration() noexcept {
        if(!_phyRegistered) return;
        SharedWiFiPhy().ReleaseRawAccess();
        _phyRegistered=false;
    }
    void ReleaseOwnedWiFiBootstrap() noexcept {
#if ESPRESSIO_ESP32_RAW_RADIO_LEAN_WIFI_BOOTSTRAP
        _leanWiFiBootstrap.Shutdown();
#endif
    }

    static std::uint32_t NextHandle(std::atomic<std::uint32_t>& counter) noexcept {
        auto value=counter.fetch_add(1,std::memory_order_relaxed);
        if(value==0) value=counter.fetch_add(1,std::memory_order_relaxed);
        return value==0?1:value;
    }

public:
    explicit Raw80211Radio(Raw80211RadioConfiguration configuration={}) noexcept:_configuration(configuration) {}

    bool Start() override {
        if(_started.load(std::memory_order_acquire)) return true;
        Raw80211Radio* expected=nullptr;
        if(!CallbackInstance().compare_exchange_strong(expected,this,std::memory_order_acq_rel,std::memory_order_acquire))
            return false;

        wifi_mode_t mode=WIFI_MODE_NULL;
        const auto modeResult=esp_wifi_get_mode(&mode);
        if((modeResult!=ESP_OK||mode==WIFI_MODE_NULL)&&_configuration.InitializeStationModeWhenNeeded){
#if ESPRESSIO_ESP32_RAW_RADIO_LEAN_WIFI_BOOTSTRAP
            const auto bootstrap=_leanWiFiBootstrap.Initialize();
            if(!bootstrap||esp_wifi_get_mode(&mode)!=ESP_OK){Stop();return false;}
#else
            if(!::WiFi.mode(WIFI_STA)){Stop();return false;}
            mode=WIFI_MODE_STA;
#endif
        }
        if(mode==WIFI_MODE_NULL||(_configuration.Interface==WIFI_IF_STA&&mode==WIFI_MODE_AP)||
           (_configuration.Interface==WIFI_IF_AP&&mode==WIFI_MODE_STA)){Stop();return false;}

        SharedWiFiPhy().SetRawAccessObserver(this);
        const auto access=SharedWiFiPhy().RegisterRawAccess(_configuration.Channel,true);
        if(!access){Stop();return false;}
        _phyRegistered=true;
        if(!ResolveLocalAddress()){Stop();return false;}

        bool promiscuous=false;
        if(esp_wifi_get_promiscuous(&promiscuous)==ESP_OK) _promiscuousWasEnabled=promiscuous;
        if(esp_wifi_set_promiscuous_rx_cb(&Raw80211Radio::PromiscuousReceive)!=ESP_OK||
           esp_wifi_register_80211_tx_cb(&Raw80211Radio::TransmitDone)!=ESP_OK||
           esp_wifi_set_promiscuous(true)!=ESP_OK){Stop();return false;}

        _readIndex.store(0,std::memory_order_relaxed);
        _writeIndex.store(0,std::memory_order_relaxed);
        _pendingTransmissionHandle.store(0,std::memory_order_relaxed);
        _terminalCompletionPending.store(false,std::memory_order_relaxed);
        _hasReceiveTimestampAlignment=false;
        _receiveTimestampAlignmentFrozen=false;
        _lastWiFiReceiveTimestampMicroseconds=0;
        _extendedWiFiReceiveTimestampMicroseconds=0;
        _selectedReceiveAlignmentMicroseconds=0;
        _receiveTimestampAlignmentCount=0;
        _receiveTimestampAlignmentResetCount=0;
        _receiveTimestampSampleNumber=0;
        _lastTimestampStatistics={};
        _started.store(true,std::memory_order_release);
        if(auto* sink=_runtimeSink.load(std::memory_order_acquire)) sink->LifecycleAvailabilityChanged(*this);
        return true;
    }

    void Stop() noexcept override {
        const bool wasStarted=_started.exchange(false,std::memory_order_acq_rel);
        (void)esp_wifi_register_80211_tx_cb(nullptr);
        (void)esp_wifi_set_promiscuous_rx_cb(nullptr);
        if(wasStarted&&!_promiscuousWasEnabled) (void)esp_wifi_set_promiscuous(false);
        _readIndex.store(0,std::memory_order_relaxed);
        _writeIndex.store(0,std::memory_order_relaxed);
        _pendingTransmissionHandle.store(0,std::memory_order_relaxed);
        _terminalCompletionPending.store(false,std::memory_order_relaxed);
        ReleasePhyRegistration();
        SharedWiFiPhy().SetRawAccessObserver(nullptr);
        ReleaseOwnedWiFiBootstrap();
        Raw80211Radio* expected=this;
        (void)CallbackInstance().compare_exchange_strong(expected,nullptr,std::memory_order_acq_rel,std::memory_order_acquire);
        if(wasStarted) if(auto* sink=_runtimeSink.load(std::memory_order_acquire)) sink->LifecycleAvailabilityChanged(*this);
    }

    bool IsStarted() const noexcept override {return _started.load(std::memory_order_acquire);}

    Radio::RadioCapabilities Capabilities() const noexcept override {
        return {Radio::RadioCapability::Broadcast|Radio::RadioCapability::Rssi|
                Radio::RadioCapability::ChannelSelection|Radio::RadioCapability::DataRateSelection|
                Radio::RadioCapability::TransmitPower|Radio::RadioCapability::HardwareAddressing|
                Radio::RadioCapability::ReceiveTimestamp|Radio::RadioCapability::CarrierSense,
                static_cast<uint16_t>(MaximumPayloadBytes),static_cast<uint8_t>(MacBytes),MaximumLogicalTransferBytes};
    }
    Radio::RadioAddress LocalAddress() const noexcept override {return _localAddress;}
    Radio::RadioContentionDomainId ContentionDomain() const noexcept override {return {ESP32WiFiRadioContentionDomain};}
    Radio::RadioProviderResourceProfile ProviderResources() const noexcept override {
        return {static_cast<std::uint16_t>(ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH-1),
                static_cast<std::uint16_t>(ESPRESSIO_ESP32_RAW_RADIO_RX_QUEUE_DEPTH-1),1,0};
    }
    bool IsTransmitReady() const noexcept override {
        return IsStarted()&&bool(SharedWiFiPhy().CachedRawAccess())&&
            _pendingTransmissionHandle.load(std::memory_order_acquire)==0;
    }

    Radio::RadioTransmissionCost EstimateTransmissionCost(
        const Radio::RadioAddress&,std::size_t payloadBytes,const Radio::RadioServiceProfile&) const noexcept override {
        const auto physicalBytes=Dot11HeaderBytes+EncapsulationBytes+payloadBytes;
        return {physicalBytes==0?1:static_cast<std::uint64_t>(physicalBytes),0,
                Radio::RadioCostEstimateQuality::RelativeOnly};
    }

    Radio::RadioSendResult Send(
        const Radio::RadioAddress& destination,const uint8_t* payload,std::size_t payloadSize) noexcept override {
        if(!IsStarted()) return {Radio::RadioSendStatus::NotStarted,0};
        if(!SharedWiFiPhy().CachedRawAccess()||_pendingTransmissionHandle.load(std::memory_order_acquire)!=0)
            return {Radio::RadioSendStatus::Busy,0};
        if(!destination.IsValid()||destination.Length!=MacBytes)
            return {Radio::RadioSendStatus::InvalidAddress,0};
        if((!payload&&payloadSize!=0)||payloadSize>MaximumPayloadBytes)
            return {Radio::RadioSendStatus::PayloadTooLarge,0};

        const auto handleValue=NextHandle(_nextTransmissionHandle);
        std::array<uint8_t,MaximumFrameBytes> frame{};
        frame[0]=0x08; frame[1]=0x00;
        std::memcpy(frame.data()+4,destination.Bytes.data(),MacBytes);
        std::memcpy(frame.data()+10,_localAddress.Bytes.data(),MacBytes);
        std::memcpy(frame.data()+16,RadioBssid,MacBytes);
        std::memcpy(frame.data()+Dot11HeaderBytes,LlcSnap,sizeof(LlcSnap));
        const auto lengthOffset=Dot11HeaderBytes+LlcBytes;
        frame[lengthOffset]=static_cast<uint8_t>(payloadSize&0xFFu);
        frame[lengthOffset+1]=static_cast<uint8_t>((payloadSize>>8u)&0xFFu);
        StoreU32(frame.data()+lengthOffset+LengthBytes,handleValue);
        if(payloadSize) std::memcpy(frame.data()+Dot11HeaderBytes+EncapsulationBytes,payload,payloadSize);

        _terminalCompletionPending.store(false,std::memory_order_release);
        _pendingTransmissionHandle.store(handleValue,std::memory_order_release);
        const auto result=esp_wifi_80211_tx(_configuration.Interface,frame.data(),
            static_cast<int>(Dot11HeaderBytes+EncapsulationBytes+payloadSize),true);
        if(result!=ESP_OK){
            std::uint32_t expected=handleValue;
            (void)_pendingTransmissionHandle.compare_exchange_strong(expected,0,std::memory_order_acq_rel);
#ifdef ESP_ERR_NO_MEM
            if(result==ESP_ERR_NO_MEM) return {Radio::RadioSendStatus::NoMemory,static_cast<int32_t>(result)};
#endif
            return {Radio::RadioSendStatus::NativeFailure,static_cast<int32_t>(result)};
        }
        return Radio::RadioSendResult::Accepted({},Radio::RadioTransmissionHandle{handleValue});
    }

    void SetReceiver(Radio::IRadioReceiver* receiver) noexcept override {_receiver=receiver;}
    void SetRuntimeSink(Radio::IRadioRuntimeSink* sink) noexcept override {_runtimeSink.store(sink,std::memory_order_release);}

    Radio::ManagedRadioIngressServiceResult ServiceInbound(std::size_t maximumPackets=0) noexcept override {
        PublishPendingTransmissionCompletion();
        const auto initialRead=_readIndex.load(std::memory_order_relaxed);
        const auto initialWrite=_writeIndex.load(std::memory_order_acquire);
        const auto initialDepth=QueueDepth(initialRead,initialWrite);
        if(initialDepth==0) return {};
        const std::size_t finiteDefault=_receiveQueue.size()-1;
        const std::size_t requested=maximumPackets==0?finiteDefault:maximumPackets;
        const std::size_t toProcess=std::min<std::size_t>(requested,initialDepth);
        auto read=initialRead;
        std::uint32_t processed=0;
        for(std::size_t i=0;i<toProcess;++i){
            const auto& queued=_receiveQueue[read];
            Radio::RadioPacketView view{queued.Source,queued.Destination,
                queued.Length?queued.Payload.data():nullptr,queued.Length,queued.RssiDbm,
                queued.Timestamp.MonotonicNanoseconds,
                queued.Destination.IsBroadcast()?Radio::RadioPacketFlag::Broadcast:Radio::RadioPacketFlag::None};
            if(_receiver) _receiver->OnRadioPacket(*this,view,queued.Timestamp);
            _lastTimestampStatistics=queued.Diagnostics;
            read=static_cast<std::uint8_t>((read+1U)%_receiveQueue.size());
            _readIndex.store(read,std::memory_order_release);
            ++processed;
        }
        const auto latestWrite=_writeIndex.load(std::memory_order_acquire);
        return {processed,read!=latestWrite};
    }

    Raw80211ReceiveTimestampStatistics ReceiveTimestampStatistics() const noexcept {return _lastTimestampStatistics;}
    std::uint64_t AcceptedIngressPackets() const noexcept {return _acceptedPackets.load(std::memory_order_relaxed);}
    std::uint64_t DroppedIngressPackets() const noexcept {return _droppedPackets.load(std::memory_order_relaxed);}
    std::uint32_t IngressHighWatermark() const noexcept {return _highWatermark.load(std::memory_order_relaxed);}

    void RawWiFiPhyAccessChanged(const RawWiFiPhyAccess&) noexcept override {
        if(auto* sink=_runtimeSink.load(std::memory_order_acquire)){
            sink->TransmitReadinessChanged(*this);
            sink->LifecycleAvailabilityChanged(*this);
        }
    }
};

} // namespace ESPressio::ESP32Platform
