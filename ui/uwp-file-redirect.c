/*
 * Copyright (c) 2026 Skitzelles - https://github.com/skitzellez
 * MIT License: free to use in any project; keep this notice.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions: the above
 * copyright notice and this permission notice shall be included in all copies
 * or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS",
 * WITHOUT WARRANTY OF ANY KIND.
 *
 * Why: in a UWP app, CreateFileW / CreateFile2 and friends only work where the
 * app container itself has rights, i.e. on an NTFS drive that was given
 * "ALL APPLICATION PACKAGES" access on a PC. On a FAT32 or exFAT stick, or an
 * unprepared NTFS one, they fail with "access denied" even though
 * broadFileSystemAccess is granted: that permission is honoured only by the
 * *FromApp functions, which go through the file broker when the direct call
 * is refused and are otherwise the same call.
 *
 * So the import tables of the app's own modules (xemu, SDL3 and the MinGW
 * DLLs next to it) and of the C runtime (_wopen, fopen, _wstat) are pointed
 * at the FromApp versions. They take the same arguments, and where the direct
 * call is allowed they behave as before. System DLLs are left alone: the
 * FromApp functions themselves call into them.
 */

#include "qemu/osdep.h"

#include <windows.h>
#include <winternl.h>
#include <wchar.h>

#include "uwp-file-redirect.h"

typedef HANDLE (WINAPI *CreateFileFromApp_fn)(LPCWSTR, DWORD, DWORD,
                                              LPSECURITY_ATTRIBUTES, DWORD,
                                              DWORD, HANDLE);
typedef HANDLE (WINAPI *CreateFile2FromApp_fn)(
    LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
typedef BOOL (WINAPI *GetFileAttributesExFromApp_fn)(LPCWSTR,
                                                     GET_FILEEX_INFO_LEVELS,
                                                     LPVOID);
typedef HANDLE (WINAPI *FindFirstFileExFromApp_fn)(LPCWSTR, FINDEX_INFO_LEVELS,
                                                   LPVOID, FINDEX_SEARCH_OPS,
                                                   LPVOID, DWORD);
typedef BOOL (WINAPI *CreateDirectoryFromApp_fn)(LPCWSTR,
                                                 LPSECURITY_ATTRIBUTES);
typedef BOOL (WINAPI *PathFromApp_fn)(LPCWSTR);
typedef BOOL (WINAPI *SetFileAttributesFromApp_fn)(LPCWSTR, DWORD);
typedef BOOL (WINAPI *MoveFileFromApp_fn)(LPCWSTR, LPCWSTR);
typedef BOOL (WINAPI *CopyFileFromApp_fn)(LPCWSTR, LPCWSTR, BOOL);
typedef BOOL (WINAPI *ReplaceFileFromApp_fn)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD,
                                             LPVOID, LPVOID);
typedef BOOL (WINAPI *VirtualProtectFromApp_fn)(PVOID, SIZE_T, ULONG, PULONG);

/* MinGW has no fileapifromapp.h, so these come from kernelbase at run time. */
static CreateFileFromApp_fn p_CreateFileFromAppW;
static CreateFile2FromApp_fn p_CreateFile2FromAppW;
static GetFileAttributesExFromApp_fn p_GetFileAttributesExFromAppW;
static FindFirstFileExFromApp_fn p_FindFirstFileExFromAppW;
static CreateDirectoryFromApp_fn p_CreateDirectoryFromAppW;
static PathFromApp_fn p_DeleteFileFromAppW;
static PathFromApp_fn p_RemoveDirectoryFromAppW;
static SetFileAttributesFromApp_fn p_SetFileAttributesFromAppW;
static MoveFileFromApp_fn p_MoveFileFromAppW;
static CopyFileFromApp_fn p_CopyFileFromAppW;
static ReplaceFileFromApp_fn p_ReplaceFileFromAppW;
static VirtualProtectFromApp_fn p_VirtualProtectFromApp;

/*
 * "\\?\F:\..." (the long-path form) is refused by the broker, and so are
 * forward slashes, which QEMU paths often have. Returns name or buf.
 */
static LPCWSTR plain_path(LPCWSTR name, wchar_t *buf, size_t len)
{
    if (!name) {
        return name;
    }
    if (name[0] == L'\\' && name[1] == L'\\' && name[2] == L'?' &&
        name[3] == L'\\' && name[4] && name[5] == L':') {
        name += 4;
    }
    if (!wcschr(name, L'/') || wcslen(name) >= len) {
        return name;
    }
    wchar_t *o = buf;
    for (LPCWSTR c = name; *c; c++) {
        *o++ = *c == L'/' ? L'\\' : *c;
    }
    *o = 0;
    return buf;
}

#define PLAIN(name) wchar_t name##_buf[MAX_PATH * 2]; \
    name = plain_path(name, name##_buf, MAX_PATH * 2)

static wchar_t *widen(LPCSTR s)
{
    if (!s) {
        return NULL;
    }
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(n * sizeof(wchar_t));
    if (w) {
        MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    }
    return w;
}

/* The first 50 refused opens go to the log, so bug reports show them. */
static void note_refused(LPCWSTR name, DWORD access, DWORD err)
{
    static LONG told;
    if (name && InterlockedIncrement(&told) <= 50) {
        fprintf(stderr, "uwp-file-redirect: open refused (error %lu, access "
                "%08lx): %ls\n", err, access, name);
    }
}

/*
 * The broker answers ERROR_INVALID_PARAMETER for an inheritable handle and for
 * some flags on FAT32/exFAT; the C runtime passes both, and _wopen then fails
 * with EINVAL. So retry the same open without them.
 */
static HANDLE WINAPI CreateFileW_redirect(LPCWSTR name, DWORD access,
                                          DWORD share, LPSECURITY_ATTRIBUTES sa,
                                          DWORD disposition, DWORD flags,
                                          HANDLE templ)
{
    PLAIN(name);
    HANDLE h = p_CreateFileFromAppW(name, access, share, sa, disposition,
                                    flags, templ);
    if (h == INVALID_HANDLE_VALUE && sa) {
        h = p_CreateFileFromAppW(name, access, share, NULL, disposition,
                                 flags, templ);
    }
    if (h == INVALID_HANDLE_VALUE &&
        GetLastError() == ERROR_INVALID_PARAMETER) {
        /* attributes and the access-pattern hints only */
        DWORD plain = flags & (0xFFFF | FILE_FLAG_SEQUENTIAL_SCAN |
                               FILE_FLAG_RANDOM_ACCESS |
                               FILE_FLAG_BACKUP_SEMANTICS);
        h = p_CreateFileFromAppW(name, access, share, NULL, disposition,
                                 plain, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        note_refused(name, access, err);
        SetLastError(err);
    }
    return h;
}

static HANDLE WINAPI CreateFileA_redirect(LPCSTR name, DWORD access,
                                          DWORD share, LPSECURITY_ATTRIBUTES sa,
                                          DWORD disposition, DWORD flags,
                                          HANDLE templ)
{
    wchar_t *w = widen(name);
    HANDLE h = CreateFileW_redirect(w, access, share, sa, disposition, flags,
                                    templ);
    DWORD err = GetLastError();
    free(w);
    SetLastError(err);
    return h;
}

/* winstorecompat turns CreateFileW into CreateFile2, so this one matters. */
static HANDLE WINAPI CreateFile2_redirect(LPCWSTR name, DWORD access,
                                          DWORD share, DWORD disposition,
                                          LPCREATEFILE2_EXTENDED_PARAMETERS ext)
{
    PLAIN(name);
    HANDLE h = p_CreateFile2FromAppW(name, access, share, disposition, ext);
    if (h == INVALID_HANDLE_VALUE && ext &&
        (ext->lpSecurityAttributes ||
         GetLastError() == ERROR_INVALID_PARAMETER)) {
        CREATEFILE2_EXTENDED_PARAMETERS plain = *ext;
        plain.lpSecurityAttributes = NULL;
        plain.hTemplateFile = NULL;
        plain.dwSecurityQosFlags = 0;
        plain.dwFileFlags &= FILE_FLAG_SEQUENTIAL_SCAN |
                             FILE_FLAG_RANDOM_ACCESS |
                             FILE_FLAG_BACKUP_SEMANTICS;
        h = p_CreateFile2FromAppW(name, access, share, disposition, &plain);
    }
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        note_refused(name, access, err);
        SetLastError(err);
    }
    return h;
}

static BOOL WINAPI GetFileAttributesExW_redirect(LPCWSTR name,
                                                 GET_FILEEX_INFO_LEVELS level,
                                                 LPVOID info)
{
    PLAIN(name);
    return p_GetFileAttributesExFromAppW(name, level, info);
}

static DWORD WINAPI GetFileAttributesW_redirect(LPCWSTR name)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW_redirect(name, GetFileExInfoStandard, &data)) {
        return INVALID_FILE_ATTRIBUTES;
    }
    return data.dwFileAttributes;
}

static DWORD WINAPI GetFileAttributesA_redirect(LPCSTR name)
{
    wchar_t *w = widen(name);
    DWORD a = GetFileAttributesW_redirect(w);
    DWORD err = GetLastError();
    free(w);
    SetLastError(err);
    return a;
}

static HANDLE WINAPI FindFirstFileExW_redirect(LPCWSTR name,
                                               FINDEX_INFO_LEVELS level,
                                               LPVOID data,
                                               FINDEX_SEARCH_OPS op,
                                               LPVOID filter, DWORD flags)
{
    PLAIN(name);
    return p_FindFirstFileExFromAppW(name, level, data, op, filter, flags);
}

static HANDLE WINAPI FindFirstFileW_redirect(LPCWSTR name,
                                             LPWIN32_FIND_DATAW data)
{
    return FindFirstFileExW_redirect(name, FindExInfoStandard, data,
                                     FindExSearchNameMatch, NULL, 0);
}

static BOOL WINAPI CreateDirectoryW_redirect(LPCWSTR name,
                                             LPSECURITY_ATTRIBUTES sa)
{
    PLAIN(name);
    return p_CreateDirectoryFromAppW(name, sa);
}

static BOOL WINAPI DeleteFileW_redirect(LPCWSTR name)
{
    PLAIN(name);
    return p_DeleteFileFromAppW(name);
}

static BOOL WINAPI RemoveDirectoryW_redirect(LPCWSTR name)
{
    PLAIN(name);
    return p_RemoveDirectoryFromAppW(name);
}

static BOOL WINAPI SetFileAttributesW_redirect(LPCWSTR name, DWORD attributes)
{
    PLAIN(name);
    return p_SetFileAttributesFromAppW(name, attributes);
}

/*
 * Saving through a temporary file renamed over the old one (settings, EEPROM).
 * MoveFileFromAppW never replaces, so an existing target is replaced with
 * ReplaceFileFromAppW, or deleted first when that is refused; another drive
 * means copy and delete, as MOVEFILE_COPY_ALLOWED asks.
 */
static BOOL WINAPI MoveFileExW_redirect(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    PLAIN(from);
    PLAIN(to);
    if (!to) {
        return p_DeleteFileFromAppW(from);
    }
    if (p_MoveFileFromAppW(from, to)) {
        return TRUE;
    }
    DWORD err = GetLastError();
    if ((flags & MOVEFILE_REPLACE_EXISTING) &&
        (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS)) {
        if (p_ReplaceFileFromAppW(to, from, NULL,
                                  REPLACEFILE_IGNORE_MERGE_ERRORS, NULL,
                                  NULL)) {
            return TRUE;
        }
        if (p_DeleteFileFromAppW(to) && p_MoveFileFromAppW(from, to)) {
            return TRUE;
        }
        err = GetLastError();
    } else if ((flags & MOVEFILE_COPY_ALLOWED) &&
               err == ERROR_NOT_SAME_DEVICE) {
        if (p_CopyFileFromAppW(from, to, !(flags & MOVEFILE_REPLACE_EXISTING)) &&
            p_DeleteFileFromAppW(from)) {
            return TRUE;
        }
        err = GetLastError();
    }
    SetLastError(err);
    return FALSE;
}

static BOOL WINAPI MoveFileW_redirect(LPCWSTR from, LPCWSTR to)
{
    return MoveFileExW_redirect(from, to, MOVEFILE_COPY_ALLOWED);
}

static BOOL WINAPI ReplaceFileW_redirect(LPCWSTR replaced, LPCWSTR replacement,
                                         LPCWSTR backup, DWORD flags,
                                         LPVOID exclude, LPVOID reserved)
{
    PLAIN(replaced);
    PLAIN(replacement);
    PLAIN(backup);
    return p_ReplaceFileFromAppW(replaced, replacement, backup, flags, exclude,
                                 reserved);
}

static const struct {
    const char *name;
    void *to;
} redirects[] = {
    { "CreateFileW", (void *)CreateFileW_redirect },
    { "CreateFileA", (void *)CreateFileA_redirect },
    { "CreateFile2", (void *)CreateFile2_redirect },
    { "GetFileAttributesExW", (void *)GetFileAttributesExW_redirect },
    { "GetFileAttributesW", (void *)GetFileAttributesW_redirect },
    { "GetFileAttributesA", (void *)GetFileAttributesA_redirect },
    { "FindFirstFileExW", (void *)FindFirstFileExW_redirect },
    { "FindFirstFileW", (void *)FindFirstFileW_redirect },
    { "CreateDirectoryW", (void *)CreateDirectoryW_redirect },
    { "DeleteFileW", (void *)DeleteFileW_redirect },
    { "RemoveDirectoryW", (void *)RemoveDirectoryW_redirect },
    { "SetFileAttributesW", (void *)SetFileAttributesW_redirect },
    { "MoveFileExW", (void *)MoveFileExW_redirect },
    { "MoveFileW", (void *)MoveFileW_redirect },
    { "ReplaceFileW", (void *)ReplaceFileW_redirect },
};

/* Points one module's imports of the functions above at the redirects. */
static unsigned redirect_module(BYTE *base)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) {
        return 0;
    }
    unsigned count = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *imp =
             (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
         imp->Name; imp++) {
        IMAGE_THUNK_DATA *thunk = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(
            base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                            : imp->FirstThunk));
        for (; names->u1.AddressOfData; names++, thunk++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            const char *fn =
                (const char *)((IMAGE_IMPORT_BY_NAME *)(base +
                                   names->u1.AddressOfData))->Name;
            for (size_t i = 0; i < ARRAYSIZE(redirects); i++) {
                if (strcmp(fn, redirects[i].name) ||
                    thunk->u1.Function == (ULONG_PTR)redirects[i].to) {
                    continue;
                }
                ULONG old;
                if (p_VirtualProtectFromApp(&thunk->u1.Function,
                                            sizeof(thunk->u1.Function),
                                            PAGE_READWRITE, &old)) {
                    thunk->u1.Function = (ULONG_PTR)redirects[i].to;
                    p_VirtualProtectFromApp(&thunk->u1.Function,
                                            sizeof(thunk->u1.Function), old,
                                            &old);
                    count++;
                }
                break;
            }
        }
    }
    return count;
}

/* Loader list entry fields (x64 PEB/LDR layout, as in xbox-fastmem.c). */
#define LDR_ENTRY_DLL_BASE  0x30
#define LDR_ENTRY_FULL_NAME 0x48
#define LDR_ENTRY_BASE_NAME 0x58

unsigned xemu_uwp_redirect_file_apis(void)
{
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    if (!kb) {
        return 0;
    }
#define BIND(fn) (*(FARPROC *)&p_##fn = GetProcAddress(kb, #fn))
    if (!BIND(CreateFileFromAppW) || !BIND(CreateFile2FromAppW) ||
        !BIND(GetFileAttributesExFromAppW) || !BIND(FindFirstFileExFromAppW) ||
        !BIND(CreateDirectoryFromAppW) || !BIND(DeleteFileFromAppW) ||
        !BIND(RemoveDirectoryFromAppW) || !BIND(SetFileAttributesFromAppW) ||
        !BIND(MoveFileFromAppW) || !BIND(CopyFileFromAppW) ||
        !BIND(ReplaceFileFromAppW) || !BIND(VirtualProtectFromApp)) {
        fprintf(stderr, "uwp-file-redirect: FromApp functions missing\n");
        return 0;
    }
#undef BIND

    /*
     * The app's own modules (the first entry is xemu.exe; SDL3 and the MinGW
     * DLLs sit in the same folder) and the C runtime.
     */
    const BYTE *peb = (const BYTE *)__readgsqword(0x60);
    const BYTE *ldr = *(const BYTE *const *)(peb + 0x18);
    const LIST_ENTRY *head = (const LIST_ENTRY *)(ldr + 0x10);
    const UNICODE_STRING *exe = NULL;
    size_t dir_len = 0;
    unsigned n = 0;
    for (const LIST_ENTRY *e = head->Flink; e != head; e = e->Flink) {
        const BYTE *entry = (const BYTE *)e;
        BYTE *dll_base = *(BYTE *const *)(entry + LDR_ENTRY_DLL_BASE);
        const UNICODE_STRING *full =
            (const UNICODE_STRING *)(entry + LDR_ENTRY_FULL_NAME);
        const UNICODE_STRING *base_name =
            (const UNICODE_STRING *)(entry + LDR_ENTRY_BASE_NAME);
        if (!full->Buffer || !base_name->Buffer) {
            continue;
        }
        if (!exe) {
            exe = full;
            dir_len = (full->Length - base_name->Length) / sizeof(wchar_t);
        }
        bool app_module = full->Length / sizeof(wchar_t) > dir_len &&
                          _wcsnicmp(full->Buffer, exe->Buffer, dir_len) == 0;
        bool crt = base_name->Length == wcslen(L"ucrtbase.dll") * 2 &&
                   _wcsnicmp(base_name->Buffer, L"ucrtbase.dll", 12) == 0;
        if (app_module || crt) {
            n += redirect_module(dll_base);
        }
    }
    fprintf(stderr, "uwp-file-redirect: %u file imports go through the "
            "file broker\n", n);
    return n;
}
