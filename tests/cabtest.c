/*
 * Тесты записи CAB: архивы создаются библиотекой, распаковываются внешними
 * распаковщиками и побайтно сверяются с исходными данными.
 *
 *   Windows: FDI (cabinet.dll) и expand.exe -F:*
 *   Linux:   cabextract (libmspack); путь можно задать переменной
 *            окружения LZX_CABEXTRACT
 *
 * Запуск: cabtest <сценарий> <рабочая директория>
 *
 * Copyright (C) 2026 roosslan
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option) any
 * later version.
 *
 * This file is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this file; if not, see https://www.gnu.org/licenses/.
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cabwriter.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <fcntl.h>
#  include <io.h>
#  include <sys/stat.h>
#  include <fdi.h>
#  include <direct.h>
#  include <wchar.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

/******************************************************************************/
/*                               Общее                                        */
/******************************************************************************/

static int failures;

#define CHECK(cond, ...)                                                \
        do {                                                            \
                if (!(cond)) {                                          \
                        fprintf(stderr, "ОШИБКА %s:%d: ", __FILE__,     \
                                __LINE__);                              \
                        fprintf(stderr, __VA_ARGS__);                   \
                        fprintf(stderr, "\n");                          \
                        failures++;                                     \
                }                                                       \
        } while (0)

static void *
xmalloc(size_t n)
{
        void *p = malloc(n ? n : 1);

        if (!p) {
                fprintf(stderr, "недостаточно памяти\n");
                exit(2);
        }
        return p;
}

static char *
xstrdup(const char *s)
{
        size_t n = strlen(s) + 1;
        char *d = (char *)xmalloc(n);

        memcpy(d, s, n);
        return d;
}

static char *
strfmt(const char *fmt, const char *a, const char *b)
{
        size_t n = strlen(fmt) + strlen(a) + (b ? strlen(b) : 0) + 1;
        char *r = (char *)xmalloc(n);

        snprintf(r, n, fmt, a, b ? b : "");
        return r;
}

/* Детерминированный генератор псевдослучайных чисел (xorshift64*) */
static uint64_t rng_state;

static void
rng_seed(uint64_t seed)
{
        rng_state = seed * 0x9E3779B97F4A7C15ull + 1;
}

static uint64_t
rng_next(void)
{
        rng_state ^= rng_state >> 12;
        rng_state ^= rng_state << 25;
        rng_state ^= rng_state >> 27;
        return rng_state * 0x2545F4914F6CDD1Dull;
}

static uint32_t
rng_below(uint32_t n)
{
        return (uint32_t)(rng_next() % n);
}

/******************************************************************************/
/*                          Файловая система                                  */
/******************************************************************************/

#ifdef _WIN32
static wchar_t *
widen(const char *s)
{
        int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
        wchar_t *w = (wchar_t *)xmalloc((size_t)n * sizeof(wchar_t));

        MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
        return w;
}

static char *
narrow(const wchar_t *w)
{
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
        char *s = (char *)xmalloc((size_t)n);

        WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
        return s;
}
#endif

static FILE *
fopen_utf8(const char *path, const char *mode)
{
#ifdef _WIN32
        wchar_t *wp = widen(path);
        wchar_t *wm = widen(mode);
        FILE *f = _wfopen(wp, wm);

        free(wp);
        free(wm);
        return f;
#else
        return fopen(path, mode);
#endif
}

static int
file_exists(const char *path)
{
#ifdef _WIN32
        wchar_t *wp = widen(path);
        int r = _waccess(wp, 0) == 0;

        free(wp);
        return r;
#else
        return access(path, F_OK) == 0;
#endif
}

static int64_t
file_size(const char *path)
{
#ifdef _WIN32
        struct _stat64 st;
        wchar_t *wp = widen(path);
        int r = _wstat64(wp, &st);

        free(wp);
        return r == 0 ? (int64_t)st.st_size : -1;
#else
        struct stat st;

        return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
#endif
}

static void
make_dir(const char *path)
{
#ifdef _WIN32
        wchar_t *wp = widen(path);

        _wmkdir(wp);
        free(wp);
#else
        mkdir(path, 0777);
#endif
}

/* Создаёт все родительские директории пути */
static void
make_parents(const char *path)
{
        char *p = xstrdup(path);
        char *s;

        for (s = p + 1; *s; s++) {
                if (*s == '/' || *s == '\\') {
                        char c = *s;

                        *s = '\0';
                        make_dir(p);
                        *s = c;
                }
        }
        free(p);
}

/* Рекурсивно удаляет директорию */
static void
remove_tree(const char *path)
{
#ifdef _WIN32
        wchar_t *wp = widen(path);
        size_t n = wcslen(wp);
        wchar_t *pattern = (wchar_t *)xmalloc((n + 3) * sizeof(wchar_t));
        struct _wfinddata64_t fd;
        intptr_t h;

        wcscpy(pattern, wp);
        wcscat(pattern, L"\\*");
        h = _wfindfirst64(pattern, &fd);
        if (h != -1) {
                do {
                        char *name;
                        char *child;

                        if (!wcscmp(fd.name, L".") || !wcscmp(fd.name, L".."))
                                continue;
                        name = narrow(fd.name);
                        child = strfmt("%s\\%s", path, name);
                        if (fd.attrib & _A_SUBDIR) {
                                remove_tree(child);
                        } else {
                                wchar_t *wc = widen(child);

                                _wchmod(wc, _S_IREAD | _S_IWRITE);
                                _wunlink(wc);
                                free(wc);
                        }
                        free(child);
                        free(name);
                } while (_wfindnext64(h, &fd) == 0);
                _findclose(h);
        }
        _wrmdir(wp);
        free(pattern);
        free(wp);
#else
        DIR *d = opendir(path);
        struct dirent *de;

        if (d) {
                while ((de = readdir(d)) != NULL) {
                        char *child;
                        struct stat st;

                        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                                continue;
                        child = strfmt("%s/%s", path, de->d_name);
                        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
                                remove_tree(child);
                        else
                                unlink(child);
                        free(child);
                }
                closedir(d);
        }
        rmdir(path);
#endif
}

/* Побайтное сравнение двух файлов */
static int
files_equal(const char *a, const char *b)
{
        FILE *fa = fopen_utf8(a, "rb");
        FILE *fb = fopen_utf8(b, "rb");
        static uint8_t ba[1 << 16];
        static uint8_t bb[1 << 16];
        int equal = 1;

        if (!fa || !fb) {
                equal = 0;
        } else {
                for (;;) {
                        size_t na = fread(ba, 1, sizeof(ba), fa);
                        size_t nb = fread(bb, 1, sizeof(bb), fb);

                        if (na != nb || memcmp(ba, bb, na) != 0) {
                                equal = 0;
                                break;
                        }
                        if (na == 0)
                                break;
                }
        }
        if (fa)
                fclose(fa);
        if (fb)
                fclose(fb);
        return equal;
}

/******************************************************************************/
/*                           Генераторы данных                                */
/******************************************************************************/

enum gen_kind {
        GEN_EMPTY,
        GEN_TEXT,       /* обычный текст */
        GEN_LOG,        /* сильно повторяющиеся строки журнала */
        GEN_RANDOM,     /* несжимаемые данные */
        GEN_X86,        /* «машинный код» с частыми инструкциями E8 */
        GEN_ZEROS,
        GEN_PERIODIC,   /* случайный блок длиной param, повторяемый с периодом param */
        GEN_COPY,       /* копия существующего файла (path) */
};

static const char *words[] = {
        "архив", "сжатие", "окно", "блок", "файл", "том", "папка", "данные",
        "the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog",
        "compression", "cabinet", "folder", "volume", "LZX", "Huffman",
        "match", "literal", "offset", "length", "window", "stream",
};

static const char *log_levels[] = { "INFO", "WARN", "DEBUG", "ERROR" };

static void
gen_write(FILE *f, enum gen_kind kind, uint64_t size, uint32_t param,
          uint64_t seed)
{
        uint8_t *buf = (uint8_t *)xmalloc(1 << 16);
        uint8_t *period = NULL;
        uint64_t done = 0;
        uint64_t line = 0;

        rng_seed(seed);
        if (kind == GEN_PERIODIC) {
                uint32_t i;

                period = (uint8_t *)xmalloc(param);
                for (i = 0; i < param; i++)
                        period[i] = (uint8_t)rng_next();
        }

        while (done < size) {
                size_t n = 0;
                size_t want = (size - done < (1 << 16)) ? (size_t)(size - done)
                                                        : (1 << 16);

                switch (kind) {
                case GEN_TEXT:
                        while (n < want) {
                                const char *w = words[rng_below(
                                        sizeof(words) / sizeof(words[0]))];
                                size_t k = strlen(w);

                                if (n + k + 1 > want)
                                        k = want - n - 1;
                                memcpy(buf + n, w, k);
                                n += k;
                                if (n < want)
                                        buf[n++] = rng_below(12) ? ' ' : '\n';
                        }
                        break;
                case GEN_LOG:
                        while (n < want) {
                                char tmp[160];
                                int k = snprintf(
                                        tmp, sizeof(tmp),
                                        "2026-10-16 12:%02u:%02u.%03u [%s] "
                                        "worker-%u: request %llu processed "
                                        "in %u ms, status=200\n",
                                        (unsigned)(line / 60000 % 60),
                                        (unsigned)(line / 1000 % 60),
                                        (unsigned)(line % 1000),
                                        log_levels[rng_below(8) ? 0
                                                   : rng_below(4)],
                                        (unsigned)rng_below(8),
                                        (unsigned long long)line,
                                        (unsigned)rng_below(50));
                                size_t kk = (size_t)k;

                                if (n + kk > want)
                                        kk = want - n;
                                memcpy(buf + n, tmp, kk);
                                n += kk;
                                line++;
                        }
                        break;
                case GEN_RANDOM:
                        for (; n < want; n++)
                                buf[n] = (uint8_t)rng_next();
                        break;
                case GEN_X86:
                        /* Короткие «инструкции» и вызовы CALL rel32 к
                         * небольшому набору целей: после E8-трансляции
                         * адреса совпадают, что и даёт выигрыш. */
                        while (n < want) {
                                uint32_t r = rng_below(16);

                                if (r < 5 && n + 5 <= want) {
                                        uint64_t pos = done + n;
                                        int32_t target = (int32_t)(
                                                rng_below(64) * 4096);
                                        int32_t rel = target - (int32_t)pos - 5;

                                        buf[n++] = 0xE8;
                                        memcpy(buf + n, &rel, 4);
                                        n += 4;
                                } else if (r < 10) {
                                        static const uint8_t ops[] = {
                                                0x48, 0x89, 0xC3, 0x8B, 0x45,
                                                0xFC, 0x90, 0xC3, 0x55, 0x5D
                                        };
                                        buf[n++] = ops[rng_below(sizeof(ops))];
                                } else {
                                        buf[n++] = (uint8_t)rng_below(32);
                                }
                        }
                        break;
                case GEN_ZEROS:
                        memset(buf, 0, want);
                        n = want;
                        break;
                case GEN_PERIODIC:
                        for (; n < want; n++)
                                buf[n] = period[(done + n) % param];
                        break;
                default:
                        n = want;
                        break;
                }
                fwrite(buf, 1, n, f);
                done += n;
        }
        free(period);
        free(buf);
}

/******************************************************************************/
/*                               Набор файлов                                 */
/******************************************************************************/

struct tfile {
        char *name;     /* имя в архиве */
        char *src;      /* исходный файл на диске */
        uint64_t size;
};

struct dataset {
        struct tfile files[256];
        int count;
        char *src_dir;
};

static void
ds_add(struct dataset *ds, const char *name, enum gen_kind kind,
       uint64_t size, uint32_t param, const char *copy_from)
{
        struct tfile *t = &ds->files[ds->count];
        char idx[32];
        FILE *f;

        snprintf(idx, sizeof(idx), "f%03d", ds->count);
        t->name = xstrdup(name);
        t->src = strfmt("%s/%s", ds->src_dir, idx);
        if (kind == GEN_COPY) {
                FILE *in = fopen_utf8(copy_from, "rb");
                uint8_t buf[65536];
                size_t n;

                f = fopen_utf8(t->src, "wb");
                if (in && f) {
                        while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
                                fwrite(buf, 1, n, f);
                }
                if (in)
                        fclose(in);
        } else {
                f = fopen_utf8(t->src, "wb");
                if (f)
                        gen_write(f, kind, size, param,
                                  (uint64_t)ds->count * 7919 + size);
        }
        if (!f) {
                fprintf(stderr, "не удалось создать %s\n", t->src);
                exit(2);
        }
        fclose(f);
        t->size = (uint64_t)file_size(t->src);
        ds->count++;
}

/* Системный файл, если есть (для проверки на настоящем машинном коде) */
static void
ds_add_system(struct dataset *ds, const char *name, const char *path)
{
        if (file_exists(path))
                ds_add(ds, name, GEN_COPY, 0, 0, path);
        else
                fprintf(stderr, "(нет %s, пропущен)\n", path);
}

static void
ds_mixed(struct dataset *ds)
{
        ds_add(ds, "readme.txt", GEN_TEXT, 300000, 0, NULL);
        ds_add(ds, "data\\random.bin", GEN_RANDOM, 100000, 0, NULL);
        ds_add(ds, "data\\code.bin", GEN_X86, 200000, 0, NULL);
        ds_add(ds, "data\\empty.dat", GEN_EMPTY, 0, 0, NULL);
        ds_add(ds, "one.txt", GEN_TEXT, 1, 0, NULL);
        ds_add(ds, "zeros\\deep\\zeros.bin", GEN_ZEROS, 500000, 0, NULL);
        ds_add(ds, "server.log", GEN_LOG, 400000, 0, NULL);
}

/******************************************************************************/
/*                             Создание архива                                */
/******************************************************************************/

struct build_opts {
        int compression;
        unsigned window;
        unsigned level;
        uint64_t volume_size;
        uint64_t reserve;
        uint64_t folder_threshold;
        int e8_disable;
        int push_api;           /* подавать данные порциями случайной длины */
        int unknown_size;       /* не сообщать размер файлов */
        int flush_every;        /* liblzx_cab_flush_folder после каждых N файлов */
        int cancel_after;       /* отменить после N событий прогресса */
        /* Не проверять через FDI: блоки CFDATA, разрезанные более чем на
         * два тома (том меньше блока), FDI не распаковывает даже в наборах,
         * созданных makecab; такие наборы проверяет cabextract */
        int skip_fdi;
};

static void
default_opts(struct build_opts *o)
{
        memset(o, 0, sizeof(*o));
        o->compression = LIBLZX_CAB_COMPRESS_LZX;
        o->window = 21;
        o->level = 50;
}

struct build_ctx {
        int events;
        int cancel_after;
};

static int
progress_cb(void *userdata, const liblzx_cab_progress_t *p)
{
        struct build_ctx *ctx = (struct build_ctx *)userdata;

        (void)p;
        ctx->events++;
        return ctx->cancel_after && ctx->events >= ctx->cancel_after;
}

/* Создаёт архив; возвращает код ошибки, тома — в vols */
static int
build_cab(const struct dataset *ds, const char *cab_path,
          const struct build_opts *o, char ***vols, unsigned *num_vols)
{
        liblzx_cab_properties_t props;
        liblzx_cab_writer_t *w;
        struct build_ctx ctx;
        int err;
        int i;

        memset(&ctx, 0, sizeof(ctx));
        ctx.cancel_after = o->cancel_after;

        liblzx_cab_init_properties(&props);
        props.cab_path = cab_path;
        props.compression = o->compression;
        props.lzx_window_bits = o->window;
        props.compression_level = o->level;
        props.volume_size = o->volume_size;
        props.first_volume_reserve = o->reserve;
        props.folder_size_threshold = o->folder_threshold;
        props.e8_disable = o->e8_disable;
        props.set_id = 1234;
        props.progress = progress_cb;
        props.userdata = &ctx;

        w = liblzx_cab_create(&props, &err);
        if (!w)
                return err;

        err = LIBLZX_CAB_OK;
        for (i = 0; i < ds->count && !err; i++) {
                liblzx_cab_file_info_t info;

                info.name = ds->files[i].name;
                info.attribs = LIBLZX_CAB_ATTRIB_ARCHIVE;
                info.dos_date = (uint16_t)((46 << 9) | (10 << 5) | 16);
                info.dos_time = (uint16_t)((12 << 11) | (30 << 5));
                info.size = o->unknown_size ? LIBLZX_CAB_SIZE_UNKNOWN
                                            : ds->files[i].size;

                if (!o->push_api) {
                        err = liblzx_cab_add_file_from_path(
                                w, ds->files[i].src, &info);
                } else {
                        FILE *f = fopen_utf8(ds->files[i].src, "rb");
                        uint8_t *buf = (uint8_t *)xmalloc(200000);

                        rng_seed(i);
                        err = liblzx_cab_begin_file(w, &info);
                        while (!err && f) {
                                size_t want = 1 + rng_below(rng_below(4) ? 70000
                                                                         : 20);
                                size_t n = fread(buf, 1, want, f);

                                if (n == 0)
                                        break;
                                err = liblzx_cab_write(w, buf, n);
                        }
                        if (!err)
                                err = liblzx_cab_end_file(w);
                        if (f)
                                fclose(f);
                        free(buf);
                }
                if (!err && o->flush_every && (i + 1) % o->flush_every == 0)
                        err = liblzx_cab_flush_folder(w);
        }
        if (!err)
                err = liblzx_cab_finish(w);

        *num_vols = 0;
        *vols = NULL;
        if (!err) {
                unsigned k;

                *num_vols = liblzx_cab_get_volume_count(w);
                *vols = (char **)xmalloc(*num_vols * sizeof(char *));
                for (k = 0; k < *num_vols; k++)
                        (*vols)[k] = xstrdup(liblzx_cab_get_volume_path(w, k));
        }
        liblzx_cab_destroy(w);
        return err;
}

/******************************************************************************/
/*                              Распаковка                                    */
/******************************************************************************/

/* Путь распакованного файла: имя из архива с разделителями платформы */
static char *
extracted_path(const char *dir, const char *name)
{
        char *p = strfmt("%s/%s", dir, name);
        char *s;

        for (s = p; *s; s++) {
#ifdef _WIN32
                if (*s == '/')
                        *s = '\\';
#else
                if (*s == '\\')
                        *s = '/';
#endif
        }
        return p;
}

static void
compare_tree(const struct dataset *ds, const char *dir, const char *tool)
{
        int i;
        int bad = 0;

        for (i = 0; i < ds->count; i++) {
                char *p = extracted_path(dir, ds->files[i].name);

                if (!file_exists(p)) {
                        CHECK(0, "%s: нет файла %s", tool, ds->files[i].name);
                        bad++;
                } else if (!files_equal(p, ds->files[i].src)) {
                        CHECK(0, "%s: файл %s отличается", tool,
                              ds->files[i].name);
                        bad++;
                }
                free(p);
                if (bad > 5)
                        break;
        }
        if (!bad)
                printf("  %s: %d файлов совпали\n", tool, ds->count);
}

#ifdef _WIN32

struct fdi_ctx {
        const char *out_dir;
        int copied;
};

static FNALLOC(fdi_alloc)
{
        return malloc(cb);
}

static FNFREE(fdi_free)
{
        free(pv);
}

static FNOPEN(fdi_open)
{
        return _open(pszFile, oflag, pmode);
}

static FNREAD(fdi_read)
{
        return (UINT)_read((int)hf, pv, cb);
}

static FNWRITE(fdi_write)
{
        return (UINT)_write((int)hf, pv, cb);
}

static FNCLOSE(fdi_close)
{
        return _close((int)hf);
}

static FNSEEK(fdi_seek)
{
        return _lseek((int)hf, dist, seektype);
}

static FNFDINOTIFY(fdi_notify)
{
        struct fdi_ctx *ctx = (struct fdi_ctx *)pfdin->pv;

        switch (fdint) {
        case fdintCOPY_FILE: {
                char *name;
                char *path;
                wchar_t *wpath;
                int fd;

                if (pfdin->attribs & _A_NAME_IS_UTF) {
                        name = xstrdup(pfdin->psz1);
                } else {
                        /* Имя в кодовой странице ANSI */
                        int n = MultiByteToWideChar(CP_ACP, 0, pfdin->psz1,
                                                    -1, NULL, 0);
                        wchar_t *w = (wchar_t *)xmalloc(n * sizeof(wchar_t));

                        MultiByteToWideChar(CP_ACP, 0, pfdin->psz1, -1, w, n);
                        name = narrow(w);
                        free(w);
                }
                path = extracted_path(ctx->out_dir, name);
                make_parents(path);
                wpath = widen(path);
                fd = _wopen(wpath, _O_BINARY | _O_CREAT | _O_TRUNC | _O_WRONLY,
                            _S_IREAD | _S_IWRITE);
                free(wpath);
                free(path);
                free(name);
                return fd;
        }
        case fdintCLOSE_FILE_INFO:
                _close((int)pfdin->hf);
                ctx->copied++;
                return TRUE;
        case fdintNEXT_CABINET:
                return 0;
        default:
                return 0;
        }
}

/* Распаковка через FDI.  FDICopy обрабатывает файлы, перечисленные в
 * указанном томе (дочитывая следующие тома для разорванных файлов), поэтому
 * вызывается для каждого тома; продолженные файлы FDI пропускает. */
static void
extract_fdi(const struct dataset *ds, char **vols, unsigned num_vols,
            const char *out_dir)
{
        ERF erf;
        HFDI h;
        struct fdi_ctx ctx;
        unsigned v;

        make_dir(out_dir);
        memset(&erf, 0, sizeof(erf));
        h = FDICreate(fdi_alloc, fdi_free, fdi_open, fdi_read, fdi_write,
                      fdi_close, fdi_seek, cpuUNKNOWN, &erf);
        CHECK(h != NULL, "FDICreate");
        if (!h)
                return;
        ctx.out_dir = out_dir;
        ctx.copied = 0;
        for (v = 0; v < num_vols; v++) {
                char dir[1024];
                const char *base = strrchr(vols[v], '/');
                const char *base2 = strrchr(vols[v], '\\');
                size_t dl;

                if (base2 > base)
                        base = base2;
                base = base ? base + 1 : vols[v];
                dl = (size_t)(base - vols[v]);
                memcpy(dir, vols[v], dl);
                dir[dl] = '\0';
                if (!FDICopy(h, (char *)base, dir, 0, fdi_notify, NULL,
                             &ctx)) {
                        CHECK(0, "FDICopy(%s): erfOper=%d", vols[v],
                              erf.erfOper);
                }
        }
        FDIDestroy(h);
        compare_tree(ds, out_dir, "FDI");
}

static void
extract_expand(const struct dataset *ds, const char *first_vol,
               const char *out_dir)
{
        char *cmd;
        wchar_t *wcmd;
        int rc;
        const char *sysroot = getenv("SystemRoot");

        make_dir(out_dir);
        /* Полный путь: в PATH может оказаться expand из coreutils */
        cmd = strfmt("%s\\System32\\expand.exe -F:* ",
                     sysroot ? sysroot : "C:\\Windows", NULL);
        {
                char *c2 = strfmt("%s\"%s\" ", cmd, first_vol);
                char *c3 = strfmt("%s\"%s\" >nul", c2, out_dir);

                free(cmd);
                free(c2);
                cmd = c3;
        }
        wcmd = widen(cmd);
        rc = _wsystem(wcmd);
        free(wcmd);
        free(cmd);
        CHECK(rc == 0, "expand.exe вернул %d", rc);
        compare_tree(ds, out_dir, "expand");
}

#else

static void
extract_cabextract(const struct dataset *ds, const char *first_vol,
                   const char *out_dir)
{
        const char *tool = getenv("LZX_CABEXTRACT");
        char *cmd;
        char *cmd2;
        int rc;

        if (!tool || !*tool)
                tool = "cabextract";
        make_dir(out_dir);
        cmd = strfmt("%s -q -d \"%s\"", tool, out_dir);
        cmd2 = strfmt("%s \"%s\"", cmd, first_vol);
        rc = system(cmd2);
        free(cmd);
        free(cmd2);
        CHECK(rc == 0, "cabextract вернул %d", rc);
        compare_tree(ds, out_dir, "cabextract");
}

#endif

/* Создаёт архив, проверяет размеры томов и распаковывает всеми средствами */
static void
build_and_verify(const struct dataset *ds, const char *work, const char *tag,
                 const struct build_opts *o)
{
        /* Каждый набор — в своей директории: имена томов по умолчанию
         * (<имя>N.cab) разных наборов иначе могли бы совпасть */
        char *dir = strfmt("%s/%s", work, tag);
        char *cab_path = strfmt("%s/%s.cab", dir, tag);
        char *tmp_path = strfmt("%s.tmp", cab_path, NULL);
        char *out;
        char **vols;
        unsigned num_vols;
        unsigned v;
        uint64_t total = 0;
        int err;
        clock_t t0 = clock();

        make_dir(dir);
        err = build_cab(ds, cab_path, o, &vols, &num_vols);
        CHECK(err == LIBLZX_CAB_OK, "%s: создание архива: %s", tag,
              liblzx_cab_strerror(err));
        if (err) {
                free(dir);
                free(cab_path);
                free(tmp_path);
                return;
        }
        for (v = 0; v < num_vols; v++) {
                int64_t size = file_size(vols[v]);
                uint64_t limit = o->volume_size ? o->volume_size
                                                : LIBLZX_CAB_MAX_VOLUME_SIZE;

                if (v == 0)
                        limit -= o->reserve;
                CHECK(size > 0 && (uint64_t)size <= limit,
                      "%s: том %u размером %lld превышает %llu", tag, v,
                      (long long)size, (unsigned long long)limit);
                total += (uint64_t)size;
        }
        printf("%s: томов %u, всего %llu байт, %.2f с\n", tag, num_vols,
               (unsigned long long)total,
               (double)(clock() - t0) / CLOCKS_PER_SEC);
        CHECK(!file_exists(tmp_path), "%s: остался временный файл", tag);

#ifdef _WIN32
        if (!o->skip_fdi) {
                out = strfmt("%s/%s", dir, "fdi");
                extract_fdi(ds, vols, num_vols, out);
                free(out);
        } else {
                printf("  FDI: пропущено (блоки разрезаны на 3+ тома)\n");
        }
        /* expand.exe не распаковывает многотомные наборы (ошибка 0x80070032
         * и на наборах, созданных makecab), поэтому проверяет только
         * однотомные архивы. */
        if (num_vols == 1) {
                out = strfmt("%s/%s", dir, "expand");
                extract_expand(ds, vols[0], out);
                free(out);
        }
#else
        out = strfmt("%s/%s", dir, "cabextract");
        extract_cabextract(ds, vols[0], out);
        free(out);
#endif
        for (v = 0; v < num_vols; v++)
                free(vols[v]);
        free(vols);
        free(dir);
        free(cab_path);
        free(tmp_path);
}

/******************************************************************************/
/*                               Сценарии                                     */
/******************************************************************************/

static void
scenario_basic(struct dataset *ds, const char *work)
{
        struct build_opts o;

        ds_mixed(ds);
        default_opts(&o);
        build_and_verify(ds, work, "lzx21", &o);
        o.push_api = 1;
        build_and_verify(ds, work, "lzx21push", &o);
        default_opts(&o);
        o.compression = LIBLZX_CAB_COMPRESS_NONE;
        build_and_verify(ds, work, "stored", &o);
        o.push_api = 1;
        build_and_verify(ds, work, "storedpush", &o);
}

/* Повторы на расстояниях около размера окна: смещение не должно
 * превышать window - 3 */
static void
scenario_windows(struct dataset *ds, const char *work)
{
        unsigned w;

        for (w = 15; w <= 21; w++) {
                uint32_t size = (uint32_t)1 << w;
                int d;

                for (d = -4; d <= 1; d++)
                        ds_add(ds, "x", GEN_PERIODIC,
                               (uint64_t)size * (w < 21 ? 3 : 2) + 1000,
                               (uint32_t)(size + d), NULL);
        }
        /* Уникальные имена */
        for (w = 0; w < (unsigned)ds->count; w++) {
                char name[32];

                free(ds->files[w].name);
                snprintf(name, sizeof(name), "period%02u.bin", w);
                ds->files[w].name = xstrdup(name);
        }
        for (w = 15; w <= 21; w++) {
                struct build_opts o;
                char tag[32];

                default_opts(&o);
                o.window = w;
                o.level = 20;
                snprintf(tag, sizeof(tag), "w%u_lazy", w);
                build_and_verify(ds, work, tag, &o);
                if (w == 15 || w == 16 || w == 18 || w == 21) {
                        o.level = 50;
                        snprintf(tag, sizeof(tag), "w%u_opt", w);
                        build_and_verify(ds, work, tag, &o);
                }
        }
}

static void
scenario_levels(struct dataset *ds, const char *work)
{
        static const unsigned levels[] = { 1, 10, 34, 35, 70, 100, 300 };
        size_t i;

        ds_mixed(ds);
        for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
                struct build_opts o;
                char tag[32];

                default_opts(&o);
                o.level = levels[i];
                o.window = (i % 2) ? 17 : 21;
                snprintf(tag, sizeof(tag), "level%u_w%u", levels[i], o.window);
                build_and_verify(ds, work, tag, &o);
        }
}

static void
scenario_multivolume(struct dataset *ds, const char *work)
{
        struct build_opts o;

        ds_mixed(ds);
        ds_add(ds, "big\\text.txt", GEN_TEXT, 1500000, 0, NULL);
        ds_add(ds, "big\\random.bin", GEN_RANDOM, 600000, 0, NULL);
        ds_add(ds, "tail.txt", GEN_TEXT, 1000, 0, NULL);

        default_opts(&o);
        o.volume_size = 65536;
        build_and_verify(ds, work, "mv_lzx", &o);
        o.window = 15;
        o.level = 20;
        build_and_verify(ds, work, "mv_lzx15", &o);
        default_opts(&o);
        o.compression = LIBLZX_CAB_COMPRESS_NONE;
        o.volume_size = 300000;
        build_and_verify(ds, work, "mv_stored", &o);
        default_opts(&o);
        o.volume_size = 100000;
        o.folder_threshold = 200000;
        build_and_verify(ds, work, "mv_folders", &o);
}

/* Тома меньше одного блока: блоки разрезаются на несколько томов,
 * файлы проходят через тома целиком (0xFFFF) */
static void
scenario_tiny(struct dataset *ds, const char *work)
{
        struct build_opts o;

        ds_add(ds, "a.txt", GEN_TEXT, 30000, 0, NULL);
        ds_add(ds, "random.bin", GEN_RANDOM, 150000, 0, NULL);
        ds_add(ds, "b.txt", GEN_TEXT, 100, 0, NULL);
        ds_add(ds, "empty", GEN_EMPTY, 0, 0, NULL);
        ds_add(ds, "c.bin", GEN_X86, 70000, 0, NULL);

        default_opts(&o);
        o.volume_size = 6000;
        o.skip_fdi = 1;
        build_and_verify(ds, work, "tiny_lzx", &o);
        o.compression = LIBLZX_CAB_COMPRESS_NONE;
        o.volume_size = 9000;
        o.skip_fdi = 1;
        build_and_verify(ds, work, "tiny_stored", &o);
}

/* Уменьшенный первый том (самораспаковывающийся архив) */
static void
scenario_reserve(struct dataset *ds, const char *work)
{
        struct build_opts o;

        ds_mixed(ds);
        default_opts(&o);
        o.volume_size = 200000;
        o.reserve = 150000;
        build_and_verify(ds, work, "reserve", &o);
}

static void
scenario_names(struct dataset *ds, const char *work)
{
        struct build_opts o;

        ds_add(ds, "Документы\\отчёт за 2026 год.txt", GEN_TEXT, 50000, 0,
               NULL);
        ds_add(ds, "Документы\\Ёлка и ёж.bin", GEN_RANDOM, 20000, 0, NULL);
        ds_add(ds, "Проект/исходники/main.c", GEN_TEXT, 30000, 0, NULL);
        ds_add(ds, "日本語.txt", GEN_TEXT, 1000, 0, NULL);
        ds_add(ds, "latin.txt", GEN_TEXT, 1000, 0, NULL);
        /* «/» в имени заменяется на «\» */
        free(ds->files[2].name);
        ds->files[2].name = xstrdup("Проект\\исходники\\main.c");

        default_opts(&o);
        build_and_verify(ds, work, "names", &o);
        o.volume_size = 40000;
        build_and_verify(ds, work, "names_mv", &o);
}

static void
scenario_empty(struct dataset *ds, const char *work)
{
        struct build_opts o;
        struct dataset only;
        char *dir;

        /* Архив только из пустых файлов */
        memset(&only, 0, sizeof(only));
        only.src_dir = ds->src_dir;
        ds_add(&only, "empty1", GEN_EMPTY, 0, 0, NULL);
        ds_add(&only, "dir\\empty2", GEN_EMPTY, 0, 0, NULL);
        default_opts(&o);
        dir = strfmt("%s/only", work, NULL);
        make_dir(dir);
        build_and_verify(&only, dir, "only_empty", &o);
        free(dir);

        /* Пустые файлы в начале, середине и конце папок */
        ds->count = only.count; /* продолжить нумерацию исходных файлов */
        memcpy(ds->files, only.files, sizeof(only.files[0]) * only.count);
        ds_add(ds, "mid.txt", GEN_TEXT, 70000, 0, NULL);
        ds_add(ds, "empty3", GEN_EMPTY, 0, 0, NULL);
        ds_add(ds, "empty4", GEN_EMPTY, 0, 0, NULL);
        ds_add(ds, "exact.bin", GEN_RANDOM, 32768 * 2 - 70000 % 32768, 0,
               NULL);
        ds_add(ds, "empty5", GEN_EMPTY, 0, 0, NULL);
        default_opts(&o);
        build_and_verify(ds, work, "empty_mixed", &o);
        o.flush_every = 3;
        build_and_verify(ds, work, "empty_flush", &o);
        o.flush_every = 0;
        o.compression = LIBLZX_CAB_COMPRESS_NONE;
        build_and_verify(ds, work, "empty_stored", &o);
        default_opts(&o);
        o.volume_size = 50000;
        build_and_verify(ds, work, "empty_mv", &o);
}

/* Машинный код: E8-трансляция, файлы больше e8_file_size (12 Мб) */
static void
scenario_e8(struct dataset *ds, const char *work)
{
        struct build_opts o;

        ds_add(ds, "synthetic.exe", GEN_X86, 14 * 1024 * 1024, 0, NULL);
#ifdef _WIN32
        ds_add_system(ds, "sys\\notepad.exe", "C:\\Windows\\System32\\notepad.exe");
        ds_add_system(ds, "sys\\kernel32.dll", "C:\\Windows\\System32\\kernel32.dll");
        ds_add_system(ds, "sys\\shell32.dll", "C:\\Windows\\System32\\shell32.dll");
#else
        ds_add_system(ds, "sys/bash", "/bin/bash");
        ds_add_system(ds, "sys/libc.so.6", "/lib/x86_64-linux-gnu/libc.so.6");
#endif
        default_opts(&o);
        o.level = 20;
        build_and_verify(ds, work, "e8_on", &o);
        o.e8_disable = 1;
        build_and_verify(ds, work, "e8_off", &o);
}

/* Несжимаемые данные */
static void
scenario_random(struct dataset *ds, const char *work)
{
        struct build_opts o;

        ds_add(ds, "random1.bin", GEN_RANDOM, 3000000, 0, NULL);
        ds_add(ds, "text.txt", GEN_TEXT, 100000, 0, NULL);
        ds_add(ds, "random2.bin", GEN_RANDOM, 77777, 0, NULL);
        default_opts(&o);
        build_and_verify(ds, work, "random_lzx", &o);
        o.level = 10;
        o.window = 16;
        build_and_verify(ds, work, "random_lzx_fast", &o);
        default_opts(&o);
        o.volume_size = 500000;
        build_and_verify(ds, work, "random_mv", &o);
}

/* Неизвестный размер файлов: поле размера исправляется после файла */
static void
scenario_unknown_size(struct dataset *ds, const char *work)
{
        struct build_opts o;

        ds_mixed(ds);
        ds_add(ds, "big.bin", GEN_RANDOM, 400000, 0, NULL);
        default_opts(&o);
        o.push_api = 1;
        o.unknown_size = 1;
        o.volume_size = 50000;
        build_and_verify(ds, work, "unknown_mv", &o);
        o.compression = LIBLZX_CAB_COMPRESS_NONE;
        build_and_verify(ds, work, "unknown_mv_stored", &o);
}

/* Несколько папок: явный сброс и порог */
static void
scenario_folders(struct dataset *ds, const char *work)
{
        struct build_opts o;

        ds_mixed(ds);
        ds_mixed(ds);
        {
                int i;

                for (i = 7; i < 14; i++) {
                        char *n = strfmt("copy\\%s", ds->files[i].name, NULL);

                        free(ds->files[i].name);
                        ds->files[i].name = n;
                }
        }
        default_opts(&o);
        o.flush_every = 1;
        build_and_verify(ds, work, "folder_each", &o);
        default_opts(&o);
        o.folder_threshold = 250000;
        o.window = 18;
        build_and_verify(ds, work, "folder_threshold", &o);
}

/* Отмена через прогресс: архив и временный файл удаляются */
static void
scenario_cancel(struct dataset *ds, const char *work)
{
        static const int points[] = { 1, 5, 30, 60 };
        size_t i;

        ds_mixed(ds);
        ds_add(ds, "big.txt", GEN_TEXT, 2000000, 0, NULL);
        for (i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
                struct build_opts o;
                char *cab = strfmt("%s/cancel.cab", work, NULL);
                char *cab2 = strfmt("%s/cancel2.cab", work, NULL);
                char *tmp = strfmt("%s/cancel.cab.tmp", work, NULL);
                char **vols;
                unsigned nv;
                int err;

                default_opts(&o);
                o.volume_size = 100000;
                o.cancel_after = points[i];
                err = build_cab(ds, cab, &o, &vols, &nv);
                CHECK(err == LIBLZX_CAB_ERR_CANCELLED,
                      "отмена на событии %d: код %d", points[i], err);
                CHECK(!file_exists(cab) && !file_exists(cab2) &&
                      !file_exists(tmp),
                      "после отмены на событии %d остались файлы",
                      points[i]);
                free(cab);
                free(cab2);
                free(tmp);
        }
        printf("отмена: проверено %d точек\n",
               (int)(sizeof(points) / sizeof(points[0])));
}

/* Ошибки параметров и порядка вызовов */
static void
scenario_errors(struct dataset *ds, const char *work)
{
        liblzx_cab_properties_t props;
        liblzx_cab_writer_t *w;
        liblzx_cab_file_info_t info;
        char *cab = strfmt("%s/err.cab", work, NULL);
        char longname[300];
        int err;

        (void)ds;
        memset(&info, 0, sizeof(info));
        info.size = LIBLZX_CAB_SIZE_UNKNOWN;

        liblzx_cab_init_properties(&props);
        w = liblzx_cab_create(&props, &err);
        CHECK(!w && err == LIBLZX_CAB_ERR_INVALID_PARAM, "нет пути");

        props.cab_path = cab;
        props.lzx_window_bits = 22;
        w = liblzx_cab_create(&props, &err);
        CHECK(!w && err == LIBLZX_CAB_ERR_INVALID_PARAM, "окно 22");

        /* Пустой архив */
        liblzx_cab_init_properties(&props);
        props.cab_path = cab;
        w = liblzx_cab_create(&props, &err);
        CHECK(w != NULL, "create");
        CHECK(liblzx_cab_finish(w) == LIBLZX_CAB_ERR_EMPTY, "пустой архив");
        liblzx_cab_destroy(w);
        CHECK(!file_exists(cab), "пустой архив создан");

        /* write без begin_file */
        w = liblzx_cab_create(&props, &err);
        CHECK(liblzx_cab_write(w, "x", 1) == LIBLZX_CAB_ERR_BAD_STATE,
              "write без begin");
        CHECK(liblzx_cab_get_error(w) == LIBLZX_CAB_ERR_BAD_STATE,
              "состояние ошибки");
        liblzx_cab_destroy(w);

        /* Длинное имя и некорректный UTF-8 */
        memset(longname, 'a', 256);
        longname[256] = '\0';
        w = liblzx_cab_create(&props, &err);
        info.name = longname;
        CHECK(liblzx_cab_begin_file(w, &info) == LIBLZX_CAB_ERR_NAME,
              "длинное имя");
        liblzx_cab_destroy(w);
        w = liblzx_cab_create(&props, &err);
        info.name = "bad\xC3(utf8";
        CHECK(liblzx_cab_begin_file(w, &info) == LIBLZX_CAB_ERR_NAME,
              "некорректный UTF-8");
        liblzx_cab_destroy(w);
        longname[255] = '\0';
        w = liblzx_cab_create(&props, &err);
        info.name = longname;
        CHECK(liblzx_cab_begin_file(w, &info) == LIBLZX_CAB_OK,
              "имя из 255 байт");
        liblzx_cab_destroy(w);

        /* Несовпадение заявленного размера */
        w = liblzx_cab_create(&props, &err);
        info.name = "a";
        info.size = 10;
        CHECK(liblzx_cab_begin_file(w, &info) == LIBLZX_CAB_OK, "begin");
        CHECK(liblzx_cab_write(w, "12345", 5) == LIBLZX_CAB_OK, "write");
        CHECK(liblzx_cab_end_file(w) == LIBLZX_CAB_ERR_SIZE_MISMATCH,
              "меньше заявленного");
        liblzx_cab_destroy(w);
        w = liblzx_cab_create(&props, &err);
        info.size = 3;
        liblzx_cab_begin_file(w, &info);
        CHECK(liblzx_cab_write(w, "12345", 5) == LIBLZX_CAB_ERR_SIZE_MISMATCH,
              "больше заявленного");
        liblzx_cab_destroy(w);

        /* Файл больше папки */
        w = liblzx_cab_create(&props, &err);
        info.size = (uint64_t)3 << 30;
        CHECK(liblzx_cab_begin_file(w, &info) == LIBLZX_CAB_ERR_FILE_TOO_LARGE,
              "файл 3 Гб");
        liblzx_cab_destroy(w);

        /* Слишком маленький том */
        props.volume_size = 60;
        w = liblzx_cab_create(&props, &err);
        info.size = LIBLZX_CAB_SIZE_UNKNOWN;
        liblzx_cab_begin_file(w, &info);
        liblzx_cab_write(w, "12345", 5);
        liblzx_cab_end_file(w);
        CHECK(liblzx_cab_finish(w) == LIBLZX_CAB_ERR_VOLUME_TOO_SMALL,
              "маленький том");
        liblzx_cab_destroy(w);
        CHECK(!file_exists(cab), "остался том после ошибки");

        /* Резерв больше тома */
        props.volume_size = 1000;
        props.first_volume_reserve = 1000;
        w = liblzx_cab_create(&props, &err);
        liblzx_cab_begin_file(w, &info);
        liblzx_cab_write(w, "12345", 5);
        liblzx_cab_end_file(w);
        CHECK(liblzx_cab_finish(w) == LIBLZX_CAB_ERR_VOLUME_TOO_SMALL,
              "резерв не меньше тома");
        liblzx_cab_destroy(w);

        /* Нет каталога назначения */
        props.volume_size = 0;
        props.first_volume_reserve = 0;
        props.cab_path = "nonexistent-dir-xyz/sub/a.cab";
        w = liblzx_cab_create(&props, &err);
        liblzx_cab_begin_file(w, &info);
        CHECK(liblzx_cab_write(w, "12345", 5) == LIBLZX_CAB_OK, "write");
        CHECK(liblzx_cab_end_file(w) == LIBLZX_CAB_OK, "end");
        CHECK(liblzx_cab_finish(w) == LIBLZX_CAB_ERR_OPEN, "нет директории");
        liblzx_cab_destroy(w);

        free(cab);
        printf("ошибки: проверено\n");
}

struct scenario {
        const char *name;
        void (*fn)(struct dataset *, const char *);
};

static const struct scenario scenarios[] = {
        { "basic", scenario_basic },
        { "windows", scenario_windows },
        { "levels", scenario_levels },
        { "multivolume", scenario_multivolume },
        { "tiny", scenario_tiny },
        { "reserve", scenario_reserve },
        { "names", scenario_names },
        { "empty", scenario_empty },
        { "e8", scenario_e8 },
        { "random", scenario_random },
        { "unknown_size", scenario_unknown_size },
        { "folders", scenario_folders },
        { "cancel", scenario_cancel },
        { "errors", scenario_errors },
};

static int
run(int argc, char **argv)
{
        struct dataset *ds;
        size_t i;

        /* Без буферизации: при аварийном завершении вывод не теряется */
        setvbuf(stdout, NULL, _IONBF, 0);

#ifdef _WIN32
        /* Отладка: cabtest fdi <директория> <том1> [том2 ...] */
        if (argc >= 4 && !strcmp(argv[1], "fdi")) {
                struct dataset empty;

                memset(&empty, 0, sizeof(empty));
                extract_fdi(&empty, argv + 3, (unsigned)(argc - 3), argv[2]);
                return failures ? 1 : 0;
        }
#endif
        if (argc != 3) {
                fprintf(stderr, "Использование: cabtest <сценарий> <директория>\n"
                                "Сценарии:");
                for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
                        fprintf(stderr, " %s", scenarios[i].name);
                fprintf(stderr, "\n");
                return 2;
        }
        for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
                if (!strcmp(argv[1], scenarios[i].name))
                        break;
        }
        if (i == sizeof(scenarios) / sizeof(scenarios[0])) {
                fprintf(stderr, "неизвестный сценарий %s\n", argv[1]);
                return 2;
        }

        remove_tree(argv[2]);
        make_parents(argv[2]);
        make_dir(argv[2]);
        ds = (struct dataset *)xmalloc(sizeof(*ds));
        memset(ds, 0, sizeof(*ds));
        ds->src_dir = strfmt("%s/src", argv[2], NULL);
        make_dir(ds->src_dir);

        scenarios[i].fn(ds, argv[2]);

        {
                int k;

                for (k = 0; k < ds->count; k++) {
                        free(ds->files[k].name);
                        free(ds->files[k].src);
                }
                free(ds->src_dir);
                free(ds);
        }

        if (failures) {
                printf("%s: ошибок %d\n", argv[1], failures);
                return 1;
        }
        /* Рабочая директория удаляется только при успехе */
        remove_tree(argv[2]);
        printf("%s: успешно\n", argv[1]);
        return 0;
}

#ifdef _WIN32
int
wmain(int argc, wchar_t **wargv)
{
        char **argv = (char **)xmalloc((size_t)(argc + 1) * sizeof(char *));
        UINT old_cp = GetConsoleOutputCP();
        int i;
        int r;

        SetConsoleOutputCP(CP_UTF8);
        for (i = 0; i < argc; i++)
                argv[i] = narrow(wargv[i]);
        argv[argc] = NULL;
        r = run(argc, argv);
        SetConsoleOutputCP(old_cp);
        return r;
}
#else
int
main(int argc, char **argv)
{
        return run(argc, argv);
}
#endif
