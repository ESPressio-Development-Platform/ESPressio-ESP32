#pragma once

#if !defined(ARDUINO_ARCH_ESP32)
#error "ESPressio_Raw80211WiFiBootstrap.hpp requires an ESP32 Arduino target"
#endif

#include <cstdint>

#include <esp_err.h>
#include <esp_event.h>
#include <esp_wifi.h>

namespace ESPressio::ESP32Platform {

/// <summary>Outcome from establishing an ESP-IDF Wi-Fi driver exclusively for Raw80211 use.</summary>

enum class Raw80211WiFiBootstrapStatus : std::uint8_t {
    Initialized,
    AlreadyInitialized,
    InvalidConfiguration,
    EventLoopInitializationFailed,
    DriverInitializationFailed,
    StorageConfigurationFailed,
    ModeConfigurationFailed,
    DriverStartFailed
};

/// <summary>Detailed Raw80211-only Wi-Fi bootstrap result.</summary>

struct Raw80211WiFiBootstrapResult final {
    Raw80211WiFiBootstrapStatus Status{Raw80211WiFiBootstrapStatus::InvalidConfiguration};
    esp_err_t NativeStatus{ESP_OK};

    constexpr explicit operator bool() const noexcept {
        return Status == Raw80211WiFiBootstrapStatus::Initialized ||
               Status == Raw80211WiFiBootstrapStatus::AlreadyInitialized;
    }
};

/// <summary>
/// Memory-conscious ESP-IDF Wi-Fi driver profile for deployments that use the integrated PHY only as a Raw80211 bearer.
/// </summary>
/// <remarks>
/// This profile is intentionally opt-in. It is not suitable for a composition that subsequently expects Arduino WiFi,
/// ESPressio-WiFi, LwIP station/AP networking, or another owner to attach to the already-initialized driver. Shared-WiFi
/// compositions must retain the ordinary Wi-Fi lifecycle and let Raw80211 join that existing PHY instead.
///
/// The defaults deliberately retain six static hardware RX buffers and a larger dynamic RX pool rather than chasing the
/// absolute minimum allocation. The principal saving comes from replacing the general-purpose station profile and its
/// unused networking/throughput features, not from starving the radio receive path. AMPDU/AMSDU, CSI and Wi-Fi NVS are
/// disabled because this provider emits and accepts independent non-QoS frames and keeps its own bounded queues. TX keeps
/// whichever allocation mode WIFI_INIT_CONFIG_DEFAULT selected, with only that active mode's buffer count reduced.
/// </remarks>

struct Raw80211WiFiBootstrapConfiguration final {
    std::uint8_t StaticRxBuffers{6U};
    std::uint8_t DynamicRxBuffers{12U};
    std::uint8_t DynamicTxBuffers{8U};
    std::uint8_t StaticTxBuffers{6U};
    bool DisableAmpdu{true};
    bool DisableNvs{true};
};

/// <summary>
/// Owns a lean ESP-IDF Wi-Fi driver lifecycle for an otherwise Wi-Fi-free Raw80211 composition.
/// </summary>
/// <remarks>
/// Initialize() is idempotent with respect to an already-running Wi-Fi driver: when another owner initialized Wi-Fi first,
/// the bootstrap reports AlreadyInitialized and assumes no ownership. When this instance initializes the driver itself it
/// also creates the default ESP event loop required by the Wi-Fi driver to publish lifecycle events. This deliberately does
/// not create esp_netif/LwIP networking. Shutdown() releases only resources this bootstrap owns.
/// </remarks>

class Raw80211WiFiBootstrap final {
private:
    Raw80211WiFiBootstrapConfiguration _configuration{};
    bool _ownsDriver{false};
    bool _ownsDefaultEventLoop{false};

    static constexpr int StaticTxBufferType = 0;

    bool ConfigurationValid() const noexcept {
        return _configuration.StaticRxBuffers >= 2U &&
               _configuration.DynamicRxBuffers >= _configuration.StaticRxBuffers &&
               _configuration.DynamicTxBuffers != 0U &&
               _configuration.StaticTxBuffers != 0U;
    }

    void ReleaseOwnedResources() noexcept {
        if (_ownsDriver) {
            (void)esp_wifi_stop();
            (void)esp_wifi_deinit();
            _ownsDriver = false;
        }
        if (_ownsDefaultEventLoop) {
            (void)esp_event_loop_delete_default();
            _ownsDefaultEventLoop = false;
        }
    }

public:
    explicit Raw80211WiFiBootstrap(
        Raw80211WiFiBootstrapConfiguration configuration = {}
    ) noexcept : _configuration(configuration) {}

    Raw80211WiFiBootstrap(const Raw80211WiFiBootstrap&) = delete;
    Raw80211WiFiBootstrap& operator=(const Raw80211WiFiBootstrap&) = delete;

    ~Raw80211WiFiBootstrap() {
        ReleaseOwnedResources();
    }

    Raw80211WiFiBootstrapResult Initialize() noexcept {
        if (_ownsDriver) {
            return {Raw80211WiFiBootstrapStatus::AlreadyInitialized, ESP_OK};
        }
        if (!ConfigurationValid()) {
            return {Raw80211WiFiBootstrapStatus::InvalidConfiguration, ESP_ERR_INVALID_ARG};
        }

        wifi_mode_t existingMode = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&existingMode) == ESP_OK && existingMode != WIFI_MODE_NULL) {
            // Another lifecycle already owns Wi-Fi. Do not mutate its event-loop, buffer, or feature policy.
            return {Raw80211WiFiBootstrapStatus::AlreadyInitialized, ESP_OK};
        }

        // The Wi-Fi driver posts WIFI_EVENT notifications even in a raw-only composition. Create only the default ESP
        // event loop; do not initialize esp_netif or LwIP. ESP_ERR_INVALID_STATE means another owner already created it.
        esp_err_t native = esp_event_loop_create_default();
        if (native == ESP_OK) {
            _ownsDefaultEventLoop = true;
        } else if (native != ESP_ERR_INVALID_STATE) {
            return {Raw80211WiFiBootstrapStatus::EventLoopInitializationFailed, native};
        }

        wifi_init_config_t driver = WIFI_INIT_CONFIG_DEFAULT();
        driver.static_rx_buf_num = static_cast<int>(_configuration.StaticRxBuffers);
        driver.dynamic_rx_buf_num = static_cast<int>(_configuration.DynamicRxBuffers);

        if (driver.tx_buf_type == StaticTxBufferType) {
            driver.static_tx_buf_num = static_cast<int>(_configuration.StaticTxBuffers);
        } else {
            driver.dynamic_tx_buf_num = static_cast<int>(_configuration.DynamicTxBuffers);
        }
        driver.cache_tx_buf_num = 0;
        driver.csi_enable = 0;
        if (_configuration.DisableAmpdu) {
            driver.ampdu_rx_enable = 0;
            driver.ampdu_tx_enable = 0;
            driver.amsdu_tx_enable = 0;
            driver.rx_ba_win = 1;
        }
        if (_configuration.DisableNvs) driver.nvs_enable = 0;

        native = esp_wifi_init(&driver);
        if (native != ESP_OK) {
            ReleaseOwnedResources();
            return {Raw80211WiFiBootstrapStatus::DriverInitializationFailed, native};
        }
        _ownsDriver = true;

        native = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (native != ESP_OK) {
            ReleaseOwnedResources();
            return {Raw80211WiFiBootstrapStatus::StorageConfigurationFailed, native};
        }

        native = esp_wifi_set_mode(WIFI_MODE_STA);
        if (native != ESP_OK) {
            ReleaseOwnedResources();
            return {Raw80211WiFiBootstrapStatus::ModeConfigurationFailed, native};
        }

        native = esp_wifi_start();
        if (native != ESP_OK) {
            ReleaseOwnedResources();
            return {Raw80211WiFiBootstrapStatus::DriverStartFailed, native};
        }

        return {Raw80211WiFiBootstrapStatus::Initialized, ESP_OK};
    }

    void Shutdown() noexcept {
        ReleaseOwnedResources();
    }

    bool OwnsDriver() const noexcept { return _ownsDriver; }
    bool OwnsDefaultEventLoop() const noexcept { return _ownsDefaultEventLoop; }

    const Raw80211WiFiBootstrapConfiguration& Configuration() const noexcept {
        return _configuration;
    }
};

} // namespace ESPressio::ESP32Platform
