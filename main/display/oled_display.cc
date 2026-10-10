#include "oled_display.h"
#include "assets/lang_config.h"
#include "lvgl_theme.h"
#include "lvgl_font.h"

#include <string>
#include <algorithm>
#include <cstring>
#include <cctype>

#include <esp_log.h>
#include <esp_err.h>
#include <esp_random.h>
#include <esp_lvgl_port.h>
#include <font_awesome.h>

#define TAG "OledDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_awesome_30_1);

OledDisplay::OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
    int width, int height, bool mirror_x, bool mirror_y, bool swap_xy)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_1);
    
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("dark", dark_theme);
    current_theme_ = dark_theme;

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.task_stack = 8192;  // 6144'ten artırıldı - WDT crash önlemek için
    port_cfg.timer_period_ms = 20;  // Refresh hızını düşür (default 5ms)
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding OLED display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * height_),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = true,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    // I2C transaction fail olduğunda wait_for_flushing sonsuz döngüye girer.
    // flush_wait_cb set edilince LVGL kendi while(flushing) döngüsü yerine
    // bu callback'i çağırır. Callback içinde flush_ready çağırarak hemen bitiriyoruz.
    lv_display_set_flush_wait_cb(display_, [](lv_display_t* disp) {
        // Kısa bekle, I2C zaten async çalışıyor
        // Eğer I2C fail olduysa flush_ready gelmeyecek, biz kendimiz çağırıyoruz
        vTaskDelay(pdMS_TO_TICKS(50));
        lv_display_flush_ready(disp);
    });

    // Note: SetupUI() should be called by Application::Initialize(), not in constructor
    // to ensure lvgl objects are created after the display is fully initialized.
}

void OledDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    
    Display::SetupUI();  // Mark SetupUI as called
    if (height_ == 64) {
        SetupUI_128x64();
    } else {
        SetupUI_128x32();
    }
}

OledDisplay::~OledDisplay() {
    if (eye_anim_timer_ != nullptr) {
        lv_timer_del(eye_anim_timer_);
        eye_anim_timer_ = nullptr;
    }

    if (content_ != nullptr) {
        lv_obj_del(content_);
    }

    bool is_128x64_layout = (top_bar_ != nullptr);
    if (status_bar_ != nullptr && is_128x64_layout) {
        status_label_ = nullptr;
        notification_label_ = nullptr;
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        network_label_ = nullptr;
        mute_label_ = nullptr;
        battery_label_ = nullptr;
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        if (!is_128x64_layout) {
            status_label_ = nullptr;
            notification_label_ = nullptr;
            network_label_ = nullptr;
            mute_label_ = nullptr;
            battery_label_ = nullptr;
        }
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
    lvgl_port_deinit();
}

bool OledDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void OledDisplay::Unlock() {
    lvgl_port_unlock();
}

void OledDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (!lock.IsLocked()) {
        return;  // Ekran meşgul, bu güncellemeyi atla - WDT'yi engelleme
    }
    if (chat_message_label_ == nullptr) {
        // 128x64 eye-only UI may not create chat label, but we still parse music intent below.
    }

    // Replace all newlines with spaces
    std::string content_str = (content != nullptr) ? content : "";
    std::replace(content_str.begin(), content_str.end(), '\n', ' ');

    if (chat_message_label_ != nullptr && content_right_ == nullptr) {
        lv_label_set_text(chat_message_label_, content_str.c_str());
    } else if (chat_message_label_ != nullptr) {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(chat_message_label_, content_str.c_str());
            lv_obj_remove_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Music icon should appear only when assistant says music playback is starting.
    std::string lowered = content_str;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const bool music_phrase = (lowered.find("açıyorum") != std::string::npos) ||
                              (lowered.find("aciyorum") != std::string::npos) ||
                              (lowered.find("çalıyorum") != std::string::npos) ||
                              (lowered.find("caliyorum") != std::string::npos);
    if (music_phrase) {
        music_mode_pending_ = true;
        if (eyes_enabled_ && !boot_anim_active_ && music_icon_ != nullptr && !overlay_active_ &&
            status_label_ != nullptr && std::strcmp(lv_label_get_text(status_label_), Lang::Strings::SPEAKING) == 0) {
            show_music_only_ = true;
            lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(music_icon_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Also catch network failure alerts that come via SetChatMessage("system", ...).
    const bool network_error_msg =
        (content != nullptr) &&
        ((std::strstr(content, Lang::Strings::SERVER_ERROR) != nullptr) ||
         (std::strstr(content, Lang::Strings::SERVER_NOT_CONNECTED) != nullptr) ||
         (std::strstr(content, Lang::Strings::SERVER_TIMEOUT) != nullptr) ||
         (std::strstr(content, "无法连接服务") != nullptr) ||
         (std::strstr(content, "请检查网络") != nullptr));

    if (network_error_msg) {
        wifi_connected_ = false;
        show_wifi_warning_ = true;
        show_music_only_ = false;
        if (eyes_enabled_ && !boot_anim_active_) {
            SetWifiWarningVisible(true);
            lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(left_brow_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(right_brow_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void OledDisplay::SetupUI_128x64() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lv_color_black(), 0);

    /* Layer 1: Top bar - hidden in eye mode */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, 0);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_color(top_bar_, lv_color_white(), 0);
    lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

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

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  // Overlap with top_bar_
    lv_obj_set_style_text_color(status_bar_, lv_color_white(), 0);
    lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content */
    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(content_, lv_color_black(), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_pad_top(content_, 0, 0);
    lv_obj_set_style_pad_bottom(content_, 0, 0);
    lv_obj_set_style_pad_left(content_, 0, 0);
    lv_obj_set_style_pad_right(content_, 0, 0);
    lv_obj_set_style_pad_row(content_, 0, 0);

    eye_area_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(eye_area_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(eye_area_, lv_color_black(), 0);
    lv_obj_set_style_border_width(eye_area_, 0, 0);
    lv_obj_set_style_pad_all(eye_area_, 0, 0);
    lv_obj_set_scrollbar_mode(eye_area_, LV_SCROLLBAR_MODE_OFF);

    content_right_ = nullptr;
    chat_message_label_ = nullptr;

    CreateEyes();

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_black(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}

void OledDisplay::SetupUI_128x32() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_column(container_, 0, 0);

    /* Emotion label on the left side */
    content_ = lv_obj_create(container_);
    lv_obj_set_size(content_, 32, 32);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_radius(content_, 0, 0);

    emotion_label_ = lv_label_create(content_);
    lv_obj_set_style_text_font(emotion_label_, large_icon_font, 0);
    lv_label_set_text(emotion_label_, FONT_AWESOME_MICROCHIP_AI);
    lv_obj_center(emotion_label_);

    /* Right side */
    side_bar_ = lv_obj_create(container_);
    lv_obj_set_size(side_bar_, width_ - 32, 32);
    lv_obj_set_flex_flow(side_bar_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(side_bar_, 0, 0);
    lv_obj_set_style_border_width(side_bar_, 0, 0);
    lv_obj_set_style_radius(side_bar_, 0, 0);
    lv_obj_set_style_pad_row(side_bar_, 0, 0);

    /* Status bar */
    status_bar_ = lv_obj_create(side_bar_);
    lv_obj_set_size(status_bar_, width_ - 32, 16);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_obj_set_style_pad_left(status_label_, 2, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_pad_left(notification_label_, 2, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    chat_message_label_ = lv_label_create(side_bar_);
    lv_obj_set_size(chat_message_label_, width_ - 32, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(chat_message_label_, 2, 0);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(chat_message_label_, "");

    // Start scrolling subtitle after a delay
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000), LV_PART_MAIN);
}

void OledDisplay::SetEmotion(const char* emotion) {
    const char* utf8 = font_awesome_get_utf8(emotion);
    DisplayLockGuard lock(this);
    if (!lock.IsLocked()) {
        return;  // Ekran meşgul, atla
    }
    requested_emotion_ = ParseEyeEmotion(emotion);
    if (!standby_sleep_mode_) {
        eye_emotion_ = requested_emotion_;
    }

    if (overlay_active_) {
        ESP_LOGD(TAG, "SetEmotion('%s') ignored: overlay active", emotion);
        return;
    }

    if (eyes_enabled_) {
        UpdateEyes();
        return;
    }
    if (emotion_label_ == nullptr) {
        return;
    }
    if (utf8 != nullptr) {
        lv_label_set_text(emotion_label_, utf8);
    } else {
        lv_label_set_text(emotion_label_, FONT_AWESOME_NEUTRAL);
    }
}

void OledDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    if (!lock.IsLocked()) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto text_font = lvgl_theme->text_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
}

OledDisplay::EyeEmotion OledDisplay::ParseEyeEmotion(const char* emotion) {
    if (emotion == nullptr) {
        return EyeEmotion::Neutral;
    }
    std::string e = emotion;
    std::transform(e.begin(), e.end(), e.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (e.find("happy") != std::string::npos || e.find("joy") != std::string::npos ||
        e.find("smile") != std::string::npos || e.find("laugh") != std::string::npos ||
        e.find("mutlu") != std::string::npos || e.find("excited") != std::string::npos) {
        return EyeEmotion::Happy;
    }
    if (e.find("sad") != std::string::npos || e.find("cry") != std::string::npos ||
        e.find("üzg") != std::string::npos || e.find("uzg") != std::string::npos) {
        return EyeEmotion::Sad;
    }
    if (e.find("angry") != std::string::npos || e.find("mad") != std::string::npos ||
        e.find("kızg") != std::string::npos || e.find("kizg") != std::string::npos) {
        return EyeEmotion::Angry;
    }
    if (e.find("sleep") != std::string::npos || e.find("tired") != std::string::npos ||
        e.find("uyku") != std::string::npos) {
        return EyeEmotion::Sleepy;
    }
    if (e.find("surpris") != std::string::npos || e.find("wow") != std::string::npos ||
        e.find("şaşk") != std::string::npos || e.find("sask") != std::string::npos) {
        return EyeEmotion::Surprised;
    }
    if (e.find("fear") != std::string::npos || e.find("nervous") != std::string::npos) {
        return EyeEmotion::Surprised;
    }
    return EyeEmotion::Neutral;
}

void OledDisplay::CreateEyes() {
    if (height_ != 64 || eye_area_ == nullptr) {
        eyes_enabled_ = false;
        return;
    }

    eyes_enabled_ = true;
    boot_anim_active_ = true;
    device_ready_ = false;
    wifi_connected_ = false;
    boot_anim_ticks_ = 0;

    left_eye_ = lv_obj_create(eye_area_);
    right_eye_ = lv_obj_create(eye_area_);
    left_brow_ = lv_obj_create(eye_area_);
    right_brow_ = lv_obj_create(eye_area_);
    boot_tube_ = lv_obj_create(eye_area_);
    boot_fill_ = lv_obj_create(boot_tube_);
    boot_glow_ = lv_obj_create(boot_tube_);

    music_icon_ = lv_label_create(eye_area_);
    const char* music_utf8 = font_awesome_get_utf8("music");
    lv_label_set_text(music_icon_, (music_utf8 != nullptr) ? music_utf8 : FONT_AWESOME_MICROPHONE_SLASH);
    lv_obj_set_style_text_color(music_icon_, lv_color_white(), 0);
    lv_obj_set_style_text_font(music_icon_, &font_awesome_30_1, 0);
    lv_obj_align(music_icon_, LV_ALIGN_CENTER, 0, 2);
    lv_obj_add_flag(music_icon_, LV_OBJ_FLAG_HIDDEN);

    // Generic overlay icon (camera/lock etc.)
    overlay_icon_ = lv_label_create(eye_area_);
    const char* cam_utf8 = font_awesome_get_utf8("camera");
    lv_label_set_text(overlay_icon_, (cam_utf8 != nullptr) ? cam_utf8 : FONT_AWESOME_MICROCHIP_AI);
    lv_obj_set_style_text_color(overlay_icon_, lv_color_white(), 0);
    lv_obj_set_style_text_font(overlay_icon_, &font_awesome_30_1, 0);
    lv_obj_align(overlay_icon_, LV_ALIGN_CENTER, 0, 2);
    lv_obj_add_flag(overlay_icon_, LV_OBJ_FLAG_HIDDEN);

    wifi_warning_label_ = lv_label_create(eye_area_);
    lv_obj_set_width(wifi_warning_label_, width_ - 8);
    lv_obj_set_style_text_align(wifi_warning_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(wifi_warning_label_, lv_color_white(), 0);
    lv_label_set_text(wifi_warning_label_, "WiFi kontrol edin");
    lv_obj_align(wifi_warning_label_, LV_ALIGN_CENTER, 0, 2);
    lv_obj_add_flag(wifi_warning_label_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* objs[] = {left_eye_, right_eye_, left_brow_, right_brow_, boot_tube_, boot_fill_, boot_glow_};
    for (lv_obj_t* obj : objs) {
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_pad_all(obj, 0, 0);
        lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    }

    lv_obj_set_style_bg_color(left_eye_, lv_color_white(), 0);
    lv_obj_set_style_bg_color(right_eye_, lv_color_white(), 0);
    lv_obj_set_style_radius(left_eye_, 7, 0);
    lv_obj_set_style_radius(right_eye_, 7, 0);
    lv_obj_set_style_bg_color(left_brow_, lv_color_white(), 0);
    lv_obj_set_style_bg_color(right_brow_, lv_color_white(), 0);

    lv_obj_set_style_bg_color(boot_tube_, lv_color_hex(0x181818), 0);
    lv_obj_set_style_border_width(boot_tube_, 1, 0);
    lv_obj_set_style_border_color(boot_tube_, lv_color_hex(0x5A5A5A), 0);
    lv_obj_set_style_radius(boot_tube_, 12, 0);
    lv_obj_set_size(boot_tube_, 98, 22);
    lv_obj_align(boot_tube_, LV_ALIGN_CENTER, 0, 2);

    lv_obj_set_style_bg_color(boot_fill_, lv_color_white(), 0);
    lv_obj_set_style_radius(boot_fill_, 10, 0);
    lv_obj_set_size(boot_fill_, 2, 16);
    lv_obj_align(boot_fill_, LV_ALIGN_LEFT_MID, 3, 0);

    lv_obj_set_style_bg_color(boot_glow_, lv_color_hex(0xE8E8E8), 0);
    lv_obj_set_style_radius(boot_glow_, 8, 0);
    lv_obj_set_size(boot_glow_, 6, 12);
    lv_obj_align(boot_glow_, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_flag(boot_glow_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(left_brow_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_brow_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(music_icon_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(overlay_icon_, LV_OBJ_FLAG_HIDDEN);

    // Radyo ikonu — kalıcı, ShowRadioIcon() çağrılana kadar gizli
    radio_icon_ = lv_label_create(eye_area_);
    const char* radio_utf8 = font_awesome_get_utf8("radio");
    // "radio" yoksa "broadcast-tower" veya "tower-broadcast" dene
    if (radio_utf8 == nullptr) radio_utf8 = font_awesome_get_utf8("tower-broadcast");
    if (radio_utf8 == nullptr) radio_utf8 = font_awesome_get_utf8("broadcast-tower");
    if (radio_utf8 == nullptr) radio_utf8 = font_awesome_get_utf8("music"); // fallback
    lv_label_set_text(radio_icon_, (radio_utf8 != nullptr) ? radio_utf8 : "R");
    lv_obj_set_style_text_color(radio_icon_, lv_color_white(), 0);
    lv_obj_set_style_text_font(radio_icon_, &font_awesome_30_1, 0);
    lv_obj_align(radio_icon_, LV_ALIGN_CENTER, 0, 8);
    lv_obj_add_flag(radio_icon_, LV_OBJ_FLAG_HIDDEN);

    eye_anim_timer_ = lv_timer_create(EyeAnimTimerCb, 40, this);
}

void OledDisplay::SetOverlayIcon(const char* icon, bool visible) {
    DisplayLockGuard lock(this);
    if (!lock.IsLocked()) return;
    if (!eyes_enabled_ || overlay_icon_ == nullptr) {
        // Non-eye layout: fallback to emotion label
        if (visible && icon) SetEmotion(icon);
        return;
    }
    const char* utf8 = (icon != nullptr) ? font_awesome_get_utf8(icon) : nullptr;
    if (visible && utf8 != nullptr) {
        overlay_active_ = true;
        lv_label_set_text(overlay_icon_, utf8);
        // Overlay should be on top of eyes
        lv_obj_remove_flag(overlay_icon_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(left_brow_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_brow_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(music_icon_, LV_OBJ_FLAG_HIDDEN);
        SetWifiWarningVisible(false);
    } else {
        overlay_active_ = false;
        lv_obj_add_flag(overlay_icon_, LV_OBJ_FLAG_HIDDEN);
        // restore eyes if ready and no other exclusive mode
        if (!boot_anim_active_ && !show_music_only_ && !show_wifi_warning_) {
            lv_obj_remove_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
            UpdateEyes();
        }
    }
}

void OledDisplay::ShowOverlayIcon(const char* icon, int duration_ms) {
    // Radyo modu aktifken başka overlay ikonunu bastırma
    if (radio_mode_active_) return;
    SetOverlayIcon(icon, true);
    if (duration_ms <= 0) return;
    // Use LVGL timer for auto-hide
    lv_timer_t* t = lv_timer_create([](lv_timer_t* timer) {
        auto* self = static_cast<OledDisplay*>(lv_timer_get_user_data(timer));
        if (self) self->SetOverlayIcon(nullptr, false);
        lv_timer_del(timer);
    }, duration_ms, this);
    (void)t;
}

void OledDisplay::ShowRadioIcon() {
    DisplayLockGuard lock(this);
    if (!lock.IsLocked()) return;
    radio_mode_active_ = true;
    if (radio_icon_ == nullptr) return;

    // Diğer tüm icon/göz bileşenlerini gizle
    if (eyes_enabled_) {
        lv_obj_add_flag(left_eye_,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_eye_,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(left_brow_,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_brow_,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(music_icon_,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(overlay_icon_,LV_OBJ_FLAG_HIDDEN);
        SetWifiWarningVisible(false);
    }
    // Radyo ikonunu göster
    lv_obj_remove_flag(radio_icon_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(radio_icon_);
    ESP_LOGI(TAG, "Radyo ikonu gosterildi");
}

void OledDisplay::HideRadioIcon() {
    DisplayLockGuard lock(this);
    if (!lock.IsLocked()) return;
    radio_mode_active_ = false;
    if (radio_icon_ != nullptr) {
        lv_obj_add_flag(radio_icon_, LV_OBJ_FLAG_HIDDEN);
    }
    // Gözleri geri getir (boot animasyonu bitmiş ve başka özel mod yoksa)
    if (eyes_enabled_ && !boot_anim_active_ && !overlay_active_ && !show_music_only_ && !show_wifi_warning_) {
        lv_obj_remove_flag(left_eye_,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
        UpdateEyes();
    }
    ESP_LOGI(TAG, "Radyo ikonu gizlendi");
}

void OledDisplay::ApplyEyeEmotionStyle(int eye_w, int eye_h, int eye_y_offset, int gaze_amp) {
    lv_obj_set_size(left_eye_, eye_w, eye_h);
    lv_obj_set_size(right_eye_, eye_w, eye_h);
    const int gap = 6;
    const int total_w = eye_w * 2 + gap;
    const int base_left = (width_ - total_w) / 2;
    const int base_right = base_left + eye_w + gap;
    const int center_y = ((height_ - eye_h) / 2) + 2;
    const int gaze_shift = gaze_dir_ * gaze_amp;
    lv_obj_set_pos(left_eye_, base_left + gaze_shift, center_y + eye_y_offset);
    lv_obj_set_pos(right_eye_, base_right + gaze_shift, center_y + eye_y_offset);
}

void OledDisplay::UpdateEyes() {
    if (!eyes_enabled_ || left_eye_ == nullptr || right_eye_ == nullptr) {
        return;
    }

    int eye_w = 60;
    int eye_h = 30;
    int eye_y_offset = 0;
    int radius = 12;
    int gaze_amp = 4;
    bool show_brows = false;
    int brow_lx = 0;
    int brow_rx = 0;
    int brow_ly = 0;
    int brow_ry = 0;

    switch (eye_emotion_) {
        case EyeEmotion::Happy:
            eye_h = 24;
            eye_w = 60;
            eye_y_offset = 2;
            radius = 16;
            eye_w += ((gaze_tick_ % 12) < 6) ? 2 : 0;
            eye_h -= ((gaze_tick_ % 16) < 8) ? 0 : 2;
            break;
        case EyeEmotion::Sad:
            eye_h = 20;
            eye_w = 54;
            eye_y_offset = 4;
            radius = 10;
            eye_h -= ((gaze_tick_ % 20) < 10) ? 0 : 2;
            break;
        case EyeEmotion::Angry:
            eye_h = 16;
            eye_w = 60;
            eye_y_offset = 4;
            radius = 6;
            gaze_amp = 1;
            show_brows = true;
            break;
        case EyeEmotion::Sleepy:
            eye_h = 10;
            eye_w = 58;
            // Gentle sleepy breathing
            eye_y_offset = 4 + ((gaze_tick_ % 24) < 12 ? 0 : 2);
            radius = 5;
            gaze_amp = 1;
            break;
        case EyeEmotion::Surprised:
            eye_h = 36;
            eye_w = 50;
            eye_y_offset = 0;
            radius = 18;
            gaze_amp = 2;
            break;
        case EyeEmotion::Neutral:
        default:
            break;
    }

    if (blink_frame_ > 0) {
        const int blink_height = (blink_frame_ == 1) ? 12 : 4;
        eye_h = std::min(eye_h, blink_height);
        eye_y_offset += 3;
    }

    // Wake-up animation: quickly open wider, then settle.
    if (wakeup_anim_ticks_ > 0) {
        const int phase = wakeup_anim_ticks_;
        if (phase > 16) {
            eye_h += 7;   // dramatic opening
            eye_w += 4;
            eye_y_offset -= 2;
            gaze_amp = 6;
        } else if (phase > 8) {
            eye_h += 4;
            eye_w += 2;
            eye_y_offset -= 1;
            gaze_amp = 5;
        } else {
            eye_h += 1;
            gaze_amp = 4;
        }
    }

    ApplyEyeEmotionStyle(eye_w, eye_h, eye_y_offset, gaze_amp);
    lv_obj_set_style_radius(left_eye_, radius, 0);
    lv_obj_set_style_radius(right_eye_, radius, 0);

    if (show_brows) {
        const int brow_y = lv_obj_get_y(left_eye_) - 7;
        const int left_x = lv_obj_get_x(left_eye_);
        const int right_x = lv_obj_get_x(right_eye_);
        lv_obj_set_size(left_brow_, 26, 3);
        lv_obj_set_size(right_brow_, 26, 3);
        brow_lx = left_x + 3;
        brow_rx = right_x + eye_w - 29;
        brow_ly = brow_y + 3;   // inward tilt
        brow_ry = brow_y - 2;   // inward tilt
        lv_obj_set_pos(left_brow_, brow_lx, brow_ly);
        lv_obj_set_pos(right_brow_, brow_rx, brow_ry);
        lv_obj_remove_flag(left_brow_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_brow_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(left_brow_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_brow_, LV_OBJ_FLAG_HIDDEN);
    }
}

void OledDisplay::EyeAnimTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<OledDisplay*>(lv_timer_get_user_data(timer));
    if (self == nullptr || !self->eyes_enabled_) {
        return;
    }

    // When overlay is active (camera/lock), show ONLY the icon.
    if (self->overlay_active_) {
        return;
    }

    // Radyo modu aktifken göz animasyonunu durdur, sadece radyo ikonu görünsün.
    if (self->radio_mode_active_) {
        return;
    }

    if (self->boot_anim_active_) {
        self->boot_anim_ticks_++;
        const int inner_w = 92;
        const int cycle = self->boot_anim_ticks_ % 90;
        const int fill_w = std::max(2, (cycle * inner_w) / 90);
        lv_obj_set_size(self->boot_fill_, std::max(2, fill_w), 16);
        lv_obj_align(self->boot_fill_, LV_ALIGN_LEFT_MID, 3, 0);
        if (self->device_ready_) {
            self->boot_anim_active_ = false;
            lv_obj_add_flag(self->boot_tube_, LV_OBJ_FLAG_HIDDEN);
            if (self->show_wifi_warning_) {
                self->SetWifiWarningVisible(true);
            } else if (self->show_music_only_) {
                lv_obj_remove_flag(self->music_icon_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(self->left_eye_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(self->right_eye_, LV_OBJ_FLAG_HIDDEN);
                self->UpdateEyes();
            }
        }
        return;
    }

    if (self->show_music_only_ || self->show_wifi_warning_) {
        return;
    }

    if (self->wakeup_anim_ticks_ > 0) {
        self->wakeup_anim_ticks_--;
    }

    self->gaze_tick_++;
    if ((self->gaze_tick_ % 22) == 0) {
        const int phase = (self->gaze_tick_ / 22) % 3;
        self->gaze_dir_ = phase - 1;  // -1, 0, +1
    }

    if (self->blink_frame_ > 0) {
        self->blink_frame_--;
    } else {
        self->next_blink_tick_--;
        if (self->next_blink_tick_ <= 0) {
            self->blink_frame_ = 2;
            self->next_blink_tick_ = 70 + (esp_random() % 110);
        }
    }

    self->UpdateEyes();
}

void OledDisplay::SetStatus(const char* status) {
    LvglDisplay::SetStatus(status);

    DisplayLockGuard lock(this);
    if (!lock.IsLocked() || !eyes_enabled_ || music_icon_ == nullptr) {
        return;
    }

    if (overlay_active_) {
        ESP_LOGD(TAG, "SetStatus ignored for eyes: overlay active");
        return;
    }

    const bool speaking = (status != nullptr) && (std::strcmp(status, Lang::Strings::SPEAKING) == 0);
    const bool listening = (status != nullptr) && (std::strcmp(status, Lang::Strings::LISTENING) == 0);
    const bool standby = (status != nullptr) && (std::strcmp(status, Lang::Strings::STANDBY) == 0);
    // NOTE:
    // Lang::Strings::CONNECTING is also used for protocol/session flow after wake-up,
    // so using it here causes false Wi-Fi warning flashes.
    // Show Wi-Fi warning only for actual network registration / wifi-config stages.
    const bool connecting = (status != nullptr) && (
        (std::strcmp(status, Lang::Strings::REGISTERING_NETWORK) == 0) ||
        (std::strcmp(status, Lang::Strings::ENTERING_WIFI_CONFIG_MODE) == 0) ||
        (std::strcmp(status, Lang::Strings::WIFI_CONFIG_MODE) == 0));
    const bool protocol_connecting = (status != nullptr) &&
        (std::strcmp(status, Lang::Strings::CONNECTING) == 0);
    const bool ready_status = standby || listening || speaking;

    if (ready_status) {
        device_ready_ = true;
    }

    // Wi-Fi warning should only be visible during active reconnect/connection attempts.
    if (connecting || (protocol_connecting && !wifi_connected_)) {
        show_wifi_warning_ = true;
        show_music_only_ = false;
    } else if (wifi_connected_) {
        show_wifi_warning_ = false;
    }

    if (standby) {
        standby_sleep_mode_ = true;
        eye_emotion_ = EyeEmotion::Sleepy;
    } else if (listening) {
        const bool was_sleeping = standby_sleep_mode_;
        standby_sleep_mode_ = false;
        eye_emotion_ = requested_emotion_;
        if (was_sleeping) {
            wakeup_anim_ticks_ = 24;  // ~1 second at 40ms tick
            blink_frame_ = 0;
            next_blink_tick_ = 40;
        }
    } else if (!standby_sleep_mode_) {
        eye_emotion_ = requested_emotion_;
    }

    if (speaking && music_mode_pending_) {
        show_music_only_ = true;
    } else if (!speaking && show_music_only_) {
        show_music_only_ = false;
        music_mode_pending_ = false;
    }

    if (show_music_only_) {
        lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(left_brow_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_brow_, LV_OBJ_FLAG_HIDDEN);
        SetWifiWarningVisible(false);
        lv_obj_remove_flag(music_icon_, LV_OBJ_FLAG_HIDDEN);
    } else if (show_wifi_warning_) {
        lv_obj_add_flag(music_icon_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(left_brow_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_brow_, LV_OBJ_FLAG_HIDDEN);
        SetWifiWarningVisible(true);
    } else {
        SetWifiWarningVisible(false);
        lv_obj_add_flag(music_icon_, LV_OBJ_FLAG_HIDDEN);
        if (!boot_anim_active_) {
            lv_obj_remove_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
            UpdateEyes();
        }
    }
}

void OledDisplay::SetWifiWarningVisible(bool visible) {
    if (wifi_warning_label_ == nullptr) {
        return;
    }
    if (visible) {
        // Wi-Fi warning should be the only thing on screen.
        boot_anim_active_ = false;
        if (boot_tube_ != nullptr) {
            lv_obj_add_flag(boot_tube_, LV_OBJ_FLAG_HIDDEN);
        }
        if (music_icon_ != nullptr) {
            lv_obj_add_flag(music_icon_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text(wifi_warning_label_, "WiFi kontrol edin");
        lv_obj_remove_flag(wifi_warning_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_center(wifi_warning_label_);
    } else {
        lv_obj_add_flag(wifi_warning_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void OledDisplay::ShowNotification(const char* notification, int duration_ms) {
    LvglDisplay::ShowNotification(notification, duration_ms);

    DisplayLockGuard lock(this);
    if (!lock.IsLocked()) {
        return;
    }

    if (notification == nullptr) {
        return;
    }

    const bool wifi_scanning = (std::strcmp(notification, Lang::Strings::SCANNING_WIFI) == 0);
    const bool wifi_config_hint =
        (std::strstr(notification, Lang::Strings::CONNECT_TO_HOTSPOT) != nullptr) ||
        (std::strstr(notification, Lang::Strings::ACCESS_VIA_BROWSER) != nullptr) ||
        (std::strstr(notification, "192.168.4.1") != nullptr) ||
        (std::strstr(notification, "Xiaozhi-") != nullptr);
    const bool network_service_error =
        (std::strstr(notification, Lang::Strings::SERVER_NOT_CONNECTED) != nullptr) ||
        (std::strstr(notification, Lang::Strings::SERVER_ERROR) != nullptr) ||
        (std::strstr(notification, Lang::Strings::SERVER_TIMEOUT) != nullptr);
    const bool wifi_connected =
        (std::strstr(notification, Lang::Strings::CONNECTED_TO) != nullptr) ||
        (std::strcmp(notification, Lang::Strings::CONNECTION_SUCCESSFUL) == 0);

    if (wifi_scanning || wifi_config_hint || network_service_error) {
        wifi_connected_ = false;
        show_wifi_warning_ = true;
        if (!boot_anim_active_) {
            SetWifiWarningVisible(true);
            lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(left_brow_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(right_brow_, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (wifi_connected) {
        wifi_connected_ = true;
        show_wifi_warning_ = false;
        if (!boot_anim_active_) {
            SetWifiWarningVisible(false);
            lv_obj_remove_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
            UpdateEyes();
        }
    }
}
void OledDisplay::ShowMessageOverlay(const char* message, int duration_ms) {
    DisplayLockGuard lock(this);
    if (!lock.IsLocked()) return;

    if (wifi_warning_label_) {
        lv_label_set_text(wifi_warning_label_, message);
        lv_obj_remove_flag(wifi_warning_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_center(wifi_warning_label_);
        lv_obj_add_flag(left_eye_,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
        if (left_brow_)    lv_obj_add_flag(left_brow_,    LV_OBJ_FLAG_HIDDEN);
        if (right_brow_)   lv_obj_add_flag(right_brow_,   LV_OBJ_FLAG_HIDDEN);
        if (music_icon_)   lv_obj_add_flag(music_icon_,   LV_OBJ_FLAG_HIDDEN);
        if (overlay_icon_) lv_obj_add_flag(overlay_icon_, LV_OBJ_FLAG_HIDDEN);
        overlay_active_ = true;
    }

    if (duration_ms > 0) {
        lv_timer_t* t = lv_timer_create([](lv_timer_t* timer) {
            OledDisplay* self = static_cast<OledDisplay*>(lv_timer_get_user_data(timer));
            if (self && self->wifi_warning_label_) {
                lv_obj_add_flag(self->wifi_warning_label_, LV_OBJ_FLAG_HIDDEN);
                self->overlay_active_ = false;
                if (!self->boot_anim_active_) {
                    lv_obj_remove_flag(self->left_eye_,  LV_OBJ_FLAG_HIDDEN);
                    lv_obj_remove_flag(self->right_eye_, LV_OBJ_FLAG_HIDDEN);
                    self->UpdateEyes();
                }
            }
            lv_timer_del(timer);
        }, duration_ms, this);
        (void)t;
    }
}
