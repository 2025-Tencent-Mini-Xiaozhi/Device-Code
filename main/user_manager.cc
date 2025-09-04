#include "user_manager.h"
#include "settings.h"
#include <cJSON.h> // 必须包含此头文件才能使用ESP_LOGI等日志宏
#include <ctime>   // 添加时间相关头文件
#include <esp_log.h>
#include <sys/time.h>

#define TAG "UserManager"

void UserManager::SaveUserInfo(const std::string &name, const std::string &account, const std::string &api_key)
{
    ESP_LOGI(TAG, "Saving user info for: %s", name.c_str());

    Settings settings("user", true);
    settings.SetString("name", name);
    settings.SetString("account", account);
    settings.SetString("password", password_); // 保存密码
    settings.SetString("api_key", api_key);
    settings.SetString("api_id", api_id_); // 保存API ID
    settings.SetInt("user_id", user_id_);  // 保存用户ID
    // 移除auth_token存储
    settings.SetInt("logged_in", 1);

    // 保存登录日期用于每日自动注销
    time_t now = time(nullptr);
    struct tm *timeinfo = localtime(&now);

    // 计算自1900年1月1日以来的天数作为日期标识
    int login_date = (timeinfo->tm_year + 1900) * 1000 + timeinfo->tm_yday; // 年份*1000 + 一年中的第几天
    settings.SetInt("login_date", login_date);

    name_ = name;
    account_ = account;
    api_key_ = api_key;
    is_logged_in_ = true;

    ESP_LOGI(TAG, "User info saved successfully: name=%s, account=%s", name.c_str(), account.c_str());
    ESP_LOGI(TAG, "API key saved (length: %d characters)", api_key.length());
    ESP_LOGI(TAG, "Login date saved: %d (Year: %d, Day of year: %d)", login_date, timeinfo->tm_year + 1900, timeinfo->tm_yday + 1);
}

void UserManager::LoadUserInfo()
{
    ESP_LOGI(TAG, "Loading user info from NVS storage");

    Settings settings("user", false);
    name_ = settings.GetString("name");
    account_ = settings.GetString("account");
    password_ = settings.GetString("password"); // 加载密码
    api_key_ = settings.GetString("api_key");
    api_id_ = settings.GetString("api_id");   // 加载API ID
    user_id_ = settings.GetInt("user_id", 0); // 加载用户ID
    // 移除auth_token加载
    is_logged_in_ = settings.GetInt("logged_in", 0) == 1;

    if (is_logged_in_)
    {
        // 检查登录日期是否为当天
        int login_date = settings.GetInt("login_date", 0);
        time_t current_time = time(nullptr);
        struct tm *current_timeinfo = localtime(&current_time);

        // 计算当前日期标识
        int current_date = (current_timeinfo->tm_year + 1900) * 1000 + current_timeinfo->tm_yday;

        ESP_LOGI(TAG, "Login date: %d, current date: %d", login_date, current_date);
        ESP_LOGI(TAG, "Current date: Year %d, Day of year %d", current_timeinfo->tm_year + 1900, current_timeinfo->tm_yday + 1);

        // 如果日期不同，自动清除用户信息
        if (login_date != current_date)
        {
            ESP_LOGW(TAG, "Login date expired (new day detected), auto clearing user info");
            ClearUserInfo();
            return;
        }

        ESP_LOGI(TAG, "User loaded successfully: %s (%s)", name_.c_str(), account_.c_str());
        ESP_LOGI(TAG, "API key loaded (length: %d characters)", api_key_.length());
        ESP_LOGI(TAG, "Same day login, user session continues");

        // 加载日程数据
        LoadSchedules();
    }
    else
    {
        ESP_LOGI(TAG, "No user logged in");
    }
}

void UserManager::ClearUserInfo()
{
    ESP_LOGI(TAG, "Clearing user info");

    Settings settings("user", true);
    settings.EraseAll();

    name_.clear();
    account_.clear();
    password_.clear(); // 清除密码
    api_key_.clear();
    api_id_.clear(); // 清除API ID
    // 移除auth_token清除
    user_id_ = 0;
    is_logged_in_ = false;

    // 清除日程数据
    ClearSchedules();

    ESP_LOGI(TAG, "User info and schedules cleared successfully");
}

bool UserManager::ParseServerResponse(const std::string &json_response)
{
    ESP_LOGI(TAG, "Parsing server response...");

    cJSON *root = cJSON_Parse(json_response.c_str());
    if (root == nullptr)
    {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }

    // 检查status字段
    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (status == nullptr || !cJSON_IsNumber(status))
    {
        ESP_LOGE(TAG, "Missing or invalid status field");
        cJSON_Delete(root);
        return false;
    }

    int status_code = status->valueint;
    ESP_LOGI(TAG, "Response status: %d", status_code);

    // 解析message字段（仅用于日志显示）
    cJSON *message = cJSON_GetObjectItem(root, "message");
    if (message != nullptr && cJSON_IsString(message))
    {
        ESP_LOGI(TAG, "Message: %s", message->valuestring);
    }

    // 如果status为1（成功），解析用户信息
    if (status_code == 1)
    {
        ESP_LOGI(TAG, "Recognition successful, parsing user info...");

        // 解析user_info对象
        cJSON *user_info = cJSON_GetObjectItem(root, "user_info");
        if (user_info != nullptr && cJSON_IsObject(user_info))
        {
            // 解析用户基本信息
            cJSON *name = cJSON_GetObjectItem(user_info, "name");
            if (name != nullptr && cJSON_IsString(name))
            {
                name_ = name->valuestring;
            }

            cJSON *account = cJSON_GetObjectItem(user_info, "account");
            if (account != nullptr && cJSON_IsString(account))
            {
                account_ = account->valuestring;
            }

            cJSON *password = cJSON_GetObjectItem(user_info, "password");
            if (password != nullptr && cJSON_IsString(password))
            {
                password_ = password->valuestring;
            }

            cJSON *api_key = cJSON_GetObjectItem(user_info, "api_key");
            if (api_key != nullptr && cJSON_IsString(api_key))
            {
                api_key_ = api_key->valuestring;
            }

            cJSON *api_id = cJSON_GetObjectItem(user_info, "api_id");
            if (api_id != nullptr && cJSON_IsString(api_id))
            {
                api_id_ = api_id->valuestring;
            }

            // 移除auth_token解析

            cJSON *user_id = cJSON_GetObjectItem(user_info, "user_id");
            if (user_id != nullptr && cJSON_IsNumber(user_id))
            {
                user_id_ = user_id->valueint;
            }
        }

        // 解析today_schedules数组
        cJSON *today_schedules = cJSON_GetObjectItem(root, "today_schedules");
        if (today_schedules != nullptr && cJSON_IsArray(today_schedules))
        {
            ESP_LOGI(TAG, "Parsing today's schedules...");
            today_schedules_.clear();

            int schedule_count = cJSON_GetArraySize(today_schedules);
            ESP_LOGI(TAG, "Found %d schedule items", schedule_count);

            for (int i = 0; i < schedule_count; i++)
            {
                cJSON *schedule_item = cJSON_GetArrayItem(today_schedules, i);
                if (schedule_item != nullptr && cJSON_IsObject(schedule_item))
                {
                    ScheduleItem item;

                    // 解析ID字段
                    cJSON *id = cJSON_GetObjectItem(schedule_item, "id");
                    if (id != nullptr && cJSON_IsString(id))
                    {
                        item.id = id->valuestring;
                    }

                    // 解析content字段
                    cJSON *content = cJSON_GetObjectItem(schedule_item, "content");
                    if (content != nullptr && cJSON_IsString(content))
                    {
                        item.content = content->valuestring;
                    }

                    // 解析schedule_date字段
                    cJSON *schedule_date = cJSON_GetObjectItem(schedule_item, "schedule_date");
                    if (schedule_date != nullptr && cJSON_IsString(schedule_date))
                    {
                        item.schedule_date = schedule_date->valuestring;
                    }

                    // 解析status字段（数字类型）
                    cJSON *status = cJSON_GetObjectItem(schedule_item, "status");
                    if (status != nullptr && cJSON_IsNumber(status))
                    {
                        item.status = status->valueint;
                    }

                    // 解析status_text字段
                    cJSON *status_text = cJSON_GetObjectItem(schedule_item, "status_text");
                    if (status_text != nullptr && cJSON_IsString(status_text))
                    {
                        item.status_text = status_text->valuestring;
                    }

                    if (!item.content.empty())
                    {
                        today_schedules_.push_back(item);
                        ESP_LOGI(TAG, "Schedule %d: [%s] %s (%s) [%s]", i + 1, item.id.c_str(), item.content.c_str(), item.schedule_date.c_str(), item.status_text.c_str());
                    }
                }
            }

            // 保存日程到持久化存储
            SaveSchedules(today_schedules_);
        }

        // 解析recognition_info对象（新增）
        cJSON *recognition_info = cJSON_GetObjectItem(root, "recognition_info");
        if (recognition_info != nullptr && cJSON_IsObject(recognition_info))
        {
            ESP_LOGI(TAG, "Parsing recognition info...");

            cJSON *similarity = cJSON_GetObjectItem(recognition_info, "similarity");
            if (similarity != nullptr && cJSON_IsNumber(similarity))
            {
                ESP_LOGI(TAG, "Face recognition similarity: %.4f", similarity->valuedouble);
            }

            cJSON *total_faces = cJSON_GetObjectItem(recognition_info, "total_faces_detected");
            if (total_faces != nullptr && cJSON_IsNumber(total_faces))
            {
                ESP_LOGI(TAG, "Total faces detected: %d", total_faces->valueint);
            }

            cJSON *largest_face = cJSON_GetObjectItem(recognition_info, "selected_largest_face");
            if (largest_face != nullptr && cJSON_IsBool(largest_face))
            {
                ESP_LOGI(TAG, "Selected largest face: %s", cJSON_IsTrue(largest_face) ? "true" : "false");
            }
        }

        // 保存用户信息到持久化存储
        SaveUserInfo(name_, account_, api_key_);

        // 打印用户信息和日程
        PrintUserInfo();
        PrintSchedules();

        cJSON_Delete(root);
        return true;
    }
    else
    {
        ESP_LOGW(TAG, "Recognition failed with status: %d", status_code);
        cJSON_Delete(root);
        return false;
    }
}

void UserManager::PrintUserInfo() const
{
    ESP_LOGI(TAG, "=== User Information ===");
    ESP_LOGI(TAG, "Name: %s", name_.c_str());
    ESP_LOGI(TAG, "Account: %s", account_.c_str());
    ESP_LOGI(TAG, "User ID: %d", user_id_);
    ESP_LOGI(TAG, "API Key: %s", api_key_.c_str());
    ESP_LOGI(TAG, "API ID: %s", api_id_.c_str());
    if (password_.empty())
    {
        ESP_LOGI(TAG, "Password: (not set)");
    }
    else
    {
        ESP_LOGI(TAG, "Password: (length: %d chars)", password_.length());
    }
    // 移除auth_token打印
    ESP_LOGI(TAG, "Login status: %s", is_logged_in_ ? "Logged in" : "Not logged in");
    ESP_LOGI(TAG, "========================");
}

void UserManager::SaveSchedules(const std::vector<ScheduleItem> &schedules)
{
    ESP_LOGI(TAG, "Saving %d schedule items to storage", schedules.size());

    Settings settings("schedules", true);
    settings.SetInt("count", schedules.size());

    for (size_t i = 0; i < schedules.size(); i++)
    {
        char id_key[32], content_key[32], date_key[32], status_key[32], text_key[32];
        snprintf(id_key, sizeof(id_key), "id_%d", (int)i);
        snprintf(content_key, sizeof(content_key), "content_%d", (int)i);
        snprintf(date_key, sizeof(date_key), "date_%d", (int)i);
        snprintf(status_key, sizeof(status_key), "status_%d", (int)i);
        snprintf(text_key, sizeof(text_key), "text_%d", (int)i);

        settings.SetString(id_key, schedules[i].id);
        settings.SetString(content_key, schedules[i].content);
        settings.SetString(date_key, schedules[i].schedule_date);
        settings.SetInt(status_key, schedules[i].status);
        settings.SetString(text_key, schedules[i].status_text);
    }

    ESP_LOGI(TAG, "Schedules saved successfully");
}

void UserManager::LoadSchedules()
{
    ESP_LOGI(TAG, "Loading schedules from storage");

    Settings settings("schedules", false);
    int count = settings.GetInt("count", 0);

    today_schedules_.clear();

    for (int i = 0; i < count; i++)
    {
        char id_key[32], content_key[32], date_key[32], status_key[32], text_key[32];
        snprintf(id_key, sizeof(id_key), "id_%d", i);
        snprintf(content_key, sizeof(content_key), "content_%d", i);
        snprintf(date_key, sizeof(date_key), "date_%d", i);
        snprintf(status_key, sizeof(status_key), "status_%d", i);
        snprintf(text_key, sizeof(text_key), "text_%d", i);

        std::string id = settings.GetString(id_key);
        std::string content = settings.GetString(content_key);
        std::string date = settings.GetString(date_key);
        int status = settings.GetInt(status_key, 0);
        std::string text = settings.GetString(text_key);

        if (!content.empty())
        {
            today_schedules_.emplace_back(id, content, date, status, text);
        }
    }

    ESP_LOGI(TAG, "Loaded %d schedule items from storage", today_schedules_.size());
}

void UserManager::ClearSchedules()
{
    ESP_LOGI(TAG, "Clearing schedules from storage");

    Settings settings("schedules", true);
    settings.EraseAll();

    today_schedules_.clear();

    ESP_LOGI(TAG, "Schedules cleared successfully");
}

void UserManager::PrintSchedules() const
{
    ESP_LOGI(TAG, "=== Today's Schedules ===");
    if (today_schedules_.empty())
    {
        ESP_LOGI(TAG, "No schedules for today");
    }
    else
    {
        for (size_t i = 0; i < today_schedules_.size(); i++)
        {
            ESP_LOGI(TAG, "%d. [%s] %s (%s) - %s", (int)(i + 1), today_schedules_[i].id.c_str(), today_schedules_[i].content.c_str(), today_schedules_[i].schedule_date.c_str(), today_schedules_[i].status_text.c_str());
        }
    }
    ESP_LOGI(TAG, "=========================");
}