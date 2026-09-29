/*
 * util.c - Small allocation, string and path helpers.
 */
#include "app.h"
#include <stdio.h>
#include <stdlib.h>

void *xmalloc(size_t n)
{
    void *p = HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
    if (!p) {
        MessageBoxW(NULL, L"Out of memory.", APP_NAME, MB_ICONERROR);
        ExitProcess(2);
    }
    return p;
}

wchar_t *wstrdup_n(const wchar_t *s, size_t n)
{
    size_t  len = 0;
    wchar_t *out;
    if (!s) return NULL;
    while (len < n && s[len]) len++;
    out = (wchar_t *)xmalloc((len + 1) * sizeof(wchar_t));
    memcpy(out, s, len * sizeof(wchar_t));
    out[len] = L'\0';
    return out;
}

wchar_t *wide_dup(const wchar_t *s)
{
    return wstrdup_n(s, s ? wcslen(s) : 0);
}

/* Truncating copy that always NUL-terminates. Returns chars actually copied. */
size_t wstr_copy(wchar_t *dst, size_t dstChars, const wchar_t *src, size_t n)
{
    size_t i = 0;
    if (!dst || dstChars == 0) return 0;
    if (!src) { dst[0] = L'\0'; return 0; }
    while (i < n && src[i] && i + 1 < dstChars) { dst[i] = src[i]; i++; }
    dst[i] = L'\0';
    return i;
}

void utf8_to_wide(const char *in, wchar_t *out, size_t outChars)
{
    int n;
    if (!out || outChars == 0) return;
    out[0] = L'\0';
    if (!in) return;
    n = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, (int)outChars);
    if (n <= 0) {
        /* Fall back to the ANSI code page for non-UTF8 registry blobs. */
        n = MultiByteToWideChar(CP_ACP, 0, in, -1, out, (int)outChars);
        if (n <= 0) out[0] = L'\0';
    }
}

/* %LOCALAPPDATA%\PreoMG14Driver */
BOOL get_settings_dir(wchar_t *out, size_t cch)
{
    wchar_t base[MAX_PATH];
    DWORD  n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return FALSE;
    if ((size_t)_snwprintf(out, cch, L"%s\\%s", base, APP_SHORT) < 0) return FALSE;
    out[cch - 1] = L'\0';
    return TRUE;
}

BOOL ensure_settings_dir(void)
{
    wchar_t dir[MAX_PATH];
    if (!get_settings_dir(dir, MAX_PATH)) return FALSE;
    if (GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES) return TRUE;
    return CreateDirectoryW(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}
