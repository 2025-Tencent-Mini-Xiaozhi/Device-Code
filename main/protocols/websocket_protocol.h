#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_

#include "device_state.h"
#include "protocol.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <web_socket.h>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class WebsocketProtocol : public Protocol
{
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

    void SetDeviceState(DeviceState state); // 设置设备状态以控制超时行为

private:
    EventGroupHandle_t event_group_handle_;
    WebSocket *websocket_ = nullptr;
    int version_ = 1;
    bool audio_channel_active_ = false;              // 音频通道状态
    bool connection_established_ = false;            // 连接状态
    DeviceState device_state_ = kDeviceStateUnknown; // 设备状态

    void ParseServerHello(const cJSON *root);
    bool SendText(const std::string &text) override;
    std::string GetHelloMessage();
    bool EstablishConnection(); // 建立基础连接
    void DisconnectWebSocket(); // 断开连接
};

#endif
