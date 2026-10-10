#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include <driver/uart.h>

// ─────────────────────────────────────────────────────────────────────────────
// Global singleton erişim fonksiyonları (cam_link.cc içinde implement edilir)
// Bu fonksiyonlar MCP tool callback'lerinden çağrılır.
// ─────────────────────────────────────────────────────────────────────────────
void esp32cam_send_capture();

/**
 * Simple, robust UART framing for JPEG transfer from ESP32-CAM to ESP32-S3.
 *
 * Frame format (ASCII header + raw bytes):
 *   "IMG:" <decimal length> ":" <jpeg bytes...> "\r\n"
 *
 * Also supports event lines (ASCII, newline terminated):
 *   "EVT:MOTION\n"
 *   "EVT:READY\n"
 */
class CamLink {
public:
    struct JpegFrame {
        uint8_t* data = nullptr;
        size_t len = 0;
    };

    using OnJpegCallback = std::function<void(JpegFrame&& frame)>;
    using OnEventCallback = std::function<void(const char* evt)>;

    CamLink() = default;
    ~CamLink() = default;

    CamLink(const CamLink&) = delete;
    CamLink& operator=(const CamLink&) = delete;

    void Start(uart_port_t port, OnJpegCallback on_jpeg, OnEventCallback on_evt);

    // Commands sent to ESP32-CAM (newline terminated).
    void SendCapture();

private:
    static void RxTaskThunk(void* arg);
    void RxTask();

    void SendLine(const char* line);
    void HandleAsciiLine(const char* line, size_t len);

    bool TryParseImgHeader(size_t& out_len);
    void ResetParser();

    uart_port_t port_ = UART_NUM_MAX;
    OnJpegCallback on_jpeg_;
    OnEventCallback on_evt_;

    // Parser state
    enum class State { Search, ReadHeader, ReadJpeg, ReadTrailer } state_ = State::Search;
    size_t expected_len_ = 0;
    size_t received_len_ = 0;
    uint8_t* jpeg_buf_ = nullptr;

    // Small ASCII line buffer (events/headers)
    static constexpr size_t kLineBufSize = 128;
    char line_buf_[kLineBufSize] = {};
    size_t line_len_ = 0;
};

