/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <algorithm>
#include <cstring>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "board.h"
#include "display.h"
#include "protocol.h"

#define TAG "MCP"

#define DEFAULT_TOOLCALL_STACK_SIZE 6144

McpServer::McpServer() {}

McpServer::~McpServer()
{
    for (auto tool : tools_)
    {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools()
{
    // To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto &board = Board::GetInstance();

    AddTool("self.get_device_status",
            "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
            "Use this tool for: \n"
            "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
            "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
            PropertyList(), [&board](const PropertyList &properties) -> ReturnValue { return board.GetDeviceStatusJson(); });

    AddTool("self.user.account_logout",
            "Clear the current user account from device and logout the user account (NOT end conversation).\n"
            "CRITICAL: This tool is ONLY for user account management, NEVER for ending conversations or saying goodbye.\n"
            "Use this tool ONLY when user wants to:\n"
            "1. Clear their user account from the device (清除用户账户)\n"
            "2. Logout from their personal account (注销用户账户)\n"
            "3. Switch to a different user account (切换用户账户)\n"
            "4. Remove their personal data from device (清除个人数据)\n"
            "Keywords that should trigger this tool: 注销账户, 登出账户, 清除用户, 切换用户, 账户登出, 用户注销\n"
            "DO NOT use for: 再见, 退出, 结束对话, goodbye, exit conversation\n"
            "After using this tool, device returns to login screen requiring face recognition.",
            PropertyList(),
            [](const PropertyList &properties) -> ReturnValue
            {
                ESP_LOGI(TAG, "=== MCP Method Called: self.user.account_logout ===");

                auto &app = Application::GetInstance();
                auto &user_manager = app.GetUserManager();

                ESP_LOGI(TAG, "Checking current login status...");
                bool is_logged_in = user_manager.IsLoggedIn();
                ESP_LOGI(TAG, "Current login status: %s", is_logged_in ? "LOGGED IN" : "NOT LOGGED IN");

                if (!is_logged_in)
                {
                    ESP_LOGW(TAG, "Logout failed: No user is currently logged in");
                    return "{\"success\": false, \"message\": \"No user is currently logged in\"}";
                }

                std::string current_user = user_manager.GetName();
                ESP_LOGI(TAG, "Current logged in user: %s", current_user.c_str());
                ESP_LOGI(TAG, "Starting user logout process...");

                user_manager.ClearUserInfo();

                // 停止所有定时器和清除巡检标志
                ESP_LOGI(TAG, "Stopping all timers and clearing inspection flags due to user logout");
                app.StopInspectionTimer();
                app.StopAutoLogoutTimer();
                app.StopDailyCheckTimer();
                app.ClearInspectionFlags();

                ESP_LOGI(TAG, "User info cleared from storage");
                ESP_LOGI(TAG, "Verifying logout status...");
                bool logout_success = !user_manager.IsLoggedIn();
                ESP_LOGI(TAG, "Logout verification: %s", logout_success ? "SUCCESS" : "FAILED");

                // 中断当前的语音交互流程
                ESP_LOGI(TAG, "Aborting current speaking and stopping listening...");
                app.AbortSpeaking(kAbortReasonNone);
                app.StopListening();

                // 完全重新初始化音频服务以确保状态一致
                ESP_LOGI(TAG, "Completely reinitializing audio service to clean state...");
                auto &audio_service = app.GetAudioService();

                // 先完全停止所有音频处理
                audio_service.EnableVoiceProcessing(false);
                audio_service.EnableWakeWordDetection(false);

                // 等待更长时间确保所有异步任务完成
                vTaskDelay(pdMS_TO_TICKS(500));

                // 强制停止音频服务并重新启动
                ESP_LOGI(TAG, "Stopping and restarting audio service...");
                audio_service.Stop();
                vTaskDelay(pdMS_TO_TICKS(200));
                audio_service.Start();
                vTaskDelay(pdMS_TO_TICKS(200));

                // 重新启用唤醒词检测
                audio_service.EnableWakeWordDetection(true);

                // 设置设备回到待命状态
                ESP_LOGI(TAG, "Setting device to idle state after logout");
                app.SetDeviceState(kDeviceStateIdle);

                // 启动10秒定时器，延迟清空屏幕显示
                ESP_LOGI(TAG, "Starting 10-second timer to clear screen display");
                static esp_timer_handle_t clear_screen_timer = nullptr;

                // 创建定时器（如果还没创建）
                if (!clear_screen_timer)
                {
                    esp_timer_create_args_t timer_args = {.callback =
                                                              [](void *arg)
                                                          {
                                                              ESP_LOGI(TAG, "Clear screen timer triggered - clearing display");
                                                              auto display = Board::GetInstance().GetDisplay();
                                                              if (display)
                                                              {
                                                                  display->SetChatMessage("system", "");
                                                              }
                                                          },
                                                          .arg = nullptr,
                                                          .dispatch_method = ESP_TIMER_TASK,
                                                          .name = "logout_clear_screen",
                                                          .skip_unhandled_events = true};
                    esp_timer_create(&timer_args, &clear_screen_timer);
                }

                // 停止可能正在运行的定时器并启动新的
                esp_timer_stop(clear_screen_timer);
                esp_timer_start_once(clear_screen_timer, 10000000); // 10秒

                ESP_LOGI(TAG, "User logout completed: %s", current_user.c_str());
                ESP_LOGI(TAG, "=== MCP Method Finished: self.user.account_logout ===");

                return "{\"success\": true, \"message\": \"User logged out successfully\", \"previous_user\": \"" + current_user + "\"}";
            });

    AddTool("self.user.get_schedules",
            "Get the user's today schedules/tasks from the device storage.\n"
            "Use this tool when user asks about:\n"
            "1. Today's schedule (今天的日程)\n"
            "2. Today's tasks (今天的任务)\n"
            "3. What to do today (今天要做什么)\n"
            "4. Daily agenda (每日议程)\n"
            "Returns a JSON object containing the list of today's schedules with their status.",
            PropertyList(),
            [](const PropertyList &properties) -> ReturnValue
            {
                ESP_LOGI(TAG, "=== MCP Method Called: self.user.get_schedules ===");

                auto &app = Application::GetInstance();
                auto &user_manager = app.GetUserManager();

                if (!user_manager.IsLoggedIn())
                {
                    ESP_LOGW(TAG, "Get schedules failed: No user is currently logged in");
                    return "{\"success\": false, \"message\": \"No user is currently logged in\"}";
                }

                const auto &schedules = user_manager.GetTodaySchedules();
                ESP_LOGI(TAG, "Retrieved %d schedule items for user: %s", schedules.size(), user_manager.GetName().c_str());

                // 构建JSON响应
                std::string json_response = "{\"success\": true, \"user\": \"" + user_manager.GetName() + "\", \"schedules\": [";

                for (size_t i = 0; i < schedules.size(); i++)
                {
                    if (i > 0)
                        json_response += ", ";
                    json_response += "{\"id\": \"" + schedules[i].id + "\", \"content\": \"" + schedules[i].content + "\", \"schedule_date\": \"" + schedules[i].schedule_date + "\", \"status\": " + std::to_string(schedules[i].status) + ", \"status_text\": \"" + schedules[i].status_text + "\"}";
                }

                json_response += "], \"total_count\": " + std::to_string(schedules.size()) + "}";

                ESP_LOGI(TAG, "=== MCP Method Finished: self.user.get_schedules ===");
                return json_response;
            });

    AddTool("self.audio_speaker.set_volume", "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.", PropertyList({Property("volume", kPropertyTypeInteger, 0, 100)}),
            [&board](const PropertyList &properties) -> ReturnValue
            {
                auto codec = board.GetAudioCodec();
                codec->SetOutputVolume(properties["volume"].value<int>());
                return true;
            });

    auto backlight = board.GetBacklight();
    if (backlight)
    {
        AddTool("self.screen.set_brightness", "Set the brightness of the screen.", PropertyList({Property("brightness", kPropertyTypeInteger, 0, 100)}),
                [backlight](const PropertyList &properties) -> ReturnValue
                {
                    uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                    backlight->SetBrightness(brightness, true);
                    return true;
                });
    }

    auto display = board.GetDisplay();
    if (display && !display->GetTheme().empty())
    {
        AddTool("self.screen.set_theme", "Set the theme of the screen. The theme can be `light` or `dark`.", PropertyList({Property("theme", kPropertyTypeString)}),
                [display](const PropertyList &properties) -> ReturnValue
                {
                    display->SetTheme(properties["theme"].value<std::string>().c_str());
                    return true;
                });
    }

    auto camera = board.GetCamera();
    if (camera)
    {
        AddTool("self.camera.take_photo",
                "Take a photo and explain it. Use this tool after the user asks you to see something.\n"
                "Args:\n"
                "  `question`: The question that you want to ask about the photo.\n"
                "Return:\n"
                "  A JSON object that provides the photo information.",
                PropertyList({Property("question", kPropertyTypeString)}),
                [camera](const PropertyList &properties) -> ReturnValue
                {
                    if (!camera->Capture())
                    {
                        return "{\"success\": false, \"message\": \"Failed to capture photo\"}";
                    }
                    auto question = properties["question"].value<std::string>();
                    return camera->Explain(question);
                });
    }

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddTool(McpTool *tool)
{
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool *t) { return t->name() == tool->name(); }) != tools_.end())
    {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s", tool->name().c_str());
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string &name, const std::string &description, const PropertyList &properties, std::function<ReturnValue(const PropertyList &)> callback) { AddTool(new McpTool(name, description, properties, callback)); }

void McpServer::ParseMessage(const std::string &message)
{
    cJSON *json = cJSON_Parse(message.c_str());
    if (json == nullptr)
    {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON *capabilities)
{
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision))
    {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url))
        {
            auto camera = Board::GetInstance().GetCamera();
            if (camera)
            {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token))
                {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON *json)
{
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0)
    {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }

    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method))
    {
        ESP_LOGE(TAG, "Missing method");
        return;
    }

    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0)
    {
        return;
    }

    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params))
    {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id))
    {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;

    if (method_str == "initialize")
    {
        if (cJSON_IsObject(params))
        {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities))
            {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    }
    else if (method_str == "tools/list")
    {
        std::string cursor_str = "";
        if (params != nullptr)
        {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor))
            {
                cursor_str = std::string(cursor->valuestring);
            }
        }
        GetToolsList(id_int, cursor_str);
    }
    else if (method_str == "tools/call")
    {
        if (!cJSON_IsObject(params))
        {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name))
        {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments))
        {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        auto stack_size = cJSON_GetObjectItem(params, "stackSize");
        if (stack_size != nullptr && !cJSON_IsNumber(stack_size))
        {
            ESP_LOGE(TAG, "tools/call: Invalid stackSize");
            ReplyError(id_int, "Invalid stackSize");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments, stack_size ? stack_size->valueint : DEFAULT_TOOLCALL_STACK_SIZE);
    }
    else
    {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string &result)
{
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string &message)
{
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string &cursor)
{
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";

    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";

    while (it != tools_.end())
    {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor)
        {
            if ((*it)->name() == cursor)
            {
                found_cursor = true;
            }
            else
            {
                ++it;
                continue;
            }
        }

        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size)
        {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }

        json += tool_json;
        ++it;
    }

    if (json.back() == ',')
    {
        json.pop_back();
    }

    if (json.back() == '[' && !tools_.empty())
    {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty())
    {
        json += "]}";
    }
    else
    {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }

    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string &tool_name, const cJSON *tool_arguments, int stack_size)
{
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), [&tool_name](const McpTool *tool) { return tool->name() == tool_name; });

    if (tool_iter == tools_.end())
    {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try
    {
        for (auto &argument : arguments)
        {
            bool found = false;
            if (cJSON_IsObject(tool_arguments))
            {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value))
                {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                }
                else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value))
                {
                    argument.set_value<int>(value->valueint);
                    found = true;
                }
                else if (argument.type() == kPropertyTypeString && cJSON_IsString(value))
                {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found)
            {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    }
    catch (const std::exception &e)
    {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Start a task to receive data with stack size
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name = "tool_call";
    cfg.stack_size = stack_size;
    cfg.prio = 1;
    esp_pthread_set_cfg(&cfg);

    // Use a thread to call the tool to avoid blocking the main thread
    tool_call_thread_ = std::thread(
        [this, id, tool_iter, arguments = std::move(arguments)]()
        {
            try
            {
                ReplyResult(id, (*tool_iter)->Call(arguments));
            }
            catch (const std::exception &e)
            {
                ESP_LOGE(TAG, "tools/call: %s", e.what());
                ReplyError(id, e.what());
            }
        });
    tool_call_thread_.detach();
}