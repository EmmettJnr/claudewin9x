// ClaudeGuiDlg.cpp : main chat window

#include "stdafx.h"
#include "ClaudeGui.h"
#include "ClaudeGuiDlg.h"
#include "ApprovalDlg.h"
#include "gui_core.h"

#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

#define COLOR_CLAUDE RGB(0, 0, 0)
#define COLOR_USER RGB(0, 0, 160)
#define COLOR_INFO RGB(128, 128, 128)
#define COLOR_ERROR RGB(192, 0, 0)

#define MAX_HISTORY 50
#define MAX_TOOL_INPUT 32768

/////////////////////////////////////////////////////////////////////////////
// Text helpers
//
// The server speaks UTF-8; the controls are ANSI. CP_UTF8 conversions need
// Windows 98 or later; if they fail the raw bytes are used unchanged.

static CString ConvertCodePage(const char *text, UINT from, UINT to)
{
    CString result;
    int wlen;
    int len;
    WCHAR *wide;

    wlen = MultiByteToWideChar(from, 0, text, -1, NULL, 0);
    if (wlen <= 0) {
        return CString(text);
    }

    wide = new WCHAR[wlen];
    MultiByteToWideChar(from, 0, text, -1, wide, wlen);

    len = WideCharToMultiByte(to, 0, wide, -1, NULL, 0, NULL, NULL);
    if (len <= 0) {
        delete[] wide;
        return CString(text);
    }

    WideCharToMultiByte(to, 0, wide, -1, result.GetBuffer(len), len, NULL,
                        NULL);
    result.ReleaseBuffer();
    delete[] wide;
    return result;
}

static CString Utf8ToAnsi(const char *text)
{
    return ConvertCodePage(text, CP_UTF8, CP_ACP);
}

static CString AnsiToUtf8(const char *text)
{
    return ConvertCodePage(text, CP_ACP, CP_UTF8);
}

// Edit controls need CRLF line breaks
static CString ToCrLf(const CString &text)
{
    CString result(text);
    result.Remove('\r');
    result.Replace("\n", "\r\n");
    return result;
}

/////////////////////////////////////////////////////////////////////////////
// CClaudeGuiDlg

CClaudeGuiDlg::CClaudeGuiDlg(CWnd *pParent)
    : CDialog(CClaudeGuiDlg::IDD, pParent), m_historyPos(0),
      m_waiting(FALSE), m_ready(FALSE), m_minSize(0, 0), m_lineHeight(1)
{
    m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CClaudeGuiDlg::DoDataExchange(CDataExchange *pDX)
{
    CDialog::DoDataExchange(pDX);
    //{{AFX_DATA_MAP(CClaudeGuiDlg)
    DDX_Control(pDX, IDC_OUTPUT, m_output);
    DDX_Control(pDX, IDC_INPUT, m_input);
    //}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(CClaudeGuiDlg, CDialog)
    //{{AFX_MSG_MAP(CClaudeGuiDlg)
    ON_WM_CLOSE()
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
    ON_BN_CLICKED(IDC_CONNECT, OnConnect)
    ON_BN_CLICKED(IDC_SEND, OnSend)
    ON_BN_CLICKED(IDC_CLEAR, OnClear)
    ON_BN_CLICKED(IDC_AUTO_APPROVE, OnAutoApprove)
    //}}AFX_MSG_MAP
    ON_MESSAGE(WM_CORE_TEXT, OnCoreText)
    ON_MESSAGE(WM_CORE_APPROVAL, OnCoreApproval)
    ON_MESSAGE(WM_CORE_STOPPED, OnCoreStopped)
END_MESSAGE_MAP()

BOOL CClaudeGuiDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    SetIcon(m_hIcon, TRUE);
    SetIcon(m_hIcon, FALSE);

    UpgradeRichEdit();

    m_font.CreatePointFont(90, "Courier New");
    m_output.SetFont(&m_font);
    m_input.SetFont(&m_font);

    {
        CClientDC dc(&m_output);
        CFont *oldFont = dc.SelectObject(&m_font);
        TEXTMETRIC tm;
        dc.GetTextMetrics(&tm);
        dc.SelectObject(oldFont);
        m_lineHeight = tm.tmHeight > 0 ? tm.tmHeight : 1;
    }

    m_output.LimitText(16 * 1024 * 1024);
    m_output.SetBackgroundColor(FALSE, RGB(255, 255, 255));

    // The dialog template's size is the minimum size
    CRect rect;
    GetWindowRect(&rect);
    m_minSize = rect.Size();

    m_ready = TRUE;
    GetClientRect(&rect);
    LayoutControls(rect.Width(), rect.Height());

    AppendLine("ClaudeWin9x GUI - Claude Code for Windows 9x/NT", COLOR_INFO);
    AppendLine("Enter sends, Shift+Enter adds a new line, "
               "Ctrl+Up/Down recalls previous messages.",
               COLOR_INFO);
    AppendLine("", COLOR_INFO);

    if (core_init(m_hWnd) != 0) {
        AppendLine("[Error: could not start networking]", COLOR_ERROR);
    }

    char ip[64];
    int port;
    core_get_server(ip, sizeof(ip), &port);
    SetDlgItemText(IDC_SERVER_IP, ip);
    SetDlgItemInt(IDC_SERVER_PORT, port, FALSE);
    CheckDlgButton(IDC_AUTO_APPROVE, core_get_auto_approve() ? 1 : 0);

    UpdateControls();
    GetDlgItem(IDC_CONNECT)->SetFocus();
    return FALSE; // focus set manually
}

// Enter and Escape must not close the dialog
void CClaudeGuiDlg::OnOK()
{
}

void CClaudeGuiDlg::OnCancel()
{
}

void CClaudeGuiDlg::OnClose()
{
    CWaitCursor wait;

    if (core_is_connected()) {
        core_disconnect();
    }
    core_shutdown();

    CDialog::OnCancel();
}

void CClaudeGuiDlg::OnGetMinMaxInfo(MINMAXINFO FAR *lpMMI)
{
    if (m_minSize.cx > 0) {
        lpMMI->ptMinTrackSize.x = m_minSize.cx;
        lpMMI->ptMinTrackSize.y = m_minSize.cy;
    }
    CDialog::OnGetMinMaxInfo(lpMMI);
}

void CClaudeGuiDlg::OnSize(UINT nType, int cx, int cy)
{
    CDialog::OnSize(nType, cx, cy);

    if (m_ready && nType != SIZE_MINIMIZED) {
        LayoutControls(cx, cy);
    }
}

// Top row keeps its template position; everything else follows the size
void CClaudeGuiDlg::LayoutControls(int cx, int cy)
{
    CRect units(7, 14, 50, 40); // margin, button height, button width, input
    MapDialogRect(&units);
    int margin = units.left;
    int buttonH = units.top;
    int buttonW = units.right;
    int inputH = units.bottom;

    CRect topRow;
    GetDlgItem(IDC_CONNECT)->GetWindowRect(&topRow);
    ScreenToClient(&topRow);

    CRect status;
    GetDlgItem(IDC_STATUS)->GetWindowRect(&status);
    int statusH = status.Height();

    int statusY = cy - margin / 2 - statusH;
    int inputY = statusY - margin / 2 - inputH;
    int outputY = topRow.bottom + margin;
    int outputH = inputY - margin - outputY;
    int inputW = cx - 3 * margin - buttonW;

    if (outputH < 20) {
        outputH = 20;
    }

    m_output.MoveWindow(margin, outputY, cx - 2 * margin, outputH);
    m_input.MoveWindow(margin, inputY, inputW, inputH);
    GetDlgItem(IDC_SEND)->MoveWindow(cx - margin - buttonW, inputY, buttonW,
                                     buttonH);
    GetDlgItem(IDC_CLEAR)->MoveWindow(cx - margin - buttonW,
                                      inputY + inputH - buttonH, buttonW,
                                      buttonH);
    GetDlgItem(IDC_STATUS)->MoveWindow(margin, statusY, cx - 2 * margin,
                                       statusH);
    Invalidate();
}

BOOL CClaudeGuiDlg::PreTranslateMessage(MSG *pMsg)
{
    if (pMsg->message == WM_KEYDOWN && pMsg->hwnd == m_input.m_hWnd) {
        BOOL shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        BOOL ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (pMsg->wParam == VK_RETURN && !shift) {
            OnSend();
            return TRUE;
        }
        if (ctrl && pMsg->wParam == VK_UP) {
            RecallHistory(-1);
            return TRUE;
        }
        if (ctrl && pMsg->wParam == VK_DOWN) {
            RecallHistory(1);
            return TRUE;
        }
        if (ctrl && pMsg->wParam == 'A') {
            m_input.SetSel(0, -1);
            return TRUE;
        }
    }

    return CDialog::PreTranslateMessage(pMsg);
}

/*
 * The dialog template uses RichEdit 1.0 (RICHED32.DLL, loaded by
 * AfxInitRichEdit), which scrolls badly on Windows 9x. Where RichEdit 2.0
 * is available (Windows 98 and later) swap in a 2.0 control in its place.
 */
void CClaudeGuiDlg::UpgradeRichEdit()
{
    CRect rect;
    HWND oldWnd;
    HWND newWnd;

    if (LoadLibrary("riched20.dll") == NULL) {
        return;
    }

    m_output.GetWindowRect(&rect);
    ScreenToClient(&rect);

    // WS_VSCROLL is dropped from GetStyle() while the scrollbar is hidden,
    // so set the scrolling styles explicitly
    newWnd = ::CreateWindowEx(m_output.GetExStyle(), "RichEdit20A", "",
                              m_output.GetStyle() | WS_VSCROLL |
                                  ES_AUTOVSCROLL | ES_DISABLENOSCROLL,
                              rect.left, rect.top,
                              rect.Width(), rect.Height(), m_hWnd,
                              (HMENU)IDC_OUTPUT, AfxGetInstanceHandle(),
                              NULL);
    if (newWnd == NULL) {
        return;
    }

    oldWnd = m_output.UnsubclassWindow();
    // Same position in the tab order as the control it replaces
    ::SetWindowPos(newWnd, oldWnd, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ::DestroyWindow(oldWnd);
    m_output.SubclassWindow(newWnd);
}

int CClaudeGuiDlg::VisibleOutputLines()
{
    CRect rect;
    int lines;

    m_output.GetRect(&rect);
    lines = rect.Height() / m_lineHeight;
    return lines > 0 ? lines : 1;
}

BOOL CClaudeGuiDlg::IsOutputAtEnd()
{
    return m_output.GetFirstVisibleLine() + VisibleOutputLines() >=
           m_output.GetLineCount() - 1;
}

/*
 * Scrolls by line count rather than WM_VSCROLL/SB_BOTTOM: RichEdit 1.0 can
 * scroll past the end of the text with SB_BOTTOM, leaving the view blank
 * until the scrollbar is dragged.
 */
void CClaudeGuiDlg::ScrollOutputToEnd()
{
    int target = m_output.GetLineCount() - VisibleOutputLines();
    int first = m_output.GetFirstVisibleLine();

    if (target < 0) {
        target = 0;
    }
    if (target != first) {
        m_output.LineScroll(target - first);
    }
}

void CClaudeGuiDlg::AppendText(LPCTSTR text, COLORREF color, BOOL bold)
{
    CHARFORMAT cf;
    long end;
    // Only follow new text if the user has not scrolled up to read
    BOOL follow = IsOutputAtEnd();

    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR | CFM_BOLD;
    cf.crTextColor = color;
    cf.dwEffects = bold ? CFE_BOLD : 0;

    end = m_output.GetTextLength();
    m_output.SetSel(end, end);
    m_output.SetSelectionCharFormat(cf);
    m_output.ReplaceSel(text);

    if (follow) {
        ScrollOutputToEnd();
    }
}

void CClaudeGuiDlg::AppendLine(LPCTSTR text, COLORREF color)
{
    CString line(text);
    line += "\r\n";
    AppendText(line, color, FALSE);
}

void CClaudeGuiDlg::UpdateControls()
{
    BOOL connected = core_is_connected();

    SetDlgItemText(IDC_CONNECT, connected ? "&Disconnect" : "&Connect");
    GetDlgItem(IDC_SERVER_IP)->EnableWindow(!connected);
    GetDlgItem(IDC_SERVER_PORT)->EnableWindow(!connected);
    GetDlgItem(IDC_SEND)->EnableWindow(connected);

    CString status;
    if (!connected) {
        status = "Not connected";
    } else if (m_waiting) {
        status = "Waiting for Claude...";
    } else {
        status.Format("Connected - session %s", core_session_id());
    }
    SetDlgItemText(IDC_STATUS, status);
}

void CClaudeGuiDlg::RecallHistory(int delta)
{
    int count = m_history.GetSize();
    if (count == 0) {
        return;
    }

    m_historyPos += delta;
    if (m_historyPos < 0) {
        m_historyPos = 0;
    }
    if (m_historyPos >= count) {
        // Past the newest entry: back to an empty line
        m_historyPos = count;
        m_input.SetWindowText("");
        return;
    }

    m_input.SetWindowText(m_history[m_historyPos]);
    m_input.SetSel(m_history[m_historyPos].GetLength(),
                   m_history[m_historyPos].GetLength());
}

void CClaudeGuiDlg::OnConnect()
{
    CWaitCursor wait;

    if (core_is_connected()) {
        core_disconnect();
        m_waiting = FALSE;
        UpdateControls();
        return;
    }

    CString ip;
    BOOL ok;
    UINT port;

    GetDlgItemText(IDC_SERVER_IP, ip);
    port = GetDlgItemInt(IDC_SERVER_PORT, &ok, FALSE);
    if (ip.IsEmpty() || !ok || port == 0 || port > 65535) {
        AppendLine("[Enter a server IP address and port]", COLOR_ERROR);
        return;
    }
    core_set_server(ip, (int)port);

    if (core_connect(NULL) == 0) {
        AppendLine("[Ready - type a message below]", COLOR_INFO);
        UpdateControls();
        m_input.SetFocus();
    } else {
        UpdateControls();
    }
}

void CClaudeGuiDlg::OnSend()
{
    CString text;

    if (!core_is_connected()) {
        AppendLine("[Not connected - click Connect first]", COLOR_ERROR);
        return;
    }

    m_input.GetWindowText(text);
    text.TrimRight();
    if (text.IsEmpty()) {
        return;
    }

    AppendText("> " + ToCrLf(text) + "\r\n", COLOR_USER, TRUE);

    m_history.Add(text);
    if (m_history.GetSize() > MAX_HISTORY) {
        m_history.RemoveAt(0);
    }
    m_historyPos = m_history.GetSize();
    m_input.SetWindowText("");

    text.Remove('\r');

    CWaitCursor wait;
    if (core_send(AnsiToUtf8(text)) == 0) {
        m_waiting = TRUE;
    }
    UpdateControls();
    m_input.SetFocus();
}

void CClaudeGuiDlg::OnClear()
{
    m_output.SetWindowText("");
    m_input.SetFocus();
}

void CClaudeGuiDlg::OnAutoApprove()
{
    BOOL enabled = IsDlgButtonChecked(IDC_AUTO_APPROVE);

    if (enabled &&
        MessageBox("Automatically allow every tool Claude asks to run, "
                   "including commands and file writes on this machine?",
                   "Auto-approve tools", MB_YESNO | MB_ICONWARNING) != IDYES) {
        CheckDlgButton(IDC_AUTO_APPROVE, 0);
        enabled = FALSE;
    }

    core_set_auto_approve(enabled);
}

LRESULT CClaudeGuiDlg::OnCoreText(WPARAM wParam, LPARAM lParam)
{
    char *raw = (char *)lParam;
    CString text = ToCrLf(Utf8ToAnsi(raw));
    core_free_text(raw);

    switch (wParam) {
    case CORE_TEXT_CLAUDE:
        AppendText(text, COLOR_CLAUDE, FALSE);
        if (m_waiting) {
            m_waiting = FALSE;
            UpdateControls();
        }
        break;
    case CORE_TEXT_ERROR:
        AppendText(text, COLOR_ERROR, FALSE);
        break;
    default:
        AppendText(text, COLOR_INFO, FALSE);
        break;
    }

    return 0;
}

LRESULT CClaudeGuiDlg::OnCoreApproval(WPARAM, LPARAM)
{
    char id[64];
    char tool[128];
    char *input = new char[MAX_TOOL_INPUT];

    if (core_take_approval(id, sizeof(id), tool, sizeof(tool), input,
                           MAX_TOOL_INPUT)) {
        CString toolText = Utf8ToAnsi(tool);
        CString inputText = ToCrLf(Utf8ToAnsi(input));

        AppendLine("[Tool approval requested: " + toolText + "]", COLOR_INFO);

        // Bring the window to the user's attention if it is in the background
        FlashWindow(TRUE);

        CApprovalDlg dlg(toolText, inputText, this);
        BOOL approved = (dlg.DoModal() == IDOK);

        CWaitCursor wait;
        core_respond_approval(id, approved);
    }

    delete[] input;
    return 0;
}

LRESULT CClaudeGuiDlg::OnCoreStopped(WPARAM, LPARAM)
{
    AppendLine("[Session ended]", COLOR_INFO);
    m_waiting = FALSE;
    UpdateControls();
    return 0;
}
