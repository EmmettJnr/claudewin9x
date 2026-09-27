// ClaudeGui.h : main header file for the ClaudeWin9x GUI client

#if !defined(AFX_CLAUDEGUI_H__INCLUDED_)
#define AFX_CLAUDEGUI_H__INCLUDED_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#ifndef __AFXWIN_H__
#error include 'stdafx.h' before including this file for PCH
#endif

#include "resource.h"

class CClaudeGuiApp : public CWinApp
{
public:
    CClaudeGuiApp();

    //{{AFX_VIRTUAL(CClaudeGuiApp)
public:
    virtual BOOL InitInstance();
    //}}AFX_VIRTUAL

    //{{AFX_MSG(CClaudeGuiApp)
    //}}AFX_MSG
    DECLARE_MESSAGE_MAP()
};

#endif // !defined(AFX_CLAUDEGUI_H__INCLUDED_)
