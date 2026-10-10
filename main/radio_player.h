#pragma once

#include <string>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================
//  RadioPlayer — Internet radyosu çalar (MP3 stream, HTTP)
//
//  Kullanım:
//    RadioPlayer::GetInstance().Start(3);   // kanal 3
//    RadioPlayer::GetInstance().Stop();
//    RadioPlayer::GetInstance().IsPlaying();
// ============================================================

class RadioPlayer {
public:
    static RadioPlayer& GetInstance() {
        static RadioPlayer instance;
        return instance;
    }
    RadioPlayer(const RadioPlayer&) = delete;
    RadioPlayer& operator=(const RadioPlayer&) = delete;

    // Kanal numarasına göre radyo başlat (1-14)
    void Start(int channel);

    // Radyoyu durdur ve kaynakları serbest bırak
    void Stop();

    // Çalıyor mu?
    bool IsPlaying() const { return playing_.load(); }

    // Mevcut kanal numarası
    int CurrentChannel() const { return current_channel_; }

    // Kanal adını döndür
    static const char* ChannelName(int channel);

    // Türkçe sayı → kanal numarası parse et (0 = bulunamadı)
    static int ParseChannelNumber(const std::string& text);

    // Kanal listesini metin olarak döndür (AI cevabı için)
    static std::string GetChannelListText();

private:
    RadioPlayer() = default;
    ~RadioPlayer() { Stop(); }

    static void RadioTask(void* arg);
    void RunRadioTask();

    std::atomic<bool> playing_{false};
    std::atomic<bool> stop_requested_{false};
    TaskHandle_t task_handle_ = nullptr;
    int current_channel_ = 0;

    struct RadioStation {
        int number;
        const char* name;
        const char* url;
    };

    static const RadioStation kStations[];
    static const int kStationCount;
};
