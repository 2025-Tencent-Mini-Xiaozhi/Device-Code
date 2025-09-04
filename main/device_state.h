#ifndef _DEVICE_STATE_H_
#define _DEVICE_STATE_H_

enum DeviceState
{
    kDeviceStateUnknown,
    kDeviceStateStarting,
    kDeviceStateWifiConfiguring,
    kDeviceStateIdle,
    kDeviceStateConnecting,
    kDeviceStateListening,
    kDeviceStateSpeaking,
    kDeviceStateUpgrading,
    kDeviceStateActivating,
    kDeviceStateAudioTesting,
    kDeviceStateFatalError,
    kDeviceStateLogin // 添加这行
};

#endif // _DEVICE_STATE_H_