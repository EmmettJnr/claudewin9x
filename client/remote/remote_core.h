/*
 * remote_core.h - Networking for the remote control client
 *
 * The remote client starts no Claude session of its own. It registers with
 * the server and then carries out commands and file operations that a
 * Claude Code session elsewhere sends through the server's remote (MCP)
 * port. A background thread does all the polling and reports to a window
 * with posted messages. Include <windows.h> before this header.
 */

#ifndef REMOTE_CORE_H
#define REMOTE_CORE_H

/* Kinds of text (same values as UI_TEXT_* in util.h) */
#define RC_TEXT_INFO 1
#define RC_TEXT_ERROR 2

/* Connection states, sent as wParam of WM_RC_STATUS */
#define RC_STATUS_IDLE 0       /* not connected, not trying */
#define RC_STATUS_CONNECTING 1 /* first attempt in progress */
#define RC_STATUS_CONNECTED 2
#define RC_STATUS_RETRYING 3     /* server unreachable, trying again */
#define RC_STATUS_DISCONNECTING 4

/*
 * Messages posted to the notify window. The receiver only touches its
 * controls from its own message handlers.
 *
 * WM_RC_TEXT:     wParam = RC_TEXT_*, lParam = char * (local code page),
 *                 which the receiver must release with rc_free_text().
 * WM_RC_APPROVAL: an approval is waiting; call rc_take_approval().
 * WM_RC_STATUS:   wParam = RC_STATUS_*.
 * WM_RC_ACTIVITY: wParam = RC_ACTIVITY_*.
 */
#define WM_RC_TEXT (WM_APP + 1)
#define WM_RC_APPROVAL (WM_APP + 2)
#define WM_RC_STATUS (WM_APP + 3)
#define WM_RC_ACTIVITY (WM_APP + 4)

/* Same values as HANDLER_ACTIVITY_* in handlers.h */
#define RC_ACTIVITY_DONE 0
#define RC_ACTIVITY_WORKING 1
#define RC_ACTIVITY_SENDING 2

/* Loads ini_path ([server] section) and starts the poll thread. 0 = ok. */
int rc_init(HWND notify, const char *ini_path);
void rc_shutdown(void);

void rc_get_server(char *ip, size_t ip_size, int *port);
/* Only while not connected */
void rc_set_server(const char *ip, int port);

int rc_get_allow_all(void);
void rc_set_allow_all(int enabled);

/* Start connecting (and keep reconnecting) / disconnect */
void rc_start(void);
void rc_stop(void);
/* True between rc_start() and rc_stop() */
int rc_is_active(void);

/*
 * Claims the pending approval. Returns 1 and fills the buffers (UTF-8) if
 * there was one; the caller must then call rc_respond_approval().
 */
int rc_take_approval(char *id, size_t id_size, char *tool, size_t tool_size,
                     char *input, size_t input_size);
void rc_respond_approval(const char *id, int approved);

void rc_free_text(char *text);

#endif /* REMOTE_CORE_H */
