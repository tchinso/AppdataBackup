#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

#define APP_TITLE L"Appdata Backup"
#define CFG_NAME L"AppdataBackup.cfg"
#define WM_LOG_MESSAGE (WM_APP + 1)
#define WM_TASK_DONE   (WM_APP + 2)

#define IDC_BACKUP_APPDATA  1001
#define IDC_BACKUP_STASH    1002
#define IDC_BACKUP_WIKI     1003
#define IDC_BACKUP_ALL      1004
#define IDC_RESTORE_APPDATA 1101
#define IDC_RESTORE_STASH   1102
#define IDC_RESTORE_WIKI    1103
#define IDC_RESTORE_ALL     1104
#define IDC_SETTINGS        1201
#define IDC_STATUS          1202
#define IDC_LOG             1203

#define IDC_SET_APPDATA     2001
#define IDC_SET_STASH       2002
#define IDC_SET_WIKI        2003
#define IDC_SET_THRESHOLD   2004
#define IDC_SET_SAVE        2005
#define IDC_SET_CANCEL      2006
#define IDC_SET_BANDIZIP    2007
#define IDC_SET_BROWSE      2008

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

typedef enum {
    OP_BACKUP_APPDATA = 1,
    OP_BACKUP_STASH,
    OP_BACKUP_WIKI,
    OP_BACKUP_ALL,
    OP_RESTORE_APPDATA,
    OP_RESTORE_STASH,
    OP_RESTORE_WIKI,
    OP_RESTORE_ALL
} Operation;

typedef struct {
    int appdata_level;
    int stash_level;
    int wiki_level;
    double local_threshold_mib;
} Config;

typedef struct {
    wchar_t **items;
    size_t count;
    size_t capacity;
} StringList;

typedef struct {
    wchar_t *data;
    size_t length;
    size_t capacity;
} WideBuffer;

typedef struct {
    wchar_t *path;
    FILETIME time;
} BackupFile;

typedef struct AppState {
    HINSTANCE instance;
    HWND hwnd;
    HWND status;
    HWND log;
    HWND buttons[9];
    HWND settings_hwnd;
    HFONT font;
    volatile LONG busy;
    Config config;
    wchar_t exe_dir[32768];
    wchar_t cfg_path[32768];
    wchar_t bandizip[32768];
} AppState;

typedef struct {
    AppState *app;
    Operation operation;
    wchar_t archive[32768];
} Task;

static wchar_t *wcsdup_heap(const wchar_t *text) {
    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    wchar_t *copy = (wchar_t *)malloc(bytes);
    if (copy) memcpy(copy, text, bytes);
    return copy;
}

static void log_post(AppState *app, const wchar_t *format, ...) {
    wchar_t buffer[4096];
    va_list args;
    va_start(args, format);
    _vsnwprintf(buffer, ARRAY_LEN(buffer) - 1, format, args);
    va_end(args);
    buffer[ARRAY_LEN(buffer) - 1] = L'\0';
    SendMessageW(app->hwnd, WM_LOG_MESSAGE, 0, (LPARAM)buffer);
}

static int path_join(wchar_t *out, size_t cap, const wchar_t *left, const wchar_t *right) {
    size_t ll = wcslen(left), rl = wcslen(right);
    int slash = ll > 0 && left[ll - 1] != L'\\' && left[ll - 1] != L'/';
    if (ll + (size_t)slash + rl + 1 > cap) return 0;
    wcscpy(out, left);
    if (slash) wcscat(out, L"\\");
    wcscat(out, right);
    return 1;
}

static const wchar_t *file_name_part(const wchar_t *path) {
    const wchar_t *a = wcsrchr(path, L'\\');
    const wchar_t *b = wcsrchr(path, L'/');
    const wchar_t *p = a;
    if (!p || (b && b > p)) p = b;
    return p ? p + 1 : path;
}

static int file_exists(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static int directory_exists(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static void string_list_init(StringList *list) {
    memset(list, 0, sizeof(*list));
}

static int string_list_add(StringList *list, const wchar_t *value) {
    if (list->count == list->capacity) {
        size_t next = list->capacity ? list->capacity * 2 : 32;
        wchar_t **items = (wchar_t **)realloc(list->items, next * sizeof(wchar_t *));
        if (!items) return 0;
        list->items = items;
        list->capacity = next;
    }
    list->items[list->count] = wcsdup_heap(value);
    if (!list->items[list->count]) return 0;
    list->count++;
    return 1;
}

static void string_list_free(StringList *list) {
    for (size_t i = 0; i < list->count; ++i) free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int buffer_reserve(WideBuffer *buffer, size_t needed) {
    if (needed <= buffer->capacity) return 1;
    size_t next = buffer->capacity ? buffer->capacity * 2 : 512;
    while (next < needed) next *= 2;
    wchar_t *data = (wchar_t *)realloc(buffer->data, next * sizeof(wchar_t));
    if (!data) return 0;
    buffer->data = data;
    buffer->capacity = next;
    return 1;
}

static int buffer_append(WideBuffer *buffer, const wchar_t *text) {
    size_t add = wcslen(text);
    if (!buffer_reserve(buffer, buffer->length + add + 1)) return 0;
    memcpy(buffer->data + buffer->length, text, (add + 1) * sizeof(wchar_t));
    buffer->length += add;
    return 1;
}

static int buffer_append_quoted(WideBuffer *buffer, const wchar_t *arg) {
    if (!buffer_append(buffer, L"\"")) return 0;
    size_t backslashes = 0;
    for (const wchar_t *p = arg; ; ++p) {
        wchar_t ch = *p;
        if (ch == L'\\') {
            backslashes++;
            continue;
        }
        if (ch == L'\"') {
            for (size_t i = 0; i < backslashes * 2 + 1; ++i)
                if (!buffer_append(buffer, L"\\")) return 0;
            if (!buffer_append(buffer, L"\"")) return 0;
            backslashes = 0;
            continue;
        }
        if (ch == L'\0') {
            for (size_t i = 0; i < backslashes * 2; ++i)
                if (!buffer_append(buffer, L"\\")) return 0;
            break;
        }
        for (size_t i = 0; i < backslashes; ++i)
            if (!buffer_append(buffer, L"\\")) return 0;
        backslashes = 0;
        wchar_t one[2] = {ch, 0};
        if (!buffer_append(buffer, one)) return 0;
    }
    return buffer_append(buffer, L"\"");
}

static void buffer_free(WideBuffer *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static int run_process(const wchar_t *exe, wchar_t *command_line, const wchar_t *working_dir, DWORD *exit_code) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    BOOL ok = CreateProcessW(exe, command_line, NULL, NULL, FALSE, 0,
                             NULL, working_dir, &si, &pi);
    if (!ok) return 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exit_code) *exit_code = code;
    return 1;
}

static int run_bz_simple(AppState *app, const wchar_t *verb, const wchar_t *archive,
                         const wchar_t *output_dir, int overwrite) {
    WideBuffer cmd = {0};
    DWORD code = 1;
    buffer_append_quoted(&cmd, app->bandizip);
    buffer_append(&cmd, L" ");
    buffer_append(&cmd, verb);
    buffer_append(&cmd, L" -y");
    if (overwrite) buffer_append(&cmd, L" -aoa");
    if (output_dir) {
        buffer_append(&cmd, L" ");
        WideBuffer option = {0};
        buffer_append(&option, L"-o:");
        buffer_append(&option, output_dir);
        buffer_append_quoted(&cmd, option.data);
        buffer_free(&option);
    }
    buffer_append(&cmd, L" ");
    buffer_append_quoted(&cmd, archive);
    int launched = run_process(app->bandizip, cmd.data, NULL, &code);
    buffer_free(&cmd);
    if (!launched) {
        log_post(app, L"Bandizip 실행 실패 (Windows 오류 %lu)", GetLastError());
        return 0;
    }
    if (code != 0) {
        log_post(app, L"Bandizip 종료 코드: %lu", code);
        return 0;
    }
    return 1;
}

static int run_archive_batch(AppState *app, const wchar_t *verb, const wchar_t *archive,
                             const wchar_t *working_dir, wchar_t **items, size_t count, int level) {
    wchar_t options[64];
    _snwprintf(options, ARRAY_LEN(options), L" -y -r -l:%d -fmt:zip ", level);
    WideBuffer cmd = {0};
    buffer_append_quoted(&cmd, app->bandizip);
    buffer_append(&cmd, L" ");
    buffer_append(&cmd, verb);
    buffer_append(&cmd, options);
    buffer_append_quoted(&cmd, archive);
    for (size_t i = 0; i < count; ++i) {
        buffer_append(&cmd, L" ");
        buffer_append_quoted(&cmd, items[i]);
    }
    DWORD code = 1;
    int launched = run_process(app->bandizip, cmd.data, working_dir, &code);
    buffer_free(&cmd);
    if (!launched) {
        log_post(app, L"Bandizip 실행 실패 (Windows 오류 %lu)", GetLastError());
        return 0;
    }
    if (code != 0) {
        log_post(app, L"Bandizip 종료 코드: %lu", code);
        return 0;
    }
    return 1;
}

static int create_archive_from_items(AppState *app, const wchar_t *archive,
                                     const wchar_t *working_dir, StringList *items, int level) {
    if (!items->count) return 0;
    size_t start = 0;
    int first = 1;
    while (start < items->count) {
        size_t end = start;
        size_t chars = 512 + wcslen(archive) + wcslen(app->bandizip);
        while (end < items->count) {
            size_t add = wcslen(items->items[end]) * 2 + 4;
            if (end > start && chars + add > 24000) break;
            chars += add;
            ++end;
        }
        if (!run_archive_batch(app, first ? L"c" : L"a", archive, working_dir,
                               &items->items[start], end - start, level)) return 0;
        first = 0;
        start = end;
    }
    return 1;
}

static int folder_size_limited(const wchar_t *folder, uint64_t limit, uint64_t *total) {
    StringList pending;
    string_list_init(&pending);
    wchar_t *pattern = (wchar_t *)malloc(32768 * sizeof(wchar_t));
    wchar_t *child = (wchar_t *)malloc(32768 * sizeof(wchar_t));
    int ok = 1;
    if (!pattern || !child || !string_list_add(&pending, folder)) ok = 0;

    while (ok && pending.count && *total <= limit) {
        wchar_t *current = pending.items[--pending.count];
        if (!path_join(pattern, 32768, current, L"*")) {
            free(current);
            ok = 0;
            break;
        }
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW(pattern, &data);
        if (find == INVALID_HANDLE_VALUE) {
            free(current);
            ok = 0;
            break;
        }
        do {
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (!path_join(child, 32768, current, data.cFileName)) {
                ok = 0;
                break;
            }
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!string_list_add(&pending, child)) {
                    ok = 0;
                    break;
                }
            } else {
                uint64_t size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
                if (*total > limit || size > limit - *total) {
                    *total = limit + 1;
                    break;
                }
                *total += size;
            }
        } while (*total <= limit && FindNextFileW(find, &data));
        FindClose(find);
        free(current);
    }

    string_list_free(&pending);
    free(child);
    free(pattern);
    return ok;
}

static int gather_appdata_items(AppState *app, StringList *items) {
    wchar_t profile[32768];
    DWORD got = GetEnvironmentVariableW(L"USERPROFILE", profile, ARRAY_LEN(profile));
    if (!got || got >= ARRAY_LEN(profile)) return 0;

    wchar_t roaming[32768], locallow[32768], local[32768];
    path_join(roaming, ARRAY_LEN(roaming), profile, L"AppData\\Roaming");
    path_join(locallow, ARRAY_LEN(locallow), profile, L"AppData\\LocalLow");
    path_join(local, ARRAY_LEN(local), profile, L"AppData\\Local");
    if (directory_exists(roaming)) string_list_add(items, L"AppData\\Roaming");
    if (directory_exists(locallow)) string_list_add(items, L"AppData\\LocalLow");
    if (!directory_exists(local)) return items->count > 0;

    wchar_t pattern[32768];
    path_join(pattern, ARRAY_LEN(pattern), local, L"*");
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return items->count > 0;
    uint64_t limit = (uint64_t)(app->config.local_threshold_mib * 1024.0 * 1024.0 + 0.5);
    size_t included = 0, excluded = 0;
    do {
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        wchar_t relative[32768], full[32768];
        _snwprintf(relative, ARRAY_LEN(relative), L"AppData\\Local\\%ls", data.cFileName);
        relative[ARRAY_LEN(relative) - 1] = 0;
        path_join(full, ARRAY_LEN(full), local, data.cFileName);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                ++excluded;
                continue;
            }
            uint64_t total = 0;
            if (folder_size_limited(full, limit, &total) && total <= limit) {
                string_list_add(items, relative);
                ++included;
            } else {
                ++excluded;
            }
        } else {
            string_list_add(items, relative);
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    log_post(app, L"Local 1차 폴더: %zu개 포함, %zu개 제외 (기준 %.1f MiB)",
             included, excluded, app->config.local_threshold_mib);
    return items->count > 0;
}

static int is_backup_name(const wchar_t *name, const wchar_t *prefix) {
    size_t p = wcslen(prefix);
    if (_wcsnicmp(name, prefix, p) || name[p] != L'_') return 0;
    const wchar_t *s = name + p + 1;
    if (wcslen(s) < 12) return 0;
    for (int i = 0; i < 6; ++i) if (!iswdigit(s[i])) return 0;
    if (s[6] != L'-') return 0;
    s += 7;
    if (!iswdigit(*s)) return 0;
    while (iswdigit(*s)) ++s;
    return !_wcsicmp(s, L".zip");
}

static int compare_backup_file(const void *a, const void *b) {
    const BackupFile *fa = (const BackupFile *)a;
    const BackupFile *fb = (const BackupFile *)b;
    return CompareFileTime(&fa->time, &fb->time);
}

static BackupFile *find_backups(AppState *app, const wchar_t *prefix, size_t *count_out) {
    *count_out = 0;
    wchar_t pattern[32768];
    path_join(pattern, ARRAY_LEN(pattern), app->exe_dir, L"*.zip");
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return NULL;
    BackupFile *files = NULL;
    size_t count = 0, cap = 0;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !is_backup_name(data.cFileName, prefix)) continue;
        if (count == cap) {
            size_t next = cap ? cap * 2 : 8;
            BackupFile *grown = (BackupFile *)realloc(files, next * sizeof(BackupFile));
            if (!grown) break;
            files = grown;
            cap = next;
        }
        wchar_t full[32768];
        path_join(full, ARRAY_LEN(full), app->exe_dir, data.cFileName);
        files[count].path = wcsdup_heap(full);
        files[count].time = data.ftLastWriteTime;
        ++count;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    qsort(files, count, sizeof(BackupFile), compare_backup_file);
    *count_out = count;
    return files;
}

static void free_backups(BackupFile *files, size_t count) {
    for (size_t i = 0; i < count; ++i) free(files[i].path);
    free(files);
}

static int latest_backup(AppState *app, const wchar_t *prefix, wchar_t *out, size_t cap) {
    size_t count = 0;
    BackupFile *files = find_backups(app, prefix, &count);
    if (!count) { free(files); return 0; }
    wcsncpy(out, files[count - 1].path, cap - 1);
    out[cap - 1] = 0;
    free_backups(files, count);
    return 1;
}

static void enforce_retention(AppState *app, const wchar_t *prefix) {
    size_t count = 0;
    BackupFile *files = find_backups(app, prefix, &count);
    for (size_t i = 0; i + 3 < count; ++i) {
        if (DeleteFileW(files[i].path))
            log_post(app, L"오래된 백업 삭제: %ls", file_name_part(files[i].path));
        else
            log_post(app, L"오래된 백업 삭제 실패: %ls", file_name_part(files[i].path));
    }
    free_backups(files, count);
}

static int make_backup_path(AppState *app, const wchar_t *prefix, wchar_t *out, size_t cap) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t stem[128];
    _snwprintf(stem, ARRAY_LEN(stem), L"%ls_%02u%02u%02u-", prefix,
               now.wYear % 100, now.wMonth, now.wDay);
    int max_sequence = 0;
    wchar_t pattern[32768];
    _snwprintf(pattern, ARRAY_LEN(pattern), L"%ls\\%ls*.zip", app->exe_dir, stem);
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const wchar_t *number = data.cFileName + wcslen(stem);
            int n = _wtoi(number);
            if (n > max_sequence) max_sequence = n;
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    wchar_t name[256];
    _snwprintf(name, ARRAY_LEN(name), L"%ls%d.zip", stem, max_sequence + 1);
    return path_join(out, cap, app->exe_dir, name);
}

static int backup_one(AppState *app, const wchar_t *prefix, int level) {
    wchar_t archive[32768], temporary[32768], working_dir[32768];
    StringList items;
    string_list_init(&items);
    if (!make_backup_path(app, prefix, archive, ARRAY_LEN(archive))) return 0;
    _snwprintf(temporary, ARRAY_LEN(temporary), L"%ls.partial.zip", archive);
    temporary[ARRAY_LEN(temporary) - 1] = 0;
    DeleteFileW(temporary);

    int ready = 0;
    if (!_wcsicmp(prefix, L"Appdata")) {
        DWORD got = GetEnvironmentVariableW(L"USERPROFILE", working_dir, ARRAY_LEN(working_dir));
        ready = got && got < ARRAY_LEN(working_dir) && gather_appdata_items(app, &items);
    } else if (!_wcsicmp(prefix, L"Stash")) {
        DWORD got = GetEnvironmentVariableW(L"USERPROFILE", working_dir, ARRAY_LEN(working_dir));
        wchar_t source[32768];
        ready = got && got < ARRAY_LEN(working_dir) &&
                path_join(source, ARRAY_LEN(source), working_dir, L".stash") && directory_exists(source);
        if (ready) string_list_add(&items, L".stash");
    } else {
        wcscpy(working_dir, L"C:\\");
        ready = directory_exists(L"C:\\Wiki");
        if (ready) string_list_add(&items, L"Wiki");
    }
    if (!ready || !items.count) {
        log_post(app, L"%ls 원본 폴더를 찾을 수 없습니다.", prefix);
        string_list_free(&items);
        return 0;
    }

    log_post(app, L"%ls 백업 시작 → %ls", prefix, file_name_part(archive));
    int ok = create_archive_from_items(app, temporary, working_dir, &items, level);
    string_list_free(&items);
    if (ok) {
        log_post(app, L"ZIP 무결성 검사 중...");
        ok = run_bz_simple(app, L"t", temporary, NULL, 0);
    }
    if (ok) {
        ok = MoveFileExW(temporary, archive, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok) log_post(app, L"백업 파일 이름 변경 실패 (Windows 오류 %lu)", GetLastError());
    }
    if (!ok) {
        DeleteFileW(temporary);
        log_post(app, L"%ls 백업 실패", prefix);
        return 0;
    }
    log_post(app, L"%ls 백업 완료: %ls", prefix, file_name_part(archive));
    enforce_retention(app, prefix);
    return 1;
}

static int restore_one(AppState *app, const wchar_t *prefix, const wchar_t *archive) {
    wchar_t destination[32768];
    if (!_wcsicmp(prefix, L"PersonalWiki")) {
        wcscpy(destination, L"C:\\");
    } else {
        DWORD got = GetEnvironmentVariableW(L"USERPROFILE", destination, ARRAY_LEN(destination));
        if (!got || got >= ARRAY_LEN(destination)) return 0;
    }
    log_post(app, L"%ls 복원 시작: %ls", prefix, file_name_part(archive));
    int ok = run_bz_simple(app, L"x", archive, destination, 1);
    log_post(app, ok ? L"%ls 복원 완료" : L"%ls 복원 실패", prefix);
    return ok;
}

static DWORD WINAPI worker_thread(LPVOID parameter) {
    Task *task = (Task *)parameter;
    AppState *app = task->app;
    int ok = 1;
    switch (task->operation) {
    case OP_BACKUP_APPDATA: ok = backup_one(app, L"Appdata", app->config.appdata_level); break;
    case OP_BACKUP_STASH: ok = backup_one(app, L"Stash", app->config.stash_level); break;
    case OP_BACKUP_WIKI: ok = backup_one(app, L"PersonalWiki", app->config.wiki_level); break;
    case OP_BACKUP_ALL:
        ok = backup_one(app, L"Appdata", app->config.appdata_level);
        if (!backup_one(app, L"Stash", app->config.stash_level)) ok = 0;
        if (!backup_one(app, L"PersonalWiki", app->config.wiki_level)) ok = 0;
        break;
    case OP_RESTORE_APPDATA: ok = restore_one(app, L"Appdata", task->archive); break;
    case OP_RESTORE_STASH: ok = restore_one(app, L"Stash", task->archive); break;
    case OP_RESTORE_WIKI: ok = restore_one(app, L"PersonalWiki", task->archive); break;
    case OP_RESTORE_ALL: {
        wchar_t archive[32768];
        if (latest_backup(app, L"Appdata", archive, ARRAY_LEN(archive))) {
            if (!restore_one(app, L"Appdata", archive)) ok = 0;
        } else { log_post(app, L"Appdata 백업을 찾지 못했습니다."); ok = 0; }
        if (latest_backup(app, L"Stash", archive, ARRAY_LEN(archive))) {
            if (!restore_one(app, L"Stash", archive)) ok = 0;
        } else { log_post(app, L"Stash 백업을 찾지 못했습니다."); ok = 0; }
        if (latest_backup(app, L"PersonalWiki", archive, ARRAY_LEN(archive))) {
            if (!restore_one(app, L"PersonalWiki", archive)) ok = 0;
        } else { log_post(app, L"PersonalWiki 백업을 찾지 못했습니다."); ok = 0; }
        break;
    }
    default: ok = 0; break;
    }
    PostMessageW(app->hwnd, WM_TASK_DONE, (WPARAM)ok, 0);
    free(task);
    return 0;
}

static int locate_bandizip(AppState *app) {
    wchar_t candidate[32768];
    if (app->bandizip[0] && file_exists(app->bandizip) &&
        !_wcsicmp(file_name_part(app->bandizip), L"Bandizip.exe")) return 1;
    wchar_t program_files[32768];
    DWORD got = GetEnvironmentVariableW(L"ProgramFiles", program_files, ARRAY_LEN(program_files));
    if (path_join(candidate, ARRAY_LEN(candidate), app->exe_dir, L"Bandizip.exe") && file_exists(candidate)) {
        wcscpy(app->bandizip, candidate); return 1;
    }
    if (got && got < ARRAY_LEN(program_files) &&
        path_join(candidate, ARRAY_LEN(candidate), program_files, L"Bandizip\\Bandizip.exe") && file_exists(candidate)) {
        wcscpy(app->bandizip, candidate); return 1;
    }
    app->bandizip[0] = 0;
    return 0;
}

static int valid_level(int value, int fallback) {
    return value == 1 || value == 4 || value == 9 ? value : fallback;
}

static void save_config(AppState *app) {
    wchar_t value[64];
    _snwprintf(value, ARRAY_LEN(value), L"%d", app->config.appdata_level);
    WritePrivateProfileStringW(L"Compression", L"Appdata", value, app->cfg_path);
    _snwprintf(value, ARRAY_LEN(value), L"%d", app->config.stash_level);
    WritePrivateProfileStringW(L"Compression", L"Stash", value, app->cfg_path);
    _snwprintf(value, ARRAY_LEN(value), L"%d", app->config.wiki_level);
    WritePrivateProfileStringW(L"Compression", L"PersonalWiki", value, app->cfg_path);
    _snwprintf(value, ARRAY_LEN(value), L"%.1f", app->config.local_threshold_mib);
    WritePrivateProfileStringW(L"Appdata", L"LocalThresholdMiB", value, app->cfg_path);
    WritePrivateProfileStringW(L"Bandizip", L"Path", app->bandizip, app->cfg_path);
}

static void load_config(AppState *app) {
    app->config.appdata_level = valid_level(GetPrivateProfileIntW(L"Compression", L"Appdata", 4, app->cfg_path), 4);
    app->config.stash_level = valid_level(GetPrivateProfileIntW(L"Compression", L"Stash", 1, app->cfg_path), 1);
    app->config.wiki_level = valid_level(GetPrivateProfileIntW(L"Compression", L"PersonalWiki", 9, app->cfg_path), 9);
    wchar_t threshold[64];
    GetPrivateProfileStringW(L"Appdata", L"LocalThresholdMiB", L"25.6", threshold, ARRAY_LEN(threshold), app->cfg_path);
    wchar_t *end = NULL;
    double parsed = wcstod(threshold, &end);
    app->config.local_threshold_mib = (end != threshold && parsed >= 0.1 && parsed <= 1048576.0) ? parsed : 25.6;
    GetPrivateProfileStringW(L"Bandizip", L"Path", L"", app->bandizip,
                             ARRAY_LEN(app->bandizip), app->cfg_path);
    if (!file_exists(app->cfg_path)) save_config(app);
}

static int browse_bandizip(HWND owner, wchar_t *out, size_t cap) {
    wchar_t file[32768] = L"";
    const wchar_t filter[] = L"Bandizip 실행 파일 (Bandizip.exe)\0Bandizip.exe\0실행 파일 (*.exe)\0*.exe\0\0";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = ARRAY_LEN(file);
    ofn.lpstrTitle = L"Bandizip 실행 파일 선택";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return 0;
    wcsncpy(out, file, cap - 1);
    out[cap - 1] = 0;
    return 1;
}

static int level_to_selection(int level) {
    return level == 1 ? 0 : level == 4 ? 1 : 2;
}

static int selection_to_level(int selection) {
    return selection == 0 ? 1 : selection == 1 ? 4 : 9;
}

static void apply_font(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
}

static HWND make_control(HWND parent, const wchar_t *class_name, const wchar_t *text,
                         DWORD style, int x, int y, int w, int h, int id, HFONT font) {
    HWND control = CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style,
                                   x, y, w, h, parent, (HMENU)(INT_PTR)id,
                                   GetModuleHandleW(NULL), NULL);
    if (control && font) apply_font(control, font);
    return control;
}

static void center_window(HWND hwnd, HWND owner) {
    RECT wr, orc;
    GetWindowRect(hwnd, &wr);
    if (owner) GetWindowRect(owner, &orc);
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &orc, 0);
    int x = orc.left + ((orc.right - orc.left) - (wr.right - wr.left)) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - (wr.bottom - wr.top)) / 2;
    SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    AppState *app = (AppState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lparam;
        app = (AppState *)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
    }
    switch (message) {
    case WM_CREATE: {
        make_control(hwnd, L"STATIC", L"Appdata 압축", 0, 24, 24, 125, 24, 0, app->font);
        make_control(hwnd, L"STATIC", L"Stash 압축", 0, 24, 64, 125, 24, 0, app->font);
        make_control(hwnd, L"STATIC", L"PersonalWiki 압축", 0, 24, 104, 125, 24, 0, app->font);
        make_control(hwnd, L"STATIC", L"Local 폴더 임계값", 0, 24, 153, 145, 24, 0, app->font);
        make_control(hwnd, L"STATIC", L"Bandizip 경로", 0, 24, 201, 125, 24, 0, app->font);
        HWND combos[3];
        combos[0] = make_control(hwnd, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                                 170, 20, 190, 200, IDC_SET_APPDATA, app->font);
        combos[1] = make_control(hwnd, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                                 170, 60, 190, 200, IDC_SET_STASH, app->font);
        combos[2] = make_control(hwnd, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                                 170, 100, 190, 200, IDC_SET_WIKI, app->font);
        for (int i = 0; i < 3; ++i) {
            SendMessageW(combos[i], CB_ADDSTRING, 0, (LPARAM)L"빠름 (1)");
            SendMessageW(combos[i], CB_ADDSTRING, 0, (LPARAM)L"균형 (4)");
            SendMessageW(combos[i], CB_ADDSTRING, 0, (LPARAM)L"압축률 (9)");
        }
        SendMessageW(combos[0], CB_SETCURSEL, level_to_selection(app->config.appdata_level), 0);
        SendMessageW(combos[1], CB_SETCURSEL, level_to_selection(app->config.stash_level), 0);
        SendMessageW(combos[2], CB_SETCURSEL, level_to_selection(app->config.wiki_level), 0);
        wchar_t threshold[64];
        _snwprintf(threshold, ARRAY_LEN(threshold), L"%.1f", app->config.local_threshold_mib);
        make_control(hwnd, L"EDIT", threshold, WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                     170, 148, 120, 28, IDC_SET_THRESHOLD, app->font);
        make_control(hwnd, L"STATIC", L"MiB", 0, 300, 153, 50, 24, 0, app->font);
        make_control(hwnd, L"EDIT", app->bandizip, WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                     24, 229, 276, 28, IDC_SET_BANDIZIP, app->font);
        make_control(hwnd, L"BUTTON", L"찾아보기", WS_TABSTOP,
                     308, 227, 76, 31, IDC_SET_BROWSE, app->font);
        make_control(hwnd, L"BUTTON", L"저장", BS_DEFPUSHBUTTON | WS_TABSTOP,
                     194, 282, 90, 32, IDC_SET_SAVE, app->font);
        make_control(hwnd, L"BUTTON", L"취소", WS_TABSTOP,
                     294, 282, 90, 32, IDC_SET_CANCEL, app->font);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_SET_SAVE) {
            wchar_t text[64];
            GetWindowTextW(GetDlgItem(hwnd, IDC_SET_THRESHOLD), text, ARRAY_LEN(text));
            wchar_t *end = NULL;
            double value = wcstod(text, &end);
            while (end && iswspace(*end)) ++end;
            if (end == text || (end && *end) || value < 0.1 || value > 1048576.0) {
                MessageBoxW(hwnd, L"임계값을 0.1 이상인 MiB 숫자로 입력하세요.", APP_TITLE, MB_OK | MB_ICONWARNING);
                return 0;
            }
            wchar_t bandizip[32768];
            GetWindowTextW(GetDlgItem(hwnd, IDC_SET_BANDIZIP), bandizip, ARRAY_LEN(bandizip));
            if (!file_exists(bandizip) || _wcsicmp(file_name_part(bandizip), L"Bandizip.exe")) {
                MessageBoxW(hwnd, L"유효한 Bandizip.exe 경로를 선택하세요.", APP_TITLE,
                            MB_OK | MB_ICONWARNING);
                return 0;
            }
            app->config.appdata_level = selection_to_level((int)SendDlgItemMessageW(hwnd, IDC_SET_APPDATA, CB_GETCURSEL, 0, 0));
            app->config.stash_level = selection_to_level((int)SendDlgItemMessageW(hwnd, IDC_SET_STASH, CB_GETCURSEL, 0, 0));
            app->config.wiki_level = selection_to_level((int)SendDlgItemMessageW(hwnd, IDC_SET_WIKI, CB_GETCURSEL, 0, 0));
            app->config.local_threshold_mib = value;
            wcscpy(app->bandizip, bandizip);
            save_config(app);
            log_post(app, L"설정을 저장했습니다. (Local 기준 %.1f MiB)", value);
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wparam) == IDC_SET_BROWSE) {
            wchar_t selected[32768];
            if (browse_bandizip(hwnd, selected, ARRAY_LEN(selected)))
                SetWindowTextW(GetDlgItem(hwnd, IDC_SET_BANDIZIP), selected);
            return 0;
        }
        if (LOWORD(wparam) == IDC_SET_CANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (app) {
            app->settings_hwnd = NULL;
            EnableWindow(app->hwnd, TRUE);
            SetForegroundWindow(app->hwnd);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static void open_settings(AppState *app) {
    if (app->settings_hwnd) {
        SetForegroundWindow(app->settings_hwnd);
        return;
    }
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AppdataBackupSettings", L"설정",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                CW_USEDEFAULT, CW_USEDEFAULT, 425, 370,
                                app->hwnd, NULL, app->instance, app);
    if (!hwnd) return;
    app->settings_hwnd = hwnd;
    EnableWindow(app->hwnd, FALSE);
    center_window(hwnd, app->hwnd);
    ShowWindow(hwnd, SW_SHOW);
}

static void set_busy(AppState *app, int busy) {
    InterlockedExchange(&app->busy, busy ? 1 : 0);
    for (size_t i = 0; i < ARRAY_LEN(app->buttons); ++i)
        if (app->buttons[i]) EnableWindow(app->buttons[i], !busy);
    SetWindowTextW(app->status, busy ? L"작업 중... 잠시 기다려 주세요." : L"준비");
}

static const wchar_t *prefix_for_operation(Operation op) {
    if (op == OP_RESTORE_APPDATA) return L"Appdata";
    if (op == OP_RESTORE_STASH) return L"Stash";
    if (op == OP_RESTORE_WIKI) return L"PersonalWiki";
    return NULL;
}

static int choose_archive(AppState *app, const wchar_t *prefix, wchar_t *out, size_t cap) {
    wchar_t file[32768] = L"";
    wchar_t title[256];
    _snwprintf(title, ARRAY_LEN(title), L"복원할 %ls 백업 선택", prefix);
    const wchar_t filter[] = L"ZIP 백업 (*.zip)\0*.zip\0모든 파일 (*.*)\0*.*\0\0";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app->hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = ARRAY_LEN(file);
    ofn.lpstrInitialDir = app->exe_dir;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return 0;
    if (!is_backup_name(file_name_part(file), prefix)) {
        MessageBoxW(app->hwnd, L"선택한 파일 이름이 해당 백업 종류와 맞지 않습니다.", APP_TITLE, MB_OK | MB_ICONWARNING);
        return 0;
    }
    wcsncpy(out, file, cap - 1);
    out[cap - 1] = 0;
    return 1;
}

static void start_task(AppState *app, Operation operation) {
    if (InterlockedCompareExchange(&app->busy, 0, 0)) return;
    if (!locate_bandizip(app)) {
        MessageBoxW(app->hwnd,
                    L"Bandizip.exe를 찾지 못했습니다.\n\nBandizip을 설치하거나 설정에서 Bandizip.exe 경로를 지정해 주세요.",
                    APP_TITLE, MB_OK | MB_ICONERROR);
        return;
    }
    Task *task = (Task *)calloc(1, sizeof(Task));
    if (!task) return;
    task->app = app;
    task->operation = operation;

    const wchar_t *prefix = prefix_for_operation(operation);
    if (prefix && !choose_archive(app, prefix, task->archive, ARRAY_LEN(task->archive))) {
        free(task);
        return;
    }
    if (operation >= OP_RESTORE_APPDATA) {
        const wchar_t *question = operation == OP_RESTORE_ALL
            ? L"각 종류의 최신 백업을 원래 위치에 복원합니다.\n같은 이름의 파일은 모두 덮어씁니다. 계속할까요?"
            : L"선택한 백업을 원래 위치에 복원합니다.\n같은 이름의 파일은 모두 덮어씁니다. 계속할까요?";
        if (MessageBoxW(app->hwnd, question, L"복원 확인", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            free(task);
            return;
        }
    }
    set_busy(app, 1);
    log_post(app, L"----------------------------------------");
    HANDLE thread = CreateThread(NULL, 0, worker_thread, task, 0, NULL);
    if (!thread) {
        set_busy(app, 0);
        free(task);
        MessageBoxW(app->hwnd, L"작업 스레드를 시작하지 못했습니다.", APP_TITLE, MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(thread);
}

static void append_log(HWND edit, const wchar_t *line) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t stamped[4600];
    _snwprintf(stamped, ARRAY_LEN(stamped), L"[%02u:%02u:%02u] %ls\r\n",
               now.wHour, now.wMinute, now.wSecond, line);
    int length = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETSEL, length, length);
    SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)stamped);
    SendMessageW(edit, EM_SCROLLCARET, 0, 0);
}

static void layout_main(AppState *app, int width, int height) {
    int margin = 18;
    int gap = 12;
    int group_width = (width - margin * 2 - gap) / 2;
    int group_y = 16;
    int group_h = 168;
    HWND backup_group = GetDlgItem(app->hwnd, 1301);
    HWND restore_group = GetDlgItem(app->hwnd, 1302);
    MoveWindow(backup_group, margin, group_y, group_width, group_h, TRUE);
    MoveWindow(restore_group, margin + group_width + gap, group_y, group_width, group_h, TRUE);
    int bw = group_width - 32;
    for (int i = 0; i < 4; ++i) {
        MoveWindow(app->buttons[i], margin + 16, group_y + 28 + i * 32, bw, 27, TRUE);
        MoveWindow(app->buttons[i + 4], margin + group_width + gap + 16, group_y + 28 + i * 32, bw, 27, TRUE);
    }
    MoveWindow(app->buttons[8], width - margin - 100, 195, 100, 30, TRUE);
    MoveWindow(app->status, margin, 201, width - margin * 2 - 112, 24, TRUE);
    MoveWindow(app->log, margin, 239, width - margin * 2, height - 257, TRUE);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    AppState *app = (AppState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lparam;
        app = (AppState *)create->lpCreateParams;
        app->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
    }
    switch (message) {
    case WM_CREATE: {
        make_control(hwnd, L"BUTTON", L"백업", BS_GROUPBOX, 0, 0, 0, 0, 1301, app->font);
        make_control(hwnd, L"BUTTON", L"복원", BS_GROUPBOX, 0, 0, 0, 0, 1302, app->font);
        const wchar_t *backup_text[] = {L"Appdata 백업", L"Stash 백업", L"PersonalWiki 백업", L"전부 백업"};
        const wchar_t *restore_text[] = {L"Appdata 복원", L"Stash 복원", L"PersonalWiki 복원", L"전부 복원"};
        int backup_ids[] = {IDC_BACKUP_APPDATA, IDC_BACKUP_STASH, IDC_BACKUP_WIKI, IDC_BACKUP_ALL};
        int restore_ids[] = {IDC_RESTORE_APPDATA, IDC_RESTORE_STASH, IDC_RESTORE_WIKI, IDC_RESTORE_ALL};
        for (int i = 0; i < 4; ++i) {
            app->buttons[i] = make_control(hwnd, L"BUTTON", backup_text[i], WS_TABSTOP, 0, 0, 0, 0, backup_ids[i], app->font);
            app->buttons[i + 4] = make_control(hwnd, L"BUTTON", restore_text[i], WS_TABSTOP, 0, 0, 0, 0, restore_ids[i], app->font);
        }
        app->buttons[8] = make_control(hwnd, L"BUTTON", L"설정", WS_TABSTOP, 0, 0, 0, 0, IDC_SETTINGS, app->font);
        app->status = make_control(hwnd, L"STATIC", L"준비", SS_LEFT, 0, 0, 0, 0, IDC_STATUS, app->font);
        app->log = make_control(hwnd, L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE |
                                ES_READONLY | ES_AUTOVSCROLL, 0, 0, 0, 0, IDC_LOG, app->font);
        RECT rc;
        GetClientRect(hwnd, &rc);
        layout_main(app, rc.right, rc.bottom);
        append_log(app->log, L"준비되었습니다. ZIP과 CFG는 이 EXE가 있는 폴더에 저장됩니다.");
        if (app->bandizip[0]) {
            wchar_t found[34000];
            _snwprintf(found, ARRAY_LEN(found), L"Bandizip: %ls", app->bandizip);
            append_log(app->log, found);
        } else {
            append_log(app->log, L"Bandizip을 찾지 못했습니다. 작업 시작 시 다시 확인합니다.");
        }
        return 0;
    }
    case WM_SIZE:
        if (app && app->log) layout_main(app, LOWORD(lparam), HIWORD(lparam));
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_BACKUP_APPDATA: start_task(app, OP_BACKUP_APPDATA); break;
        case IDC_BACKUP_STASH: start_task(app, OP_BACKUP_STASH); break;
        case IDC_BACKUP_WIKI: start_task(app, OP_BACKUP_WIKI); break;
        case IDC_BACKUP_ALL: start_task(app, OP_BACKUP_ALL); break;
        case IDC_RESTORE_APPDATA: start_task(app, OP_RESTORE_APPDATA); break;
        case IDC_RESTORE_STASH: start_task(app, OP_RESTORE_STASH); break;
        case IDC_RESTORE_WIKI: start_task(app, OP_RESTORE_WIKI); break;
        case IDC_RESTORE_ALL: start_task(app, OP_RESTORE_ALL); break;
        case IDC_SETTINGS: open_settings(app); break;
        }
        return 0;
    case WM_LOG_MESSAGE: {
        const wchar_t *line = (const wchar_t *)lparam;
        if (line) append_log(app->log, line);
        return 0;
    }
    case WM_TASK_DONE: {
        const wchar_t *summary = wparam ? L"작업이 완료되었습니다."
                                         : L"일부 또는 전체 작업이 실패했습니다. 로그를 확인하세요.";
        set_busy(app, 0);
        append_log(app->log, summary);
        MessageBoxW(hwnd, summary, APP_TITLE, MB_OK | (wparam ? MB_ICONINFORMATION : MB_ICONWARNING));
        return 0;
    }
    case WM_CLOSE:
        if (app && InterlockedCompareExchange(&app->busy, 0, 0)) {
            MessageBoxW(hwnd, L"작업이 끝난 뒤 종료해 주세요.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show_command) {
    (void)previous;
    (void)command_line;
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    AppState app;
    memset(&app, 0, sizeof(app));
    app.instance = instance;
    wchar_t exe_path[32768];
    DWORD length = GetModuleFileNameW(NULL, exe_path, ARRAY_LEN(exe_path));
    if (!length || length >= ARRAY_LEN(exe_path)) return 1;
    wchar_t *slash = wcsrchr(exe_path, L'\\');
    if (!slash) return 1;
    *slash = 0;
    wcscpy(app.exe_dir, exe_path);
    path_join(app.cfg_path, ARRAY_LEN(app.cfg_path), app.exe_dir, CFG_NAME);
    load_config(&app);
    locate_bandizip(&app);
    save_config(&app);
    app.font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, HANGUL_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = main_proc;
    wc.lpszClassName = L"AppdataBackupMain";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    if (!RegisterClassExW(&wc)) return 1;
    wc.lpfnWndProc = settings_proc;
    wc.lpszClassName = L"AppdataBackupSettings";
    wc.hIcon = NULL;
    wc.hIconSm = NULL;
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, L"AppdataBackupMain", APP_TITLE,
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 720, 530,
                                NULL, NULL, instance, &app);
    if (!hwnd) return 1;
    center_window(hwnd, NULL);
    ShowWindow(hwnd, show_command);
    UpdateWindow(hwnd);

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        if (app.settings_hwnd && IsDialogMessageW(app.settings_hwnd, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (app.font) DeleteObject(app.font);
    return (int)message.wParam;
}
