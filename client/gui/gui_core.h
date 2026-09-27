/*
 * gui_core.h - C bridge between the shared client code and the MFC GUI
 *
 * The GUI never includes claude.h directly (winsock2.h and MFC headers do
 * not mix well); everything it needs goes through these functions.
 * Include <windows.h> (or afxwin.h) before this header.
 */

#ifndef GUI_CORE_H
#define GUI_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Kinds of text (same values as UI_TEXT_* in util.h) */
#define CORE_TEXT_CLAUDE 0
#define CORE_TEXT_INFO 1
#define CORE_TEXT_ERROR 2

/*
 * Messages posted to the notify window. Any thread may post them, so the
 * GUI only touches its controls from its own message handlers.
 *
 * WM_CORE_TEXT:     wParam = CORE_TEXT_*, lParam = char * (UTF-8), which
 *                   the receiver must release with core_free_text().
 * WM_CORE_APPROVAL: a tool approval is waiting; call core_take_approval().
 * WM_CORE_STOPPED:  the server ended the session.
 */
#define WM_CORE_TEXT (WM_APP + 1)
#define WM_CORE_APPROVAL (WM_APP + 2)
#define WM_CORE_STOPPED (WM_APP + 3)

/* Returns 0 on success. Loads client.ini and starts the poll thread. */
int core_init(HWND notify);
void core_shutdown(void);

void core_get_server(char *ip, size_t ip_size, int *port);
void core_set_server(const char *ip, int port);

int core_get_auto_approve(void);
void core_set_auto_approve(int enabled);

/* Returns 0 on success; errors are reported as WM_CORE_TEXT messages. */
int core_connect(const char *working_dir);
void core_disconnect(void);
int core_is_connected(void);
const char *core_session_id(void);

/* text must be UTF-8. Returns 0 on success. */
int core_send(const char *text);

/*
 * Claims the pending approval. Returns 1 and fills the buffers if there
 * was one; the caller must then call core_respond_approval().
 */
int core_take_approval(char *id, size_t id_size, char *tool,
                       size_t tool_size, char *input, size_t input_size);
void core_respond_approval(const char *id, int approved);

void core_free_text(char *text);

#ifdef __cplusplus
}
#endif

#endif /* GUI_CORE_H */
