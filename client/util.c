/*
 * util.c - Utility functions
 */

#include "util.h"
#include <stdarg.h>

static void log_output(const char *text);

static UiOutputHook g_output_hook = NULL;

#if defined(_MSC_VER) && _MSC_VER < 1900
#define VSNPRINTF _vsnprintf
#else
#define VSNPRINTF vsnprintf
#endif

#if defined(_MSC_VER) && _MSC_VER < 1900
/*
 * Returns -1 on truncation (C99 would return the needed length); callers
 * treat both "< 0" and ">= size" as overflow.
 */
int compat_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    int len;

    if (size == 0) {
        return -1;
    }

    va_start(args, fmt);
    len = _vsnprintf(buf, size, fmt, args);
    va_end(args);

    if (len < 0 || (size_t)len >= size) {
        buf[size - 1] = '\0';
        return -1;
    }

    return len;
}
#endif

static int is_switch_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '?' || c == '-' || c == '@';
}

void path_to_backslashes(char *str)
{
    char *p = str;
    char prev = ' ';
    int in_url = 0;

    for (; *p; p++) {
        /* Detect start of URL (://) - safe because p[1]=='/' implies p[1]!='\0' */
        if (!in_url && *p == ':' && p[1] == '/' && p[2] == '/') {
            in_url = 1;
            prev = *p;
            continue;
        }

        /* Inside URL: skip until whitespace */
        if (in_url) {
            if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
                *p == '"' || *p == '\'') {
                in_url = 0;
            }
            prev = *p;
            continue;
        }

        if (*p == '/') {
            /* Don't convert switches: space/tab followed by /switchchar */
            if ((prev == ' ' || prev == '\t') && *(p + 1) &&
                is_switch_char(*(p + 1))) {
                prev = *p;
                continue;
            }
            *p = '\\';
        }
        prev = *p;
    }
}

static int normalize_path(char *path)
{
    char *segments[128];
    int depth = 0;
    char *p;
    char *start;
    int i;

    if (path[0] && path[1] == ':') {
        start = path + 2;
        if (*start == '\\') {
            start++;
        }
    } else {
        start = path;
        if (*start == '\\') {
            start++;
        }
    }

    p = start;
    while (*p) {
        char *seg_start = p;
        char *seg_end;

        while (*p && *p != '\\') {
            p++;
        }
        seg_end = p;

        if (*p == '\\') {
            *p = '\0';
            p++;
        }

        if (seg_start == seg_end || strcmp(seg_start, ".") == 0) {
            continue;
        }

        if (strcmp(seg_start, "..") == 0) {
            if (depth == 0) {
                return -1;
            }
            depth--;
            continue;
        }

        if (depth >= 128) {
            return -1;
        }
        segments[depth++] = seg_start;
    }

    if (path[0] && path[1] == ':') {
        p = path + 2;
        *p++ = '\\';
    } else {
        p = path;
        *p++ = '\\';
    }

    for (i = 0; i < depth; i++) {
        size_t len = strlen(segments[i]);
        memmove(p, segments[i], len);
        p += len;
        if (i < depth - 1) {
            *p++ = '\\';
        }
    }
    *p = '\0';

    return 0;
}

int build_full_path(const char *relative, char *out, size_t out_size)
{
    int len;

    if (!relative || relative[0] == '\0') {
        len = snprintf(out, out_size, "C:\\");
    } else if (((relative[0] >= 'A' && relative[0] <= 'Z') ||
                (relative[0] >= 'a' && relative[0] <= 'z')) &&
               relative[1] == ':') {
        /* Already has a drive letter */
        len = snprintf(out, out_size, "%s", relative);
    } else if (relative[0] == '/' || relative[0] == '\\') {
        len = snprintf(out, out_size, "C:%s", relative);
    } else {
        len = snprintf(out, out_size, "C:\\%s", relative);
    }

    if (len < 0 || (size_t)len >= out_size) {
        return -1;
    }

    path_to_backslashes(out);

    if (normalize_path(out) != 0) {
        return -1;
    }

    return 0;
}

void get_windows_version(char *buf, size_t bufsize)
{
    DWORD ver = GetVersion();
    DWORD major = (DWORD)(LOBYTE(LOWORD(ver)));
    DWORD minor = (DWORD)(HIBYTE(LOWORD(ver)));
    DWORD build = 0;

    if (ver < 0x80000000) {
        build = (DWORD)(HIWORD(ver));
    }

    if (major == 4 && minor == 0) {
        snprintf(buf, bufsize, "Windows 95");
    } else if (major == 4 && minor == 10) {
        snprintf(buf, bufsize, "Windows 98");
    } else if (major == 4 && minor == 90) {
        snprintf(buf, bufsize, "Windows ME");
    } else if (major == 5 && minor == 0) {
        snprintf(buf, bufsize, "Windows 2000 (Build %lu)", build);
    } else if (major == 5 && minor == 1) {
        snprintf(buf, bufsize, "Windows XP (Build %lu)", build);
    } else if (major == 5 && minor == 2) {
        snprintf(buf, bufsize, "Windows Server 2003 (Build %lu)", build);
    } else if (major == 6 && minor == 0) {
        snprintf(buf, bufsize, "Windows Vista (Build %lu)", build);
    } else if (major == 6 && minor == 1) {
        snprintf(buf, bufsize, "Windows 7 (Build %lu)", build);
    } else {
        snprintf(buf, bufsize, "Windows %lu.%lu (Build %lu)", major, minor,
                 build);
    }
}

void ui_set_output_hook(UiOutputHook hook)
{
    g_output_hook = hook;
}

void ui_write(int kind, const char *text)
{
    if (g_output_hook) {
        g_output_hook(kind, text);
        return;
    }

    if (kind == UI_TEXT_ERROR) {
        fputs(text, stderr);
    } else {
        fputs(text, stdout);
        fflush(stdout);
    }
}

void ui_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list args;

    va_start(args, fmt);
    VSNPRINTF(buf, sizeof(buf), fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';

    ui_write(UI_TEXT_INFO, buf);
}

void print_output(const char *text)
{
    ui_write(UI_TEXT_CLAUDE, text);
    log_output(text);
}

static void log_output(const char *text)
{
    if (g_state.logfile) {
        fprintf(g_state.logfile, "%s", text);
        fflush(g_state.logfile);
    }
}

void log_user_input(const char *text)
{
    if (g_state.logfile) {
        fprintf(g_state.logfile, "\n> %s\n", text);
        fflush(g_state.logfile);
    }
}

void log_error(const char *context, const char *message)
{
    char buf[512];

    snprintf(buf, sizeof(buf), "[Error: %s: %s]\n", context, message);
    ui_write(UI_TEXT_ERROR, buf);
    if (g_state.logfile) {
        fprintf(g_state.logfile, "[Error: %s: %s]\n", context, message);
        fflush(g_state.logfile);
    }
}

/*
 * Simple INI parser for config file.
 * Format:
 *   [server]
 *   ip=192.168.2.1
 *   port=5000
 */
void config_load(const char *filename)
{
    FILE *fp;
    char line[256];
    char key[64];
    char value[128];
    int in_server_section = 0;

    fp = fopen(filename, "r");
    if (!fp) {
        return;
    }

    ui_printf("[Loading config from %s]\n", filename);

    while (fgets(line, sizeof(line), fp)) {
        char *p = line + strlen(line) - 1;
        while (p >= line && (*p == '\n' || *p == '\r' || *p == ' ')) {
            *p-- = '\0';
        }

        if (line[0] == '\0' || line[0] == ';' || line[0] == '#') {
            continue;
        }

        if (line[0] == '[') {
            in_server_section = (strncmp(line, "[server]", 8) == 0);
            continue;
        }

        if (in_server_section) {
            if (sscanf(line, "%63[^=]=%127s", key, value) == 2) {
                p = key + strlen(key) - 1;
                while (p >= key && *p == ' ') {
                    *p-- = '\0';
                }

                if (strcmp(key, "ip") == 0) {
                    strncpy(g_state.server_ip, value,
                            sizeof(g_state.server_ip) - 1);
                    g_state.server_ip[sizeof(g_state.server_ip) - 1] = '\0';
                    ui_printf("[Config: server ip = %s]\n",
                              g_state.server_ip);
                } else if (strcmp(key, "api_port") == 0) {
                    g_state.server_port = atoi(value);
                    ui_printf("[Config: api port = %d]\n",
                              g_state.server_port);
                } else if (strcmp(key, "download_port") == 0) {
                    g_state.download_port = atoi(value);
                    ui_printf("[Config: download port = %d]\n",
                              g_state.download_port);
                } else if (strcmp(key, "upload_port") == 0) {
                    g_state.upload_port = atoi(value);
                    ui_printf("[Config: upload port = %d]\n",
                              g_state.upload_port);
                } else if (strcmp(key, "skip_permissions") == 0) {
                    g_state.skip_permissions =
                        (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
                    ui_printf("[Config: skip_permissions = %s]\n",
                              g_state.skip_permissions ? "true" : "false");
                }
            }
        }
    }

    fclose(fp);
}

/* Copy of text with bytes >= 0x80 replaced by '?', so always valid UTF-8 */
static char *ascii_only(const char *text)
{
    char *out = strdup(text);
    char *p;

    if (out) {
        for (p = out; *p; p++) {
            if ((unsigned char)*p >= 0x80) {
                *p = '?';
            }
        }
    }
    return out;
}

/*
 * UTF-8 is encoded and decoded by hand (via UTF-16) rather than with
 * CP_UTF8, which Windows 95 does not support.
 */
char *to_utf8(const char *text, UINT codepage)
{
    WCHAR *wide;
    char *out;
    char *p;
    int wlen;
    int i;

    wlen = MultiByteToWideChar(codepage, 0, text, -1, NULL, 0);
    if (wlen <= 0) {
        return ascii_only(text);
    }

    wide = (WCHAR *)malloc(wlen * sizeof(WCHAR));
    /* Each UTF-16 unit becomes at most 3 UTF-8 bytes */
    out = (char *)malloc((size_t)wlen * 3 + 1);
    if (!wide || !out) {
        free(wide);
        free(out);
        return NULL;
    }

    MultiByteToWideChar(codepage, 0, text, -1, wide, wlen);

    p = out;
    for (i = 0; i < wlen && wide[i]; i++) {
        unsigned int c = wide[i];

        if (c < 0x80) {
            *p++ = (char)c;
        } else if (c < 0x800) {
            *p++ = (char)(0xC0 | (c >> 6));
            *p++ = (char)(0x80 | (c & 0x3F));
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            /* Surrogates never come from ANSI/OEM code pages */
            *p++ = '?';
        } else {
            *p++ = (char)(0xE0 | (c >> 12));
            *p++ = (char)(0x80 | ((c >> 6) & 0x3F));
            *p++ = (char)(0x80 | (c & 0x3F));
        }
    }
    *p = '\0';

    free(wide);
    return out;
}

char *from_utf8(const char *text, UINT codepage)
{
    const unsigned char *s = (const unsigned char *)text;
    WCHAR *wide;
    WCHAR *w;
    char *out;
    int len;

    wide = (WCHAR *)malloc((strlen(text) + 1) * sizeof(WCHAR));
    if (!wide) {
        return NULL;
    }

    /* Each test on s[n] only runs if s[n - 1] was a continuation byte,
     * so this never reads past the terminator */
    w = wide;
    while (*s) {
        unsigned int c = *s;

        if (c < 0x80) {
            *w++ = (WCHAR)c;
            s++;
        } else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
            *w++ = (WCHAR)(((c & 0x1F) << 6) | (s[1] & 0x3F));
            s += 2;
        } else if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 &&
                   (s[2] & 0xC0) == 0x80) {
            *w++ = (WCHAR)(((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) |
                           (s[2] & 0x3F));
            s += 3;
        } else if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
                   (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
            /* Outside the BMP (e.g. emoji): no code page equivalent */
            *w++ = '?';
            s += 4;
        } else {
            /* Invalid UTF-8 */
            *w++ = '?';
            s++;
        }
    }
    *w = 0;

    len = WideCharToMultiByte(codepage, 0, wide, -1, NULL, 0, NULL, NULL);
    if (len <= 0) {
        free(wide);
        return strdup(text);
    }

    out = (char *)malloc(len);
    if (out) {
        WideCharToMultiByte(codepage, 0, wide, -1, out, len, NULL, NULL);
    }

    free(wide);
    return out;
}
