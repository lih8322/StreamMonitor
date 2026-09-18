#include "pch.h"
#include "MainDlg.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <ctime>
#include <windowsx.h>

namespace {

constexpr int kMargin = 8;
constexpr int kListWidth = 300;
constexpr int kBtnH = 26;
constexpr long long kPollSec = 60;   // 서버 수집 주기

constexpr long long kKstOffset = 9 * 3600;      // KST = UTC+9, DST 없음
constexpr long long kWeek = 7 * 86400;
constexpr UINT_PTR  kRefreshTimer = 1;
constexpr UINT      kRefreshMs    = 60 * 1000;   // 채널 목록 자동 새로고침
constexpr long long kLiveWindow   = 2 * kPollSec; // last_seen 이 이 안이면 "상위권 안"
const wchar_t* const kWeekdays[] = {L"일", L"월", L"화", L"수", L"목", L"금", L"토"};

// 창 위치/크기 영속화 (Prism FlowChartDlg 와 같은 방식: 레지스트리에 WINDOWPLACEMENT 통째로)
constexpr const wchar_t* kPlacementSection = L"MainDlg";
constexpr const wchar_t* kPlacementEntry   = L"WindowPlacement";

} // namespace

BEGIN_MESSAGE_MAP(CMainDlg, CDialogEx)
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
    ON_WM_DRAWITEM()
    ON_WM_DESTROY()
    ON_WM_CLOSE()
    ON_WM_TIMER()
    ON_BN_CLICKED(IDC_BTN_REFRESH, &CMainDlg::OnBnClickedRefresh)
    ON_BN_CLICKED(IDC_BTN_QUERY,   &CMainDlg::OnBnClickedQuery)
    ON_BN_CLICKED(IDC_CHK_LIVE,    &CMainDlg::OnBnClickedLiveOnly)
    ON_CBN_SELCHANGE(IDC_MODE,     &CMainDlg::OnCbnSelchangeMode)
    ON_NOTIFY(NM_DBLCLK, IDC_CHANNEL_LIST, &CMainDlg::OnListDblClk)
    ON_MESSAGE(WM_SM_CHANNELS, &CMainDlg::OnSmChannels)
    ON_MESSAGE(WM_SM_SAMPLES,  &CMainDlg::OnSmSamples)
    ON_MESSAGE(WM_SM_STATUS,   &CMainDlg::OnSmStatus)
END_MESSAGE_MAP()

CMainDlg::CMainDlg(CWnd* pParent) : CDialogEx(IDD_MAIN, pParent) {
    icon_ = AfxGetApp()->LoadIcon(IDI_APP);
}

void CMainDlg::DoDataExchange(CDataExchange* pDX) {
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_CHANNEL_LIST, list_);
    DDX_Control(pDX, IDC_CHK_LIVE,     chk_live_);
    DDX_Control(pDX, IDC_MODE,         mode_);
    DDX_Control(pDX, IDC_STATUS,       status_);
    DDX_Control(pDX, IDC_CHART,        chart_);
    DDX_Control(pDX, IDC_CHART_TITLE,  chart_title_);
}

BOOL CMainDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();
    SetIcon(icon_, TRUE);
    SetIcon(icon_, FALSE);

    list_.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    make_icons();
    mode_.AddString(L"채널 2주 분석");
    mode_.AddString(L"상위 14 당일");
    mode_.SetCurSel(kModeSingle);
    list_.SetImageList(&icons_, LVSIL_SMALL);
    list_.InsertColumn(0, L"채널", LVCFMT_LEFT, 120);
    list_.InsertColumn(1, L"현재", LVCFMT_RIGHT, 55);
    list_.InsertColumn(2, L"최고(7일)", LVCFMT_RIGHT, 62);
    list_.InsertColumn(3, L"마지막 관측", LVCFMT_LEFT, 90);

    // 수신 스레드 콜백 → PostMessage. 포인터 소유권은 메시지 쪽으로 넘긴다.
    client_ = std::make_unique<sm::StreamClient>();
    const HWND hwnd = GetSafeHwnd();
    client_->set_handlers(
        [hwnd](std::unique_ptr<std::vector<sm::Channel>> ch) {
            ::PostMessage(hwnd, WM_SM_CHANNELS, 0, reinterpret_cast<LPARAM>(ch.release()));
        },
        [hwnd](std::unique_ptr<sm::Samples> s) {
            ::PostMessage(hwnd, WM_SM_SAMPLES, 0, reinterpret_cast<LPARAM>(s.release()));
        },
        [hwnd](const std::wstring& msg) {
            ::PostMessage(hwnd, WM_SM_STATUS, 0, reinterpret_cast<LPARAM>(new std::wstring(msg)));
        });
    client_->start();
    SetTimer(kRefreshTimer, kRefreshMs, nullptr);

    restore_placement();
    CRect rc;
    GetClientRect(&rc);
    layout(rc.Width(), rc.Height());
    return TRUE;
}

void CMainDlg::OnClose() {
    save_placement();
    CDialogEx::OnClose();
}

void CMainDlg::save_placement() {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(&wp)) return;
    AfxGetApp()->WriteProfileBinary(kPlacementSection, kPlacementEntry,
                                    reinterpret_cast<LPBYTE>(&wp), sizeof(wp));
}

void CMainDlg::restore_placement() {
    LPBYTE data = nullptr;
    UINT   len  = 0;
    if (!AfxGetApp()->GetProfileBinary(kPlacementSection, kPlacementEntry, &data, &len)) {
        delete[] data;
        return;
    }
    if (data && len == sizeof(WINDOWPLACEMENT)) {
        WINDOWPLACEMENT wp{};
        std::memcpy(&wp, data, sizeof(wp));
        wp.length = sizeof(wp);
        if (wp.showCmd == SW_SHOWMINIMIZED) wp.showCmd = SW_SHOWNORMAL;   // 최소화 상태로 시작하지 않는다
        // 저장된 위치가 현재 모니터 밖이면(모니터 구성 변경) 기본 위치로
        if (::MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONULL) != nullptr)
            SetWindowPlacement(&wp);
    }
    delete[] data;
}

void CMainDlg::OnDestroy() {
    KillTimer(kRefreshTimer);
    if (client_) { client_->stop(); client_.reset(); }
    CDialogEx::OnDestroy();
}

void CMainDlg::OnOK()     { OnBnClickedQuery(); }
void CMainDlg::OnCancel() { save_placement(); DestroyWindow(); }

void CMainDlg::OnGetMinMaxInfo(MINMAXINFO* mmi) {
    mmi->ptMinTrackSize = CPoint(700, 420);
    CDialogEx::OnGetMinMaxInfo(mmi);
}

void CMainDlg::OnSize(UINT nType, int cx, int cy) {
    CDialogEx::OnSize(nType, cx, cy);
    if (list_.GetSafeHwnd()) layout(cx, cy);
}

void CMainDlg::layout(int cx, int cy) {
    const int statusH = 18;
    const int bottom  = cy - kMargin - statusH;          // 상태줄 위
    const int btnTop  = bottom - kBtnH - 4;

    list_.MoveWindow(kMargin, kMargin, kListWidth, btnTop - kMargin - 6);
    GetDlgItem(IDC_BTN_REFRESH)->MoveWindow(kMargin, btnTop, 100, kBtnH);
    chk_live_.MoveWindow(kMargin + 108, btnTop + 4, 70, kBtnH - 6);
    GetDlgItem(IDC_BTN_QUERY)->MoveWindow(kMargin + kListWidth - 90, btnTop, 90, kBtnH);

    const int chartL = kMargin + kListWidth + kMargin;
    mode_.MoveWindow(chartL, kMargin - 2, 130, 200);
    chart_title_.MoveWindow(chartL + 138, kMargin, cx - chartL - 138 - kMargin, 18);
    chart_.MoveWindow(chartL, kMargin + 22, cx - chartL - kMargin, bottom - kMargin - 22 - 4);
    status_.MoveWindow(kMargin, bottom + 2, cx - 2 * kMargin, statusH);
    chart_.Invalidate();
}

// ── 채널 목록 ─────────────────────────────────────────────────────────────
// 16x16 아이콘 두 개를 GDI 로 그려 이미지 리스트에 넣는다 (리소스 파일 불필요).
void CMainDlg::make_icons() {
    icons_.Create(16, 16, ILC_COLOR32 | ILC_MASK, 2, 0);
    CClientDC screen(this);
    for (int state = 0; state < 2; ++state) {
        CDC dc; dc.CreateCompatibleDC(&screen);
        CBitmap bmp; bmp.CreateCompatibleBitmap(&screen, 16, 16);
        CBitmap* old = dc.SelectObject(&bmp);
        const COLORREF mask = RGB(255, 0, 255);
        dc.FillSolidRect(0, 0, 16, 16, mask);
        CPen pen(PS_SOLID, 1, state ? RGB(30, 120, 60) : RGB(150, 150, 150));
        CBrush brush(state ? RGB(46, 180, 90) : RGB(255, 255, 255));
        CPen* op = dc.SelectObject(&pen); CBrush* ob = dc.SelectObject(&brush);
        dc.Ellipse(3, 3, 13, 13);
        dc.SelectObject(op); dc.SelectObject(ob); dc.SelectObject(old);
        icons_.Add(&bmp, mask);
    }
}

void CMainDlg::OnTimer(UINT_PTR id) {
    if (id == kRefreshTimer && client_ && client_->connected()) {
        client_->request_channels();
        if (mode() == kModeMulti) { if (!multi_ids_.empty()) request_multi(); }
        // 조회 중인 채널이 있으면 차트도 같은 구간으로 다시 받는다 (주가 바뀌면 자동으로 새 주 기준)
        else if (!samples_id_.empty()) {
            const long long now = static_cast<long long>(std::time(nullptr));
            week0_ = week_start_kst(now) - kWeek;
            client_->request_samples(samples_id_, week0_, now);
        }
    }
    CDialogEx::OnTimer(id);
}

void CMainDlg::OnBnClickedRefresh() {
    if (!client_ || !client_->request_channels())
        status_.SetWindowTextW(L"연결되지 않음");
}

LRESULT CMainDlg::OnSmChannels(WPARAM, LPARAM lParam) {
    std::unique_ptr<std::vector<sm::Channel>> ch(reinterpret_cast<std::vector<sm::Channel>*>(lParam));
    if (!ch) return 0;
    std::wstring selected;
    if (const int cur = list_.GetNextItem(-1, LVNI_SELECTED); cur >= 0 && cur < static_cast<int>(view_.size()))
        selected = channels_[view_[cur]].id;
    channels_ = std::move(*ch);
    fill_channels(selected);

    int live = 0;
    const long long now = static_cast<long long>(std::time(nullptr));
    for (const auto& c : channels_) if (now - c.last_seen <= kLiveWindow) ++live;
    CString s;
    s.Format(L"채널 %d개 (상위권 %d개)  %s", static_cast<int>(channels_.size()), live,
             fmt_kst(now, L"%H:%M:%S").GetString());
    status_.SetWindowTextW(s);
    return 0;
}

void CMainDlg::OnBnClickedLiveOnly() {
    std::wstring selected;
    if (const int cur = list_.GetNextItem(-1, LVNI_SELECTED); cur >= 0 && cur < static_cast<int>(view_.size()))
        selected = channels_[view_[cur]].id;
    fill_channels(selected);
}

// 정렬(현재 시청자수 내림차순)·필터(LIVE만)를 view_ 에 반영하고, 행을 제자리에서 갱신한다
// — 삭제 후 재삽입하지 않으므로 스크롤 위치와 선택이 흔들리지 않는다.
void CMainDlg::fill_channels(const std::wstring& keep_selected) {
    const long long now = static_cast<long long>(std::time(nullptr));
    const bool live_only = chk_live_.GetCheck() == BST_CHECKED;
    auto is_live = [&](const sm::Channel& c) { return now - c.last_seen <= kLiveWindow; };

    view_.clear();
    for (int i = 0; i < static_cast<int>(channels_.size()); ++i)
        if (!live_only || is_live(channels_[i])) view_.push_back(i);
    // 현재 시청자수 내림차순. 상위권 밖(오프라인)은 0 취급 → 자연히 뒤로. 동률이면 7일 최고치.
    std::stable_sort(view_.begin(), view_.end(), [&](int a, int b) {
        const int ca = is_live(channels_[a]) ? channels_[a].current : 0;
        const int cb = is_live(channels_[b]) ? channels_[b].current : 0;
        if (ca != cb) return ca > cb;
        return channels_[a].peak > channels_[b].peak;
    });

    const int n = static_cast<int>(view_.size());
    list_.SetRedraw(FALSE);
    while (list_.GetItemCount() > n) list_.DeleteItem(list_.GetItemCount() - 1);
    for (int i = 0; i < n; ++i) {
        const auto& c = channels_[view_[i]];
        const bool live = is_live(c);
        CString cur;  cur.Format(L"%d", live ? c.current : 0);
        CString peak; peak.Format(L"%d", c.peak);
        const CString seen = live ? L"LIVE" : fmt_kst(c.last_seen, L"%m-%d %H:%M");
        if (i >= list_.GetItemCount()) {
            list_.InsertItem(LVIF_TEXT | LVIF_IMAGE, i, c.name.c_str(), 0, 0, live ? 1 : 0, 0);
        } else {
            LVITEMW it{};
            it.mask = LVIF_TEXT | LVIF_IMAGE; it.iItem = i;
            it.pszText = const_cast<wchar_t*>(c.name.c_str()); it.iImage = live ? 1 : 0;
            list_.SetItem(&it);
        }
        list_.SetItemText(i, 1, cur);
        list_.SetItemText(i, 2, peak);
        list_.SetItemText(i, 3, seen);
        const bool sel = !keep_selected.empty() && c.id == keep_selected;
        list_.SetItemState(i, sel ? (LVIS_SELECTED | LVIS_FOCUSED) : 0, LVIS_SELECTED | LVIS_FOCUSED);
    }
    list_.SetRedraw(TRUE);
    list_.Invalidate();
}

// ── 조회 ──────────────────────────────────────────────────────────────────
void CMainDlg::OnListDblClk(NMHDR*, LRESULT* pResult) {
    OnBnClickedQuery();
    *pResult = 0;
}

CMainDlg::Mode CMainDlg::mode() const {
    return mode_.GetSafeHwnd() && mode_.GetCurSel() == kModeMulti ? kModeMulti : kModeSingle;
}

void CMainDlg::OnCbnSelchangeMode() {
    hover_idx_ = -1; hover_multi_ = -1;
    if (mode() == kModeMulti) {
        if (multi_ids_.empty()) request_multi();
        chart_title_.SetWindowTextW(L"상위 14 채널 — 오늘 (00:00 KST 부터)");
    } else if (samples_) {
        OnBnClickedQuery();   // 단일 모드로 돌아오면 선택 채널을 다시 조회
    }
    chart_.Invalidate();
}

// 리스트(정렬·필터 적용) 앞 kMultiCount 개 채널의 오늘 데이터를 요청한다
void CMainDlg::request_multi() {
    if (!client_ || !client_->connected()) { status_.SetWindowTextW(L"연결되지 않음"); return; }
    const long long now = static_cast<long long>(std::time(nullptr));
    day0_ = (now + kKstOffset) / 86400 * 86400 - kKstOffset;   // 오늘 00:00 KST
    multi_ids_.clear();
    for (int i = 0; i < static_cast<int>(view_.size()) && i < kMultiCount; ++i)
        multi_ids_.emplace_back(channels_[view_[i]].id, channels_[view_[i]].name);
    // 목록에서 빠진 채널의 이전 응답은 버린다
    for (auto it = multi_.begin(); it != multi_.end();) {
        bool keep = false;
        for (const auto& [id, name] : multi_ids_) if (id == it->first) { keep = true; break; }
        it = keep ? std::next(it) : multi_.erase(it);
    }
    for (const auto& [id, name] : multi_ids_) client_->request_samples(id, day0_, now);
    CString st; st.Format(L"상위 %d 채널 오늘 데이터 조회 중...", static_cast<int>(multi_ids_.size()));
    status_.SetWindowTextW(st);
}

void CMainDlg::OnBnClickedQuery() {
    if (mode() == kModeMulti) { request_multi(); return; }
    const int cur = list_.GetNextItem(-1, LVNI_SELECTED);
    if (cur < 0 || cur >= static_cast<int>(view_.size())) {
        status_.SetWindowTextW(L"채널을 선택하세요");
        return;
    }
    const auto& c = channels_[view_[cur]];
    // 이번 주(일~토) + 지난 주 = 지난주 일요일 00:00 KST 부터 지금까지
    const long long now  = static_cast<long long>(std::time(nullptr));
    const long long from = week_start_kst(now) - kWeek;

    if (!client_ || !client_->request_samples(c.id, from, now)) {
        status_.SetWindowTextW(L"연결되지 않음");
        return;
    }
    samples_name_ = c.name.c_str();
    samples_id_   = c.id;
    week0_ = from;
    CString s; s.Format(L"%s 조회 중 (2주)...", c.name.c_str());
    status_.SetWindowTextW(s);
}

LRESULT CMainDlg::OnSmSamples(WPARAM, LPARAM lParam) {
    std::unique_ptr<sm::Samples> s(reinterpret_cast<sm::Samples*>(lParam));
    if (!s) return 0;
    // 상위 14 당일 모드의 응답: 요청 목록에 있는 채널이면 채널별 저장
    if (s->from == day0_ && day0_ != 0) {
        for (const auto& [id, name] : multi_ids_)
            if (id == s->channel_id) {
                multi_[id] = std::move(s);
                if (mode() == kModeMulti) {
                    CString st; st.Format(L"당일 %d/%d 수신  %s", static_cast<int>(multi_.size()), static_cast<int>(multi_ids_.size()),
                                          fmt_kst(static_cast<long long>(std::time(nullptr)), L"%H:%M:%S").GetString());
                    status_.SetWindowTextW(st);
                    chart_.Invalidate();
                }
                return 0;
            }
    }
    if (s->channel_id != samples_id_) return 0;   // 늦게 도착한 이전 채널 응답은 버린다
    samples_ = std::move(s);
    hover_idx_ = -1;

    int peak = 0;
    for (const auto& p : samples_->points) peak = std::max(peak, p.viewers);
    const int mins = static_cast<int>(samples_->points.size()) * static_cast<int>(kPollSec / 60);
    CString dur;
    if (mins >= 60) dur.Format(L"%dh %02dm", mins / 60, mins % 60); else dur.Format(L"%dm", mins);
    CString t;
    t.Format(L"%s — 상위권 %s, 최고 %d명   (위: %s 주 / 아래: %s 주, KST)", samples_name_.GetString(),
             dur.GetString(), peak,
             fmt_kst(week0_,         L"%m-%d").GetString(),
             fmt_kst(week0_ + kWeek, L"%m-%d").GetString());
    chart_title_.SetWindowTextW(t);
    CString st; st.Format(L"차트 갱신 %s", fmt_kst(static_cast<long long>(std::time(nullptr)), L"%H:%M:%S").GetString());
    status_.SetWindowTextW(st);
    chart_.Invalidate();
    return 0;
}

LRESULT CMainDlg::OnSmStatus(WPARAM, LPARAM lParam) {
    std::unique_ptr<std::wstring> msg(reinterpret_cast<std::wstring*>(lParam));
    if (msg) status_.SetWindowTextW(msg->c_str());
    return 0;
}

// ── 차트 ──────────────────────────────────────────────────────────────────
void CMainDlg::OnDrawItem(int id, LPDRAWITEMSTRUCT dis) {
    if (id != IDC_CHART) { CDialogEx::OnDrawItem(id, dis); return; }
    CDC* dc = CDC::FromHandle(dis->hDC);
    CRect rc(dis->rcItem);

    // 더블 버퍼
    CDC mem;
    mem.CreateCompatibleDC(dc);
    CBitmap bmp;
    bmp.CreateCompatibleBitmap(dc, rc.Width(), rc.Height());
    CBitmap* old = mem.SelectObject(&bmp);
    CRect local(0, 0, rc.Width(), rc.Height());
    draw_chart(mem, local);
    dc->BitBlt(rc.left, rc.top, rc.Width(), rc.Height(), &mem, 0, 0, SRCCOPY);
    mem.SelectObject(old);
}

CString CMainDlg::fmt_kst(long long ts, const wchar_t* fmt) {
    const time_t t = static_cast<time_t>(ts + kKstOffset);
    tm tmv{};
    gmtime_s(&tmv, &t);
    wchar_t buf[64];
    wcsftime(buf, std::size(buf), fmt, &tmv);
    return buf;
}

long long CMainDlg::week_start_kst(long long ts) {
    const long long days = (ts + kKstOffset) / 86400;   // KST 기준 일 수
    const int dow = static_cast<int>((days + 4) % 7);   // 1970-01-01 = 목(4). 0 = 일요일
    return (days - dow) * 86400 - kKstOffset;
}

void CMainDlg::draw_chart(CDC& dc, const CRect& rc) {
    dc.FillSolidRect(rc, RGB(255, 255, 255));
    CFont font;
    font.CreatePointFont(85, L"맑은 고딕");
    CFont* oldFont = dc.SelectObject(&font);
    dc.SetBkMode(TRANSPARENT);

    if (mode() == kModeMulti) { draw_multi(dc, rc); dc.SelectObject(oldFont); return; }

    if (!samples_ || samples_->points.empty()) {
        dc.SetTextColor(RGB(120, 120, 120));
        CRect r(rc);
        dc.DrawText(samples_ ? L"데이터 없음" : L"채널을 선택하고 조회를 누르세요", -1, &r,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        dc.SelectObject(oldFont);
        return;
    }

    // y 축은 두 줄이 공유 (같은 눈금으로 비교)
    int vmax = 0;
    for (const auto& p : samples_->points) vmax = std::max(vmax, p.viewers);
    int step = 1;
    for (int base = 1;; base *= 10)
        for (int m : {1, 2, 5}) { if (vmax / (base * m) < 6) { step = base * m; goto found; } }
found:
    const int ymax = std::max(step, (vmax / step + 1) * step);

    // 두 줄: 위 = 지난주, 아래 = 이번주
    const int left = rc.left + 60, right = rc.right - 12;
    const int top = rc.top + 40, bottom = rc.bottom - 22;   // 아래 6시간 라벨 자리
    const int gap = 30;
    const int rowH = (bottom - top - gap) / 2;
    if (right - left < 10 || rowH < 10) { dc.SelectObject(oldFont); return; }

    ymax_ = ymax;
    draw_week(dc, CRect(left, top, right, top + rowH), week0_, ymax, step, 0);
    draw_week(dc, CRect(left, top + rowH + gap, right, bottom), week0_ + kWeek, ymax, step, 1);
    draw_hover(dc);

    // 현재 제목 (마지막 info) — 차트 상단 왼쪽
    if (!samples_->info.empty()) {
        const auto& last = samples_->info.back();
        dc.SetTextColor(RGB(60, 60, 60));
        CString s; s.Format(L"[%s] %s", last.category.c_str(), last.title.c_str());
        dc.DrawText(s, CRect(left, rc.top + 2, right, rc.top + 20), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    dc.SelectObject(oldFont);
}

// 한 주(일요일 00:00 ~ 토요일 24:00 KST)를 plot 에 그린다.
int CMainDlg::label_rows_for(long long week_start) const {
    if (!samples_) return 0;
    int n = 0;
    for (const auto& inf : samples_->info)
        if (inf.ts >= week_start && inf.ts < week_start + kWeek) ++n;
    return n == 0 ? 0 : (n > 6 ? 2 : 1);
}

void CMainDlg::draw_week(CDC& dc, const CRect& plot, long long week_start, int ymax, int ystep, int row) {
    const long long t0 = week_start, t1 = week_start + kWeek;
    // 위·아래에 라벨 전용 띠를 확보한다. 선은 data 안에만 그려지므로 띠 안의 라벨은 선과 절대 겹치지 않는다.
    constexpr int kLabelRowH = 2 * 14 + 2 + 3;   // draw_annotations 의 kLabelH + kGap 과 같은 값
    const int reserve = label_rows_for(week_start) * kLabelRowH;
    CRect data(plot.left, plot.top + reserve, plot.right, plot.bottom - reserve);
    if (data.Height() < 40) data = plot;          // 창이 너무 작으면 띠를 포기
    row_rect_[row] = data;
    auto X = [&](long long ts) { return plot.left + static_cast<int>((ts - t0) * static_cast<long long>(plot.Width()) / kWeek); };
    auto Y = [&](int v) { return data.bottom - static_cast<int>(static_cast<long long>(v) * data.Height() / ymax); };

    CPen grid(PS_SOLID, 1, RGB(232, 232, 232));
    CPen dayline(PS_SOLID, 1, RGB(150, 150, 150));
    CPen axis(PS_SOLID, 1, RGB(120, 120, 120));
    CPen* oldPen = dc.SelectObject(&grid);
    dc.SetTextColor(RGB(90, 90, 90));

    // y 눈금 (data 영역 안)
    for (int v = 0; v <= ymax; v += ystep) {
        const int y = Y(v);
        dc.MoveTo(plot.left, y); dc.LineTo(plot.right, y);
        CString s; s.Format(L"%d", v);
        dc.DrawText(s, CRect(plot.left - 58, y - 8, plot.left - 4, y + 8), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    // 6시간 보조 격자 (+ 아랫줄엔 시각 라벨)
    for (long long t = t0; t < t1; t += 6 * 3600) {
        const int x = X(t);
        if ((t - t0) % 86400 != 0) { dc.MoveTo(x, plot.top); dc.LineTo(x, plot.bottom); }
        if (row == 1) {
            const int hour = static_cast<int>(((t - t0) % 86400) / 3600);
            CString s; s.Format(L"%02d", hour);
            dc.DrawText(s, CRect(x - 14, plot.bottom + 2, x + 14, plot.bottom + 16), DT_CENTER | DT_SINGLELINE);
        }
    }
    // 요일 경계 수직선 + 요일/날짜 라벨 (칸 위쪽)
    dc.SelectObject(&dayline);
    for (int d = 0; d <= 7; ++d) {
        const long long t = t0 + d * 86400LL;
        const int x = X(t);
        dc.MoveTo(x, plot.top - 14); dc.LineTo(x, plot.bottom);
        if (d < 7) {
            CString s; s.Format(L"%s %s", kWeekdays[d], fmt_kst(t, L"%m/%d").GetString());
            const int x2 = X(t + 86400);
            dc.DrawText(s, CRect(x + 2, plot.top - 16, x2 - 2, plot.top - 1), DT_CENTER | DT_SINGLELINE);
        }
    }
    dc.SelectObject(&axis);
    dc.MoveTo(plot.left, plot.top); dc.LineTo(plot.left, plot.bottom); dc.LineTo(plot.right, plot.bottom);
    // 라벨 띠 경계 (아주 연하게)
    if (reserve > 0) {
        CPen bandline(PS_DOT, 1, RGB(225, 225, 225));
        dc.SelectObject(&bandline);
        dc.MoveTo(plot.left, data.top); dc.LineTo(plot.right, data.top);
        dc.MoveTo(plot.left, data.bottom); dc.LineTo(plot.right, data.bottom);
    }

    // 선 그리기: shift 만큼 시각을 옮겨서 (지난주를 이번주 칸에 겹칠 때 +1주). 2분 넘게 끊기면 잇지 않는다
    auto draw_line = [&](long long shift) {
        bool pen_down = false;
        long long prev_ts = 0;
        for (const auto& p : samples_->points) {
            const long long ts = p.ts + shift;
            if (ts < t0 || ts >= t1) continue;
            const int x = X(ts), y = Y(p.viewers);
            if (!pen_down || ts - prev_ts > 2 * kPollSec) { dc.MoveTo(x, y); pen_down = true; }
            else dc.LineTo(x, y);
            prev_ts = ts;
        }
    };
    // 아랫줄(이번주)에는 지난주를 회색으로 겹쳐 같은 요일·시각끼리 비교
    if (row == 1) {
        CPen gray(PS_SOLID, 1, RGB(175, 175, 175));
        dc.SelectObject(&gray);
        draw_line(kWeek);
    }
    CPen line(PS_SOLID, 2, RGB(46, 139, 87));
    dc.SelectObject(&line);
    draw_line(0);
    dc.SelectObject(oldPen);

    draw_annotations(dc, plot, data, week_start, ymax);
}

// 제목/카테고리 변경 라벨.
// 점수 기반 배치: 점 주변에 후보 위치를 격자로 깔고, 점에서 라벨까지의 직선 거리를 기본 점수로 하여
// 가장 가까운 자리를 고른다. 다른 라벨과 겹치면 탈락, 데이터 선과 겹치면 탈락, 리더 라인이 선을
// 가로지르면 벌점, 라벨 전용 띠(plot 에서 data 를 뺀 위·아래)가 아닌 데이터 영역 안이면 벌점.
// 리더는 점에서 라벨 사각형의 가장 가까운 점까지 직선(대각선)으로 잇는다.
void CMainDlg::draw_annotations(CDC& dc, const CRect& plot, const CRect& data, long long week_start, int ymax) {
    const long long t0 = week_start, t1 = week_start + kWeek;
    auto X = [&](long long ts) { return plot.left + static_cast<int>((ts - t0) * static_cast<long long>(plot.Width()) / kWeek); };
    auto Y = [&](int v) { return data.bottom - static_cast<int>(static_cast<long long>(v) * data.Height() / ymax); };

    constexpr int kLineH = 14, kLabelH = 2 * kLineH + 2, kMaxLabelW = 220, kGap = 3;

    // 데이터 선이 x 열마다 차지하는 y 구간 [col_top, col_bot]
    const int W = std::max(1, static_cast<int>(plot.Width()));
    std::vector<int> col_top(static_cast<size_t>(W) + 1, INT_MAX), col_bot(static_cast<size_t>(W) + 1, INT_MIN);
    for (const auto& pt : samples_->points) {
        if (pt.ts < t0 || pt.ts >= t1) continue;
        const int cx = X(pt.ts) - plot.left, cy = Y(pt.viewers);
        if (cx < 0 || cx > W) continue;
        col_top[cx] = std::min(col_top[cx], cy);
        col_bot[cx] = std::max(col_bot[cx], cy);
    }
    auto line_at = [&](int px, int y0, int y1) {   // x 열에서 선이 [y0,y1] 과 만나는가
        const int cx = px - plot.left;
        if (cx < 0 || cx > W || col_top[cx] == INT_MAX) return false;
        return col_top[cx] <= y1 && col_bot[cx] >= y0;
    };
    auto hits_line = [&](const CRect& r) {
        for (int x = r.left - kGap; x <= r.right + kGap; ++x)
            if (line_at(x, r.top - kGap, r.bottom + kGap)) return true;
        return false;
    };
    std::vector<CRect> placed;
    auto hits_label = [&](const CRect& r) {
        for (const auto& q : placed) {
            CRect tmp;
            if (tmp.IntersectRect(CRect(r.left - kGap, r.top - kGap, r.right + kGap, r.bottom + kGap), q)) return true;
        }
        return false;
    };
    // 점 (px,py) 에서 사각형 r 의 가장 가까운 점
    auto nearest_on_rect = [](int px, int py, const CRect& r) {
        return CPoint(std::clamp<int>(px, r.left, r.right), std::clamp<int>(py, r.top, r.bottom));
    };
    // 점 → 대상점 직선이 선을 가로지르는 열 수 (점 자신 근처 4px 는 제외)
    auto leader_cross_count = [&](int px, int py, CPoint q) {
        const int dx = q.x - px, dy = q.y - py;
        const int steps = std::max(std::abs(dx), std::abs(dy));
        int n = 0, last_col = INT_MIN;
        for (int i = 4; i <= steps; ++i) {
            const int x = px + dx * i / std::max(1, steps), y = py + dy * i / std::max(1, steps);
            if (x == last_col) continue;
            last_col = x;
            if (line_at(x, y - 1, y + 1)) ++n;
        }
        return n;
    };

    CPen leader(PS_SOLID, 1, RGB(200, 120, 60));
    CPen* oldPen = dc.SelectObject(&leader);

    for (const auto& inf : samples_->info) {
        if (inf.ts < t0 || inf.ts >= t1) continue;
        const int x = X(inf.ts);
        int y = data.bottom;
        auto it = std::lower_bound(samples_->points.begin(), samples_->points.end(), inf.ts,
                                   [](const sm::Point& p, long long t) { return p.ts < t; });
        if (it != samples_->points.end() && it->ts < t1) y = Y(it->viewers);

        const CString cat(inf.category.c_str()), title(inf.title.c_str());
        const int w = std::min<int>(std::max(dc.GetTextExtent(cat).cx, dc.GetTextExtent(title).cx) + 6, kMaxLabelW);

        // 후보 격자: 점 주위 가로 ±(w+260), 세로는 plot 전체. 점수가 가장 낮은 자리.
        CRect best;
        double best_score = 1e18;
        const int x_lo = std::max<int>(plot.left, x - w - 260), x_hi = std::min<int>(plot.right - w, x + 260);
        for (int ly = plot.top + 1; ly + kLabelH <= plot.bottom - 1; ly += 6) {
            for (int lx = x_lo; lx <= x_hi; lx += 8) {
                const CRect cand(lx, ly, lx + w, ly + kLabelH);
                if (hits_label(cand)) continue;
                const bool in_data = cand.bottom > data.top && cand.top < data.bottom;
                if (in_data && hits_line(cand)) continue;
                const CPoint q = nearest_on_rect(x, y, cand);
                const double dist = std::hypot(static_cast<double>(q.x - x), static_cast<double>(q.y - y));
                double score = dist;
                if (in_data) score += 40;                                   // 전용 띠를 우선
                score += 25.0 * leader_cross_count(x, y, q);                 // 리더가 선을 가로지르면 벌점
                if (dist < 6) score += 20;                                   // 점에 너무 붙는 것도 피함
                if (score < best_score) { best_score = score; best = cand; }
            }
        }
        if (best_score >= 1e17) {   // 정말 자리가 없으면 마커만
            dc.Ellipse(x - 2, y - 2, x + 3, y + 3);
            continue;
        }
        placed.push_back(best);

        // 리더: 점 → 라벨의 가장 가까운 점 (직선)
        const CPoint q = nearest_on_rect(x, y, best);
        dc.MoveTo(x, y); dc.LineTo(q.x, q.y);
        dc.Ellipse(x - 2, y - 2, x + 3, y + 3);

        dc.FillSolidRect(best, RGB(255, 248, 235));
        dc.SetTextColor(RGB(120, 60, 20));
        dc.DrawText(cat, CRect(best.left + 3, best.top + 1, best.right - 3, best.top + 1 + kLineH),
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        dc.SetTextColor(RGB(60, 60, 60));
        dc.DrawText(title, CRect(best.left + 3, best.top + 1 + kLineH, best.right - 3, best.bottom - 1),
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    dc.SelectObject(oldPen);
}

// ── 상위 14 당일 모드 ─────────────────────────────────────────────────────
// 채널당 한 줄. x 축은 오늘 00:00 ~ 24:00 KST 공통, y 축은 줄마다 자기 최고치.
// 왼쪽에 채널명·현재/최고, 선 위에 제목 변경 지점(주황 눈금). 툴팁은 줄 안에서 동작.
void CMainDlg::draw_multi(CDC& dc, const CRect& rc) {
    multi_rows_.clear();
    if (multi_ids_.empty()) {
        dc.SetTextColor(RGB(120, 120, 120));
        CRect r(rc);
        dc.DrawText(L"조회를 누르면 리스트 상위 14개 채널의 오늘 데이터를 나열합니다", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    const int n = static_cast<int>(multi_ids_.size());
    const int nameW = 150, rightW = 60, axisH = 18, top = rc.top + 4;
    const int rowH = std::max(24, static_cast<int>(rc.bottom - axisH - top) / n);
    const CRect axis(rc.left + nameW, top, rc.right - rightW, top + rowH * n);
    auto X = [&](long long ts) { return axis.left + static_cast<int>((ts - day0_) * static_cast<long long>(axis.Width()) / 86400); };

    CPen grid(PS_SOLID, 1, RGB(232, 232, 232));
    CPen sep(PS_SOLID, 1, RGB(210, 210, 210));
    CPen line(PS_SOLID, 2, RGB(46, 139, 87));
    CPen mark(PS_SOLID, 1, RGB(200, 120, 60));
    CPen nowpen(PS_DOT, 1, RGB(150, 150, 150));
    CPen* oldPen = dc.SelectObject(&grid);

    // 시각 격자 + 라벨 (2시간)
    dc.SetTextColor(RGB(90, 90, 90));
    for (int h = 0; h <= 24; h += 2) {
        const int x = X(day0_ + h * 3600LL);
        dc.MoveTo(x, axis.top); dc.LineTo(x, axis.bottom);
        CString s; s.Format(L"%02d", h % 24);
        dc.DrawText(s, CRect(x - 14, axis.bottom + 2, x + 14, axis.bottom + 16), DT_CENTER | DT_SINGLELINE);
    }
    // 현재 시각
    const long long now = static_cast<long long>(std::time(nullptr));
    dc.SelectObject(&nowpen);
    { const int x = X(std::min(now, day0_ + 86400)); dc.MoveTo(x, axis.top); dc.LineTo(x, axis.bottom); }

    for (int i = 0; i < n; ++i) {
        const auto& [id, name] = multi_ids_[i];
        const CRect row(axis.left, top + i * rowH, axis.right, top + (i + 1) * rowH);
        const CRect plot(row.left, row.top + 3, row.right, row.bottom - 3);
        multi_rows_.push_back(plot);
        dc.SelectObject(&sep);
        dc.MoveTo(rc.left, row.bottom); dc.LineTo(rc.right, row.bottom);

        auto it = multi_.find(id);
        const sm::Samples* smp = it == multi_.end() ? nullptr : it->second.get();
        int peak = 0, cur = 0;
        if (smp) for (const auto& p : smp->points) { peak = std::max(peak, p.viewers); cur = p.viewers; }
        const int ymax = std::max(1, peak);
        auto Y = [&](int v) { return plot.bottom - static_cast<int>(static_cast<long long>(v) * plot.Height() / ymax); };

        // 채널명 (왼쪽), 현재/최고 (오른쪽)
        dc.SetTextColor(RGB(30, 30, 30));
        CString label; label.Format(L"%d. %s", i + 1, name.c_str());
        dc.DrawText(label, CRect(rc.left + 4, row.top, row.left - 6, row.bottom), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        dc.SetTextColor(RGB(90, 90, 90));
        CString stat;
        if (smp) stat.Format(L"%d\n최고 %d", cur, peak); else stat = L"...";
        dc.DrawText(stat, CRect(row.right + 4, row.top, rc.right, row.bottom), DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        if (!smp || smp->points.empty()) continue;

        // 제목 변경 눈금 (구간 시작 값 제외: ts < day0_)
        dc.SelectObject(&mark);
        for (const auto& inf : smp->info) {
            if (inf.ts < day0_) continue;
            const int x = X(inf.ts);
            dc.MoveTo(x, plot.top); dc.LineTo(x, plot.bottom);
        }
        // 선
        dc.SelectObject(&line);
        bool pen_down = false; long long prev = 0;
        for (const auto& p : smp->points) {
            const int x = X(p.ts), y = Y(p.viewers);
            if (!pen_down || p.ts - prev > 2 * kPollSec) { dc.MoveTo(x, y); pen_down = true; } else dc.LineTo(x, y);
            prev = p.ts;
        }
    }
    dc.SelectObject(oldPen);

    // 툴팁
    if (hover_multi_ >= 0 && hover_multi_ < n && hover_idx_ >= 0) {
        auto it = multi_.find(multi_ids_[hover_multi_].first);
        if (it != multi_.end() && hover_idx_ < static_cast<int>(it->second->points.size())) {
            const auto& smp = *it->second;
            const auto& p = smp.points[hover_idx_];
            const CRect& plot = multi_rows_[hover_multi_];
            int peak = 0; for (const auto& q : smp.points) peak = std::max(peak, q.viewers);
            const int x = X(p.ts);
            const int y = plot.bottom - static_cast<int>(static_cast<long long>(p.viewers) * plot.Height() / std::max(1, peak));
            CPen cross(PS_SOLID, 1, RGB(80, 80, 80));
            CPen* op = dc.SelectObject(&cross);
            dc.MoveTo(x, axis.top); dc.LineTo(x, axis.bottom);
            CBrush dot(RGB(46, 139, 87)); CBrush* ob = dc.SelectObject(&dot);
            dc.Ellipse(x - 3, y - 3, x + 4, y + 4);
            const sm::Info* curinf = nullptr;
            for (const auto& inf : smp.info) if (inf.ts <= p.ts) curinf = &inf;
            CString l1, l2;
            l1.Format(L"%s  %s   %d명", multi_ids_[hover_multi_].second.c_str(), fmt_kst(p.ts, L"%H:%M").GetString(), p.viewers);
            if (curinf) l2.Format(L"%s | %s", curinf->category.c_str(), curinf->title.c_str());
            const CSize s1 = dc.GetTextExtent(l1), s2 = l2.IsEmpty() ? CSize(0, 0) : dc.GetTextExtent(l2);
            const int w = std::min<int>(std::max(s1.cx, s2.cx) + 12, 360), h = s1.cy + s2.cy + 8;
            int bx = x + 12, by = y - h - 6;
            if (bx + w > rc.right) bx = x - 12 - w;
            if (by < rc.top) by = y + 10;
            if (by + h > rc.bottom) by = rc.bottom - h;
            CRect box(bx, by, bx + w, by + h);
            dc.FillSolidRect(box, RGB(255, 255, 225));
            dc.Draw3dRect(box, RGB(120, 120, 120), RGB(120, 120, 120));
            dc.SetTextColor(RGB(0, 0, 0));
            dc.DrawText(l1, CRect(box.left + 6, box.top + 4, box.right - 6, box.top + 4 + s1.cy), DT_LEFT | DT_SINGLELINE);
            if (!l2.IsEmpty()) {
                dc.SetTextColor(RGB(90, 90, 90));
                dc.DrawText(l2, CRect(box.left + 6, box.top + 4 + s1.cy, box.right - 6, box.bottom - 4), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            dc.SelectObject(ob); dc.SelectObject(op);
        }
    }
}

// ── 툴팁 ──────────────────────────────────────────────────────────────────
BOOL CMainDlg::PreTranslateMessage(MSG* pMsg) {
    if (chart_.GetSafeHwnd() && pMsg->hwnd == chart_.GetSafeHwnd()) {
        if (pMsg->message == WM_MOUSEMOVE) {
            if (!tracking_) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, chart_.GetSafeHwnd(), 0 };
                ::TrackMouseEvent(&tme);
                tracking_ = true;
            }
            update_hover(CPoint(GET_X_LPARAM(pMsg->lParam), GET_Y_LPARAM(pMsg->lParam)));
        } else if (pMsg->message == WM_MOUSELEAVE) {
            tracking_ = false;
            if (hover_idx_ >= 0) { hover_idx_ = -1; chart_.Invalidate(); }
        }
    }
    return CDialogEx::PreTranslateMessage(pMsg);
}

void CMainDlg::update_hover(CPoint pt) {
    int idx = -1, row = 0;
    if (mode() == kModeMulti) {
        int mrow = -1;
        for (int i = 0; i < static_cast<int>(multi_rows_.size()); ++i)
            if (multi_rows_[i].PtInRect(pt)) { mrow = i; break; }
        if (mrow >= 0 && mrow < static_cast<int>(multi_ids_.size())) {
            auto it = multi_.find(multi_ids_[mrow].first);
            if (it != multi_.end() && !it->second->points.empty()) {
                const CRect& plot = multi_rows_[mrow];
                const long long ts = day0_ + static_cast<long long>(pt.x - plot.left) * 86400 / std::max(1, static_cast<int>(plot.Width()));
                const auto& pts = it->second->points;
                auto lb = std::lower_bound(pts.begin(), pts.end(), ts, [](const sm::Point& p, long long t) { return p.ts < t; });
                long long best = LLONG_MAX; int bi = -1;
                for (auto c : {lb, lb == pts.begin() ? pts.end() : std::prev(lb)}) {
                    if (c == pts.end()) continue;
                    const long long d = std::llabs(c->ts - ts);
                    if (d < best) { best = d; bi = static_cast<int>(c - pts.begin()); }
                }
                const long long sec_per_px = 86400 / std::max(1, static_cast<int>(plot.Width()));
                if (bi >= 0 && best <= 12 * sec_per_px) idx = bi;
            }
        }
        if (idx != hover_idx_ || mrow != hover_multi_) { hover_idx_ = idx; hover_multi_ = mrow; chart_.Invalidate(); }
        return;
    }
    if (samples_ && !samples_->points.empty()) {
        for (int r = 0; r < 2; ++r) {
            const CRect& plot = row_rect_[r];
            if (!plot.PtInRect(pt)) continue;
            const long long t0 = week0_ + r * kWeek;
            const long long ts = t0 + static_cast<long long>(pt.x - plot.left) * kWeek / std::max(1, plot.Width());
            // 가장 가까운 샘플 (같은 주 안에서)
            const auto& pts = samples_->points;
            auto it = std::lower_bound(pts.begin(), pts.end(), ts,
                                       [](const sm::Point& p, long long t) { return p.ts < t; });
            long long best = LLONG_MAX; int bi = -1;
            for (auto c : {it, it == pts.begin() ? pts.end() : std::prev(it)}) {
                if (c == pts.end()) continue;
                if (c->ts < t0 || c->ts >= t0 + kWeek) continue;
                const long long d = std::llabs(c->ts - ts);
                if (d < best) { best = d; bi = static_cast<int>(c - pts.begin()); }
            }
            // 픽셀 기준 12px 이내일 때만
            const long long sec_per_px = kWeek / std::max(1, plot.Width());
            if (bi >= 0 && best <= 12 * sec_per_px) { idx = bi; row = r; }
            break;
        }
    }
    if (idx != hover_idx_ || row != hover_row_) {
        hover_idx_ = idx; hover_row_ = row;
        chart_.Invalidate();
    }
}

void CMainDlg::draw_hover(CDC& dc) {
    if (hover_idx_ < 0 || !samples_ || hover_idx_ >= static_cast<int>(samples_->points.size())) return;
    const auto& p = samples_->points[hover_idx_];
    const CRect& plot = row_rect_[hover_row_];
    const long long t0 = week0_ + hover_row_ * kWeek;
    const int x = plot.left + static_cast<int>((p.ts - t0) * static_cast<long long>(plot.Width()) / kWeek);
    const int y = plot.bottom - static_cast<int>(static_cast<long long>(p.viewers) * plot.Height() / ymax_);

    CPen cross(PS_SOLID, 1, RGB(80, 80, 80));
    CPen* oldPen = dc.SelectObject(&cross);
    dc.MoveTo(x, plot.top); dc.LineTo(x, plot.bottom);
    CBrush dot(RGB(46, 139, 87));
    CBrush* oldBrush = dc.SelectObject(&dot);
    dc.Ellipse(x - 4, y - 4, x + 5, y + 5);

    CString line;
    const long long days = (p.ts + kKstOffset) / 86400;
    line.Format(L"%s %s   %d명", kWeekdays[(days + 4) % 7], fmt_kst(p.ts, L"%m/%d %H:%M").GetString(), p.viewers);
    const CSize sz = dc.GetTextExtent(line);
    const int w = sz.cx + 12, h = sz.cy + 8;
    int bx = x + 12, by = y - h - 6;
    if (bx + w > plot.right) bx = x - 12 - w;
    if (by < plot.top) by = y + 10;
    CRect box(bx, by, bx + w, by + h);

    dc.FillSolidRect(box, RGB(255, 255, 225));
    dc.Draw3dRect(box, RGB(120, 120, 120), RGB(120, 120, 120));
    dc.SetTextColor(RGB(0, 0, 0));
    dc.DrawText(line, CRect(box.left + 6, box.top + 4, box.right - 6, box.bottom - 4), DT_LEFT | DT_SINGLELINE);
    dc.SelectObject(oldBrush);
    dc.SelectObject(oldPen);
}
