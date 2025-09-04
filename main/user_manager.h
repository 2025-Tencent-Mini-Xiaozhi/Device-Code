#ifndef USER_MANAGER_H
#define USER_MANAGER_H

#include <string>
#include <vector>

// 日程任务结构
struct ScheduleItem
{
    std::string id; // 新增：日程ID
    std::string content;
    std::string schedule_date; // 新增：日程日期
    int status;                // 修改：状态为数字类型 (0=未完成, 1=已完成)
    std::string status_text;   // 新增：状态文本描述

    ScheduleItem() = default;
    ScheduleItem(const std::string &c, int s) : content(c), status(s) {}
    ScheduleItem(const std::string &i, const std::string &c, const std::string &d, int s, const std::string &st) : id(i), content(c), schedule_date(d), status(s), status_text(st) {}
};

class UserManager
{
private:
    bool is_logged_in_ = false;
    std::string name_;
    std::string account_;
    std::string password_; // 添加密码字段
    std::string api_key_;
    std::string api_id_; // 添加API ID字段
    // 移除auth_token字段
    int user_id_ = 0;
    std::vector<ScheduleItem> today_schedules_;

public:
    bool IsLoggedIn() const { return is_logged_in_; }

    void SaveUserInfo(const std::string &name, const std::string &account, const std::string &api_key);
    void LoadUserInfo();
    void ClearUserInfo();

    // 设置认证信息的方法
    void SetPassword(const std::string &password) { password_ = password; }
    // 移除SetAuthToken

    // 解析服务器响应并更新用户信息
    bool ParseServerResponse(const std::string &json_response);

    // 日程管理方法
    void SaveSchedules(const std::vector<ScheduleItem> &schedules);
    void LoadSchedules();
    void ClearSchedules();
    const std::vector<ScheduleItem> &GetTodaySchedules() const { return today_schedules_; }
    void PrintSchedules() const;

    // 打印用户信息
    void PrintUserInfo() const;

    const std::string &GetName() const { return name_; }
    const std::string &GetAccount() const { return account_; }
    const std::string &GetPassword() const { return password_; } // 添加密码getter
    const std::string &GetApiKey() const { return api_key_; }
    const std::string &GetApiId() const { return api_id_; } // 添加API ID getter
    // 移除GetAuthToken
    int GetUserId() const { return user_id_; }
};

#endif