#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include <tremor/ivorbisfile.h>

#include <3ds.h>
#include <citro2d.h>
#include "ui_assets.h"
#include "shield_ui_language.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_TRACKS 512
#define PATH_CAP 512
#define INPUT_CAP 16384
// Larger linear buffers give the Old 3DS CPU more time to decode MP3 data
// before NDSP consumes the queued audio and would otherwise underrun.
#define BLOCK_FRAMES 8192
#define AUDIO_BLOCK_COUNT 6
#define CHANNEL 0
#define TRACK_FADE_MS 150
#define MP3_BAD_DATA_LIMIT 8192
#define THERMAL_BEEP_CHANNEL 1
#define THERMAL_BEEP_RATE 16000
#define THERMAL_BEEP_FRAMES (THERMAL_BEEP_RATE * 2)
#define CIRCLE_PAD_KEYS (KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT)
#define REVERB_BUFFER_SIZE 2048
#define REVERB_AP_BUFFER_SIZE 1024
#define TOP_WIDTH 400
#define TOP_HEIGHT 240
#define BOTTOM_WIDTH 320
#define BOTTOM_HEIGHT 240
#define COVER_SIZE 128
#define COVER_MAX_BYTES (256U * 1024U)
// Citro2D uses GEQUAL depth testing. Keep the 2D UI on one plane so
// later text, artwork and icons paint over the panel backgrounds.
#define UI_DEPTH 0.0f

typedef struct { char name[256]; u64 duration_ms; } Track;
typedef struct { char name[256]; bool directory; } BrowseEntry;
typedef struct { ndspWaveBuf wave; int16_t *pcm; } AudioBlock;
typedef struct { struct jpeg_error_mgr base; jmp_buf jump; } CoverJpegError;

static Track tracks[MAX_TRACKS];
static int track_count, order[MAX_TRACKS], order_pos = -1;
static BrowseEntry entries[MAX_TRACKS];
static int entry_count, selection;
static char folder[PATH_CAP] = "sdmc:/";
static char current_title[256] = "Aucun morceau";
static char current_artist[128];
static char current_album[128];
static bool current_title_from_metadata;
static size_t title_scroll_offset;
static size_t display_title_chars;
static u64 title_scroll_last_ms;
static FILE *audio_file;
static mp3dec_t decoder;
static OggVorbis_File ogg_decoder;
static bool ogg_open, ogg_track;
static int ogg_channels;
static uint8_t input[INPUT_CAP];
static int input_size;
static AudioBlock blocks[AUDIO_BLOCK_COUNT];
static int16_t decoded[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int16_t pending_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
static int16_t ogg_pcm[BLOCK_FRAMES * 2];
static int pending_frames, pending_pos;
static u64 playback_position_ms, playback_last_update_ms, track_duration_ms;
static u64 playback_rate_remainder;
static u64 audio_payload_bytes;
static u64 mp3_data_offset, mp3_xing_bytes;
static u8 mp3_xing_toc[100];
static bool mp3_toc_valid;
static u64 last_touch_preview_ms, touch_seek_target_ms;
static bool touch_seek_dragging;
static int brightness_level = 3;
static u8 backlight_config[2] = {0, 3};
static bool cfgu_service_ready, backlight_config_valid;
static bool lcd_ready, brightness_changed;
static u16 cover_pixels[COVER_SIZE * COVER_SIZE];
static u8 id3_text_buffer[4096];
static bool cover_valid;
static C3D_RenderTarget *ui_top_target, *ui_bottom_target;
static C2D_TextBuf ui_top_text_buffer, ui_bottom_text_buffer, ui_fit_text_buffer;
static C2D_Font ui_font, ui_top_font;
extern const u8 top_font_start[], top_font_end[];
static C3D_Tex cover_texture;
static bool ui_renderer_ready, cover_texture_ready, cover_texture_dirty;
static u8 cover_texture_upload[128 * 128 * 4];
static const Tex3DS_SubTexture cover_subtexture = {
    COVER_SIZE, COVER_SIZE, 0.0f, 1.0f,
    (float)COVER_SIZE / 128.0f, 1.0f - (float)COVER_SIZE / 128.0f
};
static C2D_Image cover_image;
static C3D_Tex logo_texture;
static bool logo_ready;
static const Tex3DS_SubTexture logo_subtexture = {64,64,0,1,1,0};
static C2D_Image logo_image;
static C3D_Tex boot_texture;
static bool boot_texture_ready;
static u64 boot_started_ms;
static unsigned guide_page;
static bool guide_drawing;
static ShieldUiLanguage ui_language;
static bool force_language_selection;
#define GUIDE_FILE "sdmc:/3ds/ShieldDJStandard/guide-v2.dat"
static const char guide_marker[] = "SHIELD_DJ_GUIDE_2\n";

static void upload_cover_texture(void);

static void copy_name(char *destination, size_t capacity, const char *source) {
    size_t length = strnlen(source, capacity - 1);
    memcpy(destination, source, length);
    destination[length] = 0;
}

static void cover_jpeg_fail(j_common_ptr common) {
    CoverJpegError *error = (CoverJpegError *)common->err;
    longjmp(error->jump, 1);
}

static u32 be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u32 synchsafe32(const u8 *p) {
    return ((u32)(p[0] & 0x7f) << 21) | ((u32)(p[1] & 0x7f) << 14) |
           ((u32)(p[2] & 0x7f) << 7) | (p[3] & 0x7f);
}

static void copy_id3_text(char *out, size_t capacity, const u8 *data, size_t size) {
    if (!capacity) return;
    out[0] = '\0';
    if (!data || size < 2) return;
    unsigned encoding = data[0];
    size_t at = 1, dst = 0;
    bool little_endian = false;
    if (encoding == 1 && size >= 3 && data[1] == 0xff && data[2] == 0xfe) {
        little_endian = true;
        at = 3;
    } else if (encoding == 1 && size >= 3 && data[1] == 0xfe && data[2] == 0xff) {
        at = 3;
    }
    while (at < size && dst + 1 < capacity) {
        unsigned codepoint;
        if (encoding == 1 || encoding == 2) {
            if (at + 1 >= size) break;
            codepoint = little_endian ? data[at] | ((unsigned)data[at + 1] << 8) :
                                         ((unsigned)data[at] << 8) | data[at + 1];
            at += 2;
            if (!codepoint) break;
            if (codepoint >= 0xd800 && codepoint <= 0xdbff && at + 1 < size) at += 2;
            out[dst++] = codepoint >= 0x20 && codepoint <= 0x7e ? (char)codepoint : '?';
        } else {
            unsigned char c = data[at++];
            if (!c) break;
            if (c < 0x80) out[dst++] = c >= 0x20 && c != 0x7f ? (char)c : ' ';
            else {
                unsigned continuation = (c & 0xe0) == 0xc0 ? 1 :
                                       (c & 0xf0) == 0xe0 ? 2 :
                                       (c & 0xf8) == 0xf0 ? 3 : 0;
                while (continuation-- && at < size && (data[at] & 0xc0) == 0x80) at++;
                out[dst++] = '?';
            }
        }
    }
    while (dst && out[dst - 1] == ' ') dst--;
    out[dst] = '\0';
}

static bool decode_cover_jpeg(const u8 *data, size_t size) {
    if (!data || size < 4 || data[0] != 0xff || data[1] != 0xd8) return false;
    struct jpeg_decompress_struct decoder;
    CoverJpegError error;
    memset(&decoder, 0, sizeof(decoder));
    decoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = cover_jpeg_fail;
    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&decoder);
        return false;
    }
    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, (unsigned char *)data, (unsigned long)size);
    if (jpeg_read_header(&decoder, TRUE) != JPEG_HEADER_OK ||
        decoder.image_width > 8192 || decoder.image_height > 8192) {
        jpeg_destroy_decompress(&decoder);
        return false;
    }
    unsigned shortest = decoder.image_width < decoder.image_height ?
                        decoder.image_width : decoder.image_height;
    decoder.scale_num = 1;
    decoder.scale_denom = shortest >= COVER_SIZE * 8 ? 8 :
                          shortest >= COVER_SIZE * 4 ? 4 :
                          shortest >= COVER_SIZE * 2 ? 2 : 1;
    decoder.out_color_space = JCS_RGB;
    if (!jpeg_start_decompress(&decoder)) {
        jpeg_destroy_decompress(&decoder);
        return false;
    }
    unsigned width = decoder.output_width, height = decoder.output_height;
    if (!width || !height || decoder.output_components != 3) {
        jpeg_destroy_decompress(&decoder);
        return false;
    }
    unsigned crop_side = width < height ? width : height;
    unsigned crop_x = (width - crop_side) / 2;
    unsigned crop_y = (height - crop_side) / 2;
    JSAMPARRAY row = (*decoder.mem->alloc_sarray)((j_common_ptr)&decoder,
                       JPOOL_IMAGE, width * 3, 1);
    unsigned y = 0;
    while (decoder.output_scanline < decoder.output_height) {
        JDIMENSION got = jpeg_read_scanlines(&decoder, row, 1);
        if (!got) break;
        if (y < crop_y || y >= crop_y + crop_side) {
            y++;
            continue;
        }
        unsigned first_y = ((y - crop_y) * COVER_SIZE) / crop_side;
        unsigned next_y = ((y - crop_y + 1) * COVER_SIZE) / crop_side;
        if (next_y > COVER_SIZE) next_y = COVER_SIZE;
        for (unsigned dy = first_y; dy < next_y; dy++) {
            for (unsigned dx = 0; dx < COVER_SIZE; dx++) {
                unsigned sx = crop_x + (dx * crop_side) / COVER_SIZE;
                if (sx >= width) sx = width - 1;
                const u8 *rgb = &row[0][sx * 3];
                cover_pixels[dy * COVER_SIZE + dx] =
                    (u16)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
            }
        }
        y++;
    }
    bool ok = jpeg_finish_decompress(&decoder) != 0;
    jpeg_destroy_decompress(&decoder);
    return ok;
}

static bool load_mp3_cover(FILE *file) {
    cover_valid = false;
    if (!file) return false;
    long saved = ftell(file);
    if (saved < 0 || fseek(file, 0, SEEK_SET) != 0) return false;
    u8 header[10];
    bool found = false;
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "ID3", 3) || (header[3] != 3 && header[3] != 4) ||
        (header[5] & 0x80)) goto done;
    u64 tag_end = 10ULL + synchsafe32(header + 6);
    u64 pos = 10;
    if (header[5] & 0x40) {
        u8 ext[4];
        if (fread(ext, 1, 4, file) != 4) goto done;
        u32 ext_size = header[3] == 4 ? synchsafe32(ext) : be32(ext);
        if (ext_size > tag_end - 10) goto done;
        pos += ext_size + (header[3] == 3 ? 4 : 0);
        if (fseek(file, (long)pos, SEEK_SET) != 0) goto done;
    }
    while (pos + 10 <= tag_end) {
        u8 frame[10];
        if (fread(frame, 1, sizeof(frame), file) != sizeof(frame)) break;
        pos += sizeof(frame);
        if (!frame[0]) break;
        u32 frame_size = header[3] == 4 ? synchsafe32(frame + 4) : be32(frame + 4);
        if (!frame_size || pos + frame_size > tag_end) break;
        bool text_frame = !memcmp(frame, "TIT2", 4) || !memcmp(frame, "TPE1", 4) ||
                          !memcmp(frame, "TALB", 4);
        if (text_frame && frame_size <= 4096 && !(frame[8] & 0xC0) && !(frame[9] & 0xC0)) {
            if (fread(id3_text_buffer, 1, frame_size, file) != frame_size) break;
            if (!memcmp(frame, "TIT2", 4)) {
                copy_id3_text(current_title, sizeof(current_title), id3_text_buffer, frame_size);
                current_title_from_metadata = current_title[0] != '\0';
            } else if (!memcmp(frame, "TPE1", 4))
                copy_id3_text(current_artist, sizeof(current_artist), id3_text_buffer, frame_size);
            else
                copy_id3_text(current_album, sizeof(current_album), id3_text_buffer, frame_size);
            pos += frame_size;
            continue;
        }
        if (!memcmp(frame, "APIC", 4) && !(frame[8] & 0xC0) && !(frame[9] & 0xC0) &&
            frame_size <= COVER_MAX_BYTES) {
            u8 *payload = (u8 *)malloc(frame_size);
            if (!payload) break;
            if (fread(payload, 1, frame_size, file) != frame_size) {
                free(payload);
                break;
            }
            size_t at = 1;
            if (at < frame_size) {
                while (at < frame_size && payload[at]) at++;
                at++;
            }
            if (at < frame_size) at++; // picture type
            if (payload[0] == 1 || payload[0] == 2) {
                while (at + 1 < frame_size && (payload[at] || payload[at + 1])) at += 2;
                at += 2;
            } else {
                while (at < frame_size && payload[at]) at++;
                at++;
            }
            if (at < frame_size)
                found = decode_cover_jpeg(payload + at, frame_size - at);
            free(payload);
            if (found) break;
        } else if (fseek(file, (long)(pos + frame_size), SEEK_SET) != 0) {
            break;
        }
        pos += frame_size;
    }
done:
    (void)fseek(file, saved, SEEK_SET);
    cover_valid = found;
    if (found) cover_texture_dirty = true;
    return found;
}

static bool playing, paused, shuffle = true, repeat_current;
static bool track_skip_locked;
static bool audio_ready, ui_dirty = true, time_dirty;
static bool ui_was_active;
static u32 last_art_vblank;
static float art_rotation;
static bool cover_load_pending;
static bool battery_service_ready;
static bool ptmu_service_ready, charging_state_known, battery_charging;
static float battery_percent = -1.0f;
static u64 battery_last_update;
static char clock_text[6] = "--:--";
static u64 clock_next_update_ms;
static u64 charger_last_update;
static bool fs_service_ready, sd_storage_ready;
static u64 sd_total_bytes, sd_free_bytes;
static int battery_temperature_c = -1;
static u64 temperature_last_update;
static int thermal_warning_threshold = 42;
static bool warning_blink_on;
static bool thermal_beep_latched;
static int16_t *thermal_beep_pcm;
static ndspWaveBuf thermal_beep_wave;
static bool audio_eof;
static int fade_in_remaining = -1, fade_in_total;
static AudioBlock *last_queued_block;
static bool track_has_audio;
static int failed_tracks_in_a_row;
static u32 decoded_frame_count;
static const char *audio_status = "";
static const char *playback_status = "";
static bool playback_error;
static float speed = 1.0f;
static int pitch_milli = 1000;
static unsigned audio_boost_mode;
static int32_t boost_gain_q15 = 32768;
static u64 last_home_toggle_ms;
static int base_rate = 44100;
static int applied_output_rate, target_output_rate;

static void update_output_rate(void) {
    target_output_rate = (base_rate * pitch_milli + 500) / 1000;
    if (!applied_output_rate) {
        ndspChnSetRate(CHANNEL, (float)target_output_rate);
        applied_output_rate = target_output_rate;
    }
}

static void advance_output_rate(void) {
    if (!target_output_rate || !applied_output_rate ||
        target_output_rate == applied_output_rate) return;
    int step = base_rate / 200;
    if (step < 40) step = 40;
    int difference = target_output_rate - applied_output_rate;
    if (difference > step) difference = step;
    if (difference < -step) difference = -step;
    applied_output_rate += difference;
    ndspChnSetRate(CHANNEL, (float)applied_output_rate);
}
static unsigned bass_profile;
/* 0=off, then Concert, Cathedral, Extra. */
static unsigned reverb_mode;
static bool reverb_custom_enabled;
static bool reverb_long_enabled;
static unsigned reverb_custom_percent = 50;
static bool touch_reverb_dragging;
static bool touch_reverb_custom_drag;
static unsigned touch_reverb_preview;
static const char *const reverb_names[] = { "Off", "Concert", "Cathedrale", "Extra" };
static unsigned filter_profile;
static const char *const filter_names_fr[] = { "Aucun", "Rock", "Pop", "Jazz", "Clair", "Exterieur", "Classique" };
static const char *const filter_names_en[] = { "None", "Rock", "Pop", "Jazz", "Clear", "Outdoor", "Classical" };
static int32_t filter_low_state[2], filter_high_state[2];
static int32_t filter_gain_current[3] = {32768, 32768, 32768};
static bool filter_state_ready;
static int32_t reverb_wet_current_q15;
static int32_t reverb_dry_current_q15 = 32768;
static bool options_open;
static unsigned options_selection;
static int32_t reverb_comb[2][4][REVERB_BUFFER_SIZE];
static int32_t reverb_damping[2][4];
static int32_t reverb_allpass[2][3][REVERB_AP_BUFFER_SIZE];
static int32_t reverb_smoothed[2];
static unsigned reverb_position;
static const unsigned reverb_comb_delay[2][4] = {
    { 1423, 1493, 1601, 1699 }, { 1451, 1523, 1637, 1747 }
};
static const unsigned reverb_allpass_delay[2][3] = {
    { 347, 113, 251 }, { 353, 127, 263 }
};

static u64 get_payload_bytes(FILE *file, u64 *data_offset) {
    *data_offset = 0;
    struct stat st;
    if (fstat(fileno(file), &st) != 0 || st.st_size <= 0) return 0;
    u64 size = (u64)st.st_size;
    unsigned char id3[10];
    long saved = ftell(file);
    if (saved < 0 || fseek(file, 0, SEEK_SET) != 0) return size;
    if (fread(id3, 1, sizeof(id3), file) == sizeof(id3) &&
        memcmp(id3, "ID3", 3) == 0) {
        u32 tag_size = ((u32)(id3[6] & 0x7f) << 21) |
                       ((u32)(id3[7] & 0x7f) << 14) |
                       ((u32)(id3[8] & 0x7f) << 7) | (id3[9] & 0x7f);
        u64 skip = 10ULL + tag_size + ((id3[5] & 0x10) ? 10ULL : 0ULL);
        if (skip < size) {
            size -= skip;
            *data_offset = skip;
        }
    }
    fseek(file, saved, SEEK_SET);
    return size;
}

static u64 mp3_xing_duration_ms(const u8 *frame, int frame_size,
                               const mp3dec_frame_info_t *info) {
    if (!frame || !info || info->frame_offset < 0 || info->frame_bytes <= 0 ||
        info->frame_bytes > frame_size ||
        info->frame_offset > info->frame_bytes - 8)
        return 0;
    const u8 *header = frame + info->frame_offset;
    int frame_len = info->frame_bytes - info->frame_offset;
    if (frame_len < 8) return 0;
    bool mpeg1 = ((header[1] >> 3) & 3) == 3;
    bool mono = ((header[3] >> 6) & 3) == 3;
    int offset = 4 + ((header[1] & 1) ? 0 : 2) +
                 (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
    if (offset + 8 > frame_len || offset + 8 > frame_size - info->frame_offset)
        return 0;
    const u8 *xing = header + offset;
    if (memcmp(xing, "Xing", 4) && memcmp(xing, "Info", 4)) return 0;
    u32 flags = ((u32)xing[4] << 24) | ((u32)xing[5] << 16) |
                ((u32)xing[6] << 8) | xing[7];
    int field = offset + 8;
    if (!(flags & 1) || field + 4 > frame_len) return 0;
    const u8 *entry = header + field;
    u32 frames = ((u32)entry[0] << 24) | ((u32)entry[1] << 16) |
                 ((u32)entry[2] << 8) | entry[3];
    field += 4;
    if (flags & 2) {
        if (field + 4 > frame_len) return 0;
        entry = header + field;
        mp3_xing_bytes = ((u32)entry[0] << 24) | ((u32)entry[1] << 16) |
                         ((u32)entry[2] << 8) | entry[3];
        field += 4;
    }
    if ((flags & 4) && mp3_xing_bytes &&
        (size_t)field + sizeof(mp3_xing_toc) <= (size_t)frame_len) {
        memcpy(mp3_xing_toc, header + field, sizeof(mp3_xing_toc));
        mp3_toc_valid = true;
    }
    u32 samples = info->layer == 1 ? 384 : (mpeg1 ? 1152 : 576);
    return frames && info->hz > 0 ? (u64)frames * samples * 1000 / info->hz : 0;
}

static void update_playback_clock(u64 now) {
    if (!playback_last_update_ms) playback_last_update_ms = now;
    if (playing && !paused) {
        u64 old_second = playback_position_ms / 1000;
        // Use NDSP's applied rate, preserving fractional milliseconds.
        if (base_rate > 0 && applied_output_rate > 0) {
            u64 scaled = (now - playback_last_update_ms) * (u64)applied_output_rate +
                         playback_rate_remainder;
            playback_position_ms += scaled / (u64)base_rate;
            playback_rate_remainder = scaled % (u64)base_rate;
        }
        if (playback_position_ms / 1000 != old_second) time_dirty = true;
    }
    playback_last_update_ms = now;
}

static void format_track_time(char *out, size_t capacity, u64 milliseconds) {
    u64 seconds = milliseconds / 1000;
    unsigned minutes = (unsigned)(seconds / 60);
    if (minutes > 9999) minutes = 9999;
    snprintf(out, capacity, "%02u:%02u", minutes, (unsigned)(seconds % 60));
}

static void reset_reverb(void) {
    memset(reverb_comb, 0, sizeof(reverb_comb));
    memset(reverb_damping, 0, sizeof(reverb_damping));
    memset(reverb_allpass, 0, sizeof(reverb_allpass));
    reverb_smoothed[0] = reverb_smoothed[1] = 0;
    reverb_position = 0;
    reverb_wet_current_q15 = 0;
    reverb_dry_current_q15 = 32768;
}

static int32_t clamp_reverb_state(int64_t value) {
    if (value > 524287) return 524287;
    if (value < -524288) return -524288;
    return (int32_t)value;
}

static int32_t process_reverb_sample(int32_t input, int channel) {
    static const int32_t feedback_q15[] = { 0, 30890, 31550, 32010, 31690 };
    static const int32_t damping_q15[] = { 0, 24000, 22000, 20000, 22000 };
    int32_t sum = 0;
    unsigned mode = reverb_custom_enabled ? 4 : reverb_mode;
    // Optional longer decay leaves the wet/dry balance unchanged.
    static const int32_t long_add_q15[] = { 0, 800, 500, 400, 500 };
    int32_t feedback = feedback_q15[mode] +
        (reverb_long_enabled ? long_add_q15[mode] : 0);
    int32_t damping = damping_q15[mode];
    for (unsigned i = 0; i < 4; i++) {
        unsigned delay = reverb_comb_delay[channel][i];
        unsigned read = (reverb_position + REVERB_BUFFER_SIZE - delay) % REVERB_BUFFER_SIZE;
        int32_t delayed = reverb_comb[channel][i][read];
        int32_t damped = (int32_t)(((int64_t)delayed * (32768 - damping) +
                                    (int64_t)reverb_damping[channel][i] * damping) >> 15);
        reverb_damping[channel][i] = damped;
        reverb_comb[channel][i][reverb_position] =
            clamp_reverb_state(input + (((int64_t)damped * feedback) >> 15));
        sum += damped / 4;
    }
    int32_t wet = sum;
    for (unsigned i = 0; i < 3; i++) {
        unsigned delay = reverb_allpass_delay[channel][i];
        unsigned read = (reverb_position + REVERB_AP_BUFFER_SIZE - delay) % REVERB_AP_BUFFER_SIZE;
        int32_t delayed = reverb_allpass[channel][i][read];
        /* Schroeder all-pass: y[n] = x[n-D] - g*x[n],
         * xbuf[n] = x[n] + g*y[n]. Keep g at 0.5 for stable diffusion. */
        int32_t output = delayed - (wet >> 1);
        reverb_allpass[channel][i][reverb_position % REVERB_AP_BUFFER_SIZE] =
            clamp_reverb_state(wet + (output >> 1));
        wet = output;
    }
    return wet;
}

static int16_t limit_reverb_sample(int32_t sample) {
    int32_t magnitude = sample < 0 ? -sample : sample;
    if (magnitude > 30000) magnitude = 30000 + ((magnitude - 30000) >> 3);
    if (magnitude > 32760) magnitude = 32760;
    return (int16_t)(sample < 0 ? -magnitude : magnitude);
}

static void apply_reverb(int16_t *pcm, int frames) {
    static const int32_t wet_q15[] = { 0, 17750, 19050, 21850 };
    int32_t wet = reverb_custom_enabled ?
        (int32_t)(reverb_custom_percent * 27000 / 100) : wet_q15[reverb_mode];
    if (!wet && !reverb_wet_current_q15 && reverb_dry_current_q15 == 32768) return;
    for (int i = 0; i < frames; i++) {
        if (reverb_wet_current_q15 < wet) {
            reverb_wet_current_q15 += 32;
            if (reverb_wet_current_q15 > wet) reverb_wet_current_q15 = wet;
        } else if (reverb_wet_current_q15 > wet) {
            reverb_wet_current_q15 -= 32;
            if (reverb_wet_current_q15 < wet) reverb_wet_current_q15 = wet;
        }
        int32_t dry_target = 32768 - wet / (reverb_custom_enabled ? 2 : 4);
        if (reverb_dry_current_q15 < dry_target) {
            reverb_dry_current_q15 += 8;
            if (reverb_dry_current_q15 > dry_target) reverb_dry_current_q15 = dry_target;
        } else if (reverb_dry_current_q15 > dry_target) {
            reverb_dry_current_q15 -= 8;
            if (reverb_dry_current_q15 < dry_target) reverb_dry_current_q15 = dry_target;
        }
        int32_t dry_gain = reverb_dry_current_q15;
        int32_t return_gain = reverb_wet_current_q15 * 3 / 4;
        int32_t left = pcm[i * 2];
        int32_t right = pcm[i * 2 + 1];
        int32_t echo_left = process_reverb_sample(left, 0);
        int32_t echo_right = process_reverb_sample(right, 1);
        reverb_smoothed[0] += (echo_left - reverb_smoothed[0]) / 2;
        reverb_smoothed[1] += (echo_right - reverb_smoothed[1]) / 2;
        int32_t out_left = (int32_t)(((int64_t)left * dry_gain +
                            (int64_t)reverb_smoothed[0] * return_gain) >> 15);
        int32_t out_right = (int32_t)(((int64_t)right * dry_gain +
                             (int64_t)reverb_smoothed[1] * return_gain) >> 15);
        pcm[i * 2] = limit_reverb_sample(out_left);
        pcm[i * 2 + 1] = limit_reverb_sample(out_right);
        reverb_position = (reverb_position + 1) % REVERB_BUFFER_SIZE;
    }
}

/* Three broad, unity-or-lower gain bands. Smooth gain changes between presets. */
static void apply_filter(int16_t *pcm, int frames) {
    static const int32_t gains[7][3] = {
        {32768,32768,32768}, {32768,18500,32768},
        {23500,32768,30000}, {32768,31000,20500},
        {24000,31500,32768}, {21000,32768,32768},
        {30000,32768,23000}
    };
    if (!filter_profile && filter_gain_current[0] == 32768 &&
        filter_gain_current[1] == 32768 && filter_gain_current[2] == 32768) {
        filter_state_ready = false;
        return;
    }
    int rate = base_rate > 8000 ? base_rate : 44100;
    int32_t low_alpha = (int32_t)(32768.0f * 1382.3f / (rate + 1382.3f));
    int32_t high_alpha = (int32_t)(32768.0f * 18849.6f / (rate + 18849.6f));
    if (low_alpha < 1) low_alpha = 1;
    if (high_alpha < 1) high_alpha = 1;
    for (int i = 0; i < frames; i++) {
        for (unsigned band = 0; band < 3; band++) {
            int32_t target = gains[filter_profile][band];
            int32_t *current = &filter_gain_current[band];
            if (*current < target) {
                *current += 48;
                if (*current > target) *current = target;
            } else if (*current > target) {
                *current -= 48;
                if (*current < target) *current = target;
            }
        }
        for (int ch = 0; ch < 2; ch++) {
            int32_t sample = pcm[i*2+ch];
            if (!filter_state_ready) {
                filter_low_state[ch] = sample;
                filter_high_state[ch] = sample;
            }
            filter_low_state[ch] += (int32_t)(((int64_t)(sample-filter_low_state[ch])*low_alpha)>>15);
            filter_high_state[ch] += (int32_t)(((int64_t)(sample-filter_high_state[ch])*high_alpha)>>15);
            int32_t low = filter_low_state[ch];
            int32_t mid = filter_high_state[ch] - low;
            int32_t high = sample - filter_high_state[ch];
            int32_t out = (int32_t)(((int64_t)low*filter_gain_current[0] +
                (int64_t)mid*filter_gain_current[1] +
                (int64_t)high*filter_gain_current[2]) >> 15);
            if (out > 32767) out = 32767;
            if (out < -32768) out = -32768;
            pcm[i*2+ch] = (int16_t)out;
        }
        filter_state_ready = true;
    }
    if (!filter_profile && filter_gain_current[0] == 32768 &&
        filter_gain_current[1] == 32768 && filter_gain_current[2] == 32768)
        filter_state_ready = false;
}

static void reset_filter_state(void) {
    memset(filter_low_state, 0, sizeof(filter_low_state));
    memset(filter_high_state, 0, sizeof(filter_high_state));
    filter_state_ready = false;
}

static void apply_audio_boost(int16_t *pcm, int frames) {
    if (frames <= 0) return;
    if (!audio_boost_mode) {
        boost_gain_q15 = 32768;
        return;
    }
    static const int32_t gain_levels_q15[] = { 32768, 65536, 98304, 163840 };
    for (int pos = 0; pos < frames; ) {
        int count = frames - pos;
        if (count > 256) count = 256;
        int32_t peak = 0;
        for (int i = 0; i < count * 2; ++i) {
            int32_t sample = pcm[pos * 2 + i];
            int32_t magnitude = sample < 0 ? -sample : sample;
            if (magnitude > peak) peak = magnitude;
        }
        int32_t target = gain_levels_q15[audio_boost_mode];
        if (peak > 0 && (int64_t)peak * target > (int64_t)32760 * 32768)
            target = (int32_t)((int64_t)32760 * 32768 / peak);
        int32_t next_target = target;
        if (pos + count < frames) {
            int next_count = frames - pos - count;
            if (next_count > 256) next_count = 256;
            int32_t next_peak = 0;
            for (int i = 0; i < next_count * 2; ++i) {
                int32_t sample = pcm[(pos + count) * 2 + i];
                int32_t magnitude = sample < 0 ? -sample : sample;
                if (magnitude > next_peak) next_peak = magnitude;
            }
            if (next_peak > 0 && (int64_t)next_peak * next_target > (int64_t)32760 * 32768)
                next_target = (int32_t)((int64_t)32760 * 32768 / next_peak);
        }
        int32_t start = boost_gain_q15;
        if (target < start) start = target;
        int32_t end = start;
        if (target > start) {
            int32_t step = (target - start) / 32;
            end += step > 0 ? step : 1;
        }
        if (next_target < end) end = next_target;
        int64_t gain = (int64_t)start << 16;
        int64_t gain_step = ((int64_t)(end - start) << 16) / count;
        for (int i = 0; i < count; ++i) {
            int32_t current = (int32_t)(gain >> 16);
            pcm[(pos + i) * 2] = (int16_t)(((int64_t)pcm[(pos + i) * 2] * current) >> 15);
            pcm[(pos + i) * 2 + 1] = (int16_t)(((int64_t)pcm[(pos + i) * 2 + 1] * current) >> 15);
            gain += gain_step;
        }
        boost_gain_q15 = end;
        pos += count;
    }
}

static void fade_out_block(AudioBlock *block, int frames) {
    if (!block || !block->pcm || frames <= 0) return;
    int fade_frames = (int)(base_rate * speed * TRACK_FADE_MS / 1000.0f);
    if (fade_frames < 1) fade_frames = 1;
    if (fade_frames > frames) fade_frames = frames;
    int start = frames - fade_frames;
    for (int i = start; i < frames; i++) {
        int gain = frames - i - 1;
        block->pcm[i * 2] = (int16_t)((int32_t)block->pcm[i * 2] * gain / fade_frames);
        block->pcm[i * 2 + 1] = (int16_t)((int32_t)block->pcm[i * 2 + 1] * gain / fade_frames);
    }
    DSP_FlushDataCache(block->pcm + start * 2, fade_frames * 2 * sizeof(int16_t));
}

static void fade_in_block(AudioBlock *block, int frames) {
    if (fade_in_remaining < 0) {
        fade_in_total = (int)(base_rate * speed * TRACK_FADE_MS / 1000.0f);
        if (fade_in_total < 1) fade_in_total = 1;
        fade_in_remaining = fade_in_total;
    }
    for (int i = 0; i < frames && fade_in_remaining > 0; i++) {
        int gain = fade_in_total - fade_in_remaining;
        block->pcm[i * 2] = (int16_t)((int32_t)block->pcm[i * 2] * gain / fade_in_total);
        block->pcm[i * 2 + 1] = (int16_t)((int32_t)block->pcm[i * 2 + 1] * gain / fade_in_total);
        fade_in_remaining--;
    }
}

static void update_battery(void) {
    u8 whole, fraction;
    if (battery_service_ready &&
        R_SUCCEEDED(MCUHWC_ReadRegister(0x0B, &whole, 1)) && whole <= 100 &&
        R_SUCCEEDED(MCUHWC_ReadRegister(0x0C, &fraction, 1))) {
        battery_percent = whole + fraction / 256.0f;
    } else {
        battery_percent = -1.0f;
    }
}

static void update_charging_state(void) {
    u8 state;
    if (ptmu_service_ready && R_SUCCEEDED(PTMU_GetBatteryChargeState(&state))) {
        battery_charging = state != 0;
        charging_state_known = true;
    } else {
        charging_state_known = false;
    }
}

static void update_battery_temperature(void) {
    u8 temperature;
    if (battery_service_ready &&
        R_SUCCEEDED(MCUHWC_ReadRegister(0x0A, &temperature, 1)) &&
        temperature <= 100) {
        battery_temperature_c = temperature;
    }
    // Retain the last valid reading. A failed read must not clear a thermal
    // playback lock until a fresh, safe temperature is actually measured.
}

static bool low_battery_warning_active(void) {
    // Match the percentage rounded down in the UI; use only cached readings.
    int displayed_percent = ((int)battery_percent / 5) * 5;
    return battery_percent >= 0 && displayed_percent <= 20 &&
           !(charging_state_known && battery_charging);
}

static bool thermal_warning_active(void) {
    return battery_temperature_c >= thermal_warning_threshold;
}

static void init_thermal_beep(void) {
    if (!audio_ready) return;
    thermal_beep_pcm = (int16_t *)linearAlloc(THERMAL_BEEP_FRAMES * sizeof(int16_t));
    if (!thermal_beep_pcm) return;

    float sine[64];
    for (unsigned i = 0; i < 64; i++)
        sine[i] = sinf((float)i * 6.2831853f / 64.0f);
    for (unsigned i = 0; i < THERMAL_BEEP_FRAMES; i++) {
        unsigned in_half_second = i % (THERMAL_BEEP_RATE / 2);
        int16_t sample = 0;
        if (in_half_second < 2600) {
            unsigned edge = in_half_second < 1300
                          ? in_half_second : 2599 - in_half_second;
            float fade = edge < 160 ? (float)edge / 160.0f : 1.0f;
            unsigned phase = (i * 880u * 64u / THERMAL_BEEP_RATE) % 64u;
            sample = (int16_t)(sine[phase] * 9000.0f * fade);
        }
        thermal_beep_pcm[i] = sample;
    }
    DSP_FlushDataCache(thermal_beep_pcm,
                       THERMAL_BEEP_FRAMES * sizeof(int16_t));
    ndspChnReset(THERMAL_BEEP_CHANNEL);
    ndspChnSetFormat(THERMAL_BEEP_CHANNEL, NDSP_FORMAT_MONO_PCM16);
    ndspChnSetRate(THERMAL_BEEP_CHANNEL, THERMAL_BEEP_RATE);
    ndspChnSetInterp(THERMAL_BEEP_CHANNEL, NDSP_INTERP_NONE);
    float mix[12] = {0};
    mix[0] = mix[1] = 0.60f;
    ndspChnSetMix(THERMAL_BEEP_CHANNEL, mix);
    ndspChnSetPaused(THERMAL_BEEP_CHANNEL, false);
}

static void play_thermal_beep(void) {
    if (!audio_ready || !thermal_beep_pcm) return;
    ndspChnWaveBufClear(THERMAL_BEEP_CHANNEL);
    memset(&thermal_beep_wave, 0, sizeof(thermal_beep_wave));
    thermal_beep_wave.data_vaddr = thermal_beep_pcm;
    thermal_beep_wave.nsamples = THERMAL_BEEP_FRAMES;
    ndspChnWaveBufAdd(THERMAL_BEEP_CHANNEL, &thermal_beep_wave);
}

static void update_sd_storage(void) {
    FS_ArchiveResource resource = {0};
    sd_storage_ready = fs_service_ready &&
        R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&resource)) &&
        resource.clusterSize > 0;
    if (sd_storage_ready) {
        sd_total_bytes = (u64)resource.clusterSize * resource.totalClusters;
        sd_free_bytes = (u64)resource.clusterSize * resource.freeClusters;
    }
}

// Boost the audible kick/bass band without lifting the entire low-mid range.
// Y cycles through five profiles; the filter itself runs on the DSP.
static void apply_tone(void) {
    static const float gains_db[] = { 6.0f, 9.0f, 16.0f, 22.0f };
    if (!bass_profile) {
        ndspChnIirBiquadSetEnable(CHANNEL, false);
        return;
    }
    const float gain_db = gains_db[(bass_profile - 1) % 4];
    const float A = powf(10.0f, gain_db / 40.0f);
    const float w0 = 6.28318530718f * 180.0f / NDSP_SAMPLE_RATE;
    const float c = cosf(w0), alpha = sinf(w0) / (2.0f * 0.8f);
    const float a0 = 1.0f + alpha / A;
    const float a1 = -2.0f * c;
    const float a2 = 1.0f - alpha / A;
    const float b0 = 1.0f + alpha * A;
    const float b1 = -2.0f * c;
    const float b2 = 1.0f - alpha * A;
    if (ndspChnIirBiquadSetParamsCustomFilter(CHANNEL, a0, a1, a2, b0, b1, b2))
        ndspChnIirBiquadSetEnable(CHANNEL, true);
}

static bool has_extension(const char *name, const char *extension) {
    const char *dot = strrchr(name, '.');
    if (!dot || strlen(dot) != strlen(extension)) return false;
    while (*dot && *extension) {
        char a = *dot++, b = *extension++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return !*dot && !*extension;
}

static bool is_audio_file(const char *name) {
    return has_extension(name, ".mp3") || has_extension(name, ".ogg");
}

static bool directory_contains_audio(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return false;
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (is_audio_file(entry->d_name)) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

static void stop_track(bool allow_sleep) {
    touch_seek_dragging = false;
    if (audio_ready) {
        ndspChnWaveBufClear(CHANNEL);
        ndspChnSetPaused(CHANNEL, false);
    }
    applied_output_rate = 0;
    target_output_rate = 0;
    boost_gain_q15 = 32768;
    last_queued_block = NULL;
    if (ogg_open) {
        ov_clear(&ogg_decoder); // also closes the FILE passed to ov_open
        ogg_open = false;
    } else if (audio_file) {
        fclose(audio_file);
    }
    audio_file = NULL;
    ogg_track = false;
    cover_valid = false;
    cover_load_pending = false;
    art_rotation = 0.0f;
    copy_name(current_title, sizeof(current_title), "Aucun morceau");
    current_artist[0] = current_album[0] = '\0';
    current_title_from_metadata = false;
    playback_status = "";
    playback_error = false;
    reset_reverb();
    reset_filter_state();
    playing = false;
    paused = false;
    aptSetSleepAllowed(allow_sleep);
    ui_dirty = true;
}

static void scan_folder(void) {
    track_count = 0;
    entry_count = 0;
    DIR *dir = opendir(folder);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && track_count < MAX_TRACKS) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[PATH_CAP + 256];
        snprintf(child, sizeof(child), "%s%s%s", folder,
                 folder[strlen(folder)-1] == '/' ? "" : "/", entry->d_name);
        struct stat st;
        bool is_directory = stat(child, &st) == 0 && S_ISDIR(st.st_mode);
        bool is_audio = is_audio_file(entry->d_name);
        if (is_directory && !directory_contains_audio(child)) continue;
        if ((is_directory || is_audio) && entry_count < MAX_TRACKS) {
            copy_name(entries[entry_count].name, sizeof(entries[entry_count].name), entry->d_name);
            entries[entry_count].directory = is_directory;
            entry_count++;
        }
        if (is_audio) {
            copy_name(tracks[track_count].name, sizeof(tracks[track_count].name), entry->d_name);
            tracks[track_count].duration_ms = 0;
            track_count++;
        }
    }
    closedir(dir);
}

static void enter_selected_directory(void) {
    if (selection < 0 || selection >= entry_count || !entries[selection].directory) return;
    size_t n = strlen(folder);
    if (n + strlen(entries[selection].name) + 2 >= sizeof(folder)) return;
    if (folder[n-1] != '/') strcat(folder, "/");
    strcat(folder, entries[selection].name);
    strcat(folder, "/");
    selection = 0;
    scan_folder();
}

static void parent_directory(void) {
    size_t n = strlen(folder);
    while (n > 0 && folder[n-1] == '/') folder[--n] = 0;
    while (n > 0 && folder[n-1] != '/') folder[--n] = 0;
    if (n == 0) strcpy(folder, "sdmc:/");
    selection = 0;
    scan_folder();
}

static void build_order(int first) {
    for (int i = 0; i < track_count; i++) order[i] = i;
    if (shuffle) {
        for (int i = track_count - 1; i > 0; i--) {
            int j = rand() % (i + 1), t = order[i]; order[i] = order[j]; order[j] = t;
        }
        if (first >= 0) {
            for (int i = 0; i < track_count; i++) if (order[i] == first) {
                int t = order[0]; order[0] = order[i]; order[i] = t; break;
            }
        }
    }
}

static bool queue_block(AudioBlock *b) {
    if (!b->pcm) return false;
    memset(&b->wave, 0, sizeof(b->wave));
    int frames = 0;
    if (ogg_track) {
        int read_errors = 0;
        while (frames < BLOCK_FRAMES) {
            int bitstream = 0;
            int remaining = BLOCK_FRAMES - frames;
            int frame_bytes = ogg_channels * (int)sizeof(int16_t);
            int request_bytes = remaining * frame_bytes;
            if (request_bytes > (int)sizeof(ogg_pcm))
                request_bytes = sizeof(ogg_pcm);
            long bytes = ov_read(&ogg_decoder, (char *)ogg_pcm, request_bytes, &bitstream);
            if (bytes < 0 && ++read_errors < 4) continue;
            if (bytes <= 0) {
                audio_eof = true;
                break;
            }
            read_errors = 0;
            if (bytes > request_bytes || bytes % frame_bytes) {
                audio_eof = true;
                break;
            }
            int count = (int)bytes / frame_bytes;
            if (count <= 0 || count > remaining) {
                audio_eof = true;
                break;
            }
            for (int i = 0; i < count; i++) {
                int16_t left = ogg_pcm[i * ogg_channels];
                int16_t right = ogg_channels == 1 ? left : ogg_pcm[i * 2 + 1];
                b->pcm[(frames + i) * 2] = left;
                b->pcm[(frames + i) * 2 + 1] = right;
            }
            frames += count;
            track_has_audio = true;
            decoded_frame_count += (u32)count;
        }
    } else {
    int skipped_bytes = 0;
    while (frames < BLOCK_FRAMES) {
        if (pending_pos < pending_frames) {
            int take = pending_frames - pending_pos;
            if (take > BLOCK_FRAMES - frames) take = BLOCK_FRAMES - frames;
            memcpy(b->pcm + frames * 2, pending_pcm + pending_pos * 2,
                   take * 2 * sizeof(int16_t));
            pending_pos += take;
            frames += take;
            continue;
        }
        pending_frames = pending_pos = 0;
        if (input_size < 4096 && !feof(audio_file)) {
            int got = (int)fread(input + input_size, 1, INPUT_CAP - input_size, audio_file);
            input_size += got;
        }
        if (input_size < 4) {
            if (feof(audio_file)) audio_eof = true;
            break;
        }
        mp3dec_frame_info_t info;
        int count = mp3dec_decode_frame(&decoder, input, input_size, decoded, &info);
        if (!track_duration_ms && count > 0) {
            track_duration_ms = mp3_xing_duration_ms(input, input_size, &info);
            if (!track_duration_ms && audio_payload_bytes && info.bitrate_kbps > 0)
                track_duration_ms = audio_payload_bytes * 8 / (u32)info.bitrate_kbps;
            if (track_duration_ms && order_pos >= 0 && order_pos < track_count)
                tracks[order[order_pos]].duration_ms = track_duration_ms;
        }
        if (info.frame_bytes <= 0 || info.frame_bytes > input_size) {
            memmove(input, input + 1, --input_size);
            if (++skipped_bytes > MP3_BAD_DATA_LIMIT) { audio_eof = true; break; }
            continue;
        }
        memmove(input, input + info.frame_bytes, input_size - info.frame_bytes);
        input_size -= info.frame_bytes;
        if (count <= 0 || (info.channels != 1 && info.channels != 2)) {
            skipped_bytes += info.frame_bytes;
            if (skipped_bytes > MP3_BAD_DATA_LIMIT) { audio_eof = true; break; }
            continue;
        }
        skipped_bytes = 0;
        track_has_audio = true;
        decoded_frame_count++;
        for (int i = 0; i < count; i++) {
            int left = info.channels == 1 ? decoded[i] : decoded[i*2];
            int right = info.channels == 1 ? left : decoded[i*2+1];
            pending_pcm[i*2] = (int16_t)left;
            pending_pcm[i*2+1] = (int16_t)right;
        }
        pending_frames = count;
        base_rate = info.hz;
        update_output_rate();
    }
    }
    if (!frames) {
        audio_eof = true;
        if (last_queued_block && last_queued_block->wave.status == NDSP_WBUF_QUEUED)
            fade_out_block(last_queued_block, last_queued_block->wave.nsamples);
        return false;
    }
    apply_filter(b->pcm, frames);
    apply_reverb(b->pcm, frames);
    apply_audio_boost(b->pcm, frames);
    fade_in_block(b, frames);
    if (audio_eof) fade_out_block(b, frames);
    DSP_FlushDataCache(b->pcm, frames * 2 * sizeof(int16_t));
    b->wave.data_vaddr = b->pcm;
    // NDSP's nsamples is the number of sample frames per channel for PCM.
    // Stereo data is interleaved in memory, but must not double this value.
    b->wave.nsamples = frames;
    b->wave.looping = false;
    ndspChnWaveBufAdd(CHANNEL, &b->wave);
    last_queued_block = b;
    return true;
}

static bool open_track(int pos) {
    if (thermal_warning_active()) {
        playback_status = "Lecture bloquee : temperature trop elevee.";
        playback_error = true;
        ui_dirty = true;
        return false;
    }
    // Keep sleep blocked across a track handoff: a closed 3DS may enter full
    // sleep during the brief gap before the next track is queued.
    stop_track(false);
    if (!audio_ready || pos < 0 || pos >= track_count) return false;
    order_pos = pos;
    playback_position_ms = 0;
    playback_rate_remainder = 0;
    playback_last_update_ms = 0;
    track_duration_ms = 0;
    audio_payload_bytes = 0;
    mp3_data_offset = 0;
    mp3_xing_bytes = 0;
    mp3_toc_valid = false;
    char path[PATH_CAP + 256];
    snprintf(path, sizeof(path), "%s%s%s", folder,
             folder[strlen(folder)-1] == '/' ? "" : "/", tracks[order[pos]].name);
    audio_file = fopen(path, "rb");
    if (!audio_file) {
        playback_status = "Impossible d'ouvrir ce fichier audio.";
        playback_error = true;
        aptSetSleepAllowed(true);
        return false;
    }
    copy_name(current_title, sizeof(current_title), tracks[order[pos]].name);
    current_artist[0] = current_album[0] = '\0';
    current_title_from_metadata = false;
    ogg_track = has_extension(tracks[order[pos]].name, ".ogg");
    art_rotation = 0.0f;
    last_art_vblank = C3D_FrameCounter(0);
    title_scroll_offset = 0;
    title_scroll_last_ms = osGetTime();
    if (!ogg_track) {
        audio_payload_bytes = get_payload_bytes(audio_file, &mp3_data_offset);
        cover_load_pending = true;
    }
    if (ogg_track) {
        if (ov_open(audio_file, &ogg_decoder, NULL, 0) != 0) {
            fclose(audio_file);
            audio_file = NULL;
            playback_status = "Fichier OGG/Vorbis invalide ou non pris en charge.";
            playback_error = true;
            aptSetSleepAllowed(true);
            return false;
        }
        ogg_open = true;
        vorbis_comment *comments = ov_comment(&ogg_decoder, -1);
        if (comments) {
            char *tag = vorbis_comment_query(comments, "TITLE", 0);
            if (tag && *tag) {
                copy_name(current_title, sizeof(current_title), tag);
                current_title_from_metadata = true;
            }
            tag = vorbis_comment_query(comments, "ARTIST", 0);
            if (tag) copy_name(current_artist, sizeof(current_artist), tag);
            tag = vorbis_comment_query(comments, "ALBUM", 0);
            if (tag) copy_name(current_album, sizeof(current_album), tag);
        }
        vorbis_info *info = ov_info(&ogg_decoder, -1);
        if (!info || (info->channels != 1 && info->channels != 2) ||
            info->rate < 8000 || info->rate > 48000) {
            stop_track(true);
            playback_status = "OGG invalide : mono/stereo et 8-48 kHz requis.";
            playback_error = true;
            return false;
        }
        ogg_channels = info->channels;
        base_rate = (int)info->rate;
        ogg_int64_t duration = ov_time_total(&ogg_decoder, -1);
        if (duration > 0) {
            track_duration_ms = (u64)duration;
            tracks[order[order_pos]].duration_ms = track_duration_ms;
        }
        update_output_rate();
    } else {
        if (mp3_data_offset > LONG_MAX ||
            fseek(audio_file, (long)mp3_data_offset, SEEK_SET) != 0) {
            stop_track(true);
            playback_status = "En-tete MP3 invalide ou trop grand.";
            playback_error = true;
            return false;
        }
        mp3dec_init(&decoder);
    }
    input_size = 0;
    pending_frames = pending_pos = 0;
    audio_eof = false;
    fade_in_remaining = -1;
    fade_in_total = 0;
    track_has_audio = false;
    decoded_frame_count = 0;
    ndspChnSetPaused(CHANNEL, true);
    for (int i = 0; i < AUDIO_BLOCK_COUNT && !audio_eof; i++)
        queue_block(&blocks[i]);
    playing = true;
    playback_last_update_ms = osGetTime();
    ndspChnSetPaused(CHANNEL, false);
    playback_status = "";
    playback_error = false;
    aptSetSleepAllowed(false);
    ui_dirty = true;
    return true;
}

static void start_folder(int first) {
    if (thermal_warning_active()) {
        playback_status = "Lecture bloquee : temperature trop elevee.";
        playback_error = true;
        ui_dirty = true;
        return;
    }
    stop_track(false);
    failed_tracks_in_a_row = 0;
    scan_folder();
    if (!track_count) {
        aptSetSleepAllowed(true);
        return;
    }
    build_order(first);
    open_track(0);
}

static void step_track(int direction) {
    if (!track_count) return;
    int previous_track = order_pos >= 0 && order_pos < track_count ?
                         order[order_pos] : -1;
    int next = order_pos + direction;
    if (next >= track_count || next < 0) {
        if (shuffle) {
            build_order(-1);
            int boundary = direction > 0 ? 0 : track_count - 1;
            if (track_count > 1 && order[boundary] == previous_track) {
                int other = direction > 0 ? 1 : track_count - 2;
                int saved = order[boundary];
                order[boundary] = order[other];
                order[other] = saved;
            }
        }
        next = direction > 0 ? 0 : track_count - 1;
    }
    open_track(next);
}

static u64 touch_target_ms(unsigned x) {
    if (x <= 14) return 0;
    if (x >= 306) return track_duration_ms;
    return (u64)(x - 14) * track_duration_ms / 292;
}

/* Seek only after a completed touch gesture. The audio queue is rebuilt from
 * the new decoder position while paused, then restored to its prior state. */
static bool seek_track(u64 target_ms) {
    if (!playing || !audio_file || !track_duration_ms) return false;
    if (target_ms >= track_duration_ms)
        target_ms = track_duration_ms > 1000 ? track_duration_ms - 1000 : 0;

    ndspChnSetPaused(CHANNEL, true);
    if (ogg_track) {
        if (ov_time_seek(&ogg_decoder, (ogg_int64_t)target_ms) != 0) {
            stop_track(true);
            playback_status = "Impossible de chercher dans cet OGG.";
            playback_error = true;
            ui_dirty = true;
            return false;
        }
    } else {
        if (!audio_payload_bytes) {
            ndspChnSetPaused(CHANNEL, paused);
            playback_last_update_ms = osGetTime();
            return false;
        }
        u64 position = audio_payload_bytes * target_ms / track_duration_ms;
        if (mp3_toc_valid && mp3_xing_bytes) {
            u64 scaled = target_ms * 100000 / track_duration_ms;
            unsigned index = (unsigned)(scaled / 1000);
            if (index > 99) index = 99;
            unsigned fraction = (unsigned)(scaled % 1000);
            unsigned first = mp3_xing_toc[index];
            unsigned second = index == 99 ? 256 : mp3_xing_toc[index + 1];
            if (second < first) second = first;
            position = mp3_xing_bytes * (first * 1000ULL +
                       (second - first) * (u64)fraction) / 256000ULL;
        }
        if (audio_payload_bytes > 4096 &&
            position > audio_payload_bytes - 4096)
            position = audio_payload_bytes - 4096;
        if (mp3_data_offset + position > LONG_MAX ||
            fseek(audio_file, (long)(mp3_data_offset + position), SEEK_SET) != 0) {
            ndspChnSetPaused(CHANNEL, paused);
            playback_last_update_ms = osGetTime();
            playback_status = "Impossible de chercher dans ce MP3.";
            ui_dirty = true;
            return false;
        }
        mp3dec_init(&decoder);
        input_size = pending_frames = pending_pos = 0;
    }

    ndspChnWaveBufClear(CHANNEL);
    last_queued_block = NULL;
    reset_reverb();
    reset_filter_state();
    boost_gain_q15 = 32768;
    audio_eof = false;
    fade_in_remaining = -1;
    track_has_audio = false;
    decoded_frame_count = 0;
    playback_position_ms = target_ms;
    playback_rate_remainder = 0;
    for (int i = 0; i < AUDIO_BLOCK_COUNT && !audio_eof; i++)
        queue_block(&blocks[i]);
    playback_last_update_ms = osGetTime();
    ndspChnSetPaused(CHANNEL, paused);
    playback_status = "Position modifiee.";
    playback_error = false;
    ui_dirty = true;
    return true;
}

static u32 ui_color(u8 r, u8 g, u8 b) {
    return C2D_Color32(r, g, b, 255);
}

/* Keep two decimals for values like 1.05x, but show the third only when the
 * 0.025x pitch step needs it (for example, 1.075x). */
static void format_pitch_ratio(char *out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = '\0';
    if (capacity < 7) return;
    int value = pitch_milli;
    if (value < 250) value = 250;
    if (value > 2000) value = 2000;
    unsigned whole = (unsigned)value / 1000;
    unsigned fraction = (unsigned)value % 1000;
    out[0] = (char)('0' + whole);
    out[1] = '.';
    out[2] = (char)('0' + fraction / 100);
    out[3] = (char)('0' + (fraction / 10) % 10);
    if (fraction % 10 == 0) {
        out[4] = 'x';
        out[5] = '\0';
    } else {
        out[4] = (char)('0' + fraction % 10);
        out[5] = 'x';
        out[6] = '\0';
    }
}

static const char *ui_localize(const char *value) {
    if (!value || ui_language.french) return value;
    static const struct { const char *fr; const char *en; } labels[] = {
        {"BIBLIOTHÃˆQUE", "LIBRARY"},
        {"L/R VERROUILLES", "L/R LOCKED"},
        {"EN PAUSE", "PAUSED"}, {"EN LECTURE", "PLAYING"},
        {"PRET A LIRE", "READY TO PLAY"},
        {"AUDIO INDISPONIBLE", "AUDIO UNAVAILABLE"},
        {"ERREUR DE LECTURE", "PLAYBACK ERROR"},
        {"BATTERIE FAIBLE", "LOW BATTERY"},
        {"SURCHAUFFE THERMIQUE", "OVERHEATING"},
        {"Reprendre", "Resume"}, {"Pause", "Pause"},
        {"Stop", "Stop"}, {"Piste", "Track"},
        {"Bass", "Bass"}, {"Reverb", "Reverb"}, {"Pitch", "Pitch"},
        {"Quitter", "Quit"}, {"Ouvrir / Lire", "Open / Play"},
        {"Retour", "Back"}, {"Lire le dossier", "Play folder"},
        {"Profil de reverb modifie", "Reverb preset changed"},
        {"Reverb desactivee", "Reverb off"},
        {"Basses EXTREME (+22 dB)", "Bass EXTREME (+22 dB)"},
        {"Basses +16 dB", "Bass +16 dB"},
        {"Basses BOOST (+9 dB)", "Bass BOOST (+9 dB)"},
        {"Basses DOUX (+6 dB)", "Bass SOFT (+6 dB)"},
        {"Basses OFF", "Bass OFF"},
        {"L/R verrouilles", "L/R locked"},
        {"L/R deverrouilles", "L/R unlocked"},
        {"Repetition piste activee", "Track repeat on"},
        {"Repetition piste desactivee", "Track repeat off"},
        {"REP PISTE ON", "TRACK REPEAT ON"},
        {"REP PISTE OFF", "TRACK REPEAT OFF"},
        {"Aucun morceau", "No track"},
        {"Chargement de la bibliotheque...", "Loading library..."},
        {"Circle Pad gauche : tour a droite verrouille, tour a gauche deverrouille",
         "Left Circle Pad: clockwise locks, counter-clockwise unlocks"},
        {"Aucun MP3 ou OGG lisible dans ce dossier.", "No playable MP3 or OGG files in this folder."},
        {"Aucun MP3 ou OGG dans ce dossier.", "No MP3 or OGG files in this folder."},
        {"Lecture arretee : surchauffe.", "Playback stopped: overheating."},
        {"AUDIO PRET", "AUDIO READY"},
        {"PREMIERS PAS", "QUICK START"},
        {"J'ai compris", "Got it"}, {"Continuer", "Continue"},
        {"On y va a ton rythme.", "Take it at your pace."}
    };
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i)
        if (!strcmp(value, labels[i].fr)) return labels[i].en;
    return value;
}

static void ui_rect(float x, float y, float w, float h, u32 color) {
    (void)C2D_DrawRectSolid(x, y, UI_DEPTH, w, h, color);
}

static void ui_circle(float x, float y, float radius, u32 color) {
    (void)C2D_DrawCircleSolid(x, y, UI_DEPTH, radius, color);
}

static void ui_round_rect(float x, float y, float w, float h, float r, u32 color) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    ui_rect(x + r, y, w - 2 * r, h, color);
    ui_rect(x, y + r, w, h - 2 * r, color);
    ui_circle(x + r, y + r, r, color);
    ui_circle(x + w - r, y + r, r, color);
    ui_circle(x + r, y + h - r, r, color);
    ui_circle(x + w - r, y + h - r, r, color);
}

static C2D_Font ui_text_font(C2D_TextBuf buffer, float scale) {
    if (buffer != ui_top_text_buffer) return ui_font;
    // The first-run guide uses the embedded high-resolution face at every
    // size; routing its small labels through the compact system font made
    // the guide noticeably less readable than the library screen.
    if (guide_drawing && ui_top_font) return ui_top_font;
    // Keep the 40 px embedded face for prominent headings only. Tiny metadata
    // drawn with it was being downscaled into cramped, jagged glyphs on hardware.
    if (scale >= 0.55f && ui_top_font) return ui_top_font;
    return ui_font ? ui_font : ui_top_font;
}

static float ui_text_render_scale(C2D_TextBuf buffer, float scale) {
    // The guide uses the embedded 40 px face directly. Its sizes are chosen
    // below to map to whole-pixel glyph heights on the upper screen.
    if (buffer == ui_top_text_buffer && guide_drawing && ui_top_font)
        return scale;
    // System-font labels keep their native scale; only the 40 px embedded face
    // gets the layout compensation used by the large upper-screen headings.
    if (buffer == ui_top_text_buffer && ui_top_font && scale >= 0.55f)
        return scale * (guide_page ? 0.60f : 0.80f);
    return scale;
}

static void ui_text(C2D_TextBuf buffer, const char *value, float x, float y,
                    float scale, u32 color, u32 alignment) {
    C2D_Text text;
    C2D_Font font = ui_text_font(buffer, scale);
    const float render_scale = ui_text_render_scale(buffer, scale);
    value = ui_localize(value);
    const char *parsed = font
        ? C2D_TextFontParse(&text, font, buffer, value)
        : C2D_TextParse(&text, buffer, value);
    if (parsed)
        C2D_DrawText(&text, alignment | C2D_WithColor, x, y, UI_DEPTH,
                     render_scale, render_scale, color);
}

static void ui_text_center(C2D_TextBuf buffer, const char *value,
                           float center_x, float y, float scale, u32 color) {
    ui_text(buffer, value, center_x, y, scale, color, C2D_AlignCenter);
}

static void ui_text_fit(C2D_TextBuf buffer, const char *value, float x, float y,
                        float max_width, float scale, u32 color) {
    value = ui_localize(value);
    char original[128], fitted[128];
    size_t source_length = strlen(value);
    bool source_truncated = source_length >= sizeof(original);
    if (source_length >= sizeof(original)) source_length = sizeof(original) - 1;
    memcpy(original, value, source_length);
    original[source_length] = '\0';
    size_t keep = source_truncated ? sizeof(original) - 4 : source_length;

    C2D_Text text;
    C2D_Font font = ui_text_font(buffer, scale);
    const float render_scale = ui_text_render_scale(buffer, scale);
    C2D_TextBuf measure_buffer = ui_fit_text_buffer ? ui_fit_text_buffer : buffer;
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        if (keep < source_length)
            while (keep && ((unsigned char)original[keep] & 0xc0) == 0x80)
                keep--;
        memcpy(fitted, original, keep);
        size_t length = keep;
        if (keep < source_length || source_truncated) {
            fitted[length++] = '.';
            fitted[length++] = '.';
            fitted[length++] = '.';
        }
        fitted[length] = '\0';
        if (measure_buffer != buffer) C2D_TextBufClear(measure_buffer);
        const char *parsed = font
            ? C2D_TextFontParse(&text, font, measure_buffer, fitted)
            : C2D_TextParse(&text, measure_buffer, fitted);
        if (!parsed) return;
        float width = 0.0f;
        C2D_TextGetDimensions(&text, render_scale, render_scale, &width, NULL);
        if (width <= max_width || keep <= 3) {
            if (measure_buffer != buffer) {
                parsed = font
                    ? C2D_TextFontParse(&text, font, buffer, fitted)
                    : C2D_TextParse(&text, buffer, fitted);
                if (!parsed) return;
            }
            C2D_DrawText(&text, C2D_WithColor, x, y, UI_DEPTH,
                         render_scale, render_scale, color);
            return;
        }

        float estimate = (float)keep * max_width / width;
        size_t next = estimate > 4.0f ? (size_t)estimate - 1 : 3;
        if (next >= keep) next = keep - 1;
        keep = next;
    }
}

static void ui_line(float x1, float y1, float x2, float y2,
                    float thickness, u32 color) {
    (void)C2D_DrawLine(x1, y1, color, x2, y2, color, thickness, UI_DEPTH);
}

static void ui_note(float x, float y, float s, u32 color) {
    ui_line(x + 5*s, y + 3*s, x + 5*s, y + 13*s, 1.6f*s, color);
    ui_line(x + 13*s, y + 1*s, x + 13*s, y + 11*s, 1.6f*s, color);
    ui_line(x + 5*s, y + 3*s, x + 13*s, y + 1*s, 2.5f*s, color);
    ui_circle(x + 3*s, y + 13*s, 2.5f*s, color);
    ui_circle(x + 11*s, y + 11*s, 2.5f*s, color);
}

static void ui_effect_icon(int kind, float x, float y, u32 color) {
    if (kind == 0) {
        ui_line(x, y+3, x+4, y+3, 1.6f, color);
        ui_line(x+4, y+3, x+11, y+12, 1.6f, color);
        ui_line(x+11, y+12, x+16, y+12, 1.6f, color);
        ui_line(x, y+12, x+4, y+12, 1.6f, color);
        ui_line(x+4, y+12, x+11, y+3, 1.6f, color);
        ui_line(x+11, y+3, x+16, y+3, 1.6f, color);
        for (int i=0; i<2; i++) {
            float a = y+3+i*9;
            ui_line(x+13, a-3, x+16, a, 1.6f, color);
            ui_line(x+13, a+3, x+16, a, 1.6f, color);
        }
    } else if (kind == 1) {
        const int h[5] = {6,12,18,12,6};
        for (int i=0; i<5; i++) ui_rect(x+i*3, y+8-h[i]/2, 1.8f, h[i], color);
    } else if (kind == 2) {
        const int levels[4] = {3,6,5,2};
        for (int i=0; i<4; i++) for (int j=0; j<levels[i]; j++)
            ui_rect(x+i*4, y+15-j*3, 2.5f, 2, color);
    } else {
        const float dx[12]={0,3.5f,6,7,6,3.5f,0,-3.5f,-6,-7,-6,-3.5f};
        const float dy[12]={-7,-6,-3.5f,0,3.5f,6,7,6,3.5f,0,-3.5f,-6};
        for(int i=0;i<12;i++) ui_circle(x+8+dx[i],y+8+dy[i],1.0f,color);
    }
}

static void ui_key(C2D_TextBuf buf, const char *key, const char *label,
                   float x, float y, float key_width) {
    const u32 white=ui_color(232,240,248), bg=ui_color(7,16,29);
    if (strlen(key)==1) {
        ui_circle(x+6,y+6,6,white);
        ui_text_center(buf,key,x+6,y,0.34f,bg);
    } else {
        ui_round_rect(x,y,key_width,12,2,ui_color(145,168,191));
        ui_round_rect(x+1,y+1,key_width-2,10,1,bg);
        ui_text_center(buf,key,x+key_width/2,y+1,0.27f,white);
    }
    ui_text(buf,label,x+key_width+4,y,0.32f,white,C2D_AlignLeft);
}

static void guide_load(void) {
    char marker[sizeof(guide_marker)] = {0};
    FILE *file = fopen(GUIDE_FILE,"rb");
    bool seen = false;
    if (file) {
        seen = fread(marker,1,sizeof(guide_marker)-1,file)==sizeof(guide_marker)-1 &&
               !memcmp(marker,guide_marker,sizeof(guide_marker)-1);
        fclose(file);
    }
    guide_page = seen ? 0 : 1;
}

static void guide_complete(void) {
    // Write only after the user has acknowledged page 3, never on startup.
    (void)mkdir("sdmc:/3ds",0777);
    (void)mkdir("sdmc:/3ds/ShieldDJStandard",0777);
    FILE *file = fopen(GUIDE_FILE,"wb");
    if (file) {
        (void)fwrite(guide_marker,1,sizeof(guide_marker)-1,file);
        (void)fclose(file);
    }
    guide_page = 0;
}

static void guide_draw_key(const char *key, const char *label, float x, float y) {
    const u32 white=ui_color(232,240,248), bg=ui_color(5,17,28);
    ui_circle(x+8,y+8,8,white);
    ui_text_center(ui_top_text_buffer,key,x+8,y+2,0.40f,bg);
    ui_text(ui_top_text_buffer,label,x+21,y+1,0.45f,white,C2D_AlignLeft);
}

static void draw_guide(void) {
    guide_drawing=true;
    C2D_TextBuf buf=ui_top_text_buffer;
    const u32 bg=ui_color(5,17,28), cyan=ui_color(41,190,219);
    const u32 white=ui_color(232,240,248), secondary=ui_color(145,168,191);
    const char *titles[3];
    const char *keys[3][4];
    const char *lines[3][4];
    if (ui_language.french) {
        const char *t[3]={"Bienvenue dans SHIELD DJ","Les commandes de lecture","Effets et navigation"};
        const char *k[3][4]={{"FORMATS","STANDARD","LITE","SD"},
                             {"A","B","L / R","Circle Pad"},
                             {"Circle Pad","Y / SELECT","X","Pitch"}};
        const char *l[3][4]={
            {"MP3 / OGG Vorbis","Interface soignee, console stable.","Plus leger, pour consoles fatiguees.","Fichiers audio dans des dossiers SD."},
            {"Lire / pause / reprendre.","Stopper / revenir en arriere.","Morceau precedent / suivant.","Pad gauche : tour droite verrouille."},
            {"Tour gauche : deverrouille L/R.","Options : filtres, reverb prolongee.","Aleatoire liste / repetition piste.","G/D pitch; haut/bas luminosite."}
        };
        memcpy(titles,t,sizeof(t)); memcpy(keys,k,sizeof(k)); memcpy(lines,l,sizeof(l));
    } else {
        const char *t[3]={"Welcome to SHIELD DJ","Playback controls","Effects and navigation"};
        const char *k[3][4]={{"FORMATS","STANDARD","LITE","SD"},
                             {"A","B","L / R","Circle Pad"},
                             {"Circle Pad","Y / SELECT","X","Pitch"}};
        const char *l[3][4]={
            {"MP3 / OGG Vorbis","Polished UI, healthy console.","Lightweight for older consoles.","Audio files go in SD folders."},
            {"Play / pause / resume.","Stop / return to library.","Previous / next track.","Left Circle Pad: clockwise locks."},
            {"Counter-clockwise unlocks L/R.","Options: filters, long reverb.","Shuffle library / repeat track.","L/R pitch; up/down brightness."}
        };
        memcpy(titles,t,sizeof(t)); memcpy(keys,k,sizeof(k)); memcpy(lines,l,sizeof(l));
    }
    unsigned page=guide_page-1;
    ui_round_rect(6,6,388,228,7,ui_color(40,83,112));
    ui_round_rect(7,7,386,226,6,bg);
    ui_text(buf,ui_language.french ? "PREMIERS PAS" : "GETTING STARTED",21,14,0.45f,cyan,C2D_AlignLeft);
    char counter[8]; snprintf(counter,sizeof(counter),"%u/3",guide_page);
    ui_text(buf,counter,378,14,0.45f,secondary,C2D_AlignRight);
    ui_text_fit(buf,titles[page],21,36,358,0.60f,white);
    ui_rect(21,64,358,0.8f,ui_color(40,83,112));
    for(unsigned row=0;row<4;row++) {
        float y=75+row*30;
        if(page==2 && row>0) y+=8;
        const bool right_gesture = page == 1 && row == 3;
        const bool left_gesture = page == 2 && row == 0;
        float label_width=(float)strlen(keys[page][row])*9.5f;
        if(label_width<28.0f) label_width=28.0f;
        if(label_width>108.0f) label_width=108.0f;
        float body_x=23.0f+label_width+8.0f;
        float body_width=379.0f-body_x;
        if (right_gesture) {
            ui_text_fit(buf,keys[page][row],23,y,label_width,0.40f,cyan);
            ui_text_fit(buf,ui_language.french
                ? "Droite : cercle pour verrouiller L/R."
                : "Right: circle once to lock L/R.",
                body_x,y,body_width,0.45f,white);
            ui_text_fit(buf,ui_language.french
                ? "Parfois deux tours; geste encore en dev."
                : "Sometimes two turns; still in development.",
                23,y+18,354,0.40f,secondary);
        } else if (left_gesture) {
            ui_text_fit(buf,keys[page][row],23,y,label_width,0.40f,cyan);
            ui_text_fit(buf,ui_language.french
                ? "Gauche : cercle deverrouille L/R."
                : "Left: circle once to unlock L/R.",
                body_x,y,body_width,0.45f,white);
            ui_text_fit(buf,ui_language.french
                ? "Parfois deux tours; recentre le pad apres."
                : "Sometimes twice; recenter after.",
                23,y+18,354,0.40f,secondary);
        } else {
            ui_text_fit(buf,keys[page][row],23,y,label_width,0.40f,cyan);
            ui_text_fit(buf,lines[page][row],body_x,y,body_width,0.45f,white);
        }
    }
    ui_rect(21,203,358,0.8f,ui_color(40,83,112));
    if (guide_page > 1)
        guide_draw_key("B",ui_language.french ? "Page precedente" : "Previous page",23,209);
    guide_draw_key("A",guide_page==3
        ? (ui_language.french ? "J'ai compris" : "Got it")
        : (ui_language.french ? "Continuer" : "Continue"),253,209);
    guide_drawing=false;
}

static void draw_safety_overlay(void) {
    // Painted last on the top screen, including over the first-use guide.
    bool thermal = thermal_warning_active();
    if (!thermal && !low_battery_warning_active()) return;

    const u32 red = warning_blink_on ? ui_color(255, 66, 72)
                                     : ui_color(168, 38, 46);
    const u32 panel = ui_color(25, 8, 17);
    if (thermal) {
        ui_rect(10, 79, 380, 83, red);
        ui_rect(13, 82, 374, 77, panel);
        ui_text_center(ui_top_text_buffer, "DANGER SURCHAUFFE", 200, 90,
                       0.61f, red);
        ui_text_center(ui_top_text_buffer, "THERMIQUE", 200, 113,
                       0.68f, red);
        ui_text_center(ui_top_text_buffer, "ETEINDRE LA CONSOLE", 200, 137,
                       0.61f, red);
    } else {
        ui_rect(19, 92, 362, 57, red);
        ui_rect(22, 95, 356, 51, panel);
        ui_text_center(ui_top_text_buffer, "!BATTERIE FAIBLE!", 200, 109,
                       0.70f, red);
    }
}

static void draw_album_art(void) {
    const u32 near_black = ui_color(7, 16, 29);
    const u32 disc_dark = ui_color(9, 25, 40);
    const u32 disc_mid = ui_color(22, 49, 69);
    const u32 cyan = ui_color(41, 190, 219);

    ui_circle(82.0f, 119.0f, 66.0f, ui_color(30, 62, 84));
    ui_circle(82.0f, 119.0f, 64.5f, cyan);
    ui_circle(82.0f, 119.0f, 63.0f, near_black);
    // Keep the vinyl underneath the artwork. Its outer rim remains visible.
    ui_circle(82,119,61,disc_dark);
    for (int i=0;i<3;i++) {
        ui_circle(82,119,55-i*7,disc_mid);
        ui_circle(82,119,54-i*7,disc_dark);
    }
    ui_circle(82,119,40,cyan);
    ui_circle(82,119,39,disc_mid);
    if (cover_valid && cover_texture_ready) {
        (void)C2D_DrawImageAtRotated(cover_image, 82.0f, 119.0f, UI_DEPTH,
                                     art_rotation, NULL, 0.97f, 0.97f);
    } else {
        // Reuse the transparent SHIELD asset already loaded for the header.
        // It fits inside the record label and follows the same rotation.
        if (logo_ready)
            (void)C2D_DrawImageAtRotated(logo_image, 82.0f, 119.0f,
                                         UI_DEPTH, art_rotation, NULL, 1.15f, 1.15f);
    }
    ui_circle(82,119,4.5f,ui_color(145,168,191));
    ui_circle(82,119,3,near_black);
}

static void update_wall_clock(u64 now_ms) {
    if (now_ms < clock_next_update_ms) return;

    time_t wall_time = time(NULL);
    char next_text[sizeof(clock_text)] = "--:--";
    u64 delay_ms = 60000;
    if (wall_time >= 0) {
        struct tm *local_time = localtime(&wall_time);
        if (local_time)
            snprintf(next_text, sizeof(next_text), "%02d:%02d",
                     local_time->tm_hour, local_time->tm_min);
        delay_ms = (u64)(60 - (wall_time % 60)) * 1000;
    }
    if (strcmp(clock_text, next_text) != 0) {
        memcpy(clock_text, next_text, sizeof(clock_text));
        time_dirty = true;
    }
    clock_next_update_ms = now_ms + delay_ms;
}

static void draw_top_ui(void) {
    if (!ui_renderer_ready) return;
    C2D_TextBuf buf = ui_top_text_buffer;
    C2D_TextBufClear(buf);
    C2D_TargetClear(ui_top_target, ui_color(7, 16, 29));
    C2D_SceneBegin(ui_top_target);
    if (guide_page) { draw_guide(); draw_safety_overlay(); return; }

    const u32 bg = ui_color(7, 16, 29);
    const u32 panel = ui_color(5, 17, 28);
    const u32 border = ui_color(40, 83, 112);
    const u32 cyan = ui_color(41, 190, 219);
    const u32 white = ui_color(232, 240, 248);
    const u32 secondary = ui_color(180, 207, 228);

    ui_round_rect(3,3,394,234,8,border);
    ui_round_rect(4,4,392,232,7,ui_color(12,30,46));
    ui_round_rect(8,8,384,224,5,panel);
    ui_rect(8,42,384,1,cyan);

    if (logo_ready)
        (void)C2D_DrawImageAt(logo_image,13,7,UI_DEPTH,NULL,0.55f,0.55f);

    // Battery and clock stay in the header, as shown in the supplied layout.
    if (battery_percent >= 0.0f) {
        char battery[16];
        snprintf(battery, sizeof(battery), "%d%%", ((int)battery_percent / 5) * 5);
        ui_rect(293, 20, 24, 13, cyan);
        ui_rect(296, 23, 18, 7, bg);
        ui_rect(298, 24, (int)(14 * battery_percent / 100.0f), 5, cyan);
        ui_rect(318, 23, 3, 7, cyan);
        ui_text(buf, battery, 326, 18, 0.48f, white, C2D_AlignLeft);
    }
    ui_rect(354, 17, 1, 18, border);
    ui_text(buf, clock_text, 384, 18, 0.48f, white, C2D_AlignRight);

    C2D_Text header_text;
    C2D_Font header_font = ui_text_font(buf, 0.70f);
    const float header_scale = ui_text_render_scale(buf, 0.70f);
    const char *header_parsed = header_font
        ? C2D_TextFontParse(&header_text, header_font, buf, "SHIELD")
        : C2D_TextParse(&header_text, buf, "SHIELD");
    float shield_width = 62.0f;
    if (header_parsed) {
        C2D_TextGetDimensions(&header_text, header_scale, header_scale,
                              &shield_width, NULL);
        C2D_DrawText(&header_text, C2D_AlignLeft | C2D_WithColor,
                     51, 8, UI_DEPTH, header_scale, header_scale, white);
    }
    ui_text(buf, "~ DJ", 51 + shield_width + 3, 8, 0.70f, cyan, C2D_AlignLeft);
    ui_text(buf, "STANDARD / by Shield-77 v1.0.36", 52, 28, 0.45f, secondary, C2D_AlignLeft);
    if (track_skip_locked)
        ui_text(buf, "L/R VERROUILLES", 254, 31, 0.42f,
                ui_color(255, 209, 72), C2D_AlignLeft);

    ui_note(164,57,0.8f,cyan);
    ui_text(buf, playing ? (paused ? "EN PAUSE" : "EN LECTURE") : "PRET A LIRE",
            184, 55, 0.50f, cyan, C2D_AlignLeft);

    char title[sizeof(current_title)];
    size_t src = 0, dst = 0;
    while (current_title[src] && dst + 1 < sizeof(title)) {
        unsigned char c = (unsigned char)current_title[src++];
        if (c >= 0x80) {
            while (((unsigned char)current_title[src] & 0xC0) == 0x80) src++;
            title[dst++] = '?';
        } else if (c != '\n' && c != '\r') {
            title[dst++] = (char)c;
        }
    }
    title[dst] = '\0';
    if (!current_title_from_metadata) {
        char *dot = strrchr(title, '.');
        if (dot && (!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".ogg")))
            *dot = '\0';
    }
    display_title_chars = strlen(title);
    const unsigned title_width = 27;
    char title_window[title_width + 1];
    if (display_title_chars > title_width) {
        size_t loop_length = display_title_chars + 3;
        title_scroll_offset %= loop_length;
        for (size_t i = 0; i < title_width; i++) {
            size_t at = (title_scroll_offset + i) % loop_length;
            title_window[i] = at < display_title_chars ? title[at] : ' ';
        }
        title_window[title_width] = '\0';
        ui_text_fit(buf, title_window, 164, 75, 218.0f, 0.69f, white);
    } else {
        title_scroll_offset = 0;
        ui_text_fit(buf, title, 164, 75, 218.0f, 0.69f, white);
    }
    ui_text_fit(buf, current_artist[0] ? current_artist : (ogg_track ? "OGG VORBIS" : "MP3 AUDIO"),
                164, 102, 218.0f, 0.49f, secondary);
    if (current_album[0])
        ui_text_fit(buf, current_album, 164, 121, 218.0f, 0.46f, secondary);

    if (!audio_ready)
        ui_text(buf, "AUDIO INDISPONIBLE", 164, 138, 0.46f,
                ui_color(255, 90, 95), C2D_AlignLeft);
    else if (playback_error)
        ui_text(buf, "ERREUR DE LECTURE", 164, 138, 0.46f,
                ui_color(255, 90, 95), C2D_AlignLeft);
    else if (battery_percent >= 0 && battery_percent <= 20 &&
             charging_state_known && !battery_charging)
        ui_text(buf, "BATTERIE FAIBLE", 164, 138, 0.46f,
                ui_color(255, 77, 86), C2D_AlignLeft);
    else if (battery_temperature_c >= thermal_warning_threshold)
        ui_text(buf, "SURCHAUFFE THERMIQUE", 164, 138, 0.46f,
                ui_color(255, 65, 70), C2D_AlignLeft);
    else if (playback_status[0])
        ui_text_fit(buf, playback_status, 164, 138, 218, 0.46f, secondary);
    else
        ui_text(buf, repeat_current ? "REP PISTE ON" : "REP PISTE OFF",
                164, 138, 0.46f, repeat_current ? cyan : secondary,
                C2D_AlignLeft);

    ui_round_rect(164,160,218,6,3,ui_color(25,48,65));
    if (track_duration_ms) {
        u64 position = playback_position_ms > track_duration_ms
                       ? track_duration_ms : playback_position_ms;
        int width = (int)(218 * position / track_duration_ms);
        if (width > 0) ui_round_rect(164,160,(float)width,6,3,cyan);
    }
    char elapsed[16], duration[16];
    format_track_time(elapsed, sizeof(elapsed), playback_position_ms);
    if (track_duration_ms) format_track_time(duration, sizeof(duration), track_duration_ms);
    else strcpy(duration, "--:--");
    ui_text(buf, elapsed, 164, 169, 0.48f, white, C2D_AlignLeft);
    ui_text(buf, duration, 382, 169, 0.48f, white, C2D_AlignRight);

    ui_rect(14,187,372,1,border);
    const char *bass = bass_profile == 4 ? "+22 dB" : bass_profile == 3 ? "+16 dB" : bass_profile == 2 ? "+9 dB" :
                       bass_profile == 1 ? "+6 dB" : "OFF";
    char controls[4][24];
    snprintf(controls[0], sizeof(controls[0]), "Shuffle %s", shuffle ? "ON" : "OFF");
    char pitch_label[16];
    format_pitch_ratio(pitch_label, sizeof(pitch_label));
    snprintf(controls[1], sizeof(controls[1]), "Pitch %s", pitch_label);
    snprintf(controls[2], sizeof(controls[2]), "Bass %s", bass);
    const char *reverb_label_fr[] = {"OFF","Concert","Cathed.","Extra"};
    const char *reverb_label_en[] = {"OFF","Concert","Cathedral","Extra"};
    const char **reverb_label = ui_language.french ? reverb_label_fr : reverb_label_en;
    if (reverb_custom_enabled)
        snprintf(controls[3], sizeof(controls[3]), "Rev %u%%", reverb_custom_percent);
    else
        snprintf(controls[3], sizeof(controls[3]), "Rev %s", reverb_label[reverb_mode]);
    for (int i = 0; i < 4; i++) {
        float x=16+i*95;
        if (i) ui_rect(x-6,194,0.7f,19,border);
        ui_effect_icon(i,x,195,i<2?cyan:white);
        ui_text_fit(buf,controls[i],x+21,196,71,0.41f,white);
    }

    if (sd_storage_ready) {
        const u64 gb = 1024ULL * 1024ULL * 1024ULL;
        u64 free100 = sd_free_bytes * 100 / gb;
        u64 total100 = sd_total_bytes * 100 / gb;
        char storage[48];
        snprintf(storage, sizeof(storage), ui_language.french
                 ? "SD  %llu.%02llu / %llu.%02llu Go"
                 : "SD  %llu.%02llu / %llu.%02llu GB",
                 (unsigned long long)(free100 / 100),
                 (unsigned long long)(free100 % 100),
                 (unsigned long long)(total100 / 100),
                 (unsigned long long)(total100 % 100));
        ui_text_fit(buf, storage, 16, 217, 300, 0.48f, white);
    }
    char light[12];
    snprintf(light, sizeof(light), ui_language.french ? "LUM %d/5" : "BRI %d/5",
             brightness_level);
    ui_text(buf, light, 384, 217, 0.45f, secondary, C2D_AlignRight);

    // Keep the small set of circular GPU primitives together after rectangles
    // and text, so Citro2D does not keep switching shape modes during the UI.
    draw_album_art();
    draw_safety_overlay();
}

static void draw_file_icon(bool directory, float x, float y, u32 color, u32 bg) {
    if (directory) {
        ui_round_rect(x,y+2,7,5,1,color);
        ui_round_rect(x,y+4,14,10,1.5f,color);
        ui_round_rect(x+1.3f,y+5.5f,11.4f,7,0.5f,bg);
    } else {
        ui_note(x,y,0.85f,color);
    }
}

static u64 cached_track_duration(const char *name) {
    for (int i = 0; i < track_count; i++)
        if (!strcmp(tracks[i].name, name)) return tracks[i].duration_ms;
    return 0;
}

static void make_ui_label(char *out, size_t capacity, const char *source, size_t max_chars) {
    size_t src = 0, dst = 0, chars = 0, source_length = strlen(source);
    while (source[src] && chars < max_chars && dst + 1 < capacity) {
        unsigned char c = (unsigned char)source[src++];
        if (c >= 0x80) {
            unsigned continuation = (c & 0xe0) == 0xc0 ? 1 :
                                   (c & 0xf0) == 0xe0 ? 2 :
                                   (c & 0xf8) == 0xf0 ? 3 : 0;
            while (continuation-- && source[src] &&
                   ((unsigned char)source[src] & 0xc0) == 0x80) src++;
            out[dst++] = '?';
        } else out[dst++] = (char)c;
        chars++;
    }
    if (src < source_length && max_chars >= 3 && dst + 3 < capacity) {
        while (chars >= max_chars - 3 && dst) { dst--; chars--; }
        out[dst++] = '.'; out[dst++] = '.'; out[dst++] = '.';
    }
    out[dst] = '\0';
}

static void draw_options_ui(C2D_TextBuf buf) {
    const u32 cyan = ui_color(41,190,219);
    const u32 white = ui_color(232,240,248);
    const u32 secondary = ui_color(145,168,191);
    ui_text(buf,"OPTIONS",17,13,0.58f,cyan,C2D_AlignLeft);
    ui_text(buf,ui_language.french ? "FILTRES AUDIO" : "AUDIO FILTERS",
            17,35,0.38f,secondary,C2D_AlignLeft);
    ui_rect(14,52,292,1,ui_color(40,83,112));
    for (unsigned i=0; i<7; i++) {
        float x = 13.0f + (i % 2) * 148.0f;
        float y = 57.0f + (i / 2) * 21.0f;
        bool selected = i == options_selection;
        if (selected) {
            ui_round_rect(x,y,143,19,3,ui_color(9,145,179));
            ui_round_rect(x,y,3,19,1,cyan);
        }
        ui_text_fit(buf,ui_language.french ? filter_names_fr[i] : filter_names_en[i],
                    x+10,y+1,101,0.40f,selected?white:secondary);
        if (i == filter_profile)
            ui_text(buf,"ON",x+136,y+2,0.32f,white,C2D_AlignRight);
    }
    ui_round_rect(13,147,145,21,3,options_selection == 7
                  ? ui_color(9,145,179) : ui_color(12,30,46));
    ui_round_rect(161,147,146,21,3,options_selection == 8
                  ? ui_color(9,145,179) : ui_color(12,30,46));
    ui_text_fit(buf,ui_language.french ? "REV LIBRE" : "CUSTOM REV",
                19,151,87,0.36f,cyan);
    ui_text(buf,reverb_custom_enabled ? "ON" : "OFF",
            151,151,0.36f,white,C2D_AlignRight);
    ui_text_fit(buf,ui_language.french ? "PROLONGEE" : "LONG TAIL",
                167,151,93,0.36f,cyan);
    ui_text(buf,reverb_long_enabled ? "ON" : "OFF",
            299,151,0.36f,white,C2D_AlignRight);
    unsigned shown = touch_reverb_dragging ? touch_reverb_preview :
                     reverb_custom_enabled ? reverb_custom_percent : reverb_mode;
    if (reverb_custom_enabled) {
        char amount[32];
        snprintf(amount,sizeof(amount),ui_language.french ? "Intensite %u%%" : "Intensity %u%%",shown);
        ui_text_fit(buf,amount,22,171,276,0.36f,secondary);
        ui_round_rect(22,190,276,10,4,ui_color(24,48,68));
        if (shown) ui_round_rect(22,190,276.0f*shown/100,10,4,cyan);
        ui_circle(22+276.0f*shown/100,195,7,white);
    } else {
        for (unsigned i=0; i<4; i++) {
            float x=22.0f+i*69.0f;
            ui_text_fit(buf,reverb_names[i],x+2,171,64,0.32f,secondary);
            ui_round_rect(x,190,67,10,3,i==shown?cyan:ui_color(24,48,68));
        }
    }
    ui_rect(14,210,292,1,ui_color(40,83,112));
    ui_text_fit(buf,ui_language.french
        ? "A Choisir   B/SELECT Fermer"
        : "A Select   B/SELECT Close",
        16,215,286,0.34f,white);
}

static void options_activate(void) {
    if (options_selection == 7)
        reverb_custom_enabled = !reverb_custom_enabled;
    else if (options_selection == 8)
        reverb_long_enabled = !reverb_long_enabled;
    else filter_profile = options_selection;
    ui_dirty = true;
}

static void draw_bottom_ui(void) {
    if (!ui_renderer_ready) return;
    C2D_TextBuf buf = ui_bottom_text_buffer;
    C2D_TextBufClear(buf);
    C2D_TargetClear(ui_bottom_target, ui_color(7, 16, 29));
    C2D_SceneBegin(ui_bottom_target);

    const u32 panel = ui_color(5, 17, 28);
    const u32 border = ui_color(40, 83, 112);
    const u32 cyan = ui_color(41, 190, 219);
    const u32 white = ui_color(232, 240, 248);
    const u32 secondary = ui_color(145, 168, 191);
    ui_round_rect(3,3,314,234,7,border);
    ui_round_rect(4,4,312,232,6,ui_color(12,30,46));
    ui_round_rect(8,8,304,224,4,panel);

    if (options_open) {
        draw_options_ui(buf);
        return;
    }

    // Folder icon and compact library header.
    draw_file_icon(true,16,12,cyan,panel);
    ui_text(buf, "BIBLIOTHÈQUE", 38, 11, 0.54f, cyan, C2D_AlignLeft);
    char count[20];
    char path[PATH_CAP];
    make_ui_label(path, sizeof(path), folder, 43);
    ui_text_fit(buf, path, 16, 32, 239, 0.35f, secondary);
    ui_rect(13,48,294,0.8f,border);

    int current_index = selection;
    if (playing && order_pos >= 0 && order_pos < track_count) {
        const char *playing_name = tracks[order[order_pos]].name;
        for (int i = 0; i < entry_count; i++)
            if (!entries[i].directory && !strcmp(entries[i].name, playing_name)) {
                current_index = i;
                break;
            }
    }
    int first = current_index > 2 ? current_index - 2 : 0;
    snprintf(count,sizeof(count),"%d/%d",entry_count?current_index+1:0,entry_count);
    ui_text(buf,count,303,32,0.35f,secondary,C2D_AlignRight);
    if (first + 5 > entry_count) first = entry_count > 5 ? entry_count - 5 : 0;
    if (playing) {
        ui_text(buf, ui_language.french ? "EN LECTURE" : "NOW PLAYING",
                16,54,0.38f,cyan,C2D_AlignLeft);
        ui_text_fit(buf,current_title,16,68,288,0.48f,white);
        ui_text(buf, ui_language.french ? "SUIVANT" : "UP NEXT",
                16,91,0.35f,cyan,C2D_AlignLeft);
        const char *next_name = "";
        if (order_pos >= 0 && order_pos < track_count) {
            if (repeat_current) next_name = current_title;
            else if (order_pos + 1 < track_count)
                next_name = tracks[order[order_pos + 1]].name;
            else if (shuffle)
                next_name = ui_language.french ? "Selection aleatoire" : "Random track";
            else if (track_count > 0) next_name = tracks[order[0]].name;
        }
        ui_text_fit(buf,next_name,16,104,288,0.38f,secondary);

        u64 shown_position = touch_seek_dragging ? touch_seek_target_ms :
                                                  playback_position_ms;
        if (track_duration_ms && shown_position > track_duration_ms)
            shown_position = track_duration_ms;
        ui_round_rect(14,127,292,9,4,ui_color(24,48,68));
        float progress_width = track_duration_ms
            ? 292.0f * (float)shown_position / (float)track_duration_ms : 0.0f;
        if (progress_width > 0.0f)
            ui_round_rect(14,127,progress_width,9,4,cyan);
        char elapsed[16], duration[16], percent[12];
        format_track_time(elapsed,sizeof(elapsed),shown_position);
        if (track_duration_ms) format_track_time(duration,sizeof(duration),track_duration_ms);
        else strcpy(duration,"--:--");
        if (track_duration_ms)
            snprintf(percent,sizeof(percent),"%u%%",
                     (unsigned)(shown_position * 100 / track_duration_ms));
        else strcpy(percent,"--%");
        ui_text(buf,elapsed,14,140,0.34f,white,C2D_AlignLeft);
        ui_text(buf,percent,160,140,0.34f,secondary,C2D_AlignCenter);
        ui_text(buf,duration,306,140,0.34f,white,C2D_AlignRight);
        ui_text_fit(buf,ui_language.french
            ? "Glisser puis relacher pour choisir la position"
            : "Drag, then release to seek",16,154,292,0.31f,secondary);
    }
    for (int row = 0; !playing && row < 5 && first + row < entry_count; row++) {
        int index = first + row;
        float y = 54.0f + row * 22.0f;
        bool active = playing ? index == current_index : index == selection;
        u32 fg = white;
        u32 icon = active ? white : secondary;
        u32 selected_bg=ui_color(9,145,179);
        if (active) {
            ui_round_rect(12,y,296,21,2,selected_bg);
            ui_round_rect(12,y,3,21,1,cyan);
        }
        draw_file_icon(entries[index].directory, 19, y + 3, icon, active ? selected_bg : panel);
        char label[72];
        make_ui_label(label, sizeof(label), entries[index].name, 31);
        if (entries[index].directory) {
            size_t n = strlen(label);
            if (n + 1 < sizeof(label)) { label[n] = '/'; label[n + 1] = '\0'; }
        }
        u64 duration = entries[index].directory ? 0 : cached_track_duration(entries[index].name);
        if (playing && index == current_index) duration = track_duration_ms;
        ui_text_fit(buf,label,41,y+3,duration?210:257,0.41f,fg);
        if (duration) {
            char stamp[16];
            format_track_time(stamp, sizeof(stamp), duration);
            ui_text(buf, stamp, 299, y + 4, 0.36f, active?white:secondary, C2D_AlignRight);
        }
        if (row < 4 && row + first + 1 < entry_count)
            ui_rect(16, y + 21, 288, 1, ui_color(24, 48, 68));
    }

    ui_rect(14,166,292,0.8f,border);
    if (playing) {
        ui_key(buf,"A",paused?"Reprendre":"Pause",16,170,12);
        ui_key(buf,"B","Stop",96,170,12);
        ui_key(buf,"L/R","Piste",159,170,22);
        ui_key(buf,"X","Repeat",238,170,12);
        ui_key(buf,"Y","Bass",16,187,12);
        ui_key(buf,"SELECT","Options",78,187,35);
        ui_key(buf,"<>","Pitch",166,187,20);
        ui_key(buf,"START","Quitter",236,187,31);
        ui_text_fit(buf,ui_language.french
            ? "Pad : droite verrouille / gauche deverrouille  |  SELECT : filtres"
            : "Pad: clockwise locks / counter-clockwise unlocks | SELECT: filters",
                    16,203,292,0.31f,secondary);
    } else {
        ui_key(buf,"A","Ouvrir / Lire",16,173,12);
        ui_key(buf,"B","Retour",130,173,12);
        ui_key(buf,"X","Shuffle",236,173,12);
        ui_key(buf,"Y","Lire le dossier",16,190,12);
        ui_key(buf,"SELECT","Options",105,190,35);
        ui_key(buf,"START","Quitter",235,190,31);
        ui_text_fit(buf,ui_language.french
            ? "Toucher pour choisir, toucher encore pour ouvrir"
            : "Tap to select; tap again to open",16,203,292,0.31f,secondary);
    }

    ui_rect(14,215,292,0.8f,border);
    char battery[20];
    char temperature[20];
    if (battery_percent >= 0)
        snprintf(battery, sizeof(battery), "BAT %d%%",
                 ((int)battery_percent / 5) * 5);
    else
        snprintf(battery, sizeof(battery), "BAT --%%");
    if (battery_temperature_c >= 0)
        snprintf(temperature, sizeof(temperature), "TEMP %d°C", battery_temperature_c);
    else
        snprintf(temperature, sizeof(temperature), "TEMP --°C");
    bool low_battery = battery_percent >= 0 && battery_percent <= 20 &&
                       charging_state_known && !battery_charging;
    ui_text(buf, battery, 16, 218, 0.34f,
            low_battery ? ui_color(255, 78, 82) : secondary, C2D_AlignLeft);
    ui_text(buf, temperature, 158, 218, 0.34f,
            battery_temperature_c >= thermal_warning_threshold
                ? ui_color(255, 78, 82) : secondary, C2D_AlignLeft);
}

static void draw(void) {
    if (!ui_renderer_ready) return;
    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return;
    // FrameBegin waits for the previous GPU work before replacing its texture.
    if (cover_texture_dirty) {
        upload_cover_texture();
        cover_texture_dirty = false;
    }
    draw_top_ui();
    // Citro3D transfers only render targets used in this frame. Keep the
    // library's last frame on screen while the record animates above it.
    if (ui_dirty || (playing && time_dirty)) draw_bottom_ui();
    C3D_FrameEnd(0);
    ui_dirty = false;
    time_dirty = false;
}

static void draw_language_prompt(void) {
    if (!ui_renderer_ready || !C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return;
    const u32 bg=ui_color(5,17,28), border=ui_color(40,83,112);
    const u32 cyan=ui_color(41,190,219), white=ui_color(232,240,248);
    const u32 secondary=ui_color(145,168,191);
    C2D_TextBufClear(ui_top_text_buffer);
    C2D_TargetClear(ui_top_target,ui_color(7,16,29));
    C2D_SceneBegin(ui_top_target);
    ui_round_rect(5,5,390,230,7,border);
    ui_round_rect(8,8,384,224,5,bg);
    ui_text_center(ui_top_text_buffer,
        force_language_selection ? "CHOISIR LA LANGUE / CHOOSE LANGUAGE" :
        ui_language.french ? "CHOIX DE LANGUE" : "LANGUAGE CHECK",
        200,27,0.58f,cyan);
    if (force_language_selection) {
        ui_text_center(ui_top_text_buffer,
            ui_language.french ? "Maintiens X au demarrage pour changer la langue." :
                                 "Hold X at startup to change the language.",
            200,70,0.40f,white);
        ui_text_center(ui_top_text_buffer,"A : Francais     B : English",
                       200,111,0.48f,white);
    } else if (ui_language.french) {
        ui_text_center(ui_top_text_buffer,"La langue detectee est le francais ?",
                       200,70,0.48f,white);
        ui_text_center(ui_top_text_buffer,"A : Oui, continuer",
                       200,107,0.46f,cyan);
        ui_text_center(ui_top_text_buffer,"B : Non, choisir English",
                       200,139,0.46f,white);
    } else {
        ui_text_center(ui_top_text_buffer,"Is English your preferred language?",
                       200,70,0.48f,white);
        ui_text_center(ui_top_text_buffer,"A: Yes, continue",
                       200,107,0.46f,cyan);
        ui_text_center(ui_top_text_buffer,"B: No, choose French",
                       200,139,0.46f,white);
    }
    ui_text_center(ui_top_text_buffer,
        ui_language.french ? "Langues disponibles : francais et English." :
                             "Available languages: French and English.",
        200,185,0.38f,secondary);

    C2D_TextBufClear(ui_bottom_text_buffer);
    C2D_TargetClear(ui_bottom_target,ui_color(7,16,29));
    C2D_SceneBegin(ui_bottom_target);
    ui_round_rect(4,4,312,232,6,border);
    ui_round_rect(8,8,304,224,4,bg);
    ui_text_center(ui_bottom_text_buffer,"SHIELD DJ STANDARD",160,47,0.48f,cyan);
    ui_text_center(ui_bottom_text_buffer,
        ui_language.french ? "A confirme   B change la langue" :
                             "A confirms   B changes language",
        160,103,0.40f,white);
    ui_text_center(ui_bottom_text_buffer,
        ui_language.french ? "Mon anglais peut contenir des erreurs." :
                             "My English may contain mistakes.",
        160,144,0.35f,secondary);
    C3D_FrameEnd(0);
}

static void choose_language_at_startup(void) {
    if (!force_language_selection && !ui_language.confirmation_needed) return;
    while (aptMainLoop()) {
        aptHandleSleep();
        if (aptShouldClose()) break;
        hidScanInput();
        u32 down=hidKeysDown();
        if (down & KEY_A) {
            if (force_language_selection) {
                ui_language.french=true;
                ui_language.confirmation_needed=false;
                shield_ui_language_save(&ui_language);
            } else shield_ui_language_choose(&ui_language,true);
            break;
        }
        if (down & KEY_B) {
            if (force_language_selection) {
                ui_language.french=false;
                ui_language.confirmation_needed=false;
                shield_ui_language_save(&ui_language);
            } else shield_ui_language_choose(&ui_language,false);
            break;
        }
        if (down & KEY_START) {
            shield_ui_language_choose(&ui_language,true);
            break;
        }
        draw_language_prompt();
        gspWaitForVBlank();
    }
}

static bool ui_renderer_init(void) {
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) return false;
    // Glyphs also consume objects; reserve room for both screens together.
    if (!C2D_Init(1536)) {
        C3D_Fini();
        return false;
    }
    C2D_Prepare();
    ui_top_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    ui_bottom_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    ui_top_text_buffer = C2D_TextBufNew(1024);
    ui_bottom_text_buffer = C2D_TextBufNew(1024);
    ui_fit_text_buffer = C2D_TextBufNew(128);
    if (!ui_top_target || !ui_bottom_target ||
        !ui_top_text_buffer || !ui_bottom_text_buffer) {
        if (ui_bottom_target) C3D_RenderTargetDelete(ui_bottom_target);
        if (ui_top_target) C3D_RenderTargetDelete(ui_top_target);
        if (ui_bottom_text_buffer) C2D_TextBufDelete(ui_bottom_text_buffer);
        if (ui_top_text_buffer) C2D_TextBufDelete(ui_top_text_buffer);
        if (ui_fit_text_buffer) C2D_TextBufDelete(ui_fit_text_buffer);
        C2D_Fini();
        C3D_Fini();
        return false;
    }
    ui_font = C2D_FontLoadSystem(CFG_REGION_EUR);
    if (ui_font) C2D_FontSetFilter(ui_font, GPU_LINEAR, GPU_LINEAR);
    ui_top_font = C2D_FontLoadFromMem(top_font_start,
                                      (size_t)(top_font_end-top_font_start));
    if (ui_top_font) C2D_FontSetFilter(ui_top_font, GPU_LINEAR, GPU_LINEAR);
    logo_ready=C3D_TexInit(&logo_texture,64,64,GPU_RGBA8);
    if(logo_ready) {
        C3D_TexUpload(&logo_texture,shield_logo_data);
        C3D_TexFlush(&logo_texture);
        C3D_TexSetFilter(&logo_texture,GPU_LINEAR,GPU_LINEAR);
        C3D_TexSetWrap(&logo_texture,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
        logo_image=(C2D_Image){&logo_texture,&logo_subtexture};
    }
    ui_renderer_ready = true;
    return true;
}

static void ui_renderer_exit(void) {
    if (!ui_renderer_ready) return;
    // Wait for submitted draws before freeing textures referenced by them.
    if (C3D_FrameBegin(0)) C3D_FrameEnd(0);
    if (logo_ready) C3D_TexDelete(&logo_texture);
    if (cover_texture_ready) C3D_TexDelete(&cover_texture);
    if (ui_top_font) C2D_FontFree(ui_top_font);
    if (ui_font) C2D_FontFree(ui_font);
    C2D_TextBufDelete(ui_bottom_text_buffer);
    C2D_TextBufDelete(ui_top_text_buffer);
    if (ui_fit_text_buffer) C2D_TextBufDelete(ui_fit_text_buffer);
    C3D_RenderTargetDelete(ui_bottom_target);
    C3D_RenderTargetDelete(ui_top_target);
    C2D_Fini();
    C3D_Fini();
    ui_renderer_ready = false;
    cover_texture_ready = false;
}

static unsigned morton8(unsigned x, unsigned y) {
    unsigned result = 0;
    for (unsigned bit = 0; bit < 3; bit++) {
        result |= ((x >> bit) & 1u) << (bit * 2);
        result |= ((y >> bit) & 1u) << (bit * 2 + 1);
    }
    return result;
}

static void upload_cover_texture(void) {
    if (!ui_renderer_ready || !cover_valid) return;
    if (!cover_texture_ready) {
        // CPU upload into linear memory avoids a DMA transfer from ordinary BSS.
        if (!C3D_TexInit(&cover_texture, 128, 128, GPU_RGBA8)) return;
        C3D_TexSetFilter(&cover_texture, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&cover_texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        cover_texture_ready = true;
        cover_image.tex = &cover_texture;
        cover_image.subtex = &cover_subtexture;
    }
    memset(cover_texture_upload, 0, sizeof(cover_texture_upload));
    for (unsigned y = 0; y < COVER_SIZE; y++) {
        for (unsigned x = 0; x < COVER_SIZE; x++) {
            unsigned index = (((y >> 3) * 16 + (x >> 3)) * 64) + morton8(x & 7, y & 7);
            u16 pixel = cover_pixels[y * COVER_SIZE + x];
            unsigned red = ((pixel >> 11) & 31) * 255 / 31;
            unsigned green = ((pixel >> 5) & 63) * 255 / 63;
            unsigned blue = (pixel & 31) * 255 / 31;
            int dx = (int)x * 2 - (COVER_SIZE - 1);
            int dy = (int)y * 2 - (COVER_SIZE - 1);
            int distance=dx*dx+dy*dy;
            int outer=(COVER_SIZE-1)*(COVER_SIZE-1);
            int inner=(COVER_SIZE-3)*(COVER_SIZE-3);
            // PICA RGBA8 texture bytes are stored in A, B, G, R order.
            cover_texture_upload[index * 4 + 0] =
                distance<=inner ? 255 : distance>=outer ? 0 :
                (u8)((outer-distance)*255/(outer-inner));
            cover_texture_upload[index * 4 + 1] = (u8)blue;
            cover_texture_upload[index * 4 + 2] = (u8)green;
            cover_texture_upload[index * 4 + 3] = (u8)red;
        }
    }
    C3D_TexUpload(&cover_texture, cover_texture_upload);
    C3D_TexFlush(&cover_texture);
}
static void change_brightness(int delta) {
    if (!lcd_ready) return;
    int next = brightness_level + delta;
    if (next < 1) next = 1;
    if (next > 5) next = 5;
    if (next == brightness_level) return;
    if (R_SUCCEEDED(GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTH, (u32)next))) {
        brightness_level = next;
        brightness_changed = true;
        ui_dirty = true;
    }
}

static void change_pitch(int direction) {
    int next = pitch_milli + (direction > 0 ? 25 : -25);
    if (next < 250) next = 250;
    if (next > 2000) next = 2000;
    if (next == pitch_milli) return;
    pitch_milli = next;
    speed = pitch_milli / 1000.0f;
    update_output_rate();
    ui_dirty = true;
}

/* Keep L/R locking identical to Shield DJ Lite. This only reads the Circle
 * Pad and changes the track-skip lock; audio decoding and DSP stay untouched. */
static void update_track_key_lock(u64 now) {
    static int previous_sector = -1, progress;
    static bool wait_for_center, gesture_active;
    static u64 last_step_ms, gesture_start_ms;
    circlePosition circle;
    hidCircleRead(&circle);
    int dx=circle.dx, dy=circle.dy;
    int ax=abs(dx), ay=abs(dy);
    int radius_squared=dx*dx+dy*dy;
    /* Easier to trace than the old Manhattan threshold, with hysteresis to
     * ignore neutral-stick noise that could otherwise start a false gesture. */
    if (radius_squared < 55*55) {
        previous_sector=-1;
        progress=0;
        wait_for_center=false;
        gesture_active=false;
        return;
    }
    if (!gesture_active) {
        if (radius_squared < 75*75) return;
        gesture_active=true;
    }
    if (wait_for_center) return;

    int sector;
    if (ax > ay*2) sector=dx>0 ? 0 : 4;
    else if (ay > ax*2) sector=dy<0 ? 2 : 6;
    else if (dx>0) sector=dy<0 ? 1 : 7;
    else sector=dy<0 ? 3 : 5;
    if (previous_sector<0) {
        previous_sector=sector;
        gesture_start_ms=last_step_ms=now;
        return;
    }
    if (sector==previous_sector) return;
    int delta=(sector-previous_sector+8)%8;
    previous_sector=sector;
    /* Accept a skipped octant when a quick movement crosses between samples. */
    if (delta!=1 && delta!=2 && delta!=6 && delta!=7) {
        progress=0;
        gesture_start_ms=last_step_ms=now;
        return;
    }
    int step=delta<=2 ? delta : delta-8;
    if (now-last_step_ms>900) {
        progress=0;
        gesture_start_ms=now;
    }
    progress+=step;
    last_step_ms=now;
    if (abs(progress)>=8) {
        if (now-gesture_start_ms>=200 && now-gesture_start_ms<=6000) {
            bool lock=progress>0;
            if (track_skip_locked!=lock) {
                track_skip_locked=lock;
                playback_status=lock ? "L/R verrouilles" : "L/R deverrouilles";
                ui_dirty=true;
            }
        }
        wait_for_center=true;
    }
}

static void save_brightness_for_home(void) {
    // Keep the user's chosen level as the system preset so HOME does not
    // restore its previous level (or flash to the default) when we exit.
    // Preserve byte 0, which stores the separate power-saving setting.
    if (!brightness_changed || !backlight_config_valid || !cfgu_service_ready)
        return;
    backlight_config[1] = (u8)brightness_level;
    if (R_SUCCEEDED(CFG_SetConfigInfoBlk4(sizeof(backlight_config), 0x00050001,
                                          backlight_config)))
        (void)CFG_UpdateConfigSavegame();
}

static void ui_boot_begin(void) {
    if(!ui_renderer_ready) return;
    boot_texture_ready=C3D_TexInit(&boot_texture,256,256,GPU_RGBA8);
    if(!boot_texture_ready) return;
    C3D_TexUpload(&boot_texture,shield_boot_data);
    C3D_TexFlush(&boot_texture);
    C3D_TexSetFilter(&boot_texture,GPU_LINEAR,GPU_LINEAR);
    C3D_TexSetWrap(&boot_texture,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    static const Tex3DS_SubTexture subtex={256,256,0,1,1,0};
    C2D_Image splash={&boot_texture,&subtex};
    if(C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) {
        C2D_TargetClear(ui_top_target,ui_color(0,18,30));
        C2D_SceneBegin(ui_top_target);
        (void)C2D_DrawImageAt(splash,80,0,UI_DEPTH,NULL,0.9375f,0.9375f);
        C2D_TargetClear(ui_bottom_target,ui_color(5,17,28));
        C2D_SceneBegin(ui_bottom_target);
        C2D_TextBufClear(ui_bottom_text_buffer);
        ui_text_center(ui_bottom_text_buffer,"SHIELD DJ STANDARD",160,103,0.55f,ui_color(41,190,219));
        ui_text_center(ui_bottom_text_buffer,"Chargement de la bibliotheque...",160,133,0.36f,ui_color(145,168,191));
        C3D_FrameEnd(0);
    }
    boot_started_ms=osGetTime();
}

static void ui_boot_end(void) {
    if(!boot_texture_ready) return;
    // Initialization runs under the splash; keep it visible long enough to
    // see the artwork without delaying the application beyond 2.5 seconds.
    while(osGetTime()-boot_started_ms<2500 && !aptShouldClose()) {
        aptHandleSleep();
        gspWaitForVBlank();
    }
    if(C3D_FrameBegin(0)) {
        C3D_TexDelete(&boot_texture);
        boot_texture_ready=false;
        C3D_FrameEnd(0);
    }
}

int main(void) {
    gfxInitDefault();
    hidScanInput();
    force_language_selection = (hidKeysHeld() & KEY_X) != 0;
    aptSetHomeAllowed(false);
    fs_service_ready = R_SUCCEEDED(fsInit());
    update_sd_storage();
    ptmu_service_ready = R_SUCCEEDED(ptmuInit());
    update_charging_state();
    battery_service_ready = R_SUCCEEDED(mcuHwcInit());
    update_battery();
    battery_last_update = osGetTime();
    charger_last_update = battery_last_update;
    update_battery_temperature();
    temperature_last_update = battery_last_update;

    cfgu_service_ready = R_SUCCEEDED(cfguInit());
    if (cfgu_service_ready) {
        u8 system_model;
        if (R_SUCCEEDED(CFGU_GetSystemModel(&system_model))) {
            if (system_model == CFG_MODEL_2DS)
                thermal_warning_threshold = 40;
            else if (system_model == CFG_MODEL_N3DS ||
                     system_model == CFG_MODEL_N3DSXL ||
                     system_model == CFG_MODEL_N2DSXL)
                thermal_warning_threshold = 45;
            // Old 3DS and Old 3DS XL use the conservative 42°C default.
        }
        if (R_SUCCEEDED(CFGU_GetConfigInfoBlk2(sizeof(backlight_config), 0x00050001,
                                               backlight_config)) &&
            backlight_config[1] >= 1 && backlight_config[1] <= 5) {
            brightness_level = backlight_config[1];
            backlight_config_valid = true;
        }
    }
    shield_ui_language_load(&ui_language,
        "sdmc:/3ds/ShieldDJStandard/ui-language-v1.dat",cfgu_service_ready);
    (void)ui_renderer_init();
    ui_boot_begin();
    lcd_ready = R_SUCCEEDED(gspLcdInit());

    Result audio_result = ndspInit();
    audio_ready = R_SUCCEEDED(audio_result);
    if (audio_ready) {
        for (int i = 0; i < AUDIO_BLOCK_COUNT; i++) {
            blocks[i].pcm = (int16_t *)linearAlloc(BLOCK_FRAMES * 2 * sizeof(int16_t));
            if (!blocks[i].pcm) audio_ready = false;
        }
    }
    if (audio_ready) {
        ndspSetMasterVol(1.0f);
        ndspSetClippingMode(NDSP_CLIP_SOFT);
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspChnReset(CHANNEL);
        ndspChnSetInterp(CHANNEL, NDSP_INTERP_POLYPHASE);
        ndspChnSetFormat(CHANNEL, NDSP_FORMAT_STEREO_PCM16);
        float mix[12] = {0};
        mix[0] = mix[1] = 1.0f;
        ndspChnSetMix(CHANNEL, mix);
        apply_tone();
        ndspChnSetPaused(CHANNEL, false);
        init_thermal_beep();
    } else {
        audio_status = R_FAILED(audio_result) ? "dspfirm.cdc absent dans sdmc:/3ds/" : "memoire audio insuffisante";
        for (int i = 0; i < AUDIO_BLOCK_COUNT; i++) {
            if (blocks[i].pcm) { linearFree(blocks[i].pcm); blocks[i].pcm = NULL; }
        }
        if (R_SUCCEEDED(audio_result)) ndspExit();
    }
    srand((unsigned)osGetTime());
    scan_folder();
    ui_boot_end();
    choose_language_at_startup();
    guide_load();
    if (force_language_selection) guide_page = 1;
    update_wall_clock(osGetTime());
    draw();
    u64 pitch_repeat_next_ms = 0;
    u64 last_library_touch_ms = 0;
    int last_library_touch_entry = -1;

    for (;;) {
        // Sleep remains enabled for lid-close playback policy. HOME stays
        // blocked; do not force APT's jump-to-HOME transition here because
        // the app's POWER handling previously froze the console.
        aptHandleSleep();
        if (aptShouldClose()) break;
        bool screen_active = aptIsActive();
        if (screen_active && !ui_was_active) ui_dirty = true;
        ui_was_active = screen_active;
        u64 now = osGetTime();
        update_wall_clock(now);
        update_playback_clock(now);
        if (now - battery_last_update >= 5000) {
            float old_battery = battery_percent;
            update_battery();
            battery_last_update = now;
            if (battery_percent != old_battery) ui_dirty = true;
        }
        if (now - charger_last_update >= 1000) {
            bool old_known = charging_state_known;
            bool old_charging = battery_charging;
            update_charging_state();
            charger_last_update = now;
            if (charging_state_known != old_known || battery_charging != old_charging)
                ui_dirty = true;
        }
        if (now - temperature_last_update >= 15000) {
            int old_temperature = battery_temperature_c;
            update_battery_temperature();
            temperature_last_update = now;
            if (battery_temperature_c != old_temperature) ui_dirty = true;
        }
        bool hot = thermal_warning_active();
        if (hot && playing) {
            stop_track(true);
            playback_status = "Lecture arretee : surchauffe.";
            playback_error = true;
        }
        if (hot && !thermal_beep_latched) {
            thermal_beep_latched = true;
            play_thermal_beep();
        } else if (battery_temperature_c >= 0 && !hot) {
            thermal_beep_latched = false;
        }
        if (screen_active && (hot || low_battery_warning_active())) {
            bool blink_on = ((now / 500ULL) & 1ULL) == 0;
            if (blink_on != warning_blink_on) {
                warning_blink_on = blink_on;
                time_dirty = true;
            }
        }
        hidScanInput();
        // KEY_UP/DOWN/LEFT/RIGHT include Circle Pad aliases in libctru.
        // Ignore its bits so only the physical D-Pad drives these controls.
        u32 down = hidKeysDown() & ~CIRCLE_PAD_KEYS;
        u32 held = hidKeysHeld() & ~CIRCLE_PAD_KEYS;
        u32 released = hidKeysUp() & ~CIRCLE_PAD_KEYS;
        if (aptCheckHomePressRejected() && now - last_home_toggle_ms >= 350) {
            audio_boost_mode = (audio_boost_mode + 1) % 4;
            last_home_toggle_ms = now;
            playback_error = false;
            playback_status = audio_boost_mode == 3 ? "Boost audio 500%" :
                              audio_boost_mode == 2 ? "Boost audio 300%" :
                              audio_boost_mode == 1 ? "Boost audio 200%" :
                                                      "Boost audio desactive";
            ui_dirty = true;
        }
        if(guide_page) {
            if(down & KEY_START) break;
            if((down & KEY_B) && guide_page>1) {
                guide_page--;
                ui_dirty=true;
            }
            if(down & KEY_A) {
                if(guide_page==3) guide_complete();
                else guide_page++;
                ui_dirty=true;
            }
            if(screen_active && (ui_dirty || time_dirty)) draw();
            // Acknowledging a page must never also start a song.
            svcSleepThread(1000000LL);
            continue;
        }
        if (down & KEY_START) break;
        if (down & KEY_SELECT) {
            options_open = !options_open;
            options_selection = filter_profile;
            touch_seek_dragging = false;
            touch_reverb_dragging = false;
            ui_dirty = true;
            down = held = released = 0;
        } else if (options_open) {
            if (down & KEY_B) {
                options_open = false;
            } else if (down & KEY_UP) {
                if (options_selection == 8) options_selection = 7;
                else if (options_selection == 7) options_selection = 6;
                else if (options_selection >= 2) options_selection -= 2;
            } else if (down & KEY_DOWN) {
                if (options_selection >= 7) options_selection = 8;
                else if (options_selection >= 5) options_selection = 7;
                else options_selection += 2;
            } else if (down & KEY_LEFT) {
                if (options_selection == 8) options_selection = 7;
                else if (options_selection < 7 && (options_selection & 1)) options_selection--;
            } else if (down & KEY_RIGHT) {
                if (options_selection == 7) options_selection = 8;
                else if (!(options_selection & 1) && options_selection < 6) options_selection++;
            } else if (down & KEY_A) {
                options_activate();
            } else if (down & KEY_TOUCH) {
                touchPosition touch;
                hidTouchRead(&touch);
                if (touch.py >= 170 && touch.py <= 207) {
                    touch_reverb_dragging = true;
                    touch_reverb_custom_drag = reverb_custom_enabled;
                    touch_reverb_preview = touch_reverb_custom_drag
                        ? (touch.px <= 22 ? 0 : touch.px >= 298 ? 100 :
                           (unsigned)(touch.px - 22) * 100 / 276)
                        : (touch.px <= 22 ? 0 : touch.px >= 298 ? 3 :
                           (unsigned)(touch.px - 22) / 69);
                    ui_dirty = true;
                } else if (touch.py >= 147 && touch.py < 169 &&
                           touch.px >= 13 && touch.px < 307) {
                    options_selection = touch.px < 160 ? 7 : 8;
                    options_activate();
                } else if (touch.py >= 57 && touch.py < 139 &&
                           touch.px >= 13 && touch.px < 304) {
                    unsigned row = (touch.py - 57) / 21;
                    unsigned col = touch.px >= 161 ? 1 : 0;
                    unsigned index = row * 2 + col;
                    if (index < 7) {
                        options_selection = index;
                        options_activate();
                    }
                }
            }
            if (touch_reverb_dragging && (held & KEY_TOUCH)) {
                touchPosition touch;
                hidTouchRead(&touch);
                unsigned amount = touch_reverb_custom_drag
                    ? (touch.px <= 22 ? 0 : touch.px >= 298 ? 100 :
                       (unsigned)(touch.px - 22) * 100 / 276)
                    : (touch.px <= 22 ? 0 : touch.px >= 298 ? 3 :
                       (unsigned)(touch.px - 22) / 69);
                if (amount != touch_reverb_preview) {
                    touch_reverb_preview = amount;
                    ui_dirty = true;
                }
            }
            if (touch_reverb_dragging && (released & KEY_TOUCH)) {
                if (touch_reverb_custom_drag) reverb_custom_percent = touch_reverb_preview;
                else reverb_mode = touch_reverb_preview;
                touch_reverb_dragging = false;
                ui_dirty = true;
            }
            if (down) ui_dirty = true;
            down = held = released = 0;
        }
        if (playing && (down & KEY_TOUCH)) {
            touchPosition touch;
            hidTouchRead(&touch);
            if (touch.py >= 123 && touch.py < 144) {
                if (track_duration_ms) {
                    touch_seek_dragging = true;
                    touch_seek_target_ms = touch_target_ms(touch.px);
                    last_touch_preview_ms = now;
                    ui_dirty = true;
                } else {
                    playback_status = "Duree inconnue : position indisponible.";
                    ui_dirty = true;
                }
            }
        }
        if (touch_seek_dragging && (held & KEY_TOUCH)) {
            touchPosition touch;
            hidTouchRead(&touch);
            u64 target = touch_target_ms(touch.px);
            if (target != touch_seek_target_ms) {
                touch_seek_target_ms = target;
                if (now - last_touch_preview_ms >= 75) {
                    ui_dirty = true;
                    last_touch_preview_ms = now;
                }
            }
        }
        if (touch_seek_dragging && (released & KEY_TOUCH)) {
            u64 target = touch_seek_target_ms;
            touch_seek_dragging = false;
            (void)seek_track(target);
            ui_dirty = true;
        }
        bool lr_both = (held & (KEY_L | KEY_R)) == (KEY_L | KEY_R);
        if (!options_open) update_track_key_lock(now);
        /* In Standard, one tap selects a visible file/folder and a quick
         * second tap opens it. Touch is intentionally library-only. */
        if (!playing && (down & KEY_TOUCH) && entry_count > 0) {
            touchPosition touch;
            hidTouchRead(&touch);
            if (touch.px >= 12 && touch.px < 308 &&
                touch.py >= 54 && touch.py < 164) {
                int visible_first = selection > 2 ? selection - 2 : 0;
                if (visible_first + 5 > entry_count)
                    visible_first = entry_count > 5 ? entry_count - 5 : 0;
                int row = (touch.py - 54) / 22;
                int tapped = visible_first + row;
                if (row >= 0 && row < 5 && tapped < entry_count) {
                    bool activate = tapped == selection &&
                        tapped == last_library_touch_entry &&
                        now - last_library_touch_ms <= 550;
                    selection = tapped;
                    ui_dirty = true;
                    if (activate) {
                        last_library_touch_entry = -1;
                        down |= KEY_A;
                    } else {
                        last_library_touch_entry = tapped;
                        last_library_touch_ms = now;
                    }
                } else {
                    last_library_touch_entry = -1;
                }
            } else {
                last_library_touch_entry = -1;
            }
        }
        if (down) ui_dirty = true;
        if (down & KEY_START) break;
        if (down & KEY_A) {
            if (!playing && entry_count && entries[selection].directory) enter_selected_directory();
            else if (!playing && entry_count && !entries[selection].directory) {
                // Keep the selected entry and track list from the same scan.
                // A second readdir() could reorder SD entries and start a
                // different song from the one the user just selected.
                build_order(-1);
                for (int i = 0; i < track_count; i++)
                    if (!strcmp(tracks[order[i]].name, entries[selection].name)) { open_track(i); break; }
            }
            else if (playing && audio_ready && !hot) {
                paused = !paused;
                ndspChnSetPaused(CHANNEL, paused);
                aptSetSleepAllowed(paused);
            }
        }
        if (down & KEY_B) { if (playing) stop_track(true); else parent_directory(); }
        if (down & KEY_X) {
            if (playing) {
                repeat_current = !repeat_current;
                playback_error = false;
                playback_status = repeat_current ? "Repetition piste activee" :
                                                   "Repetition piste desactivee";
            } else {
                // Shuffle remains enabled by default; X can still change it
                // from the library, while playback uses X for track repeat.
                shuffle = !shuffle;
            }
            ui_dirty = true;
        }
        if (down & KEY_Y) {
            if (playing && audio_ready) {
                bass_profile = (bass_profile + 1) % 5;
                apply_tone();
                ui_dirty = true;
                playback_error = false;
                playback_status = bass_profile == 4 ? "Basses EXTREME (+22 dB)" :
                                  bass_profile == 3 ? "Basses +16 dB" :
                                  bass_profile == 2 ? "Basses BOOST (+9 dB)" :
                                  bass_profile == 1 ? "Basses DOUX (+6 dB)" :
                                                      "Basses OFF";
            } else {
                start_folder(-1);
            }
        }
        if (playing && !track_skip_locked && !lr_both && (down & KEY_L)) step_track(-1);
        if (playing && !track_skip_locked && !lr_both && (down & KEY_R)) step_track(1);
        if (down & KEY_UP) {
            if (playing) change_brightness(1);
            else if (selection > 0) selection--;
        }
        if (down & KEY_DOWN) {
            if (playing) change_brightness(-1);
            else if (selection + 1 < entry_count) selection++;
        }
        const u32 pitch_keys = held & (KEY_LEFT | KEY_RIGHT);
        if (playing && pitch_keys != (KEY_LEFT | KEY_RIGHT)) {
            if (down & KEY_RIGHT) {
                change_pitch(1);
                pitch_repeat_next_ms = now + 450;
            } else if (down & KEY_LEFT) {
                change_pitch(-1);
                pitch_repeat_next_ms = now + 450;
            } else if ((pitch_keys & KEY_RIGHT) && now >= pitch_repeat_next_ms) {
                change_pitch(1);
                pitch_repeat_next_ms = now + 200;
            } else if ((pitch_keys & KEY_LEFT) && now >= pitch_repeat_next_ms) {
                change_pitch(-1);
                pitch_repeat_next_ms = now + 200;
            }
        } else {
            pitch_repeat_next_ms = 0;
        }
        if (playing && audio_ready) advance_output_rate();
        if (playing && !paused && audio_file) {
            for (int i = 0; i < AUDIO_BLOCK_COUNT; i++) {
                if (!audio_eof && blocks[i].wave.status == NDSP_WBUF_DONE)
                    queue_block(&blocks[i]);
            }
            bool drained = true;
            for (int i = 0; i < AUDIO_BLOCK_COUNT; i++)
                if (blocks[i].wave.status == NDSP_WBUF_QUEUED ||
                    blocks[i].wave.status == NDSP_WBUF_PLAYING) drained = false;
            if (audio_eof && drained) {
                if (!track_has_audio) {
                    failed_tracks_in_a_row++;
                    if (failed_tracks_in_a_row >= track_count) {
                        stop_track(true);
                        playback_status = "Aucun MP3 ou OGG lisible dans ce dossier.";
                        playback_error = true;
                    } else {
                        step_track(1);
                    }
                } else {
                    failed_tracks_in_a_row = 0;
                    if (!repeat_current || !open_track(order_pos))
                        step_track(1);
                }
                ui_dirty = true;
            }
        }
        // Fill the NDSP queue before spending a bounded amount of CPU on the
        // embedded cover. Never decode artwork while the display is asleep.
        if (screen_active && cover_load_pending && playing && audio_file && !ogg_track) {
            cover_load_pending = false;
            (void)load_mp3_cover(audio_file);
            ui_dirty = true;
        }
        if (screen_active && playing && !paused && display_title_chars > 27 &&
            now - title_scroll_last_ms >= 500) {
            title_scroll_last_ms = now;
            title_scroll_offset++;
            time_dirty = true;
        }
        // Two VBlanks plus FrameBegin's own sync give a stable ~20 fps. This
        // avoids the timer jitter of the old 50 ms polling and draws only the
        // top screen when the library has not changed.
        bool art_frame_due = screen_active && playing && !paused &&
                             C3D_FrameCounter(0) - last_art_vblank >= 2;
        if (screen_active && (ui_dirty || time_dirty || art_frame_due)) {
            if (playing && !paused) {
                // 10 RPM at 1.00x matches the former 0.65x visual pace.
                art_rotation = (float)(playback_position_ms % 6000ULL) * 0.0010471976f;
            }
            draw();
            last_art_vblank = C3D_FrameCounter(0);
        }
        // VBlank can stop while the lid is closed. Do not make audio refill
        // and next-track handling depend on a display event.
        svcSleepThread(5 * 1000 * 1000LL);
    }

    save_brightness_for_home();
    stop_track(true);
    if (audio_ready) {
        if (thermal_beep_pcm) ndspChnWaveBufClear(THERMAL_BEEP_CHANNEL);
        ndspExit();
    }
    if (thermal_beep_pcm) linearFree(thermal_beep_pcm);
    if (fs_service_ready) fsExit();
    if (ptmu_service_ready) ptmuExit();
    if (battery_service_ready) mcuHwcExit();
    ui_renderer_exit();
    if (cfgu_service_ready) cfguExit();
    if (lcd_ready) {
        gspLcdExit();
    }
    for (int i = 0; i < AUDIO_BLOCK_COUNT; i++) {
        if (blocks[i].pcm) linearFree(blocks[i].pcm);
    }
    gfxExit();
    return 0;
}
