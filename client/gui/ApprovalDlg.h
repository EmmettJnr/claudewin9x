// ApprovalDlg.h : asks the user to allow or deny a tool request

#if !defined(AFX_APPROVALDLG_H__INCLUDED_)
#define AFX_APPROVALDLG_H__INCLUDED_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

class CApprovalDlg : public CDialog
{
public:
    // Text is already converted for display (ANSI, CRLF line breaks).
    // DoModal() returns IDOK for Allow and IDCANCEL for Deny.
    CApprovalDlg(LPCTSTR tool, LPCTSTR input, CWnd *pParent = NULL);

    //{{AFX_DATA(CApprovalDlg)
    enum { IDD = IDD_APPROVAL };
    //}}AFX_DATA

protected:
    CString m_tool;
    CString m_input;
    CFont m_font;

    //{{AFX_MSG(CApprovalDlg)
    virtual BOOL OnInitDialog();
    //}}AFX_MSG
    DECLARE_MESSAGE_MAP()
};

#endif // !defined(AFX_APPROVALDLG_H__INCLUDED_)
