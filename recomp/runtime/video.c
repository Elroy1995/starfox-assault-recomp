/* video.c - the window and the OpenGL renderer, on a dedicated render thread.
 *
 * Guest threads hand over complete frames (vertices already in clip space, batches with their
 * render state, textures to upload). The render thread draws them into a 640x528 EFB-sized
 * framebuffer, then scales the XFB copy rectangle into the window. With --dump-frames it also
 * saves every presented frame to build/GF7E01/frames/frame_NNNNN.bmp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <SDL.h>
#include "gx.h"
#include "runtime.h"

/* ---- minimal OpenGL 3.3 core loader ------------------------------------------------------------ */
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef double GLdouble;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr, GLintptr;
#ifdef _WIN32
#define GLAPI __stdcall
#else
#define GLAPI
#endif
#define GL_FUNCS(X) \
    X(void, glClear, GLbitfield) X(void, glClearColor, GLfloat, GLfloat, GLfloat, GLfloat) \
    X(void, glClearDepth, GLdouble) X(void, glViewport, GLint, GLint, GLsizei, GLsizei) \
    X(void, glEnable, GLenum) X(void, glDisable, GLenum) X(void, glDepthFunc, GLenum) \
    X(void, glDepthMask, GLboolean) X(void, glColorMask, GLboolean, GLboolean, GLboolean, GLboolean) \
    X(void, glBlendFunc, GLenum, GLenum) X(void, glBlendEquation, GLenum) X(void, glCullFace, GLenum) \
    X(void, glScissor, GLint, GLint, GLsizei, GLsizei) X(void, glDrawArrays, GLenum, GLint, GLsizei) \
    X(void, glGenTextures, GLsizei, GLuint *) X(void, glBindTexture, GLenum, GLuint) \
    X(void, glTexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *) \
    X(void, glTexParameteri, GLenum, GLenum, GLint) X(void, glPixelStorei, GLenum, GLint) \
    X(void, glReadPixels, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) \
    X(void, glActiveTexture, GLenum) X(GLuint, glCreateShader, GLenum) \
    X(void, glShaderSource, GLuint, GLsizei, const GLchar *const *, const GLint *) \
    X(void, glCompileShader, GLuint) X(void, glGetShaderiv, GLuint, GLenum, GLint *) \
    X(void, glGetShaderInfoLog, GLuint, GLsizei, GLsizei *, GLchar *) X(GLuint, glCreateProgram, void) \
    X(void, glAttachShader, GLuint, GLuint) X(void, glLinkProgram, GLuint) X(void, glUseProgram, GLuint) \
    X(GLint, glGetUniformLocation, GLuint, const GLchar *) X(void, glUniform1i, GLint, GLint) \
    X(void, glUniform1f, GLint, GLfloat) X(void, glGenBuffers, GLsizei, GLuint *) \
    X(void, glUniform4fv, GLint, GLsizei, const GLfloat *) X(void, glUniform2f, GLint, GLfloat, GLfloat) \
    X(void, glBindBuffer, GLenum, GLuint) X(void, glBufferData, GLenum, GLsizeiptr, const void *, GLenum) \
    X(void, glGenVertexArrays, GLsizei, GLuint *) X(void, glBindVertexArray, GLuint) \
    X(void, glEnableVertexAttribArray, GLuint) \
    X(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void *) \
    X(void, glGenFramebuffers, GLsizei, GLuint *) X(void, glBindFramebuffer, GLenum, GLuint) \
    X(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint) \
    X(void, glGenRenderbuffers, GLsizei, GLuint *) X(void, glBindRenderbuffer, GLenum, GLuint) \
    X(void, glRenderbufferStorage, GLenum, GLenum, GLsizei, GLsizei) \
    X(void, glFramebufferRenderbuffer, GLenum, GLenum, GLenum, GLuint) \
    X(void, glBlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)
#define GL_DECLARE(ret, name, ...) static ret(GLAPI *name)(__VA_ARGS__);
GL_FUNCS(GL_DECLARE)
#define GL_LOAD(ret, name, ...) \
    if (!(*(void **)&name = SDL_GL_GetProcAddress(#name))) rt_fatal("OpenGL function %s missing", #name);

#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_TRIANGLES 0x0004
#define GL_LINES 0x0001
#define GL_POINTS 0x0000
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_RENDERBUFFER 0x8D41
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_REVERSE_SUBTRACT 0x800B

/* GX enums -> GL */
static const GLenum k_depth_func[8] = {0x0200, 0x0201, 0x0202, 0x0203, 0x0204, 0x0205, 0x0206, 0x0207};
static const GLenum k_blend_factor[8] = {0 /*ZERO*/, 1 /*ONE*/, 0x0306 /*DST_COLOR / SRC_COLOR*/,
                                         0x0307 /*1-DST_COLOR*/, 0x0302 /*SRC_ALPHA*/, 0x0303 /*1-SRC_ALPHA*/,
                                         0x0304 /*DST_ALPHA*/, 0x0305 /*1-DST_ALPHA*/};

/* ---- input ----------------------------------------------------------------------------------------
 * Up to four game controllers, one per GameCube port: each controller takes the lowest free port when
 * it's connected and keeps it until it's unplugged (so players don't swap mid-match), and its player
 * light, where it has one, shows that number. Port 1 also takes the keyboard: arrows/WASD = stick,
 * Enter = START, Space/J = A, K/Backspace = B, L = X, I = Y, Q/E = L/R, Z = Z, C-stick on the numpad
 * (8/4/6/2). Ports without a controller read as unplugged, as on the console. */
#define NPADS 4
static SDL_mutex *g_pad_lock;
static PadState g_kbd, g_ctl[NPADS];
static SDL_GameController *g_controllers[NPADS];
static int g_auto_press;

void video_set_auto_press(int on) { g_auto_press = on; }
static volatile int g_auto_play; /* a stage is running: auto-press plays instead of tapping START */
void video_set_auto_play(int on) { g_auto_play = on; }

/* --auto-keys=TOKENS: comma-separated buttons pressed one per step (1.5 s) from launch, for reaching
 * menus auto-press doesn't (VS mode, bonus games). Tokens: a b x y z l r start up down left right,
 * and w (wait one step). After the last step, --auto-press behaviour (if on) takes over. */
#define AUTO_KEY_STEP_MS 1500
static uint16_t g_auto_keys[256];
static int g_auto_nkeys;
void video_set_auto_keys(const char *list) {
    static const struct { const char *name; uint16_t bits; } k[] = {
        {"a", 0x100}, {"b", 0x200}, {"x", 0x400}, {"y", 0x800}, {"start", 0x1000}, {"z", 0x10}, {"r", 0x20},
        {"l", 0x40}, {"left", 0x1}, {"right", 0x2}, {"down", 0x4}, {"up", 0x8}, {"w", 0}};
    g_auto_nkeys = 0;
    while (*list && g_auto_nkeys < 256) {
        size_t n = strcspn(list, ",");
        uint16_t bits = 0xFFFF;
        for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
            if (strlen(k[i].name) == n && !strncmp(k[i].name, list, n)) bits = k[i].bits;
        if (bits == 0xFFFF) fprintf(stderr, "[video] --auto-keys: unknown key '%.*s'\n", (int)n, list);
        else g_auto_keys[g_auto_nkeys++] = bits;
        list += n;
        if (*list == ',') list++;
    }
}

static int8_t axis8(Sint16 v) {
    int x = v / 256;
    if (x > -20 && x < 20) x = 0; /* dead zone */
    return (int8_t)(x > 127 ? 127 : x < -127 ? -127 : x);
}

static void poll_controller(int slot) {
    SDL_GameController *gc = g_controllers[slot];
    PadState p = {0};
    if (gc) {
        static const struct { SDL_GameControllerButton b; uint16_t bit; } map[] = {
            {SDL_CONTROLLER_BUTTON_A, 0x100}, {SDL_CONTROLLER_BUTTON_B, 0x200}, {SDL_CONTROLLER_BUTTON_X, 0x400},
            {SDL_CONTROLLER_BUTTON_Y, 0x800}, {SDL_CONTROLLER_BUTTON_START, 0x1000},
            {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x10}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x1},
            {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x2}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x4}, {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x8},
        };
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (SDL_GameControllerGetButton(gc, map[i].b)) p.buttons |= map[i].bit;
        p.stick_x = axis8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
        p.stick_y = (int8_t)-axis8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
        p.substick_x = axis8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX));
        p.substick_y = (int8_t)-axis8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));
        p.trigger_l = (uint8_t)(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 128);
        p.trigger_r = (uint8_t)(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 128);
        if (p.trigger_l > 200) p.buttons |= 0x40; /* digital L/R click at the end of the travel */
        if (p.trigger_r > 200) p.buttons |= 0x20;
        p.connected = 1;
    }
    SDL_LockMutex(g_pad_lock);
    g_ctl[slot] = p;
    SDL_UnlockMutex(g_pad_lock);
}

/* A controller was connected: give it the lowest free port. */
static void add_controller(int device_index) {
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
    for (int i = 0; i < NPADS; i++) /* already ours (it was open before the game started) */
        if (g_controllers[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_controllers[i])) == id) return;
    for (int i = 0; i < NPADS; i++)
        if (!g_controllers[i]) {
            g_controllers[i] = SDL_GameControllerOpen(device_index);
            if (!g_controllers[i]) return;
            SDL_GameControllerSetPlayerIndex(g_controllers[i], i); /* the controller's player light, if any */
            printf("[video] controller: %s -> player %d\n", SDL_GameControllerName(g_controllers[i]), i + 1);
            fflush(stdout);
            return;
        }
    printf("[video] controller ignored (4 already connected): %s\n", SDL_GameControllerNameForIndex(device_index));
    fflush(stdout);
}

static void remove_controller(SDL_JoystickID id) {
    for (int i = 0; i < NPADS; i++)
        if (g_controllers[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_controllers[i])) == id) {
            printf("[video] player %d's controller disconnected\n", i + 1);
            fflush(stdout);
            SDL_GameControllerClose(g_controllers[i]);
            g_controllers[i] = NULL;
            SDL_LockMutex(g_pad_lock);
            memset(&g_ctl[i], 0, sizeof g_ctl[i]);
            SDL_UnlockMutex(g_pad_lock);
        }
}

static void poll_keyboard(void) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    PadState p = {0};
    p.connected = 1;
    if (k[SDL_SCANCODE_RETURN]) p.buttons |= 0x1000;
    if (k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_J]) p.buttons |= 0x100;
    if (k[SDL_SCANCODE_K] || k[SDL_SCANCODE_BACKSPACE]) p.buttons |= 0x200;
    if (k[SDL_SCANCODE_L]) p.buttons |= 0x400;
    if (k[SDL_SCANCODE_I]) p.buttons |= 0x800;
    if (k[SDL_SCANCODE_Z]) p.buttons |= 0x10;
    if (k[SDL_SCANCODE_Q]) { p.buttons |= 0x40; p.trigger_l = 255; }
    if (k[SDL_SCANCODE_E]) { p.buttons |= 0x20; p.trigger_r = 255; }
    if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]) p.stick_x = -100;
    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) p.stick_x = 100;
    if (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W]) p.stick_y = 100;
    if (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S]) p.stick_y = -100;
    if (k[SDL_SCANCODE_KP_4]) p.substick_x = -100;
    if (k[SDL_SCANCODE_KP_6]) p.substick_x = 100;
    if (k[SDL_SCANCODE_KP_8]) p.substick_y = 100;
    if (k[SDL_SCANCODE_KP_2]) p.substick_y = -100;
    SDL_LockMutex(g_pad_lock);
    g_kbd = p;
    SDL_UnlockMutex(g_pad_lock);
}

static void get_pad(int port, PadState *out) {
    memset(out, 0, sizeof *out);
    if (port < 0 || port >= NPADS) return;
    if (port != 0) { /* ports 2-4: their controller, if one is connected */
        if (g_pad_lock) {
            SDL_LockMutex(g_pad_lock);
            *out = g_ctl[port];
            SDL_UnlockMutex(g_pad_lock);
        }
        /* SFA_FAKE_PADS=N[:S] (testing): ports 2..N act as connected controllers; in menus they tap A
         * every 3 s, from S seconds after launch (default 0) */
        static int fake = -1;
        static Uint32 fake_from;
        if (fake < 0) {
            const char *e = getenv("SFA_FAKE_PADS");
            fake = e ? atoi(e) : 0;
            const char *colon = e ? strchr(e, ':') : NULL;
            fake_from = colon ? (Uint32)(atof(colon + 1) * 1000) : 0;
        }
        if (!out->connected && bots_pad(port, out, SDL_GetTicks())) return; /* a bot plays this port */
        if (!out->connected && port < fake) {
            Uint32 t = SDL_GetTicks() + (Uint32)port * 700;
            out->connected = 1;
            static int walk = -1; /* SFA_FAKE_PAD_WALK=1 (testing): in a match, port 2 only walks 3 s forward, 3 s back */
            if (walk < 0) walk = getenv("SFA_FAKE_PAD_WALK") ? atoi(getenv("SFA_FAKE_PAD_WALK")) : 0;
            if (SDL_GetTicks() < fake_from) {
            } else if (g_auto_play && walk == 2 && port == 1) { /* controls test: 3 s each */
                switch ((SDL_GetTicks() / 3000) % 6) {
                case 0: out->stick_x = 100; break;
                case 1: out->substick_x = 100; break;
                case 2: out->stick_y = 100; break;
                case 3: out->buttons |= 0x20; out->trigger_r = 255; break; /* R */
                case 4: out->buttons |= 0x40; out->trigger_l = 255; break; /* L */
                default: out->substick_y = 100; break;
                }
            } else if (g_auto_play && walk == 1) {
                if (port == 1) out->stick_y = (SDL_GetTicks() / 3000) % 2 ? -100 : 100;
            } else if (g_auto_play) { /* in a match: move, turn and fire */
                if (t % 500 < 120) out->buttons |= 0x100;
                out->stick_y = 60;
                int phase = (int)(t % 8000);
                out->stick_x = (int8_t)((phase < 4000 ? phase : 8000 - phase) * 100 / 4000 - 50);
            } else if (t % 3000 < 150) out->buttons |= 0x100; /* menus: tap A (joins, confirms) */
        }
        return;
    }
    out->connected = 1;
    if (g_pad_lock) {
        SDL_LockMutex(g_pad_lock);
        PadState a = g_kbd, b = g_ctl[0];
        SDL_UnlockMutex(g_pad_lock);
        out->buttons = a.buttons | b.buttons;
        out->stick_x = b.stick_x ? b.stick_x : a.stick_x;
        out->stick_y = b.stick_y ? b.stick_y : a.stick_y;
        out->substick_x = b.substick_x ? b.substick_x : a.substick_x;
        out->substick_y = b.substick_y ? b.substick_y : a.substick_y;
        out->trigger_l = b.trigger_l > a.trigger_l ? b.trigger_l : a.trigger_l;
        out->trigger_r = b.trigger_r > a.trigger_r ? b.trigger_r : a.trigger_r;
    }
    if (g_auto_nkeys) { /* scripted keys first: press each for 150 ms at the start of its step */
        Uint32 t = SDL_GetTicks();
        int step = (int)(t / AUTO_KEY_STEP_MS);
        if (step < g_auto_nkeys) {
            uint16_t k = g_auto_keys[step];
            if (t % AUTO_KEY_STEP_MS < ((k & 0xF) ? 450u : 200u)) { /* menus need a direction held longer */
                out->buttons |= k;
                /* directions also move the main stick: menus read either */
                if (k & 0x8) out->stick_y = 100;
                if (k & 0x4) out->stick_y = -100;
                if (k & 0x1) out->stick_x = -100;
                if (k & 0x2) out->stick_x = 100;
            }
            return;
        }
    }
    static int charge = -1; /* SFA_AUTO_CHARGE=1: charged lock-on shots instead (they home in on bosses) */
    if (charge < 0) charge = getenv("SFA_AUTO_CHARGE") != NULL;
    if (g_auto_press && g_auto_play && charge) {
        Uint32 t = SDL_GetTicks();
        if (t % 2000 < 1600) out->buttons |= 0x100; /* hold A to charge, release to fire */
        int phase = (int)(t % 6000);                /* a gentle sweep, so the aim stays near the target */
        out->stick_x = (int8_t)((phase < 3000 ? phase : 6000 - phase) * 40 / 3000 - 20);
    } else if (g_auto_press && g_auto_play) { /* in a stage: fire and move, never pause */
        Uint32 t = SDL_GetTicks();
        if (t % 500 < 120) out->buttons |= 0x100; /* A: fire, and advances dialogue */
        out->stick_y = 60;                        /* forward */
        int phase = (int)(t % 8000);              /* sweep left and right every 8 s */
        out->stick_x = (int8_t)((phase < 4000 ? phase : 8000 - phase) * 100 / 4000 - 50);
    } else if (g_auto_press) { /* menus: tap START, then A, every 4 seconds */
        Uint32 t = SDL_GetTicks() % 4000;
        if (t < 150) out->buttons |= 0x1000;
        else if (t >= 2000 && t < 2150) out->buttons |= 0x100;
    }
}

/* What PADRead gets for a port. Real input (port 1, which also has the keyboard, and ports with a
 * controller) goes through bots.c as well, which watches for Z on the character screen (adding bots)
 * and takes the input while its player chooses a bot's character. */
void video_get_pad(int port, PadState *out) {
    get_pad(port, out);
    int real = port == 0;
    if (port > 0 && port < NPADS && g_pad_lock) {
        SDL_LockMutex(g_pad_lock);
        real = g_ctl[port].connected;
        SDL_UnlockMutex(g_pad_lock);
    }
    if (real) bots_human_input(port, out, SDL_GetTicks());
}

/* ---- audio output -------------------------------------------------------------------------------- */
static SDL_AudioDeviceID g_audio;

/* 32 kHz interleaved stereo from the AI DMA (host-endian samples). */
#define AUDIO_RING 16000   /* frames: 0.5 s at 32 kHz */
#define AUDIO_TARGET 3200  /* frames buffered ahead of the device: 100 ms */
static int16_t g_ring[AUDIO_RING][2];
static int g_ring_read, g_ring_write, g_ring_fill, g_audio_waiting = 1;
static double g_audio_frac;
static int16_t g_audio_last[2];
static unsigned g_audio_underruns, g_audio_drops;
static SDL_SpinLock g_ring_lock;

void video_audio_push(const int16_t *pcm, int frames) {
    static FILE *wav;
    static uint32_t wav_frames;
    if (getenv("SFA_DUMP_AUDIO") && frames > 0) { /* debugging: everything played, as a WAV file */
        if (!wav) {
            wav = fopen("build/GF7E01/audio.wav", "wb");
            static const uint8_t hdr[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
                0x00,0x7D,0,0, 0x00,0xF4,0x01,0, 4,0,16,0,'d','a','t','a',0,0,0,0};
            if (wav) fwrite(hdr, 1, 44, wav);
        }
        if (wav) {
            fwrite(pcm, 4, (size_t)frames, wav);
            wav_frames += (uint32_t)frames;
            uint32_t data = wav_frames * 4, riff = 36 + data;
            long pos = ftell(wav);
            fseek(wav, 4, SEEK_SET); fwrite(&riff, 4, 1, wav);
            fseek(wav, 40, SEEK_SET); fwrite(&data, 4, 1, wav);
            fseek(wav, pos, SEEK_SET);
            fflush(wav);
        }
    }
    if (!g_audio || frames <= 0) return;
    SDL_AtomicLock(&g_ring_lock);
    for (int i = 0; i < frames; i++) {
        if (g_ring_fill == AUDIO_RING) { /* far too much buffered (the device stalled): drop the oldest */
            g_ring_read = (g_ring_read + 1) % AUDIO_RING;
            g_ring_fill--;
            g_audio_drops++;
        }
        g_ring[g_ring_write][0] = pcm[2 * i];
        g_ring[g_ring_write][1] = pcm[2 * i + 1];
        g_ring_write = (g_ring_write + 1) % AUDIO_RING;
        g_ring_fill++;
    }
    SDL_AtomicUnlock(&g_ring_lock);
}

/* The audio device pulls from the ring at a fixed latency target. The game's sample clock and the
 * sound card drift apart slightly, so instead of dropping or repeating samples (audible clicks) the
 * read rate is nudged by up to 0.5% toward the target fill, with linear interpolation. After running
 * dry it fades out and waits until the target is buffered again before resuming. */
static int g_audio_muted; /* SFA_MUTE=1 (testing): the audio device plays silence */
static void audio_callback(void *user, Uint8 *stream, int len) {
    (void)user;
    int16_t *out = (int16_t *)stream;
    int n = len / 4;
    SDL_AtomicLock(&g_ring_lock);
    if (g_audio_waiting && g_ring_fill >= AUDIO_TARGET) g_audio_waiting = 0;
    if (g_ring_fill > AUDIO_TARGET * 4) { /* way behind (e.g. after a long stall): skip to the target */
        int skip = g_ring_fill - AUDIO_TARGET;
        g_ring_read = (g_ring_read + skip) % AUDIO_RING;
        g_ring_fill -= skip;
        g_audio_drops += (unsigned)skip;
    }
    double err = (double)(g_ring_fill - AUDIO_TARGET) / AUDIO_TARGET;
    if (err > 1) err = 1;
    if (err < -1) err = -1;
    double step = 1.0 + 0.005 * err;
    for (int i = 0; i < n; i++) {
        if (g_audio_waiting || g_ring_fill < 2) {
            if (!g_audio_waiting) { g_audio_waiting = 1; g_audio_underruns++; }
            /* fade the last sample out instead of jumping to silence */
            g_audio_last[0] = (int16_t)(g_audio_last[0] * 15 / 16);
            g_audio_last[1] = (int16_t)(g_audio_last[1] * 15 / 16);
            out[2 * i] = g_audio_last[0];
            out[2 * i + 1] = g_audio_last[1];
            continue;
        }
        int a = g_ring_read, b = (g_ring_read + 1) % AUDIO_RING;
        double f = g_audio_frac;
        for (int c = 0; c < 2; c++) {
            double v = g_ring[a][c] + (g_ring[b][c] - g_ring[a][c]) * f;
            out[2 * i + c] = g_audio_last[c] = (int16_t)v;
        }
        g_audio_frac += step;
        while (g_audio_frac >= 1.0 && g_ring_fill >= 2) {
            g_audio_frac -= 1.0;
            g_ring_read = (g_ring_read + 1) % AUDIO_RING;
            g_ring_fill--;
        }
    }
    SDL_AtomicUnlock(&g_ring_lock);
    if (g_audio_muted) memset(stream, 0, (size_t)len); /* SFA_MUTE: everything as usual, but silent */
}

/* ---- frame queue --------------------------------------------------------------------------------- */
static SDL_mutex *g_qlock;
static SDL_cond *g_qcond;
static GxFrame *g_pending_frame;
static volatile int g_quit;
static int g_dump;
static int g_last_rect[4] = {0, 0, 640, 480}; /* the last frame's XFB copy rectangle, to show it again */

int video_quit_requested(void) { return g_quit; }

/* Finished frames go back to a small pool with their arrays, so building the next frame reuses
 * memory that is already allocated and paged in (a Mission 1 frame holds ~150k vertices, ~17 MB). */
#define FRAME_POOL 4
static GxFrame *g_frame_pool[FRAME_POOL];
static int g_frame_pool_n;
static SDL_SpinLock g_frame_pool_lock;

static void free_frame(GxFrame *f) {
    for (uint32_t i = 0; i < f->nuploads; i++) free(f->uploads[i].rgba);
    f->nuploads = 0;
    SDL_AtomicLock(&g_frame_pool_lock);
    if (g_frame_pool_n < FRAME_POOL) {
        g_frame_pool[g_frame_pool_n++] = f;
        f = NULL;
    }
    SDL_AtomicUnlock(&g_frame_pool_lock);
    if (!f) return;
    free(f->uploads);
    free(f->verts);
    free(f->batches);
    free(f->clears);
    free(f->efb_copies);
    free(f);
}

volatile uint32_t g_video_frame_number; /* the frame being built, for logs */
GxFrame *video_new_frame(uint32_t number) {
    g_video_frame_number = number;
    GxFrame *f = NULL;
    SDL_AtomicLock(&g_frame_pool_lock);
    if (g_frame_pool_n) f = g_frame_pool[--g_frame_pool_n];
    SDL_AtomicUnlock(&g_frame_pool_lock);
    if (!f) f = calloc(1, sizeof *f);
    f->nverts = f->nbatches = f->nuploads = f->nclears = f->nefb_copies = f->nevents = 0;
    memset(f->copy_rect, 0, sizeof f->copy_rect);
    f->copy = NULL;
    f->number = number;
    return f;
}

void video_submit(GxFrame *frame) {
    if (!g_qlock) { /* --no-video: nothing renders, drop the frame */
        free_frame(frame);
        return;
    }
    SDL_LockMutex(g_qlock);
    if (g_pending_frame) { /* renderer is behind: keep its uploads, drop the old frame's draws */
        GxFrame *old = g_pending_frame;
        for (uint32_t i = 0; i < old->nuploads; i++) {
            if (frame->nuploads == frame->cap_uploads) {
                frame->cap_uploads = frame->cap_uploads ? frame->cap_uploads * 2 : 64;
                frame->uploads = realloc(frame->uploads, sizeof *frame->uploads * frame->cap_uploads);
            }
            frame->uploads[frame->nuploads++] = old->uploads[i];
        }
        old->nuploads = 0;
        free_frame(old);
    }
    g_pending_frame = frame;
    SDL_CondSignal(g_qcond);
    SDL_UnlockMutex(g_qlock);
}

/* A mid-frame segment ending in an EFB copy to texture. Unlike whole frames it is never dropped
 * (later draws build on this EFB content), and the caller waits for the read-back pixels. */
static SDL_cond *g_done_cond;

void video_copy_segment(GxFrame *frame) {
    GxCopyRequest *req = frame->copy;
    if (!g_qlock) { /* --no-video: no EFB to read */
        memset(req->rgba, 0, (size_t)req->w * req->h * 4);
        req->done = 1;
        free_frame(frame);
        return;
    }
    SDL_LockMutex(g_qlock);
    while (g_pending_frame) SDL_CondWait(g_done_cond, g_qlock); /* let the renderer take what is queued */
    g_pending_frame = frame;
    SDL_CondSignal(g_qcond);
    while (!req->done && !g_quit) SDL_CondWaitTimeout(g_done_cond, g_qlock, 100);
    SDL_UnlockMutex(g_qlock);
}

/* ---- renderer --------------------------------------------------------------------------------- */
static SDL_Window *g_window;
extern uint64_t g_t_render; /* gx.c, SFA_PROFILE */
static int g_prof_render;
static unsigned g_shader_compiles; /* TEV programs built for the game (see the shader cache) */
static int g_fullscreen, g_window_w = 1280, g_window_h = 960;
void video_set_window(int fullscreen, int w, int h) {
    g_fullscreen = fullscreen;
    if (w > 0 && h > 0) { g_window_w = w; g_window_h = h; }
}
static GLuint g_vao, g_vbo, g_efb_fbo, g_efb_color, g_efb_depth, g_white;
static GLuint *g_textures; /* GxTexture id -> GL name */
static uint32_t *g_texture_dims; /* GxTexture id -> width << 16 | height */
static GLuint g_copy_prog, g_copy_fbo;
static GLint g_copy_u_src, g_copy_u_mode, g_copy_u_half, g_copy_u_intensity;

/* EFB copy to texture: one fullscreen triangle over the destination texture. Each output texel is
 * what the GX texture the copy format stores would decode to (see encode_copy/decode_texture in
 * gx.c): intensity formats replicate I into RGBA, IA formats give (I, I, I, A), and so on, with the
 * same 4/5/6-bit rounding. Texture row 0 is the top of the copied rectangle; EFB row 0 (GL) is the
 * bottom. */
static const char *k_copy_vs =
    "#version 330 core\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID & 1) * 4.0 - 1.0, (gl_VertexID & 2) * 2.0 - 1.0);\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";
static const char *k_copy_fs =
    "#version 330 core\n"
    "uniform sampler2D u_efb;\n"
    "uniform vec2 u_src;\n"
    "uniform int u_mode, u_half, u_intensity;\n"
    "out vec4 o;\n"
    "vec4 px(int x, int y) { return texelFetch(u_efb, ivec2(x, 527 - y), 0); }\n"
    "float q(float v, float bits) { float n = exp2(bits) - 1.0; return floor(floor(v * 255.0 + 0.5) / exp2(8.0 - bits)) / n; }\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    int sx = int(u_src.x), sy = int(u_src.y);\n"
    "    vec4 c;\n"
    "    if (u_half != 0) {\n"
    "        int x = sx + p.x * 2, y = sy + p.y * 2;\n"
    "        c = (px(x, y) + px(x + 1, y) + px(x, y + 1) + px(x + 1, y + 1)) * 0.25;\n"
    "    } else c = px(sx + p.x, sy + p.y);\n"
    "    float I = u_intensity != 0 ? clamp(dot(c.rgb, vec3(66.0, 129.0, 25.0) / 256.0) + 16.0 / 255.0, 0.0, 1.0) : c.r;\n"
    "    if (u_mode == 0) o = vec4(q(I, 4.0));\n"
    "    else if (u_mode == 1 || u_mode == 8) o = vec4(I);\n"
    "    else if (u_mode == 2) o = vec4(vec3(q(I, 4.0)), q(c.a, 4.0));\n"
    "    else if (u_mode == 3) o = vec4(vec3(I), c.a);\n"
    "    else if (u_mode == 4) o = vec4(q(c.r, 5.0), q(c.g, 6.0), q(c.b, 5.0), 1.0);\n"
    "    else if (u_mode == 5) o = c.a >= 224.0 / 255.0 ? vec4(q(c.r, 5.0), q(c.g, 5.0), q(c.b, 5.0), 1.0)\n"
    "                                                   : vec4(q(c.r, 4.0), q(c.g, 4.0), q(c.b, 4.0), q(c.a, 3.0));\n"
    "    else if (u_mode == 7) o = vec4(c.a);\n"
    "    else if (u_mode == 9) o = vec4(c.g);\n"
    "    else if (u_mode == 10) o = vec4(c.b);\n"
    "    else if (u_mode == 11) o = vec4(vec3(c.r), c.g);\n"
    "    else if (u_mode == 12) o = vec4(vec3(c.g), c.b);\n"
    "    else o = c;\n"
    "}\n";
static uint32_t g_ntextures;

/* ---- TEV: one generated fragment shader per distinct GxTevConfig ------------------------------- */
static const char *k_vs =
    "#version 330 core\n"
    "layout(location=0) in vec4 pos; layout(location=1) in vec4 c0; layout(location=2) in vec4 c1;\n"
    "layout(location=3) in vec2 t0; layout(location=4) in vec2 t1; layout(location=5) in vec2 t2;\n"
    "layout(location=6) in vec2 t3; layout(location=7) in vec2 t4; layout(location=8) in vec2 t5;\n"
    "layout(location=9) in vec2 t6; layout(location=10) in vec2 t7;\n"
    "out vec4 v_c0, v_c1; out vec2 v_t0, v_t1, v_t2, v_t3, v_t4, v_t5, v_t6, v_t7;\n"
    "void main() { gl_Position = pos; v_c0 = c0; v_c1 = c1; v_t0 = t0; v_t1 = t1; v_t2 = t2; v_t3 = t3;\n"
    "  v_t4 = t4; v_t5 = t5; v_t6 = t6; v_t7 = t7; }\n";

typedef struct {
    GxTevConfig cfg;
    GLuint prog;
    GLint u_konst, u_reg, u_aref;
} TevProgram;
static TevProgram *g_progs;
static unsigned g_nprogs, g_cap_progs;

typedef struct { char *p; size_t n, cap; } Str;
static void sadd(Str *s, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(s->p + s->n, s->cap - s->n, fmt, ap);
        va_end(ap);
        if (k >= 0 && (size_t)k < s->cap - s->n) { s->n += (size_t)k; return; }
        s->cap = s->cap ? s->cap * 2 : 8192;
        s->p = realloc(s->p, s->cap);
    }
}

static const char *k_color_in[16] = {"prev.rgb", "prev.aaa", "r0.rgb", "r0.aaa", "r1.rgb", "r1.aaa", "r2.rgb", "r2.aaa",
                                     "tx.rgb", "tx.aaa", "ras.rgb", "ras.aaa", "vec3(1.0)", "vec3(0.5)", "kc", "vec3(0.0)"};
static const char *k_alpha_in[8] = {"prev.a", "r0.a", "r1.a", "r2.a", "tx.a", "ras.a", "ka", "0.0"};
static const char *k_dest[4] = {"prev", "r0", "r1", "r2"};
static const char *k_konst_frac[8] = {"1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125"};

static void konst_color(Str *s, int sel) {
    if (sel < 8) sadd(s, "vec3(%s)", k_konst_frac[sel]);
    else if (sel < 12) sadd(s, "vec3(0.0)");
    else if (sel < 16) sadd(s, "u_konst[%d].rgb", sel - 12);
    else sadd(s, "u_konst[%d].%c%c%c", sel & 3, "rgba"[(sel - 16) / 4], "rgba"[(sel - 16) / 4], "rgba"[(sel - 16) / 4]);
}
static void konst_alpha(Str *s, int sel) {
    if (sel < 8) sadd(s, "%s", k_konst_frac[sel]);
    else if (sel < 16) sadd(s, "0.0");
    else sadd(s, "u_konst[%d].%c", sel & 3, "rgba"[(sel - 16) / 4]);
}
static void swizzle(char out[5], const GxTevConfig *t, int table) {
    uint32_t a = t->ksel[table * 2], b = t->ksel[table * 2 + 1];
    out[0] = "rgba"[a & 3]; out[1] = "rgba"[(a >> 2) & 3]; out[2] = "rgba"[b & 3]; out[3] = "rgba"[(b >> 2) & 3]; out[4] = 0;
}
static const char *alpha_cmp(int f) {
    static const char *k[8] = {"false", "(A < R)", "(A == R)", "(A <= R)", "(A > R)", "(A != R)", "(A >= R)", "true"};
    return k[f & 7];
}

static char *tev_fragment_source(const GxTevConfig *t) {
    Str s = {0};
    sadd(&s, "#version 330 core\n"
             "in vec4 v_c0, v_c1; in vec2 v_t0, v_t1, v_t2, v_t3, v_t4, v_t5, v_t6, v_t7; out vec4 o;\n"
             "uniform sampler2D tex0, tex1, tex2, tex3, tex4, tex5, tex6, tex7;\n"
             "uniform vec4 u_konst[4]; uniform vec4 u_reg[4]; uniform vec2 u_aref;\n"
             "float q8(float x) { return floor(clamp(x, 0.0, 1.0) * 255.0 + 0.5) / 255.0; }\n"
             "vec3 q8(vec3 x) { return floor(clamp(x, 0.0, 1.0) * 255.0 + 0.5) / 255.0; }\n"
             "void main() {\n"
             "  vec4 prev = u_reg[0], r0 = u_reg[1], r1 = u_reg[2], r2 = u_reg[3];\n"
             "  vec4 tx = vec4(1.0), ras = vec4(0.0); vec3 kc; float ka;\n");
    static const float scale[4] = {1.0f, 2.0f, 4.0f, 0.5f};
    static const char *bias[3] = {"0.0", "0.5", "-0.5"};
    int last_cdest = 0, last_adest = 0;
    for (int i = 0; i < t->nstages && i < 16; i++) {
        uint32_t tr = (t->tref[i / 2] >> ((i & 1) * 12)) & 0xFFF, ce = t->color_env[i], ae = t->alpha_env[i];
        uint32_t ks = t->ksel[i / 2];
        int map = tr & 7, tc = (tr >> 3) & 7, ten = (tr >> 6) & 1, chan = (tr >> 7) & 7;
        int kcs = (i & 1) ? (ks >> 14) & 31 : (ks >> 4) & 31, kas = (i & 1) ? (ks >> 19) & 31 : (ks >> 9) & 31;
        char rsw[5], tsw[5];
        swizzle(rsw, t, ae & 3);
        swizzle(tsw, t, (ae >> 2) & 3);
        sadd(&s, "  // stage %d\n", i);
        if (ten) sadd(&s, "  tx = texture(tex%d, v_t%d).%s;\n", map, tc, tsw);
        else sadd(&s, "  tx = vec4(1.0);\n");
        sadd(&s, "  ras = %s;\n", chan == 0 ? "v_c0" : chan == 1 ? "v_c1" : "vec4(0.0)");
        if (chan <= 1) sadd(&s, "  ras = ras.%s;\n", rsw);
        sadd(&s, "  kc = "); konst_color(&s, kcs); sadd(&s, "; ka = "); konst_alpha(&s, kas); sadd(&s, ";\n");

        /* color */
        int cd = ce & 15, cc = (ce >> 4) & 15, cb = (ce >> 8) & 15, ca = (ce >> 12) & 15;
        int cbias = (ce >> 16) & 3, csub = (ce >> 18) & 1, cclamp = (ce >> 19) & 1, cscale = (ce >> 20) & 3, cdest = (ce >> 22) & 3;
        if (cbias != 3) {
            sadd(&s, "  vec3 cr%d = (%s %c mix(q8(%s), q8(%s), q8(%s)) + vec3(%s)) * %.3f;\n", i, k_color_in[cd],
                 csub ? '-' : '+', k_color_in[ca], k_color_in[cb], k_color_in[cc], bias[cbias], scale[cscale]);
        } else { /* compare mode: d + (a OP b ? c : 0) */
            int op = (cscale << 1) | csub;
            const char *A = k_color_in[ca], *B = k_color_in[cb];
            switch (op) {
            case 0: case 1: sadd(&s, "  bool cm%d = q8(%s).r %s q8(%s).r;\n", i, A, op & 1 ? "==" : ">", B); break;
            case 2: case 3: sadd(&s, "  bool cm%d = dot(q8(%s).rg, vec2(1.0, 256.0)) %s dot(q8(%s).rg, vec2(1.0, 256.0));\n", i, A, op & 1 ? "==" : ">", B); break;
            case 4: case 5: sadd(&s, "  bool cm%d = dot(q8(%s), vec3(1.0, 256.0, 65536.0)) %s dot(q8(%s), vec3(1.0, 256.0, 65536.0));\n", i, A, op & 1 ? "==" : ">", B); break;
            default: break;
            }
            if (op >= 6)
                sadd(&s, "  vec3 cr%d = %s + mix(vec3(0.0), %s, vec3(%s(q8(%s), q8(%s))));\n", i, k_color_in[cd],
                     k_color_in[cc], op & 1 ? "equal" : "greaterThan", A, B);
            else
                sadd(&s, "  vec3 cr%d = %s + (cm%d ? %s : vec3(0.0));\n", i, k_color_in[cd], i, k_color_in[cc]);
        }
        sadd(&s, "  cr%d = clamp(cr%d, %s);\n", i, i, cclamp ? "0.0, 1.0" : "-4.0, 4.0");

        /* alpha */
        int ad = (ae >> 4) & 7, ac = (ae >> 7) & 7, ab = (ae >> 10) & 7, aa = (ae >> 13) & 7;
        int abias = (ae >> 16) & 3, asub = (ae >> 18) & 1, aclamp = (ae >> 19) & 1, ascale = (ae >> 20) & 3, adest = (ae >> 22) & 3;
        if (abias != 3) {
            sadd(&s, "  float ar%d = (%s %c mix(q8(%s), q8(%s), q8(%s)) + %s) * %.3f;\n", i, k_alpha_in[ad], asub ? '-' : '+',
                 k_alpha_in[aa], k_alpha_in[ab], k_alpha_in[ac], bias[abias], scale[ascale]);
        } else {
            int op = (ascale << 1) | asub;
            sadd(&s, "  float ar%d = %s + ((q8(%s) %s q8(%s)) ? %s : 0.0);\n", i, k_alpha_in[ad], k_alpha_in[aa],
                 op & 1 ? "==" : ">", k_alpha_in[ab], k_alpha_in[ac]);
        }
        sadd(&s, "  ar%d = clamp(ar%d, %s);\n", i, i, aclamp ? "0.0, 1.0" : "-4.0, 4.0");
        sadd(&s, "  %s.rgb = cr%d; %s.a = ar%d;\n", k_dest[cdest], i, k_dest[adest], i);
        last_cdest = cdest;
        last_adest = adest;
    }
    sadd(&s, "  vec4 outc = vec4(%s.rgb, %s.a);\n", k_dest[last_cdest], k_dest[last_adest]);
    int comp0 = (t->alpha_cmp >> 16) & 7, comp1 = (t->alpha_cmp >> 19) & 7, logic = (t->alpha_cmp >> 22) & 3;
    static const char *logic_op[4] = {"&&", "||", "!=", "=="};
    char c0[64], c1[64];
    snprintf(c0, sizeof c0, "%s", alpha_cmp(comp0));
    snprintf(c1, sizeof c1, "%s", alpha_cmp(comp1));
    sadd(&s, "  float A = floor(clamp(outc.a, 0.0, 1.0) * 255.0 + 0.5);\n");
    sadd(&s, "  bool p0, p1; { float R = floor(u_aref.x * 255.0 + 0.5); p0 = %s; }\n", c0);
    sadd(&s, "  { float R = floor(u_aref.y * 255.0 + 0.5); p1 = %s; }\n", c1);
    sadd(&s, "  if (!(p0 %s p1)) discard;\n", logic_op[logic]);
    sadd(&s, "  o = clamp(outc, 0.0, 1.0);\n}\n");
    return s.p;
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "%s\n", src);
        rt_fatal("shader compile failed: %s", log);
    }
    return s;
}

static GLuint g_vs_shader;

/* ---- shader cache --------------------------------------------------------------------------------
 * Building a TEV program the first time an effect appears can take tens of milliseconds (28 at once
 * when Mission 1's effects first show made a ~70 ms frame), and it used to happen again every session.
 * So every TEV configuration the game uses is appended to user/shaders.bin when it's first built, and
 * at start the renderer builds the saved ones in the background, a few milliseconds a frame (the logos
 * and menus are light), so they are ready before they're needed. The file is a header (magic, record
 * size) and GxTevConfig records; a file that doesn't match is started over. SFA_SHADER_CACHE=0 turns
 * the cache off. */
#define SHADER_CACHE_PATH "user/shaders.bin"
static const char k_cache_magic[8] = {'S', 'F', 'A', 'T', 'E', 'V', '0', '1'};
static GxTevConfig *g_warm;          /* saved configurations still to build */
static unsigned g_nwarm, g_warm_next, g_warm_built;
static FILE *g_cache_out;            /* append new configurations here */

static void shader_cache_open(void) {
    const char *e = getenv("SFA_SHADER_CACHE");
    if (e && atoi(e) == 0) return;
    uint32_t rec = (uint32_t)sizeof(GxTevConfig);
    FILE *f = fopen(SHADER_CACHE_PATH, "rb");
    int valid = 0;
    if (f) {
        char magic[8];
        uint32_t size = 0;
        if (fread(magic, 1, 8, f) == 8 && fread(&size, 4, 1, f) == 1 && !memcmp(magic, k_cache_magic, 8) && size == rec) {
            valid = 1;
            GxTevConfig cfg;
            while (fread(&cfg, rec, 1, f) == 1) {
                if (g_nwarm % 64 == 0) g_warm = realloc(g_warm, sizeof *g_warm * (g_nwarm + 64));
                g_warm[g_nwarm++] = cfg;
            }
        }
        fclose(f);
    }
    if (valid) g_cache_out = fopen(SHADER_CACHE_PATH, "ab");
    else if ((g_cache_out = fopen(SHADER_CACHE_PATH, "wb"))) { /* new, or from another version: start over */
        fwrite(k_cache_magic, 1, 8, g_cache_out);
        fwrite(&rec, 4, 1, g_cache_out);
        fflush(g_cache_out);
    }
    printf("[video] shader cache: %u saved shader setups to build\n", g_nwarm);
}

static TevProgram *tev_program_build(const GxTevConfig *cfg, int for_game);

/* Builds saved configurations for up to budget_ms (on the render thread, between frames). */
static void shader_cache_warm(Uint32 budget_ms) {
    if (g_warm_next >= g_nwarm) return;
    Uint32 start = SDL_GetTicks();
    while (g_warm_next < g_nwarm && SDL_GetTicks() - start < budget_ms) tev_program_build(&g_warm[g_warm_next++], 0);
    if (g_warm_next == g_nwarm) {
        printf("[video] shader cache: %u shader setups built ahead of time\n", g_warm_built);
        fflush(stdout);
        free(g_warm);
        g_warm = NULL;
    }
}

static TevProgram *tev_program(const GxTevConfig *cfg) { return tev_program_build(cfg, 1); }

static TevProgram *tev_program_build(const GxTevConfig *cfg, int for_game) {
    for (unsigned i = 0; i < g_nprogs; i++)
        if (!memcmp(&g_progs[i].cfg, cfg, sizeof *cfg)) return &g_progs[i];
    if (for_game) {
        g_shader_compiles++;
        if (g_cache_out) { /* remember it for next time */
            fwrite(cfg, sizeof *cfg, 1, g_cache_out);
            fflush(g_cache_out);
        }
    } else {
        g_warm_built++;
    }
    if (g_nprogs == g_cap_progs) {
        g_cap_progs = g_cap_progs ? g_cap_progs * 2 : 64;
        g_progs = realloc(g_progs, sizeof *g_progs * g_cap_progs);
    }
    TevProgram *p = &g_progs[g_nprogs++];
    p->cfg = *cfg;
    char *fs = tev_fragment_source(cfg);
    p->prog = glCreateProgram();
    glAttachShader(p->prog, g_vs_shader);
    glAttachShader(p->prog, compile(GL_FRAGMENT_SHADER, fs));
    free(fs);
    glLinkProgram(p->prog);
    glUseProgram(p->prog);
    for (int i = 0; i < 8; i++) {
        char name[8];
        snprintf(name, sizeof name, "tex%d", i);
        glUniform1i(glGetUniformLocation(p->prog, name), i);
    }
    p->u_konst = glGetUniformLocation(p->prog, "u_konst");
    p->u_reg = glGetUniformLocation(p->prog, "u_reg");
    p->u_aref = glGetUniformLocation(p->prog, "u_aref");
    return p;
}

static void gl_setup(void) {
    GL_FUNCS(GL_LOAD)
    g_vs_shader = compile(GL_VERTEX_SHADER, k_vs);

    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, 0, sizeof(GxVertex), (void *)offsetof(GxVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, 0, sizeof(GxVertex), (void *)offsetof(GxVertex, c0));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, 0, sizeof(GxVertex), (void *)offsetof(GxVertex, c1));
    for (int i = 0; i < 8; i++) {
        glEnableVertexAttribArray(3 + i);
        glVertexAttribPointer(3 + i, 2, GL_FLOAT, 0, sizeof(GxVertex), (void *)(offsetof(GxVertex, uv) + i * 2 * sizeof(float)));
    }

    /* 1x1 white texture for texture maps that have nothing bound */
    static const uint8_t white[4] = {255, 255, 255, 255};
    glGenTextures(1, &g_white);
    glBindTexture(GL_TEXTURE_2D, g_white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);

    /* the EFB */
    glGenTextures(1, &g_efb_color);
    glBindTexture(GL_TEXTURE_2D, g_efb_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EFB_WIDTH, EFB_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenRenderbuffers(1, &g_efb_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_efb_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, EFB_WIDTH, EFB_HEIGHT);
    glGenFramebuffers(1, &g_efb_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_efb_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_efb_depth);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    /* EFB copies to texture: a pass that converts an EFB rectangle into the copy's texture */
    g_copy_prog = glCreateProgram();
    glAttachShader(g_copy_prog, compile(GL_VERTEX_SHADER, k_copy_vs));
    glAttachShader(g_copy_prog, compile(GL_FRAGMENT_SHADER, k_copy_fs));
    glLinkProgram(g_copy_prog);
    glUseProgram(g_copy_prog);
    glUniform1i(glGetUniformLocation(g_copy_prog, "u_efb"), 0);
    g_copy_u_src = glGetUniformLocation(g_copy_prog, "u_src");
    g_copy_u_mode = glGetUniformLocation(g_copy_prog, "u_mode");
    g_copy_u_half = glGetUniformLocation(g_copy_prog, "u_half");
    g_copy_u_intensity = glGetUniformLocation(g_copy_prog, "u_intensity");
    glGenFramebuffers(1, &g_copy_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
}

/* GL texture for GxTexture id, (re)allocated at w x h; returns its name. */
static GLuint texture_slot(uint32_t id, uint32_t w, uint32_t h, const void *rgba) {
    if (id >= g_ntextures) {
        uint32_t n = g_ntextures ? g_ntextures : 256;
        while (n <= id) n *= 2;
        g_textures = realloc(g_textures, sizeof *g_textures * n);
        g_texture_dims = realloc(g_texture_dims, sizeof *g_texture_dims * n);
        memset(g_textures + g_ntextures, 0, sizeof *g_textures * (n - g_ntextures));
        memset(g_texture_dims + g_ntextures, 0, sizeof *g_texture_dims * (n - g_ntextures));
        g_ntextures = n;
    }
    if (!g_textures[id]) glGenTextures(1, &g_textures[id]);
    glBindTexture(GL_TEXTURE_2D, g_textures[id]);
    uint32_t dims = w << 16 | h;
    if (rgba || g_texture_dims[id] != dims) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        g_texture_dims[id] = dims;
    }
    return g_textures[id];
}

static Uint32 g_last_upload_ms; /* time render() spent uploading textures, last frame */
static void upload(GxTextureUpload *u) { texture_slot(u->id, u->width, u->height, u->rgba); }

/* Run an EFB copy to texture on the GPU. The caller restores the EFB framebuffer and state. */
static void efb_copy_pass(const GxEfbCopy *c) {
    GLuint tex = texture_slot(c->id, c->out_w, c->out_h, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, g_copy_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, c->out_w, c->out_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glUseProgram(g_copy_prog);
    glUniform2f(g_copy_u_src, c->x, c->y);
    glUniform1i(g_copy_u_mode, c->fmt);
    glUniform1i(g_copy_u_half, c->half);
    glUniform1i(g_copy_u_intensity, c->intensity);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_efb_color);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glViewport(0, 0, EFB_WIDTH, EFB_HEIGHT);
    glEnable(GL_SCISSOR_TEST);
}

static void apply_state(const GxState *s) {
    /* EFB y grows downwards; the GL framebuffer's y grows upwards. */
    glScissor(s->scissor[0], EFB_HEIGHT - 1 - s->scissor[3], s->scissor[2] - s->scissor[0] + 1,
              s->scissor[3] - s->scissor[1] + 1);
    if (s->z_enable) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(k_depth_func[s->z_func & 7]);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(s->z_update);
    glColorMask(s->color_update, s->color_update, s->color_update, s->alpha_update);
    if (s->blend_enable) {
        glEnable(GL_BLEND);
        GLenum src = k_blend_factor[s->blend_src & 7], dst = k_blend_factor[s->blend_dst & 7];
        if ((s->blend_src & 7) == 2) src = 0x0306; /* DST_COLOR */
        if ((s->blend_src & 7) == 3) src = 0x0307;
        if ((s->blend_dst & 7) == 2) dst = 0x0300; /* SRC_COLOR */
        if ((s->blend_dst & 7) == 3) dst = 0x0301;
        glBlendFunc(src, dst);
        glBlendEquation(s->blend_sub ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
    } else {
        glDisable(GL_BLEND);
    }
    if (s->cull == 0) {
        glDisable(GL_CULL_FACE);
    } else {
        /* GX "front" faces are clockwise in EFB space, which is counter-clockwise after the y flip */
        glEnable(GL_CULL_FACE);
        glCullFace(s->cull == 1 ? GL_FRONT : s->cull == 2 ? GL_BACK : GL_FRONT_AND_BACK);
    }
    TevProgram *p = tev_program(&s->tev);
    glUseProgram(p->prog);
    glUniform4fv(p->u_konst, 4, &s->konst[0][0]);
    glUniform4fv(p->u_reg, 4, &s->reg[0][0]);
    glUniform2f(p->u_aref, s->alpha_ref[0], s->alpha_ref[1]);
    static const GLint wrap[4] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT};
    for (int m = 0; m < 8; m++) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)m);
        uint32_t id = s->texture[m];
        if (id && id < g_ntextures && g_textures[id]) {
            glBindTexture(GL_TEXTURE_2D, g_textures[id]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap[s->wrap_s[m] & 3]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[s->wrap_t[m] & 3]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s->linear[m] ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s->linear[m] ? GL_LINEAR : GL_NEAREST);
        } else {
            glBindTexture(GL_TEXTURE_2D, g_white);
        }
    }
    glActiveTexture(GL_TEXTURE0);
}

static void write_bmp(const char *path, const uint8_t *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t row = (uint32_t)w * 3, pad = (4 - row % 4) % 4, size = (row + pad) * (uint32_t)h;
    uint8_t hdr[54] = {'B', 'M'};
    uint32_t v;
    v = 54 + size; memcpy(hdr + 2, &v, 4);
    v = 54; memcpy(hdr + 10, &v, 4);
    v = 40; memcpy(hdr + 14, &v, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    memcpy(hdr + 34, &size, 4);
    fwrite(hdr, 1, 54, f);
    for (int y = 0; y < h; y++) { /* rgba rows are bottom-up already (glReadPixels) */
        for (int x = 0; x < w; x++) {
            const uint8_t *p = rgba + ((size_t)y * w + x) * 4;
            uint8_t bgr[3] = {p[2], p[1], p[0]};
            fwrite(bgr, 1, 3, f);
        }
        fwrite("\0\0\0", 1, pad, f);
    }
    fclose(f);
}

static void render(GxFrame *f) {
    uint32_t render_start = SDL_GetTicks();
    Uint32 up0 = SDL_GetTicks();
    for (uint32_t i = 0; i < f->nuploads; i++) upload(&f->uploads[i]);
    g_last_upload_ms = SDL_GetTicks() - up0;
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glViewport(0, 0, EFB_WIDTH, EFB_HEIGHT);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(GxVertex) * f->nverts), f->verts, GL_STREAM_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_SCISSOR_TEST);

    uint32_t next_clear = 0, next_copy = 0;
    for (uint32_t b = 0; b <= f->nbatches; b++) {
        for (;;) { /* clears and EFB copies before batch b, in the order they were made */
            int has_clear = next_clear < f->nclears && f->clears[next_clear].at_batch == b;
            int has_copy = next_copy < f->nefb_copies && f->efb_copies[next_copy].at_batch == b;
            if (!has_clear && !has_copy) break;
            if (has_copy && (!has_clear || f->efb_copies[next_copy].seq < f->clears[next_clear].seq)) {
                efb_copy_pass(&f->efb_copies[next_copy++]);
                continue;
            }
            GxClear *cl = &f->clears[next_clear++];
            glDisable(GL_SCISSOR_TEST);
            glColorMask(1, 1, 1, 1);
            glDepthMask(1);
            glClearColor(cl->color[0], cl->color[1], cl->color[2], cl->color[3]);
            glClearDepth(cl->depth);
            glClear((cl->clear_color ? GL_COLOR_BUFFER_BIT : 0) | (cl->clear_depth ? GL_DEPTH_BUFFER_BIT : 0));
            glEnable(GL_SCISSOR_TEST);
        }
        if (b == f->nbatches) break;
        GxBatch *bt = &f->batches[b];
        apply_state(&bt->state);
        GLenum mode = bt->kind == GX_PRIM_LINES ? GL_LINES : bt->kind == GX_PRIM_POINTS ? GL_POINTS : GL_TRIANGLES;
        glDrawArrays(mode, (GLint)bt->first, (GLsizei)bt->count);
    }
    glDisable(GL_SCISSOR_TEST);

    if (f->copy) { /* EFB copy to texture: read the rectangle back for the guest, don't present */
        GxCopyRequest *r = f->copy;
        static uint8_t *tmp;
        static size_t tmp_size;
        size_t bytes = (size_t)r->w * r->h * 4;
        if (bytes > tmp_size) { tmp = realloc(tmp, bytes); tmp_size = bytes; }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
        glReadPixels((GLint)r->x, (GLint)(EFB_HEIGHT - (r->y + r->h)), (GLsizei)r->w, (GLsizei)r->h, GL_RGBA,
                     GL_UNSIGNED_BYTE, tmp);
        for (uint32_t y = 0; y < r->h; y++) /* GL rows are bottom-up */
            memcpy(r->rgba + (size_t)y * r->w * 4, tmp + (size_t)(r->h - 1 - y) * r->w * 4, (size_t)r->w * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        SDL_LockMutex(g_qlock);
        r->done = 1;
        SDL_CondBroadcast(g_done_cond);
        SDL_UnlockMutex(g_qlock);
        return;
    }

    /* present: the XFB copy rectangle, scaled to the window */
    int ww, wh;
    SDL_GL_GetDrawableSize(g_window, &ww, &wh);
    int sx = f->copy_rect[0], sy = f->copy_rect[1], sw = f->copy_rect[2], sh = f->copy_rect[3];
    if (!sw || !sh) { sx = 0; sy = 0; sw = 640; sh = 480; }
    g_last_rect[0] = sx; g_last_rect[1] = sy; g_last_rect[2] = sw; g_last_rect[3] = sh;
    int gy0 = EFB_HEIGHT - (sy + sh), gy1 = EFB_HEIGHT - sy;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    double scale = (double)ww / sw < (double)wh / sh ? (double)ww / sw : (double)wh / sh;
    int dw = (int)(sw * scale), dh = (int)(sh * scale), dx = (ww - dw) / 2, dy = (wh - dh) / 2;
    glBlitFramebuffer(sx, gy0, sx + sw, gy1, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    if (g_dump) {
        static uint8_t *pix;
        static int n;
        if (!pix) pix = malloc((size_t)640 * 528 * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
        glReadPixels(sx, gy0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, pix);
        char path[256];
        snprintf(path, sizeof path, "build/GF7E01/frames/frame_%05u.bmp", f->number);
        if (n++ % 15 == 0) write_bmp(path, pix, sw, sh); /* every 15th frame */
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    SDL_GL_SwapWindow(g_window);

    /* SFA_FPS=1: every 5 s of wall time, the presented frame rate, hitches (the longest gap between
     * frames, and how many gaps exceeded 50 ms), shader compiles, texture uploads and audio trouble */
    static int show_fps = -1;
    static uint32_t fps_start, fps_frames, last_present, worst, slow, uploads;
    static unsigned last_compiles, last_underruns, last_drops;
    if (show_fps < 0) show_fps = getenv("SFA_FPS") != NULL;
    if (show_fps) {
        uint32_t now = SDL_GetTicks();
        if (!fps_start) fps_start = now;
        if (last_present) {
            uint32_t gap = now - last_present;
            if (gap > worst) worst = gap;
            if (gap > 50) slow++;
            static int stall_ms = -1; /* SFA_STALL_MS: report frames later than this (default 80) */
            if (stall_ms < 0) stall_ms = getenv("SFA_STALL_MS") ? atoi(getenv("SFA_STALL_MS")) : 80;
            if (gap > (uint32_t)stall_ms) { /* with SFA_PROFILE: what ran during the hitch */
                printf("[stall] frame %u: %u ms since the last frame, %u ms of it rendering this one (%u texture uploads, "
                       "%u ms of them)\n", f->number, gap, now - render_start, f->nuploads, g_last_upload_ms);
                rt_prof_stall(gap);
            }
        }
        last_present = now;
        uploads += f->nuploads;
        fps_frames++;
        if (now - fps_start >= 5000) {
            extern unsigned long g_ax_clipped; /* ax.c */
            extern double g_ax_min_gain;
            static unsigned long last_clipped;
            printf("[fps] %.1f frames/s (frame %u); longest gap %u ms, %u gaps > 50 ms; %u shader compiles, "
                   "%u texture uploads; audio %u underruns, %u frames dropped, %lu samples clipped, limiter down to %.2f\n",
                   fps_frames * 1000.0 / (now - fps_start), f->number, worst, slow, g_shader_compiles - last_compiles,
                   uploads, g_audio_underruns - last_underruns, g_audio_drops - last_drops, g_ax_clipped - last_clipped,
                   g_ax_min_gain);
            last_clipped = g_ax_clipped;
            g_ax_min_gain = 1.0;
            fflush(stdout);
            fps_start = now;
            fps_frames = worst = slow = uploads = 0;
            last_compiles = g_shader_compiles;
            last_underruns = g_audio_underruns;
            last_drops = g_audio_drops;
        }
    }
}

/* Open the controllers already connected (they don't send an "added" event here: the start screen took
 * those), in SDL's order. */
/* ---- story pilot picker ----------------------------------------------------------------------------
 * Before a story mission the game thread asks which character to play (video_pick_pilot) and waits; the
 * render thread meanwhile keeps showing the last frame, dimmed, with the choices drawn over it in the
 * launcher's font (launcher.c). Left/right (D-pad, stick or arrow keys) moves, A or START (Space, Enter)
 * confirms. */
uint8_t *font_atlas_rgba(int hi, int *w, int *h);
const uint8_t *font_widths(void);
#define UI_FCELL 24
#define UI_FPAD 2
#define UI_FCOLS 16
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303

static const char *const k_pilots[5] = {"Fox", "Falco", "Slippy", "Krystal", "Random"};
static volatile int g_pick_active, g_pick_sel, g_pick_mission;
static char g_pick_note[64]; /* under the choices once confirmed (which pilot Random gave) */

static GLuint g_ui_prog, g_ui_vao, g_ui_vbo, g_ui_font;
static GLint g_ui_u_screen;
static int g_ui_font_w, g_ui_font_h, g_ui_ready; /* 0 not set up, 1 ready, -1 no font */
typedef struct { float x, y, u, v, r, g, b, a; } UiVert;
static UiVert *g_ui_rects, *g_ui_text;
static int g_ui_nrects, g_ui_ntext, g_ui_cap_rects, g_ui_cap_text;

static const char *k_ui_vs =
    "#version 330 core\n"
    "layout(location = 0) in vec2 p; layout(location = 1) in vec2 uv; layout(location = 2) in vec4 c;\n"
    "uniform vec2 u_screen; out vec2 v_uv; out vec4 v_c;\n"
    "void main() { v_uv = uv; v_c = c; gl_Position = vec4(p.x / u_screen.x * 2.0 - 1.0, 1.0 - p.y / u_screen.y * 2.0, 0.0, 1.0); }\n";
static const char *k_ui_fs =
    "#version 330 core\n"
    "in vec2 v_uv; in vec4 v_c; uniform sampler2D u_tex; out vec4 o;\n"
    "void main() { o = v_c * texture(u_tex, v_uv); }\n";

static void ui_setup(void) {
    if (g_ui_ready) return;
    int w, h;
    uint8_t *px = font_atlas_rgba(1, &w, &h);
    if (!px) { g_ui_ready = -1; return; }
    g_ui_font_w = w;
    g_ui_font_h = h;
    glGenTextures(1, &g_ui_font);
    glBindTexture(GL_TEXTURE_2D, g_ui_font);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(px);
    g_ui_prog = glCreateProgram();
    glAttachShader(g_ui_prog, compile(GL_VERTEX_SHADER, k_ui_vs));
    glAttachShader(g_ui_prog, compile(GL_FRAGMENT_SHADER, k_ui_fs));
    glLinkProgram(g_ui_prog);
    glUseProgram(g_ui_prog);
    glUniform1i(glGetUniformLocation(g_ui_prog, "u_tex"), 0);
    g_ui_u_screen = glGetUniformLocation(g_ui_prog, "u_screen");
    glGenVertexArrays(1, &g_ui_vao);
    glBindVertexArray(g_ui_vao);
    glGenBuffers(1, &g_ui_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_ui_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(UiVert), (void *)offsetof(UiVert, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(UiVert), (void *)offsetof(UiVert, u));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, 0, sizeof(UiVert), (void *)offsetof(UiVert, r));
    glBindVertexArray(g_vao); /* the game's draws use their own */
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    g_ui_ready = 1;
}

static void ui_quad(int text, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                    const float c[4]) {
    UiVert **list = text ? &g_ui_text : &g_ui_rects;
    int *n = text ? &g_ui_ntext : &g_ui_nrects, *cap = text ? &g_ui_cap_text : &g_ui_cap_rects;
    if (*n + 6 > *cap) {
        *cap = *cap ? *cap * 2 : 600;
        *list = realloc(*list, sizeof **list * (size_t)*cap);
    }
    UiVert q[4] = {{x, y, u0, v0, c[0], c[1], c[2], c[3]}, {x + w, y, u1, v0, c[0], c[1], c[2], c[3]},
                   {x, y + h, u0, v1, c[0], c[1], c[2], c[3]}, {x + w, y + h, u1, v1, c[0], c[1], c[2], c[3]}};
    UiVert *o = *list + *n;
    o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = q[1]; o[4] = q[3]; o[5] = q[2];
    *n += 6;
}

static void ui_rect(float x, float y, float w, float h, const float c[4]) { ui_quad(0, x, y, w, h, 0, 0, 1, 1, c); }

static float ui_text_w(const char *s, float size) {
    const uint8_t *fw = font_widths();
    float w = 0, k = size / UI_FCELL;
    for (; *s; s++) {
        int ch = (unsigned char)*s;
        w += fw[(ch < 32 ? '?' : ch) - 32] * k;
    }
    return w;
}

/* text with its top-left at (x, y), size pixels tall; bold draws twice, a little apart */
static void ui_text(const char *s, float x, float y, float size, const float c[4], int bold) {
    const uint8_t *fw = font_widths();
    float k = size / UI_FCELL, cell = (UI_FCELL + 2 * UI_FPAD) * 3.0f;
    for (; *s; s++) {
        int ch = (unsigned char)*s, g = (ch < 32 ? '?' : ch) - 32;
        float u0 = (g % UI_FCOLS) * cell / g_ui_font_w, v0 = (g / UI_FCOLS) * cell / g_ui_font_h;
        float u1 = u0 + cell / g_ui_font_w, v1 = v0 + cell / g_ui_font_h, sz = (UI_FCELL + 2 * UI_FPAD) * k;
        if (ch != ' ') {
            ui_quad(1, x - UI_FPAD * k, y - UI_FPAD * k, sz, sz, u0, v0, u1, v1, c);
            if (bold) ui_quad(1, x - UI_FPAD * k + size * 0.035f, y - UI_FPAD * k, sz, sz, u0, v0, u1, v1, c);
        }
        x += fw[g] * k;
    }
}

static void ui_text_centered(const char *s, float cx, float y, float size, const float c[4], int bold) {
    ui_text(s, cx - ui_text_w(s, size) / 2, y, size, c, bold);
}

/* the picker over the frame already in the default framebuffer (ww x wh pixels) */
static void draw_picker(int ww, int wh) {
    ui_setup();
    if (g_ui_ready != 1) return;
    g_ui_nrects = g_ui_ntext = 0;
    float s = wh / 720.0f, cx = ww / 2.0f;
    if (ww / 1280.0f < s) s = ww / 1280.0f;
    static const float dim[4] = {0, 0, 0, 0.6f}, panel[4] = {0.055f, 0.08f, 0.15f, 0.94f},
                       card[4] = {0.11f, 0.15f, 0.26f, 1}, accent[4] = {0.25f, 0.47f, 1.0f, 1},
                       edge[4] = {0.45f, 0.65f, 1.0f, 1}, white[4] = {0.94f, 0.96f, 1, 1},
                       muted[4] = {0.62f, 0.68f, 0.82f, 1}, faint[4] = {0.45f, 0.5f, 0.62f, 1};
    ui_rect(0, 0, (float)ww, (float)wh, dim);
    float pw = 1000 * s, ph = 330 * s, px = cx - pw / 2, py = wh / 2.0f - ph / 2;
    ui_rect(px, py, pw, ph, panel);
    ui_rect(px, py, pw, 4 * s, accent);
    char title[48];
    snprintf(title, sizeof title, "MISSION %d", g_pick_mission);
    ui_text_centered(title, cx, py + 26 * s, 18 * s, muted, 0);
    ui_text_centered("Choose your pilot", cx, py + 52 * s, 40 * s, white, 1);
    float bw = 168 * s, bh = 96 * s, gap = 18 * s, bx = cx - (5 * bw + 4 * gap) / 2, by = py + 124 * s;
    for (int i = 0; i < 5; i++) {
        float x = bx + i * (bw + gap);
        int on = i == g_pick_sel;
        if (on) ui_rect(x - 3 * s, by - 3 * s, bw + 6 * s, bh + 6 * s, edge);
        ui_rect(x, by, bw, bh, on ? accent : card);
        ui_text_centered(k_pilots[i], x + bw / 2, by + bh / 2 - 15 * s, 30 * s, on ? white : muted, on);
    }
    if (g_pick_note[0]) ui_text_centered(g_pick_note, cx, by + bh + 34 * s, 24 * s, white, 1);
    else ui_text_centered("Left / Right  choose        A / START  confirm", cx, by + bh + 38 * s, 18 * s, faint, 0);
    ui_text_centered("For the parts on foot. Arwing flights and most cutscenes stay as Fox.", cx, py + ph - 34 * s, 15 * s, faint, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, ww, wh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_ui_prog);
    glUniform2f(g_ui_u_screen, (float)ww, (float)wh);
    glBindVertexArray(g_ui_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_ui_vbo);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_white);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(UiVert) * (size_t)g_ui_nrects), g_ui_rects, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, g_ui_nrects);
    glBindTexture(GL_TEXTURE_2D, g_ui_font);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(UiVert) * (size_t)g_ui_ntext), g_ui_text, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, g_ui_ntext);
    glDisable(GL_BLEND);
    glBindVertexArray(g_vao); /* back to the game's */
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
}

/* While the game waits on the picker no frames come: show the last one again with the picker over it. */
static void present_picker(void) {
    int ww, wh;
    SDL_GL_GetDrawableSize(g_window, &ww, &wh);
    int sx = g_last_rect[0], sy = g_last_rect[1], sw = g_last_rect[2], sh = g_last_rect[3];
    int gy0 = EFB_HEIGHT - (sy + sh), gy1 = EFB_HEIGHT - sy;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    double scale = (double)ww / sw < (double)wh / sh ? (double)ww / sw : (double)wh / sh;
    int dw = (int)(sw * scale), dh = (int)(sh * scale), dx = (ww - dw) / 2, dy = (wh - dh) / 2;
    glBlitFramebuffer(sx, gy0, sx + sw, gy1, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    draw_picker(ww, wh);
    if (g_dump) { /* --dump-frames: the picker too, now and then */
        static Uint32 last;
        if (SDL_GetTicks() - last > 500) {
            last = SDL_GetTicks();
            uint8_t *pix = malloc((size_t)ww * wh * 4);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, pix); /* bottom-up rows, as write_bmp takes */
            write_bmp("build/GF7E01/frames/picker.bmp", pix, ww, wh);
            free(pix);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    SDL_GL_SwapWindow(g_window);
}

/* Called by the game thread before a story mission (hle.c): shows the picker until the player confirms
 * and returns the character (0 Fox, 1 Falco, 2 Slippy, 3 Krystal). *choice is the highlighted entry on
 * entry (4 = Random) and the confirmed one on return. */
int video_pick_pilot(int mission, int *choice) {
    g_pick_mission = mission;
    g_pick_sel = *choice >= 0 && *choice < 5 ? *choice : 0;
    g_pick_note[0] = 0;
    g_pick_active = 1;
    Uint32 start = SDL_GetTicks();
    int last_dir = 0, released = 0;
    while (!g_quit) {
        PadState p;
        get_pad(0, &p);
        int dir = (p.buttons & 0x1) || p.stick_x < -60 ? -1 : (p.buttons & 0x2) || p.stick_x > 60 ? 1 : 0;
        if (dir && dir != last_dir) g_pick_sel = (g_pick_sel + dir + 5) % 5;
        last_dir = dir;
        int confirm = (p.buttons & 0x1100) != 0; /* A or START */
        if (!confirm) released = 1; /* a press still held from the menus doesn't count */
        else if (released && SDL_GetTicks() - start > 400) break;
        SDL_Delay(16);
    }
    int sel = g_pick_sel, ch = sel;
    if (sel == 4) {
        ch = (int)(SDL_GetPerformanceCounter() % 4);
        snprintf(g_pick_note, sizeof g_pick_note, "Random pick: %s", k_pilots[ch]);
        SDL_Delay(1200);
    }
    g_pick_active = 0;
    *choice = sel;
    return ch;
}

static void open_controllers(void) {
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) add_controller(i);
    fflush(stdout);
}

static int render_thread(void *arg) {
    (void)arg;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_window = SDL_CreateWindow("Star Fox: Assault (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                g_window_w, g_window_h,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                    (g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!g_window) rt_fatal("SDL_CreateWindow: %s", SDL_GetError());
    SDL_GLContext gl = SDL_GL_CreateContext(g_window);
    if (!gl) rt_fatal("SDL_GL_CreateContext: %s", SDL_GetError());
    SDL_GL_SetSwapInterval(0);
    gl_setup();
    printf("[video] OpenGL renderer ready\n");
    shader_cache_open();
    if (g_dump) {
#ifdef _WIN32
        system("if not exist build\\GF7E01\\frames mkdir build\\GF7E01\\frames");
#else
        system("mkdir -p build/GF7E01/frames");
#endif
    }

    open_controllers();
    while (!g_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) g_quit = 1;
            else if (e.type == SDL_KEYDOWN && !e.key.repeat &&
                     (e.key.keysym.sym == SDLK_F11 ||
                      (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)))) {
                g_fullscreen = !g_fullscreen; /* F11 or Alt+Enter: toggle fullscreen */
                SDL_SetWindowFullscreen(g_window, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                add_controller(e.cdevice.which);
            } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
                remove_controller(e.cdevice.which);
            }
        }
        poll_keyboard();
        for (int i = 0; i < NPADS; i++) poll_controller(i);
        SDL_LockMutex(g_qlock);
        if (!g_pending_frame) SDL_CondWaitTimeout(g_qcond, g_qlock, 16);
        GxFrame *f = g_pending_frame;
        g_pending_frame = NULL;
        SDL_CondBroadcast(g_done_cond); /* a waiting copy segment may queue now */
        SDL_UnlockMutex(g_qlock);
        if (!f && g_pick_active) present_picker();
        if (f) {
            uint64_t t0 = g_prof_render ? rt_timebase() : 0;
            render(f);
            if (t0) g_t_render += rt_timebase() - t0;
            free_frame(f);
            shader_cache_warm(6); /* saved shader setups, a few ms a frame until all are built */
        }
    }
    return 0;
}

void video_start(int dump_frames) {
    g_dump = dump_frames;
    g_prof_render = getenv("SFA_PROFILE") != NULL;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_STEAMDECK", "1"); /* the Deck's own controls, also when not started from Steam */
    /* SDL is initialised here (or by the start screen) on the main thread, but events are pumped on the
     * render thread. On Windows, Xbox/XInput pads arrive as raw input messages to a window on the thread
     * that initialised the joysticks, so without SDL's own joystick thread they never arrive (see launcher.c). */
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) rt_fatal("SDL_Init: %s", SDL_GetError());
    g_audio_muted = getenv("SFA_MUTE") != NULL;
    if (g_audio_muted) printf("[audio] muted (SFA_MUTE)\n");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want = {0}, have;
        want.freq = 32000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        want.callback = audio_callback;
        g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (g_audio) {
            SDL_PauseAudioDevice(g_audio, 0);
            printf("[audio] %d Hz stereo\n", have.freq);
        } else {
            printf("[audio] no audio device: %s\n", SDL_GetError());
        }
    }
    g_pad_lock = SDL_CreateMutex();
    g_qlock = SDL_CreateMutex();
    g_qcond = SDL_CreateCond();
    g_done_cond = SDL_CreateCond();
    SDL_CreateThread(render_thread, "render", NULL);
}
