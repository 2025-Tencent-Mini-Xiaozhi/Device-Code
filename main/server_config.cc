#include "server_config.h"
#include "ota.h"
#include "settings.h"
#include "wifi_configuration_ap.h"
#include <algorithm>
#include <esp_log.h>

#define TAG "ServerConfig"

// 默认的业务服务器地址（当无法从OTA地址解析时使用）
#define DEFAULT_BUSINESS_SERVER "8.138.251.153"

ServerConfig &ServerConfig::GetInstance()
{
    static ServerConfig instance;
    return instance;
}

void ServerConfig::Initialize()
{
    if (initialized_)
    {
        return;
    }

    std::string ota_url = GetConfiguredOtaUrl();
    ESP_LOGI(TAG, "Current OTA URL: %s", ota_url.c_str());

    // 尝试从OTA URL解析服务器地址
    base_server_address_ = ParseServerFromOtaUrl(ota_url);

    // 如果解析失败，使用默认地址
    if (base_server_address_.empty())
    {
        base_server_address_ = DEFAULT_BUSINESS_SERVER;
        ESP_LOGW(TAG, "Failed to parse server from OTA URL, using default: %s", base_server_address_.c_str());
    }
    else
    {
        ESP_LOGI(TAG, "Parsed business server address: %s", base_server_address_.c_str());
    }

    initialized_ = true;
}

std::string ServerConfig::GetUploadServerUrl() const
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "ServerConfig not initialized");
        return "http://" DEFAULT_BUSINESS_SERVER ":8003/upload";
    }
    return "http://" + base_server_address_ + ":8003/upload";
}

std::string ServerConfig::GetRegistrationServerUrl() const
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "ServerConfig not initialized");
        return "http://" DEFAULT_BUSINESS_SERVER ":8001/";
    }
    return "http://" + base_server_address_ + ":8001/";
}

std::string ServerConfig::GetInspectionServerUrl() const
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "ServerConfig not initialized");
        return "http://" DEFAULT_BUSINESS_SERVER ":8003/xiaozhi/push/message";
    }
    return "http://" + base_server_address_ + ":8003/xiaozhi/push/message";
}

std::string ServerConfig::GetBaseServerAddress() const
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "ServerConfig not initialized");
        return DEFAULT_BUSINESS_SERVER;
    }
    return base_server_address_;
}

std::string ServerConfig::ParseServerFromOtaUrl(const std::string &ota_url) const
{
    if (ota_url.empty())
    {
        return "";
    }

    // 解析URL格式: http://server:port/path 或 https://server:port/path
    size_t protocol_end = ota_url.find("://");
    if (protocol_end == std::string::npos)
    {
        ESP_LOGE(TAG, "Invalid OTA URL format: %s", ota_url.c_str());
        return "";
    }

    size_t host_start = protocol_end + 3;
    size_t host_end = ota_url.find(':', host_start);
    if (host_end == std::string::npos)
    {
        // 没有端口号，查找路径分隔符
        host_end = ota_url.find('/', host_start);
        if (host_end == std::string::npos)
        {
            host_end = ota_url.length();
        }
    }

    if (host_start >= host_end)
    {
        ESP_LOGE(TAG, "Cannot parse host from OTA URL: %s", ota_url.c_str());
        return "";
    }

    std::string server_address = ota_url.substr(host_start, host_end - host_start);

    // 验证解析结果
    if (server_address.empty())
    {
        ESP_LOGE(TAG, "Empty server address parsed from OTA URL: %s", ota_url.c_str());
        return "";
    }

    ESP_LOGI(TAG, "Parsed server address '%s' from OTA URL '%s'", server_address.c_str(), ota_url.c_str());
    return server_address;
}

std::string ServerConfig::GetConfiguredOtaUrl() const
{
    // 首先尝试从WiFi配置中获取用户设置的OTA地址
    auto &wifi_ap = WifiConfigurationAp::GetInstance();
    std::string user_ota_url = wifi_ap.GetOtaUrl();

    if (!user_ota_url.empty())
    {
        ESP_LOGI(TAG, "Using user configured OTA URL: %s", user_ota_url.c_str());
        return user_ota_url;
    }

    // 然后尝试从Settings获取（这与Ota::GetCheckVersionUrl()逻辑一致）
    Settings settings("wifi", false);
    std::string settings_ota_url = settings.GetString("ota_url");

    if (!settings_ota_url.empty())
    {
        ESP_LOGI(TAG, "Using settings OTA URL: %s", settings_ota_url.c_str());
        return settings_ota_url;
    }

    // 最后使用编译时配置的默认地址
#ifdef CONFIG_OTA_URL
    std::string default_ota_url = CONFIG_OTA_URL;
#else
    std::string default_ota_url = "http://8.155.160.71:8002/xiaozhi/ota/";
#endif
    ESP_LOGI(TAG, "Using default OTA URL: %s", default_ota_url.c_str());
    return default_ota_url;
}
