#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "system_reset.h"
#include "wifi_board.h"

#include <driver/spi_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

#include "esp_lcd_gc9a01.h"
#include <esp_heap_caps.h>

#define TAG "CompactWifiBoard"

// ============================================================
//  SEVGİ TEPKİSİ — uzun basış (1.2s+)
//  SEVGI TEPKİSİ — 3 saniye basili tutma
//  Kalp ATIYOR gibi: hizla aç-kapat, 3 saniye sonra kapan
// ============================================================
static void LoveReactionTask(void* /*arg*/) {
    auto display = Board::GetInstance().GetDisplay();
    // 3 saniye boyunca kalp ikonu sabit gozukur
    if (display)
        display->ShowOverlayIcon("heart", 3000);
    vTaskDelay(pdMS_TO_TICKS(3000));

    if (display) {
        display->SetOverlayIcon("heart", false);
        display->SetEmotion("happy");
        display->SetChatMessage("assistant", "Ay gidiklaniyorum! Dur dur, hehe!");
    }

    vTaskDelay(pdMS_TO_TICKS(3000));
    if (display)
        display->SetEmotion("neutral");

    vTaskDelete(nullptr);
}

// ============================================================
//  ACI TEPKİSİ — hizli tik-birak (<500ms)
//  Uzgun yuz + exclamation sesi; sonra neutral'a don
// ============================================================
static void PainReactionTask(void* /*arg*/) {
    auto display = Board::GetInstance().GetDisplay();

    if (display) {
        display->SetEmotion("sad");
        display->SetChatMessage("assistant", "Ah! Acidi be! Neden yaptin bunu?!");
    }

    // Ses yok — sadece ikon, 4 saniye bekle

    vTaskDelay(pdMS_TO_TICKS(4000));
    if (display)
        display->SetEmotion("neutral");

    vTaskDelete(nullptr);
}

// ============================================================
//  Board sinifi
// ============================================================
class CompactWifiBoard : public WifiBoard {
private:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    LcdDisplay* display_ = nullptr;

    // TTP223 → BOOT_BUTTON_GPIO (active_high, long_press=1200ms)
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    // ----------------------------------------------------------
    void InitializeDisplaySpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeGc9a01Display() {
        // ── Panel IO (SPI) ──────────────────────────────────────────
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.dc_gpio_num       = DISPLAY_DC_PIN;
        io_config.cs_gpio_num       = DISPLAY_CS_PIN;
        io_config.spi_mode          = DISPLAY_SPI_MODE;
        io_config.pclk_hz           = 10 * 1000 * 1000;  // 10 MHz (güvenli başlatma)
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits      = 8;
        io_config.lcd_param_bits    = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &panel_io_));

        // ── GC9A01 panel sürücüsü ────────────────────────────────────
        ESP_LOGI(TAG, "Install GC9A01 panel driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order  = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io_, &panel_config, &panel_));

        // Reset: önce low sonra high, yeterli bekleme
        esp_lcd_panel_reset(panel_);
        vTaskDelay(pdMS_TO_TICKS(120));  // reset sonrası bekleme

        esp_lcd_panel_init(panel_);
        vTaskDelay(pdMS_TO_TICKS(50));

        esp_lcd_panel_invert_color(panel_, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel_,     DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel_,      DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel_, true);
        vTaskDelay(pdMS_TO_TICKS(20));

        // ============================================================
        //  DONANIM TESTİ — Ekranı KIRMIZI doldur (RGB565: 0xF800)
        //  Ekran kirmizi yanarsa SPI + GC9A01 CALISIYOR demektir!
        // ============================================================
        {
            constexpr int W = DISPLAY_WIDTH;
            constexpr int H = DISPLAY_HEIGHT;
            constexpr int CHUNK = 40;   // 40 satır = 19200 byte (DMA uyumlu)
            uint16_t* buf = (uint16_t*)heap_caps_malloc(
                W * CHUNK * sizeof(uint16_t), MALLOC_CAP_DMA);
            if (buf) {
                // KIRMIZI: 2 sn
                for (int i = 0; i < W * CHUNK; i++) buf[i] = 0x00F8; // RGB565 byte-swapped kırmızı
                for (int y = 0; y < H; y += CHUNK) {
                    int y2 = (y + CHUNK - 1 < H) ? y + CHUNK - 1 : H - 1;
                    esp_lcd_panel_draw_bitmap(panel_, 0, y, W, y2 + 1, buf);
                }
                vTaskDelay(pdMS_TO_TICKS(2000));

                // YEŞİL: 2 sn — renk değişirse ekran çalışıyor!
                for (int i = 0; i < W * CHUNK; i++) buf[i] = 0xE007; // RGB565 byte-swapped yeşil
                for (int y = 0; y < H; y += CHUNK) {
                    int y2 = (y + CHUNK - 1 < H) ? y + CHUNK - 1 : H - 1;
                    esp_lcd_panel_draw_bitmap(panel_, 0, y, W, y2 + 1, buf);
                }
                vTaskDelay(pdMS_TO_TICKS(2000));

                heap_caps_free(buf);
                ESP_LOGI(TAG, "GC9A01 HW TEST TAMAM — normal moda geciliyor");
            } else {
                ESP_LOGE(TAG, "GC9A01 TEST: DMA bellek alinamadi!");
            }
        }
        // ============================================================

        display_ = new SpiLcdDisplay(panel_io_, panel_,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
                                     DISPLAY_SWAP_XY);
    }

    // ----------------------------------------------------------
    //  BUTON AYARLARI
    // ----------------------------------------------------------
    void InitializeButtons() {
        // ======================================================
        // BOOT BUTTON = GPIO13 = TTP223 dokunmatik sensör
        // Üç ayrı davranis:
        // 3 saniye+ tutunca → SEVGI tepkisi
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "TTP223: 3sn uzun basin -> SEVGI");
            xTaskCreate(LoveReactionTask, "love_task", 4096, nullptr, 2, nullptr);
        });

        // 3 kez hizli tiklama → ACI tepkisi
        boot_button_.OnMultipleClick(
            [this]() {
                ESP_LOGI(TAG, "TTP223: 3x tik -> ACI");
                xTaskCreate(PainReactionTask, "pain_task", 4096, nullptr, 2, nullptr);
            },
            3);

        // Tek tiklama → radyo modunda ise kapat, yoksa idle'dan sohbet aç
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }

            // ★ Radyo modu aktifse: 1 basilisla kapat ★
            app.ToggleChatState();  // HandleToggleChatEvent içinde radyo kontrolü var
        });

        // ======================================================
        // TOUCH BUTTON (GPIO47) — PTT dinleme
        // ======================================================
        touch_button_.OnPressDown([this]() { Application::GetInstance().StartListening(); });
        touch_button_.OnPressUp([this]() { Application::GetInstance().StopListening(); });

        // ======================================================
        // SES BUTONLARI
        // ======================================================
        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100)
                volume = 100;
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0)
                volume = 0;
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    void InitializeTools() { static LampController lamp(LAMP_GPIO); }

public:
    CompactWifiBoard()
        :  // TTP223: active_high=TRUE, long_press=3000ms (3 saniye)
          boot_button_(BOOT_BUTTON_GPIO, /*active_high=*/true, /*long_press_ms=*/3000,
                       /*short_press_ms=*/0),
          touch_button_(TOUCH_BUTTON_GPIO),
          volume_up_button_(VOLUME_UP_BUTTON_GPIO),
          volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeDisplaySpi();
        InitializeGc9a01Display();
        InitializeButtons();
        InitializeTools();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                               AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
                                               AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
                                               AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                              AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                                              AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }
};

DECLARE_BOARD(CompactWifiBoard);