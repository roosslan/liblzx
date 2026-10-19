/*
 * Запись CAB-архивов (формат Microsoft Cabinet, [MS-CAB]) со сжатием LZX
 * средствами liblzx или без сжатия.
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

#ifndef LIBLZX_CABWRITER_H
#define LIBLZX_CABWRITER_H

#include <stddef.h>
#include <stdint.h>

#include "liblzx.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Общая схема работы:
 *
 *   liblzx_cab_writer_t *w = liblzx_cab_create(&props, &err);
 *   для каждого файла:
 *       liblzx_cab_begin_file(w, &info);
 *       liblzx_cab_write(w, data, size);    -- сколько угодно раз
 *       liblzx_cab_end_file(w);
 *     или одной функцией liblzx_cab_add_file_from_path(w, path, &info);
 *   liblzx_cab_finish(w);
 *   liblzx_cab_destroy(w);
 *
 * Данные файлов подаются потоком; сжатые блоки текущего тома накапливаются
 * во временном файле, а при закрытии тома записываются в итоговый файл после
 * заголовка и таблиц CFFOLDER/CFFILE.  Все функции, возвращающие int,
 * возвращают LIBLZX_CAB_OK (0) или отрицательный код ошибки
 * liblzx_cab_error.  После первой ошибки (в том числе отмены через обратный
 * вызов прогресса) объект переходит в состояние ошибки: последующие вызовы
 * возвращают тот же код, а liblzx_cab_destroy удаляет временный файл и все
 * созданные этим объектом тома.  Если liblzx_cab_destroy вызвана без
 * успешного liblzx_cab_finish, архив также считается отменённым.
 *
 * Все пути и имена передаются в UTF-8.
 */

typedef struct liblzx_cab_writer liblzx_cab_writer_t;
typedef struct liblzx_cab_properties liblzx_cab_properties_t;
typedef struct liblzx_cab_file_info liblzx_cab_file_info_t;
typedef struct liblzx_cab_progress liblzx_cab_progress_t;
typedef struct liblzx_cab_io liblzx_cab_io_t;

enum liblzx_cab_error {
        LIBLZX_CAB_OK = 0,
        /* Не хватило памяти */
        LIBLZX_CAB_ERR_NOMEM = -1,
        /* Недопустимый параметр */
        LIBLZX_CAB_ERR_INVALID_PARAM = -2,
        /* Неверный порядок вызовов (например, write без begin_file) */
        LIBLZX_CAB_ERR_BAD_STATE = -3,
        /* Не удалось открыть или создать файл */
        LIBLZX_CAB_ERR_OPEN = -4,
        /* Ошибка чтения */
        LIBLZX_CAB_ERR_READ = -5,
        /* Ошибка записи */
        LIBLZX_CAB_ERR_WRITE = -6,
        /* Операция отменена обратным вызовом */
        LIBLZX_CAB_ERR_CANCELLED = -7,
        /* Недопустимое или слишком длинное имя файла либо тома */
        LIBLZX_CAB_ERR_NAME = -8,
        /* Файл не помещается в одну папку CAB (около 2 Гб) */
        LIBLZX_CAB_ERR_FILE_TOO_LARGE = -9,
        /* Фактический размер файла не совпал с заявленным */
        LIBLZX_CAB_ERR_SIZE_MISMATCH = -10,
        /* Размер тома слишком мал даже для одного блока данных */
        LIBLZX_CAB_ERR_VOLUME_TOO_SMALL = -11,
        /* Превышено ограничение формата на число файлов в томе */
        LIBLZX_CAB_ERR_TOO_MANY_FILES = -12,
        /* Превышено ограничение формата на число томов (65536) */
        LIBLZX_CAB_ERR_TOO_MANY_VOLUMES = -13,
        /* Внутренняя ошибка сжатия (переполнение выходного блока) */
        LIBLZX_CAB_ERR_COMPRESS = -14,
        /* В архив не добавлено ни одного файла */
        LIBLZX_CAB_ERR_EMPTY = -15,
};

/* Метод сжатия папок архива */
enum liblzx_cab_compression {
        /* Без сжатия (typeCompress = 0) */
        LIBLZX_CAB_COMPRESS_NONE = 0,
        /* LZX (typeCompress = 3 | (окно << 8)) */
        LIBLZX_CAB_COMPRESS_LZX = 3,
};

/* Атрибуты файла в CFFILE (значения из [MS-CAB]) */
enum liblzx_cab_attrib {
        LIBLZX_CAB_ATTRIB_READONLY = 0x01,
        LIBLZX_CAB_ATTRIB_HIDDEN = 0x02,
        LIBLZX_CAB_ATTRIB_SYSTEM = 0x04,
        LIBLZX_CAB_ATTRIB_ARCHIVE = 0x20,
        LIBLZX_CAB_ATTRIB_EXEC = 0x40,
        /* Имя в UTF-8; выставляется автоматически для не-ASCII имён */
        LIBLZX_CAB_ATTRIB_NAME_IS_UTF = 0x80,
};

enum liblzx_cab_limits {
        /* Наибольший размер тома, допускаемый форматом */
        LIBLZX_CAB_MAX_VOLUME_SIZE = 0x7FFFFFFF,
        /* Наибольшее число файлов (записей CFFILE) в одном томе */
        LIBLZX_CAB_MAX_FILES_PER_VOLUME = 0xFFFF,
        /* Наибольшая длина имени файла или тома в байтах (без нуля) */
        LIBLZX_CAB_MAX_NAME = 255,
        /* Уровень сжатия LZX по умолчанию */
        LIBLZX_CAB_DEFAULT_LEVEL = 50,
        /* Окно LZX по умолчанию (2^21 = 2 Мб) */
        LIBLZX_CAB_DEFAULT_WINDOW_BITS = 21,
        /* Рекомендуемый наименьший размер тома для совместимости с FDI:
         * наибольший блок CFDATA (8 + 32768 + 6144 байт) с запасом на
         * заголовок и записи файлов */
        LIBLZX_CAB_MIN_FDI_VOLUME_SIZE = 64 * 1024,
};

/* Размер файла заранее неизвестен */
#define LIBLZX_CAB_SIZE_UNKNOWN UINT64_MAX

/* Наибольший объём несжатых данных одной папки: 65534 блока по 32 Кб.
 * Файл должен целиком помещаться в одну папку. */
#define LIBLZX_CAB_MAX_FOLDER_SIZE ((uint64_t)65534 * 32768)

/* Режимы открытия файла в liblzx_cab_io_t */
enum liblzx_cab_io_mode {
        /* Чтение существующего файла */
        LIBLZX_CAB_IO_READ = 0,
        /* Создание (с усечением) файла для записи */
        LIBLZX_CAB_IO_WRITE = 1,
        /* Открытие существующего файла для записи без усечения (нужно
         * только вместе с seek, см. ниже) */
        LIBLZX_CAB_IO_UPDATE = 2,
};

/*
 * Функции ввода-вывода.  Писатель читает исходные файлы
 * (liblzx_cab_add_file_from_path), пишет и читает временный файл и пишет тома
 * строго последовательно.  Позиционирование (seek и режим UPDATE) нужно
 * только в одном случае: файл неизвестного размера (LIBLZX_CAB_SIZE_UNKNOWN)
 * оказался разорван между томами.  Тогда поле размера в его записях CFFILE
 * уже записанных томов исправляется после окончания файла.  Если seek равен
 * NULL, такой файл приводит к ошибке LIBLZX_CAB_ERR_INVALID_PARAM.
 * Реализация по умолчанию (liblzx_cab_default_io) основана на stdio; на
 * Windows пути из UTF-8 преобразуются в UTF-16 и открываются через _wfopen.
 */
struct liblzx_cab_io {
        /* Открывает файл; возвращает дескриптор или NULL при ошибке. */
        void *(*open)(void *userdata, const char *path, int mode);
        /* Читает до size байт; возвращает число прочитанных байт (0 в
         * конце файла) или (size_t)-1 при ошибке. */
        size_t (*read)(void *userdata, void *handle, void *buf, size_t size);
        /* Записывает size байт; возвращает size при успехе. */
        size_t (*write)(void *userdata, void *handle, const void *buf,
                        size_t size);
        /* Закрывает файл; возвращает 0 при успехе. */
        int (*close)(void *userdata, void *handle);
        /* Удаляет файл; возвращает 0 при успехе. */
        int (*remove)(void *userdata, const char *path);
        /* Устанавливает позицию от начала файла; возвращает 0 при успехе.
         * Может быть NULL. */
        int (*seek)(void *userdata, void *handle, uint64_t offset);
        /* Параметр, передаваемый всем функциям */
        void *userdata;
};

/* Возвращает реализацию ввода-вывода по умолчанию (stdio). */
const liblzx_cab_io_t *
liblzx_cab_default_io(void);

/* События обратного вызова прогресса */
enum liblzx_cab_event {
        /* В архив помещён очередной блок данных CFDATA */
        LIBLZX_CAB_EVENT_DATA = 0,
        /* Прочитана очередная порция исходного файла
         * (liblzx_cab_add_file_from_path) */
        LIBLZX_CAB_EVENT_READ = 1,
        /* Том полностью записан; volume_path и volume_size описывают его.
         * Исключение — разорванный файл неизвестного размера: поле его
         * размера в этом томе будет исправлено позже (см. liblzx_cab_io). */
        LIBLZX_CAB_EVENT_VOLUME_DONE = 2,
};

struct liblzx_cab_progress {
        /* Событие, см. liblzx_cab_event */
        int event;
        /* Принято несжатых данных всего */
        uint64_t total_in;
        /* Записано данных CFDATA (сжатых, с заголовками блоков) всего */
        uint64_t total_out;
        /* Принято данных текущего файла */
        uint64_t file_in;
        /* Номер текущего тома, начиная с 0 */
        unsigned volume_index;
        /* Для LIBLZX_CAB_EVENT_VOLUME_DONE: путь и размер записанного тома */
        const char *volume_path;
        uint64_t volume_size;
};

/* Обратный вызов прогресса.  Ненулевой результат отменяет создание архива:
 * текущий вызов вернёт LIBLZX_CAB_ERR_CANCELLED. */
typedef int (*liblzx_cab_progress_func_t)(void *userdata,
                                          const liblzx_cab_progress_t *progress);

/*
 * Обратный вызов имени тома.  Вызывается для томов с номерами 1, 2, ... и
 * должен записать в path путь к файлу тома (UTF-8, не более path_size байт
 * вместе с завершающим нулём), а в disk — имя диска для полей
 * szDiskPrev/szDiskNext (можно оставить пустую строку).  Ненулевой результат
 * отменяет создание архива.  Вызов может произойти и для тома, который в
 * итоге не понадобится: имя следующего тома записывается в заголовок
 * предыдущего, поэтому запрашивается заранее.
 */
typedef int (*liblzx_cab_volume_name_func_t)(void *userdata,
                                             unsigned volume_index,
                                             char *path, size_t path_size,
                                             char *disk, size_t disk_size);

struct liblzx_cab_properties {
        /* Путь к первому тому.  Обязателен. */
        const char *cab_path;

        /* Метод сжатия, см. liblzx_cab_compression */
        int compression;

        /* Размер окна LZX в битах: 15..21 (0 — по умолчанию, 21) */
        unsigned lzx_window_bits;

        /* Уровень сжатия liblzx (0 — по умолчанию).  Уровни до 34
         * включительно используют быстрый «ленивый» разбор, выше —
         * близкий к оптимальному. */
        unsigned compression_level;

        /* Параметр E8-трансляции LZX (0 — по умолчанию, 12 Мб) */
        uint32_t e8_file_size;

        /* Ненулевое значение отключает E8-трансляцию */
        int e8_disable;

        /* Наибольший размер тома в байтах.  0 — без ограничения (до
         * LIBLZX_CAB_MAX_VOLUME_SIZE); если данные не помещаются, создаются
         * следующие тома.  Как и FCI, писатель заполняет том полностью,
         * разрезая последний блок CFDATA между томами; папка, разорванная
         * между томами, завершается после уже начатых файлов, и следующий
         * файл начинает новую папку.  Для распаковки средствами Windows
         * (FDI) том должен быть больше наибольшего блока CFDATA
         * (LIBLZX_CAB_MIN_FDI_VOLUME_SIZE): блок, разрезанный более чем на
         * два тома, FDI не собирает даже в наборах, созданных makecab. */
        uint64_t volume_size;

        /* На сколько байт уменьшить первый том (например, под размер
         * программы-распаковщика самораспаковывающегося архива). */
        uint64_t first_volume_reserve;

        /* Порог размера папки: если несжатый объём текущей папки достиг
         * порога, следующий файл начинает новую папку.  0 — одна папка
         * на весь архив, пока позволяет формат. */
        uint64_t folder_size_threshold;

        /* Идентификатор набора томов (setID) */
        uint16_t set_id;

        /* Обратный вызов имени тома; NULL — шаблон <имя>2.cab, <имя>3.cab... */
        liblzx_cab_volume_name_func_t volume_name;

        /* Обратный вызов прогресса; может быть NULL */
        liblzx_cab_progress_func_t progress;

        /* Параметр обратных вызовов volume_name и progress */
        void *userdata;

        /* Путь временного файла для данных тома; NULL — <cab_path>.tmp */
        const char *temp_path;

        /* Функции ввода-вывода; NULL — liblzx_cab_default_io() */
        const liblzx_cab_io_t *io;

        /* Функции выделения памяти; NULL — malloc/free */
        liblzx_alloc_func_t alloc_func;
        liblzx_free_func_t free_func;
        void *alloc_userdata;
};

struct liblzx_cab_file_info {
        /* Имя файла в архиве (UTF-8).  Разделитель путей — «\»; символы
         * «/» заменяются на «\».  Не более LIBLZX_CAB_MAX_NAME байт. */
        const char *name;

        /* Атрибуты (liblzx_cab_attrib).  Флаг NAME_IS_UTF добавляется
         * автоматически, если в имени есть не-ASCII символы. */
        uint16_t attribs;

        /* Дата и время в формате MS-DOS */
        uint16_t dos_date;
        uint16_t dos_time;

        /* Размер файла, если известен, иначе LIBLZX_CAB_SIZE_UNKNOWN.
         * Известный размер позволяет заранее начать новую папку, если файл
         * не помещается в текущую, и записать его в CFFILE тома до
         * окончания файла (при разрыве файла между томами).  По окончании
         * файла размер проверяется. */
        uint64_t size;
};

/* Заполняет структуру свойств значениями по умолчанию (LZX, окно 21,
 * уровень по умолчанию, без ограничения размера тома). */
void
liblzx_cab_init_properties(liblzx_cab_properties_t *props);

/* Создаёт объект записи архива.  Файлы на диске создаются при подаче
 * данных.  При ошибке возвращает NULL и записывает код в *error (если
 * error не NULL). */
liblzx_cab_writer_t *
liblzx_cab_create(const liblzx_cab_properties_t *props, int *error);

/* Начинает новый файл архива. */
int
liblzx_cab_begin_file(liblzx_cab_writer_t *w,
                      const liblzx_cab_file_info_t *info);

/* Добавляет данные текущего файла. */
int
liblzx_cab_write(liblzx_cab_writer_t *w, const void *data, size_t size);

/* Завершает текущий файл. */
int
liblzx_cab_end_file(liblzx_cab_writer_t *w);

/* Добавляет файл с диска, читая его порциями через функции ввода-вывода. */
int
liblzx_cab_add_file_from_path(liblzx_cab_writer_t *w, const char *src_path,
                              const liblzx_cab_file_info_t *info);

/* Закрывает текущую папку; следующий файл начнёт новую папку (новый поток
 * LZX).  Вызывать между файлами. */
int
liblzx_cab_flush_folder(liblzx_cab_writer_t *w);

/* Завершает архив: дописывает последнюю папку и последний том. */
int
liblzx_cab_finish(liblzx_cab_writer_t *w);

/* Освобождает объект.  Если архив не был успешно завершён, удаляет
 * временный файл и все созданные тома. */
void
liblzx_cab_destroy(liblzx_cab_writer_t *w);

/* Возвращает код первой ошибки или LIBLZX_CAB_OK. */
int
liblzx_cab_get_error(const liblzx_cab_writer_t *w);

/* Возвращает число записанных томов. */
unsigned
liblzx_cab_get_volume_count(const liblzx_cab_writer_t *w);

/* Возвращает путь записанного тома с номером index или NULL. */
const char *
liblzx_cab_get_volume_path(const liblzx_cab_writer_t *w, unsigned index);

/* Возвращает текстовое описание кода ошибки (на русском языке, UTF-8). */
const char *
liblzx_cab_strerror(int error);

#ifdef __cplusplus
}
#endif

#endif /* LIBLZX_CABWRITER_H */
