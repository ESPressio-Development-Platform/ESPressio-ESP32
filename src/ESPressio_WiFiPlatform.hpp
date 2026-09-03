#pragma once

#if !defined(ARDUINO_ARCH_ESP32)
#error "ESPressio_WiFiPlatform.hpp requires an ESP32 Arduino target"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <algorithm>
#include <cmath>

#include <ESPressio_Clock.hpp>
#include <ESPressio_WiFi.hpp>

#include "ESPressio_WiFiPhyCoordinator.hpp"

namespace ESPressio::WiFi {

class WiFiPlatform final : public IWiFiPlatform {
public:
    WiFiStatus Apply(const WiFiConfiguration& configuration) override {
        if (!Validate(configuration)) return WiFiStatus::InvalidConfiguration;
        _configuration = RetainConfiguration(configuration);
        _manualDisconnect = false;
        _reconnectAttempts = 0;
        _nextReconnectMilliseconds = 0;
        _hasActiveProfile = false;
        _scanRunning = false;

        wifi_mode_t mode = WIFI_MODE_NULL;
        switch (configuration.Mode) {
            case WiFiMode::Disabled: mode = WIFI_MODE_NULL; break;
            case WiFiMode::Client: mode = WIFI_MODE_STA; break;
            case WiFiMode::AccessPoint: mode = WIFI_MODE_AP; break;
            case WiFiMode::AccessPointClient: mode = WIFI_MODE_APSTA; break;
            case WiFiMode::APUntilClient: mode = WIFI_MODE_STA; break;
            case WiFiMode::Off: mode = WIFI_MODE_NULL; break;
        }
        if (!::WiFi.mode(mode)) return WiFiStatus::PlatformError;

        // Ordinary ESPressio Wi-Fi is the owner of shared ESP32 PHY policy while its configured mode is active.
        // Raw80211 consults this same coordinator and therefore follows the effective Wi-Fi channel rather than retuning it.
        ESP32Platform::SharedWiFiPhy().SetWiFiServiceActive(mode != WIFI_MODE_NULL);

        if (configuration.Mode == WiFiMode::Off || configuration.Mode == WiFiMode::Disabled) {
            _state = WiFiRuntimeState{};
            _state.Mode = configuration.Mode;
            ++_state.Revision;
            _knownStations.clear();
            return WiFiStatus::Success;
        }

        if (!configuration.Hostname.empty()) {
            (void)::WiFi.setHostname(configuration.Hostname.c_str());
        }

        const bool automaticProfiles =
            UsesClient(configuration.Mode) &&
            configuration.Client.Enabled &&
            configuration.Client.Selection.AutomaticSelection &&
            !configuration.Client.Networks.empty();

        if (UsesClient(configuration.Mode) &&
            !automaticProfiles &&
            !ConfigureClientAddressLegacy()) {
            return WiFiStatus::PlatformError;
        }
        if (PersistentAP(configuration.Mode) && !ConfigureAccessPoint()) {
            return WiFiStatus::PlatformError;
        }
        ApplyRadioSettings();

        if (UsesClient(configuration.Mode) &&
            configuration.Client.Enabled &&
            !automaticProfiles &&
            !configuration.Client.SSID.empty()) {
            BeginClient(false);
        }

        RefreshState();
        return WiFiStatus::Success;
    }

    WiFiStatus Disable() override {
        _manualDisconnect = true;
        _scanRunning = false;
        if (!::WiFi.mode(WIFI_MODE_NULL)) return WiFiStatus::PlatformError;
        ESP32Platform::SharedWiFiPhy().SetWiFiServiceActive(false);
        _configuration.Mode = WiFiMode::Off;
        _state = WiFiRuntimeState{};
        _state.Mode = WiFiMode::Off;
        ++_state.Revision;
        _knownStations.clear();
        return WiFiStatus::Success;
    }

    WiFiStatus ConnectClient() override {
        if (!UsesClient(_configuration.Mode) ||
            !_configuration.Client.Enabled ||
            _configuration.Client.SSID.empty()) {
            return WiFiStatus::InvalidConfiguration;
        }
        _hasActiveProfile = false;
        if (!ConfigureClientAddressLegacy()) return WiFiStatus::PlatformError;
        _manualDisconnect = false;
        _reconnectAttempts = 0;
        BeginClient(false);
        return WiFiStatus::Success;
    }

    WiFiStatus ConnectClient(const ClientNetworkProfile& profile) override {
        if (!UsesClient(_configuration.Mode) ||
            !_configuration.Client.Enabled ||
            !profile.Enabled ||
            profile.SSID.empty()) {
            return WiFiStatus::InvalidConfiguration;
        }
        if (!ValidateProfile(profile)) return WiFiStatus::InvalidConfiguration;
        _activeProfile = profile;
        _hasActiveProfile = true;
        if (!ConfigureClientAddress(profile.Addressing, profile.StaticNetwork)) {
            return WiFiStatus::PlatformError;
        }
        _manualDisconnect = false;
        _reconnectAttempts = 0;
        BeginClient(false);
        return WiFiStatus::Success;
    }

    WiFiStatus DisconnectClient() override {
        if (!UsesClient(_configuration.Mode)) return WiFiStatus::NotSupported;
        _manualDisconnect = true;
        _state.Client.State = ClientState::Disconnecting;
        ++_state.Revision;
        const bool disconnected = ::WiFi.disconnect(false, false);
        _state.Client.State = ClientState::Disconnected;
        ++_state.Revision;
        return disconnected ? WiFiStatus::Success : WiFiStatus::PlatformError;
    }

    WiFiStatus StartAccessPoint() override {
        if (!CanHostAP(_configuration.Mode) ||
            !_configuration.AccessPoint.Enabled ||
            _configuration.AccessPoint.SSID.empty()) {
            return WiFiStatus::NotSupported;
        }
        if (_configuration.Mode == WiFiMode::APUntilClient &&
            !::WiFi.mode(WIFI_MODE_APSTA)) {
            return WiFiStatus::PlatformError;
        }
        _state.AccessPoint.State = AccessPointState::Starting;
        ++_state.Revision;
        if (!ConfigureAccessPoint()) {
            _state.AccessPoint.State = AccessPointState::Failed;
            ++_state.Revision;
            return WiFiStatus::PlatformError;
        }
        _state.AccessPoint.State = AccessPointState::Active;
        ++_state.Revision;
        return WiFiStatus::Success;
    }

    WiFiStatus StopAccessPoint() override {
        if (!CanHostAP(_configuration.Mode)) return WiFiStatus::NotSupported;
        if (!::WiFi.softAPdisconnect(false)) return WiFiStatus::PlatformError;
        if (_configuration.Mode == WiFiMode::APUntilClient &&
            !::WiFi.mode(WIFI_MODE_STA)) {
            return WiFiStatus::PlatformError;
        }
        _state.AccessPoint.State = AccessPointState::Disabled;
        _state.AccessPoint.Network = {};
        _knownStations.clear();
        ++_state.Revision;
        return WiFiStatus::Success;
    }

    WiFiStatus StartScan() override {
        if (_configuration.Mode == WiFiMode::Off ||
            _configuration.Mode == WiFiMode::Disabled) {
            return WiFiStatus::NotSupported;
        }
        if (_scanRunning) return WiFiStatus::Busy;
        const int16_t started = ::WiFi.scanNetworks(true, true);
        if (started == WIFI_SCAN_FAILED) {
            _state.Scan = ScanState::Failed;
            ++_state.Revision;
            return WiFiStatus::PlatformError;
        }
        _scanRunning = true;
        _state.Scan = ScanState::Scanning;
        ++_state.Revision;
        return WiFiStatus::Success;
    }

    /// <summary>Polls ESP32 Wi-Fi state using externally preferred scan and event collections supplied by the Wi-Fi core.</summary>
    WiFiStatus Poll(
        WiFiRuntimeState& state,
        WiFiVector<ScanResult>* completedScan,
        WiFiVector<WiFiPlatformEvent>* events
    ) override {
        const auto before = _state;
        PollReconnect();
        RefreshState();
        PollScan(completedScan);
        if (events) {
            BuildNetworkEvents(before, _state, *events);
            BuildStationEvents(*events);
        }
        state = _state;
        return WiFiStatus::Success;
    }

    WiFiRadioState GetRadioState() const override {
        WiFiRadioState state;

        wifi_mode_t mode = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&mode) != ESP_OK) return state;
        switch (mode) {
            case WIFI_MODE_STA:
                state.Mode = WiFiRadioMode::Station;
                state.StationInterfaceActive = true;
                break;
            case WIFI_MODE_AP:
                state.Mode = WiFiRadioMode::AccessPoint;
                state.AccessPointInterfaceActive = true;
                break;
            case WIFI_MODE_APSTA:
                state.Mode = WiFiRadioMode::AccessPointStation;
                state.StationInterfaceActive = true;
                state.AccessPointInterfaceActive = true;
                break;
            case WIFI_MODE_NULL:
            default:
                state.Mode = WiFiRadioMode::Off;
                break;
        }

        state.StationConnected =
            state.StationInterfaceActive && ::WiFi.status() == WL_CONNECTED;
        state.Scanning = _scanRunning;

        if (mode != WIFI_MODE_NULL) {
            uint8_t primary = 0;
            wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
            if (esp_wifi_get_channel(&primary, &secondary) == ESP_OK) {
                state.Channel = primary;
            }
        }

        uint8_t mac[6] = {};
        if (state.StationInterfaceActive &&
            esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
            std::copy(mac, mac + 6, state.StationMAC.Octets.begin());
        }
        if (state.AccessPointInterfaceActive &&
            esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
            std::copy(mac, mac + 6, state.AccessPointMAC.Octets.begin());
        }

        return state;
    }

private:
    struct PlatformClientConfiguration {
        bool Enabled = false;
        WiFiString SSID;
        WiFiString Password;
        AddressMode Addressing = AddressMode::DHCP;
        NetworkAddress StaticNetwork{};
    };

    struct PlatformConfiguration {
        WiFiMode Mode = WiFiMode::AccessPoint;
        PlatformClientConfiguration Client{};
        AccessPointConfiguration AccessPoint{};
        ReconnectPolicy Reconnect{};
        int8_t TxPowerDbm = 20;
        bool PowerSave = false;
    };

    static uint32_t NowMilliseconds() {
        return static_cast<uint32_t>(
            System::Clock::Monotonic().NowNanoseconds() / 1000000ULL
        );
    }

    static PlatformConfiguration RetainConfiguration(
        const WiFiConfiguration& configuration
    ) {
        PlatformConfiguration retained;
        retained.Mode = configuration.Mode;
        retained.Client.Enabled = configuration.Client.Enabled;
        retained.Client.SSID = configuration.Client.SSID;
        retained.Client.Password = configuration.Client.Password;
        retained.Client.Addressing = configuration.Client.Addressing;
        retained.Client.StaticNetwork = configuration.Client.StaticNetwork;
        retained.AccessPoint = configuration.AccessPoint;
        retained.Reconnect = configuration.Reconnect;
        retained.TxPowerDbm = configuration.TxPowerDbm;
        retained.PowerSave = configuration.PowerSave;
        return retained;
    }

    static bool PersistentAP(WiFiMode mode) {
        return mode == WiFiMode::AccessPoint || mode == WiFiMode::AccessPointClient;
    }
    static bool CanHostAP(WiFiMode mode) {
        return PersistentAP(mode) || mode == WiFiMode::APUntilClient;
    }
    static bool UsesClient(WiFiMode mode) {
        return mode == WiFiMode::Client ||
            mode == WiFiMode::AccessPointClient ||
            mode == WiFiMode::APUntilClient;
    }

    static bool ValidateCredential(const WiFiString& password) {
        return password.empty() || (password.size() >= 8 && password.size() <= 63);
    }
    static bool ValidateProfile(const ClientNetworkProfile& profile) {
        return profile.SSID.size() <= 32 && ValidateCredential(profile.Password);
    }
    static bool Validate(const WiFiConfiguration& configuration) {
        if (configuration.Hostname.size() > 32) return false;
        if (configuration.AccessPoint.SSID.size() > 32 ||
            configuration.Client.SSID.size() > 32) return false;
        if (!ValidateCredential(configuration.AccessPoint.Password) ||
            !ValidateCredential(configuration.Client.Password)) return false;
        for (const auto& profile : configuration.Client.Networks) {
            if (!ValidateProfile(profile)) return false;
        }
        if (configuration.AccessPoint.Channel < 1 || configuration.AccessPoint.Channel > 14) return false;
        if (configuration.AccessPoint.MaximumClients == 0) return false;
        if (configuration.Reconnect.BackoffMultiplier < 1.0f) return false;
        if (configuration.Reconnect.MaximumDelayMilliseconds <
            configuration.Reconnect.InitialDelayMilliseconds) return false;
        return true;
    }

    static IPv4Address Convert(const IPAddress& value) {
        return IPv4Address(value[0], value[1], value[2], value[3]);
    }
    static IPAddress Convert(const IPv4Address& value) {
        return IPAddress(
            value.Octets[0], value.Octets[1], value.Octets[2], value.Octets[3]
        );
    }
    static bool IsZero(const IPv4Address& value) {
        return value.Octets[0] == 0 && value.Octets[1] == 0 &&
            value.Octets[2] == 0 && value.Octets[3] == 0;
    }
    static bool TimeReached(uint32_t now, uint32_t target) {
        return static_cast<int32_t>(now - target) >= 0;
    }

    bool ConfigureClientAddress(AddressMode mode, const NetworkAddress& network) {
        if (mode == AddressMode::DHCP) {
            return ::WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
        }
        return ::WiFi.config(
            Convert(network.Address),
            Convert(network.Gateway),
            Convert(network.SubnetMask),
            Convert(network.PrimaryDNS),
            Convert(network.SecondaryDNS)
        );
    }

    bool ConfigureClientAddressLegacy() {
        return ConfigureClientAddress(
            _configuration.Client.Addressing,
            _configuration.Client.StaticNetwork
        );
    }

    bool ConfigureAccessPoint() {
        const auto& ap = _configuration.AccessPoint;
        if (!ap.Enabled || ap.SSID.empty()) return false;
        if (!::WiFi.softAPConfig(
                Convert(ap.Network.Address),
                Convert(ap.Network.Gateway),
                Convert(ap.Network.SubnetMask)
            )) return false;
        const char* password = ap.Password.empty() ? nullptr : ap.Password.c_str();
        if (!::WiFi.softAP(
                ap.SSID.c_str(),
                password,
                ap.Channel,
                ap.Hidden,
                ap.MaximumClients
            )) return false;
        return ConfigureDHCPServer(ap.DHCP);
    }

    static bool ConfigureDHCPServer(const DHCPServerConfiguration& configuration) {
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (!netif) return false;
        (void)esp_netif_dhcps_stop(netif);
        if (!configuration.Enabled) return true;
#if defined(ESP_NETIF_REQUESTED_IP_ADDRESS)
        dhcps_lease_t lease{};
        lease.enable = true;
        IP4_ADDR(
            &lease.start_ip,
            configuration.LeaseStart.Octets[0], configuration.LeaseStart.Octets[1],
            configuration.LeaseStart.Octets[2], configuration.LeaseStart.Octets[3]
        );
        IP4_ADDR(
            &lease.end_ip,
            configuration.LeaseEnd.Octets[0], configuration.LeaseEnd.Octets[1],
            configuration.LeaseEnd.Octets[2], configuration.LeaseEnd.Octets[3]
        );
        if (esp_netif_dhcps_option(
                netif,
                ESP_NETIF_OP_SET,
                ESP_NETIF_REQUESTED_IP_ADDRESS,
                &lease,
                sizeof(lease)
            ) != ESP_OK) return false;
#endif
#if defined(ESP_NETIF_IP_ADDRESS_LEASE_TIME)
        uint32_t leaseMinutes = std::max<uint32_t>(
            1,
            configuration.LeaseDurationSeconds / 60
        );
        if (esp_netif_dhcps_option(
                netif,
                ESP_NETIF_OP_SET,
                ESP_NETIF_IP_ADDRESS_LEASE_TIME,
                &leaseMinutes,
                sizeof(leaseMinutes)
            ) != ESP_OK) return false;
#endif
        const esp_err_t started = esp_netif_dhcps_start(netif);
#if defined(ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
        return started == ESP_OK || started == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED;
#else
        return started == ESP_OK;
#endif
    }

    void ApplyRadioSettings() {
        (void)ESP32Platform::SharedWiFiPhy().ApplyWiFiPolicy(
            _configuration.TxPowerDbm,
            _configuration.PowerSave
        );
    }

    const WiFiString& ActiveSSID() const {
        return _hasActiveProfile ? _activeProfile.SSID : _configuration.Client.SSID;
    }
    const WiFiString& ActivePassword() const {
        return _hasActiveProfile ? _activeProfile.Password : _configuration.Client.Password;
    }

    void BeginClient(bool reconnect) {
        const auto& ssid = ActiveSSID();
        const auto& password = ActivePassword();
        if (ssid.empty()) {
            _state.Client.State = ClientState::Failed;
            ++_state.Revision;
            return;
        }
        _state.Client.State = reconnect ? ClientState::Reconnecting : ClientState::Connecting;
        _state.Client.ReconnectAttempt = _reconnectAttempts;
        _clientAttemptStartedMilliseconds = NowMilliseconds();
        ++_state.Revision;
        ::WiFi.begin(ssid.c_str(), password.empty() ? nullptr : password.c_str());
    }

    void ScheduleReconnect() {
        if (_manualDisconnect || !_configuration.Reconnect.Enabled) {
            _state.Client.State = ClientState::Disconnected;
            ++_state.Revision;
            return;
        }
        if (_configuration.Reconnect.MaximumAttempts != 0 &&
            _reconnectAttempts >= _configuration.Reconnect.MaximumAttempts) {
            _state.Client.State = ClientState::Failed;
            ++_state.Revision;
            return;
        }
        ++_reconnectAttempts;
        double delay = static_cast<double>(_configuration.Reconnect.InitialDelayMilliseconds);
        if (_reconnectAttempts > 1) {
            delay *= std::pow(
                static_cast<double>(_configuration.Reconnect.BackoffMultiplier),
                static_cast<double>(_reconnectAttempts - 1)
            );
        }
        const uint32_t bounded = static_cast<uint32_t>(
            std::min<double>(
                delay,
                static_cast<double>(_configuration.Reconnect.MaximumDelayMilliseconds)
            )
        );
        _nextReconnectMilliseconds = NowMilliseconds() + bounded;
        _state.Client.State = ClientState::Reconnecting;
        _state.Client.ReconnectAttempt = _reconnectAttempts;
        ++_state.Revision;
    }

    void PollReconnect() {
        if (!UsesClient(_configuration.Mode) ||
            !_configuration.Client.Enabled ||
            _manualDisconnect ||
            ActiveSSID().empty()) return;

        const auto status = ::WiFi.status();
        if (status == WL_CONNECTED) {
            _reconnectAttempts = 0;
            _nextReconnectMilliseconds = 0;
            return;
        }
        if (_state.Client.State == ClientState::Connected) {
            ScheduleReconnect();
            return;
        }
        if (_state.Client.State == ClientState::Connecting) {
            const uint32_t elapsed = NowMilliseconds() - _clientAttemptStartedMilliseconds;
            if (status == WL_CONNECT_FAILED ||
                status == WL_NO_SSID_AVAIL ||
                elapsed >= _configuration.Reconnect.ConnectionTimeoutMilliseconds) {
                ScheduleReconnect();
            }
            return;
        }
        if (_state.Client.State == ClientState::Reconnecting &&
            _nextReconnectMilliseconds != 0 &&
            TimeReached(NowMilliseconds(), _nextReconnectMilliseconds)) {
            _nextReconnectMilliseconds = 0;
            BeginClient(true);
        }
    }

    void RefreshState() {
        WiFiRuntimeState next = _state;
        next.Mode = _configuration.Mode;
        if (UsesClient(_configuration.Mode)) {
            const auto status = ::WiFi.status();
            if (status == WL_CONNECTED) next.Client.State = ClientState::Connected;
            else if (_manualDisconnect) next.Client.State = ClientState::Disconnected;
            if (status == WL_CONNECTED) {
                const auto currentSSID = ::WiFi.SSID();
                next.Client.SSID.assign(currentSSID.c_str(), currentSSID.length());
            } else {
                next.Client.SSID = ActiveSSID();
            }
            next.Client.RSSI = status == WL_CONNECTED ? ::WiFi.RSSI() : 0;
            next.Client.Channel = status == WL_CONNECTED
                ? static_cast<uint8_t>(::WiFi.channel())
                : 0;
            next.Client.Network.Address = Convert(::WiFi.localIP());
            next.Client.Network.Gateway = Convert(::WiFi.gatewayIP());
            next.Client.Network.SubnetMask = Convert(::WiFi.subnetMask());
            next.Client.Network.PrimaryDNS = Convert(::WiFi.dnsIP(0));
            next.Client.Network.SecondaryDNS = Convert(::WiFi.dnsIP(1));
            next.Client.ReconnectAttempt = _reconnectAttempts;
        } else {
            next.Client = ClientRuntimeState{};
        }

        if (CanHostAP(_configuration.Mode)) {
            const auto apIP = ::WiFi.softAPIP();
            if (apIP == IPAddress(0, 0, 0, 0)) {
                next.AccessPoint = AccessPointRuntimeState{};
            } else {
                next.AccessPoint.State = AccessPointState::Active;
                next.AccessPoint.SSID = _configuration.AccessPoint.SSID;
                next.AccessPoint.Channel = _configuration.AccessPoint.Channel;
                next.AccessPoint.Network = _configuration.AccessPoint.Network;
                next.AccessPoint.Network.Address = Convert(apIP);
                next.AccessPoint.ConnectedStations =
                    static_cast<uint16_t>(::WiFi.softAPgetStationNum());
            }
        } else {
            next.AccessPoint = AccessPointRuntimeState{};
        }

        if (StateChanged(next, _state)) {
            next.Revision = _state.Revision + 1;
            _state = std::move(next);
        }
    }

    void PollScan(WiFiVector<ScanResult>* completedScan) {
        if (!_scanRunning) return;
        const int16_t count = ::WiFi.scanComplete();
        if (count == WIFI_SCAN_RUNNING) return;
        if (count < 0) {
            _scanRunning = false;
            _state.Scan = ScanState::Failed;
            ++_state.Revision;
            return;
        }

        WiFiVector<ScanResult> discarded;
        auto& results = completedScan != nullptr ? *completedScan : discarded;
        results.clear();
        results.reserve(static_cast<std::size_t>(count));
        for (int16_t i = 0; i < count; ++i) {
            ScanResult result;
            const auto ssid = ::WiFi.SSID(i);
            result.SSID.assign(ssid.c_str(), ssid.length());
            result.RSSI = ::WiFi.RSSI(i);
            result.Channel = static_cast<uint8_t>(::WiFi.channel(i));
            result.Security = ConvertSecurity(::WiFi.encryptionType(i));
            result.Hidden = result.SSID.empty();
            uint8_t* bssid = ::WiFi.BSSID(i);
            if (bssid) std::copy(bssid, bssid + 6, result.BSSID.Octets.begin());
            results.push_back(std::move(result));
        }
        ::WiFi.scanDelete();
        _scanRunning = false;
        _state.Scan = ScanState::Complete;
        ++_state.Revision;
    }

    void BuildNetworkEvents(
        const WiFiRuntimeState& before,
        const WiFiRuntimeState& after,
        WiFiVector<WiFiPlatformEvent>& events
    ) {
        const bool hadIP = !IsZero(before.Client.Network.Address);
        const bool hasIP = !IsZero(after.Client.Network.Address);
        if (!hadIP && hasIP) {
            WiFiPlatformEvent event;
            event.Kind = WiFiPlatformEventKind::ClientIPAddressAcquired;
            event.Network = after.Client.Network;
            events.push_back(event);
        } else if (hadIP && !hasIP) {
            WiFiPlatformEvent event;
            event.Kind = WiFiPlatformEventKind::ClientIPAddressLost;
            events.push_back(event);
        }
    }

    void BuildStationEvents(WiFiVector<WiFiPlatformEvent>& events) {
        if (!CanHostAP(_configuration.Mode) ||
            ::WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
            _knownStations.clear();
            return;
        }

        wifi_sta_list_t current{};
        if (esp_wifi_ap_get_sta_list(&current) != ESP_OK) return;
        WiFiVector<MacAddress> stations;
        stations.reserve(current.num);
        for (int i = 0; i < current.num; ++i) {
            MacAddress mac;
            std::copy(current.sta[i].mac, current.sta[i].mac + 6, mac.Octets.begin());
            stations.push_back(mac);
            if (std::find(_knownStations.begin(), _knownStations.end(), mac) == _knownStations.end()) {
                WiFiPlatformEvent event;
                event.Kind = WiFiPlatformEventKind::AccessPointStationConnected;
                event.Station = mac;
                events.push_back(event);
            }
        }
        for (const auto& known : _knownStations) {
            if (std::find(stations.begin(), stations.end(), known) == stations.end()) {
                WiFiPlatformEvent event;
                event.Kind = WiFiPlatformEventKind::AccessPointStationDisconnected;
                event.Station = known;
                events.push_back(event);
            }
        }
        _knownStations = std::move(stations);
    }

    static NetworkSecurity ConvertSecurity(wifi_auth_mode_t mode) {
        switch (mode) {
            case WIFI_AUTH_OPEN: return NetworkSecurity::Open;
            case WIFI_AUTH_WEP: return NetworkSecurity::WEP;
            case WIFI_AUTH_WPA_PSK: return NetworkSecurity::WPA;
            case WIFI_AUTH_WPA2_PSK: return NetworkSecurity::WPA2;
            case WIFI_AUTH_WPA_WPA2_PSK: return NetworkSecurity::WPA_WPA2;
#if defined(WIFI_AUTH_WPA3_PSK)
            case WIFI_AUTH_WPA3_PSK: return NetworkSecurity::WPA3;
#endif
#if defined(WIFI_AUTH_WPA2_WPA3_PSK)
            case WIFI_AUTH_WPA2_WPA3_PSK: return NetworkSecurity::WPA2_WPA3;
#endif
            default: return NetworkSecurity::Unknown;
        }
    }

    static bool StateChanged(const WiFiRuntimeState& a, const WiFiRuntimeState& b) {
        return a.Mode != b.Mode ||
            a.Client.State != b.Client.State ||
            a.Client.SSID != b.Client.SSID ||
            a.Client.RSSI != b.Client.RSSI ||
            a.Client.Channel != b.Client.Channel ||
            a.Client.Network.Address != b.Client.Network.Address ||
            a.Client.ReconnectAttempt != b.Client.ReconnectAttempt ||
            a.AccessPoint.State != b.AccessPoint.State ||
            a.AccessPoint.SSID != b.AccessPoint.SSID ||
            a.AccessPoint.Channel != b.AccessPoint.Channel ||
            a.AccessPoint.ConnectedStations != b.AccessPoint.ConnectedStations ||
            a.AccessPoint.Network.Address != b.AccessPoint.Network.Address ||
            a.Scan != b.Scan;
    }

    PlatformConfiguration _configuration{};
    WiFiRuntimeState _state{};
    ClientNetworkProfile _activeProfile{};
    bool _hasActiveProfile = false;
    bool _manualDisconnect = false;
    bool _scanRunning = false;
    uint32_t _clientAttemptStartedMilliseconds = 0;
    uint32_t _nextReconnectMilliseconds = 0;
    uint32_t _reconnectAttempts = 0;
    WiFiVector<MacAddress> _knownStations;
};

} // namespace ESPressio::WiFi
