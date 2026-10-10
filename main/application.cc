#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#include "mqtt_protocol.h"
#include "settings.h"
#include "system_info.h"
#include "websocket_protocol.h"

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_system.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <font_awesome.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

#define TAG "Application"

// ============================================================
//  Arduino (Robodog) UART HABERLEŞMESİ
//  ESP32-S3 TX GPIO17  -> Arduino RX pin2
//  Baud: 9600, newline-terminated ASCII (e.g. "happy\n", "dance\n")
// ============================================================
#define ROBODOG_UART_NUM UART_NUM_1
#define ROBODOG_TX_PIN 17
#define ROBODOG_UART_BAUD 9600

static void InitRobodogUart() {
    uart_config_t cfg = {.baud_rate = ROBODOG_UART_BAUD,
                         .data_bits = UART_DATA_8_BITS,
                         .parity = UART_PARITY_DISABLE,
                         .stop_bits = UART_STOP_BITS_1,
                         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                         .rx_flow_ctrl_thresh = 0,
                         .source_clk = UART_SCLK_DEFAULT};

    if (!uart_is_driver_installed(ROBODOG_UART_NUM)) {
        ESP_ERROR_CHECK(uart_driver_install(ROBODOG_UART_NUM, 1024, 0, 0, nullptr, 0));
        ESP_ERROR_CHECK(uart_param_config(ROBODOG_UART_NUM, &cfg));
        // TX only; RX pin unchanged.
        ESP_ERROR_CHECK(uart_set_pin(ROBODOG_UART_NUM, ROBODOG_TX_PIN, UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    }
    ESP_LOGI(TAG, "Robodog UART baslatildi (TX:%d, %d baud)", ROBODOG_TX_PIN, ROBODOG_UART_BAUD);
}

static void RobodogSendLine(const std::string& line) {
    if (line.empty())
        return;
    uart_write_bytes(ROBODOG_UART_NUM, line.data(), (int)line.size());
    uart_write_bytes(ROBODOG_UART_NUM, "\n", 1);
}

// ------------------------------------------------------------
// "Muzik caliyor gibi yap" (yerel ses efektleri ile ritim)
// ------------------------------------------------------------
static std::atomic<bool> g_music_stop{false};
static std::atomic<bool> g_dance_mode{false};  // Dans modu
static TaskHandle_t      g_music_task_handle = nullptr;

// Dans durdurma zamanlayicisi:
// Kanal kapaninca hemen durdurmak yerine 8sn beklenir.
// Eger 8sn icinde yeni kanal acilirsa (muzik seansı) timer iptal edilir.
static TimerHandle_t g_dance_stop_timer = nullptr;

static void DanceStopTimerCb(TimerHandle_t) {
    if (g_dance_mode.load()) {
        g_dance_mode.store(false);
        // UART gondermek icin Application schedule'a aktar
        Application::GetInstance().Schedule([]() {
            ESP_LOGI("DanceTimer", "8sn kanal acilmadi -> dance_stop");
            RobodogSendLine("dance_stop");
        });
    }
}

struct MusicTaskArgs {
    bool also_dance = false;
    int duration_ms = 20000;  // varsayilan 20sn
};

static void MusicTask(void* arg) {
    std::unique_ptr<MusicTaskArgs> args(static_cast<MusicTaskArgs*>(arg));
    auto display = Board::GetInstance().GetDisplay();
    auto& app = Application::GetInstance();

    g_music_stop.store(false);

    if (display) {
        const char* music = font_awesome_get_utf8("music");
        display->ShowNotification(music != nullptr ? music : "MUZIK", 2500);
    }

    if (args && args->also_dance) {
        RobodogSendLine("dance");
        if (display) {
            display->SetChatMessage("assistant", "Tamam hadi dans edelim.");
        }
    }

    const int64_t start = esp_timer_get_time() / 1000;
    while (!g_music_stop.load()) {
        const int64_t now = esp_timer_get_time() / 1000;
        if (args && args->duration_ms > 0 && (now - start) > args->duration_ms)
            break;
        // Rahatsiz edici kisa bipler yerine daha uzun/tek parca sesi kullan.
        app.PlaySound(Lang::Sounds::OGG_WELCOME);
        app.GetAudioService().WaitForPlaybackQueueEmpty();
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    g_music_task_handle = nullptr;
    vTaskDelete(nullptr);
}

static void StartMusicMode(bool also_dance, int duration_ms = 20000) {
    // varsa eskisini durdur
    if (g_music_task_handle != nullptr) {
        g_music_stop.store(true);
        // eski task kendi kendini bitirir
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    auto* args = new MusicTaskArgs();
    args->also_dance = also_dance;
    args->duration_ms = duration_ms;
    xTaskCreate(MusicTask, "music_mode", 4096, args, 2, &g_music_task_handle);
}

// Dansi ana akistan ayir (WDT riskini azaltir)
static void DanceTask(void* arg) {
    auto display = Board::GetInstance().GetDisplay();

    // OLED'de overlay ikon gözleri kapattığı için dans ikonunu notification olarak göster.
    // Böylece yüzün üstüne gelmez ve kendiliğinden kaybolur.
    if (display) {
        const char* music = font_awesome_get_utf8("music");
        display->ShowNotification(music != nullptr ? music : "DANS MODU", 2500);
    }

    // Arduino'ya dans komutu
    RobodogSendLine("dance");

    // Ekranda kisa mesaj
    if (display) {
        display->SetChatMessage("assistant", "Tamam hadi dans edelim.");
    }

    // "Rastgele sarki" yerine mevcut ses kutuphanesinden rastgele bir ses cal
    // (repo icinde muzik arsivi yok, ama bu en stabil secenek)
    const std::string_view sounds[] = {
        Lang::Sounds::OGG_POPUP,
        Lang::Sounds::OGG_SUCCESS,
        Lang::Sounds::OGG_VIBRATION,
        Lang::Sounds::OGG_WELCOME,
    };
    uint32_t r = esp_random() % (sizeof(sounds) / sizeof(sounds[0]));
    Application::GetInstance().PlaySound(sounds[r]);

    vTaskDelete(nullptr);
}

static std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) -> unsigned char {
        if (c >= 'A' && c <= 'Z')
            return (unsigned char)(c - 'A' + 'a');
        return c;
    });
    return s;
}

static bool IsLiveViewIntent(const std::string& text_lower) {
    const bool has_live_phrase = (text_lower.find("canlı izleme") != std::string::npos ||
                                  text_lower.find("canli izleme") != std::string::npos ||
                                  text_lower.find("canlı yayın linki") != std::string::npos ||
                                  text_lower.find("canli yayin linki") != std::string::npos ||
                                  text_lower.find("canlı mod") != std::string::npos ||
                                  text_lower.find("canli mod") != std::string::npos ||
                                  text_lower.find("stream") != std::string::npos);

    // Daha toleransli eslesme: "izleme" + "link" birlikte geciyorsa da tetikle
    const bool has_link_phrase = ((text_lower.find("izleme") != std::string::npos &&
                                   text_lower.find("link") != std::string::npos) ||
                                  text_lower.find("canlı izleme linki") != std::string::npos ||
                                  text_lower.find("canli izleme linki") != std::string::npos);

    return has_live_phrase || has_link_phrase;
}

static bool IsDanceIntent(const std::string& text_lower) {
    return (text_lower.find("dans") != std::string::npos);
}

// "dans dur" / "dans bitir" / "duraksın" → dance_stop
static bool IsDanceStopIntent(const std::string& t) {
    const bool has_dans = (t.find("dans") != std::string::npos);
    const bool has_stop =
        (t.find("dur") != std::string::npos || t.find("bitir") != std::string::npos ||
         t.find("kapat") != std::string::npos || t.find("durdur") != std::string::npos);
    return has_dans && has_stop;
}


static bool IsMusicOnIntent(const std::string& text_lower) {
    const bool has_music = (text_lower.find("müzik") != std::string::npos ||
                            text_lower.find("muzik") != std::string::npos);
    const bool has_on =
        (text_lower.find("aç") != std::string::npos || text_lower.find("ac") != std::string::npos);
    return has_music && has_on;
}

static bool IsMusicOffIntent(const std::string& text_lower) {
    const bool has_music = (text_lower.find("müzik") != std::string::npos ||
                            text_lower.find("muzik") != std::string::npos);
    const bool has_off = (text_lower.find("kapat") != std::string::npos ||
                          text_lower.find("durdur") != std::string::npos);
    return has_music && has_off;
}

// "radyo modu" komutu
static bool IsRadioModeIntent(const std::string& t) {
    return (t.find("radyo") != std::string::npos ||
            t.find("radio") != std::string::npos);
}

static void ShowLiveViewLinkUI() {
    auto display = Board::GetInstance().GetDisplay();
    if (!display)
        return;
    display->ShowMessageOverlay("http://192.168.1.157/stream", 8000);
}

// ============================================================
//  Application sınıfı implementasyonu
// ============================================================

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Robodog (Arduino) UART baslat
    InitRobodogUart();

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();
    mcp_server.AddSearchTools();
    mcp_server.AddMapsTools();
    mcp_server.AddPrayerTools();
    mcp_server.AddCamLinkTools();  // ESP32-CAM: fotoğraf çekme + güvenlik modu

    // Network event callback
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                break;
            case NetworkEvent::WifiConfigModeExit:
                break;
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation",
                      Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    board.StartNetwork();
    display->UpdateStatusBar(true);
}

void Application::Run() {
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark",
                  Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            if (local_cmd_suppress_ticks_ > 0) {
                local_cmd_suppress_ticks_--;
            }
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() { audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS); });

    // ── Robodog: Aktivasyon bitti, periyodik OTA kontrolünü başlat ──
    StartPeriodicOtaCheck();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Periyodik OTA Kontrolü (Robodog OTA sistemi)
//  Her 5 dakikada bir sunucuyu kontrol eder.
//  Web panelinden "Güncelle" tıklanırsa ESP internete girdiğinde güncellenir.
// ─────────────────────────────────────────────────────────────────────────────
#define ROBODOG_OTA_CHECK_INTERVAL_SEC  300   // 5 dakika

void Application::StartPeriodicOtaCheck() {
    if (ota_periodic_check_enabled_) return;  // Zaten çalışıyor

    ota_periodic_check_enabled_ = true;
    xTaskCreate(
        [](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->PeriodicOtaCheckTask();
            app->ota_check_task_handle_ = nullptr;
            vTaskDelete(NULL);
        },
        "ota_periodic", 4096 * 2, this, 1, &ota_check_task_handle_);

    ESP_LOGI(TAG, "[RobodogOTA] Periyodik kontrol başlatıldı (%ds)", ROBODOG_OTA_CHECK_INTERVAL_SEC);
}

void Application::PeriodicOtaCheckTask() {
    // İlk kontrol için 30 saniye bekle (sistem oturulsun)
    vTaskDelay(pdMS_TO_TICKS(30000));

    while (ota_periodic_check_enabled_) {
        // Sadece idle modda kontrol et (konuşurken kesme)
        auto state = GetDeviceState();
        if (state == kDeviceStateIdle || state == kDeviceStateConnecting) {
            ESP_LOGI(TAG, "[RobodogOTA] Sunucu kontrol ediliyor...");
            auto check_ota = std::make_unique<Ota>();
            esp_err_t err = check_ota->CheckVersion();
            if (err == ESP_OK && check_ota->HasNewVersion()) {
                ESP_LOGI(TAG, "[RobodogOTA] YENİ SÜRÜM: %s → indiriliyor",
                         check_ota->GetFirmwareVersion().c_str());
                Schedule([this, url = check_ota->GetFirmwareUrl(),
                          ver = check_ota->GetFirmwareVersion()]() {
                    UpgradeFirmware(url, ver);
                });
                break;  // Güncelleme başladı, task bitsin
            } else if (err == ESP_OK) {
                ESP_LOGI(TAG, "[RobodogOTA] Güncel (%s)", check_ota->GetCurrentVersion().c_str());
            } else {
                ESP_LOGW(TAG, "[RobodogOTA] Kontrol başarısız (kod=%d), sonra tekrar denenecek", err);
            }
        } else {
            ESP_LOGD(TAG, "[RobodogOTA] Cihaz meşgul (state=%d), kontrol atlandı", (int)state);
        }

        // Bir sonraki kontrole kadar bekle
        vTaskDelay(pdMS_TO_TICKS(ROBODOG_OTA_CHECK_INTERVAL_SEC * 1000));
    }
    ESP_LOGI(TAG, "[RobodogOTA] Periyodik kontrol durdu");
}

void Application::ActivationTask() {
    ota_ = std::make_unique<Ota>();
    CheckAssetsVersion();
    CheckNewVersion();
    InitializeProtocol();
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    if (assets_version_checked_)
        return;
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down",
              Lang::Sounds::OGG_UPGRADE);

        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success =
            assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10;

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err,
                     ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay,
                     error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay,
                     retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle)
                    break;
            }
            retry_delay *= 2;
            continue;
        }
        retry_count = 0;
        retry_delay = 10;

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion()))
                return;
        }

        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge())
            break;

        display->SetStatus(Lang::Strings::ACTIVATION);
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK)
                break;
            else if (err == ESP_ERR_TIMEOUT)
                vTaskDelay(pdMS_TO_TICKS(3000));
            else
                vTaskDelay(pdMS_TO_TICKS(10000));
            if (GetDeviceState() == kDeviceStateIdle)
                break;
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() { DismissAlert(); });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });

    // Dans durdurma timerını bir kez olustur
    if (!g_dance_stop_timer) {
        g_dance_stop_timer = xTimerCreate(
            "dance_stop",
            pdMS_TO_TICKS(8000),  // 8 saniye
            pdFALSE,              // one-shot
            nullptr,
            DanceStopTimerCb
        );
    }

    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d",
                     protocol_->server_sample_rate(), codec->output_sample_rate());
        }
        // Dans modu aktifse: timer'i iptal et ve dance_start yeniden gonder
        if (g_dance_mode.load()) {
            if (g_dance_stop_timer) xTimerStop(g_dance_stop_timer, 0);
            Schedule([this]() {
                ESP_LOGI(TAG, "Yeni kanal: dance_start yeniden gonderiliyor");
                RobodogSendLine("dance_start");
            });
        }
    });

    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
        // Dans timeri TTS stop/start uzerinden yonetiliyor; kanal kapanince dokunmuyoruz
    });

    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                // Dans modu aktifse: TTS basladı = muzik/ses geliyor → motorlari baslat
                if (g_dance_mode.load() && g_dance_stop_timer)
                    xTimerStop(g_dance_stop_timer, 0);  // bekleyen stop iptal
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                    if (g_dance_mode.load()) {
                        // Her TTS cumlesi motorlari re-confirm eder (arduino reset vs)
                        RobodogSendLine("dance_start");
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                // Dans modu aktifse: TTS bitti, 8sn bekle
                // 8sn icinde yeni TTS start gelirse timer iptal olur (muzik devam)
                // 8sn gecerse DanceStopTimerCb -> dance_stop
                if (g_dance_mode.load() && g_dance_stop_timer) {
                    xTimerStart(g_dance_stop_timer, 0);
                }
                Schedule([this]() {
                    // Dans modu TTS stop'ta kesilmez — muzik bitmeden aralar olur
                    // dance_stop yalnizca: timer suresi dolunca
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    std::string sentence = text->valuestring;
                    ESP_LOGI(TAG, "<< %s", sentence.c_str());

                    // Dans modu: her sentence_start muzik satiri geliyor demek
                    // → timer iptal et, dance_start yeniden gonder (motorlari canli tut)
                    if (g_dance_mode.load() && g_dance_stop_timer) {
                        xTimerStop(g_dance_stop_timer, 0);
                        RobodogSendLine("dance_start");
                    }

                    // ====================================================
                    //  AKILLI KOMUT TESPİTİ: AI'nın cevap metnine bakılır
                    //  "fotoğraf" içeriyorsa ESP32-CAM'e 's' tetikle
                    // ====================================================

                    // --- FOTOĞRAF ÇEK ---
                    bool is_capture = (sentence.find("çekiyorum") != std::string::npos ||
                                       sentence.find("poz ver") != std::string::npos ||
                                       sentence.find("fotoğraf") != std::string::npos);
                    const std::string sentence_l = ToLowerAscii(sentence);
                    bool is_live_view = IsLiveViewIntent(sentence_l);

                    Schedule([this, msg = sentence, is_capture, is_live_view]() {
                        auto display = Board::GetInstance().GetDisplay();

                        // Yerel komutlardan sonra server metnini bastirma
                        if (local_cmd_suppress_ticks_ > 0) {
                            if (is_live_view) {
                                ShowLiveViewLinkUI();
                            }
                            return;
                        }
                        display->SetChatMessage("assistant", msg.c_str());

                        if (is_capture) {
                            if (display) {
                                display->ShowOverlayIcon("camera", 4000);
                            }
                            uart_write_bytes(UART_NUM_2, "s", 1);
                            ESP_LOGI(TAG, "ESP32-CAM tetiklendi: 's'");
                        }

                        if (is_live_view) {
                            ShowLiveViewLinkUI();
                        }
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);

                Schedule([this, display, message = std::string(text->valuestring)]() {
                    if (display) {
                        display->SetChatMessage("user", message.c_str());
                    }

                    // "mikrofona canlı izleme" komutu -> sadece linki kısa göster
                    const std::string msg_l = ToLowerAscii(message);
                    const bool is_live_view_cmd = IsLiveViewIntent(msg_l);
                    if (is_live_view_cmd) {
                        // Linki gosterdikten sonra birkac saniye server mesajlarini bastirma
                        local_cmd_suppress_ticks_ = 8;
                        ShowLiveViewLinkUI();
                        if (display) {
                            display->SetChatMessage(
                                "assistant", "Buyurun canlı yayın linki, ekrana bakabilirsiniz.");
                        }
                        Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
                    }

                    // "müzik aç/kapat" komutu
                    {
                        const bool is_music_on = IsMusicOnIntent(msg_l);
                        const bool is_music_off = IsMusicOffIntent(msg_l);

                        if (is_music_off) {
                            g_music_stop.store(true);
                            if (g_dance_mode.load()) {
                                g_dance_mode.store(false);
                                RobodogSendLine("dance_stop");
                            }
                        } else if (is_music_on) {
                            g_music_stop.store(true);
                            if (display) {
                                display->ShowOverlayIcon("music", 8000);
                            }
                            Application::GetInstance().PlaySound(Lang::Sounds::OGG_WELCOME);
                        }

                        // "dans dur" / "dans bitir" komutu
                        if (IsDanceStopIntent(msg_l)) {
                            if (g_dance_mode.load()) {
                                g_dance_mode.store(false);
                                RobodogSendLine("dance_stop");
                            }
                        }

                        // "dans" komutu
                        if (IsDanceIntent(msg_l) && !IsDanceStopIntent(msg_l)) {
                            g_music_stop.store(true);
                            if (display)
                                display->ShowOverlayIcon("music", 8000);
                            g_dance_mode.store(true);
                            RobodogSendLine("dance_start");
                            Application::GetInstance().PlaySound(Lang::Sounds::OGG_WELCOME);
                        }
                    }
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    if (display) {
                        display->SetEmotion(emotion_str.c_str());
                    }
                    if (g_dance_mode.load()) {
                        return;
                    }
                    RobodogSendLine(emotion_str);
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    Schedule([this]() { Reboot(); });
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring,
                      Lang::Sounds::OGG_VIBRATION);
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule(
                    [this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                        display->SetChatMessage("system", payload_str.c_str());
                    });
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{
        {digit_sound{'0', Lang::Sounds::OGG_0}, digit_sound{'1', Lang::Sounds::OGG_1},
         digit_sound{'2', Lang::Sounds::OGG_2}, digit_sound{'3', Lang::Sounds::OGG_3},
         digit_sound{'4', Lang::Sounds::OGG_4}, digit_sound{'5', Lang::Sounds::OGG_5},
         digit_sound{'6', Lang::Sounds::OGG_6}, digit_sound{'7', Lang::Sounds::OGG_7},
         digit_sound{'8', Lang::Sounds::OGG_8}, digit_sound{'9', Lang::Sounds::OGG_9}}};

    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() { xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT); }

void Application::StartListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING); }

void Application::StopListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING); }

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    if (GetDeviceState() != kDeviceStateConnecting)
        return;

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel())
            return;
    }
    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this]() { ContinueOpenAudioChannel(kListeningModeManualStop); });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_)
            protocol_->SendStopListening();
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_)
        return;

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
            return;
        }
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        while (audio_service_.PopPacketFromSendQueue())
            ;

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            audio_service_.EnableWakeWordDetection(true);
        } else {
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    if (GetDeviceState() != kDeviceStateConnecting)
        return;

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();
            display->SetEmotion("neutral");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            audio_service_.EnableWakeWordDetection(false);
#endif

            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_)
        protocol_->SendAbortSpeaking(reason);
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        ESP_LOGE(TAG, "Firmware upgrade failed");
        audio_service_.Start();
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark",
              Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_)
        return;

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
            return;
        }
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (protocol_)
                protocol_->CloseAudioChannel();
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle)
        return false;
    if (protocol_ && protocol_->IsAudioChannelOpened())
        return false;
    if (!audio_service_.IsIdle())
        return false;
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_)
            protocol_->SendMcpMessage(payload);
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
        }

        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) { audio_service_.PlaySound(sound); }

void Application::ResetProtocol() {
    Schedule([this]() {
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        protocol_.reset();
    });
}