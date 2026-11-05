#include "settings.h"

#include <esp_log.h>
#include <nvs_flash.h>

#define TAG "Settings"

// ★ 2026-09-18 根修（禁止 ESP_ERROR_CHECK 杀整机）：NVS 读写在运行期可失败
//   （分区写满 NVS_NO_FREE_PAGES、键类型不符 NVS_TYPE_ERR、句柄被并发关闭等），
//   原实现一律 ESP_ERROR_CHECK → abort()，把"存储写入失败"升级成"整机崩溃"。
//   存储失败只影响持久化，不影响系统运行，统一降级为错误日志。

Settings::Settings(const std::string& ns, bool read_write) : ns_(ns), read_write_(read_write) {
    esp_err_t err = nvs_open(ns.c_str(), read_write_ ? NVS_READWRITE : NVS_READONLY, &nvs_handle_);
    if (err == ESP_OK) {
        return;
    }
    nvs_handle_ = 0;
    // ★ 2026-09-20 根修（华哥"为什么会有这些 NVS 报错"）：**只读打开一个尚不存在的
    //   命名空间是正常状态**（首次开机 / 从未保存过任何值），不是错误。
    //   nvs_open(NVS_READONLY) 此时必然返回 ESP_ERR_NVS_NOT_FOUND(4354)，
    //   原实现一律 ESP_LOGE → 每次开机都刷一条红色错误日志，误导排查。
    //   各 Get* 在句柄为 0 时已正确返回调用方给的默认值（见 GetInt 等），
    //   行为不受影响，仅把日志级别改为符合语义的 DEBUG。
    //   （同类问题 2026-09-18 曾在调用侧删掉 Settings("vendor") 规避；此处是源头治理，
    //     对所有只读 Settings 使用者一并生效。）
    if (!read_write_ && err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "NVS 命名空间(%s)尚无数据，使用默认值", ns_.c_str());
        return;
    }
    ESP_LOGE(TAG, "NVS 命名空间打开失败(%s): %d", ns_.c_str(), (int)err);
}

Settings::~Settings() {
    if (nvs_handle_ != 0) {
        if (read_write_ && dirty_) {
            esp_err_t err = nvs_commit(nvs_handle_);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "NVS 提交失败(%s): %d", ns_.c_str(), (int)err);
            }
        }
        nvs_close(nvs_handle_);
    }
}

std::string Settings::GetString(const std::string& key, const std::string& default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }

    size_t length = 0;
    if (nvs_get_str(nvs_handle_, key.c_str(), nullptr, &length) != ESP_OK) {
        return default_value;
    }

    std::string value;
    value.resize(length);
    if (nvs_get_str(nvs_handle_, key.c_str(), value.data(), &length) != ESP_OK) {
        ESP_LOGE(TAG, "NVS 读取失败(%s.%s)", ns_.c_str(), key.c_str());
        return default_value;
    }
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

void Settings::SetString(const std::string& key, const std::string& value) {
    if (!read_write_) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return;
    }
    esp_err_t err = nvs_set_str(nvs_handle_, key.c_str(), value.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 写入失败(%s.%s): %d", ns_.c_str(), key.c_str(), (int)err);
        return;
    }
    dirty_ = true;
}

int32_t Settings::GetInt(const std::string& key, int32_t default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }

    int32_t value;
    if (nvs_get_i32(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value;
}

void Settings::SetInt(const std::string& key, int32_t value) {
    if (!read_write_) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return;
    }
    esp_err_t err = nvs_set_i32(nvs_handle_, key.c_str(), value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 写入失败(%s.%s): %d", ns_.c_str(), key.c_str(), (int)err);
        return;
    }
    dirty_ = true;
}

bool Settings::GetBool(const std::string& key, bool default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }

    uint8_t value;
    if (nvs_get_u8(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value != 0;
}

void Settings::SetBool(const std::string& key, bool value) {
    if (!read_write_) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return;
    }
    esp_err_t err = nvs_set_u8(nvs_handle_, key.c_str(), value ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 写入失败(%s.%s): %d", ns_.c_str(), key.c_str(), (int)err);
        return;
    }
    dirty_ = true;
}

void Settings::EraseKey(const std::string& key) {
    if (!read_write_) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return;
    }
    esp_err_t ret = nvs_erase_key(nvs_handle_, key.c_str());
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "NVS 擦除失败(%s.%s): %d", ns_.c_str(), key.c_str(), (int)ret);
    }
}

void Settings::EraseAll() {
    if (!read_write_) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return;
    }
    esp_err_t err = nvs_erase_all(nvs_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 全擦除失败(%s): %d", ns_.c_str(), (int)err);
    }
}