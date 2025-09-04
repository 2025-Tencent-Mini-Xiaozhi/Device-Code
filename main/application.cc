#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "font_awesome_symbols.h"
#include "mcp_server.h"
#include "mqtt_protocol.h"
#include "settings.h"
#include "system_info.h"
#include "websocket_protocol.h"

#include <arpa/inet.h>
#include <cJSON.h>
#include <cctype>
#include <cstring>
#include <driver/gpio.h>
#include <esp_camera.h>
#include <esp_log.h>
#include <img_converters.h>

// 包含相机类头文件
#include "boards/common/esp32_camera.h"
#include "boards/sensecap-watcher/sscma_camera.h"

#define WEBSOCKET_CONNECT_GAP 5000 // 重连间隔,5s

// Unicode解码函数
std::string DecodeUnicodeEscapes(const std::string &input)
{
    std::string result;
    result.reserve(input.length());

    for (size_t i = 0; i < input.length(); ++i)
    {
        if (input[i] == '\\' && i + 1 < input.length() && input[i + 1] == 'u' && i + 5 < input.length())
        {
            // 解析 \uXXXX 格式的Unicode转义序列
            std::string hex_str = input.substr(i + 2, 4);
            char *end_ptr;
            unsigned long unicode_value = strtoul(hex_str.c_str(), &end_ptr, 16);

            if (end_ptr == hex_str.c_str() + 4) // 确保解析成功
            {
                // 将Unicode码点转换为UTF-8
                if (unicode_value <= 0x7F)
                {
                    // 1字节UTF-8
                    result += static_cast<char>(unicode_value);
                }
                else if (unicode_value <= 0x7FF)
                {
                    // 2字节UTF-8
                    result += static_cast<char>(0xC0 | (unicode_value >> 6));
                    result += static_cast<char>(0x80 | (unicode_value & 0x3F));
                }
                else if (unicode_value <= 0xFFFF)
                {
                    // 3字节UTF-8
                    result += static_cast<char>(0xE0 | (unicode_value >> 12));
                    result += static_cast<char>(0x80 | ((unicode_value >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (unicode_value & 0x3F));
                }
                i += 5; // 跳过 \uXXXX
            }
            else
            {
                result += input[i]; // 解析失败，保持原字符
            }
        }
        else
        {
            result += input[i];
        }
    }

    return result;
}

#define TAG "Application"

static const char *const STATE_STRINGS[] = {
    "unknown", "starting", "configuring", "idle", "connecting", "listening", "speaking", "upgrading", "activating", "audio_testing", "fatal_error", "invalid_state", "login",
};

Application::Application()
{
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void *arg)
                                                {
                                                    Application *app = (Application *)arg;
                                                    app->OnClockTimer();
                                                },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application()
{
    if (clock_timer_handle_ != nullptr)
    {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (inspection_timer_ != nullptr)
    {
        esp_timer_stop(inspection_timer_);
        esp_timer_delete(inspection_timer_);
    }
    if (auto_logout_timer_ != nullptr)
    {
        esp_timer_stop(auto_logout_timer_);
        esp_timer_delete(auto_logout_timer_);
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckNewVersion(Ota &ota)
{
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    auto &board = Board::GetInstance();
    auto display = board.GetDisplay();

    // 所有设备都进行版本检查，不管是否激活
    ESP_LOGI(TAG, "Starting version check for all devices (activation will be checked after user login)");

    while (true)
    {
        SetDeviceState(kDeviceStateActivating); // 这里只是版本检查状态
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);
        if (!ota.CheckVersion())
        {
            retry_count++;
            if (retry_count >= MAX_RETRY)
            {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[128];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer, "sad", Lang::Sounds::P3_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++)
            {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle)
                {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion())
        {
            Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "happy", Lang::Sounds::P3_UPGRADE);

            vTaskDelay(pdMS_TO_TICKS(3000));

            SetDeviceState(kDeviceStateUpgrading);

            display->SetIcon(FONT_AWESOME_DOWNLOAD);
            std::string message = std::string(Lang::Strings::NEW_VERSION) + ota.GetFirmwareVersion();
            display->SetChatMessage("system", message.c_str());

            board.SetPowerSaveMode(false);
            audio_service_.Stop();
            vTaskDelay(pdMS_TO_TICKS(1000));

            bool upgrade_success = ota.StartUpgrade(
                [display](int progress, size_t speed)
                {
                    char buffer[64];
                    snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                    display->SetChatMessage("system", buffer);
                });

            if (!upgrade_success)
            {
                // Upgrade failed, restart audio service and continue running
                ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
                audio_service_.Start();       // Restart audio service
                board.SetPowerSaveMode(true); // Restore power save mode
                Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "sad", Lang::Sounds::P3_EXCLAMATION);
                vTaskDelay(pdMS_TO_TICKS(3000));
                // Continue to normal operation (don't break, just fall through)
            }
            else
            {
                // Upgrade success, reboot immediately
                ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
                display->SetChatMessage("system", "Upgrade successful, rebooting...");
                vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
                Reboot();
                return; // This line will never be reached after reboot
            }
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();

        // 所有设备都跳过激活流程，直接进入待命状态
        // 激活检查将在用户登录成功后进行
        ESP_LOGI(TAG, "Version check completed, skipping activation - activation will be checked after user login");
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
        break;
    }
}

void Application::ShowActivationCode(const std::string &code, const std::string &message)
{
    struct digit_sound
    {
        char digit;
        const std::string_view &sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{digit_sound{'0', Lang::Sounds::P3_0}, digit_sound{'1', Lang::Sounds::P3_1}, digit_sound{'2', Lang::Sounds::P3_2}, digit_sound{'3', Lang::Sounds::P3_3}, digit_sound{'4', Lang::Sounds::P3_4}, digit_sound{'5', Lang::Sounds::P3_5}, digit_sound{'6', Lang::Sounds::P3_6}, digit_sound{'7', Lang::Sounds::P3_7}, digit_sound{'8', Lang::Sounds::P3_8}, digit_sound{'9', Lang::Sounds::P3_9}}};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION);

    for (const auto &digit : code)
    {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(), [digit](const digit_sound &ds) { return ds.digit == digit; });
        if (it != digit_sounds.end())
        {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char *status, const char *message, const char *emotion, const std::string_view &sound)
{
    ESP_LOGW(TAG, "Alert %s: %s [%s]", status, message, emotion);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty())
    {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert()
{
    if (device_state_ == kDeviceStateIdle)
    {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState()
{
    if (device_state_ == kDeviceStateActivating)
    {
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    else if (device_state_ == kDeviceStateWifiConfiguring)
    {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }
    else if (device_state_ == kDeviceStateAudioTesting)
    {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_)
    {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle)
    {
        Schedule(
            [this]()
            {
                // 关闭待命状态的连接，让对话使用新的连接
                if (protocol_->IsAudioChannelOpened())
                {
                    ESP_LOGI(TAG, "Closing standby WebSocket connection to enter conversation");
                    protocol_->CloseAudioChannel();
                }

                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel())
                {
                    return;
                }

                SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
            });
    }
    else if (device_state_ == kDeviceStateSpeaking)
    {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    }
    else if (device_state_ == kDeviceStateListening)
    {
        Schedule(
            [this]()
            {
                // 发送停止监听命令，但保持连接以接收通知
                protocol_->SendStopListening();
                SetDeviceState(kDeviceStateIdle); // 这会触发待命状态的连接保持逻辑
            });
    }
}

void Application::StartListening()
{
    if (device_state_ == kDeviceStateActivating)
    {
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    else if (device_state_ == kDeviceStateWifiConfiguring)
    {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_)
    {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle)
    {
        Schedule(
            [this]()
            {
                // 关闭待命状态的连接，让对话使用新的连接
                if (protocol_->IsAudioChannelOpened())
                {
                    ESP_LOGI(TAG, "Closing standby WebSocket connection to enter conversation");
                    protocol_->CloseAudioChannel();
                }

                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel())
                {
                    return;
                }

                SetListeningMode(kListeningModeManualStop);
            });
    }
    else if (device_state_ == kDeviceStateSpeaking)
    {
        Schedule(
            [this]()
            {
                AbortSpeaking(kAbortReasonNone);
                SetListeningMode(kListeningModeManualStop);
            });
    }
}

void Application::StopListening()
{
    if (device_state_ == kDeviceStateAudioTesting)
    {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end())
    {
        return;
    }

    Schedule(
        [this]()
        {
            if (device_state_ == kDeviceStateListening)
            {
                protocol_->SendStopListening();
                SetDeviceState(kDeviceStateIdle);
            }
        });
}

void Application::Start()
{

    ESP_LOGI(TAG, "Starting application with user management");

    // 加载设备激活状态（独立于用户登录状态）
    LoadDeviceActivationStatus();

    // 加载用户信息
    user_manager_.LoadUserInfo();

    // 如果用户已登录，打印用户信息
    if (user_manager_.IsLoggedIn())
    {
        ESP_LOGI(TAG, "=== Device startup - User already logged in ===");
        ESP_LOGI(TAG, "User %s is already logged in - no inspection will be triggered", user_manager_.GetName().c_str());
        ESP_LOGI(TAG, "Inspection is only triggered on fresh photo authentication login");
        user_manager_.PrintUserInfo();

        // 启动每日检查定时器，确保过期登录会被自动清除
        ESP_LOGI(TAG, "Starting daily check timer for existing logged-in user");
        StartDailyCheckTimer();
    }
    else
    {
        ESP_LOGI(TAG, "Device startup - No user logged in");
        ESP_LOGI(TAG, "User will need to authenticate with photo to login and trigger inspection");
    }

    auto &board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() { xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO); };
    callbacks.on_wake_word_detected = [this](const std::string &wake_word) { xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED); };
    callbacks.on_vad_change = [this](bool speaking) { xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE); };
    audio_service_.SetCallbacks(callbacks);

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new firmware version or get the MQTT broker address
    Ota ota;
    CheckNewVersion(ota);

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
    McpServer::GetInstance().AddCommonTools();

    if (ota.HasMqttConfig())
    {
        protocol_ = std::make_unique<MqttProtocol>();
    }
    else if (ota.HasWebsocketConfig())
    {
        protocol_ = std::make_unique<WebsocketProtocol>();
    }
    else
    {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnNetworkError(
        [this](const std::string &message)
        {
            last_error_message_ = message;
            xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
        });
    protocol_->OnIncomingAudio(
        [this](std::unique_ptr<AudioStreamPacket> packet)
        {
            if (device_state_ == kDeviceStateSpeaking)
            {
                audio_service_.PushPacketToDecodeQueue(std::move(packet));
            }
        });
    protocol_->OnAudioChannelOpened(
        [this, codec, &board]()
        {
            board.SetPowerSaveMode(false);
            if (protocol_->server_sample_rate() != codec->output_sample_rate())
            {
                ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion", protocol_->server_sample_rate(), codec->output_sample_rate());
            }
        });
    protocol_->OnAudioChannelClosed(
        [this, &board]()
        {
            board.SetPowerSaveMode(true);
            Schedule(
                [this]()
                {
                    auto display = Board::GetInstance().GetDisplay();
                    display->SetChatMessage("system", "");
                    SetDeviceState(kDeviceStateIdle);
                });
        });
    protocol_->OnIncomingJson(
        [this, display](const cJSON *root)
        {
            // Parse JSON data
            auto type = cJSON_GetObjectItem(root, "type");
            if (strcmp(type->valuestring, "tts") == 0)
            {
                auto state = cJSON_GetObjectItem(root, "state");
                if (strcmp(state->valuestring, "start") == 0)
                {
                    Schedule(
                        [this]()
                        {
                            // 标记TTS会话开始，但不立即进入说话状态
                            // 只有在收到sentence_start时才真正进入说话状态
                            aborted_ = false;
                            tts_session_active_ = true;
                            ESP_LOGI(TAG, "TTS session started, waiting for sentence_start");
                        });
                }
                else if (strcmp(state->valuestring, "stop") == 0)
                {
                    Schedule(
                        [this]()
                        {
                            tts_session_active_ = false;

                            // 如果这是登录后的TTS会话结束，标记可以在下次listening时触发巡检
                            if (pending_inspection_after_login_ && !login_tts_completed_)
                            {
                                login_tts_completed_ = true;
                                ESP_LOGI(TAG, "Login TTS session ended, will send inspection request on next listening state");
                            }

                            if (device_state_ == kDeviceStateSpeaking)
                            {
                                if (listening_mode_ == kListeningModeManualStop)
                                {
                                    SetDeviceState(kDeviceStateIdle);
                                }
                                else
                                {
                                    SetDeviceState(kDeviceStateListening);
                                }
                            }
                            ESP_LOGI(TAG, "TTS session ended");
                        });
                }
                else if (strcmp(state->valuestring, "sentence_start") == 0)
                {
                    auto text = cJSON_GetObjectItem(root, "text");
                    if (cJSON_IsString(text))
                    {
                        ESP_LOGI(TAG, "<< %s", text->valuestring);
                        Schedule(
                            [this, display, message = std::string(text->valuestring)]()
                            {
                                // 只有在TTS会话活跃时才进入说话状态
                                if (tts_session_active_ && (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening))
                                {
                                    SetDeviceState(kDeviceStateSpeaking);
                                }
                                display->SetChatMessage("assistant", message.c_str());
                            });
                    }
                }
            }
            else if (strcmp(type->valuestring, "stt") == 0)
            {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text))
                {
                    std::string message = std::string(text->valuestring);
                    ESP_LOGI(TAG, ">> %s", message.c_str());

                    // 检查消息是否包含敏感用户信息，如果包含则不显示在屏幕上
                    bool contains_sensitive_info = (message.find("\"password\"") != std::string::npos || message.find("\"api_key\"") != std::string::npos || message.find("\"api_id\"") != std::string::npos || message.find("\"account\"") != std::string::npos || message.find("\"device_id\"") != std::string::npos || message.find("hide") != std::string::npos // 新增过滤词
                    );

                    if (!contains_sensitive_info)
                    {
                        Schedule([this, display, message]() { display->SetChatMessage("user", message.c_str()); });
                    }
                    else
                    {
                        ESP_LOGI(TAG, "Skipping display of sensitive user info message");
                    }
                }
            }
            else if (strcmp(type->valuestring, "llm") == 0)
            {
                auto emotion = cJSON_GetObjectItem(root, "emotion");
                if (cJSON_IsString(emotion))
                {
                    Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() { display->SetEmotion(emotion_str.c_str()); });
                }
            }
            else if (strcmp(type->valuestring, "mcp") == 0)
            {
                auto payload = cJSON_GetObjectItem(root, "payload");
                if (cJSON_IsObject(payload))
                {
                    McpServer::GetInstance().ParseMessage(payload);
                }
            }
            else if (strcmp(type->valuestring, "system") == 0)
            {
                auto command = cJSON_GetObjectItem(root, "command");
                if (cJSON_IsString(command))
                {
                    ESP_LOGI(TAG, "System command: %s", command->valuestring);
                    if (strcmp(command->valuestring, "reboot") == 0)
                    {
                        // Do a reboot if user requests a OTA update
                        Schedule([this]() { Reboot(); });
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                    }
                }
            }
            else if (strcmp(type->valuestring, "alert") == 0)
            {
                auto status = cJSON_GetObjectItem(root, "status");
                auto message = cJSON_GetObjectItem(root, "message");
                auto emotion = cJSON_GetObjectItem(root, "emotion");
                if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion))
                {
                    Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::P3_VIBRATION);
                }
                else
                {
                    ESP_LOGW(TAG, "Alert command requires status, message and emotion");
                }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
            }
            else if (strcmp(type->valuestring, "custom") == 0)
            {
                auto payload = cJSON_GetObjectItem(root, "payload");
                ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
                if (cJSON_IsObject(payload))
                {
                    Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() { display->SetChatMessage("system", payload_str.c_str()); });
                }
                else
                {
                    ESP_LOGW(TAG, "Invalid custom message format: missing payload");
                }
#endif
            }
            else
            {
                ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
            }
        });
    bool protocol_started = protocol_->Start();

    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota.HasServerTime();
    if (protocol_started)
    {
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::P3_SUCCESS);
    }

    // Print heap stats
    SystemInfo::PrintHeapStats();

    // Enter the main event loop
    MainEventLoop();
}

void Application::OnClockTimer()
{
    clock_ticks_++;

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar();

    // Print the debug info every 10 seconds
    if (clock_ticks_ % 10 == 0)
    {
        // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
        // SystemInfo::PrintTaskList();
        SystemInfo::PrintHeapStats();
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop()
{
    // Raise the priority of the main event loop to avoid being interrupted by background tasks (which has priority 2)
    vTaskPrioritySet(NULL, 3);

    while (true)
    {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED | MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & MAIN_EVENT_ERROR)
        {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "sad", Lang::Sounds::P3_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO)
        {
            while (auto packet = audio_service_.PopPacketFromSendQueue())
            {
                if (!protocol_->SendAudio(std::move(packet)))
                {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED)
        {
            OnWakeWordDetected();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE)
        {
            if (device_state_ == kDeviceStateListening)
            {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto &task : tasks)
            {
                task();
            }
        }
    }
}

void Application::OnWakeWordDetected()
{
    if (!protocol_)
    {
        return;
    }

    if (device_state_ == kDeviceStateIdle)
    {
        // 检查用户是否已登录
        if (!user_manager_.IsLoggedIn())
        {
            // 关闭待命状态的连接
            if (protocol_->IsAudioChannelOpened())
            {
                ESP_LOGI(TAG, "Closing standby WebSocket connection to enter login");
                protocol_->CloseAudioChannel();
            }
            SetDeviceState(kDeviceStateLogin);
            return;
        }

        // 原有的连接逻辑保持不变
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened())
        {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel())
            {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket())
        {
            protocol_->SendAudio(std::move(packet));
        }
        // 构建用户信息并发送唤醒词检测消息
        std::string user_info = BuildUserInfoString();
        protocol_->SendWakeWordDetected(wake_word, user_info);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::P3_POPUP);
#endif
    }
    else if (device_state_ == kDeviceStateSpeaking)
    {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    }
    else if (device_state_ == kDeviceStateActivating)
    {
        SetDeviceState(kDeviceStateIdle);
    }
}

std::string Application::BuildUserInfoString() const
{
    if (!user_manager_.IsLoggedIn())
    {
        return "我还没有登录。";
    }

    std::string name = user_manager_.GetName();
    std::string result = "我的名字是" + (name.empty() ? "未知用户" : name) + "。";

    const auto &schedules = user_manager_.GetTodaySchedules();
    if (!schedules.empty())
    {
        result += "我今天的日程有：";
        for (size_t i = 0; i < schedules.size(); ++i)
        {
            result += schedules[i].content + "(" + schedules[i].status_text + ")";
            if (i != schedules.size() - 1)
            {
                result += "，";
            }
            else
            {
                result += "。";
            }
        }
    }
    else
    {
        result += "我今天没有日程安排。";
    }

    ESP_LOGI(TAG, "Built user info NL string: %s", result.c_str());
    return result + "hide";
}

void Application::AbortSpeaking(AbortReason reason)
{
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    protocol_->SendAbortSpeaking(reason);
}

void Application::SetListeningMode(ListeningMode mode)
{
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state)
{
    if (device_state_ == state)
    {
        return;
    }

    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // 通知协议类设备状态变化，以控制超时行为
    if (protocol_)
    {
        protocol_->SetDeviceState(state);
    }

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto &board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state)
    {
    case kDeviceStateUnknown:
    case kDeviceStateIdle:
        StopCameraPreview(); // 停止预览
        StopCameraUpload();  // 停止上传
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", ""); // 清除聊天消息
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.EnableWakeWordDetection(true);

        // 只有在用户已登录的情况下，才在待命状态建立WebSocket连接以接收服务器通知
        if (user_manager_.IsLoggedIn() && protocol_ && !protocol_->IsAudioChannelOpened())
        {
            ESP_LOGI(TAG, "User is logged in, scheduling delayed WebSocket connection for standby notifications (2s delay)");
            Schedule(
                [this]()
                {
                    // 延时2秒再连接，防止服务端还未及时清除旧的连接
                    vTaskDelay(pdMS_TO_TICKS(WEBSOCKET_CONNECT_GAP));

                    // 再次检查状态，确保仍然在待命状态且用户仍然登录
                    if (device_state_ == kDeviceStateIdle && user_manager_.IsLoggedIn() && !protocol_->IsAudioChannelOpened())
                    {
                        ESP_LOGI(TAG, "Opening WebSocket connection for standby notifications after 2s delay");
                        if (!protocol_->OpenAudioChannel())
                        {
                            ESP_LOGW(TAG, "Failed to open WebSocket connection in standby mode");
                        }
                    }
                    else
                    {
                        ESP_LOGI(TAG, "Device state or login status changed during delay, skipping WebSocket connection");
                    }
                });
        }
        else if (!user_manager_.IsLoggedIn())
        {
            ESP_LOGI(TAG, "User not logged in, skipping WebSocket connection in standby mode");
        }
        break;
    case kDeviceStateConnecting:
        StopCameraPreview(); // 停止预览
        StopCameraUpload();  // 停止上传
        display->SetStatus(Lang::Strings::CONNECTING);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
        break;
    case kDeviceStateListening:
        StopCameraPreview(); // 停止预览
        StopCameraUpload();  // 停止上传
        display->SetStatus(Lang::Strings::LISTENING);
        display->SetEmotion("neutral");

        // 检查是否需要在登录后的首次listening状态发送巡检请求
        ESP_LOGI(TAG, "Entering listening state, pending_inspection_after_login_: %s, login_tts_completed_: %s", pending_inspection_after_login_ ? "true" : "false", login_tts_completed_ ? "true" : "false");
        if (pending_inspection_after_login_ && login_tts_completed_)
        {
            ESP_LOGI(TAG, "First listening state after login TTS completed, sending inspection request");
            pending_inspection_after_login_ = false; // 清除标志
            login_tts_completed_ = false;            // 清除TTS完成标志
            SendInspectionRequest();
        }

        // Make sure the audio processor is running
        if (!audio_service_.IsAudioProcessorRunning())
        {
            // Send the start listening command
            protocol_->SendStartListening(listening_mode_);
            audio_service_.EnableVoiceProcessing(true);
            audio_service_.EnableWakeWordDetection(false);
        }
        break;
    case kDeviceStateSpeaking:
        StopCameraPreview(); // 停止预览
        StopCameraUpload();  // 停止上传
        display->SetStatus(Lang::Strings::SPEAKING);

        if (listening_mode_ != kListeningModeRealtime)
        {
            audio_service_.EnableVoiceProcessing(false);
            // Only AFE wake word can be detected in speaking mode
#if CONFIG_USE_AFE_WAKE_WORD
            audio_service_.EnableWakeWordDetection(true);
#else
            audio_service_.EnableWakeWordDetection(false);
#endif
        }
        audio_service_.ResetDecoder();
        break;
    case kDeviceStateLogin: {
        // 获取设备MAC地址的后三部分作为设备码
        std::string mac_address = SystemInfo::GetMacAddress();
        std::string device_code;

        // 提取MAC地址的后三部分 (例如: "94:a9:90:2b:c8:58" -> "2B_C8_58")
        size_t last_colon = mac_address.rfind(':');
        if (last_colon != std::string::npos && last_colon >= 6)
        {
            std::string last_three = mac_address.substr(last_colon - 5); // 获取最后三段 "2b:c8:58"
            // 转换为大写并替换冒号为下划线
            for (char &c : last_three)
            {
                if (c == ':')
                    c = '_';
                else
                    c = std::toupper(c);
            }
            device_code = last_three;
        }
        else
        {
            device_code = "DEVICE"; // 如果解析失败，使用默认值
        }

        ESP_LOGI(TAG, "Displaying device code: %s", device_code.c_str());

        display->SetStatus(device_code.c_str());
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "正在采集人脸数据进行登录");
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.EnableWakeWordDetection(true);
        StartCameraPreview(); // 启动实时预览
        StartCameraUpload();  // 启动定时上传
    }
    break;
    default:
        // Do nothing
        break;
    }
}

void Application::Reboot()
{
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

void Application::WakeWordInvoke(const std::string &wake_word)
{
    if (device_state_ == kDeviceStateIdle)
    {
        ToggleChatState();
        Schedule(
            [this, wake_word]()
            {
                if (protocol_)
                {
                    std::string user_info = BuildUserInfoString();
                    protocol_->SendWakeWordDetected(wake_word, user_info);
                }
            });
    }
    else if (device_state_ == kDeviceStateSpeaking)
    {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    }
    else if (device_state_ == kDeviceStateListening)
    {
        Schedule(
            [this]()
            {
                if (protocol_)
                {
                    protocol_->CloseAudioChannel();
                }
            });
    }
}

bool Application::CanEnterSleepMode()
{
    if (device_state_ != kDeviceStateIdle)
    {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened())
    {
        return false;
    }

    if (!audio_service_.IsIdle())
    {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string &payload)
{
    Schedule(
        [this, payload]()
        {
            if (protocol_)
            {
                protocol_->SendMcpMessage(payload);
            }
        });
}

void Application::SetAecMode(AecMode mode)
{
    aec_mode_ = mode;
    Schedule(
        [this]()
        {
            auto &board = Board::GetInstance();
            auto display = board.GetDisplay();
            switch (aec_mode_)
            {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            }

            // If the AEC mode is changed, close the audio channel
            if (protocol_ && protocol_->IsAudioChannelOpened())
            {
                protocol_->CloseAudioChannel();
            }
        });
}

void Application::PlaySound(const std::string_view &sound) { audio_service_.PlaySound(sound); }

void Application::StartCameraPreview()
{
    auto &board = Board::GetInstance();
    auto camera = board.GetCamera();
    if (!camera)
    {
        return;
    }

    // 创建定时器，每500ms捕获一次图像
    esp_timer_create_args_t timer_args = {.callback = CameraPreviewCallback, .arg = this, .name = "camera_preview"};

    if (esp_timer_create(&timer_args, &camera_preview_timer_) == ESP_OK)
    {
        esp_timer_start_periodic(camera_preview_timer_, 500000); // 500ms
        ESP_LOGI(TAG, "Camera preview started");
    }
}

void Application::StopCameraPreview()
{
    if (camera_preview_timer_)
    {
        esp_timer_stop(camera_preview_timer_);
        esp_timer_delete(camera_preview_timer_);
        camera_preview_timer_ = nullptr;
        ESP_LOGI(TAG, "Camera preview stopped");
    }
}

void Application::CameraPreviewCallback(void *arg)
{
    auto *app = static_cast<Application *>(arg);
    auto &board = Board::GetInstance();
    auto camera = board.GetCamera();

    if (camera && app->GetDeviceState() == kDeviceStateLogin)
    {
        camera->Capture(); // 这会自动显示预览
    }
}

void Application::StartCameraUpload()
{
    auto &board = Board::GetInstance();
    auto camera = board.GetCamera();
    if (!camera)
    {
        return;
    }

    // 重置上传计数器
    camera_upload_count_ = 0;

    // 创建上传定时器，每3秒上传一次
    esp_timer_create_args_t timer_args = {.callback = CameraUploadCallback, .arg = this, .name = "camera_upload"};

    if (esp_timer_create(&timer_args, &camera_upload_timer_) == ESP_OK)
    {
        esp_timer_start_periodic(camera_upload_timer_, 3000000); // 3秒
        ESP_LOGI(TAG, "Camera upload started (will upload max %d images)", MAX_UPLOAD_COUNT);
    }
}

void Application::StopCameraUpload()
{
    if (camera_upload_timer_)
    {
        esp_timer_stop(camera_upload_timer_);
        esp_timer_delete(camera_upload_timer_);
        camera_upload_timer_ = nullptr;
        ESP_LOGI(TAG, "Camera upload stopped (uploaded %d/%d images)", camera_upload_count_, MAX_UPLOAD_COUNT);

        // 重置计数器
        camera_upload_count_ = 0;
    }
}

void Application::UploadCameraImage(Camera *camera)
{
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);

    // 获取原始相机数据
    CameraRawData raw_data = camera->GetRawData();

    if (raw_data.data == nullptr || raw_data.size == 0)
    {
        ESP_LOGE(TAG, "No valid raw data available from camera");
        return;
    }

    // 构造multipart/form-data请求体
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";
    std::string server_url = "http://8.138.251.153:8003/upload";

    // 生成带时间戳的文件名
    auto now = std::time(nullptr);
    std::string filename = "camera_" + std::to_string(now) + ".jpg";

    // 构造完整的请求体
    std::string request_body;

    // 添加图像元数据字段
    request_body += "--" + boundary + "\r\n";
    request_body += "Content-Disposition: form-data; name=\"width\"\r\n";
    request_body += "\r\n";
    request_body += std::to_string(raw_data.width) + "\r\n";

    request_body += "--" + boundary + "\r\n";
    request_body += "Content-Disposition: form-data; name=\"height\"\r\n";
    request_body += "\r\n";
    request_body += std::to_string(raw_data.height) + "\r\n";

    request_body += "--" + boundary + "\r\n";
    request_body += "Content-Disposition: form-data; name=\"format\"\r\n";
    request_body += "\r\n";
    request_body += std::to_string(raw_data.format) + "\r\n";

    // 构造图像数据字段头部
    request_body += "--" + boundary + "\r\n";
    request_body += "Content-Disposition: form-data; name=\"image\"; filename=\"" + filename + "\"\r\n";

    // 根据格式设置Content-Type
    if (raw_data.format == PIXFORMAT_JPEG)
    {
        request_body += "Content-Type: image/jpeg\r\n";
    }
    else
    {
        request_body += "Content-Type: application/octet-stream\r\n";
    }
    request_body += "\r\n";

    // 构造完整的请求体（包括图像数据）
    std::string multipart_footer = "\r\n--" + boundary + "--\r\n";

    // 创建完整的请求体
    std::string complete_body;
    complete_body.reserve(request_body.size() + raw_data.size + multipart_footer.size());

    // 添加头部
    complete_body += request_body;

    // 添加图像数据
    complete_body.append((const char *)raw_data.data, raw_data.size);

    // 添加尾部
    complete_body += multipart_footer;

    ESP_LOGI(TAG, "=== HTTP Request Debug Info ===");
    ESP_LOGI(TAG, "URL: %s", server_url.c_str());
    ESP_LOGI(TAG, "Boundary: %s", boundary.c_str());
    ESP_LOGI(TAG, "Request body header size: %d bytes", (int)request_body.size());
    ESP_LOGI(TAG, "Image data size: %d bytes", (int)raw_data.size);
    ESP_LOGI(TAG, "Footer size: %d bytes", (int)multipart_footer.size());
    ESP_LOGI(TAG, "Total Content-Length: %d bytes", (int)complete_body.size());

    // 配置HTTP客户端
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    // 设置请求体内容
    http->SetContent(std::move(complete_body));

    ESP_LOGI(TAG, "=== HTTP Headers ===");
    ESP_LOGI(TAG, "Content-Type: multipart/form-data; boundary=%s", boundary.c_str());
    ESP_LOGI(TAG, "Device-Id: %s", SystemInfo::GetMacAddress().c_str());
    ESP_LOGI(TAG, "Client-Id: %s", Board::GetInstance().GetUuid().c_str());
    ESP_LOGI(TAG, "Request body header (first 200 chars): %.200s", request_body.c_str());

    if (!http->Open("POST", server_url))
    {
        ESP_LOGE(TAG, "Failed to connect to upload server");
        return;
    }

    ESP_LOGI(TAG, "=== Request sent successfully ===");

    int status_code = http->GetStatusCode();
    ESP_LOGI(TAG, "Server response status code: %d", status_code);

    // 读取服务器响应内容
    std::string response_body = http->ReadAll();
    ESP_LOGI(TAG, "=== Server Response ===");
    ESP_LOGI(TAG, "Response length: %d bytes", (int)response_body.length());

    if (!response_body.empty())
    {
        // 解码Unicode转义序列
        std::string decoded_response = DecodeUnicodeEscapes(response_body);
        ESP_LOGI(TAG, "Response content (decoded): %s", decoded_response.c_str());

        // 也打印原始响应用于调试
        ESP_LOGI(TAG, "Response content (raw): %s", response_body.c_str());

        // 解析服务器响应并更新用户信息
        if (user_manager_.ParseServerResponse(decoded_response))
        {
            ESP_LOGI(TAG, "User information updated successfully - stopping upload");

            // 识别成功，立即停止上传
            StopCameraUpload();

            // 检查设备激活状态
            CheckDeviceActivationAfterLogin();
        }
        else
        {
            ESP_LOGW(TAG, "Failed to parse server response or recognition failed");
        }
    }
    else
    {
        ESP_LOGW(TAG, "Server response body is empty");
    }

    if (status_code == 200)
    {
        ESP_LOGI(TAG, "Image uploaded successfully: %s", filename.c_str());
    }
    else
    {
        ESP_LOGE(TAG, "Failed to upload image, status code: %d", status_code);
    }

    http->Close();
}

void Application::ShowRegistrationPrompt()
{
    auto display = Board::GetInstance().GetDisplay();

    // 获取设备MAC地址的后三部分作为设备ID
    std::string mac_address = SystemInfo::GetMacAddress();
    std::string device_id;

    // 提取MAC地址的后三部分 (例如: "94:a9:90:2b:c8:58" -> "2b:c8:58")
    size_t last_colon = mac_address.rfind(':');
    if (last_colon != std::string::npos && last_colon >= 6)
    {
        device_id = mac_address.substr(last_colon - 5); // 获取最后三段
    }
    else
    {
        device_id = mac_address; // 如果解析失败，使用完整MAC地址
    }

    // 构造注册提示信息
    std::string registration_message = "请访问 http://8.138.251.153:8001/ 进行身份注册\n设备ID: " + device_id;

    ESP_LOGI(TAG, "Showing registration prompt - Device ID: %s", device_id.c_str());

    // 设置显示内容
    display->SetStatus("身份注册");
    display->SetEmotion("neutral");
    display->SetChatMessage("system", registration_message.c_str());

    // 停止相机功能
    StopCameraPreview();

    // 设置为待命状态（但保持注册提示显示）
    device_state_ = kDeviceStateIdle;
    ESP_LOGI(TAG, "STATE: idle (registration prompt)");

    // 启用唤醒词检测，等待下次唤醒
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(true);
}

void Application::TriggerWakeWordFlow()
{
    ESP_LOGI(TAG, "Triggering wake word flow after successful login");

    if (!protocol_)
    {
        ESP_LOGE(TAG, "Protocol not initialized, cannot trigger wake word flow");
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    // 执行与OnWakeWordDetected中相同的逻辑（已登录用户部分）
    audio_service_.EncodeWakeWord();

    if (!protocol_->IsAudioChannelOpened())
    {
        SetDeviceState(kDeviceStateConnecting);
        if (!protocol_->OpenAudioChannel())
        {
            ESP_LOGE(TAG, "Failed to open audio channel after login");
            SetDeviceState(kDeviceStateIdle);
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Sending login success wake word to server: %s", wake_word.c_str());

#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket())
    {
        protocol_->SendAudio(std::move(packet));
    }
    // 构建用户信息并发送唤醒词检测消息（登录成功时）
    std::string user_info = BuildUserInfoString();
    protocol_->SendWakeWordDetected(wake_word, user_info);
    SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
    SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
    // Play the pop up sound to indicate the wake word is detected
    audio_service_.PlaySound(Lang::Sounds::P3_POPUP);
#endif
}

void Application::CameraUploadCallback(void *arg)
{
    auto *app = static_cast<Application *>(arg);
    auto &board = Board::GetInstance();
    auto camera = board.GetCamera();

    if (camera && app->GetDeviceState() == kDeviceStateLogin)
    {
        // 检查是否已达到最大上传次数
        if (app->camera_upload_count_ >= app->MAX_UPLOAD_COUNT)
        {
            ESP_LOGI(TAG, "Reached maximum upload count (%d), no user found - showing registration prompt", app->MAX_UPLOAD_COUNT);

            // 停止上传
            app->StopCameraUpload();

            // 显示注册提示信息
            app->ShowRegistrationPrompt();

            return;
        }

        // 捕获图像但不显示预览（避免与预览定时器冲突）
        if (camera->Capture())
        {
            // 增加上传计数器
            app->camera_upload_count_++;
            ESP_LOGI(TAG, "Camera upload %d/%d", app->camera_upload_count_, app->MAX_UPLOAD_COUNT);

            // 上传到服务器
            app->UploadCameraImage(camera);
        }
    }
}

void Application::StartInspectionTimer()
{
    ESP_LOGI(TAG, "Starting inspection timer (60 seconds)");

    // 如果定时器已存在，先删除
    if (inspection_timer_ != nullptr)
    {
        esp_timer_stop(inspection_timer_);
        esp_timer_delete(inspection_timer_);
        inspection_timer_ = nullptr;
    }

    esp_timer_create_args_t timer_args = {.callback = InspectionCallback, .arg = this, .dispatch_method = ESP_TIMER_TASK, .name = "inspection_timer", .skip_unhandled_events = true};

    esp_err_t err = esp_timer_create(&timer_args, &inspection_timer_);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create inspection timer: %s", esp_err_to_name(err));
        return;
    }

    // 启动定时器，60秒后触发，不重复
    err = esp_timer_start_once(inspection_timer_, 60 * 1000000); // 60秒，单位是微秒
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start inspection timer: %s", esp_err_to_name(err));
        esp_timer_delete(inspection_timer_);
        inspection_timer_ = nullptr;
    }
}

void Application::StopInspectionTimer()
{
    if (inspection_timer_ != nullptr)
    {
        ESP_LOGI(TAG, "Stopping inspection timer");
        esp_timer_stop(inspection_timer_);
        esp_timer_delete(inspection_timer_);
        inspection_timer_ = nullptr;
    }
}

void Application::ClearInspectionFlags()
{
    ESP_LOGI(TAG, "Clearing inspection flags");
    pending_inspection_after_login_ = false;
    login_tts_completed_ = false;
}

void Application::InspectionCallback(void *arg)
{
    Application *app = static_cast<Application *>(arg);
    if (app != nullptr)
    {
        ESP_LOGI(TAG, "Inspection timer triggered, sending inspection request");
        app->SendInspectionRequest();
    }
}

void Application::SendInspectionRequest()
{
    ESP_LOGI(TAG, "=== Sending Inspection Request ===");

    auto &board = Board::GetInstance();
    auto network = board.GetNetwork();
    if (network == nullptr)
    {
        ESP_LOGE(TAG, "Network is not available");
        return;
    }

    auto http = network->CreateHttp(3);
    if (http == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return;
    }

    // 获取设备ID（MAC地址）
    std::string device_id = SystemInfo::GetMacAddress();

    // 构建JSON请求体
    std::string json_body = "{"
                            "\"device_id\": \"" +
                            device_id +
                            "\", "
                            "\"message\": \"进行集群巡检\", "
                            "\"auth_key\": \"3b039beb-90fa-4170-bed2-e0e146126877\", "
                            "\"bypass_llm\": false, "
                            "\"notification_type\": \"info\""
                            "}";

    // 设置HTTP头部
    http->SetHeader("Content-Type", "application/json");
    http->SetContent(std::move(json_body));

    std::string server_url = "http://8.138.251.153:8003/xiaozhi/push/message";

    ESP_LOGI(TAG, "Sending POST request to: %s", server_url.c_str());
    ESP_LOGI(TAG, "Device ID: %s", device_id.c_str());

    if (!http->Open("POST", server_url))
    {
        ESP_LOGE(TAG, "Failed to open HTTP connection for inspection request");
        http->Close();
        return;
    }

    ESP_LOGI(TAG, "=== Inspection Request sent successfully ===");

    int status_code = http->GetStatusCode();
    ESP_LOGI(TAG, "Inspection request status code: %d", status_code);

    std::string response_body = http->ReadAll();
    if (!response_body.empty())
    {
        ESP_LOGI(TAG, "Inspection response: %s", response_body.c_str());
    }

    if (status_code == 200)
    {
        ESP_LOGI(TAG, "Inspection request completed successfully");
    }
    else
    {
        ESP_LOGE(TAG, "Inspection request failed with status code: %d", status_code);
    }

    http->Close();

    // 清理定时器
    StopInspectionTimer();
}

void Application::StartAutoLogoutTimer()
{
    ESP_LOGI(TAG, "Starting auto logout timer (24 hours)");

    // 如果定时器已存在，先删除
    if (auto_logout_timer_ != nullptr)
    {
        esp_timer_stop(auto_logout_timer_);
        esp_timer_delete(auto_logout_timer_);
        auto_logout_timer_ = nullptr;
    }

    esp_timer_create_args_t timer_args = {.callback = AutoLogoutCallback, .arg = this, .dispatch_method = ESP_TIMER_TASK, .name = "auto_logout_timer", .skip_unhandled_events = true};

    esp_err_t err = esp_timer_create(&timer_args, &auto_logout_timer_);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create auto logout timer: %s", esp_err_to_name(err));
        return;
    }

    // 启动定时器，24小时后触发，不重复 (24 * 60 * 60 * 1000000 微秒)
    uint64_t timeout_us = 24ULL * 60 * 60 * 1000000; // 24小时
    err = esp_timer_start_once(auto_logout_timer_, timeout_us);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start auto logout timer: %s", esp_err_to_name(err));
        esp_timer_delete(auto_logout_timer_);
        auto_logout_timer_ = nullptr;
    }
    else
    {
        ESP_LOGI(TAG, "Auto logout timer started, will logout after 24 hours");
    }
}

void Application::StopAutoLogoutTimer()
{
    if (auto_logout_timer_ != nullptr)
    {
        ESP_LOGI(TAG, "Stopping auto logout timer");
        esp_timer_stop(auto_logout_timer_);
        esp_timer_delete(auto_logout_timer_);
        auto_logout_timer_ = nullptr;
    }
}

void Application::AutoLogoutCallback(void *arg)
{
    Application *app = static_cast<Application *>(arg);
    if (app != nullptr)
    {
        ESP_LOGI(TAG, "24-hour timer triggered, performing auto logout");
        app->PerformAutoLogout();
    }
}

void Application::PerformAutoLogout()
{
    ESP_LOGI(TAG, "=== Performing Auto Logout (24 hours elapsed) ===");

    // 停止所有相关定时器
    StopInspectionTimer();
    StopAutoLogoutTimer();

    // 清除巡检标志
    ClearInspectionFlags();

    // 清除用户信息
    user_manager_.ClearUserInfo();

    // 中断当前的语音交互流程
    ESP_LOGI(TAG, "Aborting current speaking and stopping listening due to auto logout");
    AbortSpeaking(kAbortReasonNone);
    StopListening();

    // 设置设备状态为空闲
    SetDeviceState(kDeviceStateIdle);

    // 显示登出消息
    auto &board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display != nullptr)
    {
        display->SetChatMessage("system", "24小时已到，已自动登出");
        ESP_LOGI(TAG, "Displayed auto logout message to user");
    }

    // 播放提示音（可选）
    PlaySound(Lang::Sounds::P3_POPUP);

    ESP_LOGI(TAG, "Auto logout completed successfully");
}

void Application::StartDailyCheckTimer()
{
    ESP_LOGI(TAG, "Starting daily check timer (every hour)");

    // 如果定时器已存在，先删除
    if (daily_check_timer_ != nullptr)
    {
        esp_timer_stop(daily_check_timer_);
        esp_timer_delete(daily_check_timer_);
        daily_check_timer_ = nullptr;
    }

    esp_timer_create_args_t timer_args = {.callback = DailyCheckCallback, .arg = this, .dispatch_method = ESP_TIMER_TASK, .name = "daily_check_timer", .skip_unhandled_events = true};

    esp_err_t err = esp_timer_create(&timer_args, &daily_check_timer_);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create daily check timer: %s", esp_err_to_name(err));
        return;
    }

    // 启动定时器，每1小时触发一次 (1 * 60 * 60 * 1000000 微秒)
    uint64_t period_us = 1ULL * 60 * 60 * 1000000; // 1小时
    err = esp_timer_start_periodic(daily_check_timer_, period_us);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start daily check timer: %s", esp_err_to_name(err));
        esp_timer_delete(daily_check_timer_);
        daily_check_timer_ = nullptr;
    }
    else
    {
        ESP_LOGI(TAG, "Daily check timer started, will check every hour");
    }
}

void Application::StopDailyCheckTimer()
{
    if (daily_check_timer_ != nullptr)
    {
        ESP_LOGI(TAG, "Stopping daily check timer");
        esp_timer_stop(daily_check_timer_);
        esp_timer_delete(daily_check_timer_);
        daily_check_timer_ = nullptr;
    }
}

void Application::DailyCheckCallback(void *arg)
{
    Application *app = static_cast<Application *>(arg);
    if (app != nullptr)
    {
        ESP_LOGI(TAG, "Daily check timer triggered, checking login date");
        app->CheckDailyExpiration();
    }
}

void Application::CheckDailyExpiration()
{
    ESP_LOGI(TAG, "=== Checking Daily Login Expiration ===");

    // 检查用户是否已登录
    if (!user_manager_.IsLoggedIn())
    {
        ESP_LOGI(TAG, "No user logged in, skipping daily check");
        return;
    }

    // 重新加载用户信息，这会触发日期检查
    user_manager_.LoadUserInfo();

    // 如果用户信息被清除（日期过期），停止相关定时器
    if (!user_manager_.IsLoggedIn())
    {
        ESP_LOGI(TAG, "User logged out due to date expiration, stopping related timers");

        // 停止所有相关定时器
        StopInspectionTimer();
        StopAutoLogoutTimer();
        StopDailyCheckTimer();

        // 清除巡检标志
        ClearInspectionFlags();

        // 中断当前的语音交互流程
        ESP_LOGI(TAG, "Aborting current speaking and stopping listening due to date expiration");
        AbortSpeaking(kAbortReasonNone);
        StopListening();

        // 设置设备状态为空闲
        SetDeviceState(kDeviceStateIdle);

        // 显示登出消息
        auto &board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display != nullptr)
        {
            display->SetChatMessage("system", "新的一天，请重新登录");
            ESP_LOGI(TAG, "Displayed new day logout message to user");
        }

        // 播放提示音
        PlaySound(Lang::Sounds::P3_POPUP);

        ESP_LOGI(TAG, "Daily expiration check completed - user logged out");
    }
    else
    {
        ESP_LOGI(TAG, "Daily check passed - user session continues");
    }
}

// ==================== 设备激活状态管理 ====================

void Application::LoadDeviceActivationStatus()
{
    Settings settings("device", true);
    is_device_activated_ = settings.GetInt("activated", 0) != 0;
    ESP_LOGI(TAG, "Device activation status loaded: %s", is_device_activated_ ? "activated" : "not activated");
}

void Application::SaveDeviceActivationStatus(bool activated)
{
    Settings settings("device", true);
    settings.SetInt("activated", activated ? 1 : 0);
    is_device_activated_ = activated;
    ESP_LOGI(TAG, "Device activation status saved: %s", activated ? "activated" : "not activated");
}

void Application::CheckDeviceActivationAfterLogin()
{
    ESP_LOGI(TAG, "=== Checking device activation after login ===");
    ESP_LOGI(TAG, "Device activation status: %s", is_device_activated_ ? "activated" : "not activated");

    if (is_device_activated_)
    {
        ESP_LOGI(TAG, "Device is activated, proceeding with normal login flow");

        // 设置标志，在首次进入listening状态时发送巡检请求
        ESP_LOGI(TAG, "User login successful, will send inspection request after first listening state");
        pending_inspection_after_login_ = true;

        // 启动每日检查定时器，每小时检查一次登录日期
        StartDailyCheckTimer();

        // 触发唤醒流程，让服务器知道用户已登录并发送欢迎消息
        TriggerWakeWordFlow();
    }
    else
    {
        ESP_LOGW(TAG, "Device is not activated, showing device activation prompt");
        ShowDeviceActivationPrompt();
    }
}

void Application::ShowDeviceActivationPrompt()
{
    ESP_LOGI(TAG, "Device not activated after login, requesting activation code from server");

    auto display = Board::GetInstance().GetDisplay();

    // 创建OTA实例来获取激活码
    Ota ota;

    // 检查版本以获取激活码
    if (ota.CheckVersion())
    {
        if (ota.HasActivationCode())
        {
            ESP_LOGI(TAG, "Got activation code from server, displaying to user");

            // 设置为激活状态显示
            SetDeviceState(kDeviceStateActivating);
            display->SetStatus(Lang::Strings::ACTIVATION);

            // 显示激活码和消息
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());

            // 启动激活流程
            Schedule(
                [this, ota]() mutable
                {
                    auto display = Board::GetInstance().GetDisplay();

                    // 等待用户激活
                    for (int i = 0; i < 10; ++i)
                    {
                        ESP_LOGI(TAG, "Waiting for device activation... %d/%d", i + 1, 10);
                        esp_err_t err = ota.Activate();
                        if (err == ESP_OK)
                        {
                            ESP_LOGI(TAG, "Device activation successful after login, saving activation status");
                            SaveDeviceActivationStatus(true); // 保存激活状态

                            // 激活成功，继续正常登录流程
                            ESP_LOGI(TAG, "Device activated, proceeding with normal login flow");

                            // 设置标志，在首次进入listening状态时发送巡检请求
                            pending_inspection_after_login_ = true;

                            // 启动每日检查定时器
                            StartDailyCheckTimer();

                            // 触发唤醒流程
                            TriggerWakeWordFlow();
                            return;
                        }
                        else if (err == ESP_ERR_TIMEOUT)
                        {
                            vTaskDelay(pdMS_TO_TICKS(3000));
                        }
                        else
                        {
                            vTaskDelay(pdMS_TO_TICKS(10000));
                        }

                        if (device_state_ == kDeviceStateIdle)
                        {
                            ESP_LOGI(TAG, "Activation interrupted, returning to idle state");
                            break;
                        }
                    }

                    // 激活超时或失败，返回待命状态
                    ESP_LOGW(TAG, "Device activation timeout or failed, returning to standby");
                    SetDeviceState(kDeviceStateIdle);
                });
        }
        else
        {
            ESP_LOGW(TAG, "No activation code available from server");

            // 没有激活码，显示错误信息
            display->SetStatus("激活失败");
            display->SetEmotion("sad");
            display->SetChatMessage("system", "无法获取激活码，请稍后重试");

            // 延时后返回待命状态
            Schedule(
                [this]()
                {
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    SetDeviceState(kDeviceStateIdle);
                });
        }
    }
    else
    {
        ESP_LOGE(TAG, "Failed to check version for activation code");

        // 版本检查失败，显示错误信息
        display->SetStatus("网络错误");
        display->SetEmotion("sad");
        display->SetChatMessage("system", "网络连接失败，请检查网络后重试");

        // 延时后返回待命状态
        Schedule(
            [this]()
            {
                vTaskDelay(pdMS_TO_TICKS(5000));
                SetDeviceState(kDeviceStateIdle);
            });
    }
}
