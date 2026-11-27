/* launcher.c - the start screen: pick the game disc, choose window or fullscreen, and press Play.
 *
 * It draws with SDL's 2D renderer and needs no files of its own: text uses the system font compiled
 * into the program (Droid Sans, the replacement GameCube IPL font, decoded here into a texture atlas),
 * and shapes are built from a few generated textures. It works with a mouse, the keyboard or a game
 * controller (the Steam Deck).
 *
 * Browse opens the system's file dialog (Windows' Open dialog; on Linux kdialog or zenity). With a
 * controller, in the Deck's Game Mode, or when no dialog program exists, a built-in browser is used.
 *
 * Choices are remembered in user/launcher.cfg. The disc is checked when it's picked: the disc ID
 * must be GF7E01 and its main.dol must be the one this program was translated from.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include "funcs.h"
#include "runtime.h"
#include "updater.h"
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

/* UTF-8 for a few typographic characters the font has (at their Windows-1252 positions) */
#define ELLIPSIS "\xE2\x80\xA6"
#define BULLET "\xE2\x80\xA2"
#define LARROW "\xE2\x80\xB9"

/* ---- colors ------------------------------------------------------------------------------------ */
static const SDL_Color C_TEXT = {238, 243, 255, 255}, C_MUTED = {141, 154, 184, 255}, C_FAINT = {91, 103, 133, 255},
                       C_ACCENT = {59, 130, 246, 255}, C_ACCENT_HI = {96, 160, 255, 255},
                       C_CARD = {18, 26, 48, 255}, C_CARD_HI = {26, 37, 66, 255}, C_TRACK = {11, 17, 34, 255},
                       C_GREEN = {52, 211, 153, 255}, C_RED = {248, 113, 113, 255}, C_AMBER = {251, 191, 36, 255},
                       C_RING = {214, 228, 255, 255}, C_DARK = {7, 11, 24, 255};

static SDL_Color mix(SDL_Color a, SDL_Color b, float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return (SDL_Color){(Uint8)(a.r + (b.r - a.r) * t), (Uint8)(a.g + (b.g - a.g) * t), (Uint8)(a.b + (b.b - a.b) * t),
                       (Uint8)(a.a + (b.a - a.a) * t)};
}
static SDL_Color alpha(SDL_Color c, float a) { c.a = (Uint8)(c.a * (a < 0 ? 0 : a > 1 ? 1 : a)); return c; }

/* ---- the font ------------------------------------------------------------------------------------
 * The IPL font is Yay0-compressed: an OSFontHeader, a width table and a 2-bit sheet (four gray levels,
 * tiled like an I4 texture: 8x8 pixels per tile). 224 glyphs (0x20-0xFF, Windows-1252) in 24x24 cells.
 * The atlas copies each glyph into a padded cell, once at 1x and once at 3x: the 3x copy is upscaled
 * bilinearly and its edges sharpened, so large text stays crisp. */
extern const unsigned char g_font_western[];
extern const size_t g_font_western_size;
#define FCELL 24
#define FPAD 2
#define FCOLS 16
#define FGLYPHS 224
static SDL_Texture *g_ftex[2]; /* 1x, 3x */
static uint8_t g_fwidth[FGLYPHS];
static int g_font_ok;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint8_t *yay0(const uint8_t *src, size_t n, uint32_t *out_size) {
    if (n < 16 || memcmp(src, "Yay0", 4)) return NULL;
    uint32_t size = be32(src + 4), link = be32(src + 8), chunk = be32(src + 12), mask_pos = 16, bits = 0;
    uint8_t *out = malloc(size ? size : 1);
    if (!out) return NULL;
    int nbits = 0;
    for (uint32_t o = 0; o < size;) {
        if (!nbits) {
            if (mask_pos + 4 > n) break;
            bits = be32(src + mask_pos);
            mask_pos += 4;
            nbits = 32;
        }
        if (bits & 0x80000000u) {
            if (chunk >= n) break;
            out[o++] = src[chunk++];
        } else {
            if (link + 2 > n) break;
            uint16_t v = be16(src + link);
            link += 2;
            uint32_t dist = (v & 0xFFFu) + 1, len = v >> 12;
            if (len == 0) { if (chunk >= n) break; len = src[chunk++] + 18u; }
            else len += 2;
            for (; len && o < size; len--, o++) out[o] = o >= dist ? out[o - dist] : 0;
        }
        bits <<= 1;
        nbits--;
    }
    *out_size = size;
    return out;
}

/* The replacement font (recomp/fonts) as an RGBA atlas: white, coverage in alpha, FCOLS glyph cells of
 * (FCELL + 2 * FPAD) * m pixels a row, m = 3 for the supersampled one (hi), else 1; glyph g is character
 * g + 32 (Windows-1252). Also fills the advance widths (font_widths). NULL if the font doesn't decode.
 * The launcher draws with it, and so does the game window's story pilot picker (video.c). */
uint8_t *font_atlas_rgba(int hi, int *out_w, int *out_h) {
    uint32_t n = 0;
    uint8_t *d = yay0(g_font_western, g_font_western_size, &n);
    if (!d || n < 0x30) { free(d); return NULL; }
    uint32_t first = be16(d + 2), cell_w = be16(d + 0x10), cell_h = be16(d + 0x12), cols = be16(d + 0x1A);
    uint32_t sw = be16(d + 0x1E), sh = be16(d + 0x20), wtab = be16(d + 0x22), simg = be32(d + 0x24);
    uint8_t lv[4] = {d[0x2C], d[0x2D], d[0x2E], d[0x2F]};
    if (first != 32 || cell_w != FCELL || cell_h != FCELL || !cols || simg + sw * sh / 4 > n || wtab + FGLYPHS > n) {
        free(d);
        return NULL;
    }
    memcpy(g_fwidth, d + wtab, FGLYPHS);
    uint8_t *sheet = calloc((size_t)sw * sh, 1); /* alpha, 0-255 */
    for (uint32_t ty = 0; ty < sh / 8; ty++)
        for (uint32_t tx = 0; tx < sw / 8; tx++) {
            const uint8_t *t = d + simg + (ty * (sw / 8) + tx) * 16;
            for (uint32_t row = 0; row < 8; row++)
                for (uint32_t b = 0; b < 2; b++)
                    for (uint32_t k = 0; k < 4; k++)
                        sheet[(ty * 8 + row) * sw + tx * 8 + b * 4 + k] = lv[(t[row * 2 + b] >> (6 - 2 * k)) & 3];
        }
    free(d);
    {
        int k = hi ? 3 : 1, cell = (FCELL + 2 * FPAD) * k, aw = FCOLS * cell, ah = ((FGLYPHS + FCOLS - 1) / FCOLS) * cell;
        uint8_t *px = calloc((size_t)aw * ah * 4, 1);
        for (int g = 0; g < FGLYPHS; g++) {
            int sx0 = (int)(g % cols) * FCELL, sy0 = (int)(g / cols) * FCELL;
            int dx0 = (g % FCOLS) * cell, dy0 = (g / FCOLS) * cell;
            for (int y = 0; y < cell; y++)
                for (int x = 0; x < cell; x++) {
                    float fx = (x + 0.5f) / k - FPAD - 0.5f, fy = (y + 0.5f) / k - FPAD - 0.5f; /* in glyph pixels */
                    int ix = (int)floorf(fx), iy = (int)floorf(fy);
                    float ax = fx - ix, ay = fy - iy, v = 0;
                    for (int j = 0; j < 2; j++)
                        for (int i = 0; i < 2; i++) {
                            int gx = ix + i, gy = iy + j;
                            if (gx < 0 || gy < 0 || gx >= FCELL || gy >= FCELL) continue;
                            float w = (i ? ax : 1 - ax) * (j ? ay : 1 - ay);
                            v += w * sheet[(sy0 + gy) * (int)sw + sx0 + gx];
                        }
                    v /= 255.0f;
                    if (k > 1) { /* sharpen the upscaled edge */
                        float t = (v - 0.22f) / 0.56f;
                        t = t < 0 ? 0 : t > 1 ? 1 : t;
                        v = t * t * (3 - 2 * t);
                    }
                    uint8_t *p = px + ((size_t)(dy0 + y) * aw + dx0 + x) * 4;
                    p[0] = p[1] = p[2] = 255;
                    p[3] = (uint8_t)(v * 255 + 0.5f);
                }
        }
        free(sheet);
        *out_w = aw;
        *out_h = ah;
        return px;
    }
}

const uint8_t *font_widths(void) { return g_fwidth; }

static int font_init(SDL_Renderer *r) {
    for (int s = 0; s < 2; s++) {
        int aw, ah;
        uint8_t *px = font_atlas_rgba(s, &aw, &ah);
        if (!px) return 0;
        g_ftex[s] = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, aw, ah);
        if (g_ftex[s]) {
            SDL_UpdateTexture(g_ftex[s], NULL, px, aw * 4);
            SDL_SetTextureBlendMode(g_ftex[s], SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(g_ftex[s], SDL_ScaleModeLinear);
        }
        free(px);
    }
    return g_ftex[0] && g_ftex[1];
}

/* Next character as a font index (Windows-1252). UTF-8 is decoded; other bytes (Windows' ANSI file
 * names) are taken as they are. */
static int next_char(const char **ps) {
    const unsigned char *s = (const unsigned char *)*ps;
    int c = s[0];
    if (c < 0x80) { *ps += 1; return c; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) { *ps += 2; c = (c & 0x1F) << 6 | (s[1] & 0x3F); }
    else if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *ps += 3;
        c = (c & 0x0F) << 12 | (s[1] & 0x3F) << 6 | (s[2] & 0x3F);
        static const struct { int u, b; } map[] = {{0x20AC, 0x80}, {0x2026, 0x85}, {0x2018, 0x91}, {0x2019, 0x92},
                                                   {0x201C, 0x93}, {0x201D, 0x94}, {0x2022, 0x95}, {0x2013, 0x96},
                                                   {0x2014, 0x97}, {0x2122, 0x99}, {0x2039, 0x8B}, {0x203A, 0x9B}};
        int b = '?';
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (map[i].u == c) b = map[i].b;
        return b;
    } else if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *ps += 4;
        return '?';
    } else { *ps += 1; return c; }
    return c < 256 ? c : '?';
}

static float text_w(const char *s, float size, float track) {
    float w = 0, k = size / FCELL;
    int any = 0;
    while (*s) {
        int c = next_char(&s);
        if (c < 32 || c > 255) c = '?';
        w += g_fwidth[c - 32] * k + track;
        any = 1;
    }
    return any ? w - track : 0;
}

/* Draw at (x, y) = top-left of the 24-unit cell scaled to `size` pixels; returns the width. Bold draws
 * twice, a fraction of a pixel apart. */
static float text_ex(SDL_Renderer *r, const char *s, float x, float y, float size, SDL_Color c, float track, int bold) {
    if (!g_font_ok) return 0;
    float k = size / FCELL, x0 = x;
    int hi = k > 1.05f;
    SDL_Texture *t = g_ftex[hi];
    int m = hi ? 3 : 1, cell = (FCELL + 2 * FPAD) * m;
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    while (*s) {
        int ch = next_char(&s);
        if (ch < 32 || ch > 255) ch = '?';
        int g = ch - 32;
        SDL_Rect src = {(g % FCOLS) * cell, (g / FCOLS) * cell, cell, cell};
        SDL_FRect dst = {x - FPAD * k, y - FPAD * k, (FCELL + 2 * FPAD) * k, (FCELL + 2 * FPAD) * k};
        if (ch != ' ') {
            SDL_RenderCopyF(r, t, &src, &dst);
            if (bold) { dst.x += size * 0.035f; SDL_RenderCopyF(r, t, &src, &dst); }
        }
        x += g_fwidth[g] * k + track;
    }
    return x - x0;
}
static float text(SDL_Renderer *r, const char *s, float x, float y, float size, SDL_Color c) {
    return text_ex(r, s, x, y, size, c, 0, 0);
}
/* vertically centred in [y, y + h]: the font's capitals sit about 0.58 of the cell down */
static float text_vc(SDL_Renderer *r, const char *s, float x, float y, float h, float size, SDL_Color c, int bold) {
    return text_ex(r, s, x, y + h / 2 - size * 0.56f, size, c, 0, bold);
}

/* Fit into max_w pixels with an ellipsis: at the end, or (paths) at the start to keep the file name. */
static void ellipsize(char *out, size_t n, const char *s, float size, float max_w, int keep_end) {
    snprintf(out, n, "%s", s);
    if (text_w(out, size, 0) <= max_w) return;
    size_t len = strlen(s);
    for (size_t cut = 1; cut < len; cut++) {
        if (keep_end) snprintf(out, n, ELLIPSIS "%s", s + cut);
        else snprintf(out, n, "%.*s" ELLIPSIS, (int)(len - cut), s);
        if (text_w(out, size, 0) <= max_w) return;
    }
}

/* ---- shapes ---------------------------------------------------------------------------------------
 * A smooth white disc (rounded corners, dots, icons) and a soft radial glow, generated at startup. */
static SDL_Texture *g_round, *g_glow;

static SDL_Texture *make_round(SDL_Renderer *r, int size, int soft) {
    uint8_t *px = malloc((size_t)size * size * 4);
    float c = size / 2.0f;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float d = sqrtf((x + 0.5f - c) * (x + 0.5f - c) + (y + 0.5f - c) * (y + 0.5f - c)) / c, a;
            if (soft) { a = 1 - d; a = a < 0 ? 0 : a * a * a; }
            else { a = (1 - d) * c + 0.5f; a = a < 0 ? 0 : a > 1 ? 1 : a; }
            uint8_t *p = px + ((size_t)y * size + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (uint8_t)(a * 255 + 0.5f);
        }
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, size, size);
    if (t) {
        SDL_UpdateTexture(t, NULL, px, size * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    }
    free(px);
    return t;
}

static void tint(SDL_Texture *t, SDL_Color c) { SDL_SetTextureColorMod(t, c.r, c.g, c.b); SDL_SetTextureAlphaMod(t, c.a); }

static void circle(SDL_Renderer *r, float cx, float cy, float rad, SDL_Color c) {
    tint(g_round, c);
    SDL_FRect d = {cx - rad, cy - rad, rad * 2, rad * 2};
    SDL_RenderCopyF(r, g_round, NULL, &d);
}

static void glow(SDL_Renderer *r, float cx, float cy, float rad, SDL_Color c) {
    tint(g_glow, c);
    SDL_FRect d = {cx - rad, cy - rad, rad * 2, rad * 2};
    SDL_RenderCopyF(r, g_glow, NULL, &d);
}

/* Filled rounded rectangle: four disc quarters and three rectangles, which don't overlap (so
 * translucent colours stay even). */
static void rrect(SDL_Renderer *r, SDL_FRect b, float rad, SDL_Color c) {
    b.x = roundf(b.x); b.y = roundf(b.y); b.w = roundf(b.w); b.h = roundf(b.h);
    if (rad > b.w / 2) rad = b.w / 2;
    if (rad > b.h / 2) rad = b.h / 2;
    rad = roundf(rad);
    int q = 0;
    SDL_QueryTexture(g_round, NULL, NULL, &q, NULL);
    q /= 2;
    tint(g_round, c);
    SDL_Rect s[4] = {{0, 0, q, q}, {q, 0, q, q}, {0, q, q, q}, {q, q, q, q}};
    SDL_FRect d[4] = {{b.x, b.y, rad, rad}, {b.x + b.w - rad, b.y, rad, rad},
                      {b.x, b.y + b.h - rad, rad, rad}, {b.x + b.w - rad, b.y + b.h - rad, rad, rad}};
    for (int i = 0; i < 4; i++) SDL_RenderCopyF(r, g_round, &s[i], &d[i]);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_FRect m[3] = {{b.x + rad, b.y, b.w - 2 * rad, b.h}, {b.x, b.y + rad, rad, b.h - 2 * rad},
                      {b.x + b.w - rad, b.y + rad, rad, b.h - 2 * rad}};
    SDL_RenderFillRectsF(r, m, 3);
}

static SDL_FRect grow(SDL_FRect b, float k) { return (SDL_FRect){b.x - k, b.y - k, b.w + 2 * k, b.h + 2 * k}; }

static int inside(SDL_FRect b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

/* Vertical gradient over a rectangle. */
static void vgradient(SDL_Renderer *r, SDL_FRect b, SDL_Color top, SDL_Color bottom) {
    SDL_Vertex v[4] = {{{b.x, b.y}, top, {0, 0}}, {{b.x + b.w, b.y}, top, {0, 0}},
                       {{b.x, b.y + b.h}, bottom, {0, 0}}, {{b.x + b.w, b.y + b.h}, bottom, {0, 0}}};
    static const int idx[6] = {0, 1, 2, 1, 3, 2};
    SDL_RenderGeometry(r, NULL, v, 4, idx, 6);
}

/* ---- icons ----------------------------------------------------------------------------------------- */
static void icon_disc(SDL_Renderer *r, float cx, float cy, float rad, SDL_Color c, SDL_Color bg) {
    circle(r, cx, cy, rad, c);
    circle(r, cx, cy, rad * 0.62f, mix(c, bg, 0.35f));
    circle(r, cx, cy, rad * 0.30f, c);
    circle(r, cx, cy, rad * 0.14f, bg);
}

static void icon_folder(SDL_Renderer *r, float x, float y, SDL_Color c) {
    rrect(r, (SDL_FRect){x, y, 10, 6}, 2, c);
    rrect(r, (SDL_FRect){x, y + 3, 22, 15}, 3, c);
}

static void icon_place(SDL_Renderer *r, float x, float y, SDL_Color c) {
    rrect(r, (SDL_FRect){x, y + 2, 22, 14}, 3, c);
    rrect(r, (SDL_FRect){x + 7, y + 16, 8, 3}, 1, c);
}

/* A round controller-button badge ("A", "B") or a key cap ("F11", "START"). */
static float badge(SDL_Renderer *r, const char *label, float x, float cy, SDL_Color c, int round) {
    float w = text_w(label, 12, 0.5f);
    if (round) {
        circle(r, x + 11, cy, 11, c);
        text_ex(r, label, x + 11 - w / 2, cy - 7.4f, 12, C_DARK, 0.5f, 1);
        return 22;
    }
    float bw = w + 16;
    rrect(r, (SDL_FRect){x, cy - 11, bw, 22}, 6, c);
    text_ex(r, label, x + 8, cy - 7.4f, 12, C_DARK, 0.5f, 1);
    return bw;
}

/* ---- disc checks ------------------------------------------------------------------------------- */
static uint32_t crc32_of(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static int read_at(FILE *f, uint64_t off, void *buf, size_t n) {
#ifdef _WIN32
    if (_fseeki64(f, (int64_t)off, SEEK_SET) != 0) return 0;
#else
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return 0;
#endif
    return fread(buf, 1, n, f) == n;
}

static int path_is_dir(const char *path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static int has_suffix(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    if (n < e) return 0;
    for (size_t k = 0; k < e; k++) {
        char a = name[n - e + k];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != ext[k]) return 0;
    }
    return 1;
}

static int is_compressed_image(const char *name) {
    return has_suffix(name, ".rvz") || has_suffix(name, ".wia") || has_suffix(name, ".gcz") ||
           has_suffix(name, ".ciso") || has_suffix(name, ".wbfs") || has_suffix(name, ".nkit.iso");
}

/* Check a disc image or extracted-disc folder; on failure, say why in msg. */
static int check_disc(const char *path, char *msg, size_t n) {
    uint8_t boot[0x440], hdr[0x100];
    uint8_t *dol = NULL;
    uint32_t size = 0;
    if (path_is_dir(path)) {
        char p[1100];
        snprintf(p, sizeof p, "%s/sys/boot.bin", path);
        FILE *f = fopen(p, "rb");
        if (!f) { snprintf(msg, n, "That folder isn't an extracted disc (no sys/boot.bin)"); return 0; }
        int ok = fread(boot, 1, 6, f) == 6;
        fclose(f);
        if (!ok || memcmp(boot, "GF7E01", 6)) { snprintf(msg, n, "Not Star Fox: Assault (USA, GF7E01)"); return 0; }
        snprintf(p, sizeof p, "%s/sys/main.dol", path);
        f = fopen(p, "rb");
        if (!f) { snprintf(msg, n, "No sys/main.dol in that folder"); return 0; }
        fseek(f, 0, SEEK_END);
        size = (uint32_t)ftell(f);
        dol = malloc(size ? size : 1);
        ok = dol && read_at(f, 0, dol, size);
        fclose(f);
        if (!ok) { free(dol); snprintf(msg, n, "Can't read sys/main.dol"); return 0; }
    } else {
        if (is_compressed_image(path)) {
            snprintf(msg, n, "Compressed image: convert it to ISO in Dolphin first");
            return 0;
        }
        FILE *f = fopen(path, "rb");
        if (!f) { snprintf(msg, n, "Can't open that file"); return 0; }
        if (!read_at(f, 0, boot, sizeof boot)) { fclose(f); snprintf(msg, n, "Not a GameCube disc image"); return 0; }
        if (memcmp(boot, "GF7E01", 6)) {
            fclose(f);
            int printable = 1;
            for (int i = 0; i < 6; i++) printable &= boot[i] >= 32 && boot[i] < 127;
            if (printable) snprintf(msg, n, "That's %.6s, not Star Fox: Assault (USA, GF7E01)", (const char *)boot);
            else snprintf(msg, n, "Not a GameCube disc image");
            return 0;
        }
        uint32_t dol_off = be32(boot + 0x420);
        if (!read_at(f, dol_off, hdr, sizeof hdr)) { fclose(f); snprintf(msg, n, "The disc image is incomplete"); return 0; }
        size = 0x100;
        for (int i = 0; i < 18; i++) {
            uint32_t len = be32(hdr + 0x90 + i * 4), end = be32(hdr + i * 4) + len;
            if (len && end > size) size = end;
        }
        dol = malloc(size);
        int ok = dol && read_at(f, dol_off, dol, size);
        fclose(f);
        if (!ok) { free(dol); snprintf(msg, n, "The disc image is incomplete"); return 0; }
    }
    uint32_t crc = crc32_of(dol, size);
    free(dol);
    if (size != RT_DOL_SIZE || crc != RT_DOL_CRC32) {
        snprintf(msg, n, "A different version of the game: this build needs the USA disc");
        return 0;
    }
    snprintf(msg, n, "Ready to play");
    return 1;
}

/* ---- settings ---------------------------------------------------------------------------------- */
static char g_disc[1024];
static int g_fullscreen;
static int g_fps60; /* frame rate: 0 the game's own, 1 60 fps */
static int g_check_updates = 1; /* look for a newer release on GitHub when the start screen opens */

static void load_settings(void) {
    FILE *f = fopen("user/launcher.cfg", "r");
    if (!f) return;
    char line[1100];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "disc=", 5)) snprintf(g_disc, sizeof g_disc, "%s", line + 5);
        else if (!strncmp(line, "fullscreen=", 11)) g_fullscreen = atoi(line + 11) != 0;
        else if (!strncmp(line, "fps60=", 6)) g_fps60 = atoi(line + 6) != 0;
        else if (!strncmp(line, "check_updates=", 14)) g_check_updates = atoi(line + 14) != 0;
    }
    fclose(f);
}

static void save_settings(void) {
    MKDIR("user");
    FILE *f = fopen("user/launcher.cfg", "w");
    if (!f) return;
    fprintf(f, "disc=%s\nfullscreen=%d\nfps60=%d\ncheck_updates=%d\n", g_disc, g_fullscreen, g_fps60,
            g_check_updates);
    fclose(f);
}

/* The saved frame-rate choice (also used when the start screen is skipped). */
int launcher_fps60(void) {
    load_settings();
    return g_fps60;
}

/* ---- built-in file browser ------------------------------------------------------------------------ */
enum { K_DIR, K_DISC, K_COMPRESSED, K_PLACE, K_BACK, K_UP };
typedef struct { char name[260]; char path[1100]; int kind; } Entry;
#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif
static Entry *g_entries;
static int g_nentries, g_cap_entries;
static char g_cwd[1100]; /* "" = the list of places */

static void add_entry(const char *name, const char *path, int kind) {
    if (g_nentries == g_cap_entries) {
        g_cap_entries = g_cap_entries ? g_cap_entries * 2 : 256;
        g_entries = realloc(g_entries, sizeof *g_entries * (size_t)g_cap_entries);
    }
    Entry *e = &g_entries[g_nentries++];
    snprintf(e->name, sizeof e->name, "%s", name);
    snprintf(e->path, sizeof e->path, "%s", path);
    e->kind = kind;
}

static int entry_cmp(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if ((x->kind == K_DIR) != (y->kind == K_DIR)) return x->kind == K_DIR ? -1 : 1;
    return SDL_strcasecmp(x->name, y->name);
}

static void exe_dir(char *out, size_t n) {
    out[0] = 0;
#ifdef _WIN32
    DWORD k = GetModuleFileNameA(NULL, out, (DWORD)n);
    if (k == 0 || k >= n) { out[0] = 0; return; }
#else
    ssize_t k = readlink("/proc/self/exe", out, n - 1);
    out[k > 0 ? k : 0] = 0;
#endif
    char *s = strrchr(out, '/'), *b = strrchr(out, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
}

static void list_places(void) {
    char p[1100];
    g_nentries = 0;
    g_cwd[0] = 0;
    exe_dir(p, sizeof p);
    if (p[0]) add_entry("Program folder", p, K_PLACE);
    const char *home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
    if (home) {
        add_entry("Home", home, K_PLACE);
        snprintf(p, sizeof p, "%s" SEP "Desktop", home);
        if (path_is_dir(p)) add_entry("Desktop", p, K_PLACE);
        snprintf(p, sizeof p, "%s" SEP "Downloads", home);
        if (path_is_dir(p)) add_entry("Downloads", p, K_PLACE);
    }
#ifdef _WIN32
    DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; d++)
        if (drives & (1u << d)) {
            char name[16];
            snprintf(p, sizeof p, "%c:\\", 'A' + d);
            snprintf(name, sizeof name, "Drive %c:", 'A' + d);
            add_entry(name, p, K_PLACE);
        }
#else
    const char *user = getenv("USER");
    if (user) {
        snprintf(p, sizeof p, "/run/media/%s", user); /* SD cards and USB drives on the Steam Deck */
        if (path_is_dir(p)) add_entry("Removable media", p, K_PLACE);
    }
    if (path_is_dir("/media")) add_entry("/media", "/media", K_PLACE);
    add_entry("/ (root)", "/", K_PLACE);
#endif
}

/* List `dir`: folders, and disc images (compressed ones too, shown as unusable). */
static void list_dir(const char *dir_in) {
    char dir[1100];
#ifdef _WIN32
    if (!GetFullPathNameA(dir_in, sizeof dir, dir, NULL)) snprintf(dir, sizeof dir, "%s", dir_in);
#else
    if (!realpath(dir_in, dir)) snprintf(dir, sizeof dir, "%s", dir_in);
#endif
    g_nentries = 0;
    snprintf(g_cwd, sizeof g_cwd, "%s", dir);
    add_entry("Up one folder", "", K_UP);
    char full[1400];
#ifdef _WIN32
    char pattern[1100];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const char *name = fd.cFileName;
            int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
#else
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            const char *name = de->d_name;
            snprintf(full, sizeof full, "%s/%s", dir, name);
            int is_dir = path_is_dir(full);
#endif
            if (name[0] == '.') continue;
            snprintf(full, sizeof full, "%s%s%s", dir, (dir[strlen(dir) - 1] == '/' || dir[strlen(dir) - 1] == '\\') ? "" : SEP, name);
            if (is_dir) add_entry(name, full, K_DIR);
            else if (is_compressed_image(name)) add_entry(name, full, K_COMPRESSED);
            else if (has_suffix(name, ".iso") || has_suffix(name, ".gcm")) add_entry(name, full, K_DISC);
#ifdef _WIN32
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
        }
        closedir(d);
    }
#endif
    if (g_nentries > 1) qsort(g_entries + 1, (size_t)(g_nentries - 1), sizeof *g_entries, entry_cmp);
    /* an extracted disc folder can be used as it is */
    snprintf(full, sizeof full, "%s/sys/boot.bin", dir);
    if (file_exists(full)) add_entry("Use this extracted disc folder", dir, K_DISC);
}

static void parent_of(char *dir) {
    size_t n = strlen(dir);
    while (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) dir[--n] = 0;
    char *s = strrchr(dir, '/'), *b = strrchr(dir, '\\');
    if (b > s) s = b;
    if (!s) { dir[0] = 0; return; }
    if (s == dir) { dir[1] = 0; return; } /* "/" */
#ifdef _WIN32
    if (s == dir + 2 && dir[1] == ':') { dir[3] = 0; return; } /* "C:\" */
#endif
    *s = 0;
}

static const char *base_name(const char *p) {
    const char *b = p + strlen(p);
    while (b > p && b[-1] != '/' && b[-1] != '\\') b--;
    return *b ? b : p;
}

/* The folder the disc picker should open in. */
static void start_folder(char *out, size_t n) {
    out[0] = 0;
    if (g_disc[0]) {
        snprintf(out, n, "%s", g_disc);
        if (!path_is_dir(out)) parent_of(out);
    }
    if (!out[0] || !path_is_dir(out)) exe_dir(out, n);
    char abs[1100]; /* dialogs want an absolute path */
#ifdef _WIN32
    if (GetFullPathNameA(out, sizeof abs, abs, NULL)) snprintf(out, n, "%s", abs);
#else
    if (realpath(out, abs)) snprintf(out, n, "%s", abs);
#endif
}

/* ---- the system file dialog ------------------------------------------------------------------------
 * Runs on its own thread so the start screen keeps drawing. result: 1 a file was picked, 0 cancelled,
 * -1 no dialog available. */
typedef struct {
    SDL_atomic_t done;
    int result, error;
    char path[1100], start[1100];
    void *owner; /* HWND on Windows */
} FileDialog;
static FileDialog g_dialog;
static SDL_Thread *g_dialog_thread;

#ifdef _WIN32
static int dialog_thread(void *arg) {
    FileDialog *d = arg;
    char file[1100] = "";
    /* the Open dialog is a COM component: this thread needs its own apartment */
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = (HWND)d->owner;
    ofn.lpstrFilter = "GameCube disc images (*.iso, *.gcm)\0*.iso;*.gcm\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file;
    ofn.lpstrInitialDir = d->start[0] ? d->start : NULL;
    ofn.lpstrTitle = "Choose your Star Fox: Assault disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (GetOpenFileNameA(&ofn)) {
        snprintf(d->path, sizeof d->path, "%s", file);
        d->result = 1;
    } else {
        DWORD err = CommDlgExtendedError();
        d->result = err ? -1 : 0;
        d->error = (int)err;
    }
    if (SUCCEEDED(co)) CoUninitialize();
    SDL_AtomicSet(&d->done, 1);
    return 0;
}

static int dialog_available(void) { return 1; }
#else
static int have_cmd(const char *name) {
    char c[160];
    snprintf(c, sizeof c, "command -v %s >/dev/null 2>&1", name);
    return system(c) == 0;
}

/* kdialog on KDE (the Steam Deck's desktop), else zenity, else kdialog anywhere; NULL if neither. */
static const char *dialog_program(void) {
    static const char *prog = (const char *)1;
    if (prog != (const char *)1) return prog;
    const char *desk = getenv("XDG_CURRENT_DESKTOP");
    int kde = desk && strstr(desk, "KDE");
    int k = have_cmd("kdialog"), z = have_cmd("zenity");
    prog = kde && k ? "kdialog" : z ? "zenity" : k ? "kdialog" : NULL;
    return prog;
}

static int dialog_available(void) { return dialog_program() != NULL; }

/* 'text' for sh, with embedded single quotes escaped */
static void shell_quote(char *out, size_t n, const char *s) {
    size_t o = 0;
    if (o < n - 1) out[o++] = '\'';
    for (; *s && o + 5 < n; s++) {
        if (*s == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
        else out[o++] = *s;
    }
    if (o < n - 1) out[o++] = '\'';
    out[o] = 0;
}

static int dialog_thread(void *arg) {
    FileDialog *d = arg;
    const char *prog = dialog_program();
    char dir[2300], cmd[3000];
    snprintf(cmd, sizeof cmd, "%s/", d->start);
    shell_quote(dir, sizeof dir, cmd);
    /* LD_PRELOAD (the Steam overlay) and Steam's library path are for the game, not the dialog */
    if (prog && !strcmp(prog, "kdialog"))
        snprintf(cmd, sizeof cmd, "env -u LD_PRELOAD -u LD_LIBRARY_PATH kdialog --title 'Choose your Star Fox: Assault disc image' "
                                  "--getopenfilename %s '*.iso *.gcm *.ISO *.GCM|GameCube disc images' 2>/dev/null", dir);
    else if (prog)
        snprintf(cmd, sizeof cmd, "env -u LD_PRELOAD -u LD_LIBRARY_PATH zenity --file-selection "
                                  "--title='Choose your Star Fox: Assault disc image' --filename=%s "
                                  "--file-filter='GameCube disc images | *.iso *.gcm *.ISO *.GCM' "
                                  "--file-filter='All files | *' 2>/dev/null", dir);
    d->result = -1;
    FILE *p = prog ? popen(cmd, "r") : NULL;
    if (p) {
        char line[1100] = "";
        if (!fgets(line, sizeof line, p)) line[0] = 0;
        int st = pclose(p);
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) { snprintf(d->path, sizeof d->path, "%s", line); d->result = 1; }
        else d->result = (WIFEXITED(st) && WEXITSTATUS(st) == 127) ? -1 : 0;
    }
    SDL_AtomicSet(&d->done, 1);
    return 0;
}
#endif

static void dialog_start(SDL_Window *win) {
    memset(&g_dialog, 0, sizeof g_dialog);
    start_folder(g_dialog.start, sizeof g_dialog.start);
#ifdef _WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(win, &info)) g_dialog.owner = info.info.win.window;
#else
    (void)win;
#endif
    g_dialog_thread = SDL_CreateThread(dialog_thread, "file dialog", &g_dialog);
}

/* The Deck's Game Mode (gamescope) shows one app at a time: a separate dialog window doesn't fit. */
static int in_game_mode(void) {
    const char *desk = getenv("XDG_CURRENT_DESKTOP");
    return getenv("GAMESCOPE_WAYLAND_DISPLAY") != NULL || (desk && !strcmp(desk, "gamescope"));
}

/* ---- the screen -------------------------------------------------------------------------------- */
#define VW 960
#define VH 600
enum { SCREEN_MAIN, SCREEN_BROWSE };
enum { ITEM_DISC, ITEM_DISPLAY, ITEM_FPS, ITEM_PLAY, ITEM_QUIT, ITEM_COUNT };
#define BROWSE_ROWS 9
#define ROW_H 40.0f

typedef struct { float x, y, z; } Star;
static Star g_stars[160];

static void star_reset(Star *s, int at_back) {
    s->x = (float)(rand() % 2000 - 1000);
    s->y = (float)(rand() % 2000 - 1000);
    s->z = at_back ? 1000.0f : (float)(rand() % 1000 + 1);
}

static void draw_background(SDL_Renderer *r, float dt) {
    vgradient(r, (SDL_FRect){0, 0, VW, VH}, (SDL_Color){6, 10, 22, 255}, (SDL_Color){12, 20, 42, 255});
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < (int)(sizeof g_stars / sizeof g_stars[0]); i++) { /* a slow warp */
        Star *s = &g_stars[i];
        float pz = s->z;
        s->z -= dt * 90.0f;
        if (s->z <= 1) { star_reset(s, 1); continue; }
        float x0 = VW * 0.68f + s->x / pz * 300, y0 = VH * 0.42f + s->y / pz * 300;
        float x1 = VW * 0.68f + s->x / s->z * 300, y1 = VH * 0.42f + s->y / s->z * 300;
        if (x1 < 0 || x1 >= VW || y1 < 0 || y1 >= VH) { star_reset(s, 1); continue; }
        float b = 1.0f - s->z / 1000.0f;
        SDL_SetRenderDrawColor(r, 170, 195, 255, (Uint8)(40 + 150 * b * b));
        SDL_RenderDrawLineF(r, x0, y0, x1, y1);
    }
    /* a planet rising at the bottom right, with a blue rim of light */
    glow(r, VW - 40, VH + 380, 760, alpha(C_ACCENT, 0.30f));
    circle(r, VW - 40, VH + 380, 560, (SDL_Color){14, 24, 52, 255});
    circle(r, VW - 52, VH + 392, 556, (SDL_Color){9, 15, 33, 255});
}

/* The update banner (top right): shown when a newer release is offered, and while it installs. */
static SDL_FRect update_banner(void) { return (SDL_FRect){VW - 64 - 300, 96, 300, 72}; }

static void draw_update_banner(SDL_Renderer *r, Uint32 now) {
    int st = updater_state();
    if (st != UPD_AVAILABLE && st != UPD_DOWNLOADING && st != UPD_INSTALLING && st != UPD_DONE && st != UPD_FAILED) return;
    SDL_FRect b = update_banner();
    float pulse = st == UPD_AVAILABLE ? 0.5f + 0.5f * sinf((float)now / 420.0f) : 0;
    rrect(r, b, 14, st == UPD_FAILED ? (SDL_Color){60, 24, 32, 255} : mix(C_CARD, (SDL_Color){30, 58, 110, 255}, pulse));
    const char *title = st == UPD_AVAILABLE ? "Update available" : st == UPD_FAILED ? "Update failed"
                      : st == UPD_DONE ? "Updated" : "Updating";
    text(r, title, b.x + 18, b.y + 12, 16, st == UPD_FAILED ? C_RED : C_TEXT);
    char line[160];
    if (st == UPD_AVAILABLE) snprintf(line, sizeof line, "v%s  " BULLET "  Y / click to install", updater_new_version());
    else snprintf(line, sizeof line, "%s", updater_message());
    char fit[160];
    ellipsize(fit, sizeof fit, line, 14, b.w - 36, 0);
    text(r, fit, b.x + 18, b.y + 40, 14, st == UPD_FAILED ? C_RED : C_MUTED);
}

/* Main-screen layout (logical 960x600). */
typedef struct { SDL_FRect disc_card, browse, display_card, seg, fps_card, fps_seg, play, quit; } Layout;
static Layout main_layout(void) {
    Layout L;
    L.disc_card = (SDL_FRect){64, 214, 560, 104};
    L.browse = (SDL_FRect){64 + 560 - 22 - 124, 214 + 32, 124, 40};
    L.display_card = (SDL_FRect){64, 330, 560, 60};
    L.seg = (SDL_FRect){64 + 560 - 12 - 250, 330 + 10, 250, 40};
    L.fps_card = (SDL_FRect){64, 400, 560, 60};
    L.fps_seg = (SDL_FRect){64 + 560 - 12 - 250, 400 + 10, 250, 40};
    L.play = (SDL_FRect){64, 476, 424, 60};
    L.quit = (SDL_FRect){64 + 436, 476, 124, 60};
    return L;
}
static SDL_FRect item_rect(const Layout *L, int item) {
    return item == ITEM_DISC ? L->browse : item == ITEM_DISPLAY ? L->seg : item == ITEM_FPS ? L->fps_seg
         : item == ITEM_PLAY ? L->play : L->quit;
}

static SDL_FRect browse_list(void) { return (SDL_FRect){64, 164, VW - 128, BROWSE_ROWS * ROW_H + 16}; }
static SDL_FRect browse_back(void) { return (SDL_FRect){64, 44, 92, 34}; }

static float approach(float v, float target, float dt, float speed) { return v + (target - v) * (1 - expf(-dt * speed)); }

static void hints(SDL_Renderer *r, const char *const *items, int n) {
    /* items: pairs of (badge, label); a badge starting with '#' is a key cap, else a round button */
    float x = 64, cy = 566;
    for (int i = 0; i + 1 < n; i += 2) {
        const char *b = items[i];
        int round = b[0] != '#';
        SDL_Color c = !strcmp(b, "A") ? C_GREEN : !strcmp(b, "B") ? C_RED : !strcmp(b, "Y") ? C_AMBER : C_MUTED;
        x += badge(r, round ? b : b + 1, x, cy, c, round) + 8;
        x += text(r, items[i + 1], x, cy - 8.6f, 15, C_MUTED) + 22;
    }
}

/* Controllers panel (right of the cards): every connected controller with the player number it gets in
 * the game, in the same order the game assigns them (lowest free port, in connection order). */
#define MAX_PADS 8
static SDL_GameController *g_pads[MAX_PADS];
static int g_npads;

static void pads_add(int device_index) {
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
    for (int i = 0; i < g_npads; i++)
        if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[i])) == id) return;
    if (g_npads == MAX_PADS) return;
    SDL_GameController *gc = SDL_GameControllerOpen(device_index);
    if (!gc) return;
    if (g_npads < 4) SDL_GameControllerSetPlayerIndex(gc, g_npads);
    g_pads[g_npads++] = gc;
}

static void pads_remove(SDL_JoystickID id) {
    for (int i = 0; i < g_npads; i++)
        if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[i])) == id) {
            SDL_GameControllerClose(g_pads[i]);
            memmove(&g_pads[i], &g_pads[i + 1], (size_t)(g_npads - i - 1) * sizeof g_pads[0]);
            g_npads--;
            for (int k = i; k < g_npads && k < 4; k++) SDL_GameControllerSetPlayerIndex(g_pads[k], k);
            return;
        }
}

static void draw_pads(SDL_Renderer *r, float fade) {
    static const SDL_Color player[4] = {{248, 113, 113, 255}, {96, 160, 255, 255}, {251, 191, 36, 255},
                                        {52, 211, 153, 255}}; /* the game's player colours */
    const float x = 656, w = VW - 64 - x;
    text_ex(r, "CONTROLLERS", x, 222, 12, alpha(C_ACCENT_HI, fade), 2.2f, 1);
    float y = 252;
    int shown = g_npads < 4 ? g_npads : 4;
    for (int i = 0; i < shown; i++, y += 30) {
        char label[2] = {(char)('1' + i), 0}, name[64];
        badge(r, label, x, y, alpha(player[i], fade), 1);
        const char *n = SDL_GameControllerName(g_pads[i]);
        snprintf(name, sizeof name, "%s", n ? n : "Controller");
        while (strlen(name) > 3 && text_w(name, 14, 0) > w - 34) { /* shorten long names */
            size_t l = strlen(name);
            memcpy(name + l - 4, "...", 4);
        }
        text(r, name, x + 32, y - 8, 14, alpha(C_TEXT, fade));
    }
    if (!g_npads) {
        badge(r, "1", x, y, alpha(player[0], fade), 1);
        text(r, "Keyboard", x + 32, y - 8, 14, alpha(C_TEXT, fade));
        y += 30;
    } else if (g_npads > 4) {
        char more[48];
        snprintf(more, sizeof more, "+%d more (not used)", g_npads - 4);
        text(r, more, x + 32, y - 8, 13, alpha(C_FAINT, fade));
        y += 26;
    }
    if (g_npads) {
        text(r, "Player 1 also has the keyboard.", x, y + 4, 12, alpha(C_FAINT, fade));
        y += 18;
    }
    text(r, "VS mode: up to 4 players.", x, y + 4, 12, alpha(C_FAINT, fade));
    text(r, "Bots: Z on the character screen,", x, y + 22, 12, alpha(C_FAINT, fade));
    text(r, "then left/right and A to choose", x, y + 38, 12, alpha(C_FAINT, fade));
    text(r, "its character (Z again: random).", x, y + 54, 12, alpha(C_FAINT, fade));
    text(r, "Backing out removes them.", x, y + 70, 12, alpha(C_FAINT, fade));
}

int launcher_run(const char *found_disc, int *fullscreen_io, char *disc_out, size_t disc_n) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_STEAMDECK", "1"); /* the Deck's own controls, also when not started from Steam */
    /* Windows: Xbox/XInput pads reach SDL as raw input messages to a hidden window on the thread that
     * initialises the joystick subsystem. That is this (the main) thread, which runs the game after the
     * start screen and never reads messages again, so those pads went dead in the game. With this hint
     * SDL owns that window on a thread of its own. SDL stays initialised into the game, so it's set here. */
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) return -1;
    load_settings();
    if (*fullscreen_io) g_fullscreen = 1;
    char msg[256] = "", tmp[1200];
    int disc_ok = 0;
    if (g_disc[0]) disc_ok = check_disc(g_disc, msg, sizeof msg);
    if (!disc_ok && found_disc) {
        snprintf(g_disc, sizeof g_disc, "%s", found_disc);
        disc_ok = check_disc(g_disc, msg, sizeof msg);
    }
    if (!disc_ok && !g_disc[0]) snprintf(msg, sizeof msg, "Choose your disc image (.iso or .gcm)");

    SDL_Window *win = SDL_CreateWindow("Star Fox: Assault (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       VW, VH, SDL_WINDOW_RESIZABLE | (g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) return -1;
    SDL_Renderer *r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!r) r = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!r) { SDL_DestroyWindow(win); return -1; }
    SDL_RenderSetLogicalSize(r, VW, VH);
    g_font_ok = font_init(r);
    g_round = make_round(r, 256, 0);
    g_glow = make_round(r, 256, 1);
    for (int i = 0; i < (int)(sizeof g_stars / sizeof g_stars[0]); i++) star_reset(&g_stars[i], 0);
    for (int i = 0; i < SDL_NumJoysticks(); i++) /* every controller can work the start screen */
        if (SDL_IsGameController(i)) pads_add(i);

    const char *script = getenv("SFA_LAUNCHER_SCRIPT"); /* testing: see below */
    if (g_check_updates && !getenv("SFA_NO_UPDATE_CHECK") && (!script || getenv("SFA_UPDATE_TEST"))) updater_check_start();
    int screen = SCREEN_MAIN, sel = disc_ok ? ITEM_PLAY : ITEM_DISC, bsel = 0, scroll = 0, result = 0;
    int stick_held = 0, msg_ok = disc_ok, dialog_open = 0, pad_input = 0, focus_visible = 0, hover = -1, bhover = -1;
    float hov[ITEM_COUNT] = {0}, seg_pos = g_fullscreen ? 1.0f : 0.0f, fps_pos = g_fps60 ? 1.0f : 0.0f, fade = 0;
    Uint32 last = SDL_GetTicks();
    Layout L = main_layout();

    /* open the disc picker: the system dialog, or the built-in browser */
    #define OPEN_PICKER(use_builtin)                                                           \
        do {                                                                                   \
            if (!(use_builtin) && dialog_available()) {                                        \
                dialog_start(win);                                                             \
                dialog_open = 1;                                                               \
                printf("[launcher] opening the system file dialog in %s\n", g_dialog.start);   \
            }                                                                                  \
            else {                                                                             \
                screen = SCREEN_BROWSE;                                                        \
                start_folder(tmp, sizeof tmp);                                                 \
                if (tmp[0] && path_is_dir(tmp)) list_dir(tmp); else list_places();           \
                bsel = scroll = 0;                                                             \
                if (g_disc[0])                                                                 \
                    for (int i_ = 0; i_ < g_nentries; i_++)                                    \
                        if (g_entries[i_].kind == K_DISC && !strcmp(g_entries[i_].name, base_name(g_disc))) bsel = i_; \
            }                                                                                  \
        } while (0)

    for (int running = 1; running;) {
        int move = 0, side = 0, act = 0, back = 0, play = 0, clicked = -1, wheel = 0, seg_click = -1, fps_click = -1, update = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { running = 0; result = 0; }
            if (dialog_open) continue; /* the system dialog has the input */
            if (e.type == SDL_KEYDOWN) {
                pad_input = 0;
                focus_visible = 1;
                switch (e.key.keysym.sym) {
                case SDLK_UP: move = -1; break;
                case SDLK_DOWN: move = 1; break;
                case SDLK_LEFT: side = -1; break;
                case SDLK_RIGHT: side = 1; break;
                case SDLK_PAGEUP: move = -BROWSE_ROWS; break;
                case SDLK_PAGEDOWN: move = BROWSE_ROWS; break;
                case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
                    if (e.key.keysym.mod & KMOD_ALT) {
                        g_fullscreen = !g_fullscreen;
                        SDL_SetWindowFullscreen(win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    } else act = 1;
                    break;
                case SDLK_ESCAPE: case SDLK_BACKSPACE: back = 1; break;
                case SDLK_u: update = 1; break;
                case SDLK_F11:
                    g_fullscreen = !g_fullscreen;
                    SDL_SetWindowFullscreen(win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    break;
                default: break;
                }
            } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                pads_add(e.cdevice.which);
            } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
                pads_remove(e.cdevice.which);
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                pad_input = 1;
                focus_visible = 1;
                switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: move = -1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move = 1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: side = -1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: side = 1; break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: move = -BROWSE_ROWS; break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: move = BROWSE_ROWS; break;
                case SDL_CONTROLLER_BUTTON_A: act = 1; break;
                case SDL_CONTROLLER_BUTTON_B: back = 1; break;
                case SDL_CONTROLLER_BUTTON_START: play = 1; break;
                case SDL_CONTROLLER_BUTTON_Y: update = 1; break;
                default: break;
                }
            } else if (e.type == SDL_CONTROLLERAXISMOTION &&
                       (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY || e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)) {
                int v = e.caxis.value, y = e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY;
                if (!stick_held && (v < -20000 || v > 20000)) {
                    if (y) move = v < 0 ? -1 : 1; else side = v < 0 ? -1 : 1;
                    stick_held = 1 + y;
                    pad_input = focus_visible = 1;
                } else if (stick_held == 1 + y && v > -8000 && v < 8000) stick_held = 0;
            } else if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN) {
                int mx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x;
                int my = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
                /* hover follows the mouse only once it really moves (a window opening under the
                 * pointer, or the keyboard/controller in use, shouldn't jump the selection) */
                static int mouse_x = -1, mouse_y = -1;
                if (e.type == SDL_MOUSEMOTION) { /* the pointer's place on the screen, not in the window */
                    int gx, gy;
                    SDL_GetGlobalMouseState(&gx, &gy);
                    int first = mouse_x < 0, still = abs(gx - mouse_x) < 3 && abs(gy - mouse_y) < 3;
                    if (first || still) {
                        if (first) { mouse_x = gx; mouse_y = gy; }
                        continue;
                    }
                    mouse_x = gx;
                    mouse_y = gy;
                }
                pad_input = 0;
                focus_visible = 0;
                int hit = -1;
                if (screen == SCREEN_MAIN) {
                    if (inside(L.disc_card, mx, my)) hit = ITEM_DISC;
                    else if (inside(L.display_card, mx, my)) hit = ITEM_DISPLAY;
                    else if (inside(L.fps_card, mx, my)) hit = ITEM_FPS;
                    else if (inside(L.play, mx, my)) hit = ITEM_PLAY;
                    else if (inside(L.quit, mx, my)) hit = ITEM_QUIT;
                    hover = hit;
                    if (hit >= 0) sel = hit;
                    if (hit == ITEM_DISPLAY && e.type == SDL_MOUSEBUTTONDOWN)
                        seg_click = inside(L.seg, mx, my) ? (mx >= L.seg.x + L.seg.w / 2) : !g_fullscreen;
                    if (hit == ITEM_FPS && e.type == SDL_MOUSEBUTTONDOWN)
                        fps_click = inside(L.fps_seg, mx, my) ? (mx >= L.fps_seg.x + L.fps_seg.w / 2) : !g_fps60;
                } else {
                    SDL_FRect list = browse_list();
                    bhover = -1;
                    if (inside(browse_back(), mx, my)) hit = bhover = -2;
                    else if (inside(list, mx, my) && my >= list.y + 8) {
                        int k = (int)((my - list.y - 8) / ROW_H);
                        if (k < BROWSE_ROWS && scroll + k < g_nentries) { hit = scroll + k; bsel = hit; bhover = hit; }
                    }
                }
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && screen == SCREEN_MAIN &&
                    inside(update_banner(), mx, my))
                    update = 1;
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    if (hit == -2) back = 1;
                    else if (hit >= 0) clicked = hit;
                }
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) back = 1;
            } else if (e.type == SDL_MOUSEWHEEL) {
                wheel = -e.wheel.y * 3;
            }
        }

        /* SFA_LAUNCHER_SCRIPT=down,enter,...: testing aid; every 0.6 s, save a screenshot to
         * build/GF7E01/launcher_NN.bmp, then press the next key (up down left right pgdn enter esc
         * start); it always uses the built-in browser */
        static Uint32 script_next;
        static int shot_n, take_shot;
        if (script && SDL_GetTicks() >= script_next) {
            script_next = SDL_GetTicks() + 600;
            take_shot = 1;
            focus_visible = 1;
            if (!*script) { running = 0; result = 0; }
            size_t n = strcspn(script, ",");
            if (!strncmp(script, "up", n) && n == 2) move = -1;
            else if (!strncmp(script, "down", n) && n == 4) move = 1;
            else if (!strncmp(script, "left", n) && n == 4) side = -1;
            else if (!strncmp(script, "right", n) && n == 5) side = 1;
            else if (!strncmp(script, "pgdn", n) && n == 4) move = BROWSE_ROWS;
            else if (!strncmp(script, "enter", n) && n == 5) act = 1;
            else if (!strncmp(script, "esc", n) && n == 3) back = 1;
            else if (!strncmp(script, "start", n) && n == 5) play = 1;
            else if (!strncmp(script, "update", n) && n == 6) update = 1;
            script += n + (script[n] == ',');
        }

        /* the system dialog finished */
        if (dialog_open && SDL_AtomicGet(&g_dialog.done)) {
            SDL_WaitThread(g_dialog_thread, NULL);
            g_dialog_thread = NULL;
            dialog_open = 0;
            printf("[launcher] file dialog: %s", g_dialog.result == 1 ? "picked " : g_dialog.result == 0 ? "cancelled" : "unavailable");
            if (g_dialog.result == 1) printf("%s", g_dialog.path);
            if (g_dialog.error) printf(" (error 0x%X)", (unsigned)g_dialog.error);
            printf("\n");
            fflush(stdout);
            if (g_dialog.result == 1) {
                char m[256];
                int ok = check_disc(g_dialog.path, m, sizeof m);
                snprintf(msg, sizeof msg, "%s", m);
                msg_ok = ok;
                if (ok) {
                    snprintf(g_disc, sizeof g_disc, "%s", g_dialog.path);
                    disc_ok = 1;
                    save_settings();
                    sel = ITEM_PLAY;
                }
            } else if (g_dialog.result < 0) {
                OPEN_PICKER(1); /* no dialog program after all */
            }
            SDL_RaiseWindow(win);
        }

        if (screen == SCREEN_MAIN && !dialog_open) {
            if (clicked >= 0) { sel = clicked; act = (clicked != ITEM_DISPLAY || seg_click < 0) && (clicked != ITEM_FPS || fps_click < 0); }
            if (fps_click >= 0 && fps_click != g_fps60) { g_fps60 = fps_click; save_settings(); }
            if (seg_click >= 0 && seg_click != g_fullscreen) {
                g_fullscreen = seg_click;
                SDL_SetWindowFullscreen(win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                save_settings();
            }
            if (move) { /* rows: disc, display, frame rate, then play | quit */
                int row = sel == ITEM_DISC ? 0 : sel == ITEM_DISPLAY ? 1 : sel == ITEM_FPS ? 2 : 3;
                row = (row + (move > 0 ? 1 : 3)) % 4;
                sel = row == 0 ? ITEM_DISC : row == 1 ? ITEM_DISPLAY : row == 2 ? ITEM_FPS : ITEM_PLAY;
            }
            if (side) {
                if (sel == ITEM_DISPLAY && (side > 0) != g_fullscreen) {
                    g_fullscreen = side > 0;
                    SDL_SetWindowFullscreen(win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    save_settings();
                } else if (sel == ITEM_FPS && (side > 0) != g_fps60) {
                    g_fps60 = side > 0;
                    save_settings();
                } else if (sel == ITEM_PLAY || sel == ITEM_QUIT) sel = side > 0 ? ITEM_QUIT : ITEM_PLAY;
            }
            if (back) sel = ITEM_QUIT;
            if (update) updater_install_start(); /* only does something when an update is offered */
            if (play && disc_ok) { result = 1; running = 0; }
            if (act) {
                switch (sel) {
                case ITEM_PLAY:
                    if (disc_ok) { result = 1; running = 0; }
                    else { snprintf(msg, sizeof msg, "Choose your disc image first"); msg_ok = 0; sel = ITEM_DISC; }
                    break;
                case ITEM_DISC: /* testing: SFA_LAUNCHER_DIALOG=1 lets a script use the system dialog */
                    OPEN_PICKER(pad_input || (script && !getenv("SFA_LAUNCHER_DIALOG")) || in_game_mode());
                    break;
                case ITEM_DISPLAY:
                    g_fullscreen = !g_fullscreen;
                    SDL_SetWindowFullscreen(win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    save_settings();
                    break;
                case ITEM_FPS:
                    g_fps60 = !g_fps60;
                    save_settings();
                    break;
                case ITEM_QUIT: result = 0; running = 0; break;
                }
            }
        } else if (screen == SCREEN_BROWSE) {
            if (clicked >= 0) { bsel = clicked; act = 1; }
            if (move) {
                bsel += move;
                if (bsel < 0) bsel = 0;
                if (bsel >= g_nentries) bsel = g_nentries - 1;
            }
            if (wheel) {
                scroll += wheel;
                if (scroll > g_nentries - BROWSE_ROWS) scroll = g_nentries - BROWSE_ROWS;
                if (scroll < 0) scroll = 0;
            }
            if (play && disc_ok) { result = 1; running = 0; }
            if (back) {
                if (g_cwd[0]) { act = 1; bsel = 0; } /* up one folder */
                else { screen = SCREEN_MAIN; act = 0; }
            }
            if (act && g_nentries > 0 && bsel >= 0 && bsel < g_nentries) {
                Entry en = g_entries[bsel];
                if (en.kind == K_UP) {
                    char up[1100], before[1100];
                    snprintf(up, sizeof up, "%s", g_cwd);
                    snprintf(before, sizeof before, "%s", up);
                    parent_of(up);
                    if (!up[0] || !strcmp(up, before)) list_places();
                    else list_dir(up);
                    bsel = scroll = 0;
                } else if (en.kind == K_DIR || en.kind == K_PLACE) {
                    list_dir(en.path);
                    bsel = scroll = 0;
                } else if (en.kind == K_DISC || en.kind == K_COMPRESSED) {
                    char m[256];
                    int ok = check_disc(en.path, m, sizeof m);
                    snprintf(msg, sizeof msg, "%s", m);
                    msg_ok = ok;
                    if (ok) {
                        snprintf(g_disc, sizeof g_disc, "%s", en.path);
                        disc_ok = 1;
                        save_settings();
                        screen = SCREEN_MAIN;
                        sel = ITEM_PLAY;
                    }
                }
            }
            if (bsel < scroll) scroll = bsel;
            if (bsel >= scroll + BROWSE_ROWS) scroll = bsel - BROWSE_ROWS + 1;
        }

        /* ---- draw ---- */
        Uint32 now = SDL_GetTicks();
        float dt = (float)(now - last) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        last = now;
        fade = approach(fade, 1, dt, 5);
        SDL_SetRenderDrawColor(r, 6, 10, 22, 255);
        SDL_RenderClear(r);
        draw_background(r, dt);

        if (screen == SCREEN_MAIN) {
            /* header */
            text_ex(r, "STATIC RECOMPILATION", 64, 70, 13, alpha(C_ACCENT_HI, fade), 2.6f, 1);
            text_ex(r, "Star Fox: Assault", 62, 90, 58, alpha(C_TEXT, fade), -0.6f, 1);
            text(r, "Native for PC and Steam Deck  " BULLET "  GameCube, USA", 64, 162, 17, alpha(C_MUTED, fade));
            snprintf(tmp, sizeof tmp, "v%s", updater_version());
            text(r, tmp, VW - 64 - text_w(tmp, 14, 0), 70, 14, alpha(C_FAINT, fade));
            draw_update_banner(r, now);
            draw_pads(r, fade);
            if (updater_state() == UPD_DONE) { SDL_RenderPresent(r); SDL_Delay(600); updater_restart(); }

            for (int i = 0; i < ITEM_COUNT; i++) hov[i] = approach(hov[i], hover == i ? 1.0f : 0.0f, dt, 14);
            seg_pos = approach(seg_pos, g_fullscreen ? 1.0f : 0.0f, dt, 16);
            fps_pos = approach(fps_pos, g_fps60 ? 1.0f : 0.0f, dt, 16);

            /* focus ring (keyboard and controller): drawn just before the selected control, which
             * covers all but its edge; it breathes a little */
            float ring_k = 3.0f + 0.8f * sinf((float)now / 260.0f);
            #define FOCUS(item, rad)                 if (focus_visible && sel == (item)) rrect(r, grow(item_rect(&L, item), ring_k), (rad) + ring_k, alpha(C_RING, 0.95f))

            /* game disc card */
            rrect(r, L.disc_card, 16, mix(C_CARD, C_CARD_HI, hov[ITEM_DISC]));
            icon_disc(r, L.disc_card.x + 50, L.disc_card.y + 52, 26, disc_ok ? C_ACCENT : C_FAINT, C_CARD);
            float tx = L.disc_card.x + 96, tw = L.browse.x - tx - 16;
            text(r, "Game disc", tx, L.disc_card.y + 18, 14, C_MUTED);
            if (g_disc[0]) ellipsize(tmp, sizeof tmp, base_name(g_disc), 20, tw, 0);
            else snprintf(tmp, sizeof tmp, "No disc selected");
            text(r, tmp, tx, L.disc_card.y + 38, 20, g_disc[0] ? C_TEXT : C_MUTED);
            SDL_Color sc = msg_ok ? C_GREEN : C_RED;
            circle(r, tx + 4, L.disc_card.y + 79, 4, sc);
            ellipsize(tmp, sizeof tmp, msg, 14, tw - 14, 0);
            text(r, tmp, tx + 14, L.disc_card.y + 70, 14, sc);
            FOCUS(ITEM_DISC, 12);
            rrect(r, L.browse, 12, mix(C_TRACK, (SDL_Color){34, 48, 84, 255}, hov[ITEM_DISC]));
            text_vc(r, "Browse" ELLIPSIS, L.browse.x + (L.browse.w - text_w("Browse" ELLIPSIS, 16, 0)) / 2, L.browse.y,
                    L.browse.h, 16, C_TEXT, 0);

            /* display and frame-rate cards, each with a sliding switch */
            for (int row = 0; row < 2; row++) {
                int item = row ? ITEM_FPS : ITEM_DISPLAY;
                SDL_FRect card = row ? L.fps_card : L.display_card, seg = row ? L.fps_seg : L.seg;
                float pos = row ? fps_pos : seg_pos;
                rrect(r, card, 16, mix(C_CARD, C_CARD_HI, hov[item]));
                text_vc(r, row ? "Frame rate" : "Display", card.x + 24, card.y, card.h, 18, C_TEXT, 0);
                if (row) text_vc(r, "Experimental", card.x + 132, card.y, card.h, 13, C_FAINT, 0);
                FOCUS(item, 12);
                rrect(r, seg, 12, C_TRACK);
                SDL_FRect knob = {seg.x + 4 + pos * (seg.w / 2 - 4), seg.y + 4, seg.w / 2 - 4, seg.h - 8};
                rrect(r, knob, 9, C_ACCENT);
                const char *opt[2] = {row ? "Original" : "Window", row ? "60 fps" : "Fullscreen"};
                for (int k = 0; k < 2; k++) {
                    float cx = seg.x + seg.w / 4 + k * seg.w / 2, w = text_w(opt[k], 15, 0);
                    float on = k ? pos : 1 - pos;
                    text_vc(r, opt[k], cx - w / 2, seg.y, seg.h, 15, mix(C_MUTED, C_TEXT, on), on > 0.5f);
                }
            }

            /* play and quit */
            float pulse = disc_ok ? 0.5f + 0.5f * sinf((float)now / 520.0f) : 0;
            if (disc_ok) glow(r, L.play.x + L.play.w / 2, L.play.y + L.play.h / 2, 260, alpha(C_ACCENT, 0.18f + 0.10f * pulse));
            SDL_Color pc = disc_ok ? mix(C_ACCENT, C_ACCENT_HI, hov[ITEM_PLAY]) : (SDL_Color){30, 40, 66, 255};
            FOCUS(ITEM_PLAY, 16);
            rrect(r, L.play, 16, pc);
            float pw = text_w("Play", 26, 0.4f);
            text_ex(r, "Play", L.play.x + (L.play.w - pw) / 2, L.play.y + L.play.h / 2 - 26 * 0.56f, 26,
                    disc_ok ? C_TEXT : C_FAINT, 0.4f, 1);
            if (disc_ok) badge(r, "START", L.play.x + L.play.w - 74, L.play.y + L.play.h / 2, (SDL_Color){200, 222, 255, 200}, 0);
            FOCUS(ITEM_QUIT, 16);
            rrect(r, L.quit, 16, mix(C_CARD, C_CARD_HI, hov[ITEM_QUIT]));
            text_vc(r, "Quit", L.quit.x + (L.quit.w - text_w("Quit", 18, 0)) / 2, L.quit.y, L.quit.h, 18, C_MUTED, 0);

            static const char *const h[] = {"A", "Select", "B", "Back", "#START", "Play", "#F11", "Fullscreen", "Y", "Update"};
            hints(r, h, updater_state() == UPD_AVAILABLE || updater_state() == UPD_FAILED ? 10 : 8);
            #undef FOCUS
        } else {
            /* the built-in browser */
            SDL_FRect bk = browse_back();
            rrect(r, bk, 12, bhover == -2 ? C_CARD_HI : C_CARD);
            text_vc(r, LARROW " Back", bk.x + 16, bk.y, bk.h, 15, C_TEXT, 0);
            text_ex(r, "Choose your game disc", 64, 92, 30, C_TEXT, -0.2f, 1);
            ellipsize(tmp, sizeof tmp, g_cwd[0] ? g_cwd : "Places", 15, VW - 128, 1);
            text(r, tmp, 64, 134, 15, C_MUTED);
            SDL_FRect list = browse_list();
            rrect(r, list, 16, C_CARD);
            for (int k = 0; k < BROWSE_ROWS && scroll + k < g_nentries; k++) {
                int i = scroll + k;
                Entry *en = &g_entries[i];
                SDL_FRect row = {list.x + 8, list.y + 8 + k * ROW_H, list.w - 16 - 10, ROW_H - 4};
                if (i == bsel && (focus_visible || bhover == i)) rrect(r, row, 10, focus_visible ? (SDL_Color){38, 56, 100, 255} : C_CARD_HI);
                float iy = row.y + row.h / 2;
                SDL_Color tc = C_TEXT;
                const char *tag = "";
                switch (en->kind) {
                case K_DIR: icon_folder(r, row.x + 14, iy - 9, C_AMBER); tag = "Folder"; break;
                case K_PLACE: icon_place(r, row.x + 14, iy - 10, C_ACCENT_HI); break;
                case K_UP: icon_folder(r, row.x + 14, iy - 9, C_MUTED); tc = C_MUTED; break;
                case K_DISC: icon_disc(r, row.x + 25, iy, 11, C_ACCENT, C_CARD); tag = "Disc image"; break;
                case K_COMPRESSED: icon_disc(r, row.x + 25, iy, 11, C_FAINT, C_CARD); tc = C_MUTED; tag = "Compressed: convert to ISO"; break;
                default: break;
                }
                float tagw = tag[0] ? text_w(tag, 14, 0) : 0;
                ellipsize(tmp, sizeof tmp, en->name, 17, row.w - 60 - tagw - 24, 0);
                text_vc(r, tmp, row.x + 50, row.y, row.h, 17, tc, en->kind == K_DISC);
                if (tag[0]) text_vc(r, tag, row.x + row.w - tagw - 12, row.y, row.h, 14, C_FAINT, 0);
            }
            if (g_nentries > BROWSE_ROWS) { /* scroll bar */
                float track_h = BROWSE_ROWS * ROW_H, bar_h = track_h * BROWSE_ROWS / g_nentries;
                if (bar_h < 24) bar_h = 24;
                float by = list.y + 8 + (track_h - bar_h) * scroll / (float)(g_nentries - BROWSE_ROWS);
                rrect(r, (SDL_FRect){list.x + list.w - 12, by, 5, bar_h}, 2.5f, (SDL_Color){90, 110, 160, 255});
            }
            if (!msg_ok && msg[0]) {
                circle(r, 68, 538, 4, C_RED);
                ellipsize(tmp, sizeof tmp, msg, 14, VW - 140, 0);
                text(r, tmp, 78, 529, 14, C_RED);
            }
            static const char *const h[] = {"A", "Open", "B", "Up a folder", "#LB/RB", "Page", "#START", "Play"};
            hints(r, h, disc_ok ? 8 : 6);
        }

        if (dialog_open) { /* waiting for the system dialog */
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r, 2, 4, 10, 170);
            SDL_RenderFillRectF(r, &(SDL_FRect){0, 0, VW, VH});
            SDL_FRect box = {VW / 2 - 230, VH / 2 - 60, 460, 120};
            rrect(r, box, 18, C_CARD);
            const char *a = "Choose your disc image", *b = "in the file dialog that opened.";
            text_ex(r, a, box.x + (box.w - text_w(a, 22, 0)) / 2, box.y + 30, 22, C_TEXT, 0, 1);
            text(r, b, box.x + (box.w - text_w(b, 16, 0)) / 2, box.y + 66, 16, C_MUTED);
        }

        if (take_shot) { /* SFA_LAUNCHER_SCRIPT: the frame as drawn, before it's presented */
            take_shot = 0;
            int w = 0, h = 0;
            SDL_GetRendererOutputSize(r, &w, &h);
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
            char path[64];
            snprintf(path, sizeof path, "build/GF7E01/launcher_%02d.bmp", shot_n++);
            if (s && SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) SDL_SaveBMP(s, path);
            if (s) SDL_FreeSurface(s);
        }
        SDL_RenderPresent(r);
        if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) SDL_Delay(30);
    }
    #undef OPEN_PICKER
    if (g_dialog_thread) SDL_DetachThread(g_dialog_thread); /* quit while the dialog is open */
    save_settings();
    while (g_npads) pads_remove(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[0])));
    for (int i = 0; i < 2; i++) if (g_ftex[i]) SDL_DestroyTexture(g_ftex[i]);
    if (g_round) SDL_DestroyTexture(g_round);
    if (g_glow) SDL_DestroyTexture(g_glow);
    g_ftex[0] = g_ftex[1] = g_round = g_glow = NULL;
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    free(g_entries);
    g_entries = NULL;
    g_nentries = g_cap_entries = 0;
    if (result == 1) {
        snprintf(disc_out, disc_n, "%s", g_disc);
        *fullscreen_io = g_fullscreen;
    }
    return result;
}
