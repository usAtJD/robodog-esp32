#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "assets/lang_config.h"
#include "power_manager.h"
#include "power_save_timer.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_io_expander_tca9554.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include "esp32_camera.h"

#define TAG "esp32s3_korvo2_v3_spi_oled"

// ESP32-S3 SPI OLED pin definitions for 7-pin display
#define OLED_SPI_HOST    SPI3_HOST
#define OLED_MOSI_PIN    11    // MOSI pin on ESP32-S3
#define OLED_SCK_PIN     12    // SCK pin on ESP32-S3  
#define OLED_CS_PIN      10    // CS pin on ESP32-S3
#define OLED_DC_PIN      13    // DC pin on ESP32-S3
#define OLED_RES_PIN     14    // RES pin on ESP32-S3

/* ADC Buttons */
typedef enum {
    BSP_ADC_BUTTON_REC,
    BSP_ADC_BUTTON_VOL_MUTE,
    BSP_ADC_BUTTON_PLAY,
    BSP_ADC_BUTTON_SET,
    BSP_ADC_BUTTON_VOL_DOWN,
    BSP_ADC_BUTTON_VOL_UP,
    BSP_ADC_BUTTON_NUM
} bsp_adc_button_t;

class Esp32S3Korvo2V3SPIBoard : public WifiBoard {
private:
    Button boot_button_;
    Button* adc_button_[BSP_ADC_BUTTON_NUM];
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    adc_oneshot_unit_handle_t bsp_adc_handle = NULL;
#endif
    i2c_master_bus_handle_t i2c_bus_;
    OledDisplay* display_;
    esp_io_expander_handle_t io_expander_ = NULL;
    Esp32Camera* camera_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    spi_device_handle_t oled_spi_handle_ = nullptr;

    void InitializePowerManager() {
        power_manager_ = new PowerManager(GPIO_NUM_NC, &bsp_adc_handle);
    }
    
    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
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
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeTca9554() {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander_);
        if(ret != ESP_OK) {
            ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_000, &io_expander_);
            if(ret != ESP_OK) {
                ESP_LOGE(TAG, "TCA9554 create returned error");  
                return;
            }
        }
        // Configure IO0-IO3 as output mode
        ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander_, 
            IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | 
            IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_3, 
            IO_EXPANDER_OUTPUT));

        // Reset LCD and TouchPad (not needed for OLED but keeping for compatibility)
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_,
            IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2, 1));
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_,
            IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2, 0));
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_,
            IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2, 1));
    }

    void InitializeSsd1306SpiDisplay() {
        ESP_LOGI(TAG, "Initializing 7-pin SPI OLED display for ESP32-S3");
        
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
        esp_lcd_panel_io_handle_t panel_io = nullptr;
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
        
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(oled_spi_handle_, &io_config, &panel_io));
        
        // Configure SSD1306 panel
        ESP_LOGI(TAG, "Install SSD1306 SPI driver");
        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = OLED_RES_PIN;
        panel_config.bits_per_pixel = 1;
        
        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;
        
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io, &panel_config, &panel));
        ESP_LOGI(TAG, "SSD1306 SPI driver installed");
        
        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        if (esp_lcd_panel_init(panel) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize SPI display");
            display_ = new NoDisplay();
            return;
        }
        
        // Set the display to on
        ESP_LOGI(TAG, "Turning SPI display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
        
        display_ = new OledDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        ESP_LOGI(TAG, "7-pin SPI OLED display initialized successfully");
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    Esp32S3Korvo2V3SPIBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeTca9554();
        InitializeSsd1306SpiDisplay();
        InitializeButtons();
        InitializePowerManager();
        InitializePowerSaveTimer();
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(Esp32S3Korvo2V3SPIBoard);
