#include "tool.h"
#include "board.h"
#include "display/lvgl_display/lvgl_theme.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <cstring>
#include <cstdlib>
#include "app.h"

#define TAG "Tool"

ToolManager& ToolManager::GetInstance() {
    static ToolManager instance;
    return instance;
}

void ToolManager::RegisterTool(const std::string& name, ToolCallback callback) {
    tools_[name] = callback;
    ESP_LOGI(TAG, "注册工具: %s", name.c_str());
}

bool ToolManager::ExecuteToolCommand(const cJSON* json, std::string& result, std::string& tool_id) {
    if (json == nullptr) {
        result = "Invalid JSON";
        return false;
    }

    auto name_item = cJSON_GetObjectItem(json, "name");
    if (!cJSON_IsString(name_item)) {
        result = "Missing or invalid 'name' field";
        return false;
    }

    auto method_item = cJSON_GetObjectItem(json, "method");
    if (!cJSON_IsString(method_item)) {
        result = "Missing or invalid 'method' field";
        return false;
    }

    auto params_item = cJSON_GetObjectItem(json, "parameters");
    cJSON* parameters = nullptr;
    if (params_item != nullptr && cJSON_IsObject(params_item)) {
        parameters = params_item;
    }

    tool_id = "";
    if (parameters != nullptr) {
        auto id_item = cJSON_GetObjectItem(parameters, "id");
        if (cJSON_IsString(id_item)) {
            tool_id = id_item->valuestring;
        }
    }

    std::string name = name_item->valuestring;
    std::string method = method_item->valuestring;

    ESP_LOGI(TAG, "执行工具命令: name=%s, method=%s, id=%s", 
             name.c_str(), method.c_str(), tool_id.c_str());

    auto it = tools_.find(name);
    if (it == tools_.end()) {
        result = "工具不存在: " + name;
        ESP_LOGE(TAG, "%s", result.c_str());
        return false;
    }

    ToolCommand cmd;
    cmd.name = name;
    cmd.method = method;
    cmd.parameters = parameters;

    bool success = it->second(cmd, result);
    
    if (success) {
        ESP_LOGI(TAG, "Tool executed successfully: %s", result.c_str());
    } else {
        ESP_LOGE(TAG, "Tool execution failed: %s", result.c_str());
    }

    return success;
}

bool ToolManager::ParseToolMessage(const char* data, size_t len, std::string& result) {
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        ESP_LOGE(TAG, "解析工具消息JSON失败");
        result = "Invalid JSON";
        return false;
    }

    auto type_item = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type_item) || strcmp(type_item->valuestring, "tool") != 0) {
        ESP_LOGE(TAG, "工具消息类型无效");
        cJSON_Delete(root);
        result = "Invalid message type";
        return false;
    }

    auto commands_item = cJSON_GetObjectItem(root, "commands");
    if (!cJSON_IsArray(commands_item)) {
        ESP_LOGE(TAG, "缺少或无效的 'commands' 数组");
        cJSON_Delete(root);
        result = "缺少 commands 数组";
        return false;
    }

    int cmd_count = cJSON_GetArraySize(commands_item);
    ESP_LOGI(TAG, "收到 %d个工具命令", cmd_count);

    bool all_success = true;
    std::string all_results;

    for (int i = 0; i < cmd_count; i++) {
        auto cmd_item = cJSON_GetArrayItem(commands_item, i);
        if (!cJSON_IsObject(cmd_item)) {
            ESP_LOGW(TAG, "索引 %d 处的命令无效，跳过", i); 
            continue;
        }

        std::string cmd_result;
        std::string tool_id;
        bool success = ExecuteToolCommand(cmd_item, cmd_result, tool_id);
        
        if (!success) {
            all_success = false;
        }

        if (!tool_id.empty()) {
            SendToolResponse(tool_id, success, cmd_result);
        }

        if (i > 0) {
            all_results += "; ";
        }
        all_results += cmd_result;
    }

    result = all_results;
    cJSON_Delete(root);
    return all_success;
}

std::string ToolManager::BuildToolResponse(const std::string& tool_id, bool success, const std::string& result) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "tool");
    cJSON_AddStringToObject(root, "id", tool_id.c_str());
    cJSON_AddBoolToObject(root, "success", success);
    cJSON_AddStringToObject(root, "result", result.c_str());

    char* json_str = cJSON_PrintUnformatted(root);
    std::string response(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);

    return response;
}

void ToolManager::SendToolResponse(const std::string& tool_id, bool success, const std::string& result) {
    std::string response = BuildToolResponse(tool_id, success, result);
    
    auto& app = App::GetInstance();
    app.SendToolResponse(response);
    
    ESP_LOGI(TAG, "Sent tool response: id=%s, success=%d", tool_id.c_str(), success);
}

void ToolManager::RegisterBuiltinTools() {
    // ★ 工具清理（2026-09-16 华哥拍板）：暂时只保留 set_volume。
    //   已移除 set_press_to_talk（按键模式切换）——后续需要时再按需注册。
    // ★ 2026-09-18 根修：删除 Settings("vendor") 读取——该命名空间全工程从未写入
    //   （NVS_READONLY open 必然返回 ESP_ERR_NVS_NOT_FOUND 4354），每次重连都刷一条
    //   红色错误日志；且 set_press_to_talk 已移除，press_to_talk_enabled_ 无消费者。

    RegisterTool("set_volume",
        [](const ToolCommand& cmd, std::string& result) -> bool {
            auto codec = Board::GetInstance().GetAudioCodec();
            // 绝对设置：{"volume": 80}
            auto volume_item = cJSON_GetObjectItem(cmd.parameters, "volume");
            if (cJSON_IsNumber(volume_item)) {
                int volume = volume_item->valueint;
                if (volume < 0) volume = 0;
                if (volume > 100) volume = 100;
                codec->SetOutputVolume(volume);
                ESP_LOGI(TAG, ">>> 设备音量设置为: %d", volume);
                result = "volume_set:" + std::to_string(volume);
                return true;
            }
            // 相对调节：{"delta": +15} / {"delta": -20}（在当前音量上加减，夹紧 0-100）
            auto delta_item = cJSON_GetObjectItem(cmd.parameters, "delta");
            if (cJSON_IsNumber(delta_item)) {
                int volume = codec->output_volume() + delta_item->valueint;
                if (volume < 0) volume = 0;
                if (volume > 100) volume = 100;
                codec->SetOutputVolume(volume);
                ESP_LOGI(TAG, ">>> 设备音量调节 %d → %d", delta_item->valueint, volume);
                result = "volume_set:" + std::to_string(volume);
                return true;
            }
            result = "missing 'volume' or 'delta' parameter";
            return false;
        });

    // ★ 2026-09-20 华哥需求：AI 控制设备重启（用户说"重启一下/重启设备"时服务端下发本工具）。
    //   用 esp_timer 单次定时器延迟 800ms 再重启——先让本次 tool 回执与服务端收尾消息
    //   发出去，且不阻塞主循环（在工具回调里直接 esp_restart 会把回执链路掐断）。
    RegisterTool("reboot",
        [](const ToolCommand& cmd, std::string& result) -> bool {
            ESP_LOGI(TAG, ">>> 收到重启指令，800ms 后重启设备");
            result = "restarting";
            static esp_timer_handle_t reboot_timer = nullptr;
            if (reboot_timer == nullptr) {
                esp_timer_create_args_t args = {};
                args.callback = [](void*) { esp_restart(); };
                args.name = "ai_reboot";
                esp_timer_create(&args, &reboot_timer);
            }
            if (reboot_timer != nullptr) {
                esp_timer_start_once(reboot_timer, 800 * 1000);
                return true;
            }
            result = "reboot timer create failed";
            return false;
        });

    ESP_LOGI(TAG, "Builtin tools registered: %u", (unsigned)tools_.size());
}


