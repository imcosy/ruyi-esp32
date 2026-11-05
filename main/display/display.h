#ifndef DISPLAY_H
#define DISPLAY_H

#include <lvgl.h>

#define HAVE_LVGL 1

#include <esp_timer.h>
#include <esp_log.h>
#include <esp_pm.h>

#include <string>
#include <chrono>
#include <functional>

class Theme {
public:
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;

    inline std::string name() const { return name_; }
private:
    std::string name_;
};

class Display {
public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void SetChatMessage(const char* role, const char* content);
    virtual void ClearChatMessages();
    virtual void SetTheme(Theme* theme);
    virtual Theme* GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    virtual void SetupUI() {
        setup_ui_called_ = true;
    }
    virtual void ShowProductSelector() {}
    virtual void HideProductSelector() {}
    virtual void SetSelectedProduct(const char* product_id) {}
    // 会议总结二维码（2026-09-14 meeting板）：显示/隐藏全屏二维码层，默认无操作
    virtual void ShowMeetingQrcode(const char* content) {}
    virtual void HideMeetingQrcode() {}

    inline int width() const { return width_; }
    inline int height() const { return height_; }
    inline bool IsSetupUICalled() const { return setup_ui_called_; }

protected:
    int width_ = 0;
    int height_ = 0;
    bool setup_ui_called_ = false;

    Theme* current_theme_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

// ★ 2026-09-18 根修（显示锁不得拖死主链路）：
//   ① 语义改为"取不到锁就放弃本次 UI 操作"——原实现 Lock 失败只打日志、照常往下改
//      LVGL 对象树，且析构无条件 Unlock()：等于未持锁写对象树、又释放了别的任务持有的
//      递归锁，比不加锁更危险。调用方统一写 `if (!lock.locked()) return;`。
//   ② 超时 30s → 200ms：调用方含 App 主循环（每 200ms 状态栏刷新）与 esp_timer 任务
//      （通知条到期隐藏），UI 丢一帧无关紧要，但绝不能被显示锁长时间阻塞。
//      一次性初始化路径（SetupUI/主题字体装载）可显式传更长超时。
class DisplayLockGuard {
public:
    explicit DisplayLockGuard(Display *display, int timeout_ms = 200)
        : display_(display), locked_(display->Lock(timeout_ms)) {
        if (!locked_) {
            ESP_LOGW("Display", "显示锁忙(%dms)，跳过本次 UI 操作", timeout_ms);
        }
    }
    ~DisplayLockGuard() {
        if (locked_) {
            display_->Unlock();
        }
    }
    bool locked() const { return locked_; }

private:
    Display *display_;
    bool locked_;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override {
        return true;
    }
    virtual void Unlock() override {}
};

#endif
