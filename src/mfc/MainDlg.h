#pragma once
#include "pch.h"
#include "resource.h"
#include "StreamClient.h"
#include <map>
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
    afx_msg void    OnCbnSelchangeMode();
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
    // plot = 라벨까지 쓸 수 있는 전체 영역, data = 선이 그려지는 영역 (위·아래 라벨 전용 띠를 뺀 것)
    void draw_annotations(CDC& dc, const CRect& plot, const CRect& data, long long week_start, int ymax);
    int  label_rows_for(long long week_start) const;   // 그 주의 제목 변경 수에 따른 라벨 전용 줄 수 (0/1/2)
    void draw_hover(CDC& dc);
    // 상위 10 최근 24h 모드
    enum Mode { kModeSingle = 0, kModeMulti = 1 };
    Mode mode() const;
    void request_multi();                            // view_ 앞 10개의 최근 24시간 데이터 요청
    void draw_multi(CDC& dc, const CRect& rc);
    void update_hover(CPoint pt);                    // pt: 차트 컨트롤 클라이언트 좌표
    static long long week_start_kst(long long ts);   // ts 가 속한 주의 일요일 00:00 KST (epoch)
    static CString fmt_kst(long long ts, const wchar_t* fmt);

    CListCtrl  list_;
    CComboBox  mode_;
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

    // 상위 14 당일 모드: 요청한 채널(id, name) 순서와 채널별 응답
    static constexpr int kMultiCount = 10;
    std::vector<std::pair<std::wstring, std::wstring>> multi_ids_;
    std::map<std::wstring, std::unique_ptr<sm::Samples>> multi_;
    long long multi_from_ = 0;                      // 24시간 창의 시작 (now - 24h, 분 단위 정렬)
    std::vector<CRect> multi_rows_;                 // 그리기 시 각 줄의 plot 영역 (툴팁용)
    int  hover_multi_ = -1;                         // 툴팁 대상 줄

    // 마지막 그리기의 두 줄 영역 (툴팁 좌표 변환용)
    CRect     row_rect_[2];     // 각 줄의 데이터 영역 (툴팁 좌표 변환용)
    int       ymax_ = 1;
    // 마우스 위치의 샘플. hover_idx_ < 0 이면 없음
    int       hover_idx_ = -1;
    int       hover_row_ = 0;
    bool      tracking_ = false;

    std::unique_ptr<sm::StreamClient> client_;
    HICON icon_ = nullptr;
};
