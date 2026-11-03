#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <linux/input.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define MAX_GAMES 1000
#define ROWS_PER_PAGE 6
#define APP_VERSION "1.2.5"

// Colors (ARGB 32-bit: 0xAARRGGBB)
#define COLOR_BG          0xFF0B0F19
#define COLOR_PANEL       0xFF131C2E
#define COLOR_PANEL_ALT   0xFF1A263D
#define COLOR_CARD_BORDER 0xFF25334D
#define COLOR_HIGHLIGHT   0xFF2563EB
#define COLOR_HIGHLIGHT_BORDER 0xFF38BDF8
#define COLOR_WHITE       0xFFFFFFFF
#define COLOR_GRAY        0xFF94A3B8
#define COLOR_DARK_GRAY   0xFF475569
#define COLOR_TEXT_DIM    0xFF64748B
#define COLOR_GREEN       0xFF10B981
#define COLOR_GREEN_BG    0xFF064E3B
#define COLOR_CYAN        0xFF06B6D4
#define COLOR_AMBER       0xFFF59E0B
#define COLOR_PURPLE      0xFF8B5CF6
#define COLOR_RED         0xFFEF4444

typedef struct {
    char system[64];
    char filename[256];
    char title[256];
    char size_mb[32];
    int is_thai;
    char cover_url[512];
    char rom_url[512];
    int installed;
} GameItem;

static GameItem g_games[MAX_GAMES];
static int g_game_count = 0;
static int g_filtered_indices[MAX_GAMES];
static int g_filtered_count = 0;
static int g_selected_index = 0;
static int g_top_index = 0;
static char g_server_url[256] = "http://192.168.1.118:8080";
static char g_cur_filter[64] = "ALL";
static bool g_connected = false;

// TrimUI Brick Pro native target: 1024 x 768, 32-bit ARGB
static int g_screen_w = 1024;
static int g_screen_h = 768;
static int g_bpp = 32;
static int g_line_length = 1024 * 4;
static size_t g_fb_size = 0;
static bool g_rgb_swap = false;

static int g_fb_fd = -1;
static uint8_t *g_fb_mem = NULL;
static uint32_t *g_backbuffer = NULL;

// Font
static stbtt_fontinfo g_font;
static unsigned char *g_font_buffer = NULL;

// Mascot image
static uint32_t *g_mascot_pixels = NULL;
static int g_mascot_w = 0, g_mascot_h = 0;

// Current preview cover image
static uint32_t *g_cover_pixels = NULL;
static int g_cover_w = 0, g_cover_h = 0;
static int g_cover_loaded_index = -1;

// Input event file descriptors
#define MAX_INPUTS 16
static int g_input_fds[MAX_INPUTS];
static int g_input_count = 0;

// Unified User Actions
typedef enum {
    ACT_NONE = 0,
    ACT_UP,
    ACT_DOWN,
    ACT_LEFT,
    ACT_RIGHT,
    ACT_A,
    ACT_B,
    ACT_X,
    ACT_Y,
    ACT_L1,
    ACT_R1,
    ACT_SELECT,
    ACT_START,
    ACT_MENU
} UserAction;

// Input state tracking for HAT switches and Analog sticks
static int g_last_axis_y = 0;
static int g_last_axis_x = 0;
static int g_last_hat_y = 0;
static int g_last_hat_x = 0;

// System directory mappings
typedef struct {
    const char *full_name;
    const char *short_name;
    const char *alt_name;
} SystemDirMap;

static const SystemDirMap g_system_dirs[] = {
    {"Nintendo DS (NDS)", "NDS", "Nintendo DS"},
    {"Game Boy Advance (GBA)", "GBA", "Game Boy Advance"},
    {"Sony PlayStation (PS)", "PS", "PSX"},
    {"Sony PlayStation Portable (PSP)", "PSP", "PlayStation Portable"},
    {"Super Nintendo Entertainment System (SFC)", "SFC", "SNES"},
    {"Nintendo Entertainment System (FC)", "FC", "NES"},
    {"Sega Genesis (MD)", "MD", "GENESIS"},
    {"Game Boy (GB)", "GB", "Game Boy"},
    {"Game Boy Color (GBC)", "GBC", "Game Boy Color"},
    {NULL, NULL, NULL}
};

// Drain all buffered input events
static void flush_inputs(void) {
    g_last_axis_y = 0;
    g_last_axis_x = 0;
    g_last_hat_y = 0;
    g_last_hat_x = 0;
    struct input_event ev;
    for (int i = 0; i < g_input_count; i++) {
        while (read(g_input_fds[i], &ev, sizeof(ev)) > 0) {}
    }
}

// Unified input polling: handles D-Pad (Keys & Hat switch) and Analog sticks
static UserAction poll_user_action(int timeout_ms) {
    struct pollfd pfd[MAX_INPUTS];
    for (int i = 0; i < g_input_count; i++) {
        pfd[i].fd = g_input_fds[i];
        pfd[i].events = POLLIN;
    }

    int ret = poll(pfd, g_input_count, timeout_ms);
    if (ret <= 0) return ACT_NONE;

    for (int i = 0; i < g_input_count; i++) {
        if (!(pfd[i].revents & POLLIN)) continue;

        struct input_event ev;
        while (read(g_input_fds[i], &ev, sizeof(ev)) > 0) {
            // 1. Digital Keys & Gamepad Buttons (Press event: value == 1)
            if (ev.type == EV_KEY && ev.value == 1) {
                switch (ev.code) {
                    case 103: case 544: return ACT_UP;      // KEY_UP or BTN_DPAD_UP
                    case 108: case 545: return ACT_DOWN;    // KEY_DOWN or BTN_DPAD_DOWN
                    case 105: case 546: return ACT_LEFT;    // KEY_LEFT or BTN_DPAD_LEFT
                    case 106: case 547: return ACT_RIGHT;   // KEY_RIGHT or BTN_DPAD_RIGHT
                    case 305: case 28:  return ACT_A;       // TrimUI A: Right / KEY_ENTER
                    case 304: case 1:   return ACT_B;       // TrimUI B: Down / KEY_ESC
                    case 308: case 45:  return ACT_X;       // TrimUI X: Top (Linux 308) / KEY_X
                    case 307: case 21:  return ACT_Y;       // TrimUI Y: Left (Linux 307) / KEY_Y
                    case 310:           return ACT_L1;      // BTN_TL
                    case 311:           return ACT_R1;      // BTN_TR
                    case 314: case 353: return ACT_SELECT;  // BTN_SELECT
                    case 315:           return ACT_START;   // BTN_START
                    case 316: case 139: return ACT_MENU;    // BTN_MODE (MENU)
                }
            }
            // 2. Analog Joysticks & D-Pad HAT Switches
            else if (ev.type == EV_ABS) {
                // D-Pad HAT Switch Y (-1 = UP, +1 = DOWN, 0 = RELEASE)
                if (ev.code == ABS_HAT0Y) {
                    if (ev.value < 0 && g_last_hat_y >= 0) { g_last_hat_y = ev.value; return ACT_UP; }
                    if (ev.value > 0 && g_last_hat_y <= 0) { g_last_hat_y = ev.value; return ACT_DOWN; }
                    if (ev.value == 0) { g_last_hat_y = 0; }
                }
                // D-Pad HAT Switch X (-1 = LEFT, +1 = RIGHT, 0 = RELEASE)
                else if (ev.code == ABS_HAT0X) {
                    if (ev.value < 0 && g_last_hat_x >= 0) { g_last_hat_x = ev.value; return ACT_LEFT; }
                    if (ev.value > 0 && g_last_hat_x <= 0) { g_last_hat_x = ev.value; return ACT_RIGHT; }
                    if (ev.value == 0) { g_last_hat_x = 0; }
                }
                // Analog Joystick Y (Left Stick)
                else if (ev.code == ABS_Y) {
                    if (ev.value < -16000 && g_last_axis_y >= -16000) { g_last_axis_y = ev.value; return ACT_UP; }
                    if (ev.value > 16000 && g_last_axis_y <= 16000) { g_last_axis_y = ev.value; return ACT_DOWN; }
                    if (abs(ev.value) < 10000) { g_last_axis_y = 0; }
                }
                // Analog Joystick X (Left Stick)
                else if (ev.code == ABS_X) {
                    if (ev.value < -16000 && g_last_axis_x >= -16000) { g_last_axis_x = ev.value; return ACT_LEFT; }
                    if (ev.value > 16000 && g_last_axis_x <= 16000) { g_last_axis_x = ev.value; return ACT_RIGHT; }
                    if (abs(ev.value) < 10000) { g_last_axis_x = 0; }
                }
            }
        }
    }
    return ACT_NONE;
}

// UTF-8 decoding
static uint32_t next_utf8(const char **ptr) {
    const unsigned char *s = (const unsigned char *)*ptr;
    if (!*s) return 0;
    uint32_t codepoint = 0;
    if (*s < 0x80) {
        codepoint = *s++;
    } else if ((*s & 0xE0) == 0xC0) {
        codepoint = (*s++ & 0x1F) << 6;
        if (*s) codepoint |= (*s++ & 0x3F);
    } else if ((*s & 0xF0) == 0xE0) {
        codepoint = (*s++ & 0x0F) << 12;
        if (*s) codepoint |= (*s++ & 0x3F) << 6;
        if (*s) codepoint |= (*s++ & 0x3F);
    } else if ((*s & 0xF8) == 0xF0) {
        codepoint = (*s++ & 0x07) << 18;
        if (*s) codepoint |= (*s++ & 0x3F) << 12;
        if (*s) codepoint |= (*s++ & 0x3F) << 6;
        if (*s) codepoint |= (*s++ & 0x3F);
    } else {
        s++;
    }
    *ptr = (const char *)s;
    return codepoint;
}

// Drawing primitives
static inline uint32_t blend_pixel(uint32_t bg, uint32_t fg) {
    uint32_t a = (fg >> 24) & 0xFF;
    if (a == 255) return fg;
    if (a == 0) return bg;
    uint32_t inv_a = 255 - a;
    uint32_t r = (((fg >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * inv_a) / 255;
    uint32_t g = (((fg >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * inv_a) / 255;
    uint32_t b = ((fg & 0xFF) * a + (bg & 0xFF) * inv_a) / 255;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

static void draw_pixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= g_screen_w || y < 0 || y >= g_screen_h || !g_backbuffer) return;
    g_backbuffer[y * g_screen_w + x] = blend_pixel(g_backbuffer[y * g_screen_w + x], color);
}

static void draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g_backbuffer) return;
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = x + w > g_screen_w ? g_screen_w : x + w;
    int y2 = y + h > g_screen_h ? g_screen_h : y + h;
    for (int cy = y1; cy < y2; cy++) {
        uint32_t *row = &g_backbuffer[cy * g_screen_w + x1];
        for (int cx = x1; cx < x2; cx++) {
            *row = blend_pixel(*row, color);
            row++;
        }
    }
}

static void draw_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness) {
    draw_rect(x, y, w, thickness, color);
    draw_rect(x, y + h - thickness, w, thickness, color);
    draw_rect(x, y, thickness, h, color);
    draw_rect(x + w - thickness, y, thickness, h, color);
}

static void draw_rounded_rect(int x, int y, int w, int h, int r, uint32_t color) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    draw_rect(x + r, y, w - 2 * r, h, color);
    draw_rect(x, y + r, r, h - 2 * r, color);
    draw_rect(x + w - r, y + r, r, h - 2 * r, color);
}

// Render crisp mini Thai Flag icon (5 stripes)
static void draw_thai_flag(int x, int y, int w, int h) {
    int h1 = h / 6;
    if (h1 < 1) h1 = 1;
    int h2 = h - 4 * h1;
    draw_rect(x, y, w, h1, 0xFFEF4444);              // Red
    draw_rect(x, y + h1, w, h1, 0xFFFFFFFF);         // White
    draw_rect(x, y + 2 * h1, w, h2, 0xFF1E3A8A);     // Blue
    draw_rect(x, y + 2 * h1 + h2, w, h1, 0xFFFFFFFF);// White
    draw_rect(x, y + 3 * h1 + h2, w, h1, 0xFFEF4444);// Red
    draw_rect_outline(x, y, w, h, 0x44FFFFFF, 1);
}

// Text rendering using stb_truetype
static int draw_char(int x, int y, uint32_t codepoint, float size, uint32_t color) {
    if (!g_font_buffer) return 0;
    int glyph = stbtt_FindGlyphIndex(&g_font, codepoint);
    if (glyph == 0 && codepoint != ' ') return 0; // Skip missing glyphs (.notdef tofu box)
    float scale = stbtt_ScaleForPixelHeight(&g_font, size);
    int advance, lsb, x0, y0, x1, y1;
    stbtt_GetCodepointHMetrics(&g_font, codepoint, &advance, &lsb);
    stbtt_GetCodepointBitmapBox(&g_font, codepoint, scale, scale, &x0, &y0, &x1, &y1);

    int w = x1 - x0;
    int h = y1 - y0;
    if (w > 0 && h > 0) {
        unsigned char *bitmap = malloc(w * h);
        stbtt_MakeCodepointBitmap(&g_font, bitmap, w, h, w, scale, scale, codepoint);
        for (int j = 0; j < h; j++) {
            int py = y + y0 + j;
            if (py < 0 || py >= g_screen_h) continue;
            for (int i = 0; i < w; i++) {
                int px = x + x0 + i;
                if (px < 0 || px >= g_screen_w) continue;
                unsigned char alpha = bitmap[j * w + i];
                if (alpha > 0) {
                    uint32_t c = (alpha << 24) | (color & 0x00FFFFFF);
                    draw_pixel(px, py, c);
                }
            }
        }
        free(bitmap);
    }
    return (int)(advance * scale);
}

static void draw_text(int x, int y, const char *text, float size, uint32_t color) {
    if (!text || !g_font_buffer) return;
    const char *p = text;
    int cur_x = x;
    while (*p) {
        uint32_t cp = next_utf8(&p);
        if (cp == '\n') break;
        cur_x += draw_char(cur_x, y, cp, size, color);
    }
}

static void draw_truncated_text(int x, int y, const char *text, float size, uint32_t color, int max_w) {
    if (!text || !g_font_buffer) return;
    const char *p = text;
    int cur_x = x;
    while (*p) {
        if (cur_x >= x + max_w - 24) {
            draw_text(cur_x, y, "...", size, color);
            break;
        }
        uint32_t cp = next_utf8(&p);
        if (cp == '\n') break;
        cur_x += draw_char(cur_x, y, cp, size, color);
    }
}

// Image scaling & rendering
static void draw_image_scaled(int dst_x, int dst_y, int dst_w, int dst_h, const uint32_t *src, int src_w, int src_h) {
    if (!src || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || !g_backbuffer) return;
    for (int y = 0; y < dst_h; y++) {
        int py = dst_y + y;
        if (py < 0 || py >= g_screen_h) continue;
        int src_y = (y * src_h) / dst_h;
        const uint32_t *src_row = &src[src_y * src_w];
        for (int x = 0; x < dst_w; x++) {
            int px = dst_x + x;
            if (px < 0 || px >= g_screen_w) continue;
            int src_x = (x * src_w) / dst_w;
            g_backbuffer[py * g_screen_w + px] = blend_pixel(g_backbuffer[py * g_screen_w + px], src_row[src_x]);
        }
    }
}

// Image loader from file using stb_image
static uint32_t *load_image_file(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    unsigned char *data = malloc(sz);
    if (!data) { fclose(f); return NULL; }
    if (fread(data, 1, sz, f) != (size_t)sz) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    int comp;
    unsigned char *img = stbi_load_from_memory(data, sz, w, h, &comp, 4);
    free(data);
    if (!img) return NULL;

    uint32_t *argb = malloc((*w) * (*h) * sizeof(uint32_t));
    if (!argb) {
        stbi_image_free(img);
        return NULL;
    }
    for (int i = 0; i < (*w) * (*h); i++) {
        uint32_t r = img[i * 4 + 0];
        uint32_t g = img[i * 4 + 1];
        uint32_t b = img[i * 4 + 2];
        uint32_t a = img[i * 4 + 3];
        argb[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    stbi_image_free(img);
    return argb;
}

// Flip backbuffer to /dev/fb0 with automatic format conversion
static void flip_screen(void) {
    if (!g_fb_mem || !g_backbuffer) return;

    for (int y = 0; y < g_screen_h; y++) {
        uint8_t *dst_row = g_fb_mem + y * g_line_length;
        const uint32_t *src_row = &g_backbuffer[y * g_screen_w];

        if (g_bpp == 16) {
            uint16_t *dst16 = (uint16_t *)dst_row;
            for (int x = 0; x < g_screen_w; x++) {
                uint32_t c = src_row[x];
                uint16_t r = (c >> 19) & 0x1F;
                uint16_t g = (c >> 10) & 0x3F;
                uint16_t b = (c >> 3)  & 0x1F;
                if (g_rgb_swap) {
                    dst16[x] = (b << 11) | (g << 5) | r;
                } else {
                    dst16[x] = (r << 11) | (g << 5) | b;
                }
            }
        } else if (g_bpp == 32) {
            uint32_t *dst32 = (uint32_t *)dst_row;
            if (g_rgb_swap) {
                for (int x = 0; x < g_screen_w; x++) {
                    uint32_t c = src_row[x];
                    uint32_t a = c & 0xFF000000;
                    uint32_t r = (c >> 16) & 0xFF;
                    uint32_t g = (c >> 8)  & 0xFF;
                    uint32_t b = c & 0xFF;
                    dst32[x] = a | (b << 16) | (g << 8) | r;
                }
            } else {
                memcpy(dst32, src_row, g_screen_w * sizeof(uint32_t));
            }
        } else if (g_bpp == 24) {
            uint8_t *dst24 = dst_row;
            for (int x = 0; x < g_screen_w; x++) {
                uint32_t c = src_row[x];
                if (g_rgb_swap) {
                    dst24[x * 3 + 0] = (c >> 16) & 0xFF;
                    dst24[x * 3 + 1] = (c >> 8)  & 0xFF;
                    dst24[x * 3 + 2] = c & 0xFF;
                } else {
                    dst24[x * 3 + 0] = c & 0xFF;
                    dst24[x * 3 + 1] = (c >> 8)  & 0xFF;
                    dst24[x * 3 + 2] = (c >> 16) & 0xFF;
                }
            }
        }
    }
}

// Save config to config.txt (unconditionally writes & syncs to flash)
static void save_config(const char *mode, const char *local_url, const char *online_url) {
    const char *paths[] = {
        "config.txt",
        "./config.txt",
        "/mnt/SDCARD/Tools/tg5040/ThaiRomStore.pak/config.txt",
        "/mnt/sdcard/Tools/tg5040/ThaiRomStore.pak/config.txt",
        "/mnt/SDCARD/Tools/ThaiRomStore.pak/config.txt",
        "/mnt/sdcard/Tools/ThaiRomStore.pak/config.txt",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "w");
        if (f) {
            fprintf(f, "# Mirai ROM Store Config\n");
            fprintf(f, "MODE=%s\n", mode ? mode : "AUTO");
            fprintf(f, "LOCAL_URL=%s\n", local_url ? local_url : "");
            fprintf(f, "ONLINE_URL=%s\n", online_url ? online_url : "");
            fprintf(f, "SERVER_URL=%s\n", local_url ? local_url : "");
            fclose(f);
        }
    }
    sync();
}

// Find appropriate ROM and Cover destinations on SD card
static void get_rom_paths(const char *system, const char *filename,
                          char *out_rom_path, size_t rom_len,
                          char *out_cover_path, size_t cover_len) {
    char base_sd[64] = "/mnt/SDCARD";
    struct stat st;
    if (stat("/mnt/SDCARD/Roms", &st) == 0 && S_ISDIR(st.st_mode)) {
        strcpy(base_sd, "/mnt/SDCARD");
    } else if (stat("/mnt/sdcard/Roms", &st) == 0 && S_ISDIR(st.st_mode)) {
        strcpy(base_sd, "/mnt/sdcard");
    } else if (stat("/mnt/sdcard", &st) == 0) {
        strcpy(base_sd, "/mnt/sdcard");
    }

    const char *chosen_folder = system;
    for (int i = 0; g_system_dirs[i].full_name; i++) {
        if (strcmp(system, g_system_dirs[i].full_name) == 0) {
            char test_path[512];
            // Check short name (e.g. NDS, GBA)
            snprintf(test_path, sizeof(test_path), "%s/Roms/%s", base_sd, g_system_dirs[i].short_name);
            if (stat(test_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                chosen_folder = g_system_dirs[i].short_name;
                break;
            }
            // Check full name (e.g. Nintendo DS (NDS))
            snprintf(test_path, sizeof(test_path), "%s/Roms/%s", base_sd, g_system_dirs[i].full_name);
            if (stat(test_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                chosen_folder = g_system_dirs[i].full_name;
                break;
            }
            // Check alt name
            if (g_system_dirs[i].alt_name) {
                snprintf(test_path, sizeof(test_path), "%s/Roms/%s", base_sd, g_system_dirs[i].alt_name);
                if (stat(test_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                    chosen_folder = g_system_dirs[i].alt_name;
                    break;
                }
            }
            chosen_folder = g_system_dirs[i].full_name;
            break;
        }
    }

    snprintf(out_rom_path, rom_len, "%s/Roms/%s/%s", base_sd, chosen_folder, filename);

    // Cover art stem
    char stem[256];
    strncpy(stem, filename, sizeof(stem) - 1);
    stem[sizeof(stem) - 1] = '\0';
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';

    snprintf(out_cover_path, cover_len, "%s/Roms/%s/.media/%s.png", base_sd, chosen_folder, stem);
}

// Check if a ROM is already installed on SD card
static int check_rom_installed(const char *system, const char *filename) {
    char rom_path[512];
    char cover_path[512];
    get_rom_paths(system, filename, rom_path, sizeof(rom_path), cover_path, sizeof(cover_path));
    struct stat st;
    if (stat(rom_path, &st) == 0 && st.st_size > 0) return 1;

    for (int i = 0; g_system_dirs[i].full_name; i++) {
        if (strcmp(system, g_system_dirs[i].full_name) == 0) {
            char alt_path[512];
            snprintf(alt_path, sizeof(alt_path), "/mnt/SDCARD/Roms/%s/%s", g_system_dirs[i].short_name, filename);
            if (stat(alt_path, &st) == 0 && st.st_size > 0) return 1;
            snprintf(alt_path, sizeof(alt_path), "/mnt/sdcard/Roms/%s/%s", g_system_dirs[i].short_name, filename);
            if (stat(alt_path, &st) == 0 && st.st_size > 0) return 1;
            break;
        }
    }
    return 0;
}

// Accurately match console system for filtering (prevents PSX from matching PSP)
static bool system_matches_filter(const char *sys, const char *filter) {
    if (!sys || !filter) return false;
    if (strcmp(filter, "ALL") == 0) return true;

    // PSX / PlayStation 1 (Strictly exclude PSP / Portable)
    if (strcmp(filter, "PS") == 0 || strcmp(filter, "PSX") == 0 || strcmp(filter, "PS1") == 0) {
        if (strstr(sys, "PSP") || strstr(sys, "Portable")) return false;
        return (strstr(sys, "(PS)") != NULL || strstr(sys, "PlayStation") != NULL || strstr(sys, "PSX") != NULL);
    }

    // PSP
    if (strcmp(filter, "PSP") == 0) {
        return (strstr(sys, "PSP") != NULL || strstr(sys, "Portable") != NULL);
    }

    // NDS
    if (strcmp(filter, "NDS") == 0 || strcmp(filter, "DS") == 0) {
        return (strstr(sys, "NDS") != NULL || strstr(sys, "Nintendo DS") != NULL);
    }

    // GBA
    if (strcmp(filter, "GBA") == 0) {
        return (strstr(sys, "GBA") != NULL || strstr(sys, "Game Boy Advance") != NULL);
    }

    // SFC / SNES
    if (strcmp(filter, "SFC") == 0 || strcmp(filter, "SNES") == 0) {
        return (strstr(sys, "SFC") != NULL || strstr(sys, "SNES") != NULL || strstr(sys, "Super Nintendo") != NULL);
    }

    // FC / NES
    if (strcmp(filter, "FC") == 0 || strcmp(filter, "NES") == 0) {
        return (strstr(sys, "FC") != NULL || strstr(sys, "NES") != NULL || strstr(sys, "Famicom") != NULL);
    }

    // MD / Genesis
    if (strcmp(filter, "MD") == 0 || strcmp(filter, "GENESIS") == 0) {
        return (strstr(sys, "MD") != NULL || strstr(sys, "Genesis") != NULL || strstr(sys, "Mega Drive") != NULL);
    }

    // GB (Original Game Boy - Strictly exclude GBC / GBA)
    if (strcmp(filter, "GB") == 0) {
        if (strstr(sys, "GBC") || strstr(sys, "GBA") || strstr(sys, "Color") || strstr(sys, "Advance")) return false;
        return (strstr(sys, "(GB)") != NULL || strstr(sys, "Game Boy") != NULL);
    }

    // GBC
    if (strcmp(filter, "GBC") == 0) {
        return (strstr(sys, "GBC") != NULL || strstr(sys, "Game Boy Color") != NULL);
    }

    return (strstr(sys, filter) != NULL);
}

// Apply console filters
static void apply_filter(void) {
    g_filtered_count = 0;
    for (int i = 0; i < g_game_count; i++) {
        if (system_matches_filter(g_games[i].system, g_cur_filter)) {
            g_filtered_indices[g_filtered_count++] = i;
        }
    }
    if (g_selected_index >= g_filtered_count) g_selected_index = g_filtered_count - 1;
    if (g_selected_index < 0) g_selected_index = 0;
    g_top_index = (g_selected_index / ROWS_PER_PAGE) * ROWS_PER_PAGE;
}

// Load catalog from /tmp/catalog.txt
static void load_catalog(void) {
    FILE *f = fopen("/tmp/catalog.txt", "r");
    if (!f) {
        g_game_count = 0;
        apply_filter();
        return;
    }
    char line[1024];
    g_game_count = 0;
    while (fgets(line, sizeof(line), f) && g_game_count < MAX_GAMES) {
        char *nl = strchr(line, '\r'); if (nl) *nl = '\0';
        nl = strchr(line, '\n'); if (nl) *nl = '\0';
        if (strlen(line) == 0) continue;

        // Parse: system|filename|title|has_cover|rom_url|cover_url|size_mb|is_thai
        char *tok = strtok(line, "|");
        if (!tok) continue;
        strncpy(g_games[g_game_count].system, tok, sizeof(g_games[0].system) - 1);

        tok = strtok(NULL, "|"); if (!tok) continue;
        strncpy(g_games[g_game_count].filename, tok, sizeof(g_games[0].filename) - 1);

        tok = strtok(NULL, "|"); if (!tok) continue;
        strncpy(g_games[g_game_count].title, tok, sizeof(g_games[0].title) - 1);

        tok = strtok(NULL, "|"); if (!tok) continue; // has_cover

        tok = strtok(NULL, "|"); if (!tok) continue;
        strncpy(g_games[g_game_count].rom_url, tok, sizeof(g_games[0].rom_url) - 1);

        tok = strtok(NULL, "|"); if (!tok) continue;
        strncpy(g_games[g_game_count].cover_url, tok, sizeof(g_games[0].cover_url) - 1);

        tok = strtok(NULL, "|"); if (!tok) continue;
        strncpy(g_games[g_game_count].size_mb, tok, sizeof(g_games[0].size_mb) - 1);

        tok = strtok(NULL, "|"); if (!tok) continue;
        g_games[g_game_count].is_thai = atoi(tok);

        g_games[g_game_count].installed = check_rom_installed(g_games[g_game_count].system, g_games[g_game_count].filename);
        g_game_count++;
    }
    fclose(f);
    g_connected = (g_game_count > 0);
    if (g_game_count == 0) {
        if (g_cover_pixels) {
            free(g_cover_pixels);
            g_cover_pixels = NULL;
        }
        g_cover_loaded_index = -1;
    }
    apply_filter();
}

// Test server connection via /api/info
static bool test_server_connection(const char *url) {
    if (!url || strlen(url) == 0) return false;
    char cmd[512];
    unlink("/tmp/test_info.json");
    snprintf(cmd, sizeof(cmd), "bin/curl -s -m 2.5 \"%s/api/info\" -o /tmp/test_info.json", url);
    if (system(cmd) == 0) {
        FILE *f = fopen("/tmp/test_info.json", "r");
        if (f) {
            char buf[512];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = '\0';
            if (strstr(buf, "server_ip") || strstr(buf, "supported_systems")) {
                return true;
            }
        }
    }
    return false;
}

// Sync catalog from server
static bool sync_catalog_from_server(const char *url) {
    if (!url || strlen(url) == 0) return false;
    char cmd[512];
    unlink("/tmp/catalog.txt");
    snprintf(cmd, sizeof(cmd), "bin/curl -L -s -m 7 \"%s/api/catalog/sync\" -o /tmp/catalog.txt", url);
    if (system(cmd) == 0) {
        struct stat st;
        if (stat("/tmp/catalog.txt", &st) == 0 && st.st_size > 10) {
            load_catalog();
            return (g_game_count > 0);
        }
    }
    return false;
}

// Load cover image for current selected game
static void update_cover_preview(void) {
    if (g_filtered_count == 0) return;
    int real_idx = g_filtered_indices[g_selected_index];
    if (g_cover_loaded_index == real_idx) return;

    if (g_cover_pixels) {
        free(g_cover_pixels);
        g_cover_pixels = NULL;
    }
    g_cover_loaded_index = real_idx;

    char rom_path[512];
    char sd_cover[512];
    get_rom_paths(g_games[real_idx].system, g_games[real_idx].filename,
                  rom_path, sizeof(rom_path), sd_cover, sizeof(sd_cover));

    char cover_path[512];
    snprintf(cover_path, sizeof(cover_path), "/tmp/covers/%s.png", g_games[real_idx].title);

    struct stat st;
    if (stat(sd_cover, &st) == 0 && st.st_size > 0) {
        g_cover_pixels = load_image_file(sd_cover, &g_cover_w, &g_cover_h);
        return;
    }

    if (stat(cover_path, &st) == 0 && st.st_size > 0) {
        g_cover_pixels = load_image_file(cover_path, &g_cover_w, &g_cover_h);
        return;
    }

    // Fetch from server with short timeout
    if (strlen(g_games[real_idx].cover_url) > 0) {
        char cmd[1024];
        char fetch_url[512];
        mkdir("/tmp/covers", 0777);
        const char *cover_path_api = strstr(g_games[real_idx].cover_url, "/api/download/");
        if (cover_path_api) {
            snprintf(fetch_url, sizeof(fetch_url), "%s%s", g_server_url, cover_path_api);
        } else {
            snprintf(fetch_url, sizeof(fetch_url), "%s", g_games[real_idx].cover_url);
        }
        snprintf(cmd, sizeof(cmd), "bin/curl -s -m 2 -L \"%s\" -o \"%s\"", fetch_url, cover_path);
        if (system(cmd) == 0) {
            g_cover_pixels = load_image_file(cover_path, &g_cover_w, &g_cover_h);
        }
    }
}

// Draw console badge (Enlarged)
static void draw_console_badge(int x, int y, const char *sys) {
    uint32_t badge_color = COLOR_HIGHLIGHT;
    const char *tag = "ROM";
    if (strstr(sys, "GBA")) { badge_color = 0xFF059669; tag = "GBA"; }
    else if (strstr(sys, "PSP")) { badge_color = 0xFF2563EB; tag = "PSP"; }
    else if (strstr(sys, "PS")) { badge_color = 0xFF7C3AED; tag = "PSX"; }
    else if (strstr(sys, "NDS")) { badge_color = 0xFFD97706; tag = "NDS"; }
    else if (strstr(sys, "SFC") || strstr(sys, "SNES")) { badge_color = 0xFFDC2626; tag = "SFC"; }
    else if (strstr(sys, "FC") || strstr(sys, "NES")) { badge_color = 0xFFDC2626; tag = "FC"; }
    else if (strstr(sys, "GB")) { badge_color = 0xFF4B5563; tag = "GB"; }

    draw_rounded_rect(x, y, 78, 36, 6, badge_color);
    draw_text(x + 14, y + 26, tag, 18.0f, COLOR_WHITE);
}

// Parse IP octets and port from URL string
static void parse_ip_from_url(const char *url, int octets[4], int *port) {
    octets[0] = 192; octets[1] = 168; octets[2] = 1; octets[3] = 118;
    *port = 8080;
    if (!url || strlen(url) == 0) return;
    const char *p = strstr(url, "://");
    if (p) p += 3; else p = url;
    int a, b, c, d, pt = 8080;
    if (sscanf(p, "%d.%d.%d.%d:%d", &a, &b, &c, &d, &pt) >= 4) {
        octets[0] = a; octets[1] = b; octets[2] = c; octets[3] = d;
        if (pt > 0) *port = pt;
    }
}

// Render Offline / Disconnected State UI
static void render_offline_ui(void) {
    draw_rect(0, 0, g_screen_w, g_screen_h, COLOR_BG);

    // Header Bar
    draw_rect(0, 0, g_screen_w, 72, COLOR_PANEL);
    draw_rect(0, 72, g_screen_w, 1, COLOR_CARD_BORDER);

    if (g_mascot_pixels) {
        draw_image_scaled(14, 8, 56, 56, g_mascot_pixels, g_mascot_w, g_mascot_h);
    }
    draw_text(82, 34, "THAI ROM STORE  v" APP_VERSION, 28.0f, COLOR_WHITE);
    draw_text(82, 60, "Mirai Guild | Rushing Translation Edition", 16.0f, COLOR_CYAN);

    // Status pill (Offline)
    int pill_w = 340;
    int pill_x = g_screen_w - pill_w - 14;
    draw_rounded_rect(pill_x, 15, pill_w, 42, 21, 0xFF451A1A);
    draw_rect_outline(pill_x, 15, pill_w, 42, COLOR_RED, 1);
    draw_rounded_rect(pill_x + 18, 30, 10, 10, 5, COLOR_RED);
    draw_text(pill_x + 36, 42, "ยังไม่ได้เชื่อมต่อกับ Server", 17.0f, 0xFFFCA5A5);

    // Center Dialog Box
    int box_w = 780;
    int box_h = 490;
    int box_x = (g_screen_w - box_w) / 2;
    int box_y = 105;

    draw_rounded_rect(box_x, box_y, box_w, box_h, 16, COLOR_PANEL);
    draw_rect_outline(box_x, box_y, box_w, box_h, COLOR_CARD_BORDER, 2);

    draw_text(box_x + 40, box_y + 58, "ยินดีต้อนรับสู่ THAI ROM STORE", 28.0f, COLOR_WHITE);
    draw_text(box_x + 40, box_y + 98, "ขณะนี้เครื่องยังไม่สามารถเชื่อมต่อกับคอมพิวเตอร์ได้", 19.0f, COLOR_GRAY);

    // Current URL Display Box
    int ip_box_y = box_y + 125;
    draw_rounded_rect(box_x + 40, ip_box_y, box_w - 80, 56, 10, COLOR_PANEL_ALT);
    draw_rect_outline(box_x + 40, ip_box_y, box_w - 80, 56, COLOR_CARD_BORDER, 1);
    char cur_str[256];
    snprintf(cur_str, sizeof(cur_str), "URL ปัจจุบัน:  %s", g_server_url);
    draw_text(box_x + 60, ip_box_y + 36, cur_str, 22.0f, COLOR_CYAN);

    // Action Prompts
    int btn_w = box_w - 80;
    int b1_y = ip_box_y + 78;
    draw_rounded_rect(box_x + 40, b1_y, btn_w, 68, 12, COLOR_HIGHLIGHT);
    draw_rect_outline(box_x + 40, b1_y, btn_w, 68, COLOR_HIGHLIGHT_BORDER, 2);
    draw_text(box_x + 50, b1_y + 42, "[SELECT / A]  ตั้งค่า IP คอมพิวเตอร์ / ค้นหา Wi-Fi อัตโนมัติ", 22.0f, COLOR_WHITE);

    int b2_y = b1_y + 86;
    draw_rounded_rect(box_x + 40, b2_y, btn_w, 60, 10, COLOR_GREEN_BG);
    draw_rect_outline(box_x + 40, b2_y, btn_w, 60, COLOR_GREEN, 1);
    draw_text(box_x + 50, b2_y + 38, "[Y]  กดเพื่อลองเชื่อมต่อใหม่อีกครั้ง", 20.0f, COLOR_GREEN);

    draw_text(box_x + 40, box_y + box_h - 26, "* ตรวจสอบว่าเปิด server.py ในคอมพิวเตอร์และต่อ Wi-Fi วงเดียวกันแล้ว", 16.0f, COLOR_AMBER);

    // Footer Bar
    int footer_h = 56;
    int fy = g_screen_h - footer_h;
    draw_rect(0, fy, g_screen_w, footer_h, COLOR_PANEL);
    draw_rect(0, fy, g_screen_w, 1, COLOR_CARD_BORDER);

    draw_text(24, fy + 36, "[SELECT/A] ตั้งค่า IP/URL", 18.0f, COLOR_WHITE);
    draw_text(290, fy + 36, "[Y] ลองเชื่อมต่อใหม่", 18.0f, COLOR_AMBER);
    draw_text(g_screen_w - 230, fy + 36, "[B/MENU] ออกสู่ NextUI", 18.0f, COLOR_RED);

    flip_screen();
}

// Forward declarations
static void open_settings_menu(void);
static void close_inputs(void);
static void close_fb(void);

// Render Main UI (With Enlarged Typography for 324 PPI)
static void render_ui(void) {
    if (!g_backbuffer) return;

    if (g_game_count == 0) {
        render_offline_ui();
        return;
    }

    draw_rect(0, 0, g_screen_w, g_screen_h, COLOR_BG);

    // Header Bar (Y: 0 to 72)
    draw_rect(0, 0, g_screen_w, 72, COLOR_PANEL);
    draw_rect(0, 72, g_screen_w, 1, COLOR_CARD_BORDER);

    if (g_mascot_pixels) {
        draw_image_scaled(14, 8, 56, 56, g_mascot_pixels, g_mascot_w, g_mascot_h);
    }
    draw_text(82, 34, "THAI ROM STORE  v" APP_VERSION, 28.0f, COLOR_WHITE);
    draw_text(82, 60, "Mirai Guild | Rushing Translation Edition", 16.0f, COLOR_CYAN);

    // Server IP & Game Count Pill
    char status_str[128];
    snprintf(status_str, sizeof(status_str), "%s  |  %d เกม", g_server_url, g_filtered_count);
    int pill_w = 340;
    int pill_x = g_screen_w - pill_w - 14;
    draw_rounded_rect(pill_x, 15, pill_w, 42, 21, COLOR_PANEL_ALT);
    draw_rect_outline(pill_x, 15, pill_w, 42, COLOR_CARD_BORDER, 1);
    draw_rounded_rect(pill_x + 16, 31, 10, 10, 5, COLOR_GREEN);
    draw_text(pill_x + 34, 42, status_str, 17.0f, COLOR_GREEN);

    // Layout configuration
    int list_x = 16;
    int list_y = 82;
    int footer_h = 56;

    int list_w = (g_screen_w >= 1200) ? 730 : 580;
    int card_x = list_x + list_w + 16;
    int card_w = g_screen_w - card_x - 16;

    int available_h = g_screen_h - list_y - footer_h - 10;
    int row_h = (available_h - 8 * (ROWS_PER_PAGE - 1)) / ROWS_PER_PAGE;
    if (row_h < 86) row_h = 86;

    // Left Column: Game List (Larger Text)
    for (int i = 0; i < ROWS_PER_PAGE; i++) {
        int idx = g_top_index + i;
        if (idx >= g_filtered_count) break;
        int real_idx = g_filtered_indices[idx];
        GameItem *item = &g_games[real_idx];

        int cy = list_y + i * (row_h + 8);
        bool is_selected = (idx == g_selected_index);

        if (is_selected) {
            draw_rounded_rect(list_x, cy, list_w, row_h, 10, COLOR_HIGHLIGHT);
            draw_rect_outline(list_x, cy, list_w, row_h, COLOR_HIGHLIGHT_BORDER, 3);
        } else {
            draw_rounded_rect(list_x, cy, list_w, row_h, 10, COLOR_PANEL);
            draw_rect_outline(list_x, cy, list_w, row_h, COLOR_CARD_BORDER, 1);
        }

        // Console Tag
        int badge_y = cy + 12;
        draw_console_badge(list_x + 12, badge_y, item->system);

        // Thai translation pill with real rendered mini Thai flag
        if (item->is_thai) {
            draw_rounded_rect(list_x + 98, badge_y, 114, 36, 6, COLOR_GREEN_BG);
            draw_thai_flag(list_x + 106, badge_y + 11, 20, 14);
            draw_text(list_x + 132, badge_y + 26, "แปลไทย", 17.0f, COLOR_GREEN);
        }

        // Title (Larger Font: 25.0f)
        int title_x = item->is_thai ? list_x + 222 : list_x + 100;
        int max_title_w = list_w - title_x - 140;
        uint32_t title_color = is_selected ? COLOR_WHITE : 0xFFF8FAFC;
        draw_truncated_text(title_x, cy + 38, item->title, 25.0f, title_color, max_title_w);

        // Subtitle (Larger Font: 18.0f)
        char sub_str[128];
        snprintf(sub_str, sizeof(sub_str), "%s   |   %s", item->size_mb, item->system);
        draw_truncated_text(title_x, cy + 76, sub_str, 18.0f, is_selected ? 0xFFBAE6FD : COLOR_TEXT_DIM, max_title_w);

        // Status pill on the right
        int pill_btn_w = 130;
        int pill_btn_x = list_x + list_w - pill_btn_w - 12;
        int pill_btn_y = cy + (row_h - 42) / 2;
        if (item->installed) {
            draw_rounded_rect(pill_btn_x, pill_btn_y, pill_btn_w, 42, 8, 0xFF064E3B);
            draw_text(pill_btn_x + 22, pill_btn_y + 28, "มีในเครื่อง", 17.0f, COLOR_GREEN);
        } else {
            draw_rounded_rect(pill_btn_x, pill_btn_y, pill_btn_w, 42, 8, is_selected ? 0xFF1D4ED8 : COLOR_PANEL_ALT);
            draw_text(pill_btn_x + 24, pill_btn_y + 28, "[A] โหลด", 17.0f, is_selected ? COLOR_WHITE : COLOR_GRAY);
        }
    }

    // Right Column: Showcase Card (Larger Text)
    int card_y = list_y;
    int card_h = available_h;

    draw_rounded_rect(card_x, card_y, card_w, card_h, 14, COLOR_PANEL);
    draw_rect_outline(card_x, card_y, card_w, card_h, COLOR_CARD_BORDER, 1);

    if (g_filtered_count > 0 && g_selected_index < g_filtered_count) {
        int real_idx = g_filtered_indices[g_selected_index];
        GameItem *cur = &g_games[real_idx];

        int box_x = card_x + 16;
        int box_y = card_y + 16;
        int box_w = card_w - 32;
        int box_h = (card_h * 52) / 100;

        draw_rounded_rect(box_x, box_y, box_w, box_h, 10, COLOR_PANEL_ALT);

        if (g_cover_pixels && g_cover_w > 0 && g_cover_h > 0) {
            float aspect = (float)g_cover_w / (float)g_cover_h;
            int draw_w = box_w;
            int draw_h = (int)(draw_w / aspect);
            if (draw_h > box_h) {
                draw_h = box_h;
                draw_w = (int)(draw_h * aspect);
            }
            int img_x = box_x + (box_w - draw_w) / 2;
            int img_y = box_y + (box_h - draw_h) / 2;
            draw_image_scaled(img_x, img_y, draw_w, draw_h, g_cover_pixels, g_cover_w, g_cover_h);
        } else {
            draw_text(box_x + (box_w / 2) - 75, box_y + (box_h / 2), "กำลังโหลดปก...", 20.0f, COLOR_GRAY);
        }

        // Details below cover
        int info_y = box_y + box_h + 30;
        draw_truncated_text(card_x + 16, info_y, cur->title, 26.0f, COLOR_WHITE, card_w - 32);
        draw_text(card_x + 16, info_y + 34, cur->system, 20.0f, COLOR_CYAN);

        char sz_info[128];
        snprintf(sz_info, sizeof(sz_info), "ขนาด: %s    แปล: %s",
                 cur->size_mb, cur->is_thai ? "ภาษาไทย" : "ต้นฉบับ");
        draw_text(card_x + 16, info_y + 64, sz_info, 18.0f, COLOR_GRAY);

        // Download Action Button
        int btn_h = 56;
        int btn_y = card_y + card_h - btn_h - 16;
        if (cur->installed) {
            draw_rounded_rect(card_x + 16, btn_y, card_w - 32, btn_h, 10, COLOR_GREEN_BG);
            draw_rect_outline(card_x + 16, btn_y, card_w - 32, btn_h, COLOR_GREEN, 1);
            draw_text(card_x + (card_w / 2) - 120, btn_y + 36, "มีในเครื่องแล้ว (กด A โหลดซ้ำ)", 18.0f, COLOR_GREEN);
        } else {
            draw_rounded_rect(card_x + 16, btn_y, card_w - 32, btn_h, 10, COLOR_HIGHLIGHT);
            draw_rect_outline(card_x + 16, btn_y, card_w - 32, btn_h, COLOR_HIGHLIGHT_BORDER, 1);
            draw_text(card_x + (card_w / 2) - 110, btn_y + 36, "[A] ดาวน์โหลดลงเครื่อง", 21.0f, COLOR_WHITE);
        }
    }

    // Footer Bar (Controls Hint)
    int fy = g_screen_h - footer_h;
    draw_rect(0, fy, g_screen_w, footer_h, COLOR_PANEL);
    draw_rect(0, fy, g_screen_w, 1, COLOR_CARD_BORDER);

    draw_text(16, fy + 36, "[D-Pad] เลือก", 18.0f, COLOR_GRAY);
    draw_text(150, fy + 36, "[A] โหลด", 18.0f, COLOR_WHITE);

    const char *filt_label = g_cur_filter;
    if (strcmp(g_cur_filter, "ALL") == 0) filt_label = "ทั้งหมด";
    char filter_btn[64];
    snprintf(filter_btn, sizeof(filter_btn), "[X] กรอง: %s", filt_label);
    draw_text(250, fy + 36, filter_btn, 18.0f, COLOR_CYAN);

    draw_text(420, fy + 36, "[Y] รีเฟรช", 18.0f, COLOR_AMBER);
    draw_text(550, fy + 36, "[SELECT] ตั้งค่า/อัปเดต", 18.0f, 0xFF38BDF8);
    draw_text(g_screen_w - 200, fy + 36, "[B/MENU] ออก", 18.0f, COLOR_RED);

    flip_screen();
}

// Download Modal Screen with Live Real-time Progress & Process Monitoring
static void execute_download(int filtered_idx) {
    if (filtered_idx < 0 || filtered_idx >= g_filtered_count) return;
    int real_idx = g_filtered_indices[filtered_idx];
    GameItem *item = &g_games[real_idx];

    flush_inputs();

    char rom_path[512];
    char cover_path[512];
    get_rom_paths(item->system, item->filename,
                  rom_path, sizeof(rom_path), cover_path, sizeof(cover_path));

    char rom_dir[512];
    char cover_dir[512];
    strncpy(rom_dir, rom_path, sizeof(rom_dir) - 1);
    char *s1 = strrchr(rom_dir, '/');
    if (s1) *s1 = '\0';

    strncpy(cover_dir, cover_path, sizeof(cover_dir) - 1);
    char *s2 = strrchr(cover_dir, '/');
    if (s2) *s2 = '\0';

    // Ensure target directories exist on both /mnt/SDCARD and /mnt/sdcard
    char mkdir_cmd[1024];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\" \"%s\"", rom_dir, cover_dir);
    system(mkdir_cmd);

    char part_path[512];
    snprintf(part_path, sizeof(part_path), "%s.part", rom_path);
    unlink(part_path);

    char dl_rom[512];
    char dl_cover[512];
    const char *rom_path_api = strstr(item->rom_url, "/api/download/");
    if (rom_path_api) {
        snprintf(dl_rom, sizeof(dl_rom), "%s%s", g_server_url, rom_path_api);
    } else {
        snprintf(dl_rom, sizeof(dl_rom), "%s", item->rom_url);
    }

    const char *cover_path_api = strstr(item->cover_url, "/api/download/");
    if (cover_path_api) {
        snprintf(dl_cover, sizeof(dl_cover), "%s%s", g_server_url, cover_path_api);
    } else {
        snprintf(dl_cover, sizeof(dl_cover), "%s", item->cover_url);
    }

    float target_mb = (float)atof(item->size_mb);
    if (target_mb <= 0.0f) target_mb = 1.0f;

    // Launch curl with output logging
    char launch_cmd[2048];
    unlink("/tmp/dl.pid");
    unlink("/tmp/curl_err.txt");
    snprintf(launch_cmd, sizeof(launch_cmd),
             "bin/curl -L --connect-timeout 8 \"%s\" -o \"%s\" > /tmp/curl_err.txt 2>&1 & echo $! > /tmp/dl.pid",
             dl_rom, part_path);
    system(launch_cmd);

    pid_t curl_pid = -1;
    for (int retry = 0; retry < 10; retry++) {
        usleep(50000);
        FILE *pf = fopen("/tmp/dl.pid", "r");
        if (pf) {
            int p = -1;
            if (fscanf(pf, "%d", &p) == 1 && p > 0) {
                curl_pid = (pid_t)p;
            }
            fclose(pf);
            if (curl_pid > 0) break;
        }
    }

    int modal_w = 720;
    int modal_h = 400;
    int modal_x = (g_screen_w - modal_w) / 2;
    int modal_y = (g_screen_h - modal_h) / 2;

    bool download_cancelled = false;
    bool download_success = false;
    int anim_frame = 0;

    while (curl_pid > 0) {
        if (kill(curl_pid, 0) != 0) {
            break;
        }

        UserAction act = poll_user_action(120);
        if (act == ACT_B || act == ACT_MENU) {
            kill(curl_pid, SIGTERM);
            download_cancelled = true;
            break;
        }

        long cur_bytes = 0;
        struct stat st;
        if (stat(part_path, &st) == 0) {
            cur_bytes = st.st_size;
        }
        float cur_mb = (float)cur_bytes / (1024.0f * 1024.0f);
        int percent = (int)((cur_mb / target_mb) * 100.0f);
        if (percent > 99) percent = 99;
        if (percent < 0) percent = 0;

        draw_rect(0, 0, g_screen_w, g_screen_h, 0xDD000000);
        draw_rounded_rect(modal_x, modal_y, modal_w, modal_h, 16, COLOR_PANEL);
        draw_rect_outline(modal_x, modal_y, modal_w, modal_h, COLOR_HIGHLIGHT_BORDER, 2);

        static const char *spinners[] = {"[ - ]", "[ \\ ]", "[ | ]", "[ / ]"};
        char header_str[128];
        snprintf(header_str, sizeof(header_str), "กำลังดาวน์โหลดเกม... %s", spinners[(anim_frame / 2) % 4]);
        draw_text(modal_x + 36, modal_y + 58, header_str, 28.0f, COLOR_WHITE);
        draw_truncated_text(modal_x + 36, modal_y + 104, item->title, 24.0f, COLOR_CYAN, modal_w - 72);

        char info_str[128];
        snprintf(info_str, sizeof(info_str), "เครื่อง: %s   |   ขนาดไฟล์: %s", item->system, item->size_mb);
        draw_text(modal_x + 36, modal_y + 148, info_str, 19.0f, COLOR_GRAY);

        int bar_x = modal_x + 36;
        int bar_y = modal_y + 190;
        int bar_w = modal_w - 72;
        int bar_h = 32;
        draw_rounded_rect(bar_x, bar_y, bar_w, bar_h, 8, COLOR_PANEL_ALT);

        int fill_w = (int)((bar_w * percent) / 100);
        if (fill_w > 0) {
            draw_rounded_rect(bar_x, bar_y, fill_w, bar_h, 8, COLOR_HIGHLIGHT);
        }

        char prog_str[128];
        snprintf(prog_str, sizeof(prog_str), "ดาวน์โหลดแล้ว: %.1f MB / %.1f MB (%d%%)", cur_mb, target_mb, percent);
        draw_text(bar_x, bar_y + 68, prog_str, 20.0f, COLOR_GREEN);

        draw_text(bar_x, modal_y + modal_h - 30, "[B] กดเพื่อยกเลิกการดาวน์โหลด", 17.0f, COLOR_AMBER);
        flip_screen();

        anim_frame++;
        usleep(100000);
    }

    struct stat final_st;
    if (!download_cancelled && stat(part_path, &final_st) == 0 && final_st.st_size > 1024) {
        rename(part_path, rom_path);
        download_success = true;

        char cover_cmd[1024];
        snprintf(cover_cmd, sizeof(cover_cmd), "bin/curl -s -m 5 -L \"%s\" -o \"%s\"", dl_cover, cover_path);
        system(cover_cmd);
    } else {
        unlink(part_path);
    }

    item->installed = check_rom_installed(item->system, item->filename);
    flush_inputs();

    // Result Modal Dialog
    draw_rect(0, 0, g_screen_w, g_screen_h, 0xDD000000);
    draw_rounded_rect(modal_x, modal_y, modal_w, modal_h, 16, COLOR_PANEL);

    if (download_success && item->installed) {
        draw_rect_outline(modal_x, modal_y, modal_w, modal_h, COLOR_GREEN, 3);
        draw_text(modal_x + 36, modal_y + 65, "[สำเร็จ] ดาวน์โหลดเรียบร้อยแล้ว!", 28.0f, COLOR_GREEN);
        draw_truncated_text(modal_x + 36, modal_y + 118, item->title, 24.0f, COLOR_WHITE, modal_w - 72);
        draw_text(modal_x + 36, modal_y + 168, "ติดตั้ง ROM และรูปปก Box Art ลงเครื่องเรียบร้อยแล้ว", 20.0f, COLOR_GRAY);

        int ok_btn_y = modal_y + modal_h - 80;
        draw_rounded_rect(modal_x + 36, ok_btn_y, modal_w - 72, 58, 12, COLOR_GREEN_BG);
        draw_rect_outline(modal_x + 36, ok_btn_y, modal_w - 72, 58, COLOR_GREEN, 1);
        draw_text(modal_x + (modal_w / 2) - 130, ok_btn_y + 38, "กด [A] หรือ [B] เพื่อกลับสู่เมนู", 20.0f, COLOR_WHITE);
    } else if (download_cancelled) {
        draw_rect_outline(modal_x, modal_y, modal_w, modal_h, COLOR_AMBER, 3);
        draw_text(modal_x + 36, modal_y + 65, "ยกเลิกการดาวน์โหลดแล้ว", 28.0f, COLOR_AMBER);
        draw_truncated_text(modal_x + 36, modal_y + 118, item->title, 24.0f, COLOR_WHITE, modal_w - 72);
        draw_text(modal_x + 36, modal_y + 168, "ไฟล์ดาวน์โหลดยังไม่สมบูรณ์และถูกยกเลิกแล้ว", 20.0f, COLOR_GRAY);

        int ok_btn_y = modal_y + modal_h - 80;
        draw_rounded_rect(modal_x + 36, ok_btn_y, modal_w - 72, 58, 12, COLOR_PANEL_ALT);
        draw_text(modal_x + (modal_w / 2) - 110, ok_btn_y + 38, "กด [B] เพื่อกลับสู่เมนู", 20.0f, COLOR_WHITE);
    } else {
        draw_rect_outline(modal_x, modal_y, modal_w, modal_h, COLOR_RED, 3);
        draw_text(modal_x + 36, modal_y + 65, "[ไม่สำเร็จ] ดาวน์โหลดล้มเหลว", 28.0f, COLOR_RED);
        draw_truncated_text(modal_x + 36, modal_y + 118, item->title, 24.0f, COLOR_WHITE, modal_w - 72);

        char err_detail[256] = "ไม่สามารถเชื่อมต่อ Server หรือเขียนไฟล์ลง SD Card ได้";
        FILE *ef = fopen("/tmp/curl_err.txt", "r");
        if (ef) {
            char line[256];
            while (fgets(line, sizeof(line), ef)) {
                if (strstr(line, "curl:") || strstr(line, "Failed") || strstr(line, "Error")) {
                    char *nl = strchr(line, '\r'); if (nl) *nl = '\0';
                    nl = strchr(line, '\n'); if (nl) *nl = '\0';
                    strncpy(err_detail, line, sizeof(err_detail) - 1);
                    break;
                }
            }
            fclose(ef);
        }
        draw_truncated_text(modal_x + 36, modal_y + 168, err_detail, 18.0f, COLOR_AMBER, modal_w - 72);

        int ok_btn_y = modal_y + modal_h - 80;
        draw_rounded_rect(modal_x + 36, ok_btn_y, modal_w - 72, 58, 12, 0xFF451A1A);
        draw_rect_outline(modal_x + 36, ok_btn_y, modal_w - 72, 58, COLOR_RED, 1);
        draw_text(modal_x + (modal_w / 2) - 130, ok_btn_y + 38, "กด [A] หรือ [B] เพื่อกลับสู่เมนู", 20.0f, COLOR_WHITE);
    }
    flip_screen();

    usleep(500000);
    flush_inputs();

    bool waiting = true;
    while (waiting) {
        UserAction act = poll_user_action(100);
        if (act == ACT_A || act == ACT_B || act == ACT_START) {
            waiting = false;
            break;
        }
    }

    usleep(300000);
    flush_inputs();
}

// -------------------------------------------------------------
// Interactive On-Screen Settings & IP/URL Editor
// -------------------------------------------------------------

typedef enum {
    SETTING_VIEW_MAIN,
    SETTING_VIEW_AUTOSCAN,
    SETTING_VIEW_LOCAL_IP,
    SETTING_VIEW_ONLINE_URL,
    SETTING_VIEW_OTA_UPDATE
} SettingView;

static void open_settings_menu(void) {
    flush_inputs();

    SettingView view = SETTING_VIEW_MAIN;
    int main_sel = 0; // 0: AutoScan, 1: LocalIP, 2: OnlineURL, 3: OTA Update, 4: Exit

    // OTA Update state
    char server_ver[64] = "";
    char server_build[64] = "";
    char server_changelog[512] = "";
    bool ver_checked = false;
    bool has_update = false;

    // Local IP state with on-screen Numpad
    int octets[4] = {192, 168, 1, 118};
    int ip_port = 8080;
    int octet_field = 3; // 0..3: IP octet, 4: Port
    bool octet_fresh[5] = {true, true, true, true, true};
    parse_ip_from_url(g_server_url, octets, &ip_port);

    int ip_area = 1; // 0: Octet boxes, 1: Numpad grid, 2: Actions
    int np_row = 0; // 0..3
    int np_col = 0; // 0..4
    int ip_act_idx = 0; // 0: Save, 1: Test, 2: Cancel

    static const char *np_labels[4][5] = {
        {"1", "2", "3", "+1", "+10"},
        {"4", "5", "6", "-1", "-10"},
        {"7", "8", "9", "0", "ลบ <"},
        {"< ก่อน", "ถัดไป >", "ล้าง", "Wi-Fi", ":8080"}
    };

    // Online URL state
    char url_edit[256];
    strncpy(url_edit, g_server_url, sizeof(url_edit) - 1);
    url_edit[sizeof(url_edit) - 1] = '\0';
    int kb_area = 1; // 0: shortcuts, 1: keys, 2: actions
    int kb_shortcut_idx = 0;
    int kb_row = 1;
    int kb_col = 0;
    int kb_action_idx = 2; // default to "Connect"

    static const char *kb_shortcuts[] = {"http://", "https://", ":8080", ".com", "192.168."};
    static const char *kb_rows[4] = {
        "1234567890-=",
        "qwertyuiop/.",
        "asdfghjkl:_@",
        "zxcvbnm?&%+~"
    };

    char message_notice[256] = "";
    uint32_t message_color = COLOR_GREEN;

    int modal_w = 780;
    int modal_h = 580;
    int modal_x = (g_screen_w - modal_w) / 2;
    int modal_y = (g_screen_h - modal_h) / 2;

    bool in_settings = true;

    while (in_settings) {
        draw_rect(0, 0, g_screen_w, g_screen_h, 0xEE000000);
        draw_rounded_rect(modal_x, modal_y, modal_w, modal_h, 16, COLOR_PANEL);
        draw_rect_outline(modal_x, modal_y, modal_w, modal_h, COLOR_CARD_BORDER, 2);

        if (view == SETTING_VIEW_MAIN) {
            draw_text(modal_x + 36, modal_y + 40, "การตั้งค่าระบบและการเชื่อมต่อ", 28.0f, COLOR_WHITE);
            draw_text(modal_x + 36, modal_y + 68, "เลือกตั้งค่าการเชื่อมต่อไปยัง PC Server หรือล้างแคชไฟล์", 17.0f, COLOR_GRAY);

            char cur_info[256];
            snprintf(cur_info, sizeof(cur_info), "URL: %s  (%s)  |  แอป: v%s",
                     g_server_url, g_connected ? "เชื่อมต่ออยู่" : "ไม่ได้เชื่อมต่อ", APP_VERSION);
            draw_text(modal_x + 36, modal_y + 96, cur_info, 17.0f, g_connected ? COLOR_GREEN : COLOR_AMBER);

            static const char *menu_titles[] = {
                "[1] ค้นหาคอมพิวเตอร์ใน Wi-Fi (Auto-Scan)",
                "[2] ปรับแต่งตัวเลข IP คอมพิวเตอร์ (Local IP)",
                "[3] พิมพ์ URL ออนไลน์ / คลาวด์ภายนอก",
                "[4] ตรวจสอบและอัปเดตโปรแกรม (OTA Update)",
                "[5] ล้างรูปภาพและแคชชั่วคราว (Clear Cache)",
                "[6] ย้อนกลับสู่หน้าหลัก"
            };
            static const char *menu_desc[] = {
                "สแกนหาคอมพิวเตอร์ที่เปิด server.py ในวงแลนเดียวกันอัตโนมัติ (รองรับ IP ทุกวง)",
                "พิมพ์หรือปรับตัวเลข IP ด้วยแป้นพิมพ์ตัวเลข (Numpad) บนหน้าจอ",
                "ใช้คีย์บอร์ดเสมือนพิมพ์โดเมนเนม หรือ Cloudflare Tunnel สำหรับเล่นนอกบ้าน",
                "เช็คเวอร์ชันใหม่จาก Server และดาวน์โหลดอัปเดตลงเครื่องได้ทันทีไม่ต้องถอด SD Card",
                "ล้างแคชรายชื่อเกมเก่าและรูปภาพปกชั่วคราวในเครื่องเพื่อรีเฟรชข้อมูลใหม่",
                "บันทึกและปิดหน้าต่างตั้งค่า"
            };

            for (int i = 0; i < 6; i++) {
                int iy = modal_y + 124 + i * 66;
                bool is_sel = (i == main_sel);
                if (is_sel) {
                    draw_rounded_rect(modal_x + 36, iy, modal_w - 72, 58, 10, COLOR_HIGHLIGHT);
                    draw_rect_outline(modal_x + 36, iy, modal_w - 72, 58, COLOR_HIGHLIGHT_BORDER, 2);
                } else {
                    draw_rounded_rect(modal_x + 36, iy, modal_w - 72, 58, 10, COLOR_PANEL_ALT);
                    draw_rect_outline(modal_x + 36, iy, modal_w - 72, 58, COLOR_CARD_BORDER, 1);
                }
                draw_text(modal_x + 52, iy + 26, menu_titles[i], 20.0f, is_sel ? COLOR_WHITE : 0xFFE2E8F0);
                draw_text(modal_x + 52, iy + 48, menu_desc[i], 14.0f, is_sel ? 0xFFBAE6FD : COLOR_GRAY);
            }

            if (strlen(message_notice) > 0) {
                draw_text(modal_x + 36, modal_y + modal_h - 18, message_notice, 16.0f, message_color);
            } else {
                draw_text(modal_x + 36, modal_y + modal_h - 18, "[D-Pad] เลือกตัวเลือก     [A] ตกลง     [B] ย้อนกลับ", 16.0f, COLOR_GRAY);
            }
        }
        else if (view == SETTING_VIEW_LOCAL_IP) {
            draw_text(modal_x + 36, modal_y + 42, "ปรับแต่งตัวเลข Local IP คอมพิวเตอร์", 26.0f, COLOR_WHITE);
            draw_text(modal_x + 36, modal_y + 68, "กดเลือกพิมพ์ตัวเลขจาก Numpad ด้านล่าง หรือใช้ปุ่ม [ขึ้น/ลง] ปรับเลขได้ทันที", 16.0f, COLOR_GRAY);

            int box_start_x = modal_x + 36;
            int box_y = modal_y + 92;
            int box_w = 95;
            int box_h = 50;

            for (int i = 0; i < 4; i++) {
                int bx = box_start_x + i * 115;
                bool is_box_focus = (octet_field == i);
                bool is_active_area = (ip_area == 0 && is_box_focus);

                if (is_active_area) {
                    draw_rounded_rect(bx, box_y, box_w, box_h, 8, COLOR_HIGHLIGHT);
                    draw_rect_outline(bx, box_y, box_w, box_h, COLOR_WHITE, 3);
                } else if (is_box_focus) {
                    draw_rounded_rect(bx, box_y, box_w, box_h, 8, COLOR_PANEL_ALT);
                    draw_rect_outline(bx, box_y, box_w, box_h, COLOR_GREEN, 2);
                } else {
                    draw_rounded_rect(bx, box_y, box_w, box_h, 8, COLOR_PANEL_ALT);
                    draw_rect_outline(bx, box_y, box_w, box_h, COLOR_CARD_BORDER, 1);
                }
                char num_str[32];
                snprintf(num_str, sizeof(num_str), "%d", octets[i]);
                int str_w = (int)strlen(num_str) * 16;
                draw_text(bx + (box_w - str_w) / 2, box_y + 34, num_str, 26.0f, is_box_focus ? COLOR_WHITE : 0xFFCBD5E1);

                if (i < 3) {
                    draw_text(bx + box_w + 8, box_y + 34, ".", 26.0f, COLOR_GRAY);
                }
            }

            int port_x = box_start_x + 4 * 115 + 10;
            int port_w = 125;
            bool is_port_focus = (octet_field == 4);
            bool is_port_active = (ip_area == 0 && is_port_focus);
            draw_text(port_x - 14, box_y + 34, ":", 26.0f, COLOR_GRAY);
            if (is_port_active) {
                draw_rounded_rect(port_x, box_y, port_w, box_h, 8, COLOR_HIGHLIGHT);
                draw_rect_outline(port_x, box_y, port_w, box_h, COLOR_WHITE, 3);
            } else if (is_port_focus) {
                draw_rounded_rect(port_x, box_y, port_w, box_h, 8, COLOR_PANEL_ALT);
                draw_rect_outline(port_x, box_y, port_w, box_h, COLOR_GREEN, 2);
            } else {
                draw_rounded_rect(port_x, box_y, port_w, box_h, 8, COLOR_PANEL_ALT);
                draw_rect_outline(port_x, box_y, port_w, box_h, COLOR_CARD_BORDER, 1);
            }
            char port_str[32];
            snprintf(port_str, sizeof(port_str), "%d", ip_port);
            draw_text(port_x + 20, box_y + 34, port_str, 26.0f, is_port_focus ? COLOR_WHITE : 0xFFCBD5E1);

            char full_test_url[256];
            snprintf(full_test_url, sizeof(full_test_url), "http://%d.%d.%d.%d:%d",
                     octets[0], octets[1], octets[2], octets[3], ip_port);
            draw_text(modal_x + 36, box_y + box_h + 24, "เป้าหมาย:", 16.0f, COLOR_GRAY);
            draw_text(modal_x + 115, box_y + box_h + 24, full_test_url, 18.0f, COLOR_CYAN);

            // Virtual Numpad Grid (4 rows x 5 cols)
            int np_start_y = box_y + box_h + 38;
            int np_key_w = 132;
            int np_key_h = 42;
            int np_spacing = 11;

            for (int r = 0; r < 4; r++) {
                int ry = np_start_y + r * (np_key_h + np_spacing);
                for (int c = 0; c < 5; c++) {
                    int kx = modal_x + 36 + c * (np_key_w + np_spacing);
                    bool is_sel = (ip_area == 1 && np_row == r && np_col == c);

                    uint32_t bg_col = COLOR_PANEL_ALT;
                    if (is_sel) bg_col = COLOR_HIGHLIGHT;
                    else if (strcmp(np_labels[r][c], "Wi-Fi") == 0) bg_col = 0xFF14532D;

                    draw_rounded_rect(kx, ry, np_key_w, np_key_h, 8, bg_col);
                    if (is_sel) draw_rect_outline(kx, ry, np_key_w, np_key_h, COLOR_WHITE, 2);
                    else draw_rect_outline(kx, ry, np_key_w, np_key_h, COLOR_CARD_BORDER, 1);

                    uint32_t txt_col = is_sel ? COLOR_WHITE : 0xFFE2E8F0;
                    if (!is_sel) {
                        if (r < 3 && c <= 2) txt_col = COLOR_WHITE;
                        else if (c >= 3) txt_col = COLOR_CYAN;
                        if (strcmp(np_labels[r][c], "Wi-Fi") == 0) txt_col = 0xFF86EFAC;
                    }
                    draw_text(kx + 22, ry + 28, np_labels[r][c], 18.0f, txt_col);
                }
            }

            // Action Buttons
            int btn_y = np_start_y + 4 * (np_key_h + np_spacing) + 14;
            bool btn_save_sel = (ip_area == 2 && ip_act_idx == 0);
            bool btn_test_sel = (ip_area == 2 && ip_act_idx == 1);
            bool btn_canc_sel = (ip_area == 2 && ip_act_idx == 2);

            draw_rounded_rect(modal_x + 36, btn_y, 320, 48, 10, btn_save_sel ? COLOR_GREEN : COLOR_GREEN_BG);
            if (btn_save_sel) draw_rect_outline(modal_x + 36, btn_y, 320, 48, COLOR_WHITE, 3);
            draw_text(modal_x + 55, btn_y + 31, "[START / A] บันทึก IP ลงเครื่อง", 19.0f, COLOR_WHITE);

            draw_rounded_rect(modal_x + 372, btn_y, 195, 48, 10, btn_test_sel ? COLOR_HIGHLIGHT : COLOR_PANEL_ALT);
            if (btn_test_sel) draw_rect_outline(modal_x + 372, btn_y, 195, 48, COLOR_WHITE, 2);
            draw_text(modal_x + 395, btn_y + 31, "[X] ทดสอบเชื่อมต่อ", 18.0f, COLOR_CYAN);

            draw_rounded_rect(modal_x + 582, btn_y, 160, 48, 10, btn_canc_sel ? COLOR_HIGHLIGHT : COLOR_PANEL_ALT);
            if (btn_canc_sel) draw_rect_outline(modal_x + 582, btn_y, 160, 48, COLOR_WHITE, 2);
            draw_text(modal_x + 625, btn_y + 31, "[B] ย้อนกลับ", 18.0f, COLOR_WHITE);

            if (strlen(message_notice) > 0) {
                draw_text(modal_x + 36, btn_y + 70, message_notice, 17.0f, message_color);
            } else {
                draw_text(modal_x + 36, btn_y + 70, "[D-Pad] เลื่อนปุ่ม     [A] กดพิมพ์     [START] บันทึกทันที     [B] ย้อนกลับ", 15.0f, COLOR_GRAY);
            }
        }
        else if (view == SETTING_VIEW_ONLINE_URL) {
            draw_text(modal_x + 36, modal_y + 46, "พิมพ์ URL ออนไลน์ / โดเมนเนมภายนอก", 26.0f, COLOR_WHITE);

            int edit_box_y = modal_y + 68;
            draw_rounded_rect(modal_x + 36, edit_box_y, modal_w - 72, 48, 10, COLOR_PANEL_ALT);
            draw_rect_outline(modal_x + 36, edit_box_y, modal_w - 72, 48, COLOR_HIGHLIGHT_BORDER, 2);

            char display_url[280];
            snprintf(display_url, sizeof(display_url), "%s|", url_edit);
            draw_truncated_text(modal_x + 50, edit_box_y + 32, display_url, 22.0f, COLOR_CYAN, modal_w - 100);

            int sc_y = edit_box_y + 60;
            int sc_w = 125;
            for (int i = 0; i < 5; i++) {
                int sc_x = modal_x + 36 + i * (sc_w + 12);
                bool is_sel = (kb_area == 0 && kb_shortcut_idx == i);
                draw_rounded_rect(sc_x, sc_y, sc_w, 36, 8, is_sel ? COLOR_HIGHLIGHT : COLOR_PANEL_ALT);
                if (is_sel) draw_rect_outline(sc_x, sc_y, sc_w, 36, COLOR_WHITE, 2);
                draw_text(sc_x + 14, sc_y + 24, kb_shortcuts[i], 16.0f, is_sel ? COLOR_WHITE : 0xFFCBD5E1);
            }

            int kb_start_y = sc_y + 50;
            int key_w = 52;
            int key_h = 44;
            int key_spacing = 7;

            for (int r = 0; r < 4; r++) {
                int ry = kb_start_y + r * (key_h + key_spacing);
                int row_len = (int)strlen(kb_rows[r]);
                for (int c = 0; c < row_len; c++) {
                    int kx = modal_x + 36 + c * (key_w + key_spacing);
                    bool is_sel = (kb_area == 1 && kb_row == r && kb_col == c);

                    draw_rounded_rect(kx, ry, key_w, key_h, 8, is_sel ? COLOR_HIGHLIGHT : COLOR_PANEL_ALT);
                    if (is_sel) draw_rect_outline(kx, ry, key_w, key_h, COLOR_WHITE, 2);

                    char ch_str[2] = {kb_rows[r][c], '\0'};
                    draw_text(kx + 19, ry + 30, ch_str, 22.0f, is_sel ? COLOR_WHITE : 0xFFE2E8F0);
                }
            }

            int act_y = kb_start_y + 4 * (key_h + key_spacing) + 12;
            static const char *act_labels[] = {"[Y] ลบ", "[X] ล้าง", "[START] บันทึก", "[B] ยกเลิก"};
            int act_widths[] = {120, 100, 270, 150};
            int cur_ax = modal_x + 36;
            for (int i = 0; i < 4; i++) {
                bool is_sel = (kb_area == 2 && kb_action_idx == i);
                uint32_t bg_col = COLOR_PANEL_ALT;
                if (i == 2) bg_col = is_sel ? COLOR_GREEN : COLOR_GREEN_BG;
                else if (is_sel) bg_col = COLOR_HIGHLIGHT;

                draw_rounded_rect(cur_ax, act_y, act_widths[i], 46, 10, bg_col);
                if (is_sel) draw_rect_outline(cur_ax, act_y, act_widths[i], 46, COLOR_WHITE, 2);
                draw_text(cur_ax + 16, act_y + 30, act_labels[i], 18.0f, COLOR_WHITE);
                cur_ax += act_widths[i] + 16;
            }

            if (strlen(message_notice) > 0) {
                draw_text(modal_x + 36, act_y + 60, message_notice, 17.0f, message_color);
            }
        }
        else if (view == SETTING_VIEW_AUTOSCAN) {
            draw_text(modal_x + 36, modal_y + 50, "ค้นหาคอมพิวเตอร์อัตโนมัติ (Wi-Fi Auto-Scan)", 28.0f, COLOR_WHITE);

            int spin_y = modal_y + 140;
            draw_rounded_rect(modal_x + 36, spin_y, modal_w - 72, 240, 16, COLOR_PANEL_ALT);
            draw_rect_outline(modal_x + 36, spin_y, modal_w - 72, 240, COLOR_CARD_BORDER, 1);

            static const char *spins[] = {"-", "\\", "|", "/"};
            static int spin_cnt = 0;
            char spin_head[128];
            snprintf(spin_head, sizeof(spin_head), "กำลังสแกนค้นหา PC ในเครือข่าย Wi-Fi... [%s]", spins[spin_cnt % 4]);
            draw_text(modal_x + 60, spin_y + 60, spin_head, 26.0f, COLOR_WHITE);
            draw_text(modal_x + 60, spin_y + 105, "ระบบจะสแกนหาเครื่องคอมพิวเตอร์ที่รัน Mirai ROM Hub (พอร์ต 8080)", 19.0f, COLOR_GRAY);

            if (strlen(message_notice) > 0) {
                draw_text(modal_x + 60, spin_y + 160, message_notice, 21.0f, message_color);
            }

            draw_rounded_rect(modal_x + 36, modal_y + modal_h - 90, modal_w - 72, 54, 12, COLOR_PANEL_ALT);
            draw_text(modal_x + (modal_w / 2) - 110, modal_y + modal_h - 55, "กด [B] เพื่อกลับสู่เมนูตั้งค่า", 20.0f, COLOR_WHITE);
            spin_cnt++;
        }
        else if (view == SETTING_VIEW_OTA_UPDATE) {
            draw_text(modal_x + 36, modal_y + 45, "ตรวจสอบและอัปเดตโปรแกรม (OTA Update)", 28.0f, COLOR_WHITE);
            draw_text(modal_x + 36, modal_y + 75, "อัปเดตโปรแกรมเวอร์ชันใหม่ผ่าน Wi-Fi ได้ทันทีโดยไม่ต้องถอด SD Card", 18.0f, COLOR_GRAY);

            if (!ver_checked) {
                draw_rounded_rect(modal_x + 36, modal_y + 115, modal_w - 72, 320, 12, COLOR_PANEL_ALT);
                draw_rect_outline(modal_x + 36, modal_y + 115, modal_w - 72, 320, COLOR_CARD_BORDER, 1);
                draw_text(modal_x + 56, modal_y + 240, "กำลังเชื่อมต่อตรวจสอบเวอร์ชันใหม่จาก Server...", 22.0f, COLOR_CYAN);
                flip_screen();

                char cmd[512];
                snprintf(cmd, sizeof(cmd), "bin/curl -s -m 4 \"%s/api/app/version\" -o /tmp/app_ver.json", g_server_url);
                system(cmd);

                FILE *vf = fopen("/tmp/app_ver.json", "r");
                if (vf) {
                    char vline[1024];
                    if (fgets(vline, sizeof(vline), vf)) {
                        char *vp = strstr(vline, "\"version\":");
                        if (vp) {
                            vp = strchr(vp + 10, '"');
                            if (vp) {
                                vp++;
                                char *vend = strchr(vp, '"');
                                if (vend) {
                                    *vend = '\0';
                                    strncpy(server_ver, vp, sizeof(server_ver) - 1);
                                }
                            }
                        }
                        char *bp = strstr(vline, "\"build_time\":");
                        if (bp) {
                            bp = strchr(bp + 13, '"');
                            if (bp) {
                                bp++;
                                char *bend = strchr(bp, '"');
                                if (bend) {
                                    *bend = '\0';
                                    strncpy(server_build, bp, sizeof(server_build) - 1);
                                }
                            }
                        }
                        char *cp = strstr(vline, "\"changelog\":");
                        if (cp) {
                            cp = strchr(cp + 12, '"');
                            if (cp) {
                                cp++;
                                char *cend = strchr(cp, '"');
                                if (cend) {
                                    *cend = '\0';
                                    strncpy(server_changelog, cp, sizeof(server_changelog) - 1);
                                }
                            }
                        }
                    }
                    fclose(vf);
                }

                ver_checked = true;
                if (strlen(server_ver) > 0) {
                    has_update = (strcmp(server_ver, APP_VERSION) != 0);
                }
            }

            int box_y = modal_y + 115;
            int box_h = 320;
            draw_rounded_rect(modal_x + 36, box_y, modal_w - 72, box_h, 12, COLOR_PANEL_ALT);
            draw_rect_outline(modal_x + 36, box_y, modal_w - 72, box_h, COLOR_CARD_BORDER, 1);

            char cur_vstr[128];
            snprintf(cur_vstr, sizeof(cur_vstr), "เวอร์ชันในเครื่องปัจจุบัน:   v%s", APP_VERSION);
            draw_text(modal_x + 56, box_y + 40, cur_vstr, 22.0f, COLOR_WHITE);

            char srv_vstr[128];
            if (strlen(server_ver) > 0) {
                snprintf(srv_vstr, sizeof(srv_vstr), "เวอร์ชันบน Server:          v%s  (%s)", server_ver, server_build);
                draw_text(modal_x + 56, box_y + 80, srv_vstr, 22.0f, COLOR_CYAN);
            } else {
                draw_text(modal_x + 56, box_y + 80, "เวอร์ชันบน Server:          [!] ไม่สามารถตรวจสอบได้", 22.0f, COLOR_RED);
            }

            if (strlen(server_changelog) > 0) {
                draw_text(modal_x + 56, box_y + 130, "สิ่งที่อัปเดตใหม่ (Changelog):", 19.0f, COLOR_GRAY);
                draw_truncated_text(modal_x + 56, box_y + 165, server_changelog, 18.0f, 0xFFE2E8F0, modal_w - 112);
            }

            if (strlen(server_ver) == 0) {
                draw_rounded_rect(modal_x + 56, box_y + 220, modal_w - 112, 55, 8, 0xFF451A1A);
                draw_text(modal_x + 76, box_y + 255, "[!] ไม่สามารถเชื่อมต่อกับ Server กรุณาตรวจสอบ Wi-Fi หรือเปิด server.py", 18.0f, 0xFFFCA5A5);
            } else if (has_update) {
                draw_rounded_rect(modal_x + 56, box_y + 220, modal_w - 112, 55, 8, COLOR_GREEN_BG);
                draw_text(modal_x + 76, box_y + 255, "[มีอัปเดต] พบเวอร์ชันใหม่พร้อมติดตั้ง! กด [A] เพื่อเริ่มดาวน์โหลดทันที", 19.0f, COLOR_GREEN);
            } else {
                draw_rounded_rect(modal_x + 56, box_y + 220, modal_w - 112, 55, 8, COLOR_PANEL);
                draw_text(modal_x + 76, box_y + 255, "[ล่าสุด] คุณใช้เวอร์ชันล่าสุดแล้ว (สามารถกด [A] เพื่อดาวน์โหลดติดตั้งซ้ำได้)", 18.0f, COLOR_CYAN);
            }

            int btn_y = modal_y + modal_h - 90;
            int btn_w = (modal_w - 90) / 2;

            draw_rounded_rect(modal_x + 36, btn_y, btn_w, 58, 12, (strlen(server_ver) > 0) ? COLOR_HIGHLIGHT : COLOR_PANEL_ALT);
            draw_rect_outline(modal_x + 36, btn_y, btn_w, 58, (strlen(server_ver) > 0) ? COLOR_HIGHLIGHT_BORDER : COLOR_CARD_BORDER, 2);
            draw_text(modal_x + 56, btn_y + 36, "[A] ดาวน์โหลดและติดตั้งอัปเดต", 20.0f, (strlen(server_ver) > 0) ? COLOR_WHITE : COLOR_GRAY);

            draw_rounded_rect(modal_x + 36 + btn_w + 18, btn_y, btn_w, 58, 12, COLOR_PANEL_ALT);
            draw_rect_outline(modal_x + 36 + btn_w + 18, btn_y, btn_w, 58, COLOR_CARD_BORDER, 1);
            draw_text(modal_x + 36 + btn_w + 60, btn_y + 36, "[B] ยกเลิกกลับสู่เมนู", 20.0f, COLOR_GRAY);
        }

        flip_screen();

        if (view == SETTING_VIEW_AUTOSCAN) {
            usleep(200000);
            unlink("/tmp/found_server.txt");
            system("bin/autoscan.sh > /dev/null 2>&1");

            FILE *ff = fopen("/tmp/found_server.txt", "r");
            if (ff) {
                char found_ip[256];
                if (fgets(found_ip, sizeof(found_ip), ff)) {
                    char *nl = strchr(found_ip, '\r'); if (nl) *nl = '\0';
                    nl = strchr(found_ip, '\n'); if (nl) *nl = '\0';
                    if (strlen(found_ip) > 0) {
                        strncpy(g_server_url, found_ip, sizeof(g_server_url) - 1);
                        save_config("AUTO", g_server_url, "");
                        snprintf(message_notice, sizeof(message_notice), "[พบแล้ว] คอมพิวเตอร์: %s (กำลังโหลดเกม...)", g_server_url);
                        message_color = COLOR_GREEN;
                        flip_screen();

                        sync_catalog_from_server(g_server_url);
                        snprintf(message_notice, sizeof(message_notice), "[สำเร็จ] เชื่อมต่อสำเร็จ! โหลดได้ %d เกม", g_game_count);
                        flip_screen();
                        usleep(1500000);
                        in_settings = false;
                        break;
                    }
                }
                fclose(ff);
            } else {
                snprintf(message_notice, sizeof(message_notice), "[ไม่พบ] คอมพิวเตอร์ในวง Wi-Fi (ตรวจสอบว่าเปิด server.py อยู่)");
                message_color = COLOR_RED;
            }
        }

        UserAction act = poll_user_action(150);
        if (act != ACT_NONE) {
            if (act == ACT_B || act == ACT_MENU) {
                if (view == SETTING_VIEW_MAIN) {
                    in_settings = false;
                } else {
                    view = SETTING_VIEW_MAIN;
                    message_notice[0] = '\0';
                }
            }
            else if (view == SETTING_VIEW_MAIN) {
                if (act == ACT_UP && main_sel > 0) main_sel--;
                else if (act == ACT_DOWN && main_sel < 5) main_sel++;
                else if (act == ACT_A || act == ACT_START) {
                    if (main_sel == 0) {
                        view = SETTING_VIEW_AUTOSCAN;
                        message_notice[0] = '\0';
                    } else if (main_sel == 1) {
                        view = SETTING_VIEW_LOCAL_IP;
                        parse_ip_from_url(g_server_url, octets, &ip_port);
                        octet_field = 3;
                        for (int k = 0; k < 5; k++) octet_fresh[k] = true;
                        ip_area = 1;
                        np_row = 0;
                        np_col = 0;
                        ip_act_idx = 0;
                        message_notice[0] = '\0';
                    } else if (main_sel == 2) {
                        view = SETTING_VIEW_ONLINE_URL;
                        message_notice[0] = '\0';
                    } else if (main_sel == 3) {
                        view = SETTING_VIEW_OTA_UPDATE;
                        ver_checked = false;
                        server_ver[0] = '\0';
                        server_build[0] = '\0';
                        server_changelog[0] = '\0';
                        has_update = false;
                        message_notice[0] = '\0';
                    } else if (main_sel == 4) {
                        // Clear Cache
                        unlink("/tmp/catalog.txt");
                        system("rm -rf /tmp/covers/*");
                        if (g_cover_pixels) {
                            free(g_cover_pixels);
                            g_cover_pixels = NULL;
                        }
                        g_cover_loaded_index = -1;
                        snprintf(message_notice, sizeof(message_notice), "[ล้างแล้ว] ล้างแคชรายชื่อและรูปปกชั่วคราวเรียบร้อย");
                        message_color = COLOR_GREEN;
                    } else if (main_sel == 5) {
                        in_settings = false;
                    }
                }
            }
            else if (view == SETTING_VIEW_OTA_UPDATE) {
                if (act == ACT_A || act == ACT_START) {
                    if (strlen(server_ver) > 0) {
                        draw_rect(0, 0, g_screen_w, g_screen_h, 0xEE000000);
                        draw_rounded_rect(modal_x, modal_y, modal_w, modal_h, 16, COLOR_PANEL);
                        draw_rect_outline(modal_x, modal_y, modal_w, modal_h, COLOR_CARD_BORDER, 2);

                        draw_text(modal_x + 36, modal_y + 60, "[อัปเดต] กำลังดำเนินการอัปเดต...", 28.0f, COLOR_WHITE);
                        draw_text(modal_x + 36, modal_y + 110, "กรุณารอสักครู่ ห้ามปิดเครื่องขณะกำลังดาวน์โหลด", 20.0f, COLOR_AMBER);

                        int prog_bar_x = modal_x + 36;
                        int prog_bar_y = modal_y + 170;
                        int prog_bar_w = modal_w - 72;
                        draw_rounded_rect(prog_bar_x, prog_bar_y, prog_bar_w, 36, 8, COLOR_PANEL_ALT);
                        draw_rounded_rect(prog_bar_x, prog_bar_y, prog_bar_w / 2, 36, 8, COLOR_HIGHLIGHT);

                        draw_text(prog_bar_x, prog_bar_y + 75, "กำลังดาวน์โหลด store.elf และสคริปต์เวอร์ชันล่าสุด...", 20.0f, COLOR_CYAN);
                        flip_screen();

                        char dl_cmd[1024];
                        snprintf(dl_cmd, sizeof(dl_cmd),
                                 "bin/curl -s -L -m 60 \"%s/api/app/latest/store.elf\" -o /tmp/store.elf.update && "
                                 "bin/curl -s -L -m 15 \"%s/api/app/latest/launch.sh\" -o /tmp/launch.sh.update && "
                                 "bin/curl -s -L -m 15 \"%s/api/app/latest/autoscan.sh\" -o /tmp/autoscan.sh.update",
                                 g_server_url, g_server_url, g_server_url);
                        int dl_res = system(dl_cmd);

                        struct stat ust;
                        if (dl_res == 0 && stat("/tmp/store.elf.update", &ust) == 0 && ust.st_size > 100000) {
                            draw_rounded_rect(prog_bar_x, prog_bar_y, prog_bar_w, 36, 8, COLOR_GREEN);
                            draw_text(prog_bar_x, prog_bar_y + 75, "[สำเร็จ] ดาวน์โหลดสำเร็จ! (ขนาดไฟล์: 900+ KB)", 20.0f, COLOR_GREEN);
                            draw_text(prog_bar_x, prog_bar_y + 125, "กำลังบันทึกลง SD Card และรีสตาร์ทเข้าสู่แอปใหม่...", 22.0f, COLOR_WHITE);
                            flip_screen();
                            sleep(2);

                            close_inputs();
                            close_fb();
                            exit(42);
                        } else {
                            snprintf(message_notice, sizeof(message_notice), "[ไม่สำเร็จ] ดาวน์โหลดล้มเหลว ตรวจสอบการเชื่อมต่อ Server");
                            message_color = COLOR_RED;
                            view = SETTING_VIEW_MAIN;
                        }
                    }
                }
            }
            else if (view == SETTING_VIEW_LOCAL_IP) {
                if (act == ACT_START) {
                    char test_url[256];
                    snprintf(test_url, sizeof(test_url), "http://%d.%d.%d.%d:%d",
                             octets[0], octets[1], octets[2], octets[3], ip_port);

                    // 1. UNCONDITIONAL SAVE TO CONFIG & SYNC
                    save_config("LOCAL", test_url, "");
                    strncpy(g_server_url, test_url, sizeof(g_server_url) - 1);
                    snprintf(message_notice, sizeof(message_notice), "บันทึก IP: %s แล้ว กำลังเชื่อมต่อ...", test_url);
                    message_color = COLOR_CYAN;
                    flip_screen();

                    if (test_server_connection(test_url)) {
                        sync_catalog_from_server(test_url);
                        snprintf(message_notice, sizeof(message_notice), "[สำเร็จ] เชื่อมต่อสำเร็จ! (%d เกม)", g_game_count);
                        message_color = COLOR_GREEN;
                        flip_screen();
                        usleep(1000000);
                        in_settings = false;
                    } else {
                        snprintf(message_notice, sizeof(message_notice), "[บันทึกแล้ว] บันทึก IP สำเร็จ! (สถานะ: ออฟไลน์ ตรวจสอบ server.py)");
                        message_color = COLOR_AMBER;
                    }
                } else if (act == ACT_X) {
                    // Backspace / Delete last digit
                    if (octet_field <= 3) octets[octet_field] /= 10;
                    else ip_port /= 10;
                } else if (act == ACT_Y) {
                    // Clear current octet
                    if (octet_field <= 3) octets[octet_field] = 0;
                    else ip_port = 8080;
                    octet_fresh[octet_field] = true;
                } else if (act == ACT_L1) {
                    if (octet_field <= 3) octets[octet_field] = (octets[octet_field] - 10 + 256) % 256;
                    else if (ip_port > 10) ip_port -= 10;
                } else if (act == ACT_R1) {
                    if (octet_field <= 3) octets[octet_field] = (octets[octet_field] + 10) % 256;
                    else ip_port += 10;
                } else if (act == ACT_UP) {
                    if (ip_area == 2) {
                        ip_area = 1;
                        np_row = 3;
                    } else if (ip_area == 1) {
                        if (np_row > 0) np_row--;
                        else ip_area = 0;
                    } else if (ip_area == 0) {
                        if (octet_field <= 3) octets[octet_field] = (octets[octet_field] + 1) % 256;
                        else ip_port++;
                    }
                } else if (act == ACT_DOWN) {
                    if (ip_area == 0) {
                        ip_area = 1;
                        np_row = 0;
                    } else if (ip_area == 1) {
                        if (np_row < 3) np_row++;
                        else ip_area = 2;
                    }
                } else if (act == ACT_LEFT) {
                    if (ip_area == 0) {
                        if (octet_field > 0) { octet_field--; octet_fresh[octet_field] = true; }
                    } else if (ip_area == 1) {
                        if (np_col > 0) np_col--;
                    } else if (ip_area == 2) {
                        if (ip_act_idx > 0) ip_act_idx--;
                    }
                } else if (act == ACT_RIGHT) {
                    if (ip_area == 0) {
                        if (octet_field < 4) { octet_field++; octet_fresh[octet_field] = true; }
                    } else if (ip_area == 1) {
                        if (np_col < 4) np_col++;
                    } else if (ip_area == 2) {
                        if (ip_act_idx < 2) ip_act_idx++;
                    }
                } else if (act == ACT_A) {
                    if (ip_area == 0) {
                        octet_field = (octet_field + 1) % 5;
                        octet_fresh[octet_field] = true;
                    } else if (ip_area == 1) {
                        const char *lbl = np_labels[np_row][np_col];
                        if (lbl[0] >= '0' && lbl[0] <= '9' && lbl[1] == '\0') {
                            int digit = lbl[0] - '0';
                            if (octet_field <= 3) {
                                if (octet_fresh[octet_field]) {
                                    octets[octet_field] = digit;
                                    octet_fresh[octet_field] = false;
                                } else {
                                    int n = octets[octet_field] * 10 + digit;
                                    if (n > 255) n = 255;
                                    octets[octet_field] = n;
                                }
                            } else {
                                if (octet_fresh[4]) {
                                    ip_port = digit;
                                    octet_fresh[4] = false;
                                } else {
                                    int p = ip_port * 10 + digit;
                                    if (p > 65535) p = 65535;
                                    ip_port = p;
                                }
                            }
                        } else if (strcmp(lbl, "+1") == 0) {
                            if (octet_field <= 3) octets[octet_field] = (octets[octet_field] + 1) % 256;
                            else ip_port++;
                        } else if (strcmp(lbl, "-1") == 0) {
                            if (octet_field <= 3) octets[octet_field] = (octets[octet_field] - 1 + 256) % 256;
                            else if (ip_port > 1) ip_port--;
                        } else if (strcmp(lbl, "+10") == 0) {
                            if (octet_field <= 3) octets[octet_field] = (octets[octet_field] + 10) % 256;
                            else ip_port += 10;
                        } else if (strcmp(lbl, "-10") == 0) {
                            if (octet_field <= 3) octets[octet_field] = (octets[octet_field] - 10 + 256) % 256;
                            else if (ip_port > 10) ip_port -= 10;
                        } else if (strcmp(lbl, "ลบ <") == 0) {
                            if (octet_field <= 3) octets[octet_field] /= 10;
                            else ip_port /= 10;
                        } else if (strcmp(lbl, "< ก่อน") == 0) {
                            if (octet_field > 0) { octet_field--; octet_fresh[octet_field] = true; }
                        } else if (strcmp(lbl, "ถัดไป >") == 0) {
                            if (octet_field < 4) { octet_field++; octet_fresh[octet_field] = true; }
                        } else if (strcmp(lbl, "ล้าง") == 0) {
                            if (octet_field <= 3) octets[octet_field] = 0;
                            else ip_port = 8080;
                            octet_fresh[octet_field] = true;
                        } else if (strcmp(lbl, "Wi-Fi") == 0) {
                            FILE *p = popen("ip -4 addr show wlan0 2>/dev/null | awk '/inet / {print $2}' | cut -d/ -f1", "r");
                            if (!p) p = popen("ifconfig wlan0 2>/dev/null | grep 'inet ' | sed 's/.*addr://;s/ .*//'", "r");
                            if (p) {
                                char wip[64] = "";
                                if (fgets(wip, sizeof(wip), p)) {
                                    int a, b, c, d;
                                    if (sscanf(wip, "%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
                                        octets[0] = a; octets[1] = b; octets[2] = c;
                                        snprintf(message_notice, sizeof(message_notice), "[Wi-Fi] ดึง Subnet %d.%d.%d. สำเร็จ!", a, b, c);
                                        message_color = COLOR_GREEN;
                                    }
                                }
                                pclose(p);
                            }
                        } else if (strcmp(lbl, ":8080") == 0) {
                            ip_port = 8080;
                            octet_fresh[4] = true;
                        }
                    } else if (ip_area == 2) {
                        if (ip_act_idx == 0) {
                            // SAVE & CONNECT
                            char test_url[256];
                            snprintf(test_url, sizeof(test_url), "http://%d.%d.%d.%d:%d",
                                     octets[0], octets[1], octets[2], octets[3], ip_port);

                            // UNCONDITIONAL SAVE TO CONFIG
                            save_config("LOCAL", test_url, "");
                            strncpy(g_server_url, test_url, sizeof(g_server_url) - 1);
                            snprintf(message_notice, sizeof(message_notice), "บันทึก IP แล้ว กำลังเชื่อมต่อ...");
                            message_color = COLOR_CYAN;
                            flip_screen();

                            if (test_server_connection(test_url)) {
                                sync_catalog_from_server(test_url);
                                snprintf(message_notice, sizeof(message_notice), "[สำเร็จ] เชื่อมต่อสำเร็จ! (%d เกม)", g_game_count);
                                message_color = COLOR_GREEN;
                                flip_screen();
                                usleep(1000000);
                                in_settings = false;
                            } else {
                                snprintf(message_notice, sizeof(message_notice), "[บันทึกแล้ว] บันทึก IP สำเร็จ! (สถานะ: ออฟไลน์ ตรวจสอบ server.py)");
                                message_color = COLOR_AMBER;
                            }
                        } else if (ip_act_idx == 1) {
                            // TEST ONLY
                            char test_url[256];
                            snprintf(test_url, sizeof(test_url), "http://%d.%d.%d.%d:%d",
                                     octets[0], octets[1], octets[2], octets[3], ip_port);
                            snprintf(message_notice, sizeof(message_notice), "กำลังทดสอบ: %s...", test_url);
                            message_color = COLOR_AMBER;
                            flip_screen();

                            if (test_server_connection(test_url)) {
                                snprintf(message_notice, sizeof(message_notice), "[พบเซิร์ฟเวอร์] ติดต่อ %s ได้สำเร็จ!", test_url);
                                message_color = COLOR_GREEN;
                            } else {
                                snprintf(message_notice, sizeof(message_notice), "[ไม่พบเซิร์ฟเวอร์] ติดต่อไม่ได้ ตรวจสอบ IP และ server.py");
                                message_color = COLOR_RED;
                            }
                        } else if (ip_act_idx == 2) {
                            view = SETTING_VIEW_MAIN;
                        }
                    }
                }
            }
            else if (view == SETTING_VIEW_ONLINE_URL) {
                if (act == ACT_UP) {
                    if (kb_area == 2) kb_area = 1, kb_row = 3;
                    else if (kb_area == 1) {
                        if (kb_row > 0) kb_row--;
                        else kb_area = 0;
                    }
                } else if (act == ACT_DOWN) {
                    if (kb_area == 0) kb_area = 1, kb_row = 0;
                    else if (kb_area == 1) {
                        if (kb_row < 3) kb_row++;
                        else kb_area = 2;
                    }
                } else if (act == ACT_LEFT) {
                    if (kb_area == 0 && kb_shortcut_idx > 0) kb_shortcut_idx--;
                    else if (kb_area == 1 && kb_col > 0) kb_col--;
                    else if (kb_area == 2 && kb_action_idx > 0) kb_action_idx--;
                } else if (act == ACT_RIGHT) {
                    if (kb_area == 0 && kb_shortcut_idx < 4) kb_shortcut_idx++;
                    else if (kb_area == 1 && kb_col < 11) kb_col++;
                    else if (kb_area == 2 && kb_action_idx < 3) kb_action_idx++;
                } else if (act == ACT_Y) {
                    size_t ulen = strlen(url_edit);
                    if (ulen > 0) url_edit[ulen - 1] = '\0';
                } else if (act == ACT_X) {
                    url_edit[0] = '\0';
                } else if (act == ACT_START) {
                    // UNCONDITIONAL SAVE TO CONFIG
                    save_config("ONLINE", "", url_edit);
                    strncpy(g_server_url, url_edit, sizeof(g_server_url) - 1);
                    snprintf(message_notice, sizeof(message_notice), "บันทึก URL แล้ว กำลังเชื่อมต่อ...");
                    message_color = COLOR_CYAN;
                    flip_screen();

                    if (test_server_connection(url_edit)) {
                        sync_catalog_from_server(g_server_url);
                        snprintf(message_notice, sizeof(message_notice), "[สำเร็จ] เชื่อมต่อสำเร็จ! (%d เกม)", g_game_count);
                        message_color = COLOR_GREEN;
                        flip_screen();
                        usleep(1000000);
                        in_settings = false;
                    } else {
                        snprintf(message_notice, sizeof(message_notice), "[บันทึกแล้ว] บันทึก URL สำเร็จ! (สถานะ: ออฟไลน์ ตรวจสอบเน็ต)");
                        message_color = COLOR_AMBER;
                    }
                } else if (act == ACT_A) {
                    if (kb_area == 0) {
                        strncat(url_edit, kb_shortcuts[kb_shortcut_idx], sizeof(url_edit) - strlen(url_edit) - 1);
                    } else if (kb_area == 1) {
                        char ch = kb_rows[kb_row][kb_col];
                        size_t ulen = strlen(url_edit);
                        if (ulen < sizeof(url_edit) - 2) {
                            url_edit[ulen] = ch;
                            url_edit[ulen + 1] = '\0';
                        }
                    } else if (kb_area == 2) {
                        if (kb_action_idx == 0) {
                            size_t ulen = strlen(url_edit);
                            if (ulen > 0) url_edit[ulen - 1] = '\0';
                        } else if (kb_action_idx == 1) {
                            url_edit[0] = '\0';
                        } else if (kb_action_idx == 2) {
                            // UNCONDITIONAL SAVE TO CONFIG
                            save_config("ONLINE", "", url_edit);
                            strncpy(g_server_url, url_edit, sizeof(g_server_url) - 1);
                            snprintf(message_notice, sizeof(message_notice), "บันทึก URL แล้ว กำลังเชื่อมต่อ...");
                            message_color = COLOR_CYAN;
                            flip_screen();

                            if (test_server_connection(url_edit)) {
                                sync_catalog_from_server(g_server_url);
                                snprintf(message_notice, sizeof(message_notice), "[สำเร็จ] เชื่อมต่อสำเร็จ! (%d เกม)", g_game_count);
                                message_color = COLOR_GREEN;
                                flip_screen();
                                usleep(1000000);
                                in_settings = false;
                            } else {
                                snprintf(message_notice, sizeof(message_notice), "[บันทึกแล้ว] บันทึก URL สำเร็จ! (สถานะ: ออฟไลน์ ตรวจสอบเน็ต)");
                                message_color = COLOR_AMBER;
                            }
                        } else if (kb_action_idx == 3) {
                            view = SETTING_VIEW_MAIN;
                        }
                    }
                }
            }
        }
    }

    usleep(200000);
    flush_inputs();
}

// Open all Linux evdev nodes
static void init_inputs(void) {
    g_input_count = 0;
    for (int i = 0; i < 16; i++) {
        char dev[64];
        snprintf(dev, sizeof(dev), "/dev/input/event%d", i);
        int fd = open(dev, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            g_input_fds[g_input_count++] = fd;
            if (g_input_count >= MAX_INPUTS) break;
        }
    }
}

static void close_inputs(void) {
    for (int i = 0; i < g_input_count; i++) {
        if (g_input_fds[i] >= 0) close(g_input_fds[i]);
    }
}

// Initialize Framebuffer with dynamic resolution query
static bool init_fb(void) {
    g_fb_fd = open("/dev/fb0", O_RDWR);
    if (g_fb_fd < 0) {
        fprintf(stderr, "Cannot open /dev/fb0, running in virtual buffer mode (1024x768 32bpp).\n");
        g_screen_w = 1024;
        g_screen_h = 768;
        g_bpp = 32;
        g_line_length = 1024 * 4;
        g_backbuffer = (uint32_t *)calloc(g_screen_w * g_screen_h, sizeof(uint32_t));
        return false;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
        ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0) {
        g_screen_w = vinfo.xres;
        g_screen_h = vinfo.yres;
        g_bpp = vinfo.bits_per_pixel;
        g_line_length = finfo.line_length;
        if (g_line_length == 0) g_line_length = g_screen_w * (g_bpp / 8);

        if (vinfo.red.offset == 0) {
            g_rgb_swap = true;
        }

        fprintf(stderr, "Detected FB: %dx%d, %d bpp, stride: %d bytes, red_offset: %d\n",
                g_screen_w, g_screen_h, g_bpp, g_line_length, vinfo.red.offset);
    } else {
        g_screen_w = 1024;
        g_screen_h = 768;
        g_bpp = 32;
        g_line_length = 1024 * 4;
        fprintf(stderr, "FB ioctl failed, defaulting to 1024x768 32bpp\n");
    }

    g_fb_size = (size_t)g_line_length * g_screen_h;
    g_fb_mem = (uint8_t *)mmap(NULL, g_fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
    if (g_fb_mem == MAP_FAILED) {
        fprintf(stderr, "mmap /dev/fb0 failed: %s\n", strerror(errno));
        g_fb_mem = NULL;
        close(g_fb_fd);
        g_fb_fd = -1;
    }

    g_backbuffer = (uint32_t *)calloc(g_screen_w * g_screen_h, sizeof(uint32_t));
    return (g_fb_mem != NULL);
}

static void close_fb(void) {
    if (g_fb_mem && g_fb_size > 0) {
        munmap(g_fb_mem, g_fb_size);
        g_fb_mem = NULL;
    }
    if (g_fb_fd >= 0) {
        close(g_fb_fd);
        g_fb_fd = -1;
    }
    if (g_backbuffer) {
        free(g_backbuffer);
        g_backbuffer = NULL;
    }
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strlen(argv[1]) > 0) {
        strncpy(g_server_url, argv[1], sizeof(g_server_url) - 1);
    } else {
        FILE *cfg = fopen("config.txt", "r");
        if (cfg) {
            char cline[256];
            while (fgets(cline, sizeof(cline), cfg)) {
                if (strncmp(cline, "LOCAL_URL=", 10) == 0 ||
                    strncmp(cline, "SERVER_URL=", 11) == 0) {
                    char *val = strchr(cline, '=') + 1;
                    char *nl = strchr(val, '\r'); if (nl) *nl = '\0';
                    nl = strchr(val, '\n'); if (nl) *nl = '\0';
                    if (strlen(val) > 0) strncpy(g_server_url, val, sizeof(g_server_url) - 1);
                }
            }
            fclose(cfg);
        }
    }

    init_fb();
    init_inputs();

    // Load font
    FILE *ff = fopen("res/font.ttf", "rb");
    if (!ff) ff = fopen("/mnt/SDCARD/Tools/tg5040/ThaiRomStore.pak/res/font.ttf", "rb");
    if (!ff) ff = fopen("/mnt/sdcard/Tools/tg5040/ThaiRomStore.pak/res/font.ttf", "rb");
    if (ff) {
        fseek(ff, 0, SEEK_END);
        long fsz = ftell(ff);
        fseek(ff, 0, SEEK_SET);
        g_font_buffer = malloc(fsz);
        if (g_font_buffer) {
            if (fread(g_font_buffer, 1, fsz, ff) == (size_t)fsz) {
                stbtt_InitFont(&g_font, g_font_buffer, stbtt_GetFontOffsetForIndex(g_font_buffer, 0));
            }
        }
        fclose(ff);
    }

    // Load mascot logo
    g_mascot_pixels = load_image_file("icon.png", &g_mascot_w, &g_mascot_h);
    if (!g_mascot_pixels) {
        g_mascot_pixels = load_image_file("res/icon.png", &g_mascot_w, &g_mascot_h);
    }

    // Load catalog
    load_catalog();

    bool running = true;
    update_cover_preview();
    render_ui();

    while (running) {
        UserAction act = poll_user_action(100);
        bool need_render = false;

        if (act != ACT_NONE) {
            switch (act) {
                case ACT_UP:
                    if (g_game_count > 0 && g_selected_index > 0) {
                        g_selected_index--;
                        if (g_selected_index < g_top_index) g_top_index -= ROWS_PER_PAGE;
                        if (g_top_index < 0) g_top_index = 0;
                        need_render = true;
                    }
                    break;
                case ACT_DOWN:
                    if (g_game_count > 0 && g_selected_index < g_filtered_count - 1) {
                        g_selected_index++;
                        if (g_selected_index >= g_top_index + ROWS_PER_PAGE) g_top_index += ROWS_PER_PAGE;
                        need_render = true;
                    }
                    break;
                case ACT_LEFT:
                case ACT_L1:
                    if (g_game_count > 0) {
                        g_selected_index -= ROWS_PER_PAGE;
                        if (g_selected_index < 0) g_selected_index = 0;
                        g_top_index = (g_selected_index / ROWS_PER_PAGE) * ROWS_PER_PAGE;
                        need_render = true;
                    }
                    break;
                case ACT_RIGHT:
                case ACT_R1:
                    if (g_game_count > 0) {
                        g_selected_index += ROWS_PER_PAGE;
                        if (g_selected_index >= g_filtered_count) g_selected_index = g_filtered_count - 1;
                        if (g_selected_index < 0) g_selected_index = 0;
                        g_top_index = (g_selected_index / ROWS_PER_PAGE) * ROWS_PER_PAGE;
                        need_render = true;
                    }
                    break;
                case ACT_A:
                    if (g_game_count > 0) {
                        execute_download(g_selected_index);
                        need_render = true;
                    } else {
                        open_settings_menu();
                        need_render = true;
                    }
                    break;
                case ACT_SELECT:
                    open_settings_menu();
                    need_render = true;
                    break;
                case ACT_B:
                case ACT_MENU:
                    running = false;
                    break;
                case ACT_X:
                    if (g_game_count > 0) {
                        if (strcmp(g_cur_filter, "ALL") == 0) strcpy(g_cur_filter, "GBA");
                        else if (strcmp(g_cur_filter, "GBA") == 0) strcpy(g_cur_filter, "NDS");
                        else if (strcmp(g_cur_filter, "NDS") == 0) strcpy(g_cur_filter, "PSX");
                        else if (strcmp(g_cur_filter, "PSX") == 0) strcpy(g_cur_filter, "PSP");
                        else if (strcmp(g_cur_filter, "PSP") == 0) strcpy(g_cur_filter, "SFC");
                        else strcpy(g_cur_filter, "ALL");
                        g_selected_index = 0;
                        g_top_index = 0;
                        apply_filter();
                        need_render = true;
                    }
                    break;
                case ACT_Y:
                    sync_catalog_from_server(g_server_url);
                    need_render = true;
                    break;
                default:
                    break;
            }
        }

        if (need_render) {
            update_cover_preview();
            render_ui();
        }
    }

    close_inputs();
    close_fb();
    if (g_font_buffer) free(g_font_buffer);
    if (g_mascot_pixels) free(g_mascot_pixels);
    if (g_cover_pixels) free(g_cover_pixels);

    return 0;
}
