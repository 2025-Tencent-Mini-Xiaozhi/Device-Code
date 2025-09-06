#ifndef _SERVER_CONFIG_H_
#define _SERVER_CONFIG_H_

#include <string>

/**
 * 服务器配置管理类
 * 从OTA配置中解析出各种业务服务器的地址
 */
class ServerConfig
{
public:
    static ServerConfig &GetInstance();

    /**
     * 初始化服务器配置
     * 从OTA地址或配网页面配置中解析服务器地址
     */
    void Initialize();

    /**
     * 获取图片上传服务器地址
     * @return 完整的上传URL，如 "http://server:8003/upload"
     */
    std::string GetUploadServerUrl() const;

    /**
     * 获取用户注册页面地址
     * @return 注册页面URL，如 "http://server:8001/"
     */
    std::string GetRegistrationServerUrl() const;

    /**
     * 获取巡检通知服务器地址
     * @return 巡检API URL，如 "http://server:8003/xiaozhi/push/message"
     */
    std::string GetInspectionServerUrl() const;

    /**
     * 获取基础服务器地址（不包含端口）
     * @return 服务器IP或域名，如 "8.138.251.153"
     */
    std::string GetBaseServerAddress() const;

private:
    ServerConfig() = default;
    ~ServerConfig() = default;

    // 禁止拷贝
    ServerConfig(const ServerConfig &) = delete;
    ServerConfig &operator=(const ServerConfig &) = delete;

    /**
     * 从OTA URL解析服务器地址
     * @param ota_url OTA服务器URL
     * @return 解析出的服务器地址
     */
    std::string ParseServerFromOtaUrl(const std::string &ota_url) const;

    /**
     * 获取当前配置的OTA地址
     * @return OTA服务器URL
     */
    std::string GetConfiguredOtaUrl() const;

    std::string base_server_address_; // 基础服务器地址
    bool initialized_ = false;
};

#endif // _SERVER_CONFIG_H_
