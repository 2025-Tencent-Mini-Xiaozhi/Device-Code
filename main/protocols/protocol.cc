#include "protocol.h"

#include <esp_log.h>

#define TAG "Protocol"

void Protocol::OnIncomingJson(std::function<void(const cJSON *root)> callback) { on_incoming_json_ = callback; }

void Protocol::OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback) { on_incoming_audio_ = callback; }

void Protocol::OnAudioChannelOpened(std::function<void()> callback) { on_audio_channel_opened_ = callback; }

void Protocol::OnAudioChannelClosed(std::function<void()> callback) { on_audio_channel_closed_ = callback; }

void Protocol::OnNetworkError(std::function<void(const std::string &message)> callback) { on_network_error_ = callback; }

void Protocol::SetError(const std::string &message)
{
    error_occurred_ = true;
    if (on_network_error_ != nullptr)
    {
        on_network_error_(message);
    }
}

void Protocol::SendAbortSpeaking(AbortReason reason)
{
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"abort\"";
    if (reason == kAbortReasonWakeWordDetected)
    {
        message += ",\"reason\":\"wake_word_detected\"";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendWakeWordDetected(const std::string &wake_word, const std::string &user_info)
{
    // 构建JSON消息，将用户信息作为独立的JSON对象而不是字符串
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "session_id", session_id_.c_str());
    cJSON_AddStringToObject(json, "type", "listen");
    cJSON_AddStringToObject(json, "state", "detect");
    cJSON_AddStringToObject(json, "text", wake_word.c_str());

    // 如果有用户信息，解析并添加为JSON对象
    if (!user_info.empty())
    {
        cJSON *user_info_json = cJSON_Parse(user_info.c_str());
        if (user_info_json != nullptr)
        {
            cJSON_AddItemToObject(json, "user_info", user_info_json);
        }
        else
        {
            ESP_LOGW("Protocol", "Failed to parse user_info JSON, sending as text");
            // 如果解析失败，回退到原来的方式
            std::string text = wake_word + "|" + user_info;
            cJSON *text_item = cJSON_GetObjectItem(json, "text");
            cJSON_SetValuestring(text_item, text.c_str());
        }
    }

    // 转换为字符串并发送
    char *json_string = cJSON_PrintUnformatted(json);
    std::string message(json_string);
    cJSON_free(json_string);
    cJSON_Delete(json);

    SendText(message);
}

void Protocol::SendStartListening(ListeningMode mode)
{
    std::string message = "{\"session_id\":\"" + session_id_ + "\"";
    message += ",\"type\":\"listen\",\"state\":\"start\"";
    if (mode == kListeningModeRealtime)
    {
        message += ",\"mode\":\"realtime\"";
    }
    else if (mode == kListeningModeAutoStop)
    {
        message += ",\"mode\":\"auto\"";
    }
    else
    {
        message += ",\"mode\":\"manual\"";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendStopListening()
{
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"stop\"}";
    SendText(message);
}

void Protocol::SendMcpMessage(const std::string &payload)
{
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"mcp\",\"payload\":" + payload + "}";
    SendText(message);
}

bool Protocol::IsTimeout() const
{
    const int kTimeoutSeconds = 120;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout)
    {
        ESP_LOGE(TAG, "Channel timeout %ld seconds", (long)duration.count());
    }
    return timeout;
}

bool Protocol::IsTimeout(bool check_timeout) const
{
    if (!check_timeout)
    {
        return false; // 在待命状态下不进行超时检查
    }
    return IsTimeout();
}
