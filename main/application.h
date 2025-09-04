#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio_service.h"
#include "device_state_event.h"
#include "ota.h"
#include "protocol.h"
#include "user_manager.h"

#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_CHECK_NEW_VERSION_DONE (1 << 5)

enum AecMode
{
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application
{
private:
    UserManager user_manager_;
    bool is_device_activated_ = false; // 设备激活状态，独立于用户登录状态
    esp_timer_handle_t camera_preview_timer_ = nullptr;
    esp_timer_handle_t camera_upload_timer_ = nullptr; // 新增上传定时器
    esp_timer_handle_t inspection_timer_ = nullptr;    // 新增巡检定时器
    esp_timer_handle_t auto_logout_timer_ = nullptr;   // 新增24小时自动登出定时器
    esp_timer_handle_t daily_check_timer_ = nullptr;   // 新增每日检查定时器（1小时一次）
    int camera_upload_count_ = 0;                      // 上传计数器
    static const int MAX_UPLOAD_COUNT = 10;            // 最大上传次数

public:
    UserManager &GetUserManager() { return user_manager_; }
    void StartCameraPreview();
    void StopCameraPreview();
    void StartCameraUpload();      // 新增上传控制方法
    void StopCameraUpload();       // 新增上传控制方法
    void StartInspectionTimer();   // 新增巡检定时器方法
    void StopInspectionTimer();    // 停止巡检定时器方法
    void ClearInspectionFlags();   // 清除巡检标志方法
    void StartAutoLogoutTimer();   // 新增24小时自动登出定时器方法
    void StopAutoLogoutTimer();    // 停止自动登出定时器方法
    void StartDailyCheckTimer();   // 新增每日检查定时器方法
    void StopDailyCheckTimer();    // 停止每日检查定时器方法
    void ShowRegistrationPrompt(); // 显示注册提示
    void TriggerWakeWordFlow();    // 触发唤醒流程
    static void CameraPreviewCallback(void *arg);
    static void CameraUploadCallback(void *arg); // 新增上传回调
    static void InspectionCallback(void *arg);   // 新增巡检回调
    static void AutoLogoutCallback(void *arg);   // 新增自动登出回调
    static void DailyCheckCallback(void *arg);   // 新增每日检查回调
    void UploadCameraImage(Camera *camera);
    void SendInspectionRequest(); // 发送巡检请求
    void PerformAutoLogout();     // 执行自动登出
    void CheckDailyExpiration();  // 执行每日过期检查
    static Application &GetInstance()
    {
        static Application instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    void Start();
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    void Schedule(std::function<void()> callback);
    void SetDeviceState(DeviceState state);
    void Alert(const char *status, const char *message, const char *emotion = "", const std::string_view &sound = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void Reboot();
    void WakeWordInvoke(const std::string &wake_word);
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string &payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view &sound);
    AudioService &GetAudioService() { return audio_service_; }

    // 设备激活状态管理
    bool IsDeviceActivated() const { return is_device_activated_; }
    void LoadDeviceActivationStatus();               // 从持久化存储加载设备激活状态
    void SaveDeviceActivationStatus(bool activated); // 保存设备激活状态到持久化存储
    void CheckDeviceActivationAfterLogin();          // 登录成功后检查设备激活状态
    void ShowDeviceActivationPrompt();               // 显示设备激活提示

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool tts_session_active_ = false;             // 跟踪TTS会话状态
    bool pending_inspection_after_login_ = false; // 标记登录后是否需要在TTS结束后的首次listening时发送巡检
    bool login_tts_completed_ = false;            // 标记登录后的TTS是否已完成
    int clock_ticks_ = 0;
    TaskHandle_t check_new_version_task_handle_ = nullptr;

    void MainEventLoop();
    void OnWakeWordDetected();
    void CheckNewVersion(Ota &ota);
    void ShowActivationCode(const std::string &code, const std::string &message);
    void OnClockTimer();
    void SetListeningMode(ListeningMode mode);
    std::string BuildUserInfoString() const; // 构建用户信息字符串
};

#endif // _APPLICATION_H_
