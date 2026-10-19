/*
 * Запись CAB-архивов (формат Microsoft Cabinet, [MS-CAB]).
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

/*
 * Устройство.
 *
 * Папка (folder) — непрерывный поток несжатых данных всех её файлов,
 * разбитый на блоки по 32 Кб; каждый блок сжимается в одну запись CFDATA.
 * Смещения файлов (uoffFolderStart) отсчитываются от начала папки, даже если
 * папка началась в одном из предыдущих томов.
 *
 * Сжатые блоки текущего тома пишутся во временный файл.  При закрытии тома
 * создаётся файл тома: CFHEADER, CFFOLDER[], CFFILE[], затем копируются
 * данные из временного файла.  Поэтому все поля известны к моменту записи и
 * позиционирование не требуется.
 *
 * Многотомность устроена так же, как у FCI, потому что на это рассчитаны
 * распаковщики (FDI, libmspack).  Пусть C — несжатое смещение в текущей
 * папке, до которого все блоки целиком лежат в уже заполненных томах.  Если
 * папка продолжается в следующем томе («разорвана»), то в текущем томе
 * перечисляются все известные файлы этой папки: файлы, закончившиеся до C, —
 * с номером папки, остальные — как продолжающиеся (0xFFFE).  В следующем
 * томе те же незаконченные файлы перечисляются первыми как продолженные
 * (0xFFFD, или 0xFFFF, если папка рвётся снова).  libmspack сшивает папки
 * томов, сравнивая эти списки, и отбрасывает файлы продолженной папки из
 * правого тома, а FDI извлекает их при обработке левого тома.  Поэтому в
 * разорванную папку новые файлы не добавляются: следующий файл начинает
 * новую папку.  Место под записи CFFILE всех известных файлов текущей папки
 * резервируется в томе заранее.
 *
 * Блок, не поместившийся в остаток тома, разрезается: первая часть
 * записывается в текущий том с cbUncomp = 0, остаток — в начало
 * следующего тома с настоящим cbUncomp (как делает FCI).  Каждая часть
 * имеет свою контрольную сумму.
 */

#include "cabwriter.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <wchar.h>
#endif

#define CAB_BLOCK_SIZE          32768u
#define CFHEADER_SIZE           36u
#define CFFOLDER_SIZE           8u
#define CFFILE_FIXED_SIZE       16u
#define CFDATA_HEADER_SIZE      8u

/* Наибольший размер сжатого блока CFDATA по [MS-CAB] */
#define CFDATA_MAX_COMPRESSED   (CAB_BLOCK_SIZE + 6144u)

/* Не более 65534 блоков в папке: при сшивке папок томов libmspack
 * проверяет сумму числа блоков до вычета разрезанного блока. */
#define MAX_FOLDER_BLOCKS       65534u

#define CFHDR_PREV_CABINET      0x0001u
#define CFHDR_NEXT_CABINET      0x0002u

#define IFOLD_CONT_FROM_PREV    0xFFFDu
#define IFOLD_CONT_TO_NEXT      0xFFFEu
#define IFOLD_CONT_PREV_NEXT    0xFFFFu

#define TCOMP_TYPE_LZX          0x0003u

#define MAX_VOLUMES             65536u

/* Запас под имя следующего тома и диска, пока имя ещё не известно */
#define NEXT_NAME_RESERVE       (2u * (LIBLZX_CAB_MAX_NAME + 1u))

#define IO_BUF_SIZE             65536u

#define MAX_PATH_BYTES          4096u

enum writer_state {
        STATE_IDLE,
        STATE_IN_FILE,
        STATE_FINISHED,
};

/* Файл архива */
struct cab_file {
        struct cab_file *next;
        char *name;
        size_t name_len;
        uint16_t attribs;
        uint16_t date;
        uint16_t time;
        /* Порядковый номер папки */
        uint32_t folder_seq;
        /* Смещение в папке и текущий размер */
        uint32_t offset;
        uint32_t size;
        /* Заявленный размер или LIBLZX_CAB_SIZE_UNKNOWN */
        uint64_t declared_size;
        /* Файл ещё принимает данные */
        bool open;
        /* Файл перечислен в одном из уже записанных томов */
        bool listed_before;
        /* Файл не закончился в записанном томе и переносится в следующий */
        bool carried;
};

/* Запись CFFILE текущего тома */
struct vol_entry {
        struct cab_file *file;
        uint16_t folder_index;
};

/* Запись CFFOLDER текущего тома */
struct vol_folder {
        /* Смещение первого CFDATA от начала области данных тома */
        uint64_t data_start;
        uint32_t num_data;
        uint16_t type;
};

/* Отложенная запись размера файла */
struct size_patch {
        unsigned volume;
        uint64_t offset;
};

struct liblzx_cab_writer {
        /* Параметры */
        liblzx_cab_properties_t props;
        char *cab_path;
        char *temp_path;
        liblzx_cab_io_t io;
        uint16_t type_compress;
        uint64_t volume_size;

        int error;
        enum writer_state state;

        /* Сжатие */
        liblzx_compressor_t *lzx;
        uint8_t *stored_buf;
        uint32_t stored_used;

        /* Файлы: от самого старого ещё нужного к последнему добавленному */
        struct cab_file *files_head;
        struct cab_file *files_tail;
        /* Текущий открытый файл */
        struct cab_file *cur_file;
        /* Первый файл, ещё не перечисленный ни в одном томе */
        struct cab_file *next_unlisted;

        /* Текущая папка */
        bool folder_open;
        uint32_t folder_seq;
        /* Несжатых байт принято, передано в блоки и размещено в томах
         * целиком (C) */
        uint32_t folder_fed;
        uint32_t folder_emitted;
        uint32_t folder_placed;
        uint32_t folder_blocks;
        /* Индекс текущей папки в таблице папок тома или -1 */
        int cur_folder_vidx;
        /* Папка разорвана между томами: новые файлы в неё не добавляются */
        bool folder_split;
        /* Имя тома и диска, в котором началась текущая папка: FCI пишет
         * их в szCabinetPrev/szDiskPrev тома, где папка продолжается */
        char folder_start_name[LIBLZX_CAB_MAX_NAME + 1];
        char folder_start_disk[LIBLZX_CAB_MAX_NAME + 1];

        /* Текущий том */
        bool vol_open;
        unsigned vol_index;
        uint64_t vol_limit;
        char vol_path[MAX_PATH_BYTES];
        char vol_disk[LIBLZX_CAB_MAX_NAME + 1];
        bool has_prev;
        char prev_name[LIBLZX_CAB_MAX_NAME + 1];
        char prev_disk[LIBLZX_CAB_MAX_NAME + 1];
        bool next_known;
        char next_path[MAX_PATH_BYTES];
        char next_disk[LIBLZX_CAB_MAX_NAME + 1];

        struct vol_entry *entries;
        size_t num_entries;
        size_t cap_entries;
        uint64_t entries_bytes;

        struct vol_folder *folders;
        size_t num_folders;
        size_t cap_folders;

        /* Временный файл с CFDATA текущего тома */
        void *temp_handle;
        bool temp_created;
        uint64_t temp_bytes;

        /* Открытый на запись файл тома (для очистки при ошибке) */
        void *out_handle;

        /* Пути всех созданных томов */
        char **vol_paths;
        unsigned num_vol_paths;

        /* Статистика */
        uint64_t total_in;
        uint64_t total_out;

        /* Буфер копирования временного файла и буфер чтения исходных
         * файлов (раздельные: копирование может начаться посреди подачи
         * прочитанных данных) */
        uint8_t *io_buf;
        uint8_t *read_buf;

        /* Места поля cbFile текущего файла в записанных томах, которые
         * нужно исправить по окончании файла (размер был неизвестен) */
        struct size_patch *patches;
        size_t num_patches;
        size_t cap_patches;
};

/******************************************************************************/
/*                          Ввод-вывод по умолчанию                           */
/******************************************************************************/

#ifdef _WIN32
/* Преобразует UTF-8 в UTF-16.  Возвращает число символов без нуля или
 * (size_t)-1 при ошибке кодировки; при out == NULL только считает. */
static size_t
utf8_to_utf16(const char *in, wchar_t *out)
{
        const unsigned char *p = (const unsigned char *)in;
        size_t n = 0;

        while (*p) {
                uint32_t cp;
                unsigned extra;
                unsigned i;

                if (p[0] < 0x80) {
                        cp = p[0];
                        extra = 0;
                } else if ((p[0] & 0xE0) == 0xC0) {
                        cp = p[0] & 0x1F;
                        extra = 1;
                } else if ((p[0] & 0xF0) == 0xE0) {
                        cp = p[0] & 0x0F;
                        extra = 2;
                } else if ((p[0] & 0xF8) == 0xF0) {
                        cp = p[0] & 0x07;
                        extra = 3;
                } else {
                        return (size_t)-1;
                }
                for (i = 1; i <= extra; i++) {
                        if ((p[i] & 0xC0) != 0x80)
                                return (size_t)-1;
                        cp = (cp << 6) | (p[i] & 0x3F);
                }
                p += extra + 1;

                if (cp >= 0x10000) {
                        cp -= 0x10000;
                        if (out) {
                                out[n] = (wchar_t)(0xD800 + (cp >> 10));
                                out[n + 1] = (wchar_t)(0xDC00 + (cp & 0x3FF));
                        }
                        n += 2;
                } else {
                        if (out)
                                out[n] = (wchar_t)cp;
                        n++;
                }
        }
        if (out)
                out[n] = 0;
        return n;
}

static wchar_t *
utf8_to_wide_alloc(const char *s)
{
        size_t n = utf8_to_utf16(s, NULL);
        wchar_t *w;

        if (n == (size_t)-1)
                return NULL;
        w = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
        if (w)
                utf8_to_utf16(s, w);
        return w;
}
#endif /* _WIN32 */

static void *
stdio_open(void *userdata, const char *path, int mode)
{
        FILE *f;

        (void)userdata;
#ifdef _WIN32
        {
                wchar_t *wpath = utf8_to_wide_alloc(path);

                if (!wpath)
                        return NULL;
                f = _wfopen(wpath, mode == LIBLZX_CAB_IO_WRITE ? L"wb" :
                                   mode == LIBLZX_CAB_IO_UPDATE ? L"r+b" :
                                   L"rb");
                free(wpath);
        }
#else
        f = fopen(path, mode == LIBLZX_CAB_IO_WRITE ? "wb" :
                        mode == LIBLZX_CAB_IO_UPDATE ? "r+b" : "rb");
#endif
        return f;
}

static size_t
stdio_read(void *userdata, void *handle, void *buf, size_t size)
{
        size_t n;

        (void)userdata;
        n = fread(buf, 1, size, (FILE *)handle);
        if (n < size && ferror((FILE *)handle))
                return (size_t)-1;
        return n;
}

static size_t
stdio_write(void *userdata, void *handle, const void *buf, size_t size)
{
        (void)userdata;
        return fwrite(buf, 1, size, (FILE *)handle);
}

static int
stdio_close(void *userdata, void *handle)
{
        (void)userdata;
        return fclose((FILE *)handle) == 0 ? 0 : -1;
}

static int
stdio_remove(void *userdata, const char *path)
{
        (void)userdata;
#ifdef _WIN32
        {
                wchar_t *wpath = utf8_to_wide_alloc(path);
                int result;

                if (!wpath)
                        return -1;
                result = _wremove(wpath);
                free(wpath);
                return result;
        }
#else
        return remove(path);
#endif
}

static int
stdio_seek(void *userdata, void *handle, uint64_t offset)
{
        (void)userdata;
        /* Тома не превышают 2 Гб, поэтому long достаточно */
        if (offset > LIBLZX_CAB_MAX_VOLUME_SIZE)
                return -1;
        return fseek((FILE *)handle, (long)offset, SEEK_SET) == 0 ? 0 : -1;
}

static const liblzx_cab_io_t default_io = {
        stdio_open, stdio_read, stdio_write, stdio_close, stdio_remove,
        stdio_seek, NULL
};

const liblzx_cab_io_t *
liblzx_cab_default_io(void)
{
        return &default_io;
}

/******************************************************************************/
/*                              Вспомогательное                               */
/******************************************************************************/

static void *
default_alloc(void *userdata, size_t size)
{
        (void)userdata;
        return malloc(size);
}

static void
default_free(void *userdata, void *ptr)
{
        (void)userdata;
        free(ptr);
}

static void *
w_alloc(liblzx_cab_writer_t *w, size_t size)
{
        return w->props.alloc_func(w->props.alloc_userdata, size);
}

static void
w_free(liblzx_cab_writer_t *w, void *ptr)
{
        if (ptr)
                w->props.free_func(w->props.alloc_userdata, ptr);
}

static char *
w_strdup(liblzx_cab_writer_t *w, const char *s)
{
        size_t n = strlen(s) + 1;
        char *d = (char *)w_alloc(w, n);

        if (d)
                memcpy(d, s, n);
        return d;
}

/* Увеличивает массив до вместимости не меньше need элементов */
static bool
w_grow(liblzx_cab_writer_t *w, void **arr, size_t *cap, size_t need,
       size_t elem_size)
{
        size_t new_cap;
        void *p;

        if (need <= *cap)
                return true;
        new_cap = *cap ? *cap * 2 : 16;
        while (new_cap < need)
                new_cap *= 2;
        p = w_alloc(w, new_cap * elem_size);
        if (!p)
                return false;
        if (*arr) {
                memcpy(p, *arr, *cap * elem_size);
                w_free(w, *arr);
        }
        *arr = p;
        *cap = new_cap;
        return true;
}

static void
put_le16(uint8_t *p, uint32_t v)
{
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
}

static void
put_le32(uint8_t *p, uint32_t v)
{
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        p[3] = (uint8_t)(v >> 24);
}

/* Контрольная сумма CAB ([MS-CAB], раздел 2.4) */
static uint32_t
cab_checksum(const uint8_t *data, size_t size, uint32_t seed)
{
        uint32_t csum = seed;
        uint32_t ul = 0;
        size_t i;

        for (i = size >> 2; i > 0; i--, data += 4) {
                csum ^= (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                        ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
        }
        switch (size & 3) {
        case 3:
                ul |= (uint32_t)*data++ << 16;
                /* fall through */
        case 2:
                ul |= (uint32_t)*data++ << 8;
                /* fall through */
        case 1:
                ul |= *data;
                break;
        default:
                break;
        }
        return csum ^ ul;
}

/* Имя файла без пути */
static const char *
path_basename(const char *path)
{
        const char *base = path;
        const char *p;

        for (p = path; *p; p++) {
                if (*p == '/' || *p == '\\' || *p == ':')
                        base = p + 1;
        }
        return base;
}

/* Проверяет корректность UTF-8; *non_ascii — есть ли не-ASCII символы */
static bool
utf8_valid(const char *s, bool *non_ascii)
{
        const unsigned char *p = (const unsigned char *)s;

        *non_ascii = false;
        while (*p) {
                unsigned extra;
                uint32_t cp;
                unsigned i;

                if (p[0] < 0x80) {
                        p++;
                        continue;
                }
                *non_ascii = true;
                if ((p[0] & 0xE0) == 0xC0) {
                        extra = 1;
                        cp = p[0] & 0x1F;
                } else if ((p[0] & 0xF0) == 0xE0) {
                        extra = 2;
                        cp = p[0] & 0x0F;
                } else if ((p[0] & 0xF8) == 0xF0) {
                        extra = 3;
                        cp = p[0] & 0x07;
                } else {
                        return false;
                }
                for (i = 1; i <= extra; i++) {
                        if ((p[i] & 0xC0) != 0x80)
                                return false;
                        cp = (cp << 6) | (p[i] & 0x3F);
                }
                /* Избыточные формы, суррогаты и значения за U+10FFFF */
                if ((extra == 1 && cp < 0x80) ||
                    (extra == 2 && cp < 0x800) ||
                    (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
                    (cp >= 0xD800 && cp <= 0xDFFF))
                        return false;
                p += extra + 1;
        }
        return true;
}

static int
set_error(liblzx_cab_writer_t *w, int error)
{
        if (w->error == LIBLZX_CAB_OK)
                w->error = error;
        return w->error;
}

static int
report_progress(liblzx_cab_writer_t *w, int event, const char *volume_path,
                uint64_t volume_size)
{
        liblzx_cab_progress_t p;

        if (!w->props.progress)
                return LIBLZX_CAB_OK;
        p.event = event;
        p.total_in = w->total_in;
        p.total_out = w->total_out;
        p.file_in = w->cur_file ? w->cur_file->size : 0;
        p.volume_index = w->vol_index;
        p.volume_path = volume_path;
        p.volume_size = volume_size;
        if (w->props.progress(w->props.userdata, &p) != 0)
                return set_error(w, LIBLZX_CAB_ERR_CANCELLED);
        return LIBLZX_CAB_OK;
}

static int
io_write_all(liblzx_cab_writer_t *w, void *handle, const void *buf,
             size_t size)
{
        if (size == 0)
                return LIBLZX_CAB_OK;
        if (w->io.write(w->io.userdata, handle, buf, size) != size)
                return set_error(w, LIBLZX_CAB_ERR_WRITE);
        return LIBLZX_CAB_OK;
}

/******************************************************************************/
/*                                  Тома                                      */
/******************************************************************************/

/* Шаблон имени тома по умолчанию: <имя><N+1><расширение> */
static int
default_volume_name(liblzx_cab_writer_t *w, unsigned index, char *path,
                    size_t path_size)
{
        const char *base = path_basename(w->cab_path);
        const char *dot = strrchr(base, '.');
        size_t stem_len = dot ? (size_t)(dot - w->cab_path)
                              : strlen(w->cab_path);
        const char *ext = dot ? dot : "";
        int n;

        n = snprintf(path, path_size, "%.*s%u%s", (int)stem_len, w->cab_path,
                     index + 1, ext);
        if (n < 0 || (size_t)n >= path_size)
                return LIBLZX_CAB_ERR_NAME;
        return LIBLZX_CAB_OK;
}

/* Запрашивает имя следующего тома */
static int
fetch_next_name(liblzx_cab_writer_t *w)
{
        unsigned index = w->vol_index + 1;

        if (w->next_known)
                return LIBLZX_CAB_OK;
        if (index >= MAX_VOLUMES)
                return set_error(w, LIBLZX_CAB_ERR_TOO_MANY_VOLUMES);

        w->next_path[0] = '\0';
        w->next_disk[0] = '\0';
        if (w->props.volume_name) {
                if (w->props.volume_name(w->props.userdata, index,
                                         w->next_path, sizeof(w->next_path),
                                         w->next_disk,
                                         sizeof(w->next_disk)) != 0)
                        return set_error(w, LIBLZX_CAB_ERR_CANCELLED);
                w->next_path[sizeof(w->next_path) - 1] = '\0';
                w->next_disk[sizeof(w->next_disk) - 1] = '\0';
        } else {
                int err = default_volume_name(w, index, w->next_path,
                                              sizeof(w->next_path));
                if (err)
                        return set_error(w, err);
        }
        if (w->next_path[0] == '\0' ||
            strlen(path_basename(w->next_path)) > LIBLZX_CAB_MAX_NAME ||
            strlen(w->next_disk) > LIBLZX_CAB_MAX_NAME)
                return set_error(w, LIBLZX_CAB_ERR_NAME);
        w->next_known = true;
        return LIBLZX_CAB_OK;
}

/* Размер полей имён следующего тома в заголовке (или запас под них) */
static uint64_t
next_names_size(const liblzx_cab_writer_t *w)
{
        if (!w->next_known)
                return NEXT_NAME_RESERVE;
        return strlen(path_basename(w->next_path)) + 1 +
               strlen(w->next_disk) + 1;
}

/* Размер заголовка CFHEADER с именами соседних томов */
static uint64_t
header_size(const liblzx_cab_writer_t *w, bool with_next)
{
        uint64_t size = CFHEADER_SIZE;

        if (w->has_prev)
                size += strlen(w->prev_name) + 1 + strlen(w->prev_disk) + 1;
        if (with_next)
                size += next_names_size(w);
        return size;
}

static int
add_entry(liblzx_cab_writer_t *w, struct cab_file *f, uint16_t folder_index)
{
        if (!w_grow(w, (void **)&w->entries, &w->cap_entries,
                    w->num_entries + 1, sizeof(*w->entries)))
                return set_error(w, LIBLZX_CAB_ERR_NOMEM);
        w->entries[w->num_entries].file = f;
        w->entries[w->num_entries].folder_index = folder_index;
        w->num_entries++;
        w->entries_bytes += CFFILE_FIXED_SIZE + f->name_len + 1;
        return LIBLZX_CAB_OK;
}

/* Добавляет текущую папку в таблицу папок тома, если её там ещё нет */
static int
ensure_folder_in_volume(liblzx_cab_writer_t *w)
{
        struct vol_folder *vf;

        if (w->cur_folder_vidx >= 0)
                return LIBLZX_CAB_OK;
        if (!w_grow(w, (void **)&w->folders, &w->cap_folders,
                    w->num_folders + 1, sizeof(*w->folders)))
                return set_error(w, LIBLZX_CAB_ERR_NOMEM);
        vf = &w->folders[w->num_folders];
        vf->data_start = w->temp_bytes;
        vf->num_data = 0;
        vf->type = w->type_compress;
        if (!w->folder_split) {
                strcpy(w->folder_start_name, path_basename(w->vol_path));
                strcpy(w->folder_start_disk, w->vol_disk);
        }
        w->cur_folder_vidx = (int)w->num_folders;
        w->num_folders++;
        return LIBLZX_CAB_OK;
}

/* Открывает очередной том (логически: файл создаётся при закрытии) */
static int
open_volume(liblzx_cab_writer_t *w)
{
        int err;

        if (w->vol_open)
                return LIBLZX_CAB_OK;

        if (w->num_vol_paths == 0) {
                w->vol_index = 0;
                if (strlen(w->cab_path) >= sizeof(w->vol_path))
                        return set_error(w, LIBLZX_CAB_ERR_NAME);
                strcpy(w->vol_path, w->cab_path);
                w->vol_disk[0] = '\0';
        } else {
                /* Имя этого тома уже записано в заголовок предыдущего */
                strcpy(w->vol_path, w->next_path);
                strcpy(w->vol_disk, w->next_disk);
                w->next_known = false;
        }

        w->vol_limit = w->volume_size;
        if (w->vol_index == 0) {
                if (w->props.first_volume_reserve >= w->vol_limit)
                        return set_error(w, LIBLZX_CAB_ERR_VOLUME_TOO_SMALL);
                w->vol_limit -= w->props.first_volume_reserve;
        }

        w->num_entries = 0;
        w->entries_bytes = 0;
        w->num_folders = 0;
        w->temp_bytes = 0;
        w->cur_folder_vidx = -1;
        w->vol_open = true;

        /* Если размер тома задан явно, имя следующего тома нужно заранее,
         * чтобы точно знать размер заголовка. */
        if (w->props.volume_size != 0) {
                err = fetch_next_name(w);
                if (err)
                        return err;
        }

        /* Продолжение разорванной папки: её незаконченные файлы
         * перечисляются первыми */
        {
                struct cab_file *f;

                for (f = w->files_head; f; f = f->next) {
                        if (!f->carried)
                                continue;
                        err = ensure_folder_in_volume(w);
                        if (err)
                                return err;
                        err = add_entry(w, f, (uint16_t)w->cur_folder_vidx);
                        if (err)
                                return err;
                        f->carried = false;
                }
        }
        return LIBLZX_CAB_OK;
}

/* Перечисляет в томе файлы текущей папки со смещением начала <= upto */
static int
list_files_upto(liblzx_cab_writer_t *w, uint32_t upto)
{
        int err;

        while (w->next_unlisted &&
               w->next_unlisted->folder_seq == w->folder_seq &&
               w->next_unlisted->offset <= upto) {
                err = ensure_folder_in_volume(w);
                if (err)
                        return err;
                err = add_entry(w, w->next_unlisted,
                                (uint16_t)w->cur_folder_vidx);
                if (err)
                        return err;
                w->next_unlisted = w->next_unlisted->next;
        }
        return LIBLZX_CAB_OK;
}

/* Сколько записей CFFILE (и байт) ещё понадобится текущей папке: все её
 * известные, но не перечисленные файлы */
static void
count_unlisted(const liblzx_cab_writer_t *w, size_t *count, uint64_t *bytes)
{
        const struct cab_file *f;

        *count = 0;
        *bytes = 0;
        for (f = w->next_unlisted; f && f->folder_seq == w->folder_seq;
             f = f->next) {
                (*count)++;
                *bytes += CFFILE_FIXED_SIZE + f->name_len + 1;
        }
}

/* Размер тома без данных при заданном числе дополнительных папок и файлов */
static uint64_t
volume_overhead(const liblzx_cab_writer_t *w, size_t extra_folders,
                uint64_t extra_file_bytes)
{
        return header_size(w, true) +
               (uint64_t)(w->num_folders + extra_folders) * CFFOLDER_SIZE +
               w->entries_bytes + extra_file_bytes;
}

/* Освобождает файлы, которые уже полностью описаны в записанных томах */
static void
release_files(liblzx_cab_writer_t *w)
{
        while (w->files_head && w->files_head->listed_before &&
               !w->files_head->carried &&
               w->files_head != w->cur_file &&
               w->files_head != w->next_unlisted) {
                struct cab_file *f = w->files_head;

                w->files_head = f->next;
                if (!w->files_head)
                        w->files_tail = NULL;
                w_free(w, f->name);
                w_free(w, f);
        }
}

/* Копирует данные из временного файла в файл тома */
static int
copy_temp_to_volume(liblzx_cab_writer_t *w, void *out)
{
        uint64_t remaining = w->temp_bytes;
        int err;

        if (!w->temp_handle)
                return remaining ? set_error(w, LIBLZX_CAB_ERR_READ)
                                 : LIBLZX_CAB_OK;

        if (w->io.close(w->io.userdata, w->temp_handle) != 0) {
                w->temp_handle = NULL;
                return set_error(w, LIBLZX_CAB_ERR_WRITE);
        }
        w->temp_handle = w->io.open(w->io.userdata, w->temp_path,
                                    LIBLZX_CAB_IO_READ);
        if (!w->temp_handle)
                return set_error(w, LIBLZX_CAB_ERR_OPEN);

        while (remaining > 0) {
                size_t chunk = remaining < IO_BUF_SIZE ? (size_t)remaining
                                                       : IO_BUF_SIZE;
                size_t n = w->io.read(w->io.userdata, w->temp_handle,
                                      w->io_buf, chunk);

                if (n == (size_t)-1 || n == 0)
                        return set_error(w, LIBLZX_CAB_ERR_READ);
                err = io_write_all(w, out, w->io_buf, n);
                if (err)
                        return err;
                remaining -= n;
        }

        w->io.close(w->io.userdata, w->temp_handle);
        w->temp_handle = NULL;
        return LIBLZX_CAB_OK;
}

/*
 * Записывает текущий том.  has_next — будет ли следующий том.  Если текущая
 * папка присутствует в томе и ещё не закончена, она продолжается в
 * следующем томе.
 */
static int
close_volume(liblzx_cab_writer_t *w, bool has_next)
{
        bool continuing = has_next && w->folder_open &&
                          w->cur_folder_vidx >= 0;
        size_t num_to_next = 0;
        uint64_t hdr_size;
        uint64_t files_start;
        uint64_t data_start;
        uint64_t cab_size;
        uint8_t buf[CFHEADER_SIZE + 4 * (LIBLZX_CAB_MAX_NAME + 1)];
        uint8_t *p;
        uint16_t flags = 0;
        uint64_t entry_pos;
        uint32_t file_size;
        char *path_copy;
        void *out;
        size_t i;
        int err;

        if (continuing) {
                /* Перечисляются все известные файлы разорванной папки;
                 * незаконченные в этом томе переносятся в следующий. */
                err = list_files_upto(w, UINT32_MAX);
                if (err)
                        return err;
                for (i = 0; i < w->num_entries; i++) {
                        struct cab_file *f = w->entries[i].file;

                        if (f->folder_seq == w->folder_seq &&
                            (f->open || (uint64_t)f->offset + f->size >
                                                w->folder_placed)) {
                                f->carried = true;
                                num_to_next++;
                        }
                }
                /* Данные за позицией C принадлежат известным файлам,
                 * поэтому хотя бы один файл переносится всегда. */
                if (num_to_next == 0)
                        return set_error(w, LIBLZX_CAB_ERR_COMPRESS);
        }

        if (has_next) {
                err = fetch_next_name(w);
                if (err)
                        return err;
                flags |= CFHDR_NEXT_CABINET;
        }
        if (w->has_prev)
                flags |= CFHDR_PREV_CABINET;

        if (w->num_entries > LIBLZX_CAB_MAX_FILES_PER_VOLUME)
                return set_error(w, LIBLZX_CAB_ERR_TOO_MANY_FILES);

        hdr_size = header_size(w, has_next);
        files_start = hdr_size + (uint64_t)w->num_folders * CFFOLDER_SIZE;
        data_start = files_start + w->entries_bytes;
        cab_size = data_start + w->temp_bytes;
        if (cab_size > w->vol_limit || cab_size > LIBLZX_CAB_MAX_VOLUME_SIZE)
                return set_error(w, LIBLZX_CAB_ERR_VOLUME_TOO_SMALL);

        /* Файл тома регистрируется до создания, чтобы при ошибке его
         * можно было удалить. */
        {
                char **paths = (char **)w_alloc(
                        w, (w->num_vol_paths + 1) * sizeof(char *));

                if (!paths)
                        return set_error(w, LIBLZX_CAB_ERR_NOMEM);
                if (w->vol_paths) {
                        memcpy(paths, w->vol_paths,
                               w->num_vol_paths * sizeof(char *));
                        w_free(w, w->vol_paths);
                }
                w->vol_paths = paths;
                path_copy = w_strdup(w, w->vol_path);
                if (!path_copy)
                        return set_error(w, LIBLZX_CAB_ERR_NOMEM);
                w->vol_paths[w->num_vol_paths++] = path_copy;
        }

        out = w->io.open(w->io.userdata, w->vol_path, LIBLZX_CAB_IO_WRITE);
        if (!out)
                return set_error(w, LIBLZX_CAB_ERR_OPEN);
        w->out_handle = out;

        /* CFHEADER */
        memset(buf, 0, CFHEADER_SIZE);
        buf[0] = 'M';
        buf[1] = 'S';
        buf[2] = 'C';
        buf[3] = 'F';
        put_le32(buf + 8, (uint32_t)cab_size);           /* cbCabinet */
        put_le32(buf + 16, (uint32_t)files_start);       /* coffFiles */
        buf[24] = 3;                                     /* versionMinor */
        buf[25] = 1;                                     /* versionMajor */
        put_le16(buf + 26, (uint32_t)w->num_folders);    /* cFolders */
        put_le16(buf + 28, (uint32_t)w->num_entries);    /* cFiles */
        put_le16(buf + 30, flags);                       /* flags */
        put_le16(buf + 32, w->props.set_id);             /* setID */
        put_le16(buf + 34, w->vol_index);                /* iCabinet */
        p = buf + CFHEADER_SIZE;
        if (w->has_prev) {
                size_t n = strlen(w->prev_name) + 1;

                memcpy(p, w->prev_name, n);
                p += n;
                n = strlen(w->prev_disk) + 1;
                memcpy(p, w->prev_disk, n);
                p += n;
        }
        if (has_next) {
                const char *next_name = path_basename(w->next_path);
                size_t n = strlen(next_name) + 1;

                memcpy(p, next_name, n);
                p += n;
                n = strlen(w->next_disk) + 1;
                memcpy(p, w->next_disk, n);
                p += n;
        }
        err = io_write_all(w, out, buf, (size_t)(p - buf));
        if (err)
                return err;

        /* CFFOLDER */
        for (i = 0; i < w->num_folders; i++) {
                const struct vol_folder *vf = &w->folders[i];

                put_le32(buf, (uint32_t)(data_start + vf->data_start));
                put_le16(buf + 4, vf->num_data);
                put_le16(buf + 6, vf->type);
                err = io_write_all(w, out, buf, CFFOLDER_SIZE);
                if (err)
                        return err;
        }

        /* CFFILE */
        entry_pos = files_start;
        for (i = 0; i < w->num_entries; i++) {
                const struct vol_entry *e = &w->entries[i];
                const struct cab_file *f = e->file;
                bool from_prev = f->listed_before;
                bool to_next = f->carried;
                uint16_t ifolder;

                if (from_prev && to_next)
                        ifolder = IFOLD_CONT_PREV_NEXT;
                else if (from_prev)
                        ifolder = IFOLD_CONT_FROM_PREV;
                else if (to_next)
                        ifolder = IFOLD_CONT_TO_NEXT;
                else
                        ifolder = e->folder_index;

                /* Размер ещё открытого граничного файла: заявленный, а
                 * если он неизвестен — исправляется по окончании файла. */
                if (!f->open) {
                        file_size = f->size;
                } else if (f->declared_size != LIBLZX_CAB_SIZE_UNKNOWN) {
                        file_size = (uint32_t)f->declared_size;
                } else {
                        if (!w->io.seek)
                                return set_error(w,
                                                 LIBLZX_CAB_ERR_INVALID_PARAM);
                        if (!w_grow(w, (void **)&w->patches,
                                    &w->cap_patches, w->num_patches + 1,
                                    sizeof(*w->patches)))
                                return set_error(w, LIBLZX_CAB_ERR_NOMEM);
                        w->patches[w->num_patches].volume =
                                w->num_vol_paths - 1;
                        w->patches[w->num_patches].offset = entry_pos;
                        w->num_patches++;
                        file_size = f->size;
                }
                put_le32(buf, file_size);
                put_le32(buf + 4, f->offset);
                put_le16(buf + 8, ifolder);
                put_le16(buf + 10, f->date);
                put_le16(buf + 12, f->time);
                put_le16(buf + 14, f->attribs);
                memcpy(buf + CFFILE_FIXED_SIZE, f->name, f->name_len + 1);
                err = io_write_all(w, out, buf,
                                   CFFILE_FIXED_SIZE + f->name_len + 1);
                if (err)
                        return err;
                entry_pos += CFFILE_FIXED_SIZE + f->name_len + 1;
        }

        /* CFDATA */
        err = copy_temp_to_volume(w, out);
        if (err)
                return err;

        w->out_handle = NULL;
        if (w->io.close(w->io.userdata, out) != 0)
                return set_error(w, LIBLZX_CAB_ERR_WRITE);

        err = report_progress(w, LIBLZX_CAB_EVENT_VOLUME_DONE, w->vol_path,
                              cab_size);
        if (err)
                return err;

        /* Переход к следующему тому */
        for (i = 0; i < w->num_entries; i++)
                w->entries[i].file->listed_before = true;
        if (continuing)
                w->folder_split = true;
        release_files(w);

        if (continuing) {
                strcpy(w->prev_name, w->folder_start_name);
                strcpy(w->prev_disk, w->folder_start_disk);
        } else {
                strcpy(w->prev_name, path_basename(w->vol_path));
                strcpy(w->prev_disk, w->vol_disk);
        }
        w->has_prev = true;
        w->vol_open = false;
        w->vol_index++;
        w->cur_folder_vidx = -1;
        w->num_entries = 0;
        w->entries_bytes = 0;
        w->num_folders = 0;
        w->temp_bytes = 0;
        return LIBLZX_CAB_OK;
}

/* Записывает CFDATA (или часть разрезанного блока) во временный файл */
static int
write_cfdata(liblzx_cab_writer_t *w, const uint8_t *data, uint32_t size,
             uint32_t uncomp)
{
        uint8_t hdr[CFDATA_HEADER_SIZE];
        uint32_t csum;
        int err;

        if (!w->temp_handle) {
                w->temp_handle = w->io.open(w->io.userdata, w->temp_path,
                                            LIBLZX_CAB_IO_WRITE);
                if (!w->temp_handle)
                        return set_error(w, LIBLZX_CAB_ERR_OPEN);
                w->temp_created = true;
        }

        put_le16(hdr + 4, size);
        put_le16(hdr + 6, uncomp);
        csum = cab_checksum(data, size, 0);
        csum = cab_checksum(hdr + 4, 4, csum);
        put_le32(hdr, csum);

        err = io_write_all(w, w->temp_handle, hdr, sizeof(hdr));
        if (err)
                return err;
        err = io_write_all(w, w->temp_handle, data, size);
        if (err)
                return err;

        w->folders[w->cur_folder_vidx].num_data++;
        w->temp_bytes += CFDATA_HEADER_SIZE + size;
        w->total_out += CFDATA_HEADER_SIZE + size;
        return LIBLZX_CAB_OK;
}

/*
 * Размещает очередной сжатый блок текущей папки (size байт, соответствующих
 * uncomp несжатым) в томах, при необходимости закрывая тома и разрезая
 * блок.
 */
static int
place_block(liblzx_cab_writer_t *w, const uint8_t *data, uint32_t size,
            uint32_t uncomp)
{
        uint32_t new_placed = w->folder_placed + uncomp;
        int err;

        if (size == 0 || size > CFDATA_MAX_COMPRESSED)
                return set_error(w, LIBLZX_CAB_ERR_COMPRESS);

        for (;;) {
                size_t count;
                uint64_t bytes;
                size_t extra_folders;
                uint64_t base;
                bool files_ok;

                err = open_volume(w);
                if (err)
                        return err;
                extra_folders = (w->cur_folder_vidx < 0) ? 1 : 0;

                /* Место резервируется под записи всех известных файлов
                 * папки: при разрыве папки все они перечисляются в томе. */
                count_unlisted(w, &count, &bytes);
                files_ok = w->num_entries + count <=
                           LIBLZX_CAB_MAX_FILES_PER_VOLUME;
                base = volume_overhead(w, extra_folders, bytes) +
                       w->temp_bytes + CFDATA_HEADER_SIZE;

                /* Блок целиком */
                if (files_ok && base + size <= w->vol_limit) {
                        err = ensure_folder_in_volume(w);
                        if (!err)
                                err = write_cfdata(w, data, size, uncomp);
                        if (!err) {
                                w->folder_placed = new_placed;
                                err = list_files_upto(w, new_placed);
                        }
                        if (err)
                                return err;
                        return report_progress(w, LIBLZX_CAB_EVENT_DATA,
                                               NULL, 0);
                }

                /* Часть блока */
                if (files_ok && base < w->vol_limit && size > 1) {
                        uint64_t avail = w->vol_limit - base;
                        uint32_t part = (avail < size) ? (uint32_t)avail
                                                       : size - 1;

                        err = ensure_folder_in_volume(w);
                        if (!err)
                                err = list_files_upto(w, w->folder_placed);
                        if (!err)
                                err = write_cfdata(w, data, part, 0);
                        if (err)
                                return err;
                        data += part;
                        size -= part;
                } else if (w->temp_bytes == 0) {
                        /* В пустой том не помещается ничего */
                        return set_error(w, files_ok
                                         ? LIBLZX_CAB_ERR_VOLUME_TOO_SMALL
                                         : LIBLZX_CAB_ERR_TOO_MANY_FILES);
                }

                err = close_volume(w, true);
                if (err)
                        return err;
        }
}

/******************************************************************************/
/*                                  Папки                                     */
/******************************************************************************/

static void *
lzx_alloc_cb(void *userdata, size_t size)
{
        liblzx_cab_writer_t *w = (liblzx_cab_writer_t *)userdata;

        return w_alloc(w, size);
}

static void
lzx_free_cb(void *userdata, void *ptr)
{
        liblzx_cab_writer_t *w = (liblzx_cab_writer_t *)userdata;

        w_free(w, ptr);
}

static int
start_folder(liblzx_cab_writer_t *w)
{
        if (w->props.compression == LIBLZX_CAB_COMPRESS_LZX) {
                if (!w->lzx) {
                        liblzx_compress_properties_t lp;

                        memset(&lp, 0, sizeof(lp));
                        lp.lzx_variant = LIBLZX_VARIANT_CAB_DELTA;
                        lp.window_size = (uint32_t)1
                                         << w->props.lzx_window_bits;
                        lp.chunk_granularity = CAB_BLOCK_SIZE;
                        lp.compression_level =
                                (uint16_t)w->props.compression_level;
                        lp.e8_file_size = w->props.e8_disable
                                          ? 0 : w->props.e8_file_size;
                        lp.alloc_func = lzx_alloc_cb;
                        lp.free_func = lzx_free_cb;
                        lp.userdata = w;
                        w->lzx = liblzx_compress_create(&lp);
                        if (!w->lzx)
                                return set_error(w, LIBLZX_CAB_ERR_NOMEM);
                } else {
                        liblzx_compress_reset(w->lzx);
                }
        } else {
                if (!w->stored_buf) {
                        w->stored_buf = (uint8_t *)w_alloc(w, CAB_BLOCK_SIZE);
                        if (!w->stored_buf)
                                return set_error(w, LIBLZX_CAB_ERR_NOMEM);
                }
                w->stored_used = 0;
        }

        w->folder_open = true;
        w->folder_seq++;
        w->folder_fed = 0;
        w->folder_emitted = 0;
        w->folder_placed = 0;
        w->folder_blocks = 0;
        w->cur_folder_vidx = -1;
        w->folder_split = false;
        return LIBLZX_CAB_OK;
}

/* Передаёт готовый сжатый блок в тома */
static int
emit_block(liblzx_cab_writer_t *w, const void *data, size_t size)
{
        uint32_t uncomp = w->folder_fed - w->folder_emitted;
        int err;

        if (uncomp > CAB_BLOCK_SIZE)
                uncomp = CAB_BLOCK_SIZE;
        if (uncomp == 0 || w->folder_blocks >= MAX_FOLDER_BLOCKS)
                return set_error(w, LIBLZX_CAB_ERR_COMPRESS);
        w->folder_emitted += uncomp;
        w->folder_blocks++;
        err = place_block(w, (const uint8_t *)data, (uint32_t)size, uncomp);
        return err;
}

/* Подаёт данные в поток текущей папки */
static int
feed_folder(liblzx_cab_writer_t *w, const uint8_t *data, size_t size)
{
        int err;

        if ((uint64_t)w->folder_fed + size > LIBLZX_CAB_MAX_FOLDER_SIZE)
                return set_error(w, LIBLZX_CAB_ERR_FILE_TOO_LARGE);

        if (w->lzx) {
                while (size > 0) {
                        size_t n = liblzx_compress_add_input(w->lzx, data,
                                                             size);
                        const liblzx_output_chunk_t *chunk;

                        w->folder_fed += (uint32_t)n;
                        w->cur_file->size += (uint32_t)n;
                        w->total_in += n;
                        data += n;
                        size -= n;

                        chunk = liblzx_compress_get_next_chunk(w->lzx);
                        if (chunk) {
                                err = emit_block(w, chunk->data, chunk->size);
                                if (err)
                                        return err;
                                liblzx_compress_release_next_chunk(w->lzx);
                        } else if (n == 0) {
                                return set_error(w, LIBLZX_CAB_ERR_COMPRESS);
                        }
                }
        } else {
                while (size > 0) {
                        size_t n;

                        /* Полный блок отдаётся только когда пришли данные
                         * за ним: так последний блок папки всегда
                         * формируется в flush, когда известны все файлы. */
                        if (w->stored_used == CAB_BLOCK_SIZE) {
                                err = emit_block(w, w->stored_buf,
                                                 CAB_BLOCK_SIZE);
                                if (err)
                                        return err;
                                w->stored_used = 0;
                        }
                        n = CAB_BLOCK_SIZE - w->stored_used;
                        if (n > size)
                                n = size;
                        memcpy(w->stored_buf + w->stored_used, data, n);
                        w->stored_used += (uint32_t)n;
                        w->folder_fed += (uint32_t)n;
                        w->cur_file->size += (uint32_t)n;
                        w->total_in += n;
                        data += n;
                        size -= n;
                }
        }
        return LIBLZX_CAB_OK;
}

/* Завершает текущую папку */
static int
end_folder(liblzx_cab_writer_t *w)
{
        int err;

        if (!w->folder_open)
                return LIBLZX_CAB_OK;

        if (w->lzx) {
                const liblzx_output_chunk_t *chunk;

                liblzx_compress_end_input(w->lzx);
                while ((chunk = liblzx_compress_get_next_chunk(w->lzx))) {
                        err = emit_block(w, chunk->data, chunk->size);
                        if (err)
                                return err;
                        liblzx_compress_release_next_chunk(w->lzx);
                }
        } else if (w->stored_used > 0) {
                err = emit_block(w, w->stored_buf, w->stored_used);
                if (err)
                        return err;
                w->stored_used = 0;
        }

        /* Все принятые данные должны попасть в блоки */
        if (w->folder_emitted != w->folder_fed)
                return set_error(w, LIBLZX_CAB_ERR_COMPRESS);

        if (w->folder_blocks == 0) {
                /* Папка только из пустых файлов: запись CFFOLDER без
                 * блоков данных. */
                size_t count;
                uint64_t bytes;

                err = open_volume(w);
                if (err)
                        return err;
                count_unlisted(w, &count, &bytes);
                if (w->num_entries + count > LIBLZX_CAB_MAX_FILES_PER_VOLUME ||
                    volume_overhead(w, 1, bytes) + w->temp_bytes >
                            w->vol_limit) {
                        if (w->num_entries == 0 && w->temp_bytes == 0)
                                return set_error(
                                        w, LIBLZX_CAB_ERR_VOLUME_TOO_SMALL);
                        err = close_volume(w, true);
                        if (err)
                                return err;
                        err = open_volume(w);
                        if (err)
                                return err;
                }
                err = ensure_folder_in_volume(w);
                if (err)
                        return err;
        }

        err = list_files_upto(w, w->folder_fed);
        if (err)
                return err;

        w->folder_open = false;
        w->folder_split = false;
        w->cur_folder_vidx = -1;
        return LIBLZX_CAB_OK;
}

/******************************************************************************/
/*                               Внешний API                                  */
/******************************************************************************/

void
liblzx_cab_init_properties(liblzx_cab_properties_t *props)
{
        memset(props, 0, sizeof(*props));
        props->compression = LIBLZX_CAB_COMPRESS_LZX;
        props->lzx_window_bits = LIBLZX_CAB_DEFAULT_WINDOW_BITS;
        props->compression_level = LIBLZX_CAB_DEFAULT_LEVEL;
        props->e8_file_size = LIBLZX_CONST_DEFAULT_E8_FILE_SIZE;
}

liblzx_cab_writer_t *
liblzx_cab_create(const liblzx_cab_properties_t *props, int *error)
{
        liblzx_cab_writer_t *w;
        liblzx_alloc_func_t alloc_func;
        int err = LIBLZX_CAB_ERR_INVALID_PARAM;

        if (error)
                *error = LIBLZX_CAB_OK;

        if (!props || !props->cab_path || !props->cab_path[0])
                goto fail;
        if ((props->alloc_func == NULL) != (props->free_func == NULL))
                goto fail;
        if (props->compression != LIBLZX_CAB_COMPRESS_NONE &&
            props->compression != LIBLZX_CAB_COMPRESS_LZX)
                goto fail;
        if (props->compression == LIBLZX_CAB_COMPRESS_LZX &&
            props->lzx_window_bits != 0 &&
            (props->lzx_window_bits < 15 || props->lzx_window_bits > 21))
                goto fail;
        if (strlen(path_basename(props->cab_path)) > LIBLZX_CAB_MAX_NAME ||
            strlen(props->cab_path) >= MAX_PATH_BYTES) {
                err = LIBLZX_CAB_ERR_NAME;
                goto fail;
        }

        alloc_func = props->alloc_func ? props->alloc_func : default_alloc;
        w = (liblzx_cab_writer_t *)alloc_func(props->alloc_userdata,
                                              sizeof(*w));
        if (!w) {
                err = LIBLZX_CAB_ERR_NOMEM;
                goto fail;
        }
        memset(w, 0, sizeof(*w));
        w->props = *props;
        if (!w->props.alloc_func) {
                w->props.alloc_func = default_alloc;
                w->props.free_func = default_free;
        }
        if (w->props.lzx_window_bits == 0)
                w->props.lzx_window_bits = LIBLZX_CAB_DEFAULT_WINDOW_BITS;
        if (w->props.compression_level == 0)
                w->props.compression_level = LIBLZX_CAB_DEFAULT_LEVEL;
        if (w->props.compression_level > 0xFFFF)
                w->props.compression_level = 0xFFFF;
        if (w->props.e8_file_size == 0)
                w->props.e8_file_size = LIBLZX_CONST_DEFAULT_E8_FILE_SIZE;
        w->io = props->io ? *props->io : default_io;
        w->cur_folder_vidx = -1;

        if (w->props.compression == LIBLZX_CAB_COMPRESS_LZX)
                w->type_compress = (uint16_t)(TCOMP_TYPE_LZX |
                                              (w->props.lzx_window_bits << 8));
        else
                w->type_compress = 0;

        w->volume_size = w->props.volume_size;
        if (w->volume_size == 0 ||
            w->volume_size > LIBLZX_CAB_MAX_VOLUME_SIZE)
                w->volume_size = LIBLZX_CAB_MAX_VOLUME_SIZE;

        w->cab_path = w_strdup(w, props->cab_path);
        if (props->temp_path) {
                w->temp_path = w_strdup(w, props->temp_path);
        } else if (w->cab_path) {
                size_t n = strlen(props->cab_path);

                w->temp_path = (char *)w_alloc(w, n + 5);
                if (w->temp_path) {
                        memcpy(w->temp_path, props->cab_path, n);
                        memcpy(w->temp_path + n, ".tmp", 5);
                }
        }
        w->io_buf = (uint8_t *)w_alloc(w, IO_BUF_SIZE);
        w->read_buf = (uint8_t *)w_alloc(w, IO_BUF_SIZE);
        if (!w->cab_path || !w->temp_path || !w->io_buf || !w->read_buf) {
                liblzx_cab_destroy(w);
                err = LIBLZX_CAB_ERR_NOMEM;
                goto fail;
        }
        w->state = STATE_IDLE;
        return w;

fail:
        if (error)
                *error = err;
        return NULL;
}

/*
 * Проверяет, что в открытом томе есть место под запись CFFILE нового файла
 * (с учётом записей остальных файлов текущей папки).  Если места нет, том
 * закрывается; если при этом текущая папка разрывается, она завершается в
 * следующем томе, а новый файл начнёт новую папку.
 */
static int
reserve_entry(liblzx_cab_writer_t *w, size_t name_len)
{
        uint64_t entry = CFFILE_FIXED_SIZE + name_len + 1;
        size_t count;
        uint64_t bytes;
        size_t extra_folders;
        int err;

        if (!w->vol_open)
                return LIBLZX_CAB_OK;

        if (w->folder_open) {
                count_unlisted(w, &count, &bytes);
                extra_folders = (w->cur_folder_vidx < 0) ? 1 : 0;
        } else {
                count = 0;
                bytes = 0;
                extra_folders = 1;
        }
        if (w->num_entries + count + 1 <= LIBLZX_CAB_MAX_FILES_PER_VOLUME &&
            volume_overhead(w, extra_folders, bytes + entry) + w->temp_bytes <=
                    w->vol_limit)
                return LIBLZX_CAB_OK;

        if (w->num_entries == 0 && w->temp_bytes == 0)
                return set_error(w, LIBLZX_CAB_ERR_VOLUME_TOO_SMALL);
        err = close_volume(w, true);
        if (err)
                return err;
        if (w->folder_open && w->folder_split)
                return end_folder(w);
        return LIBLZX_CAB_OK;
}

int
liblzx_cab_begin_file(liblzx_cab_writer_t *w,
                      const liblzx_cab_file_info_t *info)
{
        struct cab_file *f;
        bool non_ascii;
        size_t name_len;
        size_t i;
        int err;

        if (!w)
                return LIBLZX_CAB_ERR_INVALID_PARAM;
        if (w->error)
                return w->error;
        if (w->state != STATE_IDLE)
                return set_error(w, LIBLZX_CAB_ERR_BAD_STATE);
        if (!info || !info->name)
                return set_error(w, LIBLZX_CAB_ERR_INVALID_PARAM);

        name_len = strlen(info->name);
        if (name_len == 0 || name_len > LIBLZX_CAB_MAX_NAME ||
            !utf8_valid(info->name, &non_ascii))
                return set_error(w, LIBLZX_CAB_ERR_NAME);
        if (info->size != LIBLZX_CAB_SIZE_UNKNOWN &&
            info->size > LIBLZX_CAB_MAX_FOLDER_SIZE)
                return set_error(w, LIBLZX_CAB_ERR_FILE_TOO_LARGE);

        /* Новая папка, если текущая разорвана между томами, достигла
         * порога или файл известного размера в неё не помещается. */
        if (w->folder_open && w->folder_split) {
                err = end_folder(w);
                if (err)
                        return err;
        }
        if (w->folder_open && w->folder_fed > 0) {
                bool new_folder = false;

                if (w->props.folder_size_threshold != 0 &&
                    w->folder_fed >= w->props.folder_size_threshold)
                        new_folder = true;
                if (info->size != LIBLZX_CAB_SIZE_UNKNOWN &&
                    w->folder_fed + info->size > LIBLZX_CAB_MAX_FOLDER_SIZE)
                        new_folder = true;
                if (new_folder) {
                        err = end_folder(w);
                        if (err)
                                return err;
                }
        }
        err = reserve_entry(w, name_len);
        if (err)
                return err;
        if (!w->folder_open) {
                err = start_folder(w);
                if (err)
                        return err;
        }

        f = (struct cab_file *)w_alloc(w, sizeof(*f));
        if (!f)
                return set_error(w, LIBLZX_CAB_ERR_NOMEM);
        memset(f, 0, sizeof(*f));
        f->name = w_strdup(w, info->name);
        if (!f->name) {
                w_free(w, f);
                return set_error(w, LIBLZX_CAB_ERR_NOMEM);
        }
        for (i = 0; i < name_len; i++) {
                if (f->name[i] == '/')
                        f->name[i] = '\\';
        }
        f->name_len = name_len;
        f->attribs = (uint16_t)(info->attribs |
                                (non_ascii ? LIBLZX_CAB_ATTRIB_NAME_IS_UTF : 0));
        f->date = info->dos_date;
        f->time = info->dos_time;
        f->folder_seq = w->folder_seq;
        f->offset = w->folder_fed;
        f->declared_size = info->size;
        f->open = true;

        if (w->files_tail)
                w->files_tail->next = f;
        else
                w->files_head = f;
        w->files_tail = f;
        if (!w->next_unlisted)
                w->next_unlisted = f;
        w->cur_file = f;
        w->state = STATE_IN_FILE;
        return LIBLZX_CAB_OK;
}

int
liblzx_cab_write(liblzx_cab_writer_t *w, const void *data, size_t size)
{
        if (!w)
                return LIBLZX_CAB_ERR_INVALID_PARAM;
        if (w->error)
                return w->error;
        if (w->state != STATE_IN_FILE)
                return set_error(w, LIBLZX_CAB_ERR_BAD_STATE);
        if (size == 0)
                return LIBLZX_CAB_OK;
        if (!data)
                return set_error(w, LIBLZX_CAB_ERR_INVALID_PARAM);
        if (w->cur_file->declared_size != LIBLZX_CAB_SIZE_UNKNOWN &&
            w->cur_file->size + (uint64_t)size > w->cur_file->declared_size)
                return set_error(w, LIBLZX_CAB_ERR_SIZE_MISMATCH);
        return feed_folder(w, (const uint8_t *)data, size);
}

/* Записывает окончательный размер текущего файла в уже записанные тома */
static int
apply_size_patches(liblzx_cab_writer_t *w)
{
        uint8_t buf[4];
        size_t i;

        put_le32(buf, w->cur_file->size);
        for (i = 0; i < w->num_patches; i++) {
                const struct size_patch *sp = &w->patches[i];
                void *h = w->io.open(w->io.userdata,
                                     w->vol_paths[sp->volume],
                                     LIBLZX_CAB_IO_UPDATE);
                int err;

                if (!h)
                        return set_error(w, LIBLZX_CAB_ERR_OPEN);
                if (w->io.seek(w->io.userdata, h, sp->offset) != 0) {
                        w->io.close(w->io.userdata, h);
                        return set_error(w, LIBLZX_CAB_ERR_WRITE);
                }
                err = io_write_all(w, h, buf, sizeof(buf));
                if (w->io.close(w->io.userdata, h) != 0 && !err)
                        err = set_error(w, LIBLZX_CAB_ERR_WRITE);
                if (err)
                        return err;
        }
        w->num_patches = 0;
        return LIBLZX_CAB_OK;
}

int
liblzx_cab_end_file(liblzx_cab_writer_t *w)
{
        int err;

        if (!w)
                return LIBLZX_CAB_ERR_INVALID_PARAM;
        if (w->error)
                return w->error;
        if (w->state != STATE_IN_FILE)
                return set_error(w, LIBLZX_CAB_ERR_BAD_STATE);
        if (w->cur_file->declared_size != LIBLZX_CAB_SIZE_UNKNOWN &&
            w->cur_file->size != w->cur_file->declared_size)
                return set_error(w, LIBLZX_CAB_ERR_SIZE_MISMATCH);
        if (w->num_patches > 0) {
                err = apply_size_patches(w);
                if (err)
                        return err;
        }
        w->cur_file->open = false;
        w->cur_file = NULL;
        w->state = STATE_IDLE;

        if (w->props.folder_size_threshold != 0 &&
            w->folder_fed >= w->props.folder_size_threshold)
                return end_folder(w);
        return LIBLZX_CAB_OK;
}

int
liblzx_cab_add_file_from_path(liblzx_cab_writer_t *w, const char *src_path,
                              const liblzx_cab_file_info_t *info)
{
        void *in;
        int err;

        if (!w)
                return LIBLZX_CAB_ERR_INVALID_PARAM;
        if (w->error)
                return w->error;
        if (!src_path)
                return set_error(w, LIBLZX_CAB_ERR_INVALID_PARAM);

        in = w->io.open(w->io.userdata, src_path, LIBLZX_CAB_IO_READ);
        if (!in)
                return set_error(w, LIBLZX_CAB_ERR_OPEN);

        err = liblzx_cab_begin_file(w, info);
        while (!err) {
                size_t n = w->io.read(w->io.userdata, in, w->read_buf,
                                      IO_BUF_SIZE);

                if (n == (size_t)-1) {
                        err = set_error(w, LIBLZX_CAB_ERR_READ);
                        break;
                }
                if (n == 0)
                        break;
                err = liblzx_cab_write(w, w->read_buf, n);
                if (!err)
                        err = report_progress(w, LIBLZX_CAB_EVENT_READ,
                                              NULL, 0);
        }
        if (!err)
                err = liblzx_cab_end_file(w);

        w->io.close(w->io.userdata, in);
        return err;
}

int
liblzx_cab_flush_folder(liblzx_cab_writer_t *w)
{
        if (!w)
                return LIBLZX_CAB_ERR_INVALID_PARAM;
        if (w->error)
                return w->error;
        if (w->state != STATE_IDLE)
                return set_error(w, LIBLZX_CAB_ERR_BAD_STATE);
        return end_folder(w);
}

int
liblzx_cab_finish(liblzx_cab_writer_t *w)
{
        int err;

        if (!w)
                return LIBLZX_CAB_ERR_INVALID_PARAM;
        if (w->error)
                return w->error;
        if (w->state != STATE_IDLE)
                return set_error(w, LIBLZX_CAB_ERR_BAD_STATE);
        if (!w->files_head && w->num_vol_paths == 0)
                return set_error(w, LIBLZX_CAB_ERR_EMPTY);

        err = end_folder(w);
        if (err)
                return err;
        if (w->vol_open) {
                err = close_volume(w, false);
                if (err)
                        return err;
        }
        if (w->temp_created) {
                w->io.remove(w->io.userdata, w->temp_path);
                w->temp_created = false;
        }
        w->state = STATE_FINISHED;
        return LIBLZX_CAB_OK;
}

void
liblzx_cab_destroy(liblzx_cab_writer_t *w)
{
        struct cab_file *f;
        unsigned i;

        if (!w)
                return;

        if (w->out_handle)
                w->io.close(w->io.userdata, w->out_handle);
        if (w->temp_handle)
                w->io.close(w->io.userdata, w->temp_handle);
        if (w->temp_created)
                w->io.remove(w->io.userdata, w->temp_path);

        /* Незавершённый архив удаляется целиком */
        if (w->state != STATE_FINISHED) {
                for (i = 0; i < w->num_vol_paths; i++)
                        w->io.remove(w->io.userdata, w->vol_paths[i]);
        }
        for (i = 0; i < w->num_vol_paths; i++)
                w_free(w, w->vol_paths[i]);
        w_free(w, w->vol_paths);

        f = w->files_head;
        while (f) {
                struct cab_file *next = f->next;

                w_free(w, f->name);
                w_free(w, f);
                f = next;
        }

        if (w->lzx)
                liblzx_compress_destroy(w->lzx);
        w_free(w, w->stored_buf);
        w_free(w, w->entries);
        w_free(w, w->folders);
        w_free(w, w->io_buf);
        w_free(w, w->read_buf);
        w_free(w, w->patches);
        w_free(w, w->cab_path);
        w_free(w, w->temp_path);
        w_free(w, w);
}

int
liblzx_cab_get_error(const liblzx_cab_writer_t *w)
{
        return w ? w->error : LIBLZX_CAB_ERR_INVALID_PARAM;
}

unsigned
liblzx_cab_get_volume_count(const liblzx_cab_writer_t *w)
{
        return w ? w->num_vol_paths : 0;
}

const char *
liblzx_cab_get_volume_path(const liblzx_cab_writer_t *w, unsigned index)
{
        if (!w || index >= w->num_vol_paths)
                return NULL;
        return w->vol_paths[index];
}

const char *
liblzx_cab_strerror(int error)
{
        switch (error) {
        case LIBLZX_CAB_OK:
                return "нет ошибки";
        case LIBLZX_CAB_ERR_NOMEM:
                return "недостаточно памяти";
        case LIBLZX_CAB_ERR_INVALID_PARAM:
                return "недопустимый параметр";
        case LIBLZX_CAB_ERR_BAD_STATE:
                return "неверный порядок вызовов";
        case LIBLZX_CAB_ERR_OPEN:
                return "не удалось открыть файл";
        case LIBLZX_CAB_ERR_READ:
                return "ошибка чтения";
        case LIBLZX_CAB_ERR_WRITE:
                return "ошибка записи";
        case LIBLZX_CAB_ERR_CANCELLED:
                return "операция отменена";
        case LIBLZX_CAB_ERR_NAME:
                return "недопустимое имя файла или тома";
        case LIBLZX_CAB_ERR_FILE_TOO_LARGE:
                return "файл слишком велик для формата CAB";
        case LIBLZX_CAB_ERR_SIZE_MISMATCH:
                return "размер файла не совпал с заявленным";
        case LIBLZX_CAB_ERR_VOLUME_TOO_SMALL:
                return "размер тома слишком мал";
        case LIBLZX_CAB_ERR_TOO_MANY_FILES:
                return "слишком много файлов в томе";
        case LIBLZX_CAB_ERR_TOO_MANY_VOLUMES:
                return "слишком много томов";
        case LIBLZX_CAB_ERR_COMPRESS:
                return "внутренняя ошибка сжатия";
        case LIBLZX_CAB_ERR_EMPTY:
                return "в архив не добавлено ни одного файла";
        default:
                return "неизвестная ошибка";
        }
}
