#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int         id;
    const char *name;
} Tool;

#ifdef ENCODER
static const Tool g_tools[] = {
    { 101, "ww2ogg.exe"                     },
    { 102, "packed_codebooks_aoTuV_603.bin" },
    { 103, "mp.exe"                         },
    { 104, "oggre_enc.exe"                  },
    { 105, "oggre_dec.exe"                  },
    { 106, "hdiffz.exe"                     },
};
#else
static const Tool g_tools[] = {
    { 201, "oggre_dec.exe" },
    { 202, "mp.exe"        },
    { 203, "hpatchz.exe"   },
};
#endif

#define NTOOLS (sizeof(g_tools) / sizeof(g_tools[0]))

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char  *p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

typedef struct {
    char **v;
    size_t n;
    size_t cap;
} StrList;

static int sl_push(StrList *l, const char *s)
{
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 64;
        char **nv = (char **)realloc(l->v, nc * sizeof(*nv));
        if (!nv)
            return 0;
        l->v   = nv;
        l->cap = nc;
    }
    l->v[l->n] = dup_str(s);
    if (!l->v[l->n])
        return 0;
    l->n++;
    return 1;
}

static void sl_free(StrList *l)
{
    size_t i;
    for (i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
    l->v   = NULL;
    l->n   = 0;
    l->cap = 0;
}

static void join_path(char *dst, size_t cap, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    if (n && (dir[n - 1] == '\\' || dir[n - 1] == '/'))
        snprintf(dst, cap, "%s%s", dir, name);
    else
        snprintf(dst, cap, "%s\\%s", dir, name);
}

static int make_abs(const char *path, char *out, size_t cap)
{
    DWORD n = GetFullPathNameA(path, (DWORD)cap, out, NULL);
    return n > 0 && n < (DWORD)cap;
}

static const char *file_slash(const char *s)
{
    const char *a = strrchr(s, '\\');
    const char *b = strrchr(s, '/');
    if (!a)
        return b;
    if (!b)
        return a;
    return (a > b) ? a : b;
}

static int has_ext(const char *path, const char *ext)
{
    const char *dot   = strrchr(path, '.');
    const char *slash = file_slash(path);
    if (!dot || (slash && dot < slash))
        return 0;
    return _stricmp(dot + 1, ext) == 0;
}

static void with_ext(const char *src, const char *ext, char *dst, size_t cap)
{
    char *dot;
    char *slash;
    size_t n;

    snprintf(dst, cap, "%s", src);
    dot   = strrchr(dst, '.');
    slash = (char *)file_slash(dst);
    if (dot && (!slash || dot > slash))
        *dot = '\0';
    n = strlen(dst);
    snprintf(dst + n, cap - n, ".%s", ext);
}

static void walk_dir(const char *dir, const char *ext, StrList *out)
{
    char           pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE         h;

    join_path(pattern, sizeof(pattern), dir, "*");
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        char full[MAX_PATH];
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;

        join_path(full, sizeof(full), dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walk_dir(full, ext, out);
        } else if (has_ext(full, ext)) {
            char abs[MAX_PATH];
            if (make_abs(full, abs, sizeof(abs)))
                sl_push(out, abs);
            else
                sl_push(out, full);
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

#ifdef ENCODER
static void delete_by_ext(const char *dir, const char *ext)
{
    StrList l = { 0 };
    size_t  i;

    walk_dir(dir, ext, &l);
    for (i = 0; i < l.n; i++)
        DeleteFileA(l.v[i]);
    sl_free(&l);
}
#endif

static void remove_tree(const char *dir)
{
    char           pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE         h;

    join_path(pattern, sizeof(pattern), dir, "*");
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char full[MAX_PATH];
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;

            join_path(full, sizeof(full), dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_tree(full);
            } else {
                SetFileAttributesA(full, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(full);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

static void remove_tree_retry(const char *dir)
{
    int i;
    for (i = 0; i < 20; i++) {
        remove_tree(dir);
        if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES)
            return;
        Sleep(100);
    }
}

static const unsigned char PKG_MAGIC[8] = { 'W', 'E', 'M', 'T', 'O', 'O', 'L', '1' };

#ifdef ENCODER
static void put_u64le(unsigned char *p, unsigned long long v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}
#endif

static unsigned long long get_u64le(const unsigned char *p)
{
    unsigned long long v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v |= ((unsigned long long)p[i]) << (8 * i);
    return v;
}

static int get_file_size(const char *path, unsigned long long *out)
{
    FILE   *f = fopen(path, "rb");
    __int64 n;

    if (!f)
        return 0;
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    n = _ftelli64(f);
    fclose(f);
    if (n < 0)
        return 0;
    *out = (unsigned long long)n;
    return 1;
}

#ifdef ENCODER
static void truncate_file(const char *path, unsigned long long size)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER li;

    if (h == INVALID_HANDLE_VALUE)
        return;
    li.QuadPart = (LONGLONG)size;
    SetFilePointerEx(h, li, NULL, FILE_BEGIN);
    SetEndOfFile(h);
    CloseHandle(h);
}

static int append_patch(const char *ww, const char *diff)
{
    unsigned long long wwsize, diffsize;
    unsigned char      tr[24];
    char               buf[65536];
    FILE              *fd;
    FILE              *fa;
    size_t             n;
    int                ok = 1;

    if (!get_file_size(ww, &wwsize) || !get_file_size(diff, &diffsize))
        return 0;

    fd = fopen(diff, "rb");
    if (!fd)
        return 0;
    fa = fopen(ww, "ab");
    if (!fa) {
        fclose(fd);
        return 0;
    }

    while ((n = fread(buf, 1, sizeof(buf), fd)) > 0) {
        if (fwrite(buf, 1, n, fa) != n) {
            ok = 0;
            break;
        }
    }
    if (ok && ferror(fd))
        ok = 0;

    if (ok) {
        put_u64le(tr, wwsize);
        put_u64le(tr + 8, diffsize);
        memcpy(tr + 16, PKG_MAGIC, 8);
        if (fwrite(tr, 1, 24, fa) != 24)
            ok = 0;
    }

    fclose(fa);
    fclose(fd);

    if (!ok)
        truncate_file(ww, wwsize);
    return ok;
}

static int is_package(const char *pkg)
{
    unsigned long long size, wwsize, diffsize;
    unsigned char      tr[24];
    unsigned char      head[5];
    FILE              *f;
    int                ok = 0;

    if (!get_file_size(pkg, &size) || size < 24)
        return 0;

    f = fopen(pkg, "rb");
    if (!f)
        return 0;

    if (fread(head, 1, 5, f) == 5 && memcmp(head, "OGGRE", 5) == 0 &&
        _fseeki64(f, (__int64)(size - 24), SEEK_SET) == 0 &&
        fread(tr, 1, 24, f) == 24 &&
        memcmp(tr + 16, PKG_MAGIC, 8) == 0) {
        wwsize  = get_u64le(tr);
        diffsize = get_u64le(tr + 8);
        if (wwsize >= 4 && wwsize <= size - 24 &&
            diffsize == size - 24 - wwsize)
            ok = 1;
    }

    fclose(f);
    return ok;
}
#endif

#ifndef ENCODER
static int extract_patch(const char *pkg, const char *outdiff)
{
    unsigned long long size, wwsize, diffsize, left;
    unsigned char      tr[24];
    unsigned char      head[5];
    char               buf[65536];
    FILE              *f = NULL;
    FILE              *o = NULL;
    int                ok = 0;

    if (!get_file_size(pkg, &size) || size < 24)
        return 0;

    f = fopen(pkg, "rb");
    if (!f)
        return 0;

    if (fread(head, 1, 5, f) != 5 || memcmp(head, "OGGRE", 5) != 0)
        goto done;
    if (_fseeki64(f, (__int64)(size - 24), SEEK_SET) != 0)
        goto done;
    if (fread(tr, 1, 24, f) != 24 || memcmp(tr + 16, PKG_MAGIC, 8) != 0)
        goto done;

    wwsize  = get_u64le(tr);
    diffsize = get_u64le(tr + 8);
    if (wwsize < 4 || wwsize > size - 24 || diffsize != size - 24 - wwsize)
        goto done;

    if (_fseeki64(f, (__int64)wwsize, SEEK_SET) != 0)
        goto done;

    o = fopen(outdiff, "wb");
    if (!o)
        goto done;

    left = diffsize;
    while (left) {
        size_t chunk = (left > sizeof(buf)) ? sizeof(buf) : (size_t)left;
        size_t n = fread(buf, 1, chunk, f);
        if (n != chunk || fwrite(buf, 1, n, o) != n)
            goto done;
        left -= n;
    }
    ok = 1;

done:
    if (f)
        fclose(f);
    if (o) {
        fclose(o);
        if (!ok)
            DeleteFileA(outdiff);
    }
    return ok;
}
#endif

static int make_temp_dir(char *out, size_t cap)
{
    char   tmp[MAX_PATH];
    DWORD  n = GetTempPathA(MAX_PATH, tmp);
    size_t tl;
    int    i;

    if (n == 0 || n >= MAX_PATH)
        return 0;

    tl = strlen(tmp);
    if (tl + 13 > cap)
        return 0;

    for (i = 0; i < 100; i++) {
        unsigned a = (unsigned)GetCurrentProcessId() & 0xFFFFu;
        unsigned b = (unsigned)(GetTickCount() + (DWORD)i * 7919u) & 0xFFFFu;
        memcpy(out, tmp, tl);
        sprintf(out + tl, "wemt%04X%04X", a, b);
        if (CreateDirectoryA(out, NULL))
            return 1;
    }
    return 0;
}

static int extract_tools(const char *dir)
{
    size_t i;

    for (i = 0; i < NTOOLS; i++) {
        HRSRC   hr = FindResourceA(NULL, MAKEINTRESOURCEA(g_tools[i].id), RT_RCDATA);
        HGLOBAL hg;
        void   *data;
        DWORD   size;
        char    path[MAX_PATH];
        FILE   *f;

        if (!hr) {
            printf(" [WemTool] Internal resource missing: %s\n", g_tools[i].name);
            return 0;
        }
        size = SizeofResource(NULL, hr);
        hg   = LoadResource(NULL, hr);
        data = hg ? LockResource(hg) : NULL;
        if (!data) {
            printf(" [WemTool] Cannot read internal resource: %s\n", g_tools[i].name);
            return 0;
        }

        join_path(path, sizeof(path), dir, g_tools[i].name);
        f = fopen(path, "wb");
        if (!f) {
            printf(" [WemTool] Cannot create: %s\n", path);
            return 0;
        }
        if (size && fwrite(data, 1, size, f) != size) {
            fclose(f);
            printf(" [WemTool] Cannot write: %s\n", path);
            return 0;
        }
        fclose(f);
    }
    return 1;
}

static HANDLE g_log  = INVALID_HANDLE_VALUE;
static HANDLE g_null = INVALID_HANDLE_VALUE;

static int start_process(const char *cmdline, PROCESS_INFORMATION *pi)
{
    STARTUPINFOA si;
    char        *cmd = dup_str(cmdline);
    BOOL         ok;

    if (!cmd)
        return 0;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (g_log != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput  = (g_null != INVALID_HANDLE_VALUE)
                        ? g_null : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = g_log;
        si.hStdError  = g_log;
    }

    ZeroMemory(pi, sizeof(*pi));
    ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, pi);
    free(cmd);
    return ok;
}

static ULONGLONG filetime_now(void)
{
    FILETIME        ft;
    ULARGE_INTEGER  u;

    GetSystemTimeAsFileTime(&ft);
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

/* Count files with the given extension that were written at/after "since". */
static unsigned count_recent(const char *dir, const char *ext, ULONGLONG since)
{
    StrList  l = { 0 };
    unsigned n = 0;
    size_t   i;

    walk_dir(dir, ext, &l);
    for (i = 0; i < l.n; i++) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        ULARGE_INTEGER          u;

        if (!GetFileAttributesExA(l.v[i], GetFileExInfoStandard, &fad))
            continue;
        u.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
        u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        if (u.QuadPart >= since)
            n++;
    }
    sl_free(&l);
    return n;
}

#define PROGRESS_WIDTH 28

static void progress_draw(unsigned step, unsigned steps, const char *label,
                          unsigned done, unsigned total)
{
    int fill = 0;
    int i;

    if (total)
        fill = (int)((unsigned long long)done * PROGRESS_WIDTH / total);
    if (fill > PROGRESS_WIDTH)
        fill = PROGRESS_WIDTH;

    printf("\r [%u/%u] %-12s [", step, steps, label);
    for (i = 0; i < PROGRESS_WIDTH; i++)
        putchar(i < fill ? '#' : '-');
    printf("] %u/%u", done, total);
    fflush(stdout);
}

static void progress_done(unsigned step, unsigned steps, const char *label,
                          unsigned done, unsigned total)
{
    progress_draw(step, steps, label, done, total);
    putchar('\n');
    fflush(stdout);
}

static void tool_path(const char *tools, const char *name, char *out, size_t cap)
{
    join_path(out, cap, tools, name);
}

/* Run mp.exe with its console output redirected, and poll the output folder
   to keep a progress bar moving. */
static int run_mp_progress(const char *tools, const char *listname,
                           const char *watch_dir, const char *watch_ext,
                           unsigned total, unsigned step, unsigned steps,
                           const char *label)
{
    char                mp[MAX_PATH];
    char                list[MAX_PATH];
    char                cmd[2 * MAX_PATH + 64];
    PROCESS_INFORMATION pi;
    ULONGLONG           since;
    DWORD               ec = 1;
    unsigned            done;

    tool_path(tools, "mp.exe", mp, sizeof(mp));
    join_path(list, sizeof(list), tools, listname);
    snprintf(cmd, sizeof(cmd), "\"%s\" --input=\"%s\"", mp, list);
    fflush(stdout);

    since = filetime_now();
    if (since > 20000000ULL)
        since -= 20000000ULL;   /* 2 s slack for coarse filesystem clocks */

    if (!start_process(cmd, &pi)) {
        printf(" [WemTool] Could not start mp.exe\n");
        return -1;
    }

    for (;;) {
        DWORD w = WaitForSingleObject(pi.hProcess, 200);

        done = count_recent(watch_dir, watch_ext, since);
        if (done > total)
            done = total;

        if (w == WAIT_TIMEOUT) {
            progress_draw(step, steps, label, done, total);
        } else {
            progress_done(step, steps, label, done, total);
            break;
        }
    }

    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (ec != 0)
        printf(" [WemTool] Warning: one or more files failed in this step.\n");

    return (int)ec;
}

#ifdef ENCODER
static int do_encode(const char *tools)
{
    StrList wems = { 0 };
    StrList tmp;
    FILE   *f;
    char    list[MAX_PATH];
    char    exe[MAX_PATH];
    char    pcb[MAX_PATH];
    size_t  i;
    unsigned total_wem;
    unsigned packaged = 0;
    int     failures = 0;

    walk_dir(".", "wem", &wems);
    if (wems.n == 0) {
        printf(" No .wem files found in this folder.\n\n");
        sl_free(&wems);
        return 0;
    }
    total_wem = (unsigned)wems.n;
    printf(" Found %u .wem file(s)\n\n", total_wem);

    join_path(list, sizeof(list), tools, "wemtool.bin");
    f = fopen(list, "wb");
    if (!f) {
        printf(" [WemTool] Cannot create command list.\n");
        sl_free(&wems);
        return 1;
    }
    tool_path(tools, "ww2ogg.exe", exe, sizeof(exe));
    tool_path(tools, "packed_codebooks_aoTuV_603.bin", pcb, sizeof(pcb));
    for (i = 0; i < wems.n; i++) {
        char out[MAX_PATH];
        with_ext(wems.v[i], "wttmp", out, sizeof(out));
        fprintf(f, "\"%s\" \"%s\" --pcb \"%s\" -o \"%s\"\r\n", exe, wems.v[i], pcb, out);
    }
    fclose(f);
    sl_free(&wems);
    if (run_mp_progress(tools, "wemtool.bin", ".", "wttmp", total_wem,
                        1, 5, "WEM to OGG") != 0)
        failures++;

    tmp = (StrList){ 0 };
    walk_dir(".", "wttmp", &tmp);
    if (tmp.n) {
        join_path(list, sizeof(list), tools, "wemtool2.bin");
        f = fopen(list, "wb");
        if (!f) {
            printf(" [WemTool] Cannot create command list.\n");
            sl_free(&tmp);
            return 1;
        }
        tool_path(tools, "oggre_enc.exe", exe, sizeof(exe));
        for (i = 0; i < tmp.n; i++) {
            char out[MAX_PATH];
            with_ext(tmp.v[i], "ww", out, sizeof(out));
            fprintf(f, "\"%s\" \"%s\" \"%s\"\r\n", exe, tmp.v[i], out);
        }
        fclose(f);
        if (run_mp_progress(tools, "wemtool2.bin", ".", "ww",
                            (unsigned)tmp.n, 2, 5, "OGG to WW") != 0)
            failures++;
    }
    sl_free(&tmp);

    tmp = (StrList){ 0 };
    walk_dir(".", "ww", &tmp);
    if (tmp.n) {
        join_path(list, sizeof(list), tools, "wemtool3.bin");
        f = fopen(list, "wb");
        if (!f) {
            printf(" [WemTool] Cannot create command list.\n");
            sl_free(&tmp);
            return 1;
        }
        tool_path(tools, "oggre_dec.exe", exe, sizeof(exe));
        for (i = 0; i < tmp.n; i++) {
            char out[MAX_PATH];
            with_ext(tmp.v[i], "wtdec", out, sizeof(out));
            fprintf(f, "\"%s\" \"%s\" \"%s\"\r\n", exe, tmp.v[i], out);
        }
        fclose(f);
        if (run_mp_progress(tools, "wemtool3.bin", ".", "wtdec",
                            (unsigned)tmp.n, 3, 5, "WW to REF") != 0)
            failures++;
    }
    sl_free(&tmp);

    tmp = (StrList){ 0 };
    walk_dir(".", "wtdec", &tmp);
    if (tmp.n) {
        join_path(list, sizeof(list), tools, "wemtool4.bin");
        f = fopen(list, "wb");
        if (!f) {
            printf(" [WemTool] Cannot create command list.\n");
            sl_free(&tmp);
            return 1;
        }
        tool_path(tools, "hdiffz.exe", exe, sizeof(exe));
        for (i = 0; i < tmp.n; i++) {
            char out[MAX_PATH];
            char dif[MAX_PATH];
            char name[32];
            with_ext(tmp.v[i], "wem", out, sizeof(out));
            snprintf(name, sizeof(name), "d%04u.diffz", (unsigned)i);
            join_path(dif, sizeof(dif), tools, name);
            fprintf(f, "\"%s\" -c-lzma2-9 -C-no \"%s\" \"%s\" \"%s\"\r\n",
                    exe, tmp.v[i], out, dif);
        }
        fclose(f);
        if (run_mp_progress(tools, "wemtool4.bin", tools, "diffz",
                            (unsigned)tmp.n, 4, 5, "MAKE PATCH") != 0)
            failures++;

        for (i = 0; i < tmp.n; i++) {
            char ww[MAX_PATH];
            char dif[MAX_PATH];
            char name[32];
            snprintf(name, sizeof(name), "d%04u.diffz", (unsigned)i);
            join_path(dif, sizeof(dif), tools, name);
            with_ext(tmp.v[i], "ww", ww, sizeof(ww));
            if (append_patch(ww, dif)) {
                packaged++;
            } else {
                printf("\n [WemTool] Could not package: %s\n", ww);
                failures++;
            }
            DeleteFileA(dif);
            if (i + 1 < tmp.n)
                progress_draw(5, 5, "PACK WW", (unsigned)(i + 1), (unsigned)tmp.n);
        }
        progress_done(5, 5, "PACK WW", (unsigned)tmp.n, (unsigned)tmp.n);
    }
    sl_free(&tmp);

    delete_by_ext(".", "wttmp");
    delete_by_ext(".", "wtdec");

    tmp = (StrList){ 0 };
    walk_dir(".", "ww", &tmp);
    for (i = 0; i < tmp.n; i++) {
        char out[MAX_PATH];
        with_ext(tmp.v[i], "wem", out, sizeof(out));
        if (is_package(tmp.v[i]))
            DeleteFileA(out);
        else
            printf(" [WemTool] Keeping source .wem (no patch inside .ww): %s\n", out);
    }
    sl_free(&tmp);

    printf("\n Done - %u of %u file(s) encoded.\n", packaged, total_wem);
    return failures ? 1 : 0;
}
#endif

#ifndef ENCODER
static int do_decode(const char *tools)
{
    typedef struct {
        char ww[MAX_PATH];
        char wtdec[MAX_PATH];
        char wem[MAX_PATH];
        char diff[MAX_PATH];
        int  legacy;
    } DecItem;

    StrList  wws = { 0 };
    DecItem *items = NULL;
    size_t   nitems = 0, cap = 0;
    FILE    *f1 = NULL, *f2 = NULL;
    char     list1[MAX_PATH], list2[MAX_PATH];
    char     oggre[MAX_PATH], hpatch[MAX_PATH];
    size_t   i, pkgidx = 0;
    unsigned total_ww;
    unsigned restored = 0;
    int      failures = 0;

    walk_dir(".", "ww", &wws);
    if (wws.n == 0) {
        printf(" No .ww packages found in this folder.\n\n");
        sl_free(&wws);
        return 0;
    }
    total_ww = (unsigned)wws.n;
    printf(" Found %u .ww package(s)\n\n", total_ww);

    tool_path(tools, "oggre_dec.exe", oggre, sizeof(oggre));
    tool_path(tools, "hpatchz.exe", hpatch, sizeof(hpatch));
    join_path(list1, sizeof(list1), tools, "wemtoold1.bin");
    join_path(list2, sizeof(list2), tools, "wemtoold2.bin");
    f1 = fopen(list1, "wb");
    f2 = fopen(list2, "wb");
    if (!f1 || !f2) {
        printf(" [WemTool] Cannot create command list.\n");
        if (f1)
            fclose(f1);
        if (f2)
            fclose(f2);
        sl_free(&wws);
        return 1;
    }
    fflush(stdout);

    for (i = 0; i < wws.n; i++) {
        DecItem it;
        char    tmpdiff[MAX_PATH];
        char    name[32];

        memset(&it, 0, sizeof(it));
        snprintf(it.ww, sizeof(it.ww), "%s", wws.v[i]);
        with_ext(it.ww, "wtdec", it.wtdec, sizeof(it.wtdec));
        with_ext(it.ww, "wem", it.wem, sizeof(it.wem));
        with_ext(it.ww, "DIFFZ", it.diff, sizeof(it.diff));

        snprintf(name, sizeof(name), "p%04u.diffz", (unsigned)pkgidx);
        join_path(tmpdiff, sizeof(tmpdiff), tools, name);

        if (extract_patch(it.ww, tmpdiff)) {
            snprintf(it.diff, sizeof(it.diff), "%s", tmpdiff);
            it.legacy = 0;
        } else if (GetFileAttributesA(it.diff) != INVALID_FILE_ATTRIBUTES) {
            it.legacy = 1;
        } else {
            printf(" [WemTool] Skipping (no patch inside): %s\n", it.ww);
            failures++;
            continue;
        }

        DeleteFileA(it.wtdec);

        fprintf(f1, "\"%s\" \"%s\" \"%s\"\r\n", oggre, it.ww, it.wtdec);
        fprintf(f2, "\"%s\" \"%s\" \"%s\" \"%s\"\r\n",
                hpatch, it.wtdec, it.diff, it.wem);
        pkgidx++;

        if (nitems == cap) {
            size_t   nc = cap ? cap * 2 : 64;
            DecItem *nv = (DecItem *)realloc(items, nc * sizeof(*nv));
            if (!nv) {
                printf(" [WemTool] Out of memory.\n");
                failures++;
                break;
            }
            items = nv;
            cap   = nc;
        }
        items[nitems++] = it;
    }
    fclose(f1);
    fclose(f2);

    if (nitems) {
        if (run_mp_progress(tools, "wemtoold1.bin", ".", "wtdec",
                            (unsigned)nitems, 1, 2, "WW to REF") != 0)
            failures++;
        if (run_mp_progress(tools, "wemtoold2.bin", ".", "wem",
                            (unsigned)nitems, 2, 2, "RESTORE WEM") != 0)
            failures++;
    }

    for (i = 0; i < nitems; i++) {
        DecItem *it = &items[i];
        if (GetFileAttributesA(it->wem) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA(it->ww);
            DeleteFileA(it->wtdec);
            if (it->legacy)
                DeleteFileA(it->diff);
            restored++;
        } else {
            printf(" [WemTool] Could not restore: %s\n", it->wem);
            failures++;
        }
    }

    printf("\n Done - %u of %u file(s) restored.\n", restored, total_ww);
    free(items);
    sl_free(&wws);
    return failures ? 1 : 0;
}
#endif

int main(int argc, char **argv)
{
    char               tools[MAX_PATH];
    char               logpath[MAX_PATH];
    SECURITY_ATTRIBUTES sa;
    HANDLE             logh, nullh;
    int                rc;
    int                have_log = 0;

    (void)argc;
    (void)argv;

    SetConsoleTitleA("WemTool");

    if (!make_temp_dir(tools, sizeof(tools))) {
        printf(" [WemTool] Cannot create a temporary folder.\n");
        return 1;
    }
    if (!extract_tools(tools)) {
        remove_tree_retry(tools);
        return 1;
    }

    /* Every tool writes to a private log instead of the console. */
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    join_path(logpath, sizeof(logpath), tools, "tools.log");
    g_log = CreateFileA(logpath, GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log != INVALID_HANDLE_VALUE)
        have_log = 1;

    g_null = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                         OPEN_EXISTING, 0, NULL);

    if (g_log == INVALID_HANDLE_VALUE)
        g_log = CreateFileA("NUL", GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            OPEN_EXISTING, 0, NULL);

#ifdef ENCODER
    printf("\n [WemTool] Encoder\n");
#else
    printf("\n [WemTool] Decoder\n");
#endif

#ifdef ENCODER
    rc = do_encode(tools);
#else
    rc = do_decode(tools);
#endif
    fflush(stdout);

    logh  = g_log;
    nullh = g_null;
    g_log  = INVALID_HANDLE_VALUE;
    g_null = INVALID_HANDLE_VALUE;
    if (logh != INVALID_HANDLE_VALUE)
        CloseHandle(logh);
    if (nullh != INVALID_HANDLE_VALUE && nullh != logh)
        CloseHandle(nullh);

    if (rc != 0) {
        if (have_log && CopyFileA(logpath, "wemtool_error.log", FALSE))
            printf("\n Details saved to wemtool_error.log\n");
    } else {
        DeleteFileA("wemtool_error.log");
    }

    remove_tree_retry(tools);
    return rc;
}