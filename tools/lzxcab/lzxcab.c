/*
 * lzxcab — создание CAB-архива из списка файлов и директорий.
 * Пример использования библиотеки записи CAB (cab/cabwriter.h).
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
#  include <io.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <wchar.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

/* Элемент списка добавляемых файлов */
struct entry {
        char *src;       /* путь на диске (UTF-8) */
        char *name;      /* имя в архиве (UTF-8, разделитель «\») */
        uint64_t size;
        uint16_t attribs;
        uint16_t date;
        uint16_t time;
};

static struct entry *entries;
static size_t num_entries;
static size_t cap_entries;
static uint64_t total_size;
static int quiet;

static void *
xmalloc(size_t n)
{
        void *p = malloc(n ? n : 1);

        if (!p) {
                fprintf(stderr, "lzxcab: недостаточно памяти\n");
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
path_join(const char *a, const char *b, char sep)
{
        size_t na = strlen(a);
        size_t nb = strlen(b);
        char *r = (char *)xmalloc(na + nb + 2);

        memcpy(r, a, na);
        r[na] = sep;
        memcpy(r + na + 1, b, nb + 1);
        return r;
}

/* Дата и время MS-DOS из time_t (местное время) */
static void
to_dos_time(time_t t, uint16_t *date, uint16_t *dtime)
{
        struct tm tmv;
        struct tm *tm;

#ifdef _WIN32
        tm = localtime_s(&tmv, &t) == 0 ? &tmv : NULL;
#else
        tm = localtime_r(&t, &tmv);
#endif
        if (!tm || tm->tm_year < 80) {
                /* 1980-01-01 00:00:00 */
                *date = (uint16_t)((0 << 9) | (1 << 5) | 1);
                *dtime = 0;
                return;
        }
        *date = (uint16_t)(((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) |
                           tm->tm_mday);
        *dtime = (uint16_t)((tm->tm_hour << 11) | (tm->tm_min << 5) |
                            (tm->tm_sec / 2));
}

static void
add_entry(const char *src, const char *name, uint64_t size, uint16_t attribs,
          time_t mtime)
{
        struct entry *e;

        if (num_entries == cap_entries) {
                size_t new_cap = cap_entries ? cap_entries * 2 : 64;
                struct entry *n = (struct entry *)xmalloc(new_cap * sizeof(*n));

                if (entries) {
                        memcpy(n, entries, num_entries * sizeof(*n));
                        free(entries);
                }
                entries = n;
                cap_entries = new_cap;
        }
        e = &entries[num_entries++];
        e->src = xstrdup(src);
        e->name = xstrdup(name);
        e->size = size;
        e->attribs = attribs;
        to_dos_time(mtime, &e->date, &e->time);
        total_size += size;
}

static int
cmp_str(const void *a, const void *b)
{
        return strcmp(*(char *const *)a, *(char *const *)b);
}

#ifdef _WIN32

static char *
wide_to_utf8(const wchar_t *w)
{
        size_t len = 0;
        const wchar_t *p;
        char *out;
        char *q;

        for (p = w; *p; p++)
                len += 4;
        out = (char *)xmalloc(len + 1);
        q = out;
        for (p = w; *p; p++) {
                uint32_t cp = *p;

                if (cp >= 0xD800 && cp <= 0xDBFF && p[1] >= 0xDC00 &&
                    p[1] <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (p[1] - 0xDC00);
                        p++;
                }
                if (cp < 0x80) {
                        *q++ = (char)cp;
                } else if (cp < 0x800) {
                        *q++ = (char)(0xC0 | (cp >> 6));
                        *q++ = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                        *q++ = (char)(0xE0 | (cp >> 12));
                        *q++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                        *q++ = (char)(0x80 | (cp & 0x3F));
                } else {
                        *q++ = (char)(0xF0 | (cp >> 18));
                        *q++ = (char)(0x80 | ((cp >> 12) & 0x3F));
                        *q++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                        *q++ = (char)(0x80 | (cp & 0x3F));
                }
        }
        *q = '\0';
        return out;
}

static wchar_t *
utf8_to_wide(const char *s)
{
        size_t len = strlen(s);
        wchar_t *out = (wchar_t *)xmalloc((len + 1) * sizeof(wchar_t));
        const unsigned char *p = (const unsigned char *)s;
        wchar_t *q = out;

        while (*p) {
                uint32_t cp;
                int extra;

                if (*p < 0x80) {
                        cp = *p;
                        extra = 0;
                } else if ((*p & 0xE0) == 0xC0) {
                        cp = *p & 0x1F;
                        extra = 1;
                } else if ((*p & 0xF0) == 0xE0) {
                        cp = *p & 0x0F;
                        extra = 2;
                } else {
                        cp = *p & 0x07;
                        extra = 3;
                }
                p++;
                while (extra-- > 0 && (*p & 0xC0) == 0x80)
                        cp = (cp << 6) | (*p++ & 0x3F);
                if (cp >= 0x10000) {
                        cp -= 0x10000;
                        *q++ = (wchar_t)(0xD800 + (cp >> 10));
                        *q++ = (wchar_t)(0xDC00 + (cp & 0x3FF));
                } else {
                        *q++ = (wchar_t)cp;
                }
        }
        *q = 0;
        return out;
}

static uint16_t
win_attribs(unsigned attrib)
{
        return (uint16_t)(attrib & (_A_RDONLY | _A_HIDDEN | _A_SYSTEM |
                                    _A_ARCH));
}

static int
collect(const char *src, const char *name)
{
        wchar_t *wsrc = utf8_to_wide(src);
        struct _stat64 st;

        if (_wstat64(wsrc, &st) != 0) {
                fprintf(stderr, "lzxcab: не найден: %s\n", src);
                free(wsrc);
                return -1;
        }
        if (st.st_mode & _S_IFDIR) {
                wchar_t *pattern;
                struct _wfinddata64_t fd;
                intptr_t h;
                char **names = NULL;
                size_t n = 0;
                size_t cap = 0;
                size_t i;
                int result = 0;

                pattern = (wchar_t *)xmalloc((wcslen(wsrc) + 3) *
                                             sizeof(wchar_t));
                wcscpy(pattern, wsrc);
                wcscat(pattern, L"\\*");
                h = _wfindfirst64(pattern, &fd);
                free(pattern);
                if (h != -1) {
                        do {
                                if (!wcscmp(fd.name, L".") ||
                                    !wcscmp(fd.name, L".."))
                                        continue;
                                if (n == cap) {
                                        char **nn;

                                        cap = cap ? cap * 2 : 32;
                                        nn = (char **)xmalloc(cap * sizeof(char *));
                                        if (names) {
                                                memcpy(nn, names, n * sizeof(char *));
                                                free(names);
                                        }
                                        names = nn;
                                }
                                names[n++] = wide_to_utf8(fd.name);
                        } while (_wfindnext64(h, &fd) == 0);
                        _findclose(h);
                }
                qsort(names, n, sizeof(char *), cmp_str);
                for (i = 0; i < n; i++) {
                        char *s = path_join(src, names[i], '\\');
                        char *a = path_join(name, names[i], '\\');

                        if (collect(s, a) != 0)
                                result = -1;
                        free(s);
                        free(a);
                        free(names[i]);
                }
                free(names);
                free(wsrc);
                return result;
        } else {
                struct _wfinddata64_t fd;
                intptr_t h = _wfindfirst64(wsrc, &fd);
                uint16_t attribs = LIBLZX_CAB_ATTRIB_ARCHIVE;

                if (h != -1) {
                        attribs = win_attribs(fd.attrib);
                        _findclose(h);
                }
                add_entry(src, name, (uint64_t)st.st_size, attribs,
                          (time_t)st.st_mtime);
                free(wsrc);
                return 0;
        }
}

#else /* POSIX */

static int
collect(const char *src, const char *name)
{
        struct stat st;

        if (stat(src, &st) != 0) {
                fprintf(stderr, "lzxcab: не найден: %s\n", src);
                return -1;
        }
        if (S_ISDIR(st.st_mode)) {
                DIR *d = opendir(src);
                struct dirent *de;
                char **names = NULL;
                size_t n = 0;
                size_t cap = 0;
                size_t i;
                int result = 0;

                if (!d) {
                        fprintf(stderr, "lzxcab: не удалось открыть: %s\n", src);
                        return -1;
                }
                while ((de = readdir(d)) != NULL) {
                        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                                continue;
                        if (n == cap) {
                                char **nn;

                                cap = cap ? cap * 2 : 32;
                                nn = (char **)xmalloc(cap * sizeof(char *));
                                if (names) {
                                        memcpy(nn, names, n * sizeof(char *));
                                        free(names);
                                }
                                names = nn;
                        }
                        names[n++] = xstrdup(de->d_name);
                }
                closedir(d);
                qsort(names, n, sizeof(char *), cmp_str);
                for (i = 0; i < n; i++) {
                        char *s = path_join(src, names[i], '/');
                        char *a = path_join(name, names[i], '\\');

                        if (collect(s, a) != 0)
                                result = -1;
                        free(s);
                        free(a);
                        free(names[i]);
                }
                free(names);
                return result;
        }
        if (!S_ISREG(st.st_mode))
                return 0;
        add_entry(src, name, (uint64_t)st.st_size,
                  (uint16_t)(LIBLZX_CAB_ATTRIB_ARCHIVE |
                             ((st.st_mode & S_IWUSR) ? 0
                                                     : LIBLZX_CAB_ATTRIB_READONLY)),
                  st.st_mtime);
        return 0;
}

#endif

/* Имя в архиве для аргумента командной строки: последний компонент пути */
static char *
arg_name(const char *arg)
{
        const char *end = arg + strlen(arg);
        const char *start;
        char *r;

        while (end > arg && (end[-1] == '/' || end[-1] == '\\'))
                end--;
        start = end;
        while (start > arg && start[-1] != '/' && start[-1] != '\\' &&
               start[-1] != ':')
                start--;
        r = (char *)xmalloc((size_t)(end - start) + 1);
        memcpy(r, start, (size_t)(end - start));
        r[end - start] = '\0';
        return r;
}

/* Разбор размера с суффиксами K, M, G (степени 1024) */
static int
parse_size(const char *s, uint64_t *out)
{
        char *end;
        unsigned long long v = strtoull(s, &end, 10);

        if (end == s)
                return -1;
        switch (*end) {
        case 'k': case 'K':
                v <<= 10;
                end++;
                break;
        case 'm': case 'M':
                v <<= 20;
                end++;
                break;
        case 'g': case 'G':
                v <<= 30;
                end++;
                break;
        default:
                break;
        }
        if (*end)
                return -1;
        *out = v;
        return 0;
}

static int
progress_cb(void *userdata, const liblzx_cab_progress_t *p)
{
        static int last_percent = -1;

        (void)userdata;
        if (quiet)
                return 0;
        if (p->event == LIBLZX_CAB_EVENT_VOLUME_DONE) {
                fprintf(stderr, "\rтом %u: %s (%llu байт)            \n",
                        p->volume_index + 1, p->volume_path,
                        (unsigned long long)p->volume_size);
                last_percent = -1;
        } else if (total_size > 0) {
                int percent = (int)(p->total_in * 100 / total_size);

                if (percent != last_percent) {
                        fprintf(stderr, "\r%3d%%", percent);
                        last_percent = percent;
                }
        }
        return 0;
}

static void
usage(void)
{
        fprintf(stderr,
"Использование: lzxcab [параметры] <архив.cab> <файл|директория>...\n"
"\n"
"Параметры:\n"
"  -m none|lzx   метод сжатия (по умолчанию lzx)\n"
"  -w 15..21     размер окна LZX в битах (по умолчанию 21)\n"
"  -l N          уровень сжатия (по умолчанию 50; до 34 — быстрый разбор)\n"
"  -v РАЗМЕР     размер тома (суффиксы K, M, G); следующие тома\n"
"                называются <имя>2.cab, <имя>3.cab...\n"
"  -r РАЗМЕР     уменьшить первый том на РАЗМЕР байт\n"
"  -f РАЗМЕР     порог размера папки (новая папка после него)\n"
"  -s ЧИСЛО      идентификатор набора томов (setID)\n"
"  -e            отключить E8-трансляцию\n"
"  -q            не выводить прогресс\n"
"\n"
"Директории добавляются рекурсивно; в архиве сохраняются пути\n"
"относительно родительской директории аргумента.\n");
}

static int
run(int argc, char **argv)
{
        liblzx_cab_properties_t props;
        liblzx_cab_writer_t *w;
        const char *cab_path = NULL;
        int err;
        int i;
        int collect_failed = 0;
        size_t k;

        liblzx_cab_init_properties(&props);
        props.set_id = (uint16_t)time(NULL);

        for (i = 1; i < argc; i++) {
                const char *a = argv[i];
                const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
                uint64_t v;

                if (a[0] != '-' || a[1] == '\0' || a[2] != '\0')
                        break;
                switch (a[1]) {
                case 'm':
                        if (!val)
                                goto bad;
                        if (!strcmp(val, "none"))
                                props.compression = LIBLZX_CAB_COMPRESS_NONE;
                        else if (!strcmp(val, "lzx"))
                                props.compression = LIBLZX_CAB_COMPRESS_LZX;
                        else
                                goto bad;
                        i++;
                        break;
                case 'w':
                        if (!val || parse_size(val, &v) || v < 15 || v > 21)
                                goto bad;
                        props.lzx_window_bits = (unsigned)v;
                        i++;
                        break;
                case 'l':
                        if (!val || parse_size(val, &v) || v < 1 || v > 1000)
                                goto bad;
                        props.compression_level = (unsigned)v;
                        i++;
                        break;
                case 'v':
                        if (!val || parse_size(val, &v))
                                goto bad;
                        props.volume_size = v;
                        i++;
                        break;
                case 'r':
                        if (!val || parse_size(val, &v))
                                goto bad;
                        props.first_volume_reserve = v;
                        i++;
                        break;
                case 'f':
                        if (!val || parse_size(val, &v))
                                goto bad;
                        props.folder_size_threshold = v;
                        i++;
                        break;
                case 's':
                        if (!val || parse_size(val, &v) || v > 0xFFFF)
                                goto bad;
                        props.set_id = (uint16_t)v;
                        i++;
                        break;
                case 'e':
                        props.e8_disable = 1;
                        break;
                case 'q':
                        quiet = 1;
                        break;
                default:
                        goto bad;
                }
        }
        if (argc - i < 2)
                goto bad;
        cab_path = argv[i++];

        if (props.volume_size != 0 &&
            props.volume_size < LIBLZX_CAB_MIN_FDI_VOLUME_SIZE && !quiet)
                fprintf(stderr, "lzxcab: предупреждение: тома меньше %u байт "
                                "могут не распаковываться средствами Windows "
                                "(FDI, expand)\n",
                        (unsigned)LIBLZX_CAB_MIN_FDI_VOLUME_SIZE);

        for (; i < argc; i++) {
                char *name = arg_name(argv[i]);

                if (!name[0] || collect(argv[i], name) != 0)
                        collect_failed = 1;
                free(name);
        }
        if (collect_failed)
                return 1;
        if (num_entries == 0) {
                fprintf(stderr, "lzxcab: нет файлов для добавления\n");
                return 1;
        }

        props.cab_path = cab_path;
        props.progress = progress_cb;
        w = liblzx_cab_create(&props, &err);
        if (!w) {
                fprintf(stderr, "lzxcab: %s\n", liblzx_cab_strerror(err));
                return 1;
        }

        err = LIBLZX_CAB_OK;
        for (k = 0; k < num_entries && !err; k++) {
                liblzx_cab_file_info_t info;

                info.name = entries[k].name;
                info.attribs = entries[k].attribs;
                info.dos_date = entries[k].date;
                info.dos_time = entries[k].time;
                info.size = entries[k].size;
                err = liblzx_cab_add_file_from_path(w, entries[k].src, &info);
                if (err)
                        fprintf(stderr, "\nlzxcab: %s: %s\n", entries[k].src,
                                liblzx_cab_strerror(err));
        }
        if (!err) {
                err = liblzx_cab_finish(w);
                if (err)
                        fprintf(stderr, "\nlzxcab: %s\n",
                                liblzx_cab_strerror(err));
        }
        if (!err && !quiet)
                fprintf(stderr, "\rфайлов: %lu, томов: %u\n",
                        (unsigned long)num_entries,
                        liblzx_cab_get_volume_count(w));
        liblzx_cab_destroy(w);

        for (k = 0; k < num_entries; k++) {
                free(entries[k].src);
                free(entries[k].name);
        }
        free(entries);
        return err ? 1 : 0;

bad:
        usage();
        return 2;
}

#ifdef _WIN32
int
wmain(int argc, wchar_t **wargv)
{
        char **argv = (char **)xmalloc((size_t)(argc + 1) * sizeof(char *));
        UINT old_cp = GetConsoleOutputCP();
        int i;
        int result;

        /* Сообщения выводятся в UTF-8; кодовая страница консоли
         * восстанавливается при выходе.  Это единственное место, где
         * утилита обращается к Windows API: библиотека от него не зависит. */
        SetConsoleOutputCP(CP_UTF8);
        for (i = 0; i < argc; i++)
                argv[i] = wide_to_utf8(wargv[i]);
        argv[argc] = NULL;
        result = run(argc, argv);
        for (i = 0; i < argc; i++)
                free(argv[i]);
        free(argv);
        SetConsoleOutputCP(old_cp);
        return result;
}
#else
int
main(int argc, char **argv)
{
        return run(argc, argv);
}
#endif
