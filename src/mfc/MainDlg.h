#pragma once
#include "pch.h"
#include "resource.h"
#include "StreamClient.h"
#include <memory>
#include <vector>

// 좌: 채널 목록 / 우: 선택 채널의 시청자수 차트 (2주, 일~토 한 줄씩 2줄).
class CMainDlg : public CDialogEx {
public:
    explicit CMainDlg(CWnd* pParent = nullptr);
    enum { IDD = IDD_MAIN };

    // 수신 스레드 → UI 스레드
    static constexpr UINT WM_SM_CHANNELS = WM_APP + 1;
    static constexpr UINT WM_SM_SAMPLES  = WM_APP + 2;
    static constexpr UINT WM_SM_STATUS   = WM_APP + 3;

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    void OnOK() override;          // Enter = 조회
    void OnCancel() override;      // ESC 무시, 닫기는 X 로만

    afx_msg void    OnSize(UINT nType, int cx, int cy);
    afx_msg void    OnGetMinMaxInfo(MINMAXINFO* mmi);
    afx_msg void    OnDrawItem(int id, LPDRAWITEMSTRUCT dis);
    BOOL PreTranslateMessage(MSG* pMsg) override;   // 차트 위 마우스 이동/이탈 → 툴팁
    afx_msg void    OnDestroy();
    afx_msg void    OnClose();
    afx_msg void    OnTimer(UINT_PTR id);
    afx_msg void    OnBnClickedRefresh();
    afx_msg void    OnBnClickedQuery();
    afx_msg void    OnBnClickedLiveOnly();
    afx_msg void    OnListDblClk(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg LRESULT OnSmChannels(WPARAM, LPARAM lParam);
    afx_msg LRESULT OnSmSamples(WPARAM, LPARAM lParam);
    afx_msg LRESULT OnSmStatus(WPARAM, LPARAM lParam);
    DECLARE_MESSAGE_MAP()

private:
    void layout(int cx, int cy);
    void save_placement();
    void restore_placement();
    void fill_channels(const std::wstring& keep_selected);
    void make_icons();
    void draw_chart(CDC& dc, const CRect& rc);
    void draw_week(CDC& dc, const CRect& plot, long long week_start, int ymax, int ystep, int row);
    void draw_annotations(CDC& dc, const CRect& plot, long long week_start, int ymax);
    void draw_hover(CDC& dc);
    void update_hover(CPoint pt);                    // pt: 차트 컨트롤 클라이언트 좌표
    static long long week_start_kst(long long ts);   // ts 가 속한 주의 일요일 00:00 KST (epoch)
    static CString fmt_kst(long long ts, const wchar_t* fmt);

    CListCtrl  list_;
    CButton    chk_live_;
    CImageList icons_;      // 0 = 상위권 밖(회색 ○), 1 = 방송 중·상위권(초록 ●)
    CStatic    status_;
    CStatic    chart_;
    CStatic    chart_title_;

    std::vector<sm::Channel>     channels_;   // 서버 순서 그대로
    std::vector<int>             view_;       // 리스트 행 → channels_ 인덱스 (정렬·필터 적용)
    std::unique_ptr<sm::Samples> samples_;
    CString                      samples_name_;
    long long                    week0_ = 0;   // 지난주 일요일 00:00 KST
    std::wstring                 samples_id_;  // 마지막 조회 채널 (1분 갱신용)

    // 마지막 그리기의 두 줄 영역 (툴팁 좌표 변환용)
    CRect     row_rect_[2];
    int       ymax_ = 1;
    // 마우스 위치의 샘플. hover_idx_ < 0 이면 없음
    int       hover_idx_ = -1;
    int       hover_row_ = 0;
    bool      tracking_ = false;

    std::unique_ptr<sm::StreamClient> client_;
    HICON icon_ = nullptr;
};
