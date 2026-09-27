// ClaudeGuiDlg.h : main chat window

#if !defined(AFX_CLAUDEGUIDLG_H__INCLUDED_)
#define AFX_CLAUDEGUIDLG_H__INCLUDED_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

class CClaudeGuiDlg : public CDialog
{
public:
    CClaudeGuiDlg(CWnd *pParent = NULL);

    //{{AFX_DATA(CClaudeGuiDlg)
    enum { IDD = IDD_CLAUDEGUI_DIALOG };
    CRichEditCtrl m_output;
    CEdit m_input;
    //}}AFX_DATA

    //{{AFX_VIRTUAL(CClaudeGuiDlg)
public:
    virtual BOOL PreTranslateMessage(MSG *pMsg);

protected:
    virtual void DoDataExchange(CDataExchange *pDX);
    //}}AFX_VIRTUAL

protected:
    HICON m_hIcon;
    CFont m_font;
    CStringArray m_history;
    int m_historyPos;
    BOOL m_waiting;
    BOOL m_ready; // controls exist, safe to lay out
    CSize m_minSize;
    int m_lineHeight; // output line height in pixels, for scrolling

    void UpgradeRichEdit();
    int VisibleOutputLines();
    BOOL IsOutputAtEnd();
    void ScrollOutputToEnd();
    void AppendText(LPCTSTR text, COLORREF color, BOOL bold);
    void AppendLine(LPCTSTR text, COLORREF color);
    void UpdateControls();
    void LayoutControls(int cx, int cy);
    void RecallHistory(int delta);

    //{{AFX_MSG(CClaudeGuiDlg)
    virtual BOOL OnInitDialog();
    virtual void OnOK();
    virtual void OnCancel();
    afx_msg void OnClose();
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO FAR *lpMMI);
    afx_msg void OnConnect();
    afx_msg void OnSend();
    afx_msg void OnClear();
    afx_msg void OnAutoApprove();
    //}}AFX_MSG
    afx_msg LRESULT OnCoreText(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnCoreApproval(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnCoreStopped(WPARAM wParam, LPARAM lParam);
    DECLARE_MESSAGE_MAP()
};

#endif // !defined(AFX_CLAUDEGUIDLG_H__INCLUDED_)
