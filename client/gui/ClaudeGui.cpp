// ClaudeGui.cpp : application entry point

#include "stdafx.h"
#include "ClaudeGui.h"
#include "ClaudeGuiDlg.h"

BEGIN_MESSAGE_MAP(CClaudeGuiApp, CWinApp)
    //{{AFX_MSG_MAP(CClaudeGuiApp)
    //}}AFX_MSG_MAP
END_MESSAGE_MAP()

CClaudeGuiApp::CClaudeGuiApp()
{
}

CClaudeGuiApp theApp;

BOOL CClaudeGuiApp::InitInstance()
{
    char exe_dir[MAX_PATH];
    char *slash;

    // client.ini and claude.log live next to the exe, wherever we are
    // started from
    if (GetModuleFileName(NULL, exe_dir, sizeof(exe_dir)) > 0) {
        slash = strrchr(exe_dir, '\\');
        if (slash != NULL) {
            *slash = '\0';
            SetCurrentDirectory(exe_dir);
        }
    }

    AfxInitRichEdit();

    CClaudeGuiDlg dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();

    // Dialog closed: exit rather than start the message pump
    return FALSE;
}
