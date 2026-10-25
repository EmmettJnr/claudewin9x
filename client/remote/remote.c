/*
 * remote.c - Remote control client: lets a Claude Code session on another
 * computer run commands and edit files on this machine via the server.
 *
 * Plain Win32 (no MFC), builds with Visual C++ 6.0 or Open Watcom.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "remote_core.h"
#include "resource.h"

#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

/* VC6 only has _snprintf; every buffer here is terminated by hand */
#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf
#endif

#define INI_NAME "client.ini"
#define MAX_TOOL_INPUT 32768
/* Win9x multi-line edit controls hold at most 64 KB */
#define LOG_MAX_CHARS 48000
#define LOG_TRIM_CHARS 16000

typedef struct {
    const char *tool;
    const char *input;
} ApprovalInfo;

static HINSTANCE g_inst;
static HFONT g_mono_font;
static char g_ini_path[MAX_PATH];

/*
 * The server speaks UTF-8; the controls are ANSI. CP_UTF8 needs Windows 98
 * or later, so on failure the bytes are used unchanged. Returns a malloc'd
 * string with CRLF line breaks (as edit controls need), or NULL.
 */
static char *utf8_to_display(const char *text)
{
    char *ansi = NULL;
    char *result;
    const char *p;
    char *q;
    size_t newlines = 0;
    int wlen;

    wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (wlen > 0) {
        WCHAR *wide = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
        if (wide) {
            int len;
            MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, wlen);
            len = WideCharToMultiByte(CP_ACP, 0, wide, -1, NULL, 0, NULL,
                                      NULL);
            if (len > 0) {
                ansi = (char *)malloc((size_t)len);
                if (ansi) {
                    WideCharToMultiByte(CP_ACP, 0, wide, -1, ansi, len, NULL,
                                        NULL);
                }
            }
            free(wide);
        }
    }
    if (!ansi) {
        ansi = _strdup(text);
        if (!ansi) {
            return NULL;
        }
    }

    for (p = ansi; *p; p++) {
        if (*p == '\n') {
            newlines++;
        }
    }

    result = (char *)malloc(strlen(ansi) + newlines + 1);
    if (result) {
        for (p = ansi, q = result; *p; p++) {
            if (*p == '\r') {
                continue;
            }
            if (*p == '\n') {
                *q++ = '\r';
            }
            *q++ = *p;
        }
        *q = '\0';
    }
    free(ansi);
    return result;
}

/* Local text (handler messages, command output) only needs CRLF */
static char *to_crlf(const char *text)
{
    const char *p;
    char *q;
    char *result;
    size_t newlines = 0;

    for (p = text; *p; p++) {
        if (*p == '\n') {
            newlines++;
        }
    }

    result = (char *)malloc(strlen(text) + newlines + 1);
    if (!result) {
        return NULL;
    }
    for (p = text, q = result; *p; p++) {
        if (*p == '\r') {
            continue;
        }
        if (*p == '\n') {
            *q++ = '\r';
        }
        *q++ = *p;
    }
    *q = '\0';
    return result;
}

static void append_log(HWND dlg, const char *text)
{
    HWND log = GetDlgItem(dlg, IDC_LOG);
    char *crlf = to_crlf(text);
    int len;

    if (!crlf) {
        return;
    }

    len = GetWindowTextLength(log);
    if (len + (int)strlen(crlf) > LOG_MAX_CHARS) {
        /* Drop the oldest lines */
        SendMessage(log, EM_SETSEL, 0,
                    SendMessage(log, EM_LINEINDEX,
                                SendMessage(log, EM_LINEFROMCHAR,
                                            LOG_TRIM_CHARS, 0) + 1,
                                0));
        SendMessage(log, EM_REPLACESEL, FALSE, (LPARAM) "");
        len = GetWindowTextLength(log);
    }

    SendMessage(log, EM_SETSEL, len, len);
    SendMessage(log, EM_REPLACESEL, FALSE, (LPARAM)crlf);
    SendMessage(log, EM_SCROLLCARET, 0, 0);
    free(crlf);
}

static void update_controls(HWND dlg, int status)
{
    char text[160];
    char ip[64];
    int port;
    int active = rc_is_active();

    rc_get_server(ip, sizeof(ip), &port);

    switch (status) {
    case RC_STATUS_CONNECTING:
        snprintf(text, sizeof(text), "Connecting to %s:%d...", ip, port);
        break;
    case RC_STATUS_CONNECTED:
        snprintf(text, sizeof(text),
                 "Connected to %s:%d - waiting for Claude", ip, port);
        break;
    case RC_STATUS_RETRYING:
        snprintf(text, sizeof(text), "Cannot reach %s:%d - retrying...", ip,
                 port);
        break;
    case RC_STATUS_DISCONNECTING:
        strcpy(text, "Disconnecting (after the current task)...");
        break;
    default:
        strcpy(text, "Not connected");
        break;
    }
    text[sizeof(text) - 1] = '\0';

    SetDlgItemText(dlg, IDC_STATUS, text);
    SetDlgItemText(dlg, IDC_CONNECT, active ? "&Disconnect" : "&Connect");
    EnableWindow(GetDlgItem(dlg, IDC_SERVER_IP), !active);
    EnableWindow(GetDlgItem(dlg, IDC_SERVER_PORT), !active);
    EnableWindow(GetDlgItem(dlg, IDC_CONNECT),
                 status != RC_STATUS_DISCONNECTING);
}

static BOOL CALLBACK approval_proc(HWND dlg, UINT msg, WPARAM wParam,
                                   LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG: {
        const ApprovalInfo *info = (const ApprovalInfo *)lParam;
        char *tool = utf8_to_display(info->tool);
        char *input = utf8_to_display(info->input[0] ? info->input
                                                     : "(no details)");

        SendDlgItemMessage(dlg, IDC_TOOL_INPUT, WM_SETFONT,
                           (WPARAM)g_mono_font, FALSE);
        SetDlgItemText(dlg, IDC_TOOL_NAME, tool ? tool : info->tool);
        SetDlgItemText(dlg, IDC_TOOL_INPUT, input ? input : info->input);
        free(tool);
        free(input);

        MessageBeep(MB_ICONQUESTION);
        /* Deny is the default button, so a stray Enter never approves */
        SetFocus(GetDlgItem(dlg, IDCANCEL));
        return FALSE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(dlg, LOWORD(wParam));
            return TRUE;
        }
        break;
    }

    return FALSE;
}

static void handle_approval(HWND dlg)
{
    char id[64];
    char tool[128];
    char *input = (char *)malloc(MAX_TOOL_INPUT);
    ApprovalInfo info;
    HCURSOR old_cursor;
    int approved;

    if (!input) {
        return;
    }

    if (rc_take_approval(id, sizeof(id), tool, sizeof(tool), input,
                         MAX_TOOL_INPUT)) {
        append_log(dlg, "[Waiting for you to allow or deny a request]\n");

        /* Get the user's attention if the window is in the background */
        if (IsIconic(dlg)) {
            ShowWindow(dlg, SW_RESTORE);
        }
        FlashWindow(dlg, TRUE);

        info.tool = tool;
        info.input = input;
        approved = DialogBoxParam(g_inst, MAKEINTRESOURCE(IDD_APPROVAL), dlg,
                                  approval_proc, (LPARAM)&info) == IDOK;

        old_cursor = SetCursor(LoadCursor(NULL, IDC_WAIT));
        rc_respond_approval(id, approved);
        SetCursor(old_cursor);
    }

    free(input);
}

static void on_connect(HWND dlg)
{
    char ip[64];
    char port_text[16];
    BOOL ok;
    UINT port;

    if (rc_is_active()) {
        rc_stop();
        return;
    }

    GetDlgItemText(dlg, IDC_SERVER_IP, ip, sizeof(ip));
    port = GetDlgItemInt(dlg, IDC_SERVER_PORT, &ok, FALSE);
    if (!ip[0] || !ok || port == 0 || port > 65535) {
        MessageBox(dlg, "Enter the server's IP address and port.",
                   "ClaudeWin9x Remote", MB_OK | MB_ICONEXCLAMATION);
        return;
    }

    rc_set_server(ip, (int)port);

    /* Remember the server for next time (shared with the other clients) */
    snprintf(port_text, sizeof(port_text), "%u", port);
    WritePrivateProfileString("server", "ip", ip, g_ini_path);
    WritePrivateProfileString("server", "api_port", port_text, g_ini_path);

    rc_start();
    update_controls(dlg, RC_STATUS_CONNECTING);
}

/* Reads a yes/no setting from the [remote] section of client.ini */
static int ini_flag(const char *key)
{
    char value[16];

    GetPrivateProfileString("remote", key, "", value, sizeof(value),
                            g_ini_path);
    return strcmp(value, "1") == 0 || _stricmp(value, "true") == 0 ||
           _stricmp(value, "yes") == 0;
}

static void on_allow_all(HWND dlg)
{
    int enabled = IsDlgButtonChecked(dlg, IDC_ALLOW_ALL) == BST_CHECKED;

    if (enabled &&
        MessageBox(dlg,
                   "Let Claude run any command and write any file on this "
                   "machine without asking first?",
                   "Allow anything", MB_YESNO | MB_ICONWARNING |
                                         MB_DEFBUTTON2) != IDYES) {
        CheckDlgButton(dlg, IDC_ALLOW_ALL, BST_UNCHECKED);
        enabled = 0;
    }

    rc_set_allow_all(enabled);
    append_log(dlg, enabled ? "[Allow anything: on]\n"
                            : "[Allow anything: off - you will be asked]\n");
}

static BOOL CALLBACK main_proc(HWND dlg, UINT msg, WPARAM wParam,
                               LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG: {
        HICON icon = LoadIcon(g_inst, MAKEINTRESOURCE(IDR_MAINFRAME));
        char ip[64];
        int port;

        SendMessage(dlg, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessage(dlg, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        SendDlgItemMessage(dlg, IDC_LOG, WM_SETFONT, (WPARAM)g_mono_font,
                           FALSE);
        SendDlgItemMessage(dlg, IDC_LOG, EM_LIMITTEXT, 0xFFFF, 0);

        append_log(dlg, "ClaudeWin9x Remote - lets Claude Code on another "
                        "computer use this one.\n");

        if (rc_init(dlg, g_ini_path) != 0) {
            append_log(dlg, "[Error: could not start networking]\n");
        }

        /*
         * allow_all in [remote] turns on "Allow anything" at startup, for
         * this client only (skip_permissions in [server] also applies to
         * the chat clients)
         */
        if (ini_flag("allow_all")) {
            rc_set_allow_all(1);
        }

        rc_get_server(ip, sizeof(ip), &port);
        SetDlgItemText(dlg, IDC_SERVER_IP, ip);
        SetDlgItemInt(dlg, IDC_SERVER_PORT, port, FALSE);
        CheckDlgButton(dlg, IDC_ALLOW_ALL,
                       rc_get_allow_all() ? BST_CHECKED : BST_UNCHECKED);
        update_controls(dlg, RC_STATUS_IDLE);

        if (rc_get_allow_all()) {
            append_log(dlg, "[Allow anything: on (from client.ini)]\n");
        }

        /* auto_connect in [remote] connects at startup */
        if (ini_flag("auto_connect")) {
            append_log(dlg, "[Connecting automatically (from client.ini)]\n");
            PostMessage(dlg, WM_COMMAND, MAKEWPARAM(IDC_CONNECT, BN_CLICKED),
                        0);
        }

        SetFocus(GetDlgItem(dlg, IDC_CONNECT));
        return FALSE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_CONNECT:
            on_connect(dlg);
            return TRUE;
        case IDC_ALLOW_ALL:
            on_allow_all(dlg);
            return TRUE;
        case IDC_CLEAR:
            SetDlgItemText(dlg, IDC_LOG, "");
            return TRUE;
        case IDOK:
        case IDCANCEL:
            /* Enter and Escape must not close the window */
            return TRUE;
        }
        break;

    case WM_RC_TEXT:
        append_log(dlg, (const char *)lParam);
        rc_free_text((char *)lParam);
        return TRUE;

    case WM_RC_STATUS:
        update_controls(dlg, (int)wParam);
        return TRUE;

    case WM_RC_APPROVAL:
        handle_approval(dlg);
        return TRUE;

    case WM_CLOSE: {
        HCURSOR old_cursor = SetCursor(LoadCursor(NULL, IDC_WAIT));
        rc_shutdown();
        SetCursor(old_cursor);
        EndDialog(dlg, 0);
        return TRUE;
    }
    }

    return FALSE;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    char *slash;

    (void)prev;
    (void)cmdline;
    (void)show;

    g_inst = inst;

    /*
     * client.ini lives next to the exe. Use a full path: the exe may be run
     * from a network share, which cannot be the current directory on 9x.
     */
    if (GetModuleFileName(NULL, g_ini_path, sizeof(g_ini_path)) > 0 &&
        (slash = strrchr(g_ini_path, '\\')) != NULL &&
        (size_t)(slash - g_ini_path) + sizeof("\\" INI_NAME) <=
            sizeof(g_ini_path)) {
        strcpy(slash + 1, INI_NAME);
    } else {
        strcpy(g_ini_path, INI_NAME);
    }

    g_mono_font = CreateFont(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                             FIXED_PITCH | FF_MODERN, "Courier New");

    DialogBox(inst, MAKEINTRESOURCE(IDD_REMOTE), NULL, main_proc);

    if (g_mono_font) {
        DeleteObject(g_mono_font);
    }
    return 0;
}
