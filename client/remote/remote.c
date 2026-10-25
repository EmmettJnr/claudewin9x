/*
 * remote.c - Remote control client: lets a Claude Code session on another
 * computer run commands and edit files on this machine via the server.
 *
 * Plain Win32 (no MFC), builds with Visual C++ 6.0 or Open Watcom.
 */

#include <windows.h>
#include <shellapi.h>
#include <stddef.h>
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

/* Tray icon frames, in the order of IDI_TRAY_* */
#define TRAY_OFF 0
#define TRAY_IDLE 1
#define TRAY_LEFT 2
#define TRAY_RIGHT 3
#define TRAY_BOTH 4
#define TRAY_FRAMES 5

#define WM_TRAY (WM_APP + 10)
#define TRAY_ID 1
#define TRAY_TIMER 1
#define TRAY_TICK_MS 150
/* Keep flickering briefly after quick operations so they are visible */
#define ACTIVITY_LINGER_MS 700
#define BLINK_MS 600

/*
 * The Windows 95/98 shell only accepts the original NOTIFYICONDATA (64
 * character tooltip); newer headers declare a larger structure.
 */
#define NID_V1_SIZE (offsetof(NOTIFYICONDATA, szTip) + 64 * sizeof(CHAR))

typedef struct {
    const char *tool;
    const char *input;
} ApprovalInfo;

static HINSTANCE g_inst;
static HFONT g_mono_font;
static char g_ini_path[MAX_PATH];

/* Tray icon state (UI thread only) */
static int g_tray_ok = 0;
static UINT g_taskbar_created;
static HICON g_tray_icons[TRAY_FRAMES];
static int g_tray_frame = -1;
static char g_tray_tip[64];
static int g_status = RC_STATUS_IDLE;
static int g_activity = RC_ACTIVITY_DONE;
static DWORD g_activity_end;
static unsigned long g_flicker_seed = 1;

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

/* Adds or updates the tray icon's image and tooltip */
static void tray_notify(HWND dlg, DWORD message)
{
    NOTIFYICONDATA nid;

    memset(&nid, 0, sizeof(nid));
    nid.cbSize = NID_V1_SIZE;
    nid.hWnd = dlg;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_tray_icons[g_tray_frame < 0 ? TRAY_OFF : g_tray_frame];
    strncpy(nid.szTip, g_tray_tip, 63);
    nid.szTip[63] = '\0';

    if (Shell_NotifyIcon(message, &nid) && message == NIM_ADD) {
        g_tray_ok = 1;
    }
}

static void tray_remove(HWND dlg)
{
    NOTIFYICONDATA nid;

    if (!g_tray_ok) {
        return;
    }
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = NID_V1_SIZE;
    nid.hWnd = dlg;
    nid.uID = TRAY_ID;
    Shell_NotifyIcon(NIM_DELETE, &nid);
    g_tray_ok = 0;
}

/* Which picture the tray should show right now */
static int tray_frame_now(void)
{
    DWORD now = GetTickCount();

    switch (g_status) {
    case RC_STATUS_CONNECTED:
        if (g_activity == RC_ACTIVITY_SENDING) {
            return TRAY_BOTH;
        }
        if (g_activity == RC_ACTIVITY_WORKING ||
            now - g_activity_end < ACTIVITY_LINGER_MS) {
            /* Random flicker, like modem lights */
            g_flicker_seed = g_flicker_seed * 1103515245UL + 12345UL;
            switch ((g_flicker_seed >> 16) % 5) {
            case 0:
            case 1:
                return TRAY_LEFT;
            case 2:
            case 3:
                return TRAY_RIGHT;
            default:
                return TRAY_BOTH;
            }
        }
        return TRAY_IDLE;
    case RC_STATUS_CONNECTING:
    case RC_STATUS_RETRYING:
        return (now / BLINK_MS) % 2 ? TRAY_IDLE : TRAY_OFF;
    default:
        return TRAY_OFF;
    }
}

static void tray_animate(HWND dlg)
{
    int frame = tray_frame_now();

    if (g_tray_ok && frame != g_tray_frame) {
        g_tray_frame = frame;
        tray_notify(dlg, NIM_MODIFY);
    }
}

static void update_controls(HWND dlg, int status)
{
    char text[160];
    char ip[64];
    int port;
    int active = rc_is_active();

    g_status = status;
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

    snprintf(g_tray_tip, sizeof(g_tray_tip), "Claude Remote: %s", text);
    g_tray_tip[sizeof(g_tray_tip) - 1] = '\0';
    if (g_tray_ok) {
        g_tray_frame = tray_frame_now();
        tray_notify(dlg, NIM_MODIFY);
    }
}

static void show_window(HWND dlg)
{
    ShowWindow(dlg, IsIconic(dlg) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(dlg);
}

/* Hides to the tray if there is one; returns 0 if there is not */
static int hide_window(HWND dlg)
{
    if (!g_tray_ok) {
        return 0;
    }
    ShowWindow(dlg, SW_HIDE);
    return 1;
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

        /* The main window may be hidden in the tray: stay on top of it all */
        SetWindowPos(dlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(dlg);
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

        if (IsWindowVisible(dlg)) {
            FlashWindow(dlg, TRUE);
        }

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
        show_window(dlg);
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

/* From the checkbox or the tray menu; turning it on asks first */
static void set_allow_all(HWND dlg, int enabled)
{
    if (enabled &&
        MessageBox(dlg,
                   "Let Claude run any command and write any file on this "
                   "machine without asking first?",
                   "Allow anything", MB_YESNO | MB_ICONWARNING |
                                         MB_DEFBUTTON2 | MB_SETFOREGROUND) !=
            IDYES) {
        enabled = 0;
    }

    CheckDlgButton(dlg, IDC_ALLOW_ALL, enabled ? BST_CHECKED : BST_UNCHECKED);
    if (enabled == rc_get_allow_all()) {
        return;
    }

    rc_set_allow_all(enabled);
    append_log(dlg, enabled ? "[Allow anything: on]\n"
                            : "[Allow anything: off - you will be asked]\n");
}

static void show_tray_menu(HWND dlg)
{
    HMENU menu = LoadMenu(g_inst, MAKEINTRESOURCE(IDR_TRAYMENU));
    HMENU popup;
    POINT pt;

    if (!menu) {
        return;
    }
    popup = GetSubMenu(menu, 0);

    ModifyMenu(popup, IDM_CONNECT, MF_BYCOMMAND | MF_STRING, IDM_CONNECT,
               rc_is_active() ? "&Disconnect" : "&Connect");
    EnableMenuItem(popup, IDM_CONNECT,
                   MF_BYCOMMAND | (g_status == RC_STATUS_DISCONNECTING
                                       ? MF_GRAYED
                                       : MF_ENABLED));
    CheckMenuItem(popup, IDM_ALLOW_ALL,
                  MF_BYCOMMAND |
                      (rc_get_allow_all() ? MF_CHECKED : MF_UNCHECKED));
    SetMenuDefaultItem(popup, IDM_SHOW, FALSE);

    /* Needed for the menu to close when clicking elsewhere (KB135788) */
    GetCursorPos(&pt);
    SetForegroundWindow(dlg);
    TrackPopupMenu(popup, TPM_RIGHTBUTTON, pt.x, pt.y, 0, dlg, NULL);
    PostMessage(dlg, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

static void exit_app(HWND dlg)
{
    HCURSOR old_cursor = SetCursor(LoadCursor(NULL, IDC_WAIT));

    KillTimer(dlg, TRAY_TIMER);
    rc_shutdown();
    tray_remove(dlg);
    SetCursor(old_cursor);
    DestroyWindow(dlg);
}

static void on_init(HWND dlg)
{
    HICON icon = LoadIcon(g_inst, MAKEINTRESOURCE(IDR_MAINFRAME));
    char ip[64];
    int port;
    int i;

    SendMessage(dlg, WM_SETICON, ICON_BIG, (LPARAM)icon);
    SendMessage(dlg, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    SendDlgItemMessage(dlg, IDC_LOG, WM_SETFONT, (WPARAM)g_mono_font, FALSE);
    SendDlgItemMessage(dlg, IDC_LOG, EM_LIMITTEXT, 0xFFFF, 0);

    append_log(dlg, "ClaudeWin9x Remote - lets Claude Code on another "
                    "computer use this one.\n");

    for (i = 0; i < TRAY_FRAMES; i++) {
        g_tray_icons[i] = (HICON)LoadImage(g_inst,
                                           MAKEINTRESOURCE(IDI_TRAY_OFF + i),
                                           IMAGE_ICON, 16, 16, 0);
    }
    strcpy(g_tray_tip, "Claude Remote: Not connected");
    g_tray_frame = TRAY_OFF;
    tray_notify(dlg, NIM_ADD);
    SetTimer(dlg, TRAY_TIMER, TRAY_TICK_MS, NULL);

    if (rc_init(dlg, g_ini_path) != 0) {
        append_log(dlg, "[Error: could not start networking]\n");
    }

    /*
     * allow_all in [remote] turns on "Allow anything" at startup, for this
     * client only (skip_permissions in [server] also applies to the chat
     * clients)
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
        PostMessage(dlg, WM_COMMAND, MAKEWPARAM(IDC_CONNECT, BN_CLICKED), 0);
    }

    if (!g_tray_ok) {
        append_log(dlg, "[No notification area: closing the window exits]\n");
    }

    /* start_hidden in [remote] starts in the tray, if there is one */
    if (!(ini_flag("start_hidden") && g_tray_ok)) {
        ShowWindow(dlg, SW_SHOW);
    }
}

static BOOL CALLBACK main_proc(HWND dlg, UINT msg, WPARAM wParam,
                               LPARAM lParam)
{
    /* Explorer restarted: put the icon back */
    if (msg == g_taskbar_created && g_taskbar_created != 0) {
        g_tray_ok = 0;
        tray_notify(dlg, NIM_ADD);
        return TRUE;
    }

    switch (msg) {
    case WM_INITDIALOG:
        on_init(dlg);
        SetFocus(GetDlgItem(dlg, IDC_CONNECT));
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_CONNECT:
        case IDM_CONNECT:
            on_connect(dlg);
            return TRUE;
        case IDC_ALLOW_ALL:
            set_allow_all(dlg,
                          IsDlgButtonChecked(dlg, IDC_ALLOW_ALL) == BST_CHECKED);
            return TRUE;
        case IDM_ALLOW_ALL:
            set_allow_all(dlg, !rc_get_allow_all());
            return TRUE;
        case IDC_CLEAR:
            SetDlgItemText(dlg, IDC_LOG, "");
            return TRUE;
        case IDM_SHOW:
            show_window(dlg);
            return TRUE;
        case IDM_EXIT:
            exit_app(dlg);
            return TRUE;
        case IDOK:
        case IDCANCEL:
            /* Enter and Escape must not close the window */
            return TRUE;
        }
        break;

    case WM_TRAY:
        switch (lParam) {
        case WM_LBUTTONDBLCLK:
            show_window(dlg);
            break;
        case WM_RBUTTONUP:
            show_tray_menu(dlg);
            break;
        }
        return TRUE;

    case WM_TIMER:
        if (wParam == TRAY_TIMER) {
            tray_animate(dlg);
        }
        return TRUE;

    case WM_RC_TEXT:
        append_log(dlg, (const char *)lParam);
        rc_free_text((char *)lParam);
        return TRUE;

    case WM_RC_STATUS:
        update_controls(dlg, (int)wParam);
        return TRUE;

    case WM_RC_ACTIVITY:
        g_activity = (int)wParam;
        if (g_activity == RC_ACTIVITY_DONE) {
            g_activity_end = GetTickCount();
        }
        tray_animate(dlg);
        return TRUE;

    case WM_RC_APPROVAL:
        handle_approval(dlg);
        return TRUE;

    case WM_SYSCOMMAND:
        /* Minimising goes to the tray */
        if ((wParam & 0xFFF0) == SC_MINIMIZE && hide_window(dlg)) {
            return TRUE;
        }
        break;

    case WM_CLOSE:
        /* Closing goes to the tray too; Exit is on the tray menu */
        if (!hide_window(dlg)) {
            exit_app(dlg);
        }
        return TRUE;

    case WM_ENDSESSION:
        if (wParam) {
            /* Windows is shutting down: tell the server we are going */
            rc_shutdown();
            tray_remove(dlg);
        }
        return TRUE;

    case WM_DESTROY:
        PostQuitMessage(0);
        return TRUE;
    }

    return FALSE;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    char *slash;
    HWND dlg;
    MSG msg;

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

    /* Sent to all top-level windows when Explorer (re)creates the taskbar */
    g_taskbar_created = RegisterWindowMessage("TaskbarCreated");

    /* Modeless, so the window can start hidden in the tray */
    dlg = CreateDialog(inst, MAKEINTRESOURCE(IDD_REMOTE), NULL, main_proc);
    if (!dlg) {
        return 1;
    }

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessage(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    if (g_mono_font) {
        DeleteObject(g_mono_font);
    }
    return 0;
}
