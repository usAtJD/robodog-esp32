#include "cam_link.h"

#include <cstring>
#include <cstdlib>

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "CamLink";

static uint8_t* AllocImageBuf(size_t len) {
    // Prefer PSRAM when available, fall back to internal.
    uint8_t* p = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
    return (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_8BIT);
}

void CamLink::Start(uart_port_t port, OnJpegCallback on_jpeg, OnEventCallback on_evt) {
    port_ = port;
    on_jpeg_ = std::move(on_jpeg);
    on_evt_ = std::move(on_evt);

    ResetParser();
    // UART driver mcp_server.cc içinde kurulduğu için burada sadece task başlatıyoruz
    xTaskCreate(&CamLink::RxTaskThunk, "cam_uart_rx", 4096 * 2, this, 8, nullptr);
}

void CamLink::SendCapture() { SendLine("CAPTURE\n"); }

// ─────────────────────────────────────────────────────────────────────────────
// Global singleton CamLink nesnesi
// McpServer::AddCamLinkTools() tarafından başlatılır.
// MCP tool callback'leri bu fonksiyonlar aracılığıyla erişir.
// ─────────────────────────────────────────────────────────────────────────────
CamLink g_cam_link;

void esp32cam_send_capture()    { g_cam_link.SendCapture(); }

void CamLink::SendLine(const char* line) {
    if (port_ == UART_NUM_MAX || line == nullptr) return;
    uart_write_bytes(port_, line, std::strlen(line));
}

void CamLink::RxTaskThunk(void* arg) {
    static_cast<CamLink*>(arg)->RxTask();
}

void CamLink::ResetParser() {
    state_ = State::Search;
    expected_len_ = 0;
    received_len_ = 0;
    if (jpeg_buf_) {
        heap_caps_free(jpeg_buf_);
        jpeg_buf_ = nullptr;
    }
    std::memset(line_buf_, 0, sizeof(line_buf_));
    line_len_ = 0;
}

bool CamLink::TryParseImgHeader(size_t& out_len) {
    // Expect line_buf_ to contain: "IMG:<len>:"
    // We parse only when we have the trailing ':'.
    if (line_len_ < 6) return false;  // "IMG:0:" minimal
    if (std::memcmp(line_buf_, "IMG:", 4) != 0) return false;

    // Find second ':'
    const char* p = line_buf_ + 4;
    char* end = nullptr;
    long v = std::strtol(p, &end, 10);
    if (end == nullptr || *end != ':') return false;
    if (v <= 0) return false;
    out_len = (size_t)v;
    return true;
}

void CamLink::HandleAsciiLine(const char* line, size_t len) {
    if (len == 0 || line == nullptr) return;
    if (len >= 4 && std::memcmp(line, "EVT:", 4) == 0) {
        if (on_evt_) on_evt_(line + 4);
        return;
    }
    // Other lines: ignore but log for debugging.
    ESP_LOGI(TAG, "UART: %.*s", (int)len, line);
}

void CamLink::RxTask() {
    uint8_t buf[256];

    while (true) {
        int n = uart_read_bytes(port_, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n <= 0) continue;

        ESP_LOGD(TAG, "Read %d bytes from UART", n);

        for (int i = 0; i < n; i++) {
            const uint8_t b = buf[i];

            if (state_ == State::Search) {
                // Search for ASCII lines first; if we see 'I' start header capture
                if (b == 'I') {
                    line_len_ = 0;
                    line_buf_[line_len_++] = (char)b;
                    state_ = State::ReadHeader;
                } else if (b == 'E') {
                    line_len_ = 0;
                    line_buf_[line_len_++] = (char)b;
                    state_ = State::ReadHeader;
                } else {
                    // ignore noise
                }
                continue;
            }

            if (state_ == State::ReadHeader) {
                if (line_len_ + 1 < kLineBufSize) {
                    line_buf_[line_len_++] = (char)b;
                } else {
                    ESP_LOGW(TAG, "Line overflow, reset");
                    ResetParser();
                    continue;
                }

                // JPEG header ends with ':' (IMG:<len>:), events end with '\n'
                if (b == '\n') {
                    // ASCII event line
                    // trim \r?\n
                    size_t l = line_len_;
                    while (l > 0 && (line_buf_[l - 1] == '\n' || line_buf_[l - 1] == '\r')) l--;
                    HandleAsciiLine(line_buf_, l);
                    ResetParser();
                    continue;
                }

                if (b == ':') {
                    size_t img_len = 0;
                    if (TryParseImgHeader(img_len)) {
                        expected_len_ = img_len;
                        received_len_ = 0;
                        jpeg_buf_ = AllocImageBuf(expected_len_);
                        if (!jpeg_buf_) {
                            ESP_LOGE(TAG, "No memory for JPEG (%u bytes)", (unsigned)expected_len_);
                            ResetParser();
                            continue;
                        }
                        state_ = State::ReadJpeg;
                        continue;
                    }
                }

                continue;
            }

            if (state_ == State::ReadJpeg) {
                if (!jpeg_buf_) {
                    ResetParser();
                    continue;
                }
                jpeg_buf_[received_len_++] = b;
                if (received_len_ >= expected_len_) {
                    state_ = State::ReadTrailer;
                }
                continue;
            }

            if (state_ == State::ReadTrailer) {
                // Consume optional \r\n, then emit frame.
                // Accept both '\n' alone or '\r\n'. Also accept immediate next header start.
                if (b == '\n' || b == '\r') {
                    // keep consuming until we see '\n' then finish
                    if (b == '\n') {
                        if (on_jpeg_) {
                            JpegFrame frame;
                            frame.data = jpeg_buf_;
                            frame.len = expected_len_;
                            jpeg_buf_ = nullptr;  // ownership transferred
                            on_jpeg_(std::move(frame));
                        } else {
                            heap_caps_free(jpeg_buf_);
                            jpeg_buf_ = nullptr;
                        }
                        state_ = State::Search;
                        expected_len_ = 0;
                        received_len_ = 0;
                        line_len_ = 0;
                    }
                    continue;
                }

                // Trailer missing; still deliver frame and treat this byte as potential next header start.
                if (on_jpeg_) {
                    JpegFrame frame;
                    frame.data = jpeg_buf_;
                    frame.len = expected_len_;
                    jpeg_buf_ = nullptr;
                    on_jpeg_(std::move(frame));
                } else if (jpeg_buf_) {
                    heap_caps_free(jpeg_buf_);
                    jpeg_buf_ = nullptr;
                }
                state_ = State::Search;
                expected_len_ = 0;
                received_len_ = 0;
                line_len_ = 0;

                // Re-process this byte as start of next message.
                i--;
                continue;
            }
        }
    }
}

