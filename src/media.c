#include "media.h"

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static uint32_t le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int tag(const uint8_t *p, const char *s) {
    return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1] &&
           p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3];
}
int media_wave_open(MediaWave *wave, const void *file, uint32_t bytes) {
    if (!wave) return MEDIA_BAD_FILE;
    *wave = (MediaWave){0};
    if (!file || bytes < 12) return MEDIA_BAD_FILE;
    const uint8_t *p = file;
    if (!tag(p, "RIFF") || !tag(p + 8, "WAVE")) return MEDIA_BAD_FILE;
    uint32_t riff = le32(p + 4);
    if (riff < 4 || riff > bytes - 8) return MEDIA_BAD_FILE;
    uint32_t end = riff + 8, pos = 12;
    int have_format = 0, have_data = 0;
    MediaWave parsed = {0};
    while (pos < end) {
        if (end - pos < 8) return MEDIA_BAD_FILE;
        uint32_t size = le32(p + pos + 4);
        const uint8_t *body = p + pos + 8;
        pos += 8;
        if (size > end - pos) return MEDIA_BAD_FILE;
        if (tag(body - 8, "fmt ")) {
            if (have_format || size < 16) return MEDIA_BAD_FILE;
            if (le16(body) != 1) return MEDIA_UNSUPPORTED;
            parsed.channels = le16(body + 2);
            parsed.sample_rate = le32(body + 4);
            parsed.block_align = le16(body + 12);
            parsed.bits_per_sample = le16(body + 14);
            if ((parsed.channels != 1 && parsed.channels != 2) ||
                (parsed.bits_per_sample != 8 && parsed.bits_per_sample != 16) ||
                parsed.sample_rate < 5000 || parsed.sample_rate > 48000)
                return MEDIA_UNSUPPORTED;
            unsigned align = parsed.channels * (parsed.bits_per_sample / 8);
            if (parsed.block_align != align ||
                le32(body + 8) != parsed.sample_rate * align) return MEDIA_BAD_FILE;
            have_format = 1;
        } else if (tag(body - 8, "data")) {
            if (have_data) return MEDIA_BAD_FILE;
            parsed.data = body;
            parsed.data_bytes = size;
            have_data = 1;
        }
        pos += size;
        if (size & 1) {
            if (pos == end) return MEDIA_BAD_FILE;
            ++pos;
        }
    }
    if (!have_format || !have_data || !parsed.data_bytes ||
        parsed.data_bytes % parsed.block_align) return MEDIA_BAD_FILE;
    parsed.frames = parsed.data_bytes / parsed.block_align;
    *wave = parsed;
    return MEDIA_OK;
}
uint32_t media_wave_read(MediaWave *wave, int16_t *output, uint32_t max_frames) {
    if (!wave || !output || wave->frame_cursor >= wave->frames) return 0;
    uint32_t count = wave->frames - wave->frame_cursor;
    if (count > max_frames) count = max_frames;
    const uint8_t *p = wave->data + wave->frame_cursor * wave->block_align;
    uint32_t samples = count * wave->channels;
    if (wave->bits_per_sample == 8) {
        for (uint32_t i = 0; i < samples; ++i) output[i] = (int16_t)(((int)p[i] - 128) * 256);
    } else {
        for (uint32_t i = 0; i < samples; ++i) output[i] = (int16_t)le16(p + i * 2);
    }
    wave->frame_cursor += count;
    return count;
}
const char *media_error_string(int error) {
    switch (error) {
    case MEDIA_OK: return "Ready";
    case MEDIA_BAD_FILE: return "Invalid or truncated audio file";
    case MEDIA_UNSUPPORTED: return "Unsupported audio format";
    case MEDIA_TOO_LARGE: return "Audio file exceeds playback capacity";
    case MEDIA_NO_DEVICE: return "Sound Blaster 16 not found";
    case MEDIA_DEVICE_ERROR: return "Audio hardware did not respond";
    case MEDIA_UNDERRUN: return "Playback stopped: audio buffer underrun";
    default: return "Audio error";
    }
}
