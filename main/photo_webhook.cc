#include "photo_webhook.h"

#include "settings.h"

#include <cstring>
#include <string>

#include <esp_log.h>
#include <esp_http_client.h>

static const char* TAG = "PhotoWebhook";

static std::string GetSetting(const char* key, const char* def = "") {
    Settings s("photo", false);
    return s.GetString(key, def);
}

bool PhotoWebhook::UploadJpeg(const uint8_t* jpeg, size_t len) {
    if (jpeg == nullptr || len == 0) {
        ESP_LOGE(TAG, "Gecersiz JPEG verisi (null veya bos)");
        return false;
    }

    ESP_LOGI(TAG, "Fotoğraf yükleme işlemi başlatıldı, boyut: %u bayt", (unsigned)len);

    std::string url = GetSetting("webhook_url", "");
    if (url.empty()) {
        url = "https://script.google.com/macros/s/AKfycbwuVXDntDzjXWXc89gBoKXFXmxG70nT28QByMLsYhJoKf4xS-cCfK8XL6fKHsBo63Uu/exec";
    }
    
    if (url.empty()) {
        ESP_LOGW(TAG, "Gonderim yok: webhook ayarla (photo.webhook_url)");
        return false;
    }
    const std::string to = GetSetting("to_email", "yesilyurtisa58@gmail.com");

    // Build a minimal multipart/form-data body.
    const char* boundary = "----xiaozhiBoundary7MA4YWxkTrZu0gW";
    std::string head;
    head.reserve(256);
    head += "--";
    head += boundary;
    head += "\r\nContent-Disposition: form-data; name=\"to\"\r\n\r\n";
    head += to;
    head += "\r\n--";
    head += boundary;
    head += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"photo.jpg\"\r\n";
    head += "Content-Type: image/jpeg\r\n\r\n";

    std::string tail;
    tail.reserve(64);
    tail += "\r\n--";
    tail += boundary;
    tail += "--\r\n";

    const size_t total_len = head.size() + len + tail.size();

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 20000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return false;
    }

    std::string ct = "multipart/form-data; boundary=";
    ct += boundary;
    esp_http_client_set_header(client, "Content-Type", ct.c_str());
    esp_http_client_set_header(client, "Accept", "*/*");

    esp_err_t err = esp_http_client_open(client, (int)total_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    auto w1 = esp_http_client_write(client, head.data(), (int)head.size());
    if (w1 < 0) {
        ESP_LOGE(TAG, "http write head failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    auto w2 = esp_http_client_write(client, (const char*)jpeg, (int)len);
    if (w2 < 0) {
        ESP_LOGE(TAG, "http write jpeg failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    auto w3 = esp_http_client_write(client, tail.data(), (int)tail.size());
    if (w3 < 0) {
        ESP_LOGE(TAG, "http write tail failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    err = esp_http_client_fetch_headers(client);
    bool success = false;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Upload status=%d", status);
        success = (status >= 200 && status < 300);
    } else {
        ESP_LOGE(TAG, "HTTP istegi basarisiz: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return success;
}
