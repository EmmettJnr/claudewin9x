// ApprovalDlg.cpp : asks the user to allow or deny a tool request

#include "stdafx.h"
#include "ClaudeGui.h"
#include "ApprovalDlg.h"

CApprovalDlg::CApprovalDlg(LPCTSTR tool, LPCTSTR input, CWnd *pParent)
    : CDialog(CApprovalDlg::IDD, pParent), m_tool(tool), m_input(input)
{
}

BEGIN_MESSAGE_MAP(CApprovalDlg, CDialog)
    //{{AFX_MSG_MAP(CApprovalDlg)
    //}}AFX_MSG_MAP
END_MESSAGE_MAP()

BOOL CApprovalDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    m_font.CreatePointFont(90, "Courier New");
    GetDlgItem(IDC_TOOL_INPUT)->SetFont(&m_font);

    SetDlgItemText(IDC_TOOL_NAME, m_tool);
    SetDlgItemText(IDC_TOOL_INPUT,
                   m_input.IsEmpty() ? "(no input)" : (LPCTSTR)m_input);

    MessageBeep(MB_ICONQUESTION);

    // Deny is the default button, so a stray Enter never approves
    GetDlgItem(IDCANCEL)->SetFocus();
    return FALSE;
}
