#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "lvgl_display.h"
#include "display/product_selector.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <atomic>
#include <memory>
#include <functional>


class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    lv_obj_t* meeting_qrcode_layer_ = nullptr;   // 会议总结二维码全屏层
    void* meeting_qrcode_buf_ = nullptr;         // 二维码 canvas RGB565 缓冲（PSRAM）
    ProductSelector* product_selector_ = nullptr;

    void InitializeLcdThemes();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);

public:
    ~LcdDisplay();
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;

    void CreateProductSelector(const lv_font_t* text_font = nullptr, const lv_font_t* title_font = nullptr);
    void ShowProductSelector();
    void HideProductSelector();
    bool IsProductSelectorVisible() const;
    void SetSelectedProduct(const char* product_id);
    const char* GetSelectedProductId() const;
    const char* GetSelectedProductName() const;
    // 会议总结二维码全屏层（meeting板 bye 消息触发，60s 自动销毁）
    virtual void ShowMeetingQrcode(const char* content) override;
    virtual void HideMeetingQrcode() override;
    virtual void SetTheme(Theme* theme) override;
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif // LCD_DISPLAY_H