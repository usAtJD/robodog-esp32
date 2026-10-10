#include "radio_player.h"
#include "audio_codec.h"
#include "board.h"

#include <esp_http_client.h>
#include <esp_log.h>
#include <decoder/impl/esp_mp3_dec.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

#define TAG "RadioPlayer"

// ============================================================
//  Türkiye Radyo İstasyonları
// ============================================================
const RadioPlayer::RadioStation RadioPlayer::kStations[] = {
    {1, "TRT Turku", "http://radyo.trt.net.tr/live/trtturku.mp3"},
    {2, "TRT FM", "http://radyo.trt.net.tr/live/trtfm.mp3"},
    {3, "TRT Nagme", "http://radyo.trt.net.tr/live/trtnagme.mp3"},
    {4, "TRT Radyo 1", "http://radyo.trt.net.tr/live/trtradyo1.mp3"},
    {5, "TRT Haber", "http://radyo.trt.net.tr/live/trthaber.mp3"},
    {6, "TRT Anadolu", "http://radyo.trt.net.tr/live/trtanadolu.mp3"},
    {7, "Power FM",
     "http://playerservices.streamtheworld.com/api/livestream-redirect/POWERFM_TR.mp3"},
    {8, "Joy FM", "http://playerservices.streamtheworld.com/api/livestream-redirect/JOYFM_TR.mp3"},
    {9, "Slow Turk",
     "http://playerservices.streamtheworld.com/api/livestream-redirect/SLOWTURK.mp3"},
    {10, "Metro FM",
     "http://playerservices.streamtheworld.com/api/livestream-redirect/METROFM_TR.mp3"},
    {11, "Kral FM", "http://playerservices.streamtheworld.com/api/livestream-redirect/KRALFM.mp3"},
    {12, "Kral Pop",
     "http://playerservices.streamtheworld.com/api/livestream-redirect/KRALPOP.mp3"},
    {13, "Radyo D",
     "http://playerservices.streamtheworld.com/api/livestream-redirect/RADYOD_TR.mp3"},
    {14, "NTV Radyo",
     "http://playerservices.streamtheworld.com/api/livestream-redirect/NTVRADYO.mp3"},
    // --- Kullanici istegi: Ozel FM 103.2 Istanbul ---
    {15, "Ozel FM 103.2", "http://85.95.248.111:8014/"},
};
const int RadioPlayer::kStationCount =
    (int)(sizeof(RadioPlayer::kStations) / sizeof(RadioPlayer::kStations[0]));
// kStationCount = 15 (otomatik hesaplanir)

// ============================================================
//  Yardımcı: Kanal adı
// ============================================================
const char* RadioPlayer::ChannelName(int channel) {
    for (int i = 0; i < kStationCount; i++) {
        if (kStations[i].number == channel)
            return kStations[i].name;
    }
    return "Bilinmeyen";
}

// ============================================================
//  Yardımcı: Kanal listesi metni
// ============================================================
std::string RadioPlayer::GetChannelListText() {
    std::string list = "Mevcut kanallar: ";
    for (int i = 0; i < kStationCount; i++) {
        if (i > 0)
            list += ", ";
        list += std::to_string(kStations[i].number);
        list += " ";
        list += kStations[i].name;
    }
    return list;
}

// ============================================================
//  Türkçe sayı → kanal numarası
// ============================================================
int RadioPlayer::ParseChannelNumber(const std::string& text) {
    // Önce rakam ara — 2 basamaklı önce dene (10-15)
    for (int i = 0; i < (int)text.size(); i++) {
        if (text[i] >= '1' && text[i] <= '9') {
            // İki basamaklı kontrol
            if (i + 1 < (int)text.size() && text[i + 1] >= '0' && text[i + 1] <= '9') {
                int two = (text[i] - '0') * 10 + (text[i + 1] - '0');
                if (two >= 10 && two <= kStationCount)
                    return two;
            }
            // Tek basamak
            int num = text[i] - '0';
            if (num >= 1 && num <= kStationCount)
                return num;
        }
    }

    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower((int)c); });

    // Türkçe kelimeler (uzundan kısaya, en spesifik önce)
    struct {
        const char* word;
        int num;
    } words[] = {
        {"on bes", 15},  {"on besh", 15},  // Özel FM 103.2
        {"on dort", 14}, {"on dört", 14}, {"on uc", 13}, {"on üç", 13}, {"on iki", 12},
        {"on bir", 11},  {"on", 10},      {"dokuz", 9},  {"sekiz", 8},  {"yedi", 7},
        {"alti", 6},     {"altı", 6},     {"bes", 5},    {"beş", 5},    {"dort", 4},
        {"dört", 4},     {"uc", 3},       {"üç", 3},     {"iki", 2},    {"bir", 1},
    };
    for (auto& w : words) {
        if (lower.find(w.word) != std::string::npos)
            return w.num;
    }

    // ★ Kanal adına göre eşleştir (TRT Haber, Kral FM, Power FM vb.) ★
    // Kanal adını küçük harfe çevirip metin içinde ara
    for (int i = 0; i < kStationCount; i++) {
        std::string name_lower = kStations[i].name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                       [](unsigned char c) { return (char)std::tolower((int)c); });
        if (lower.find(name_lower) != std::string::npos)
            return kStations[i].number;
    }

    return 0;
}

// ============================================================
//  Start / Stop
// ============================================================
void RadioPlayer::Start(int channel) {
    if (channel < 1 || channel > kStationCount) {
        ESP_LOGW(TAG, "Gecersiz kanal: %d", channel);
        return;
    }

    // Varsa önce durdur
    Stop();

    current_channel_ = channel;
    stop_requested_.store(false);
    playing_.store(true);

    xTaskCreate(RadioTask, "radio_task", 8192, this, 3, &task_handle_);
    ESP_LOGI(TAG, "Radyo baslatildi: %d - %s", channel, ChannelName(channel));
}

void RadioPlayer::Stop() {
    if (!playing_.load())
        return;
    stop_requested_.store(true);

    // Task'ın bitmesini bekle (maks 3 saniye)
    int waited = 0;
    while (task_handle_ != nullptr && waited < 300) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited++;
    }
    playing_.store(false);
    current_channel_ = 0;
    ESP_LOGI(TAG, "Radyo durduruldu");
}

// ============================================================
//  RadioTask
// ============================================================
void RadioPlayer::RadioTask(void* arg) {
    auto* self = static_cast<RadioPlayer*>(arg);
    self->RunRadioTask();
    self->task_handle_ = nullptr;
    self->playing_.store(false);
    vTaskDelete(nullptr);
}

void RadioPlayer::RunRadioTask() {
    // URL'yi bul
    const char* url = nullptr;
    for (int i = 0; i < kStationCount; i++) {
        if (kStations[i].number == current_channel_) {
            url = kStations[i].url;
            break;
        }
    }
    if (!url) {
        ESP_LOGE(TAG, "URL bulunamadi");
        return;
    }

    ESP_LOGI(TAG, "HTTP akisi: %s", url);

    // HTTP client yapılandır
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 10000;
    config.buffer_size = 4096;
    config.buffer_size_tx = 512;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client olusturulamadi");
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open hatasi: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP yanit: %d, icerik boyutu: %d", status, content_length);

    // MP3 decoder aç
    void* dec_handle = nullptr;
    esp_audio_err_t dec_err = esp_mp3_dec_open(nullptr, 0, &dec_handle);
    if (dec_err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "MP3 decoder acilamadi: %d", (int)dec_err);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    // AudioCodec referansı al (PCM yazmak için)
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        ESP_LOGE(TAG, "AudioCodec bulunamadi");
        esp_mp3_dec_close(dec_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    // -----------------------------------------------------------
    //  Sabit boyutlu input buffer (FLAT, memmove ile yönetilir)
    // -----------------------------------------------------------
    const int FLAT_SIZE = 16384;
    const int READ_CHUNK = 2048;
    // Bir MP3 frame'i = 1152 sample × 4 byte (stereo 16-bit) = 4608 byte
    const int PCM_SIZE = 4608 * 2;  // güvenli marj

    uint8_t* flat_buf = (uint8_t*)heap_caps_malloc(FLAT_SIZE, MALLOC_CAP_DEFAULT);
    uint8_t* pcm_buf = (uint8_t*)heap_caps_malloc(PCM_SIZE, MALLOC_CAP_DEFAULT);

    if (!flat_buf || !pcm_buf) {
        ESP_LOGE(TAG, "Bellek yetersiz");
        free(flat_buf);
        free(pcm_buf);
        esp_mp3_dec_close(dec_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    int flat_used = 0;    // flat_buf içindeki geçerli byte sayısı
    int flat_offset = 0;  // decoder'ın okuduğu pozisyon

    while (!stop_requested_.load()) {
        // --- 1. flat buffer doldurmaya çalış ---
        int remain = flat_used - flat_offset;

        // Kalan veriyi başa taşı (flat_offset çok büyüdüğünde)
        if (flat_offset > READ_CHUNK && remain > 0) {
            memmove(flat_buf, flat_buf + flat_offset, remain);
            flat_used = remain;
            flat_offset = 0;
        } else if (remain == 0) {
            flat_used = 0;
            flat_offset = 0;
        }

        // HTTP'den yeni veri oku (boş alan varsa)
        int space = FLAT_SIZE - flat_used;
        if (space >= READ_CHUNK) {
            int n = esp_http_client_read(client, (char*)(flat_buf + flat_used), READ_CHUNK);
            if (n > 0) {
                flat_used += n;
            } else if (n == 0) {
                ESP_LOGW(TAG, "HTTP akisi bitti");
                break;
            } else {
                ESP_LOGE(TAG, "HTTP okuma hatasi: %d", n);
                break;
            }
        }

        // --- 2. Yeterli veri yoksa bekle ---
        remain = flat_used - flat_offset;
        if (remain < 128) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // --- 3. MP3 decode ---
        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = flat_buf + flat_offset;
        raw.len = (uint32_t)remain;

        esp_audio_dec_out_frame_t frame = {};
        frame.buffer = pcm_buf;
        frame.len = (uint32_t)PCM_SIZE;

        esp_audio_dec_info_t info = {};

        esp_audio_err_t ret = esp_mp3_dec_decode(dec_handle, &raw, &frame, &info);

        // Her durumda consumed kadar ilerle
        if (raw.consumed > 0) {
            flat_offset += (int)raw.consumed;
        }

        if (ret == ESP_AUDIO_ERR_OK) {
            // decoded_size byte PCM verisi hazır
            uint32_t decoded = frame.decoded_size;
            if (decoded > 0) {
                int samples = (int)(decoded / sizeof(int16_t));
                std::vector<int16_t> pcm_vec((int16_t*)pcm_buf, (int16_t*)pcm_buf + samples);
                codec->OutputData(pcm_vec);
            }
        } else if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            // Çıkış buffer yeterli değil — bir sonraki iterasyona bırak
            vTaskDelay(pdMS_TO_TICKS(5));
        } else if (ret != ESP_AUDIO_ERR_OK) {
            // Sync kaybı vs: consumed = 0 ise elle 1 byte atla
            if (raw.consumed == 0) {
                flat_offset += 1;
            }
        }

        taskYIELD();
    }

    // Temizlik
    free(flat_buf);
    free(pcm_buf);
    esp_mp3_dec_close(dec_handle);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "RadioTask bitti");
}
