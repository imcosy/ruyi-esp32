#include "assets.h"
#include "board.h"
#include "display.h"
#include "app.h"
#include "lvgl_theme.h"
#if HAVE_LVGL
#include "display/lcd_display.h"
#include <spi_flash_mmap.h>
#endif

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <cbin_font.h>


#define TAG "Assets"
#define PARTITION_LABEL "assets"

struct mmap_assets_table {
    char asset_name[32];          /*!< Name of the asset */
    uint32_t asset_size;          /*!< Size of the asset */
    uint32_t asset_offset;        /*!< Offset of the asset */
    uint16_t asset_width;         /*!< Width of the asset */
    uint16_t asset_height;        /*!< Height of the asset */
};

Assets::Assets() {
#if HAVE_LVGL
    strategy_ = std::make_unique<Assets::LvglStrategy>();
#else
    strategy_ = std::make_unique<Assets::EmoteStrategy>();
#endif
    // Initialize the partition
    InitializePartition();
}

Assets::~Assets() {
    UnApplyPartition();
}

bool Assets::FindPartition(Assets* assets) {
    assets->partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, PARTITION_LABEL);
    if (assets->partition_ == nullptr) {
        ESP_LOGI(TAG, "No assets partition found");
        return false;
    } else {
        ESP_LOGI(TAG, "Assets partition found: %s", PARTITION_LABEL);
    }
    return true;
}

bool Assets::Apply(bool refresh_display_theme) {
    return strategy_ ? strategy_->Apply(this, refresh_display_theme) : false;
}

bool Assets::InitializePartition() {
    return strategy_ ? strategy_->InitializePartition(this) : false;
}

void Assets::UnApplyPartition() {
    if (strategy_) {
        strategy_->UnApplyPartition(this);
    }
}

bool Assets::GetAssetData(const std::string& name, void*& ptr, size_t& size) {
    return strategy_ ? strategy_->GetAssetData(this, name, ptr, size) : false;
}

bool Assets::LoadSrmodelsFromIndex(Assets* assets, cJSON* root) {
    void* ptr = nullptr;
    size_t size = 0;
    bool need_delete_root = false;

    // If root is not provided, parse index.json
    if (root == nullptr) {
        if (!assets->GetAssetData("index.json", ptr, size)) {
            ESP_LOGE(TAG, "The index.json file is not found");
            return false;
        }

        root = cJSON_ParseWithLength(static_cast<char*>(ptr), size);
        if (root == nullptr) {
            ESP_LOGE(TAG, "The index.json file is not valid");
            return false;
        }
        need_delete_root = true;
    }

    cJSON* srmodels = cJSON_GetObjectItem(root, "srmodels");
    if (cJSON_IsString(srmodels)) {
#if CONFIG_USE_CUSTOM_WAKE_WORD
        ESP_LOGI(TAG, "index.json has srmodels field: %s", srmodels->valuestring);
        std::string srmodels_file = srmodels->valuestring;
        if (assets->GetAssetData(srmodels_file, ptr, size)) {
            ESP_LOGI(TAG, "srmodels.bin loaded, size=%d", size);
            if (assets->models_list_ != nullptr) {
                esp_srmodel_deinit(assets->models_list_);
                assets->models_list_ = nullptr;
            }
            assets->models_list_ = srmodel_load(static_cast<uint8_t*>(ptr));
            if (assets->models_list_ != nullptr) {
                auto& app = App::GetInstance();
                app.GetAudioService().SetModelsList(assets->models_list_);
                if (need_delete_root) {
                    cJSON_Delete(root);
                }
                return true;
            } else {
                ESP_LOGE(TAG, "Failed to load srmodels.bin");
            }
        } else {
            ESP_LOGE(TAG, "The srmodels file %s is not found", srmodels_file.c_str());
        }
#else
        // ★ 2026-09-16 卡滞感根修（华哥确认架构）：s2s 唤醒走服务端——未唤醒时设备
        //   持续上行短语音，服务端帧数门控（10~40帧）+ anyWordWakeup 匹配唤醒词。
        //   本地唤醒推理（MultiNet7 每 32ms 一帧 + 2.6MB 模型内存 + 加载期 SRAM
        //   最低水位 9.9KB）由 USE_CUSTOM_WAKE_WORD 总门控，关闭时模型完全不进内存。
        ESP_LOGI(TAG, "srmodels field present (%s) but USE_CUSTOM_WAKE_WORD=n, "
                      "skip loading (唤醒走服务端)", srmodels->valuestring);
#endif
    } else {
        ESP_LOGW(TAG, "index.json has NO srmodels field (srmodels=%p)", (void*)srmodels);
    }

    if (need_delete_root) {
        cJSON_Delete(root);
    }
    return false;
}

#if HAVE_LVGL
uint32_t Assets::LvglStrategy::CalculateChecksum(const char* data, uint32_t length) {
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < length; i++) {
        checksum += data[i];
    }
    return checksum & 0xFFFF;
}

bool Assets::LvglStrategy::InitializePartition(Assets* assets) {
    assets->partition_valid_ = false;
    assets_.clear();

    if (!Assets::FindPartition(assets)) {
        return false;
    }

    int free_pages = spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA);
    uint32_t storage_size = free_pages * 64 * 1024;
    ESP_LOGI(TAG, "The storage free size is %ld KB", storage_size / 1024);
    ESP_LOGI(TAG, "The partition size is %ld KB", assets->partition_->size / 1024);
    if (storage_size < assets->partition_->size) {
        ESP_LOGE(TAG, "The free size %ld KB is less than assets partition required %ld KB", storage_size / 1024, assets->partition_->size / 1024);
        return false;
    }

    esp_err_t err = esp_partition_mmap(assets->partition_, 0, assets->partition_->size, ESP_PARTITION_MMAP_DATA, (const void**)&mmap_root_, &mmap_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mmap assets partition: %s", esp_err_to_name(err));
        return false;
    }

    assets->partition_valid_ = true;

    uint32_t stored_files = *(uint32_t*)(mmap_root_ + 0);
    uint32_t stored_chksum = *(uint32_t*)(mmap_root_ + 4);
    uint32_t stored_len = *(uint32_t*)(mmap_root_ + 8);

    if (stored_len > assets->partition_->size - 12) {
        ESP_LOGD(TAG, "The stored_len (0x%lx) is greater than the partition size (0x%lx) - 12", stored_len, assets->partition_->size);
        return false;
    }

    auto start_time = esp_timer_get_time();
    uint32_t calculated_checksum = CalculateChecksum(mmap_root_ + 12, stored_len);
    auto end_time = esp_timer_get_time();
    ESP_LOGI(TAG, "The checksum calculation time is %d ms", int((end_time - start_time) / 1000));

    if (calculated_checksum != stored_chksum) {
        ESP_LOGE(TAG, "校验和不匹配: 计算值(0x%lx) != 存储值(0x%lx)", calculated_checksum, stored_chksum);
        assets->partition_valid_ = false;
        return false;
    }

    checksum_valid_ = true;

    for (uint32_t i = 0; i < stored_files; i++) {
        auto item = (const mmap_assets_table*)(mmap_root_ + 12 + i * sizeof(mmap_assets_table));
        auto asset = Asset{
            .size = static_cast<size_t>(item->asset_size),
            .offset = static_cast<size_t>(12 + sizeof(mmap_assets_table) * stored_files + item->asset_offset)
        };
        assets_[item->asset_name] = asset;
    }
    return checksum_valid_;
}

void Assets::LvglStrategy::UnApplyPartition(Assets* assets) {
    if (mmap_handle_ != 0) {
        esp_partition_munmap(mmap_handle_);
        mmap_handle_ = 0;
        mmap_root_ = nullptr;
    }
    checksum_valid_ = false;
    assets_.clear();
    (void)assets; // Unused parameter
}

bool Assets::LvglStrategy::GetAssetData(Assets* assets, const std::string& name, void*& ptr, size_t& size) {
    auto asset = assets_.find(name);
    if (asset == assets_.end()) {
        return false;
    }
    auto data = (const char*)(mmap_root_ + asset->second.offset);
    if (data[0] != 'Z' || data[1] != 'Z') {
        ESP_LOGE(TAG, "The asset %s is not valid with magic %02x%02x", name.c_str(), data[0], data[1]);
        return false;
    }

    ptr = static_cast<void*>(const_cast<char*>(data + 2));
    size = asset->second.size;
    return true;
}

bool Assets::LvglStrategy::Apply(Assets* assets, bool refresh_display_theme) {
    void* ptr = nullptr;
    size_t size = 0;
    if (!assets->GetAssetData("index.json", ptr, size)) {
        ESP_LOGE(TAG, "The index.json file is not found");
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(static_cast<char*>(ptr), size);
    if (root == nullptr) {
        ESP_LOGE(TAG, "The index.json file is not valid");
        return false;
    }

    cJSON* version = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsNumber(version)) {
        if (version->valuedouble > 1) {
            ESP_LOGE(TAG, "The assets version %d is not supported, please upgrade the firmware", version->valueint);
            cJSON_Delete(root);   // ★ 2026-09-18：原实现该分支泄漏整棵 JSON 树
            return false;
        }
    }

    Assets::LoadSrmodelsFromIndex(assets, root);

    auto& theme_manager = LvglThemeManager::GetInstance();
    auto light_theme = theme_manager.GetTheme("light");
    auto dark_theme = theme_manager.GetTheme("dark");

    cJSON* font = cJSON_GetObjectItem(root, "text_font");
    if (cJSON_IsString(font)) {
        std::string fonts_text_file = font->valuestring;
        if (assets->GetAssetData(fonts_text_file, ptr, size)) {
            auto text_font = std::make_shared<LvglCBinFont>(ptr);
            if (text_font->font() == nullptr) {
                ESP_LOGE(TAG, "Failed to load fonts.bin");
                return false;
            }
            if (light_theme != nullptr) {
                light_theme->set_text_font(text_font);
            }
            if (dark_theme != nullptr) {
                dark_theme->set_text_font(text_font);
            }
            // ★ 自定义主题（tool.cc 写入NV配色后由 LcdDisplay 构造注册）同样换
            //   全字库，否则自定义主题下 SetupUI 固化的仍是内置小字库（文字不全）
            auto custom_theme = theme_manager.GetTheme("custom");
            if (custom_theme != nullptr) {
                custom_theme->set_text_font(text_font);
            }
        } else {
            ESP_LOGE(TAG, "The font file %s is not found", fonts_text_file.c_str());
        }
    }

    cJSON* skin = cJSON_GetObjectItem(root, "skin");
    if (cJSON_IsObject(skin)) {
        cJSON* light_skin = cJSON_GetObjectItem(skin, "light");
        if (cJSON_IsObject(light_skin) && light_theme != nullptr) {
            cJSON* text_color = cJSON_GetObjectItem(light_skin, "text_color");
            cJSON* background_color = cJSON_GetObjectItem(light_skin, "background_color");
            cJSON* background_image = cJSON_GetObjectItem(light_skin, "background_image");
            if (cJSON_IsString(text_color)) {
                light_theme->set_text_color(LvglTheme::ParseColor(text_color->valuestring));
            }
            if (cJSON_IsString(background_color)) {
                light_theme->set_background_color(LvglTheme::ParseColor(background_color->valuestring));
                light_theme->set_chat_background_color(LvglTheme::ParseColor(background_color->valuestring));
            }
            if (cJSON_IsString(background_image)) {
                if (!assets->GetAssetData(background_image->valuestring, ptr, size)) {
                    ESP_LOGE(TAG, "The background image file %s is not found", background_image->valuestring);
                    return false;
                }
                auto background_image = std::make_shared<LvglCBinImage>(ptr);
                light_theme->set_background_image(background_image);
            }
        }
        cJSON* dark_skin = cJSON_GetObjectItem(skin, "dark");
        if (cJSON_IsObject(dark_skin) && dark_theme != nullptr) {
            cJSON* text_color = cJSON_GetObjectItem(dark_skin, "text_color");
            cJSON* background_color = cJSON_GetObjectItem(dark_skin, "background_color");
            cJSON* background_image = cJSON_GetObjectItem(dark_skin, "background_image");
            if (cJSON_IsString(text_color)) {
                dark_theme->set_text_color(LvglTheme::ParseColor(text_color->valuestring));
            }
            if (cJSON_IsString(background_color)) {
                dark_theme->set_background_color(LvglTheme::ParseColor(background_color->valuestring));
                dark_theme->set_chat_background_color(LvglTheme::ParseColor(background_color->valuestring));
            }
            if (cJSON_IsString(background_image)) {
                if (!assets->GetAssetData(background_image->valuestring, ptr, size)) {
                    ESP_LOGE(TAG, "The background image file %s is not found", background_image->valuestring);
                    return false;
                }
                auto background_image = std::make_shared<LvglCBinImage>(ptr);
                dark_theme->set_background_image(background_image);
            }
        }
    }

    if (refresh_display_theme) {
        auto display = Board::GetInstance().GetDisplay();
        ESP_LOGI(TAG, "Refreshing display theme...");

        auto current_theme = display->GetTheme();
        if (current_theme != nullptr) {
            display->SetTheme(current_theme);
        }

        // Parse hide_subtitle configuration
        cJSON* hide_subtitle = cJSON_GetObjectItem(root, "hide_subtitle");
        if (cJSON_IsBool(hide_subtitle)) {
            bool hide = cJSON_IsTrue(hide_subtitle);
            auto lcd_display = dynamic_cast<LcdDisplay*>(display);
            if (lcd_display != nullptr) {
                ESP_LOGI(TAG, "hide_subtitle=%s (not implemented in LcdDisplay)", hide ? "true" : "false");
            }
        }
    }
    
    cJSON_Delete(root);
    return true;
}
#endif // HAVE_LVGL

bool Assets::EmoteStrategy::InitializePartition(Assets* assets) {
    assets->partition_valid_ = false;
    (void)assets;
    return false;
}

void Assets::EmoteStrategy::UnApplyPartition(Assets* assets) {
    (void)assets;
}

bool Assets::EmoteStrategy::GetAssetData(Assets* assets, const std::string& name, void*& ptr, size_t& size) {
    (void)assets;
    (void)name;
    ptr = nullptr;
    size = 0;
    return false;
}

bool Assets::EmoteStrategy::Apply(Assets* assets, bool refresh_display_theme) {
    Assets::LoadSrmodelsFromIndex(assets);
    (void)refresh_display_theme;
    return true;
}

bool Assets::Download(std::string url, std::function<void(int progress, size_t speed)> progress_callback) {
    ESP_LOGI(TAG, "Downloading new version of assets from %s", url.c_str());

    // 取消当前资源分区的内存映射
    UnApplyPartition();

    // ★ 2026-09-18 根修（unmap 后失败必须回滚）：本函数解映射后，任何失败 return
    //   （HTTP 打开 / 状态码 / 长度校验 / 擦除 / 写入 / 大小校验，共 6+ 处）都会让
    //   已生效的字体、图片指针指向未映射的 flash → 后续访问 LoadProhibited 崩溃。
    //   用 RAII 守卫保证"未重新映射即自动恢复"，日后新增失败分支也不会再漏。
    struct PartitionRestoreGuard {
        Assets* self;
        bool restored = false;
        ~PartitionRestoreGuard() {
            if (!restored) {
                self->InitializePartition();
            }
        }
    } partition_guard{this};

    // 下载新的资源文件
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get assets, status code: %d", http->GetStatusCode());
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        return false;
    }

    if (content_length > partition_->size) {
        ESP_LOGE(TAG, "资产文件大小 (%u) 超过分区大小 (%lu)", content_length, partition_->size);
        return false;
    }

    // 定义扇区大小为4KB（ESP32的标准扇区大小）
    const size_t SECTOR_SIZE = esp_partition_get_main_flash_sector_size();
    
    // 计算需要擦除的扇区数量
    size_t sectors_to_erase = (content_length + SECTOR_SIZE - 1) / SECTOR_SIZE; // 向上取整
    size_t total_erase_size = sectors_to_erase * SECTOR_SIZE;
    
    ESP_LOGI(TAG, "Sector size: %u, content length: %u, sectors to erase: %u, total erase size: %u", 
             SECTOR_SIZE, content_length, sectors_to_erase, total_erase_size);
    
    // 写入新的资源文件到分区，一边erase一边写入
    char* buffer = (char*)heap_caps_malloc(SECTOR_SIZE, MALLOC_CAP_INTERNAL);
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return false;
    }
    size_t total_written = 0;
    size_t recent_written = 0;
    size_t current_sector = 0;
    auto last_calc_time = esp_timer_get_time();
    
    while (true) {
        int ret = http->Read(buffer, SECTOR_SIZE);
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            heap_caps_free(buffer);
            return false;
        }

        if (ret == 0) {
            break;
        }

        // 检查是否需要擦除新的扇区
        size_t write_end_offset = total_written + ret;
        size_t needed_sectors = (write_end_offset + SECTOR_SIZE - 1) / SECTOR_SIZE;
        
        // 擦除需要的新扇区
        while (current_sector < needed_sectors) {
            size_t sector_start = current_sector * SECTOR_SIZE;
            size_t sector_end = (current_sector + 1) * SECTOR_SIZE;
            
            // 确保擦除范围不超过分区大小
            if (sector_end > partition_->size) {
                ESP_LOGE(TAG, "扇区结束(%u)超过分区大小(%lu)", sector_end, partition_->size);
                heap_caps_free(buffer);
                return false;
            }
            
            ESP_LOGD(TAG, "Erasing sector %u (offset: %u, size: %u)", current_sector, sector_start, SECTOR_SIZE);
            esp_err_t err = esp_partition_erase_range(partition_, sector_start, SECTOR_SIZE);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to erase sector %u at offset %u: %s", current_sector, sector_start, esp_err_to_name(err));
                heap_caps_free(buffer);
                return false;
            }
            
            current_sector++;
        }

        // 写入数据到分区
        esp_err_t err = esp_partition_write(partition_, total_written, buffer, ret);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write to assets partition at offset %u: %s", total_written, esp_err_to_name(err));
            heap_caps_free(buffer);
            return false;
        }

        total_written += ret;
        recent_written += ret;

        // 计算进度和速度
        if (esp_timer_get_time() - last_calc_time >= 1000000 || total_written == content_length || ret == 0) {
            size_t progress = total_written * 100 / content_length;
            size_t speed = recent_written; // 每秒的字节数
            ESP_LOGI(TAG, "进度: %u%% (%u/%u), 速度: %u B/s, 已擦除扇区数: %u", 
                     progress, total_written, content_length, speed, current_sector);
            if (progress_callback) {
                progress_callback(progress, speed);
            }
            last_calc_time = esp_timer_get_time();
            recent_written = 0; // 重置最近写入的字节数
        }
    }
    
    http->Close();
    heap_caps_free(buffer);

    if (total_written != content_length) {
        ESP_LOGE(TAG, "下载大小 (%u) 与预期大小 (%u) 不匹配", total_written, content_length);
        return false;
    }

    ESP_LOGI(TAG, "资产下载完成，已写入总字节数: %u, 已擦除总扇区数: %u", 
             total_written, current_sector);

    // 重新初始化资源分区
    if (!InitializePartition()) {
        ESP_LOGE(TAG, "资产分区重新初始化失败");
        return false;
    }
    partition_guard.restored = true;   // 已重新映射，守卫不再动作

    return true;
}
