#ifndef PRODUCT_SELECTOR_H
#define PRODUCT_SELECTOR_H

#include <lvgl.h>
#include <string>

struct ProductInfo {
    const char* id;
    const char* name;
    const char* icon;
};

class ProductSelector {
public:
    ProductSelector();
    ~ProductSelector();

    void Create(lv_obj_t* parent,
                const lv_font_t* text_font = nullptr, const lv_font_t* title_font = nullptr);
    void Show();
    void Hide();
    bool IsVisible() const { return screen_ != nullptr && lv_obj_has_flag(screen_, LV_OBJ_FLAG_HIDDEN) == 0; }
    void SetSelected(const char* product_id);
    const char* GetSelectedId() const { return selected_id_.c_str(); }
    const char* GetSelectedName() const { return selected_name_.c_str(); }

private:
    void UpdateHighlight(int idx);

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* cards_[1] = {nullptr};
    lv_obj_t* card_labels_[1] = {nullptr};
    lv_obj_t* card_icons_[1] = {nullptr};
    std::string selected_id_;
    std::string selected_name_;
    const lv_font_t* text_font_ = nullptr;
    const lv_font_t* title_font_ = nullptr;

    static ProductInfo kProducts[1];
    static constexpr int kCardCount = 1;
};

#endif
