#include "pch.h"
#include "MainDlg.h"
#include <algorithm>
#include <climits>
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
    chart_title_.MoveWindow(chartL, kMargin, cx - chartL - kMargin, 18);
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
        // 조회 중인 채널이 있으면 차트도 같은 구간으로 다시 받는다 (주가 바뀌면 자동으로 새 주 기준)
        if (!samples_id_.empty()) {
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

void CMainDlg::OnBnClickedQuery() {
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
    row_rect_[0] = CRect(left, top, right, top + rowH);
    row_rect_[1] = CRect(left, top + rowH + gap, right, bottom);
    draw_week(dc, row_rect_[0], week0_, ymax, step, 0);
    draw_week(dc, row_rect_[1], week0_ + kWeek, ymax, step, 1);
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
void CMainDlg::draw_week(CDC& dc, const CRect& plot, long long week_start, int ymax, int ystep, int row) {
    const long long t0 = week_start, t1 = week_start + kWeek;
    auto X = [&](long long ts) { return plot.left + static_cast<int>((ts - t0) * static_cast<long long>(plot.Width()) / kWeek); };
    auto Y = [&](int v) { return plot.bottom - static_cast<int>(static_cast<long long>(v) * plot.Height() / ymax); };

    CPen grid(PS_SOLID, 1, RGB(232, 232, 232));
    CPen dayline(PS_SOLID, 1, RGB(150, 150, 150));
    CPen axis(PS_SOLID, 1, RGB(120, 120, 120));
    CPen* oldPen = dc.SelectObject(&grid);
    dc.SetTextColor(RGB(90, 90, 90));

    // y 눈금
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

    draw_annotations(dc, plot, week_start, ymax);
}

// 제목/카테고리 변경: 변경 시각의 데이터 점에서 직선을 끌어 위쪽 빈 자리에 내용을 적는다.
// 라벨은 3개 레인(row 상단)에 왼쪽부터 채우고, 겹치면 다음 레인 / 오른쪽으로 민다.
void CMainDlg::draw_annotations(CDC& dc, const CRect& plot, long long week_start, int ymax) {
    const long long t0 = week_start, t1 = week_start + kWeek;
    auto X = [&](long long ts) { return plot.left + static_cast<int>((ts - t0) * static_cast<long long>(plot.Width()) / kWeek); };
    auto Y = [&](int v) { return plot.bottom - static_cast<int>(static_cast<long long>(v) * plot.Height() / ymax); };

    constexpr int kLineH = 14, kLabelH = 2 * kLineH + 2, kMaxLabelW = 220, kGap = 3;   // 두 줄: 카테고리 / 타이틀

    // 데이터 선을 피하기 위해 x 열마다 선이 차지하는 y 구간 [col_top, col_bot] 을 구해 둔다.
    const int W = std::max(1, plot.Width());
    std::vector<int> col_top(static_cast<size_t>(W) + 1, INT_MAX), col_bot(static_cast<size_t>(W) + 1, INT_MIN);
    for (const auto& pt : samples_->points) {
        if (pt.ts < t0 || pt.ts >= t1) continue;
        const int cx = X(pt.ts) - plot.left, cy = Y(pt.viewers);
        if (cx < 0 || cx > W) continue;
        col_top[cx] = std::min(col_top[cx], cy);
        col_bot[cx] = std::max(col_bot[cx], cy);
    }
    // 라벨 사각형이 선과 겹치는지: 그 x 구간의 어느 열에서든 선의 y 구간과 라벨의 y 구간이 만나면 겹침
    auto hits_line = [&](const CRect& r) {
        const int x0 = std::max(0, static_cast<int>(r.left - plot.left)), x1 = std::min(W, static_cast<int>(r.right - plot.left));
        for (int x = x0; x <= x1; ++x)
            if (col_top[x] != INT_MAX && col_top[x] <= r.bottom + kGap && col_bot[x] >= r.top - kGap) return true;
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
    auto inside = [&](const CRect& r) { return r.left >= plot.left && r.right <= plot.right && r.top >= plot.top && r.bottom <= plot.bottom; };

    CPen leader(PS_SOLID, 1, RGB(200, 120, 60));
    CPen* oldPen = dc.SelectObject(&leader);
    dc.SetTextColor(RGB(150, 70, 20));

    // 구간 시작 이전 값(ts < from)은 범위 밖이라 자연히 빠진다.
    for (const auto& inf : samples_->info) {
        if (inf.ts < t0 || inf.ts >= t1) continue;
        const int x = X(inf.ts);

        // 변경 시각의 시청자수 (그 시각 이후 첫 샘플)
        int y = plot.bottom;
        auto it = std::lower_bound(samples_->points.begin(), samples_->points.end(), inf.ts,
                                   [](const sm::Point& p, long long t) { return p.ts < t; });
        if (it != samples_->points.end() && it->ts < t1) y = Y(it->viewers);

        const CString cat(inf.category.c_str()), title(inf.title.c_str());
        const int w = std::min<int>(std::max(dc.GetTextExtent(cat).cx, dc.GetTextExtent(title).cx) + 6, kMaxLabelW);

        // 후보 위치: 점에서 위·아래로 한 단계(kLabelH+kGap)씩 번갈아 멀어지며, 각 단계에서
        // 오른쪽 → 왼쪽 → 더 오른쪽 → 더 왼쪽. 기존 라벨·데이터 선·플롯 경계와 겹치지 않는 첫 자리.
        CRect box;
        bool found = false;
        for (int step = 1; step <= 40 && !found; ++step) {
            for (int dir : {-1, +1}) {
                const int ly = y + dir * step * (kLabelH + kGap) - (dir < 0 ? kLabelH : 0);
                const int dxs[] = {6, -w - 6, 40, -w - 40, 90, -w - 90};
                for (int dx : dxs) {
                    const CRect cand(x + dx, ly, x + dx + w, ly + kLabelH);
                    if (!inside(cand) || hits_label(cand) || hits_line(cand)) continue;
                    box = cand; found = true; break;
                }
                if (found) break;
            }
        }
        if (!found) {
            // 점 주변에 자리가 없으면 x 에서 가까운 순으로 플롯 전체를 훑는다 (선은 무시, 라벨끼리만 회피)
            for (int ly = plot.top + 2; ly + kLabelH <= plot.bottom && !found; ly += kLabelH + kGap)
                for (int off = 0; off <= W && !found; off += 8)
                    for (int sgn : {+1, -1}) {
                        const int lx = x + sgn * off - (sgn < 0 ? w : 0);
                        const CRect cand(lx, ly, lx + w, ly + kLabelH);
                        if (!inside(cand) || hits_label(cand)) continue;
                        box = cand; found = true; break;
                    }
        }
        if (!found) {   // 정말 자리가 없으면 마커만
            dc.Ellipse(x - 2, y - 2, x + 3, y + 3);
            continue;
        }
        placed.push_back(box);

        // 리더 라인: 점 → 라벨의 가까운 아래 모서리 (ㄴ/ㄱ 자)
        const int ax = static_cast<int>(std::abs(x - box.left) <= std::abs(x - box.right) ? box.left : box.right);
        dc.MoveTo(x, y);
        dc.LineTo(x, box.bottom + 1);
        if (ax != x) dc.LineTo(ax, box.bottom + 1);
        dc.Ellipse(x - 2, y - 2, x + 3, y + 3);

        dc.FillSolidRect(box, RGB(255, 248, 235));
        dc.SetTextColor(RGB(120, 60, 20));
        dc.DrawText(cat, CRect(box.left + 3, box.top + 1, box.right - 3, box.top + 1 + kLineH),
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        dc.SetTextColor(RGB(60, 60, 60));
        dc.DrawText(title, CRect(box.left + 3, box.top + 1 + kLineH, box.right - 3, box.bottom - 1),
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    dc.SelectObject(oldPen);
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
