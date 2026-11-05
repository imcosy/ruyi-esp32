#include "lcd_display.h"
#include "product_selector.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "audio/assets/lang_config.h"
#include "websocket.h"

#include <vector>
#include <algorithm>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <src/misc/cache/lv_cache.h>
#include "qrcode/qrcodegen.h"

#include "board.h"

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_awesome_30_4);

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_4);

    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x0A0A0A));
    dark_theme->set_text_color(lv_color_hex(0xF0F0F0));
    dark_theme->set_chat_background_color(lv_color_hex(0x111111));
    dark_theme->set_user_bubble_color(lv_color_hex(0x2D2D3D));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x9333EA));
    dark_theme->set_system_bubble_color(lv_color_hex(0x1A1A1A));
    dark_theme->set_system_text_color(lv_color_hex(0xA0AEC0));
    dark_theme->set_border_color(lv_color_hex(0x1AFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xE94560));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // 恢复NVS中保存的自定义配色（如有）
    {
        Settings nvs("display", false);
        if (nvs.GetBool("has_custom_theme", false)) {
            auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
            auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
            auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_4);

            auto parse_nvs_color = [&nvs](const char* key, lv_color_t fallback) -> lv_color_t {
                std::string s = nvs.GetString(key, "");
                if (s.empty()) return fallback;
                unsigned int val = static_cast<unsigned int>(strtoul(s.c_str(), nullptr, 16));
                return lv_color_make((val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF);
            };

            auto* dark = static_cast<LvglTheme*>(LvglThemeManager::GetInstance().GetTheme("dark"));
            lv_color_t def_bg = dark ? dark->background_color() : lv_color_hex(0x0A0A0A);
            lv_color_t def_stc = dark ? dark->status_text_color() : lv_color_hex(0xCCCCCC);
            lv_color_t def_txt = dark ? dark->text_color() : lv_color_hex(0xF0F0F0);
            lv_color_t def_cbg = dark ? dark->chat_background_color() : lv_color_hex(0x111111);
            lv_color_t def_ub = dark ? dark->user_bubble_color() : lv_color_hex(0x2D2D3D);
            lv_color_t def_ab = dark ? dark->assistant_bubble_color() : lv_color_hex(0x9333EA);
            lv_color_t def_sb = dark ? dark->system_bubble_color() : lv_color_hex(0x1A1A1A);
            lv_color_t def_stxt = dark ? dark->system_text_color() : lv_color_hex(0xA0AEC0);
            lv_color_t def_brd = dark ? dark->border_color() : lv_color_hex(0x1AFFFFFF);
            lv_color_t def_lbt = dark ? dark->low_battery_color() : lv_color_hex(0xE94560);

            auto* custom = new LvglTheme("custom");
            custom->set_background_color(parse_nvs_color("custom_theme_bg", def_bg));
            custom->set_status_text_color(parse_nvs_color("custom_theme_status_text", def_stc));
            custom->set_text_color(parse_nvs_color("custom_theme_text", def_txt));
            custom->set_chat_background_color(parse_nvs_color("custom_theme_chat_bg", def_cbg));
            custom->set_user_bubble_color(parse_nvs_color("custom_theme_user_bubble", def_ub));
            custom->set_assistant_bubble_color(parse_nvs_color("custom_theme_assistant_bubble", def_ab));
            custom->set_system_bubble_color(parse_nvs_color("custom_theme_system_bubble", def_sb));
            custom->set_system_text_color(parse_nvs_color("custom_theme_system_text", def_stxt));
            custom->set_border_color(parse_nvs_color("custom_theme_border", def_brd));
            custom->set_low_battery_color(parse_nvs_color("custom_theme_low_battery", def_lbt));
            custom->set_text_font(text_font);
            custom->set_icon_font(icon_font);
            custom->set_large_icon_font(large_icon_font);

            LvglThemeManager::GetInstance().RegisterTheme("custom", custom);
            ESP_LOGI(TAG, "🎨 从NVS恢复自定义主题配色");
        }
    }

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "dark");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);
    if (current_theme_ == nullptr) {
        if (theme_name != "dark") {
            ESP_LOGW(TAG, "Theme '%s' not found, falling back to 'dark'", theme_name.c_str());
            settings.SetString("theme", "dark");
        }
        current_theme_ = LvglThemeManager::GetInstance().GetTheme("dark");
    }
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
#if CONFIG_LV_USE_BUILTIN_MALLOC
        // ★ 2026-09-20 对齐 xiaozhi 上游：LVGL builtin TLSF 池本体在 PSRAM
        //   （LV_MEM_POOL_INCLUDE 注入），此处为 8MB PSRAM 板扩池——TLSF 拒绝单池
        //   大于 LV_MEM_SIZE（POOL_EXPAND_SIZE=0 时 block_size_max 上限），
        //   所以按 LV_MEM_SIZE 分块追加。
        //   ★ 2026-09-20 池预算收缩（0070 实测崩溃根修）：原 extra_total=2560KB
        //   （池共 3.5MB）把 PSRAM 余量挤爆——AFE（MORE_PSRAM 模式）+ 音频队列
        //   分配失败 → PsramAllocator abort。本板收缩为池共 2MB（+1024KB）、
        //   图片缓存 1MB，给 AFE/音频留 ~2MB。
        const size_t extra_total = 1024 * 1024;
        const size_t chunk_size = LV_MEM_SIZE;
        size_t added = 0;
        while (added < extra_total) {
            size_t this_chunk = extra_total - added;
            if (this_chunk > chunk_size) {
                this_chunk = chunk_size;
            }
            void* pool = heap_caps_malloc(this_chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (pool == nullptr || lv_mem_add_pool(pool, this_chunk) == nullptr) {
                if (pool != nullptr) {
                    heap_caps_free(pool);
                }
                ESP_LOGE(TAG, "Failed to add %uKB LVGL PSRAM pool (added %uKB)",
                         (unsigned)(this_chunk / 1024), (unsigned)(added / 1024));
                break;
            }
            added += this_chunk;
        }
        if (added == extra_total) {
            ESP_LOGI(TAG, "Added %uKB LVGL pool in PSRAM", (unsigned)(added / 1024));
        }
#endif
        lv_image_cache_resize(1 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 1MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}


// RGB LCD implementation
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 0,
            .full_refresh = 1,
            .direct_mode = 1,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        }
    };
    
    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }
    
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                            int width, int height,  int offset_x, int offset_y,
                            bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram =false,
            .sw_rotate = true,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        }
    };
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    if (product_selector_) {
        delete product_selector_;
        product_selector_ = nullptr;
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    
    // 开机一次性初始化：给足锁等待时间（此后 UI 刷新一律 200ms 短超时）。
    // ★ 先取锁再置 setup_ui_called_——锁失败时不留"已初始化"的假状态，
    //   否则后续所有 UI 调用都会以为对象树已建好而访问空指针。
    DisplayLockGuard lock(this, 5000);
    if (!lock.locked()) {
        ESP_LOGE(TAG, "SetupUI 取显示锁失败，UI 未初始化");
        return;
    }
    Display::SetupUI();  // Mark SetupUI as called

    if (current_theme_ == nullptr) {
        ESP_LOGW(TAG, "current_theme_ is null, falling back to 'dark' theme");
        current_theme_ = LvglThemeManager::GetInstance().GetTheme("dark");
    }
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    if (lvgl_theme == nullptr || lvgl_theme->text_font() == nullptr || lvgl_theme->icon_font() == nullptr || lvgl_theme->large_icon_font() == nullptr) {
        ESP_LOGE(TAG, "Failed to get valid theme with fonts, aborting SetupUI");
        return;
    }
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);
    
    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(), 0); // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);
    
    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0); // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    // Create product selector (display-only, mode switching via voice)
    // 注意：先于低电量弹窗创建，保证弹窗层级在待机界面之上
    CreateProductSelector(text_font);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}
#if CONFIG_IDF_TARGET_ESP32P4
#define  MAX_MESSAGES 40
#else
#define  MAX_MESSAGES 20
#endif

// ─── 流式用户气泡（asr_partial 边说边更新，不新建气泡） ──────────────
// 查找最后一个流式用户气泡：content_ > container(右对齐全宽) > bubble(user_data="user_stream")
static lv_obj_t* FindLastStreamBubble(lv_obj_t* content_) {
    uint32_t n = lv_obj_get_child_cnt(content_);
    if (n == 0) return nullptr;
    lv_obj_t* last_container = lv_obj_get_child(content_, n - 1);
    if (last_container == nullptr || !lv_obj_is_valid(last_container)) return nullptr;
    if (lv_obj_get_child_cnt(last_container) == 0) return nullptr;
    lv_obj_t* bubble = lv_obj_get_child(last_container, 0);
    if (bubble == nullptr || !lv_obj_is_valid(bubble)) return nullptr;
    void* ud = lv_obj_get_user_data(bubble);
    if (ud != nullptr && strcmp((const char*)ud, "user_stream") == 0) return bubble;
    return nullptr;
}

// 更新气泡文本并重算宽度（与新建路径的宽度约束一致）
static void UpdateBubbleText(lv_obj_t* bubble, const char* content) {
    if (bubble == nullptr || !lv_obj_is_valid(bubble)) return;
    if (lv_obj_get_child_cnt(bubble) == 0) return;
    lv_obj_t* msg_text = lv_obj_get_child(bubble, 0);
    if (msg_text == nullptr || !lv_obj_is_valid(msg_text)) return;
    lv_label_set_text(msg_text, content);

    // 重算文本宽度并约束（与新建路径一致：min 20 / max 85%屏宽-16）
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;
    lv_coord_t min_width = 20;
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);
    if (text_width < min_width) text_width = min_width;
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;
    lv_obj_set_width(msg_text, bubble_width);
    lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    // 滚动到最新消息
    lv_obj_scroll_to_view_recursive(lv_obj_get_parent(bubble), LV_ANIM_OFF);
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetChatMessage('%s', '%s') failed: content_ is nullptr (SetupUI() was called but container not created)", role, content);
        }
        return;
    }

    // ── 流式用户消息：更新最后一个流式气泡，不新建 ──
    // "user_stream"：asr_partial 边说边更新（全量替换文本）
    // "user_final" ：asr_result 定稿——更新文本并解除流式标记（下一轮 partial 不再覆盖它）
    if (strcmp(role, "user_stream") == 0 || strcmp(role, "user_final") == 0) {
        lv_obj_t* stream_bubble = FindLastStreamBubble(content_);
        if (stream_bubble != nullptr) {
            if (strlen(content) > 0) {
                UpdateBubbleText(stream_bubble, content);
            }
            if (strcmp(role, "user_final") == 0) {
                lv_obj_set_user_data(stream_bubble, (void*)"user");
            }
            return;
        }
        // 无流式气泡：落到新建路径（user_final→普通user气泡；user_stream→流式气泡，
        // 空文本由下方 Avoid empty 检查拦截）
    }

    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            // 同步删除：在 DisplayLockGuard 锁内立即删除，避免 del_async 延迟期间
            // LVGL 绘制线程访问已标记删除对象导致 use-after-free（draw_scrollbar 崩溃）
            lv_obj_del(first_child);
            child_count = lv_obj_get_child_cnt(content_);
        }
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }
    
    if (strcmp(role, "system") == 0) {
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) && lv_obj_get_child_cnt(last_container) > 0) {
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr && strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    }

    // Avoid empty message boxes
    if(strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // 流式role样式归一：user_stream/user_final 复用 user 样式，仅气泡标记不同
    // user_stream → "user_stream"（可被后续 partial 更新）；user_final → "user"（定稿）
    const char* style_role = role;
    const char* bubble_mark = role;
    if (strcmp(role, "user_stream") == 0) {
        style_role = "user";
        bubble_mark = "user_stream";
    } else if (strcmp(role, "user_final") == 0) {
        style_role = "user";
        bubble_mark = "user";
    }

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);

    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;

    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);

    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;

    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(style_role, "user") == 0) {
        // User messages: 深灰背景 #2D2D3D, 纯白文字, 不透明, 圆角12
        lv_obj_set_style_bg_color(msg_bubble, lv_color_hex(0x2D2D3D), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(msg_bubble, 12, 0);
        lv_obj_set_style_text_color(msg_text, lv_color_hex(0xFFFFFF), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)bubble_mark);

        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(style_role, "assistant") == 0) {
        // Assistant messages: 品牌紫 #9333EA, 纯白文字, 不透明, 圆角12
        lv_obj_set_style_bg_color(msg_bubble, lv_color_hex(0x9333EA), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(msg_bubble, 12, 0);
        lv_obj_set_style_text_color(msg_text, lv_color_hex(0xFFFFFF), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");

        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(style_role, "system") == 0) {
        // System messages: 卡片色 #1A1A1A, 次文字 #A0AEC0, 不透明, 圆角8
        lv_obj_set_style_bg_color(msg_bubble, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(msg_bubble, 8, 0);
        lv_obj_set_style_text_color(msg_text, lv_color_hex(0xA0AEC0), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");

        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }
    
    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(style_role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);
        
        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);
        
        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(style_role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }
    
    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (!lock.locked()) {
        return;
    }
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }
    
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);
    
    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);
    
    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);
    
    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;  // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100; // 50% of screen height
    
    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld", img_width, img_height, max_width, max_height);
    }
    
    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;
    
    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256) zoom = 256;
    
    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);
    
    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release(); // Release ownership of smart pointer
    lv_obj_add_event_cb(preview_image, [](lv_event_t* e) {
        LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
        if (img != nullptr) {
            delete img; // Properly release memory by deleting LvglImage object
        }
    }, LV_EVENT_DELETE, (void*)raw_image);
    
    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;
    
    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);
    
    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);
    
    // Center the image within the bubble
    lv_obj_center(preview_image);
    
    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (!lock.locked()) {
        return;
    }
    if (content_ == nullptr) {
        return;
    }
    
    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);
    
    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;

    // Show product selector in idle state
    ShowProductSelector();

    ESP_LOGI(TAG, "Chat messages cleared");
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    if (!lock.locked()) {
        return;
    }
    
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    if (lvgl_theme == nullptr || lvgl_theme->text_font() == nullptr || lvgl_theme->icon_font() == nullptr || lvgl_theme->large_icon_font() == nullptr) {
        ESP_LOGE(TAG, "SetTheme: invalid theme or missing fonts, ignoring");
        return;
    }
    
    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    if (text_font->line_height >= 40) {
        lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
    } else {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    // Set background image
    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }
    
    // Update top bar background color with 50% opacity
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }
    
    // Update status bar elements
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->status_text_color(), 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);

    // If we have the chat message style, update all message bubbles
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr) continue;

        lv_obj_t* bubble = nullptr;

        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }

        if (bubble == nullptr) continue;

        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);

            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0);
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }

            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);

            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "child[%lu] Bubble type is not found", i);
        }
    }

    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::CreateProductSelector(const lv_font_t* text_font, const lv_font_t* title_font) {
    if (product_selector_ == nullptr) {
        product_selector_ = new ProductSelector();
    }
    auto screen = lv_screen_active();
    product_selector_->Create(screen, text_font, title_font);
}

// ★ 2026-09-18：选择器显示/隐藏由 App 主循环在状态切换时调用，必须与 LVGL 渲染任务
//   互斥（原实现完全不加锁，直接改对象树 = 与渲染并发写同一对象）
void LcdDisplay::ShowProductSelector() {
    DisplayLockGuard lock(this);
    if (lock.locked() && product_selector_ != nullptr) {
        product_selector_->Show();
    }
}

void LcdDisplay::HideProductSelector() {
    DisplayLockGuard lock(this);
    if (lock.locked() && product_selector_ != nullptr) {
        product_selector_->Hide();
    }
}

bool LcdDisplay::IsProductSelectorVisible() const {
    return product_selector_ != nullptr && product_selector_->IsVisible();
}

void LcdDisplay::SetSelectedProduct(const char* product_id) {
    DisplayLockGuard lock(this);
    if (lock.locked() && product_selector_ != nullptr) {
        product_selector_->SetSelected(product_id);
    }
}

const char* LcdDisplay::GetSelectedProductId() const {
    return product_selector_ != nullptr ? product_selector_->GetSelectedId() : nullptr;
}

// ====== 会议总结二维码全屏层（2026-09-14 meeting板） ======
// bye 消息携带 qrcode 内容（URL），设备本地生成二维码显示（零图片传输）。
// QR 生成用 main/qrcode/qrcodegen.c（Nayuki MIT 库）——LVGL 自带 lv_qrcode 与
// esp_emote_gfx 组件的 qrcodegen 同符号链接冲突（multiple definition），故关
// LV_USE_QRCODE 改用本组件源码 + lv_canvas(RGB565) 手绘。
// 黑底全屏 + 顶部标题 + 居中二维码；容器 CLICKABLE 拦截触摸穿透，
// 显示期间用户误触不会改变设备状态，由 App 侧倒计时统一销毁并回 Idle。
// ★ 2026-09-18 结构整改：
//   ① QR 编码 + 画布位图填充是 CPU/PSRAM 重活（数万次写 + 两次分配），原实现整段持
//      LVGL 锁执行，单次持锁可达数十毫秒，直接放大"主循环/其它任务等锁"的卡死概率；
//      现全部移到锁外预计算，锁内只做 LVGL 对象树操作。
//   ② 锁收敛为 DisplayLockGuard（原为裸 Lock(30000) + 4 处手工 Unlock，任一 return
//      漏配对就永久占锁）。锁不可用时安全退出，不留悬空指针。
//   ③ 日志不再打印完整 URL（含 token），只打长度。
void LcdDisplay::ShowMeetingQrcode(const char* content) {
    // ── 锁外预计算 ─────────────────────────────────────────────────
    uint16_t* canvas_buf = nullptr;
    void* old_buf = meeting_qrcode_buf_;             // 旧缓冲随旧层在锁内一起释放
    int modules = 0, scale = 0;
    const int area = 200;                            // 二维码绘制区（320x240 屏）
    bool have_new = false;

    if (content != nullptr && content[0] != '\0') {
        uint8_t* q0 = (uint8_t*)heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint8_t* temp = (uint8_t*)heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (q0 != nullptr && temp != nullptr) {
            bool ok = qrcodegen_encodeText(content, temp, q0, qrcodegen_Ecc_MEDIUM,
                                           qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                           qrcodegen_Mask_AUTO, true);
            if (ok) {
                modules = qrcodegen_getSize(q0);
                scale = area / (modules + 4);   // 4 模块标准白边 quiet zone
                if (scale < 2) scale = 2;
                int qr_px = modules * scale;
                int pad = (area - qr_px) / 2;

                // ★ 64 字节对齐分配（2026-09-15 崩溃加固）：LVGL v9 sw 渲染的 blend 路径存在
                //   uint32 指针优化，PSRAM canvas 源同样要求对齐（MALLOC_CAP_8BIT 普通分配仅
                //   保证 4 字节，aligned_alloc(64) 覆盖所有渲染路径的对齐假设）
                canvas_buf = (uint16_t*)heap_caps_aligned_alloc(
                    64, area * area * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (canvas_buf != nullptr) {
                    for (int i = 0; i < area * area; i++) {
                        canvas_buf[i] = 0xFFFF;   // 白底
                    }
                    for (int my = 0; my < modules; my++) {
                        for (int mx = 0; mx < modules; mx++) {
                            if (!qrcodegen_getModule(q0, mx, my)) continue;
                            // 模块 → scale×scale 黑块（RGB565 黑=0x0000）
                            for (int dy = 0; dy < scale; dy++) {
                                uint16_t* row = canvas_buf + (pad + my * scale + dy) * area + pad + mx * scale;
                                for (int dx = 0; dx < scale; dx++) row[dx] = 0x0000;
                            }
                        }
                    }
                    have_new = true;
                } else {
                    ESP_LOGE(TAG, "ShowMeetingQrcode: alloc canvas buffer failed");
                }
            } else {
                ESP_LOGE(TAG, "ShowMeetingQrcode: 二维码编码失败（url_len=%u）", (unsigned)strlen(content));
            }
        } else {
            ESP_LOGE(TAG, "ShowMeetingQrcode: alloc qrcode buffer failed");
        }
        if (q0 != nullptr) heap_caps_free(q0);
        if (temp != nullptr) heap_caps_free(temp);
    }

    // ── 锁内：仅 LVGL 对象树操作 ────────────────────────────────────
    {
        DisplayLockGuard lock(this);
        if (!lock.locked()) {
            // 锁不可用：新缓冲未挂到对象树，直接释放；旧层/旧缓冲保持原状（一致，无悬空）
            if (canvas_buf != nullptr) heap_caps_free(canvas_buf);
            return;
        }
        if (meeting_qrcode_layer_ != nullptr) {   // 已在显示：先销毁重建（内容可能变化）
            lv_obj_delete(meeting_qrcode_layer_);
            meeting_qrcode_layer_ = nullptr;
        }
        if (old_buf != nullptr) {                 // 旧画布缓冲随旧层一起释放
            heap_caps_free(old_buf);
            meeting_qrcode_buf_ = nullptr;
        }
        if (!have_new) {
            return;
        }

        // 全屏黑底容器
        meeting_qrcode_layer_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(meeting_qrcode_layer_, width_, height_);
    lv_obj_set_pos(meeting_qrcode_layer_, 0, 0);
    lv_obj_set_style_bg_color(meeting_qrcode_layer_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(meeting_qrcode_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(meeting_qrcode_layer_, 0, 0);
    lv_obj_set_style_radius(meeting_qrcode_layer_, 0, 0);
    lv_obj_set_style_pad_all(meeting_qrcode_layer_, 0, 0);
    lv_obj_clear_flag(meeting_qrcode_layer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(meeting_qrcode_layer_, LV_OBJ_FLAG_CLICKABLE);   // 拦截触摸穿透
    lv_obj_move_foreground(meeting_qrcode_layer_);

    // 顶部标题
    // ★ 2026-09-15 晚 meeting 板崩溃根修（Guru Meditation LoadStoreAlignment）：
    //   meeting 板默认字体 Montserrat14 无中文字形，中文标题渲染为 fallback 方框且
    //   label 实际宽度为奇数 → invalidate 区域 x1 奇数 → LVGL 渲染 dest（IRAM DMA
    //   draw buffer）行起点 2mod4 → lv_draw_sw_blend_to_rgb565.c 的 opacity 填充
    //   fast path（x+=2 步进 + uint32 指针强转）在奇数像素位未对齐访问 → ESP32-S3
    //   默认 -mno-unaligned-access 直接硬崩。修复：label 设固定偶数宽度（整屏宽），
    //   invalidate 区域 x1 恒为 0（4 对齐），文本居中显示不变。
    lv_obj_t* title = lv_label_create(meeting_qrcode_layer_);
    lv_obj_set_width(title, width_ - (width_ % 4));
    lv_label_set_text(title, "扫码获取会议总结");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // 画布缓冲与 QR 位图已在锁外预计算完成（见函数头注释）
        meeting_qrcode_buf_ = canvas_buf;
        lv_obj_t* canvas = lv_canvas_create(meeting_qrcode_layer_);
        lv_canvas_set_buffer(canvas, meeting_qrcode_buf_, area, area, LV_COLOR_FORMAT_RGB565);
        lv_obj_center(canvas);
    }

    ESP_LOGI(TAG, "ShowMeetingQrcode: url_len=%u modules=%d scale=%d",
             (unsigned)strlen(content), modules, scale);
}

void LcdDisplay::HideMeetingQrcode() {
    void* buf = nullptr;
    {
        DisplayLockGuard lock(this);
        if (!lock.locked()) {
            return;   // 锁不可用：层与缓冲保持一致，下次调用再清（不留悬空指针）
        }
        if (meeting_qrcode_layer_ != nullptr) {
            lv_obj_delete(meeting_qrcode_layer_);
            meeting_qrcode_layer_ = nullptr;
            ESP_LOGI(TAG, "HideMeetingQrcode");
        }
        buf = meeting_qrcode_buf_;   // 层已销毁，缓冲可安全释放（放锁外，避免锁内做 heap 操作）
        meeting_qrcode_buf_ = nullptr;
    }
    if (buf != nullptr) {
        heap_caps_free(buf);
    }
}

const char* LcdDisplay::GetSelectedProductName() const {
    return product_selector_ != nullptr ? product_selector_->GetSelectedName() : nullptr;
}
