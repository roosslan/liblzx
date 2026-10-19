/*
 * cabbench — сравнение записи CAB с LZX средствами liblzx и cabinet.dll (FCI)
 * по времени упаковки и размеру архива.  Только для Windows.
 *
 * Запуск: cabbench <рабочая директория> [набор ...]
 * Наборы: log text exe random (по умолчанию все).
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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fci.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cabwriter.h"

/* Набор данных: список файлов на диске */
struct bench_set {
        const char *name;
        char **files;
        int count;
        uint64_t total;
};

static uint64_t rng_state = 1;

static uint64_t
rng_next(void)
{
        rng_state ^= rng_state >> 12;
        rng_state ^= rng_state << 25;
        rng_state ^= rng_state >> 27;
        return rng_state * 0x2545F4914F6CDD1Dull;
}

static double
now_sec(void)
{
        LARGE_INTEGER f, c;

        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return (double)c.QuadPart / (double)f.QuadPart;
}

static uint64_t
file_size_of(const char *path)
{
        struct _stat64 st;

        return _stat64(path, &st) == 0 ? (uint64_t)st.st_size : 0;
}

static void
set_add(struct bench_set *s, const char *path)
{
        uint64_t size = file_size_of(path);

        if (size == 0)
                return;
        s->files = (char **)realloc(s->files, (s->count + 1) * sizeof(char *));
        s->files[s->count++] = _strdup(path);
        s->total += size;
}

/* Сильно повторяющийся журнал: однотипные строки */
static void
gen_log(const char *path, uint64_t size)
{
        static const char *levels[] = { "INFO", "INFO", "INFO", "WARN" };
        FILE *f = fopen(path, "wb");
        uint64_t done = 0;
        uint64_t line = 0;
        char buf[256];

        if (!f)
                return;
        while (done < size) {
                int n = snprintf(buf, sizeof(buf),
                                 "2026-10-16 12:%02u:%02u.%03u [%s] "
                                 "worker-%u: request %llu processed in %u ms, "
                                 "status=200\r\n",
                                 (unsigned)(line / 60000 % 60),
                                 (unsigned)(line / 1000 % 60),
                                 (unsigned)(line % 1000),
                                 levels[rng_next() % 4],
                                 (unsigned)(rng_next() % 8),
                                 (unsigned long long)line,
                                 (unsigned)(rng_next() % 50));
                fwrite(buf, 1, (size_t)n, f);
                done += (uint64_t)n;
                line++;
        }
        fclose(f);
}

static void
gen_random(const char *path, uint64_t size)
{
        FILE *f = fopen(path, "wb");
        uint64_t buf[8192];
        uint64_t done = 0;

        if (!f)
                return;
        while (done < size) {
                size_t i;

                for (i = 0; i < 8192; i++)
                        buf[i] = rng_next();
                fwrite(buf, 1, sizeof(buf), f);
                done += sizeof(buf);
        }
        fclose(f);
}

/* Добавляет файлы директории с заданными расширениями (без рекурсии) */
static void
add_dir_files(struct bench_set *s, const char *dir, const char *const *exts,
              uint64_t limit)
{
        char pattern[MAX_PATH];
        struct __finddata64_t fd;
        intptr_t h;

        snprintf(pattern, sizeof(pattern), "%s\\*", dir);
        h = _findfirst64(pattern, &fd);
        if (h == -1)
                return;
        do {
                const char *dot = strrchr(fd.name, '.');
                int k;

                if ((fd.attrib & _A_SUBDIR) || !dot)
                        continue;
                for (k = 0; exts[k]; k++) {
                        if (!_stricmp(dot, exts[k]))
                                break;
                }
                if (!exts[k])
                        continue;
                if (s->total + (uint64_t)fd.size > limit)
                        continue;
                {
                        char path[MAX_PATH];

                        snprintf(path, sizeof(path), "%s\\%s", dir, fd.name);
                        set_add(s, path);
                }
        } while (_findnext64(h, &fd) == 0 && s->total < limit);
        _findclose(h);
}

/* Рекурсивно добавляет исходные тексты */
static void
add_sources(struct bench_set *s, const char *dir, uint64_t limit)
{
        static const char *const exts[] = {
                ".c", ".h", ".cpp", ".hpp", ".txt", ".md", ".py", ".cs",
                ".js", ".ts", ".java", ".xml", ".json", ".html", ".css", NULL
        };
        char pattern[MAX_PATH];
        struct __finddata64_t fd;
        intptr_t h;

        if (s->total >= limit)
                return;
        add_dir_files(s, dir, exts, limit);
        snprintf(pattern, sizeof(pattern), "%s\\*", dir);
        h = _findfirst64(pattern, &fd);
        if (h == -1)
                return;
        do {
                if ((fd.attrib & _A_SUBDIR) && fd.name[0] != '.') {
                        char sub[MAX_PATH];

                        snprintf(sub, sizeof(sub), "%s\\%s", dir, fd.name);
                        add_sources(s, sub, limit);
                }
        } while (_findnext64(h, &fd) == 0 && s->total < limit);
        _findclose(h);
}

/******************************************************************************/
/*                                   FCI                                      */
/******************************************************************************/

struct fci_ctx {
        char cab_dir[MAX_PATH];
        int temp_counter;
};

static FNFCIALLOC(fci_alloc) { return malloc(cb); }
static FNFCIFREE(fci_free) { free(memory); }

static FNFCIOPEN(fci_open)
{
        int fd = _open(pszFile, oflag, pmode);

        (void)pv;
        if (fd == -1)
                *err = errno;
        return fd;
}

static FNFCIREAD(fci_read)
{
        int n = _read((int)hf, memory, cb);

        (void)pv;
        if (n < 0)
                *err = errno;
        return (UINT)n;
}

static FNFCIWRITE(fci_write)
{
        int n = _write((int)hf, memory, cb);

        (void)pv;
        if (n < 0)
                *err = errno;
        return (UINT)n;
}

static FNFCICLOSE(fci_close)
{
        (void)pv;
        (void)err;
        return _close((int)hf);
}

static FNFCISEEK(fci_seek)
{
        (void)pv;
        (void)err;
        return _lseek((int)hf, dist, seektype);
}

static FNFCIDELETE(fci_delete)
{
        (void)pv;
        (void)err;
        return remove(pszFile);
}

static FNFCIGETTEMPFILE(fci_gettemp)
{
        struct fci_ctx *ctx = (struct fci_ctx *)pv;

        snprintf(pszTempName, (size_t)cbTempName, "%s\\fci%05d.tmp",
                 ctx->cab_dir, ctx->temp_counter++);
        return TRUE;
}

static FNFCIFILEPLACED(fci_placed)
{
        (void)pccab;
        (void)pszFile;
        (void)cbFile;
        (void)fContinuation;
        (void)pv;
        return 0;
}

static FNFCIGETNEXTCABINET(fci_nextcab)
{
        (void)pccab;
        (void)cbPrevCab;
        (void)pv;
        return FALSE;
}

static FNFCISTATUS(fci_status)
{
        (void)typeStatus;
        (void)cb1;
        (void)cb2;
        (void)pv;
        return 0;
}

static FNFCIGETOPENINFO(fci_openinfo)
{
        struct _stat64 st;
        int fd;

        (void)pv;
        fd = _open(pszName, _O_RDONLY | _O_BINARY);
        if (fd == -1) {
                *err = errno;
                return -1;
        }
        _stat64(pszName, &st);
        *pdate = 0x5550;        /* 2022-10-16 */
        *ptime = 0;
        *pattribs = _A_ARCH;
        return fd;
}

static int
build_fci(const struct bench_set *s, const char *dir, unsigned window,
          uint64_t *out_size, double *seconds)
{
        CCAB ccab;
        ERF erf;
        HFCI h;
        struct fci_ctx ctx;
        double t0 = now_sec();
        int i;
        char path[MAX_PATH];

        memset(&ccab, 0, sizeof(ccab));
        memset(&ctx, 0, sizeof(ctx));
        strcpy(ctx.cab_dir, dir);
        ccab.cb = 0x7FFFFFFF;
        ccab.cbFolderThresh = 0x7FFFFFFF;
        ccab.setID = 1;
        snprintf(ccab.szCabPath, sizeof(ccab.szCabPath), "%s\\", dir);
        strcpy(ccab.szCab, "fci.cab");

        h = FCICreate(&erf, fci_placed, fci_alloc, fci_free, fci_open,
                      fci_read, fci_write, fci_close, fci_seek, fci_delete,
                      fci_gettemp, &ccab, &ctx);
        if (!h)
                return -1;
        for (i = 0; i < s->count; i++) {
                char name[64];

                snprintf(name, sizeof(name), "f%04d", i);
                if (!FCIAddFile(h, s->files[i], name, FALSE, fci_nextcab,
                                fci_status, fci_openinfo,
                                (TCOMP)TCOMPfromLZXWindow(window))) {
                        FCIDestroy(h);
                        return -1;
                }
        }
        if (!FCIFlushCabinet(h, FALSE, fci_nextcab, fci_status)) {
                FCIDestroy(h);
                return -1;
        }
        FCIDestroy(h);
        *seconds = now_sec() - t0;
        snprintf(path, sizeof(path), "%s\\fci.cab", dir);
        *out_size = file_size_of(path);
        remove(path);
        return 0;
}

static int
build_liblzx(const struct bench_set *s, const char *dir, unsigned window,
             unsigned level, uint64_t *out_size, double *seconds)
{
        liblzx_cab_properties_t props;
        liblzx_cab_writer_t *w;
        char path[MAX_PATH];
        double t0 = now_sec();
        int err;
        int i;

        snprintf(path, sizeof(path), "%s\\lzx.cab", dir);
        liblzx_cab_init_properties(&props);
        props.cab_path = path;
        props.lzx_window_bits = window;
        props.compression_level = level;
        w = liblzx_cab_create(&props, &err);
        if (!w)
                return err;
        for (i = 0; i < s->count && !err; i++) {
                liblzx_cab_file_info_t info;
                char name[64];

                snprintf(name, sizeof(name), "f%04d", i);
                info.name = name;
                info.attribs = LIBLZX_CAB_ATTRIB_ARCHIVE;
                info.dos_date = 0x5550;
                info.dos_time = 0;
                info.size = file_size_of(s->files[i]);
                err = liblzx_cab_add_file_from_path(w, s->files[i], &info);
        }
        if (!err)
                err = liblzx_cab_finish(w);
        liblzx_cab_destroy(w);
        *seconds = now_sec() - t0;
        *out_size = file_size_of(path);
        remove(path);
        return err;
}

static void
run_set(const struct bench_set *s, const char *dir)
{
        static const unsigned windows[] = { 15, 18, 21 };
        static const unsigned levels[] = { 20, 50, 100 };
        size_t wi;
        size_t li;

        printf("\n## %s: файлов %d, %.1f Мб\n\n", s->name, s->count,
               (double)s->total / 1048576.0);
        printf("| окно | упаковщик | размер, байт | %% от исходного | время, с | Мб/с |\n");
        printf("|---|---|---:|---:|---:|---:|\n");
        for (wi = 0; wi < sizeof(windows) / sizeof(windows[0]); wi++) {
                uint64_t size;
                double sec;

                if (build_fci(s, dir, windows[wi], &size, &sec) == 0)
                        printf("| %u | cabinet.dll (FCI) | %llu | %.2f | %.2f | %.1f |\n",
                               windows[wi], (unsigned long long)size,
                               100.0 * (double)size / (double)s->total, sec,
                               (double)s->total / 1048576.0 / sec);
                else
                        printf("| %u | cabinet.dll (FCI) | ошибка | | | |\n",
                               windows[wi]);
                fflush(stdout);
                for (li = 0; li < sizeof(levels) / sizeof(levels[0]); li++) {
                        if (build_liblzx(s, dir, windows[wi], levels[li],
                                         &size, &sec) == 0)
                                printf("| %u | liblzx, уровень %u | %llu | %.2f | %.2f | %.1f |\n",
                                       windows[wi], levels[li],
                                       (unsigned long long)size,
                                       100.0 * (double)size / (double)s->total,
                                       sec, (double)s->total / 1048576.0 / sec);
                        else
                                printf("| %u | liblzx, уровень %u | ошибка | | | |\n",
                                       windows[wi], levels[li]);
                        fflush(stdout);
                }
        }
}

static int
wanted(int argc, char **argv, const char *name)
{
        int i;

        if (argc <= 2)
                return 1;
        for (i = 2; i < argc; i++) {
                if (!strcmp(argv[i], name))
                        return 1;
        }
        return 0;
}

int
main(int argc, char **argv)
{
        const char *dir;
        char path[MAX_PATH];
        char sysdir[MAX_PATH];

        if (argc < 2) {
                fprintf(stderr, "Использование: cabbench <директория> [log text exe random]\n");
                return 2;
        }
        SetConsoleOutputCP(CP_UTF8);
        dir = argv[1];
        _mkdir(dir);
        GetSystemDirectoryA(sysdir, sizeof(sysdir));

        if (wanted(argc, argv, "log")) {
                struct bench_set s = { "Повторяющийся журнал", NULL, 0, 0 };

                snprintf(path, sizeof(path), "%s\\server.log", dir);
                gen_log(path, 64ull << 20);
                set_add(&s, path);
                run_set(&s, dir);
        }
        if (wanted(argc, argv, "text")) {
                struct bench_set s = { "Текст и исходники", NULL, 0, 0 };
                const char *src = getenv("CABBENCH_SOURCES");

                add_sources(&s, src ? src : "C:\\Program Files (x86)\\Windows Kits\\10\\Include",
                            48ull << 20);
                run_set(&s, dir);
        }
        if (wanted(argc, argv, "exe")) {
                struct bench_set s = { "Исполняемые файлы (exe/dll)", NULL, 0, 0 };
                static const char *const exts[] = { ".dll", ".exe", NULL };

                add_dir_files(&s, sysdir, exts, 48ull << 20);
                run_set(&s, dir);
        }
        if (wanted(argc, argv, "random")) {
                struct bench_set s = { "Случайные данные", NULL, 0, 0 };

                snprintf(path, sizeof(path), "%s\\random.bin", dir);
                gen_random(path, 16ull << 20);
                set_add(&s, path);
                run_set(&s, dir);
        }
        return 0;
}
