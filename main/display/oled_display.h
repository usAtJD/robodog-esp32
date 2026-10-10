#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>


class OledDisplay : public LvglDisplay {
private:
    enum class EyeEmotion {
        Neutral,
        Happy,
        Sad,
        Angry,
        Sleepy,
        Surprised,
    };

    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* content_left_ = nullptr;
    lv_obj_t* content_right_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t *emotion_label_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    lv_obj_t* eye_area_ = nullptr;
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* left_brow_ = nullptr;
    lv_obj_t* right_brow_ = nullptr;
    lv_obj_t* boot_tube_ = nullptr;
    lv_obj_t* boot_fill_ = nullptr;
    lv_obj_t* boot_glow_ = nullptr;
    lv_obj_t* music_icon_ = nullptr;
    lv_obj_t* overlay_icon_ = nullptr;
    bool overlay_active_ = false;
    lv_obj_t* radio_icon_ = nullptr;
    bool radio_mode_active_ = false;
    lv_obj_t* wifi_warning_label_ = nullptr;
    lv_timer_t* eye_anim_timer_ = nullptr;
    EyeEmotion eye_emotion_ = EyeEmotion::Neutral;
    EyeEmotion requested_emotion_ = EyeEmotion::Neutral;
    int blink_frame_ = 0;
    int next_blink_tick_ = 90;
    int gaze_tick_ = 0;
    int gaze_dir_ = 0;
    int boot_anim_ticks_ = 0;
    bool device_ready_ = false;
    bool boot_anim_active_ = false;
    bool show_music_only_ = false;
    bool music_mode_pending_ = false;
    bool standby_sleep_mode_ = false;
    bool show_wifi_warning_ = false;
    bool wifi_connected_ = false;
    int wakeup_anim_ticks_ = 0;
    bool eyes_enabled_ = false;

    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    static void EyeAnimTimerCb(lv_timer_t* timer);
    static EyeEmotion ParseEyeEmotion(const char* emotion);
    void CreateEyes();
    void UpdateEyes();
    void ApplyEyeEmotionStyle(int eye_w, int eye_h, int eye_y_offset, int gaze_amp);
    void SetWifiWarningVisible(bool visible);

    void SetupUI_128x64();
    void SetupUI_128x32();

public:
    OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height, bool mirror_x, bool mirror_y, bool swap_xy = true);
    ~OledDisplay();

    virtual void SetupUI() override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetOverlayIcon(const char* icon, bool visible) override;
    virtual void ShowOverlayIcon(const char* icon, int duration_ms = 1500) override;
    virtual void ShowMessageOverlay(const char* message, int duration_ms = 8000) override;
    virtual void SetStatus(const char* status) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowRadioIcon() override;
    virtual void HideRadioIcon() override;
};

#endif // OLED_DISPLAY_H
