#include "pch.h"
#include "App.h"
#include "MainDlg.h"

CStreamMonitorApp theApp;

BOOL CStreamMonitorApp::InitInstance() {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    ::InitCommonControlsEx(&icc);
    CWinApp::InitInstance();
    SetRegistryKey(L"StreamMonitor");   // HKCU\Software\StreamMonitor\<section>

    CMainDlg dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();
    return FALSE;   // 대화상자 종료 = 앱 종료
}
