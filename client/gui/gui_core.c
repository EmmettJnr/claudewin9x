/*
 * gui_core.c - Session handling for the GUI client
 *
 * Mirrors the console client's session.c / claude.c logic, but reports
 * everything to a window via posted messages instead of the console.
 */

#include "../claude.h"
#include "../handlers.h"
#include "../http.h"
#include "../util.h"
#include "gui_core.h"
#include <process.h>

/* Same defaults as the console client (see claude.c) */
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

    /*
     * Text from the UI thread is delivered immediately so it stays in order
     * with what the GUI writes itself; other threads must post.
     */
    if (GetCurrentThreadId() == g_ui_thread) {
        SendMessage(g_notify, WM_CORE_TEXT, (WPARAM)kind, (LPARAM)copy);
    } else if (!PostMessage(g_notify, WM_CORE_TEXT, (WPARAM)kind,
                            (LPARAM)copy)) {
        free(copy);
    }
}

static void clear_session(void)
{
    EnterCriticalSection(&g_state.output_lock);
    g_state.session_id[0] = '\0';
    g_state.connected = 0;
    LeaveCriticalSection(&g_state.output_lock);
}

static void poll_output(const char *session_id)
{
    static char response[BUFFER_SIZE];
    char path[256];
    cJSON *json;
    const cJSON *output;
    const cJSON *status;

    snprintf(path, sizeof(path), "/output?session_id=%s", session_id);

    if (http_request("GET", path, NULL, response, sizeof(response)) !=
        HTTP_OK) {
        return;
    }

    json = cJSON_Parse(response);
    if (!json) {
        return;
    }

    output = cJSON_GetObjectItem(json, "output");
    status = cJSON_GetObjectItem(json, "status");

    if (cJSON_IsString(output) && output->valuestring[0]) {
        print_output(output->valuestring);
    }

    if (cJSON_IsString(status) && strcmp(status->valuestring, "stopped") == 0) {
        clear_session();
        PostMessage(g_notify, WM_CORE_STOPPED, 0, 0);
    }

    cJSON_Delete(json);
}

static unsigned __stdcall poll_thread_func(void *param)
{
    char local_session_id[64];
    char id[64];
    char tool[128];

    (void)param;

    while (g_state.running) {
        EnterCriticalSection(&g_state.output_lock);
        strncpy(local_session_id, g_state.session_id,
                sizeof(local_session_id));
        local_session_id[sizeof(local_session_id) - 1] = '\0';
        LeaveCriticalSection(&g_state.output_lock);

        if (!local_session_id[0]) {
            Sleep(POLL_SLEEP_MS);
            continue;
        }

        /* Output first, so Claude's explanation precedes its tool requests */
        poll_output(local_session_id);

        handle_fileop();
        handle_command();

        if (poll_approval()) {
            if (g_state.skip_permissions) {
                if (core_take_approval(id, sizeof(id), tool, sizeof(tool),
                                       NULL, 0)) {
                    ui_printf("[Auto-approving: %s]\n", tool);
                    core_respond_approval(id, 1);
                }
            } else {
                PostMessage(g_notify, WM_CORE_APPROVAL, 0, 0);
            }
        }

        Sleep(POLL_SLEEP_MS);
    }

    return 0;
}

int core_init(HWND notify)
{
    WSADATA wsa;
    unsigned thread_id;

    g_notify = notify;
    g_ui_thread = GetCurrentThreadId();
    ui_set_output_hook(post_text);
    handlers_set_hidden_exec(1);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_error("init", "Failed to initialize Winsock");
        return -1;
    }
    g_wsa_started = 1;

    InitializeCriticalSection(&g_state.output_lock);

    config_load("client.ini");

    g_state.poll_thread = (HANDLE)_beginthreadex(NULL, 0, poll_thread_func,
                                                 NULL, 0, &thread_id);
    if (g_state.poll_thread == NULL) {
        log_error("init", "Could not start poll thread");
        return -1;
    }
    SetThreadPriority(g_state.poll_thread, THREAD_PRIORITY_BELOW_NORMAL);

    return 0;
}

void core_shutdown(void)
{
    g_state.running = 0;

    if (g_state.poll_thread != NULL) {
        /* An in-flight request can take up to HTTP_TIMEOUT_SEC to fail */
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

void core_get_server(char *ip, size_t ip_size, int *port)
{
    EnterCriticalSection(&g_state.output_lock);
    strncpy(ip, g_state.server_ip, ip_size - 1);
    ip[ip_size - 1] = '\0';
    *port = g_state.server_port;
    LeaveCriticalSection(&g_state.output_lock);
}

void core_set_server(const char *ip, int port)
{
    EnterCriticalSection(&g_state.output_lock);
    strncpy(g_state.server_ip, ip, sizeof(g_state.server_ip) - 1);
    g_state.server_ip[sizeof(g_state.server_ip) - 1] = '\0';
    g_state.server_port = port;
    LeaveCriticalSection(&g_state.output_lock);
}

int core_get_auto_approve(void)
{
    return g_state.skip_permissions;
}

void core_set_auto_approve(int enabled)
{
    g_state.skip_permissions = enabled ? 1 : 0;
}

int core_connect(const char *working_dir)
{
    static char response[BUFFER_SIZE];
    cJSON *request;
    cJSON *json;
    const cJSON *session_id_item;
    const cJSON *error_item;
    char *body;
    char win_version[128];
    HttpResult ret;

    if (core_is_connected()) {
        return 0;
    }

    get_windows_version(win_version, sizeof(win_version));
    ui_printf("[Client: %s]\n", win_version);
    ui_printf("[Connecting to %s:%d...]\n", g_state.server_ip,
              g_state.server_port);

    request = cJSON_CreateObject();
    if (!request) {
        log_error("session", "Out of memory");
        return -1;
    }

    if (working_dir && working_dir[0]) {
        cJSON_AddStringToObject(request, "working_directory", working_dir);
    }
    cJSON_AddStringToObject(request, "windows_version", win_version);

    body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);

    if (!body) {
        log_error("session", "Out of memory");
        return -1;
    }

    ret = http_request("POST", "/start", body, response, sizeof(response));
    free(body);

    if (ret != HTTP_OK) {
        log_error("session", http_error_string(ret));
        return -1;
    }

    json = cJSON_Parse(response);
    if (!json) {
        log_error("session", "Invalid response from server");
        return -1;
    }

    error_item = cJSON_GetObjectItem(json, "error");
    if (cJSON_IsString(error_item)) {
        log_error("session", error_item->valuestring);
        cJSON_Delete(json);
        return -1;
    }

    session_id_item = cJSON_GetObjectItem(json, "session_id");
    if (!cJSON_IsString(session_id_item)) {
        log_error("session", "No session ID returned");
        cJSON_Delete(json);
        return -1;
    }

    EnterCriticalSection(&g_state.output_lock);
    strncpy(g_state.session_id, session_id_item->valuestring,
            sizeof(g_state.session_id) - 1);
    g_state.session_id[sizeof(g_state.session_id) - 1] = '\0';
    g_state.connected = 1;
    g_state.session_stopped = 0;
    LeaveCriticalSection(&g_state.output_lock);
    cJSON_Delete(json);

    ui_printf("[Connected! Session: %s]\n", g_state.session_id);
    return 0;
}

void core_disconnect(void)
{
    static char response[BUFFER_SIZE];
    char session_id[64];
    cJSON *request;
    char *body;

    EnterCriticalSection(&g_state.output_lock);
    strncpy(session_id, g_state.session_id, sizeof(session_id) - 1);
    session_id[sizeof(session_id) - 1] = '\0';
    LeaveCriticalSection(&g_state.output_lock);

    if (!session_id[0]) {
        return;
    }

    request = cJSON_CreateObject();
    if (request) {
        cJSON_AddStringToObject(request, "session_id", session_id);
        body = cJSON_PrintUnformatted(request);
        cJSON_Delete(request);

        if (body) {
            http_request("POST", "/stop", body, response, sizeof(response));
            free(body);
        }
    }

    clear_session();
    ui_printf("[Disconnected]\n");
}

int core_is_connected(void)
{
    int connected;

    EnterCriticalSection(&g_state.output_lock);
    connected = g_state.session_id[0] != '\0';
    LeaveCriticalSection(&g_state.output_lock);

    return connected;
}

const char *core_session_id(void)
{
    return g_state.session_id;
}

int core_send(const char *text)
{
    static char response[BUFFER_SIZE];
    char session_id[64];
    char *text_with_newline;
    cJSON *request;
    cJSON *json;
    char *body;
    size_t len;
    HttpResult ret;

    EnterCriticalSection(&g_state.output_lock);
    strncpy(session_id, g_state.session_id, sizeof(session_id) - 1);
    session_id[sizeof(session_id) - 1] = '\0';
    LeaveCriticalSection(&g_state.output_lock);

    if (!session_id[0]) {
        log_error("input", "Not connected");
        return -1;
    }

    log_user_input(text);

    len = strlen(text);
    text_with_newline = (char *)malloc(len + 2);
    if (!text_with_newline) {
        log_error("input", "Out of memory");
        return -1;
    }
    memcpy(text_with_newline, text, len);
    text_with_newline[len] = '\n';
    text_with_newline[len + 1] = '\0';

    request = cJSON_CreateObject();
    if (!request) {
        free(text_with_newline);
        log_error("input", "Out of memory");
        return -1;
    }

    cJSON_AddStringToObject(request, "session_id", session_id);
    cJSON_AddStringToObject(request, "text", text_with_newline);
    free(text_with_newline);

    body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);

    if (!body) {
        log_error("input", "Out of memory");
        return -1;
    }

    ret = http_request("POST", "/input", body, response, sizeof(response));
    free(body);

    if (ret != HTTP_OK) {
        log_error("input", http_error_string(ret));
        return -1;
    }

    json = cJSON_Parse(response);
    if (json) {
        const cJSON *error_item = cJSON_GetObjectItem(json, "error");
        if (cJSON_IsString(error_item)) {
            log_error("input", error_item->valuestring);
            cJSON_Delete(json);
            return -1;
        }
        cJSON_Delete(json);
    }

    return 0;
}

int core_take_approval(char *id, size_t id_size, char *tool,
                       size_t tool_size, char *input, size_t input_size)
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

void core_respond_approval(const char *id, int approved)
{
    /* Not static: called from both the UI thread and the poll thread */
    char response[1024];
    char body[512];

    if (id[0]) {
        snprintf(body, sizeof(body), "{\"approval_id\":\"%s\",\"approved\":%s}",
                 id, approved ? "true" : "false");

        if (http_request("POST", "/approval/respond", body, response,
                         sizeof(response)) == HTTP_OK) {
            ui_printf("[%s]\n", approved ? "Approved" : "Rejected");
        } else {
            log_error("approval", "Could not send response to server");
        }
    }

    EnterCriticalSection(&g_state.output_lock);
    g_state.approval_in_progress = 0;
    LeaveCriticalSection(&g_state.output_lock);
}

void core_free_text(char *text)
{
    free(text);
}
