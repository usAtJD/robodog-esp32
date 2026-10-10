#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "settings.h"
#include "config.h"
#include "power_save_timer.h"
#include "press_to_talk_mcp_tool.h"

#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#define TAG "XminiC3Board"

// Display type selection - set to true for 7-pin SPI, false for 4-pin I2C
#define USE_SPI_OLED_DISPLAY true

// SPI OLED pin definitions for 7-pin display (only used when USE_SPI_OLED_DISPLAY is true)
#define OLED_SPI_HOST    SPI2_HOST
#define OLED_MOSI_PIN    23    // SDA pin on 7-pin OLED  
#define OLED_SCK_PIN     18    // SCK pin on 7-pin OLED
#define OLED_CS_PIN      5     // CS pin on 7-pin OLED
#define OLED_DC_PIN      4     // DC pin on 7-pin OLED
#define OLED_RES_PIN     2     // RES pin on 7-pin OLED

class XminiC3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    spi_device_handle_t oled_spi_handle_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    PowerSaveTimer* power_save_timer_ = nullptr;
    PressToTalkMcpTool* press_to_talk_tool_ = nullptr;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(160, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral for audio codec
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));

        // Print I2C bus info
        if (i2c_master_probe(codec_i2c_bus_, 0x18, 1000) != ESP_OK) {
            while (true) {
                ESP_LOGE(TAG, "Failed to probe I2C bus, please check if you have installed the correct firmware");
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
        }
    }

#if USE_SPI_OLED_DISPLAY
    void InitializeSsd1306SpiDisplay() {
        ESP_LOGI(TAG, "Initializing 7-pin SPI OLED display");
        
        // Configure SPI bus
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = OLED_MOSI_PIN,
            .miso_io_num = -1,  // No MISO for OLED
            .sclk_io_num = OLED_SCK_PIN,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT / 8 + 8,
        };
        
        ESP_ERROR_CHECK(spi_bus_initialize(OLED_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));
        
        // Configure SPI device
        spi_device_interface_config_t dev_cfg = {
            .command_bits = 8,
            .address_bits = 0,
            .dummy_bits = 0,
            .mode = 0,  // CPOL=0, CPHA=0
            .duty_cycle_pos = 128,
            .cs_ena_pretrans = 1,
            .cs_ena_posttrans = 1,
            .clock_speed_hz = 20 * 1000 * 1000,  // 20MHz
            .input_delay_ns = 0,
            .spics_io_num = OLED_CS_PIN,
            .flags = SPI_DEVICE_NO_DUMMY,
        };
        
        ESP_ERROR_CHECK(spi_bus_add_device(OLED_SPI_HOST, &dev_cfg, &oled_spi_handle_));
        
        // Configure LCD panel IO for SPI
        esp_lcd_panel_io_spi_config_t io_config = {
            .dc_gpio_num = OLED_DC_PIN,
            .cs_gpio_num = OLED_CS_PIN,
            .pclk_hz = 20 * 1000 * 1000,  // 20MHz
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .spi_mode = 0,
            .trans_queue_depth = 10,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .octal_mode = 0,
                .lsb_first = 0,
                .cs_high_active = 0,
            },
        };
        
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(oled_spi_handle_, &io_config, &panel_io_));
        
        // Configure SSD1306 panel
        ESP_LOGI(TAG, "Install SSD1306 SPI driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = OLED_RES_PIN;
        panel_config.bits_per_pixel = 1;
        
        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;
        
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_LOGI(TAG, "SSD1306 SPI driver installed");
        
        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize SPI display");
            display_ = new NoDisplay();
            return;
        }
        
        // Set the display to on
        ESP_LOGI(TAG, "Turning SPI display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));
        
        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        ESP_LOGI(TAG, "7-pin SPI OLED display initialized successfully");
    }
#else
    void InitializeSsd1306I2cDisplay() {
        ESP_LOGI(TAG, "Initializing 4-pin I2C OLED display");
        
        // SSD1306 I2C config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(codec_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 I2C driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_LOGI(TAG, "SSD1306 I2C driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize I2C display");
            display_ = new NoDisplay();
            return;
        }

        // Set the display to on
        ESP_LOGI(TAG, "Turning I2C display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        ESP_LOGI(TAG, "4-pin I2C OLED display initialized successfully");
    }
#endif

    void InitializeDisplay() {
#if USE_SPI_OLED_DISPLAY
        InitializeSsd1306SpiDisplay();
#else
        InitializeSsd1306I2cDisplay();
#endif
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (!press_to_talk_tool_ || !press_to_talk_tool_->IsPressToTalkEnabled()) {
                app.ToggleChatState();
            }
        });
        boot_button_.OnPressDown([this]() {
            if (power_save_timer_) {
                power_save_timer_->WakeUp();
            }
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StartListening();
            }
        });
        boot_button_.OnPressUp([this]() {
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StopListening();
            }
        });
    }

    void InitializeTools() {
        press_to_talk_tool_ = new PressToTalkMcpTool();
        press_to_talk_tool_->Initialize();
    }

public:
    XminiC3Board() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeCodecI2c();
        InitializeDisplay();
        InitializeButtons();
        InitializePowerSaveTimer();
        InitializeTools();

        // 避免使用错误的固件，把 EFUSE 操作放在最后
        // 把 ESP32C3 的 VDD SPI 引脚作为普通 GPIO 口使用
        esp_efuse_write_field_bit(ESP_EFUSE_VDD_SPI_AS_GPIO);
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(XminiC3Board);
