/*
 * rsk_osd — Linux-аналог RSK OSD для Ninkear A16Pro.
 *
 * Демон: опрашивает режим питания (байт TDPF EC 0x0A, fallback EV20)
 * и при смене показывает LayerShell-оверлей поверх всех окон,
 * как RSK под Windows (плашка 3000мс, белый текст на чёрном
 * скруглённом квадрате, 70% непрозрачности).
 *
 * Один файл, статический бинарь, зависимости времени выполнения: нет
 * (только ядро: ec_sys или acpi_call + Wayland-композитор с layer-shell).
 *
 * Использование:
 *   sudo ./rsk_osd                обычный режим демона
 *   sudo ./rsk_osd --once         напечатать текущий режим и выйти
 *   sudo ./rsk_osd --demo turbo   показать плашку один раз и выйти
 *   sudo ./rsk_osd --no-gui       только опрос EC (для отладки детекта)
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <dirent.h>

#include <wayland-client-core.h>
/* get_popup из layer-shell не используется, но сгенерённый заголовок
   ссылается на struct xdg_popup — forward declaration достаточно. */
struct xdg_popup;
extern const struct wl_interface xdg_popup_interface;
#include "wlr-layer-shell-unstable-v1.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>
#include "font_bold.h"

/* get_popup из layer-shell демон никогда не вызывает, но сгенерённый
   протокол ссылается на xdg_popup_interface в таблице типов.
   Заглушка, чтобы слинковалось; в рантайме не используется. */
const struct wl_interface xdg_popup_interface = {
    "xdg_popup", 1, 0, NULL, 0, NULL
};

/* ------------------------------------------------------------------ */
/* Режим питания                                                      */
/* ------------------------------------------------------------------ */
typedef enum { M_LOW = 0, M_BAL = 1, M_TURBO = 2, M_UNKNOWN = -1 } powmode_t;

static const char *mode_name(powmode_t m)
{
    switch (m) {
    case M_LOW:   return "Low";
    case M_BAL:   return "Balanced";
    case M_TURBO: return "Turbo";
    default:      return "Unknown";
    }
}

#define EC_IO     "/sys/kernel/debug/ec/ec0/io"
#define ACPI_CALL "/proc/acpi/call"
#define EC_TDPF_OFF 0x0A   /* TDPF: 0x23=Low 0x2D=Balanced 0x37=Turbo */

static int ec_read(int off)
{
    int fd = open(EC_IO, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    unsigned char b = 0;
    ssize_t r = pread(fd, &b, 1, (off_t)off);
    close(fd);
    return (r == 1) ? (int)b : -1;
}

static powmode_t mode_from_tdpf(int v)
{
    if (v == 0x23)
        return M_LOW;
    if (v == 0x2D)
        return M_BAL;
    if (v == 0x37)
        return M_TURBO;
    return M_UNKNOWN;
}

/* Fallback: EV20 через acpi_call (возвращает 0/1/2). */
static powmode_t mode_from_ev20(void)
{
    int fd = open(ACPI_CALL, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return M_UNKNOWN;
    static const char q[] = "\\_SB.PCI0.WMID.EV20";
    if (write(fd, q, sizeof(q)) != (ssize_t)sizeof(q)) {
        close(fd);
        return M_UNKNOWN;
    }
    char buf[128];
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0)
        return M_UNKNOWN;
    buf[r] = '\0';
    if (strstr(buf, "Error"))
        return M_UNKNOWN;
    char *p = strstr(buf, "0x");
    if (!p)
        return M_UNKNOWN;
    long v = strtol(p, NULL, 16);
    if (v == 0)
        return M_LOW;
    if (v == 1)
        return M_BAL;
    if (v == 2)
        return M_TURBO;
    return M_UNKNOWN;
}

static powmode_t detect_mode(void)
{
    powmode_t m = mode_from_tdpf(ec_read(EC_TDPF_OFF));
    if (m != M_UNKNOWN)
        return m;
    return mode_from_ev20();
}

/* ------------------------------------------------------------------ */
/* Поиск Wayland-сессии (нужно и руту под sudo, и будущему сервису).  */
/* Если WAYLAND_DISPLAY не задан — ищем сокет /run/user/<uid>/wayland-* */
/* и подставляем его + XDG_RUNTIME_DIR автоматически.                 */
/* ------------------------------------------------------------------ */
static void fix_wayland_env(void)
{
    const char *wd = getenv("WAYLAND_DISPLAY");
    const char *xd = getenv("XDG_RUNTIME_DIR");
    if (wd && *wd && xd && *xd)
        return; /* всё уже есть (обычный запуск из сессии) */

    /* WAYLAND_DISPLAY задан, но абсолютным путём без XDG_RUNTIME_DIR. */
    if (wd && *wd && strchr(wd, '/') && (!xd || !*xd)) {
        char dir[256];
        snprintf(dir, sizeof(dir), "%s", wd);
        char *sl = strrchr(dir, '/');
        if (sl && sl != dir) {
            *sl = '\0';
            setenv("XDG_RUNTIME_DIR", dir, 0);
            setenv("WAYLAND_DISPLAY", sl + 1, 1);
            return;
        }
    }

    /* Сканируем /run/user/<uid>/wayland-*. */
    DIR *rd = opendir("/run/user");
    if (!rd)
        return;
    struct dirent *de;
    while ((de = readdir(rd)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        char *end = NULL;
        (void)strtol(de->d_name, &end, 10);
        if (!end || *end != '\0')
            continue; /* не uid-каталог */
        char sockdir[256], sock[300];
        snprintf(sockdir, sizeof(sockdir), "/run/user/%s", de->d_name);
        for (int i = 0; i <= 3; i++) {
            snprintf(sock, sizeof(sock), "%s/wayland-%d", sockdir, i);
            struct stat st;
            if (stat(sock, &st) == 0 && S_ISSOCK(st.st_mode)) {
                char wdname[32];
                snprintf(wdname, sizeof(wdname), "wayland-%d", i);
                setenv("XDG_RUNTIME_DIR", sockdir, 0);
                setenv("WAYLAND_DISPLAY", wdname, 0);
                closedir(rd);
                return;
            }
        }
    }
    closedir(rd);
}
enum ost { OS_HIDDEN, OS_FADEIN, OS_SHOWN, OS_FADEOUT };
#define FADE_MS 120

struct app {
    struct wl_display *dpy;
    struct wl_registry *reg;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer;
    struct wl_output *out;
    int out_w, out_h, out_scale;
    bool out_ready;

    struct wl_surface *surf;
    struct zwlr_layer_surface_v1 *lsurf;
    struct wl_buffer *buf;
    struct wl_shm_pool *pool;
    void *px;
    uint32_t *base;   /* кадр на полной яркости (для fade) */
    size_t pxsz;
    int shm_fd;
    int w, h;
    bool configured;
    bool closed;

    enum ost ost;
    int64_t t0;          /* начало текущей фазы */
    int64_t hold_until;  /* когда гасить после SHOWN */
};

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
                         int32_t pw, int32_t ph, int32_t sub, const char *make,
                         const char *model, int32_t tr)
{
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph;
    (void)sub; (void)make; (void)model; (void)tr;
}
static void out_mode(void *d, struct wl_output *o, uint32_t flags,
                     int32_t w, int32_t h, int32_t refresh)
{
    struct app *a = d;
    (void)o; (void)refresh;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        a->out_w = w;
        a->out_h = h;
    }
}
static void out_done(void *d, struct wl_output *o)
{
    struct app *a = d;
    (void)o;
    if (a->out_w > 0 && a->out_h > 0)
        a->out_ready = true;
}
static void out_scale(void *d, struct wl_output *o, int32_t s)
{
    struct app *a = d;
    (void)o;
    if (s > 0)
        a->out_scale = s;
}
static const struct wl_output_listener out_listener = {
    out_geometry, out_mode, out_done, out_scale,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver)
{
    struct app *a = d;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        a->comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        a->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        uint32_t v = ver < 1 ? ver : 1;
        a->layer = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, v);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        if (!a->out) {
            uint32_t v = ver < 2 ? ver : 2;
            a->out = wl_registry_bind(r, name, &wl_output_interface, v);
            wl_output_add_listener(a->out, &out_listener, a);
        }
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{
    (void)d; (void)r; (void)name;
}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void lsurf_configure(void *d, struct zwlr_layer_surface_v1 *s,
                            uint32_t serial, uint32_t w, uint32_t h)
{
    struct app *a = d;
    (void)s; (void)w; (void)h;
    zwlr_layer_surface_v1_ack_configure(a->lsurf, serial);
    if (a->buf) {
        wl_surface_attach(a->surf, a->buf, 0, 0);
        wl_surface_damage(a->surf, 0, 0, a->w, a->h);
        wl_surface_commit(a->surf);
    }
    a->configured = true;
}
static void lsurf_closed(void *d, struct zwlr_layer_surface_v1 *s)
{
    struct app *a = d;
    (void)s;
    a->closed = true;
}
static const struct zwlr_layer_surface_v1_listener lsurf_listener = {
    lsurf_configure, lsurf_closed,
};

/* ------------------------------------------------------------------ */
/* Рендер: чёрный скруглённый квадрат (alpha 178/255 = 70%) + белый    */
/* жирный текст по центру. Буфер ARGB8888 premultiplied.              */
/* ------------------------------------------------------------------ */
static stbtt_fontinfo g_font;
static bool g_font_ok = false;

/* Минимальный UTF-8 декодер (кириллица в субтитре). */
static uint32_t utf8_next(const char **pp)
{
    const unsigned char *s = (const unsigned char *)*pp;
    uint32_t c = *s++;
    if (c < 0x80) {
        /* ASCII */
    } else if ((c & 0xE0) == 0xC0) {
        c = ((c & 0x1F) << 6) | (s[0] & 0x3F);
        s += 1;
    } else if ((c & 0xF0) == 0xE0) {
        c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F);
        s += 2;
    } else if ((c & 0xF8) == 0xF0) {
        c = ((c & 0x07) << 18) | ((s[0] & 0x3F) << 12) |
            ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        s += 3;
    } else {
        c = 0xFFFD;
    }
    *pp = (const char *)s;
    return c;
}

#define OSD_PAD_X   56
#define OSD_PAD_TOP 26
#define OSD_PAD_BOT 26
#define OSD_GAP     12
#define OSD_RADIUS  30
#define OSD_FONT_PX 46
#define OSD_SUB_PX  24
#define BOX_ALPHA   178

/* Номиналы режима из DSDT (_Q56/57/58 + батарейные ветки BTDV<=0x3A98):
 * ac    — STAPM на сети, Вт
 * boost — PPT fast (кратковременный потолок), Вт
 * bat   — STAPM на батарее, диапазон batt-low..batt-ok, Вт */
struct mode_info { int ac, boost, bat_lo, bat_hi; };
static struct mode_info mode_info(powmode_t m)
{
    switch (m) {
    case M_LOW:   return (struct mode_info){ 35, 45, 25, 35 };
    case M_BAL:   return (struct mode_info){ 45, 55, 35, 45 };
    case M_TURBO: return (struct mode_info){ 55, 55, 45, 55 };
    default:      return (struct mode_info){ 0, 0, 0, 0 };
    }
}

static int text_width_px(const char *s, float scale)
{
    /* UTF-8 -> codepoints, иначе кириллица развалится на байты. */
    uint32_t prev = 0;
    bool first = true;
    int w = 0;
    const char *p = s;
    while (*p) {
        uint32_t c = utf8_next(&p);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, (int)c, &adv, &lsb);
        w += (int)(adv * scale + 0.5f);
        if (!first)
            w += (int)(stbtt_GetCodepointKernAdvance(&g_font, (int)prev,
                                                    (int)c) * scale);
        prev = c;
        first = false;
    }
    return w;
}

/* Точка внутри скруглённого прямоугольника? */
static bool in_rrect(int x, int y, int w, int h, int r)
{
    if (x < 0 || y < 0 || x >= w || y >= h)
        return false;
    int cx = (x < r) ? r - x : (x >= w - r ? x - (w - r - 1) : -1);
    int cy = (y < r) ? r - y : (y >= h - r ? y - (h - r - 1) : -1);
    if (cx < 0 || cy < 0)
        return true;
    return cx * cx + cy * cy <= r * r;
}

struct osd_layout {
    int w, h;
    int base_main, base_s1, base_s2;
    float fs_main, fs_sub;
};

static void layout_osd(struct osd_layout *L, const char *main_t,
                       const char *s1, const char *s2, int scale)
{
    L->fs_main = stbtt_ScaleForPixelHeight(&g_font, (float)(OSD_FONT_PX * scale));
    L->fs_sub = stbtt_ScaleForPixelHeight(&g_font, (float)(OSD_SUB_PX * scale));
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&g_font, &asc, &desc, &gap);
    int tw = text_width_px(main_t, L->fs_main);
    if (s1[0]) {
        int t = text_width_px(s1, L->fs_sub);
        if (t > tw)
            tw = t;
    }
    if (s2[0]) {
        int t = text_width_px(s2, L->fs_sub);
        if (t > tw)
            tw = t;
    }
    L->w = tw + 2 * OSD_PAD_X * scale;
    if (L->w < 300 * scale)
        L->w = 300 * scale;
    L->base_main = OSD_PAD_TOP * scale + (int)(asc * L->fs_main);
    L->base_s1 = L->base_main + (int)(desc * L->fs_main) +
                 OSD_GAP * scale + (int)(asc * L->fs_sub);
    L->base_s2 = L->base_s1 + (int)(desc * L->fs_sub) +
                 (OSD_GAP * scale) / 2 + (int)(asc * L->fs_sub);
    L->h = L->base_s2 + (int)(desc * L->fs_sub) + OSD_PAD_BOT * scale;
}

static void draw_line(uint32_t *px, int w, int h, const char *s,
                      float fscale, int baseline, float bright)
{
    int tw = text_width_px(s, fscale);
    int x = (w - tw) / 2;
    uint32_t prev = 0;
    bool first = true;
    const char *p = s;
    while (*p) {
        uint32_t c = utf8_next(&p);
        if (!first)
            x += (int)(stbtt_GetCodepointKernAdvance(&g_font, (int)prev,
                                                    (int)c) * fscale);
        int gw, gh, xoff, yoff;
        unsigned char *bm = stbtt_GetCodepointBitmap(&g_font, fscale, fscale,
                                                    (int)c, &gw, &gh,
                                                    &xoff, &yoff);
        if (bm) {
            for (int gy = 0; gy < gh; gy++) {
                for (int gx = 0; gx < gw; gx++) {
                    int dx = x + xoff + gx;
                    int dy = baseline + yoff + gy;
                    if (dx < 0 || dy < 0 || dx >= w || dy >= h)
                        continue;
                    /* Белый поверх фона, premultiplied over. */
                    float sa = bm[(size_t)gy * gw + gx] / 255.0f * bright;
                    if (sa <= 0.0f)
                        continue;
                    if (sa > 1.0f)
                        sa = 1.0f;
                    uint32_t dst = px[(size_t)dy * w + dx];
                    float da = ((dst >> 24) & 0xFF) / 255.0f;
                    float oa = sa + da * (1.0f - sa);
                    uint8_t cc = (uint8_t)(255.0f * sa + 0.5f);
                    uint8_t aa = (uint8_t)(255.0f * oa + 0.5f);
                    px[(size_t)dy * w + dx] =
                        ((uint32_t)aa << 24) | ((uint32_t)cc << 16) |
                        ((uint32_t)cc << 8) | (uint32_t)cc;
                }
            }
            stbtt_FreeBitmap(bm, NULL);
        }
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, (int)c, &adv, &lsb);
        x += (int)(adv * fscale + 0.5f);
        prev = c;
        first = false;
    }
}

static void render_osd(uint32_t *px, const struct osd_layout *L,
                       const char *main_t, const char *s1, const char *s2,
                       int scale)
{
    int w = L->w, h = L->h;
    /* Фон: чёрный, alpha 178 (premultiplied: rgb=0). */
    size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++)
        px[i] = ((uint32_t)BOX_ALPHA << 24);

    /* Скругление: пиксели вне фигуры -> полностью прозрачные. */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (!in_rrect(x, y, w, h, OSD_RADIUS * scale))
                px[(size_t)y * w + x] = 0;

    draw_line(px, w, h, main_t, L->fs_main, L->base_main, 1.0f);
    if (s1[0])
        draw_line(px, w, h, s1, L->fs_sub, L->base_s1, 0.9f);
    if (s2[0])
        draw_line(px, w, h, s2, L->fs_sub, L->base_s2, 0.75f);
}

/* ------------------------------------------------------------------ */
/* SHM-буфер                                                          */
/* ------------------------------------------------------------------ */
static int shm_alloc(size_t size)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir)
        dir = "/tmp";
    char path[256];
    snprintf(path, sizeof(path), "%s/rsk_osd-XXXXXX", dir);
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------ */
/* Показ плашки (блокирующий, show_ms миллисекунд)                     */
/* ------------------------------------------------------------------ */
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int pump(struct app *a, int timeout_ms)
{
    wl_display_flush(a->dpy);
    struct pollfd p = { wl_display_get_fd(a->dpy), POLLIN, 0 };
    int r = poll(&p, 1, timeout_ms);
    if (r < 0)
        return (errno == EINTR) ? 0 : -1;
    if (r > 0) {
        if (wl_display_dispatch(a->dpy) == -1)
            return -1;
    } else {
        if (wl_display_dispatch_pending(a->dpy) == -1)
            return -1;
    }
    return 0;
}

static volatile sig_atomic_t g_quit = 0;

/* Полное уничтожение оверлея (только при выходе). */
static void osd_teardown(struct app *a)
{
    if (a->lsurf) {
        zwlr_layer_surface_v1_destroy(a->lsurf);
        a->lsurf = NULL;
    }
    if (a->surf) {
        wl_surface_destroy(a->surf);
        a->surf = NULL;
    }
    if (a->buf) {
        wl_buffer_destroy(a->buf);
        a->buf = NULL;
    }
    if (a->pool) {
        wl_shm_pool_destroy(a->pool);
        a->pool = NULL;
    }
    if (a->px) {
        munmap(a->px, a->pxsz);
        a->px = NULL;
    }
    if (a->base) {
        free(a->base);
        a->base = NULL;
    }
    if (a->shm_fd >= 0) {
        close(a->shm_fd);
        a->shm_fd = -1;
    }
    a->configured = false;
    a->closed = false;
    a->ost = OS_HIDDEN;
}

/* Масштабировать кадр на фактор f (premultiplied — линейно корректно). */
static void osd_apply_fade(struct app *a, float f)
{
    if (f < 0.0f)
        f = 0.0f;
    if (f > 1.0f)
        f = 1.0f;
    uint32_t *px = a->px;
    size_t n = (size_t)a->w * (size_t)a->h;
    for (size_t i = 0; i < n; i++) {
        uint32_t b = a->base[i];
        px[i] = ((uint32_t)(((b >> 24) & 0xFF) * f + 0.5f) << 24) |
                ((uint32_t)(((b >> 16) & 0xFF) * f + 0.5f) << 16) |
                ((uint32_t)(((b >> 8) & 0xFF) * f + 0.5f) << 8) |
                (uint32_t)((b & 0xFF) * f + 0.5f);
    }
    wl_surface_damage(a->surf, 0, 0, a->w, a->h);
    wl_surface_commit(a->surf);
}

/* Построить строки режима. */
static void mode_strings(powmode_t m, char *s1, size_t n1, char *s2, size_t n2)
{
    struct mode_info mi = mode_info(m);
    if (mi.boost != mi.ac)
        snprintf(s1, n1, "сеть: %d Вт · буст %d Вт", mi.ac, mi.boost);
    else
        snprintf(s1, n1, "сеть: %d Вт", mi.ac);
    snprintf(s2, n2, "батарея: %d–%d Вт", mi.bat_lo, mi.bat_hi);
}

static int osd_scale(struct app *a)
{
    int s = a->out_scale > 0 ? a->out_scale : 1;
    return (s > 3) ? 3 : s;
}

/* Одноразовое создание поверхности с ФИКСИРОВАННОЙ геометрией
   (максимум по всем трём режимам). Дальше окно не пересоздаётся —
   KWin не играет open-анимацию, размер не прыгает. */
static int osd_setup(struct app *a)
{
    int scale = osd_scale(a);
    struct osd_layout L;
    char s1[64], s2[64];
    a->w = 0;
    a->h = 0;
    for (int m = M_LOW; m <= M_TURBO; m++) {
        mode_strings((powmode_t)m, s1, sizeof(s1), s2, sizeof(s2));
        layout_osd(&L, mode_name((powmode_t)m), s1, s2, scale);
        if (L.w > a->w)
            a->w = L.w;
        if (L.h > a->h)
            a->h = L.h;
    }
    int stride = a->w * 4;
    a->pxsz = (size_t)stride * (size_t)a->h;

    a->base = malloc(a->pxsz);
    if (!a->base) {
        fprintf(stderr, "rsk_osd: out of memory\n");
        return -1;
    }
    memset(a->base, 0, a->pxsz);
    a->shm_fd = shm_alloc(a->pxsz);
    if (a->shm_fd < 0) {
        fprintf(stderr, "rsk_osd: shm_alloc failed\n");
        free(a->base);
        a->base = NULL;
        return -1;
    }
    a->px = mmap(NULL, a->pxsz, PROT_READ | PROT_WRITE, MAP_SHARED, a->shm_fd, 0);
    if (a->px == MAP_FAILED) {
        fprintf(stderr, "rsk_osd: mmap failed\n");
        a->px = NULL;
        osd_teardown(a);
        return -1;
    }
    memset(a->px, 0, a->pxsz); /* стартуем с прозрачности */
    a->pool = wl_shm_create_pool(a->shm, a->shm_fd, (int32_t)a->pxsz);
    a->buf = wl_shm_pool_create_buffer(a->pool, 0, a->w, a->h, stride,
                                       WL_SHM_FORMAT_ARGB8888);

    a->surf = wl_compositor_create_surface(a->comp);
    wl_surface_set_buffer_scale(a->surf, scale);
    a->lsurf = zwlr_layer_shell_v1_get_layer_surface(
        a->layer, a->surf, a->out,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "rsk-osd");
    zwlr_layer_surface_v1_add_listener(a->lsurf, &lsurf_listener, a);
    zwlr_layer_surface_v1_set_size(a->lsurf, a->w / scale, a->h / scale);
    zwlr_layer_surface_v1_set_anchor(
        a->lsurf, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
    zwlr_layer_surface_v1_set_exclusive_zone(a->lsurf, 0);
    int margin_bottom = (a->out_h > 0) ? a->out_h / 16 : 68;
    zwlr_layer_surface_v1_set_margin(a->lsurf, 0, 0, margin_bottom, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        a->lsurf, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(a->surf);

    a->ost = OS_HIDDEN;
    int64_t end = now_ms() + 1500;
    while (!a->configured && !a->closed && !g_quit && now_ms() < end) {
        if (pump(a, 50) < 0)
            break;
    }
    if (!a->configured) {
        fprintf(stderr, "rsk_osd: no configure from compositor\n");
        osd_teardown(a);
        return -1;
    }
    return 0;
}

/* Показать режим: только перерисовка в тот же буфер + fade-in.
   Прерывает любое текущее состояние мгновенно (без очереди). */
static void osd_show(struct app *a, powmode_t m)
{
    int scale = osd_scale(a);
    char s1[64], s2[64];
    mode_strings(m, s1, sizeof(s1), s2, sizeof(s2));
    struct osd_layout L;
    layout_osd(&L, mode_name(m), s1, s2, scale);
    L.w = a->w; /* геометрия фиксирована */
    L.h = a->h;
    render_osd(a->base, &L, mode_name(m), s1, s2, scale);
    a->ost = OS_FADEIN;
    a->t0 = now_ms();
}

/* Один кадр анимации. Вызывать часто (каждые ~40мс). */
static void osd_frame(struct app *a)
{
    if (a->ost == OS_HIDDEN || !a->surf)
        return;
    int64_t n = now_ms();
    if (a->ost == OS_FADEIN) {
        float f = (float)(n - a->t0) / (float)FADE_MS;
        if (f >= 1.0f) {
            f = 1.0f;
            a->ost = OS_SHOWN;
        }
        osd_apply_fade(a, f);
    } else if (a->ost == OS_SHOWN) {
        if (n >= a->hold_until) {
            a->ost = OS_FADEOUT;
            a->t0 = n;
        }
    } else if (a->ost == OS_FADEOUT) {
        float f = 1.0f - (float)(n - a->t0) / (float)FADE_MS;
        if (f <= 0.0f) {
            osd_apply_fade(a, 0.0f);
            a->ost = OS_HIDDEN;
            return;
        }
        osd_apply_fade(a, f);
    }
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */
static void on_sig(int s)
{
    (void)s;
    g_quit = 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --once            print current mode and exit\n"
            "  --demo MODE       show OSD once (low|balanced|turbo) and exit\n"
            "  --no-gui          poll EC only, no overlay (debug)\n"
            "  --poll-ms N       EC poll interval (default 100)\n"
            "  --show-ms N       OSD visible time (default 1000)\n"
            "  --wait-session    wait for Wayland session socket\n"
            "                    (for systemd service at boot)\n",
            argv0);
}

int main(int argc, char **argv)
{
    /* Сессия для Wayland: чиним окружение ПЕРЕД чем-либо ещё
       (sudo сносит WAYLAND_DISPLAY/XDG_RUNTIME_DIR). */
    fix_wayland_env();

    bool opt_once = false, opt_nogui = false, opt_wait = false;
    powmode_t opt_demo = M_UNKNOWN;
    bool opt_demo_set = false;
    int poll_ms = 100, show_ms = 1000;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--once"))
            opt_once = true;
        else if (!strcmp(argv[i], "--no-gui"))
            opt_nogui = true;
        else if (!strcmp(argv[i], "--demo") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "low") || !strcmp(v, "1"))
                opt_demo = M_LOW;
            else if (!strcmp(v, "balanced") || !strcmp(v, "2"))
                opt_demo = M_BAL;
            else if (!strcmp(v, "turbo") || !strcmp(v, "3"))
                opt_demo = M_TURBO;
            else {
                fprintf(stderr, "bad --demo mode: %s\n", v);
                return 2;
            }
            opt_demo_set = true;
        } else if (!strcmp(argv[i], "--wait-session")) {
            opt_wait = true;
        } else if (!strcmp(argv[i], "--poll-ms") && i + 1 < argc) {
            poll_ms = atoi(argv[++i]);
            if (poll_ms < 40)
                poll_ms = 40;
        } else if (!strcmp(argv[i], "--show-ms") && i + 1 < argc) {
            show_ms = atoi(argv[++i]);
            if (show_ms < 500)
                show_ms = 500;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    /* ec_sys для чтения EC (только чтение, запись запрещена). */
    if (access(EC_IO, R_OK) != 0) {
        if (system("modprobe ec_sys >/dev/null 2>&1") != 0)
            (void)0;
    }

    if (!opt_once && !opt_demo_set) {
        int lockfd = open("/tmp/rsk_osd.lock", O_CREAT | O_RDWR, 0644);
        if (lockfd >= 0) {
            if (flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
                fprintf(stderr, "rsk_osd: already running\n");
                return 1;
            }
        }
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    if (stbtt_InitFont(&g_font, rsk_font_bold,
                       stbtt_GetFontOffsetForIndex(rsk_font_bold, 0)) == 0) {
        fprintf(stderr, "rsk_osd: bad embedded font\n");
        return 1;
    }
    g_font_ok = true;

    struct app a;
    memset(&a, 0, sizeof(a));
    a.shm_fd = -1;
    a.out_scale = 1;
    bool gui = !opt_once && !opt_nogui;

    /* Режим сервиса: на загрузке сессии ещё нет — ждём сокет Wayland,
       а не падаем (systemd всё равно перезапустит, но так чище). */
    if (gui && opt_wait && !getenv("WAYLAND_DISPLAY")) {
        printf("rsk_osd: waiting for Wayland session...\n");
        fflush(stdout);
        while (!g_quit && !getenv("WAYLAND_DISPLAY")) {
            struct timespec ts = { 2, 0 };
            nanosleep(&ts, NULL);
            fix_wayland_env();
        }
        if (g_quit)
            return 0;
        printf("rsk_osd: session found (WAYLAND_DISPLAY=%s)\n",
               getenv("WAYLAND_DISPLAY"));
        fflush(stdout);
    }

    if (gui) {
        a.dpy = wl_display_connect(NULL);
        if (!a.dpy) {
            fprintf(stderr,
                    "rsk_osd: cannot connect to Wayland display "
                    "(WAYLAND_DISPLAY=%s). Run inside user session.\n",
                    getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
            return 1;
        }
        a.reg = wl_display_get_registry(a.dpy);
        wl_registry_add_listener(a.reg, &reg_listener, &a);
        wl_display_roundtrip(a.dpy);
        wl_display_roundtrip(a.dpy);
        if (!a.comp || !a.shm || !a.layer) {
            fprintf(stderr, "rsk_osd: compositor lacks layer-shell support\n");
            return 1;
        }
    }

    if (opt_demo_set) {
        if (!gui) {
            fprintf(stderr, "--demo needs GUI\n");
            return 2;
        }
        printf("rsk_osd: demo %s\n", mode_name(opt_demo));
        int rc = 0;
        if (osd_setup(&a) != 0) {
            wl_display_disconnect(a.dpy);
            return 1;
        }
        osd_show(&a, opt_demo);
        a.hold_until = now_ms() + show_ms;
        int64_t cap = now_ms() + 6000;
        while (a.ost != OS_HIDDEN && now_ms() < cap && !g_quit) {
            if (pump(&a, 40) < 0) {
                rc = 1;
                break;
            }
            osd_frame(&a);
        }
        osd_teardown(&a);
        wl_display_flush(a.dpy);
        wl_display_disconnect(a.dpy);
        return rc;
    }

    powmode_t cur = detect_mode();
    printf("rsk_osd: start, mode=%s(%d)\n", mode_name(cur), (int)cur);
    fflush(stdout);

    if (opt_once)
        return (cur == M_UNKNOWN) ? 1 : 0;

    if (gui) {
        if (osd_setup(&a) != 0)
            return 1;
        /* Стартовый показ текущего режима (обратная связь, что демон жив). */
        if (cur != M_UNKNOWN) {
            osd_show(&a, cur);
            a.hold_until = now_ms() + show_ms;
        }
    }

    /* Главный цикл: тик 40мс (анимация), опрос EC каждые poll_ms.
       Смена режима МГНОВЕННО сносит текущий тост и показывает новый —
       никакой очереди. */
    int64_t last_poll = 0;
    while (!g_quit) {
        if (gui) {
            if (pump(&a, 40) < 0) {
                fprintf(stderr, "rsk_osd: compositor lost, exiting\n");
                break;
            }
        } else {
            struct timespec ts = { 0, 40 * 1000000L };
            nanosleep(&ts, NULL);
            if (g_quit)
                break;
        }
        int64_t n = now_ms();
        if (n - last_poll >= poll_ms) {
            last_poll = n;
            powmode_t m = detect_mode();
            if (m != M_UNKNOWN && m != cur) {
                cur = m;
                printf("rsk_osd: mode -> %s(%d)\n", mode_name(cur), (int)cur);
                fflush(stdout);
                if (gui) {
                    /* Прерывание: сносим текущий кадр и сразу рисуем
                       новый в ТО ЖЕ окно — без очереди и без пересоздания. */
                    osd_show(&a, cur);
                    a.hold_until = now_ms() + show_ms;
                }
            }
        }
        if (gui)
            osd_frame(&a);
    }

    if (gui)
        osd_teardown(&a);
    printf("rsk_osd: stop\n");
    if (gui) {
        wl_display_flush(a.dpy);
        wl_display_disconnect(a.dpy);
    }
    return 0;
}
