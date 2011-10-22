/* Copyright (c) 2011 The MyoMake Project <http://www.myomake.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#if !defined(UNICODE)
#define UNICODE
#endif
#if defined(_MSC_VER) && _MSC_VER >= 1200
#pragma warning(push, 3)
#endif

/*
 * disable or reduce the frequency of...
 *   C4057: indirection to slightly different base types
 *   C4075: slight indirection changes (unsigned short* vs short[])
 *   C4100: unreferenced formal parameter
 *   C4127: conditional expression is constant
 *   C4163: '_rotl64' : not available as an intrinsic function
 *   C4201: nonstandard extension nameless struct/unions
 *   C4244: int to char/short - precision loss
 *   C4514: unreferenced inline function removed
 */
#pragma warning(disable: 4100 4127 4163 4201 4514; once: 4057 4075 4244)

/*
 * Ignore Microsoft's interpretation of secure development
 * and the POSIX string handling API
 */
#if defined(_MSC_VER) && _MSC_VER >= 1400
#define _CRT_SECURE_NO_DEPRECATE
#endif
#pragma warning(disable: 4996)

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0502
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <process.h>
#include <io.h>

#define XPATH_MAX 8192

static int debug = 0;
static const char aslicense[] = ""                                          \
    "Licensed under the Apache License, Version 2.0 (the ""License"");\n"   \
    "you may not use this file except in compliance with the License.\n"    \
    "You may obtain a copy of the License at\n\n"                           \
    "http://www.apache.org/licenses/LICENSE-2.0\n";

static const wchar_t *cygroot = 0;
static wchar_t  windrive[] = { 0, L':', L'\\', 0};
static const wchar_t *pathmatches[] = {
    L"/cygdrive/?/*",
    L"/bin/*",
    L"/dev/*",
    L"/etc/*",
    L"/home/*",
    L"/lib/*",
    L"/lib64/*",
    L"/opt/*",
    L"/proc/*",
    L"/sbin/*",
    L"/usr/*",
    L"/tmp/*",
    L"/var/*",
    0
};

static const wchar_t *optmatch[] = {
    0,
    L"I",
    L"Fa",
    L"Fd",
    L"Fe",
    L"FI",
    L"Fl",
    L"Fm",
    L"Fo",
    L"Fr",
    L"FR",
    L"Tc",
    L"Tp",
    0,
    L"BASE:@",
    L"IDLOUT:",
    L"IMPLIB:",
    L"KEYFILE:",
    L"LIBPATH:",
    L"MANIFESTFILE:",
    L"MAP:",
    L"OUTPUTRESOURCE:",
    L"OUT:",
    L"PGD:",
    L"PDB:",
    L"PDBSTRIPPED:",
    L"TLBOUT:",
    0
};

static void *xmalloc(size_t size)
{
    void *p = calloc(size, 1);
    if (p == 0) {
        _wperror(L"malloc");
        _exit(1);
    }
    return p;
}

static wchar_t **waalloc(size_t size)
{
    return (wchar_t **)xmalloc((size + 1) * sizeof(wchar_t *));
}

static __inline void xfree(void *m)
{
    if (m != 0)
        free(m);
}

static void wafree(wchar_t **array)
{
    wchar_t **ptr = array;

    if (array == 0)
        return;
    while (*ptr != 0)
        xfree(*(ptr++));
    xfree(array);
}

static wchar_t *xwcsdup(const wchar_t *s)
{
    wchar_t *d;
    if (s == 0)
        return 0;
    d = wcsdup(s);
    if (d == 0) {
        _wperror(L"wcsdup");
        _exit(1);
    }
    return d;
}

static wchar_t *xwcsndup(const wchar_t *s, size_t size)
{
    wchar_t *p;

    if (s == 0)
        return 0;
    if (wcslen(s) < size)
        size = wcslen(s);
    p = (wchar_t *)xmalloc((size + 2) * sizeof(wchar_t));
    memcpy(p, s, size * sizeof(wchar_t));
    return p;
}

static wchar_t *xwcsrbrk(const wchar_t *s, const wchar_t *set)
{
    const wchar_t *p = s;

    while (*p != 0)
        p++;
    while (s <= p) {
        const wchar_t *q = set;
        while (*q != 0) {
            if (*p == *q)
                return (wchar_t *)p;
            q++;
        }
        p--;
    }
    return 0;
}

static wchar_t *xgetenv(const wchar_t *s)
{
    wchar_t *d;
    if (s == 0)
        return 0;
    if ((d = _wgetenv(s)) == 0)
        return 0;
    return xwcsdup(d);
}

static wchar_t *xwcsvcat(const wchar_t *str, ...)
{
    wchar_t *cp, *argp, *res;
    size_t  saved_lengths[32];
    int     nargs = 0;
    size_t  len;
    va_list adummy;

    /* Pass one --- find length of required string */
    if (str == 0)
        return 0;

    len = wcslen(str);
    va_start(adummy, str);
    saved_lengths[nargs++] = len;
    while ((cp = va_arg(adummy, wchar_t *)) != NULL) {
        size_t cplen = wcslen(cp);
        if (nargs < 32)
            saved_lengths[nargs++] = cplen;
        len += cplen;
    }
    va_end(adummy);

    /* Allocate the required string */
    res = (wchar_t *)xmalloc((len + 2) * sizeof(wchar_t));
    cp = res;

    /* Pass two --- copy the argument strings into the result space */
    va_start(adummy, str);

    nargs = 0;
    len = saved_lengths[nargs++];
    memcpy(cp, str, len * sizeof(wchar_t));
    cp += len;

    while ((argp = va_arg(adummy, wchar_t *)) != NULL) {
        if (nargs < 32)
            len = saved_lengths[nargs++];
        else
            len = wcslen(argp);
        memcpy(cp, argp, len * sizeof(wchar_t));
        cp += len;
    }

    va_end(adummy);
    return res;
}

/* Match = 0, NoMatch = 1, Abort = -1
 * Based loosely on sections of wildmat.c by Rich Salz
 */
static int wchrimatch(const wchar_t *str, const wchar_t *exp, int *match)
{
    int x, y, d;

    if (match == 0)
        match = &d;
    for (x = 0, y = 0; exp[y]; ++y, ++x) {
        if (!str[x] && exp[y] != L'*')
            return -1;
        if (exp[y] == L'*') {
            while (exp[++y] == L'*');
            if (!exp[y])
                return 0;
            while (str[x]) {
                int ret;
                *match = x;
                if ((ret = wchrimatch(&str[x++], &exp[y], match)) != 1)
                    return ret;
            }
            *match = 0;
            return -1;
        }
        else if (exp[y] != L'?') {
            if (towlower(str[x]) != towlower(exp[y]))
                return 1;
        }
    }
    return (str[x] != L'\0');
}

/**
 * Check if the argument is special windows cmdline option.
 * Some programs have some weird command line option where the
 * patch can be integral part of option name.
 * Eg. cl.exe has /Fo/some/path.
 */
static wchar_t *checkdefs(wchar_t *str, int *idx)
{
    wchar_t *ml;
    int i = 1;

    *idx = 0;
    if (*str == L'-' && *(str + 1) == L'-') {
        /* Check for --foo=/...
         */
        if ((ml = wcschr(str, L'='))) {
            if (*(ml + 1) == L'"')
                ++ml;
            if (*(ml + 1) == L'/' && ml < wcschr(str, L'/'))
                return ml + 1;
        }
        return 0;
    }
    if (*str == L'-' || *str == L'/') {
        str++;
        while (optmatch[i] != 0) {
            if (wcsncmp(str, optmatch[i], wcslen(optmatch[i])) == 0) {
                *idx = i;
                return str + wcslen(optmatch[i]);
            }
            i++;
        }
        i++; /* Skip NULL separator for icase options */
        while (optmatch[i] != 0) {
            if (wcsnicmp(str, optmatch[i], wcslen(optmatch[i])) == 0) {
                *idx = i;
                return str + wcslen(optmatch[i]);
            }
            i++;
        }
    }
    if ((iswalnum(*str) || *str == L'_') && (ml = wcschr(str, L'='))) {
        if (*(ml + 1) == L'"')
            ++ml;
        /* Special case for environment variables */
        if (*(ml + 1) == '/' && ml < wcschr(str, L'/'))
            return ml + 1;
    }
    return 0;
}

static int iscygwinpath(const wchar_t *str)
{
    const wchar_t  *equ;
    const wchar_t **mp = pathmatches;

    if ((equ = wcschr(str, L'=')) != 0) {
        /* Special case for environment variables
         * We check here for FOO=/... and if thats
         * the case we then check if this is cygwin path
         */
        if (*(equ + 1) == L'"')
            ++equ;
        if (*(equ + 1) == L'/' && equ < wcschr(str, L'/'))
            str =  equ + 1;
    }
    while (*mp != 0) {
        if (wchrimatch(str, *mp, 0) == 0)
            return 1;
        mp++;
    }
    return 0;
}

static int envsort(const void *arg1, const void *arg2)
{
    /* Compare all of both strings: */
    return _wcsicmp( *(wchar_t **)arg1, *(wchar_t **)arg2);
}

static wchar_t **splitpath(const wchar_t *str, size_t *tokens)
{
    int c = 1;
    wchar_t **sa = 0;
    const wchar_t *b;
    const wchar_t *e;
    const wchar_t *s;

    b = s = str;
    while (*b != L'\0') {
        if (*b++ == L':')
            c++;
    }
#if 0
    while ((e = wcschr(b, L':'))) {
        c++;
        b = e + 1;
    }
#endif
    sa = waalloc(c);
    c  = 0;
    b  = s = str;
    while ((e = wcschr(b, L':'))) {
        int cn = 1;
        int ch = *(e + 1);
        if (ch == L'/' || ch == L'.' || ch == L':' || ch == L'\0') {
            /* Is the previous token path or flag */
            if (iscygwinpath(s)) {
                int cp = c++;
                while ((ch = *(e + cn)) == L':') {
                    sa[c++] = xwcsdup(L"");
                    cn++;
                }
                sa[cp] = xwcsndup(b, e - b);
                s = e + cn;
            }
        }
        b = e + cn;
    }
    sa[c++] = xwcsdup(s);
    if (tokens != 0)
        *tokens = c;
    return sa;
}

static wchar_t *mergepath(wchar_t * const *paths)
{
    size_t len = 0;
    wchar_t *rv;
    wchar_t *const *pp;

    pp = paths;
    while (*pp != 0) {
        len += wcslen(*pp) + 1;
        pp++;
    }
    rv = xmalloc((len + 1) * sizeof(wchar_t));
    pp = paths;
    while (*pp != 0) {
        if (pp != paths)
            wcscat(rv, L";");
        wcscat(rv, *pp);
        pp++;
    }
    return rv;
}

static int is_valid_fnchar[128] = {
    /* Reject all ctrl codes... Escape \n and \r (ascii 10 and 13)      */
       0,0,0,0,0,0,0,0,0,0,2,0,0,2,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /*   ! " # $ % & ' ( ) * + , - . /  0 1 2 3 4 5 6 7 8 9 : ; < = > ? */
       1,1,2,1,3,3,3,3,3,3,2,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,0,3,2,1,2,2,
    /* @ A B C D E F G H I J K L M N O  P Q R S T U V W X Y Z [ \ ] ^ _ */
       1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,3,2,3,3,1,
    /* ` a b c d e f g h i j k l m n o  p q r s t u v w x y z { | } ~   */
       3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,3,2,3,3,1
};

static __inline int is_fnchar(wchar_t c)
{
    if (c > 127)
        return 1;
    else
        return is_valid_fnchar[c] & 1;
}

static __inline void fs2bs(wchar_t *s)
{
    wchar_t *p = s;
    while (is_fnchar(*p)) {
        if (*p == L'/')
            *p = L'\\';
        p++;
    }
}

static wchar_t *posix2win(const wchar_t *root, const wchar_t *str)
{
    wchar_t *rv = 0;
    wchar_t **pa;
    size_t i, tokens;
    wchar_t so[4] = { 0, 0, 0, 0 }; /* Storage for option flag */

    if (wcschr(str, L'/') == 0) {
        /* Nothing to do */
        return 0;
    }
    pa = splitpath(str, &tokens);
    for (i = 0; i < tokens; i++) {
        size_t mx = 0;
        int   matched = 0;
        int   mcmatch = 0;
        int   dx = 0;
        wchar_t *pp;
        wchar_t  eq[2] = { 0, 0};
        const wchar_t  *ep = 0;
        const wchar_t **mp = pathmatches;

        if ((pp = checkdefs(pa[i], &dx)) != 0) {
            /* Special case for environment variables */
            if (dx == 0) {
                ep = pa[i];
                eq[0] = *(pp - 1);
                *(pp - 1) = L'\0';
            }
            else {
                eq[0] = L'=';
                so[0] = *(pa[i]);
                ep = optmatch[dx];
            }
            mcmatch = 1;
        }
        else
            pp = pa[i];
        while (*mp != 0) {
            int match = 0;
            if (wchrimatch(pp, *mp, &match) == 0) {
                wchar_t *lp = pp + match;
                const wchar_t *wp;
                int drive = windrive[0];
                if (mx == 0) {
                    /* /cygdrive/x/... absolute path */
                    windrive[0] = towupper(pp[10]);
                    wp  = windrive;
                    lp += wcslen(*mp + 1);
                }
                else {
                    /* Posix internal path */
                    wp  = root;
                }
                fs2bs(lp);
                rv = pa[i];
                if (ep) {
                    if (dx)
                        pa[i] = xwcsvcat(so, ep, wp, lp, 0);
                    else
                        pa[i] = xwcsvcat(ep, eq, wp, lp, 0);
                }
                else
                    pa[i] = xwcsvcat(wp, lp, 0);
                xfree(rv);
                matched = 1;
                windrive[0] = drive;
                break;
            }
            mx++;
            mp++;
        }
        if (!matched && mcmatch) {
            /* We didn't have a match */
            fs2bs(pp);
            rv = pa[i];
            pa[i] = xwcsvcat(ep, L"=", pp, 0);
            xfree(rv);
        }
    }
    rv = mergepath(pa);
    wafree(pa);
    return rv;
}


static BOOL WINAPI console_handler(DWORD ctrl)
{
    switch (ctrl) {
        case CTRL_BREAK_EVENT:
            return FALSE;
        case CTRL_C_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
        case CTRL_LOGOFF_EVENT:
            return TRUE;
        break;
    }
    return FALSE;
}

wchar_t *getpexe(DWORD pid)
{
    wchar_t buf[XPATH_MAX];
    wchar_t *pp = 0;
    DWORD  ppid = 0;
    HANDLE h;
    PROCESSENTRY32W e;

    h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    e.dwSize = (DWORD)sizeof(PROCESSENTRY32W);
    if (!Process32FirstW(h, &e)) {
        CloseHandle(h);
        return 0;
    }
    do {
        if (e.th32ProcessID == pid) {
            /* We found ourself :)
             */
            ppid = e.th32ParentProcessID;
            break;
        }

    } while (Process32NextW(h, &e));
    CloseHandle(h);
    if (ppid == 0)
        return 0;
    h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, ppid);
    if (h == 0)
        return 0;
    if (GetModuleFileNameExW(h, 0, buf, XPATH_MAX) != 0)
        pp = xwcsdup(buf);
    CloseHandle(h);
    return pp;
}

static const wchar_t *getcygroot(wchar_t *drive)
{
    wchar_t *r;

    if (cygroot != 0)
        return cygroot;
    if ((r = xgetenv(L"CYGWIN_ROOT")) == 0) {
        r = getpexe(GetCurrentProcessId());
        if (r != 0) {
            int x;
            if (wchrimatch(r, L"*\\cygwin\\*", &x) == 0) {
                r[x + 7] = L'\0';
                if (drive != 0)
                    *drive = towupper(*r);
                return r;
            }
            xfree(r);
            r = 0;
        }
    }
    if (r == 0) {
        if (_waccess(L"C:\\cygwin\\bin\\bash.exe", 0) == 0) {
            r = xwcsdup(L"C:\\cygwin");
            if (drive != 0)
                *drive = L'C';
            return r;
        }
    }
    else {
        wchar_t *p = r;
        if (drive != 0)
            *drive = towupper(*r);
        while (*p != L'\0') {
            if (*p == L'/')
                *p = L'\\';
            ++p;
        }
    }
    return r;
}

static int cygspawn(int argc, wchar_t **wargv, int envc, wchar_t **wenvp)
{
    int i, rv;
    intptr_t rp;
    wchar_t *p;

    if ((cygroot = getcygroot(windrive)) == 0) {
        fprintf(stderr, "cannot find Cygwin root\n");
        return 1;
    }
    if (debug)
        wprintf(L"Arguments (%d):\n", argc);
    for (i = 0; i < argc; i++) {
        p = wargv[i];
        if ((p = posix2win(cygroot, wargv[i])) != 0) {
            if (debug) {
                wprintf(L"[%2d] : %s\n", i, wargv[i]);
                wprintf(L"     * %s\n", p);
            }
            xfree(wargv[i]);
            wargv[i] = p;
        }
        else if (debug) {
            wprintf(L"[%2d] : %s\n", i, wargv[i]);
        }
    }
    if (debug)
        wprintf(L"\nEnvironment (%d):\n", envc);
    for (i = 0; i < envc; i++) {
        p = wenvp[i];
        if ((p = posix2win(cygroot, wenvp[i])) != 0) {
            if (debug) {
                wprintf(L"[%2d] : %s\n", i, wenvp[i]);
                wprintf(L"     * %s\n", p);
            }
            xfree(wenvp[i]);
            wenvp[i] = p;
        }
        else if (debug) {
            wprintf(L"[%2d] : %s\n", i, wenvp[i]);
        }
    }
    if (debug)
        return 0;
    qsort((void *)wenvp, envc, sizeof(wchar_t *), envsort);
    /* We have a valid environment. Install the console handler
     * XXX: Check if its needed ?
     */
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)console_handler, TRUE);
    rp = _wspawnvpe(_P_WAIT, wargv[0], wargv, wenvp);
    if (rp == (intptr_t)-1) {
        _wperror(wargv[0]);
        return 1;
    }
    rv = (int)rp;
    return rv;
}

static int usage(int rv)
{
    FILE *os = rv == 0 ? stdout : stderr;
    fprintf(os, "Usage cygspawn [OPTIONS]... PROGRAM [ARGUMENTS]...\n");
    fprintf(os, "Execute PROGRAM.\n\nOptions are:\n");
    fprintf(os, " -D, --debug      print replaced arguments and environment\n");
    fprintf(os, "                  instead executing PROGRAM.\n");
    fprintf(os, " -V, --version    print version information and exit.\n");
    fprintf(os, "     --help       print this screen and exit.\n");
    fprintf(os, "     --root=PATH  use PATH as cygwin root\n\n");
    if (rv == 0)
        fputs(aslicense, os);
    return rv;
}


static int version(int license)
{
    fprintf(stdout, "cygspawn 1.0.0\n");
    fprintf(stdout, "Written by Mladen Turk (mturk@redhat.com).\n\n");
    if (license)
        fputs(aslicense, stdout);
    return 0;
}

#if defined(CONSOLE)
int wmain(int argc, const wchar_t **wargv, const wchar_t **wenv)
#else
int cmain(int argc, const wchar_t **wargv, const wchar_t **wenv);
int WINAPI wWinMain(HINSTANCE hInstance,
                    HINSTANCE hPrevInstance,
                    LPWSTR lpCmdLine,
                    int nCmdShow)
{
    AttachConsole(ATTACH_PARENT_PROCESS);
    return cmain(__argc, __wargv, _wenviron);
}

static int cmain(int argc, const wchar_t **wargv, const wchar_t **wenv)
#endif
{
    int i, rv = 0;
    wchar_t **dupwargv = 0;
    wchar_t **dupwenvp = 0;
    int envc = 0;
    int narg = 0;
    int opts = 1;

    if (argc < 2)
        return usage(1);
    dupwargv = (wchar_t **)xmalloc(argc * sizeof(wchar_t *));
    for (i = 1; i < argc; i++) {
        const wchar_t *p = wargv[i];
        if (opts) {
            if (p[0] == L'-' && p[1] != L'\0') {
                if (wcscmp(p, L"-V") == 0 || wcscmp(p, L"--version") == 0)
                    return version(1);
                else if (wcscmp(p, L"-D") == 0 || wcscmp(p, L"--debug") == 0)
                    debug = 1;
                else if (wcsncmp(p, L"--root=", 7) == 0)
                    cygroot = wargv[i] + 7;
                else if (wcscmp(p, L"--help") == 0)
                    return usage(0);
                else
                    return usage(1);
                continue;
            }
            opts = 0;
        }
        dupwargv[narg++] = xwcsdup(wargv[i]);
    }
    for (;;) {
        if (wenv[envc] == 0)
            break;
        ++envc;
    }
    dupwenvp = (wchar_t **)xmalloc((envc + 1) * sizeof(wchar_t *));
    for (i = 0; i < envc; i++)
        dupwenvp[i] = xwcsdup(wenv[i]);
    return cygspawn(narg, dupwargv, envc, dupwenvp);
}
