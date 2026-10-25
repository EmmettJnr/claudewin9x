/*
 * remote_core.c - Networking for the remote control client
 *
 * Shares the file operation, command and transfer handlers with the other
 * clients. Instead of a Claude session it holds a remote session: it
 * registers with /remote/connect, sends a heartbeat each cycle (which also
 * tells the server whether "allow anything" is on) and reconnects by
 * itself if the server restarts or the network drops.
 */

#include "../claude.h"
#include "../handlers.h"
#include "../http.h"
#include "../util.h"
#include "remote_core.h"
#include <process.h>

/* Wait between connection attempts while the server is unreachable */
#define RECONNECT_DELAY_MS 5000
/* Slice for waits that must notice rc_stop() promptly */
#define WAIT_SLICE_MS 100

/* Positional initializer, see claude.c */
ClientState g_state = {
    "192.168.2.1",      /* server_ip */
    PORT_API,           /* server_port */
    PORT_FILE_DOWNLOAD, /* download_port */
    PORT_FILE_UPLOAD,   /* upload_port */
    "",                 /* session_id */
    0,                  /* connected */
    1,                  /* running */
    NULL,               /* logfile */
    "claude.log"        /* logpath */
};

static HWND g_notify = NULL;
static DWORD g_ui_thread = 0;
static int g_wsa_started = 0;
static volatile LONG g_wanted = 0; /* user asked to be connected */
static int g_status = RC_STATUS_IDLE;

static void post_text(int kind, const char *text)
{
    char *copy;

    if (!g_notify || !text || !text[0]) {
        return;
    }

    copy = strdup(text);
    if (!copy) {
        return;
    }

    /* Keep UI-thread text in order with what the window writes itself */
    if (GetCurrentThreadId() == g_ui_thread) {
        SendMessage(g_notify, WM_RC_TEXT, (WPARAM)kind, (LPARAM)copy);
    } else if (!PostMessage(g_notify, WM_RC_TEXT, (WPARAM)kind,
                            (LPARAM)copy)) {
        free(copy);
    }
}

static void set_status(int status)
{
    if (status != g_status) {
        g_status = status;
        PostMessage(g_notify, WM_RC_STATUS, (WPARAM)status, 0);
    }
}

static void get_session(char *buf, size_t size)
{
    EnterCriticalSection(&g_state.output_lock);
    strncpy(buf, g_state.session_id, size - 1);
    buf[size - 1] = '\0';
    LeaveCriticalSection(&g_state.output_lock);
}

static void set_session(const char *session_id)
{
    EnterCriticalSection(&g_state.output_lock);
    strncpy(g_state.session_id, session_id, sizeof(g_state.session_id) - 1);
    g_state.session_id[sizeof(g_state.session_id) - 1] = '\0';
    g_state.connected = session_id[0] != '\0';
    LeaveCriticalSection(&g_state.output_lock);
}

/* Sleeps up to ms, returning early if the user disconnects or we exit */
static void wait_while_wanted(DWORD ms)
{
    DWORD waited = 0;

    while (waited < ms && g_wanted && g_state.running) {
        Sleep(WAIT_SLICE_MS);
        waited += WAIT_SLICE_MS;
    }
}

static int remote_connect(void)
{
    static char response[BUFFER_SIZE];
    char win_version[128];
    char computer_name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD name_size = sizeof(computer_name);
    cJSON *request;
    cJSON *json;
    const cJSON *session_id;
    char *body;
    HttpResult ret;

    get_windows_version(win_version, sizeof(win_version));
    if (!GetComputerName(computer_name, &name_size)) {
        strcpy(computer_name, "unknown");
    }

    request = cJSON_CreateObject();
    if (!request) {
        return -1;
    }
    cJSON_AddStringToObject(request, "windows_version", win_version);
    {
        char *name = to_utf8(computer_name, CP_ACP);
        cJSON_AddStringToObject(request, "computer_name",
                                name ? name : computer_name);
        free(name);
    }
    cJSON_AddBoolToObject(request, "allow_all", g_state.skip_permissions);

    body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!body) {
        return -1;
    }

    ret = http_request("POST", "/remote/connect", body, response,
                       sizeof(response));
    free(body);

    if (ret != HTTP_OK) {
        if (g_status != RC_STATUS_RETRYING) {
            log_error("connect", ret == HTTP_ERR_SERVER
                                     ? "Server rejected the connection "
                                       "(is it running the remote build?)"
                                     : http_error_string(ret));
        }
        return -1;
    }

    json = cJSON_Parse(response);
    session_id = json ? cJSON_GetObjectItem(json, "session_id") : NULL;
    if (!cJSON_IsString(session_id)) {
        log_error("connect", "Invalid response from server");
        cJSON_Delete(json);
        return -1;
    }

    set_session(session_id->valuestring);
    cJSON_Delete(json);

    ui_printf("[Connected to %s:%d as %s (%s)]\n", g_state.server_ip,
              g_state.server_port, computer_name, win_version);
    return 0;
}

static HttpResult remote_heartbeat(const char *session_id)
{
    static char response[1024];
    char path[256];

    snprintf(path, sizeof(path), "/remote/heartbeat?session_id=%s&allow_all=%d",
             session_id, g_state.skip_permissions ? 1 : 0);
    return http_request("GET", path, NULL, response, sizeof(response));
}

static void remote_disconnect(const char *session_id)
{
    static char response[1024];
    char body[128];

    snprintf(body, sizeof(body), "{\"session_id\":\"%s\"}", session_id);
    http_request("POST", "/remote/disconnect", body, response,
                 sizeof(response));
    set_session("");
    ui_printf("[Disconnected]\n");
}

static unsigned __stdcall poll_thread_func(void *param)
{
    char session_id[64];
    char id[64];
    char tool[128];
    int failures = 0;
    int worked;
    HttpResult ret;

    (void)param;

    while (g_state.running) {
        get_session(session_id, sizeof(session_id));

        if (!g_wanted) {
            /* Disconnect here, so it never races a connect in progress */
            if (session_id[0]) {
                remote_disconnect(session_id);
            }
            failures = 0;
            set_status(RC_STATUS_IDLE);
            Sleep(WAIT_SLICE_MS);
            continue;
        }

        if (!session_id[0]) {
            set_status(failures ? RC_STATUS_RETRYING : RC_STATUS_CONNECTING);
            if (remote_connect() == 0) {
                failures = 0;
                set_status(RC_STATUS_CONNECTED);
            } else {
                failures++;
                set_status(RC_STATUS_RETRYING);
                wait_while_wanted(RECONNECT_DELAY_MS);
            }
            continue;
        }

        ret = remote_heartbeat(session_id);
        if (ret == HTTP_ERR_SERVER) {
            /* Server restarted or another client took over */
            ui_printf("[Server dropped the session, reconnecting]\n");
            set_session("");
            continue;
        }
        if (ret != HTTP_OK) {
            if (failures++ == 0) {
                log_error("heartbeat", http_error_string(ret));
            }
            set_status(RC_STATUS_RETRYING);
            wait_while_wanted(POLL_BACKOFF_MS);
            continue;
        }
        if (failures) {
            ui_printf("[Connection restored]\n");
            failures = 0;
        }
        set_status(RC_STATUS_CONNECTED);

        worked = handle_fileop();
        worked |= handle_command();

        if (poll_approval()) {
            worked = 1;
            if (g_state.skip_permissions) {
                if (rc_take_approval(id, sizeof(id), tool, sizeof(tool), NULL,
                                     0)) {
                    ui_printf("[Auto-approving: %s]\n", tool);
                    rc_respond_approval(id, 1);
                }
            } else {
                PostMessage(g_notify, WM_RC_APPROVAL, 0, 0);
            }
        }

        /* Go straight round again while there is work queued */
        if (!worked) {
            Sleep(POLL_SLEEP_MS);
        }
    }

    get_session(session_id, sizeof(session_id));
    if (session_id[0]) {
        remote_disconnect(session_id);
    }

    return 0;
}

int rc_init(HWND notify, const char *ini_path)
{
    WSADATA wsa;
    unsigned thread_id;

    g_notify = notify;
    g_ui_thread = GetCurrentThreadId();
    ui_set_output_hook(post_text);
    handlers_set_hidden_exec(1);

    InitializeCriticalSection(&g_state.output_lock);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_error("init", "Failed to initialize Winsock");
        return -1;
    }
    g_wsa_started = 1;

    config_load(ini_path);

    g_state.poll_thread = (HANDLE)_beginthreadex(NULL, 0, poll_thread_func,
                                                 NULL, 0, &thread_id);
    if (g_state.poll_thread == NULL) {
        log_error("init", "Could not start poll thread");
        return -1;
    }
    SetThreadPriority(g_state.poll_thread, THREAD_PRIORITY_BELOW_NORMAL);

    return 0;
}

void rc_shutdown(void)
{
    InterlockedExchange((LONG *)&g_wanted, 0);
    g_state.running = 0;

    if (g_state.poll_thread != NULL) {
        /*
         * An in-flight request can take up to HTTP_TIMEOUT_SEC to fail; a
         * running command can take far longer, so do not wait for that.
         */
        WaitForSingleObject(g_state.poll_thread,
                            (HTTP_TIMEOUT_SEC + 2) * 1000);
        CloseHandle(g_state.poll_thread);
        g_state.poll_thread = NULL;
    }

    ui_set_output_hook(NULL);
    g_notify = NULL;

    if (g_wsa_started) {
        WSACleanup();
        g_wsa_started = 0;
    }
}

void rc_get_server(char *ip, size_t ip_size, int *port)
{
    EnterCriticalSection(&g_state.output_lock);
    strncpy(ip, g_state.server_ip, ip_size - 1);
    ip[ip_size - 1] = '\0';
    *port = g_state.server_port;
    LeaveCriticalSection(&g_state.output_lock);
}

void rc_set_server(const char *ip, int port)
{
    EnterCriticalSection(&g_state.output_lock);
    strncpy(g_state.server_ip, ip, sizeof(g_state.server_ip) - 1);
    g_state.server_ip[sizeof(g_state.server_ip) - 1] = '\0';
    g_state.server_port = port;
    LeaveCriticalSection(&g_state.output_lock);
}

int rc_get_allow_all(void)
{
    return g_state.skip_permissions;
}

void rc_set_allow_all(int enabled)
{
    /* Reaches the server with the next heartbeat */
    g_state.skip_permissions = enabled ? 1 : 0;
}

void rc_start(void)
{
    InterlockedExchange((LONG *)&g_wanted, 1);
}

void rc_stop(void)
{
    InterlockedExchange((LONG *)&g_wanted, 0);
    set_status(RC_STATUS_DISCONNECTING);
}

int rc_is_active(void)
{
    return g_wanted != 0;
}

int rc_take_approval(char *id, size_t id_size, char *tool, size_t tool_size,
                     char *input, size_t input_size)
{
    EnterCriticalSection(&g_state.output_lock);
    if (!g_state.has_pending_approval) {
        LeaveCriticalSection(&g_state.output_lock);
        return 0;
    }

    strncpy(id, g_state.approval_id, id_size - 1);
    id[id_size - 1] = '\0';
    strncpy(tool, g_state.approval_tool_name, tool_size - 1);
    tool[tool_size - 1] = '\0';
    if (input && input_size > 0) {
        strncpy(input, g_state.approval_tool_input, input_size - 1);
        input[input_size - 1] = '\0';
    }

    g_state.approval_in_progress = 1;
    g_state.has_pending_approval = 0;
    LeaveCriticalSection(&g_state.output_lock);

    return 1;
}

void rc_respond_approval(const char *id, int approved)
{
    /* Not static: called from both the UI thread and the poll thread */
    char response[1024];
    char body[512];

    if (id[0]) {
        snprintf(body, sizeof(body), "{\"approval_id\":\"%s\",\"approved\":%s}",
                 id, approved ? "true" : "false");

        if (http_request("POST", "/approval/respond", body, response,
                         sizeof(response)) == HTTP_OK) {
            ui_printf("[%s]\n", approved ? "Allowed" : "Denied");
        } else {
            log_error("approval", "Could not send the answer to the server "
                                  "(it may have timed out)");
        }
    }

    EnterCriticalSection(&g_state.output_lock);
    g_state.approval_in_progress = 0;
    LeaveCriticalSection(&g_state.output_lock);
}

void rc_free_text(char *text)
{
    free(text);
}
