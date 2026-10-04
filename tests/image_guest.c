/* Ordinary images decoded and drawn inside the real QEMU kernel. */
#include "app.h"
#include "audio.h"
#include "fs.h"
#include "image_viewer.h"
#include "platform.h"
#include "persist.h"
#include "image_fixtures.h"

/* This client fixture supplies theme values; all hardware, rendering, storage,
 * process setup and image code are the actual production objects. */
uint8_t app_accent, app_accent_dk, app_text, app_text_dim, app_chrome, app_chrome_dk;
static unsigned loaded_pixels;
static unsigned poll_calls, poll_last, poll_gap;
static int poll_measure;
void platform_poll(void) {
    if (poll_measure) {
        unsigned now = timer_ticks(), gap = now - poll_last;
        if (gap > poll_gap) poll_gap = gap;
        poll_last = now; poll_calls++;
    }
    audio_poll();
}

static void check(int ok, const char *message) {
    if (!ok) { platform_log("IMAGE-QEMU-FAIL "); platform_log(message); platform_log("\n"); panic(message); }
}
static unsigned checksum(const unsigned char *bytes, unsigned size) {
    unsigned hash = 2166136261u;
    while (size--) hash = (hash ^ *bytes++) * 16777619u;
    return hash;
}
static void present(const char *subtitle) {
    draw_rect(0, 0, fb_w, fb_h, gfx_rgb(27, 48, 70));
    draw_string_bold("BaseOS / real image decoding", 70, 30, COLOR_WHITE);
    draw_string(subtitle, 70, 58, gfx_gray(215));
    draw_round_rect(64, 97, 752, 552, 12, app_chrome);
    draw_string_bold(image_viewer_title(), 80, 110, app_text);
    image_viewer_draw(65, 140, 750, 508);
    gfx_present();
}
static void open_image(const unsigned char *source, unsigned size, const char *name) {
    const ImageReference *reference = 0;
    for (unsigned i = 0; i < sizeof references / sizeof references[0]; i++) {
        if (size != references[i].bytes) continue;
        unsigned j = 0;
        while (j < size && source[j] == references[i].data[j]) j++;
        if (j == size) { reference = &references[i]; break; }
    }
    check(reference != 0, "ordinary source matches independent reference");
    ImageDecoded decoded;
    check(image_decode(source, size, (unsigned char *)IMAGE_BASE + IMAGE_MAX_PIXELS,
                       IMAGE_CAPACITY - IMAGE_MAX_PIXELS, &decoded) == IMAGE_OK, "reference image decode");
    check(decoded.width == reference->width && decoded.height == reference->height &&
          decoded.format == reference->format, "reference dimensions and format");
    loaded_pixels = decoded.width * decoded.height;
    for (unsigned i = 0; i < reference->count; i++) {
        const ImagePixelCheck *sample = &reference->checks[i];
        unsigned at = sample->y * decoded.width + sample->x;
        const unsigned char *pixel = decoded.pixels + at * (decoded.channels ? decoded.channels : 1);
        if (!decoded.channels) { check(*pixel == sample->rgba[0], "BOS1 palette reference"); continue; }
        unsigned rgba[4] = {pixel[0], pixel[0], pixel[0], 255};
        if (decoded.channels >= 3) { rgba[1] = pixel[1]; rgba[2] = pixel[2]; }
        if (decoded.channels == 2 || decoded.channels == 4) rgba[3] = pixel[decoded.channels - 1];
        check(rgba[3] == sample->rgba[3], "reference alpha");
        if (!rgba[3]) continue;
        for (unsigned channel = 0; channel < 3; channel++) {
            int difference = (int)rgba[channel] - sample->rgba[channel];
            if (difference < 0) difference = -difference;
            check(difference <= (decoded.format == IMAGE_FORMAT_JPEG ? 3 : 0), "independent reference pixels");
        }
    }
    int result = image_viewer_open(source, size, name);
    if (result != IMAGE_OK) { platform_log(image_viewer_status()); platform_log("\n"); }
    check(result == IMAGE_OK && image_viewer_loaded(), name);
}
static void shot(const char *name, const char *subtitle) {
    present(subtitle);
    platform_log("IMAGE-QEMU-SHOT-"); platform_log(name); platform_log("\n");
    timer_delay(TIMER_HZ * 2);
}
#ifdef IMAGE_TEST_AUDIO
#ifndef IMAGE_AUDIO_RATE
#define IMAGE_AUDIO_RATE 44100u
#endif
static void le16(unsigned char *out, unsigned value) { out[0] = value; out[1] = value >> 8; }
static void le32(unsigned char *out, unsigned value) { le16(out, value); le16(out + 2, value >> 16); }
static void audio_benchmark(void) {
    check(audio_init() == MEDIA_OK, "SB16 available for image/audio check");
    audio_set_volume(100);
#ifdef IMAGE_TEST_MP3
    check(audio_play(fixture_mp3, sizeof fixture_mp3) == MEDIA_OK, "start continuous MP3");
#else
    unsigned char *wave = (unsigned char *)AUDIO_WORK_BASE;
    unsigned frames = IMAGE_AUDIO_RATE * 8, bytes = frames * 4;
    check(bytes + 44 <= AUDIO_WORK_CAPACITY, "audio fixture fits owned buffer");
    kmemset(wave, 0, bytes + 44);
    kmemcpy(wave, "RIFF", 4); le32(wave + 4, bytes + 36);
    kmemcpy(wave + 8, "WAVEfmt ", 8); le32(wave + 16, 16);
    le16(wave + 20, 1); le16(wave + 22, 2); le32(wave + 24, IMAGE_AUDIO_RATE);
    le32(wave + 28, IMAGE_AUDIO_RATE * 4); le16(wave + 32, 4); le16(wave + 34, 16);
    kmemcpy(wave + 36, "data", 4); le32(wave + 40, bytes);
    for (unsigned frame = 0; frame < frames; frame++) {
        le16(wave + 44 + frame * 4, frame % 100 < 50 ? 12000 : -12000);
        le16(wave + 46 + frame * 4, frame % 50 < 25 ? 9000 : -9000);
    }
    check(audio_play_wav(wave, bytes + 44) == MEDIA_OK, "start continuous stereo");
#endif
    while (audio_status()->state == AUDIO_LOADING) audio_poll();
    check(audio_status()->state == AUDIO_PLAYING, "audio playing before image work");
    timer_delay(2);
    unsigned repeats = 6;
#ifdef IMAGE_TEST_MP3
    repeats = 12;
#endif
    for (unsigned i = 0; i < repeats; i++) {
        unsigned before = timer_ticks();
        poll_measure = 1; poll_calls = poll_gap = 0; poll_last = before;
        int gif = 0;
#ifdef IMAGE_TEST_MP3
        gif = i % 3 == 2;
#endif
        int result = gif ? image_viewer_open(fixture_large_gif, sizeof fixture_large_gif, "large.gif") :
                     i & 1 ? image_viewer_open(fixture_png, sizeof fixture_png, "landscape.png") :
                             image_viewer_open(fixture_jpeg, sizeof fixture_jpeg, "landscape.jpg");
        unsigned elapsed = timer_ticks() - before;
        check(result == IMAGE_OK, "image open while audio active");
        platform_poll(); poll_measure = 0;
        char number[20];
        platform_log(gif ? "IMAGE-AUDIO-GIF ticks=" : i & 1 ? "IMAGE-AUDIO-PNG ticks=" : "IMAGE-AUDIO-JPEG ticks=");
        fmt_uint(number, elapsed); platform_log(number); platform_log(" state=");
        fmt_uint(number, audio_status()->state); platform_log(number); platform_log(" underruns=");
        fmt_uint(number, audio_status()->underruns); platform_log(number); platform_log(" polls=");
        fmt_uint(number, poll_calls); platform_log(number); platform_log(" max-gap=");
        fmt_uint(number, poll_gap); platform_log(number); platform_log("\n");
        check(audio_status()->state == AUDIO_PLAYING && !audio_status()->underruns, "audio continues without underrun during images");
        check(poll_calls > 10 && poll_gap < TIMER_HZ / 14, "bounded image progress polling");
        timer_delay(2);
    }
    audio_stop();
    platform_log("IMAGE-QEMU-AUDIO-PASS\n");
}
#endif
static void x87_preserved(void) {
    /* A normal, nondefault caller x87 state survives an integer decoder call. */
    unsigned previous;
    __asm__ volatile("mov %%cr0,%0" : "=r"(previous));
    unsigned enabled = (previous & ~12u) | 2u;
    __asm__ volatile("mov %0,%%cr0" :: "r"(enabled) : "memory");
    struct { unsigned char bytes[108]; } original, before, after;
    kmemset(&original, 0, sizeof original); kmemset(&before, 0, sizeof before); kmemset(&after, 0, sizeof after);
    unsigned short control = 0x077f;
    /* Establish the comparison after one save/restore round trip: QEMU's
     * legacy FRSTOR normalizes saved instruction-pointer metadata to zero. */
    __asm__ volatile("fnsave %0\n\tfninit\n\tfldcw %1\n\tfldpi\n\tfld1\n\tfnsave %2\n\tfrstor %2\n\tfnsave %2\n\tfrstor %2"
                     : "=m"(original), "+m"(control), "=m"(before) :: "memory");
    open_image(fixture_jpeg, sizeof fixture_jpeg, "landscape.jpg");
    __asm__ volatile("fnsave %0\n\tfrstor %1" : "=m"(after) : "m"(original) : "memory");
    __asm__ volatile("mov %0,%%cr0" :: "r"(previous) : "memory");
    for (unsigned i = 0; i < sizeof before; i++) if (before.bytes[i] != after.bytes[i]) {
        char number[16]; fmt_uint(number, i); platform_log("x87 offset "); platform_log(number);
        fmt_uint(number, before.bytes[i]); platform_log(" before "); platform_log(number);
        fmt_uint(number, after.bytes[i]); platform_log(" after "); platform_log(number); platform_log("\n");
        check(0, "caller x87 state preserved");
    }
    platform_log("IMAGE-QEMU-X87-PASS\n");
}
void image_guest(void) {
    platform_validate_memory();
    const BootInfo *bi = (const BootInfo *)BOOTINFO_ADDR;
    check(video_info_valid(bi), "graphics boot info");
    gfx_init((uint8_t *)FB_BASE, (uint8_t *)(uintptr_t)bi->lfb, bi->width, bi->height, bi->bpp, bi->pitch);
    app_accent = gfx_rgb(43, 103, 190); app_text = gfx_gray(30); app_text_dim = gfx_gray(90);
    app_chrome = gfx_gray(239); app_chrome_dk = gfx_gray(190);
    disk_configure(bi->sectors_per_track);
    fs_init(); fs_load_disk(); check(fs_file_limit() == IMAGE_MAX_FILE_BYTES, "large data volume available");
#ifdef IMAGE_TEST_AUDIO
    audio_benchmark();
#endif
    x87_preserved();
    shot("JPEG", "Baseline JPEG / independent reference-pixel test passed on the host");
    open_image(fixture_progressive, sizeof fixture_progressive, "progressive.jpg");
    present("Progressive JPEG");
    platform_log("IMAGE-QEMU-PROGRESSIVE-PASS\n");
    int file = fs_create(fs_root(), "landscape.png"); check(file >= 0, "create ordinary PNG");
    check(fs_write(file, (const char *)fixture_png, sizeof fixture_png) == sizeof fixture_png, "write ordinary PNG");
    open_image((const unsigned char *)fs_data(file), (unsigned)fs_size(file), fs_name(file));
    unsigned before = checksum((const unsigned char *)IMAGE_BASE, loaded_pixels);
    check(fs_write(file, "source replaced after open", 26) == 26, "mutate filesystem source");
    check(before == checksum((const unsigned char *)IMAGE_BASE, loaded_pixels), "owned image survives source mutation");
    shot("PNG", "Lossless PNG / owned pixels survive filesystem compaction and source replacement");
    image_viewer_key(0, '1'); image_viewer_key(0, '+'); image_viewer_key(0, '+');
    image_viewer_scroll(3); image_viewer_key(KEY_RIGHT, 0);
    image_viewer_draw(65, 140, 360, 200); gfx_present();
    check(image_viewer_loaded(), "zoom pan and minimum geometry");
    image_viewer_key(0, 'f'); platform_log("IMAGE-QEMU-CONTROLS-PASS\n");
    open_image(fixture_alpha, sizeof fixture_alpha, "transparent.png");
    shot("ALPHA", "RGBA PNG / transparency composited over a checkerboard");
    open_image(fixture_bmp, sizeof fixture_bmp, "landscape.bmp");
    present("BMP"); platform_log("IMAGE-QEMU-BMP-PASS\n");
    open_image(fixture_gif, sizeof fixture_gif, "animated.gif");
    present("GIF first frame"); platform_log("IMAGE-QEMU-GIF-PASS\n");
    open_image(fixture_large_gif, sizeof fixture_large_gif, "large.gif");
    present("1024 x 512 GIF first frame"); platform_log("IMAGE-QEMU-LARGE-GIF-PASS\n");
    open_image(fixture_bos, sizeof fixture_bos, "paint.pbm");
    present("BaseOS Paint BOS1"); platform_log("IMAGE-QEMU-BOS1-PASS\n");
    check(image_viewer_open("ordinary text", 13, "notes.txt") == IMAGE_UNSUPPORTED, "unsupported format reports a clear error");
    image_viewer_draw(65, 140, 360, 200); gfx_present();
    image_viewer_close(); check(!image_viewer_loaded(), "close clears image state");
    open_image(fixture_jpeg, sizeof fixture_jpeg, "landscape.jpg");
    present("JPEG, PNG, BMP, GIF first frame and BOS1 / fit, actual size, zoom and pan");
    platform_log("IMAGE-QEMU-PASS\n");
    for (;;) __asm__ volatile("hlt");
}
