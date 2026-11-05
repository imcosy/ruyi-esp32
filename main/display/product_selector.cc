#include "product_selector.h"
#include "esp_log.h"
#include <font_awesome.h>

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(font_awesome_30_4);

#define TAG "ProductSel"

ProductInfo ProductSelector::kProducts[1] = {
    {"dialogue",  "\xE5\xAF\xB9\xE8\xAF\x9D\xE6\xA8\xA1\xE5\xBC\x8F", "\xEF\x82\x86"},
};

static const lv_color_t kCardColors[1] = {
    lv_color_hex(0x2DD4BF),
};
static const lv_color_t kSelectedColor = lv_color_hex(0x9333EA);

ProductSelector::ProductSelector() {}

ProductSelector::~ProductSelector() {
    if (screen_) {
        lv_obj_del(screen_);
        screen_ = nullptr;
    }
}

void ProductSelector::Create(lv_obj_t* parent,
                              const lv_font_t* text_font, const lv_font_t* title_font) {
    text_font_ = text_font ? text_font : &BUILTIN_TEXT_FONT;
    title_font_ = title_font ? title_font : &BUILTIN_TEXT_FONT;

    int parent_w = lv_disp_get_hor_res(lv_disp_get_default());
    int parent_h = lv_disp_get_ver_res(lv_disp_get_default());

    // 待机界面全屏覆盖（含顶部状态栏区域）：退回 idle 时彻底遮住聆听界面的
    // 状态栏/顶栏，避免顶部残留半截状态栏造成界面错乱
    screen_ = lv_obj_create(parent);
    lv_obj_set_pos(screen_, 0, 0);
    lv_obj_set_size(screen_, parent_w, parent_h);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_border_width(screen_, 0, 0);
    lv_obj_set_style_pad_all(screen_, 0, 0);
    lv_obj_set_style_layout(screen_, LV_LAYOUT_NONE, 0);
    lv_obj_set_scrollbar_mode(screen_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(screen_, LV_OBJ_FLAG_HIDDEN);

    // Centered cluster: avatar + title + hint. Flex column adapts to both
    // 240x320 and 240x240. 50px reserved at the bottom for the separator.
    lv_obj_t* cluster = lv_obj_create(screen_);
    lv_obj_set_pos(cluster, 0, 0);
    lv_obj_set_size(cluster, parent_w, parent_h - 50);
    lv_obj_set_style_bg_opa(cluster, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cluster, 0, 0);
    lv_obj_set_style_pad_all(cluster, 0, 0);
    lv_obj_set_style_pad_row(cluster, 12, 0);
    lv_obj_set_flex_flow(cluster, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cluster, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(cluster, LV_SCROLLBAR_MODE_OFF);

    // Circular avatar (reused as cards_[0] so UpdateHighlight/SetSelected keep working)
    cards_[0] = lv_obj_create(cluster);
    lv_obj_set_size(cards_[0], 72, 72);
    lv_obj_set_style_radius(cards_[0], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(cards_[0], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cards_[0], lv_color_hex(0x9333EA), 0);
    lv_obj_set_style_border_width(cards_[0], 0, 0);
    lv_obj_set_style_pad_all(cards_[0], 0, 0);
    lv_obj_clear_flag(cards_[0], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cards_[0], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(cards_[0], LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_shadow_width(cards_[0], 18, 0);
    lv_obj_set_style_shadow_color(cards_[0], lv_color_hex(0x9333EA), 0);
    lv_obj_set_style_shadow_opa(cards_[0], LV_OPA_40, 0);
    lv_obj_set_style_shadow_offset_y(cards_[0], 4, 0);

    card_icons_[0] = lv_label_create(cards_[0]);
    lv_label_set_text(card_icons_[0], FONT_AWESOME_USER_ROBOT);
    lv_obj_set_style_text_font(card_icons_[0], &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(card_icons_[0], lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(card_icons_[0]);

    // Title
    title_label_ = lv_label_create(cluster);
    lv_label_set_text(title_label_, "\xE8\x80\x90\xE6\x80\x9D" "AI");  // 耐思AI
    lv_obj_set_style_text_font(title_label_, title_font_, 0);
    lv_obj_set_style_text_color(title_label_, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title_label_, parent_w - 32);

    // Wake-up hint
    card_labels_[0] = lv_label_create(cluster);
    lv_label_set_text(card_labels_[0],
        "\xE8\xAF\xB4\xE3\x80\x8C\xE4\xBD\xA0\xE5\xA5\xBD\xE5\xA6\x82\xE6\x84\x8F\xE3\x80\x8D"
        "\xE5\x94\xA4\xE9\x86\x92\xE6\x88\x91");  // 说「你好如意」唤醒我
    lv_obj_set_style_text_font(card_labels_[0], text_font_, 0);
    lv_obj_set_style_text_color(card_labels_[0], lv_color_hex(0xA0AEC0), 0);
    lv_obj_set_style_text_align(card_labels_[0], LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(card_labels_[0], LV_LABEL_LONG_WRAP);
    lv_obj_set_width(card_labels_[0], parent_w - 40);

    // Bottom accent separator line
    lv_obj_t* separator = lv_obj_create(screen_);
    lv_obj_set_size(separator, 60, 2);
    lv_obj_set_style_radius(separator, 1, 0);
    lv_obj_set_style_bg_opa(separator, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(separator, lv_color_hex(0x9333EA), 0);
    lv_obj_set_style_border_width(separator, 0, 0);
    lv_obj_set_style_pad_all(separator, 0, 0);
    lv_obj_clear_flag(separator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(separator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(separator, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(separator, LV_ALIGN_BOTTOM_MID, 0, -36);

    selected_id_ = kProducts[0].id;
    selected_name_ = kProducts[0].name;
    UpdateHighlight(0);
}

void ProductSelector::UpdateHighlight(int idx) {
    for (int i = 0; i < kCardCount; i++) {
        if (i == idx) {
            lv_obj_set_style_bg_color(cards_[i], kSelectedColor, 0);
        } else {
            lv_obj_set_style_bg_color(cards_[i], kCardColors[i], 0);
        }
    }
}

void ProductSelector::SetSelected(const char* product_id) {
    if (!product_id) return;
    for (int i = 0; i < kCardCount; i++) {
        if (strcmp(kProducts[i].id, product_id) == 0) {
            selected_id_ = kProducts[i].id;
            selected_name_ = kProducts[i].name;
            UpdateHighlight(i);
            ESP_LOGI(TAG, "\xE6\xA8\xA1\xE5\xBC\x8F\xE5\x88\x87\xE6\x8D\xA2: %s (%s)",
                     selected_name_.c_str(), selected_id_.c_str());
            break;
        }
    }
}

void ProductSelector::Show() {
    if (screen_) {
        lv_obj_clear_flag(screen_, LV_OBJ_FLAG_HIDDEN);
    }
}

void ProductSelector::Hide() {
    if (screen_) {
        lv_obj_add_flag(screen_, LV_OBJ_FLAG_HIDDEN);
    }
}
