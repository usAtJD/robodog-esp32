#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Uploads a JPEG to a user-configured webhook (Make.com / IFTTT / Apps Script).
 * This avoids hardcoding Gmail passwords into firmware.
 *
 * Settings namespace: "photo"
 * - webhook_url: string (required)
 * - to_email: string (optional; default is user's gmail)
 */
class PhotoWebhook {
public:
    // Returns true if upload HTTP status is 2xx.
    static bool UploadJpeg(const uint8_t* jpeg, size_t len);
};

