# StreamMonitor

치지직(CHZZK) 라이브 상위 채널의 시청자 수를 매분 기록하고, 지난주 같은 요일·같은 시각과
비교해 보는 개인용 도구입니다. 리눅스(Oracle Cloud)에서 24시간 수집하고, 윈도우 MFC 앱이
SSH 터널로 붙어 차트를 그립니다.

```
┌─ Linux (Oracle Cloud) ───────────────────┐        ┌─ Windows ─────────────────────┐
│  collector  ── CHZZK Open API 매분 상위 50 │        │  StreamMonitor (MFC)          │
│      │ SQLite (WAL)                      │  SSH   │   ├ 채널 목록 (7일 최고치 순)  │
│      ▼                                   │◀─터널─▶│   ├ 2주 차트 (일~토 × 2줄)     │
│  push  ── 127.0.0.1:9002 줄 단위 JSON      │  9002  │   └ 지난주 회색 오버레이·툴팁  │
└──────────────────────────────────────────┘        └───────────────────────────────┘
```

## 구성

| 경로 | 설명 |
|---|---|
| `src/collector/` | 수집 데몬 `monitor.py`. 매분 `/open/v1/lives` 상위 N개를 SQLite 에 기록. `chzzk/` 에 인증·스키마 |
| `src/push/` | 중계 데몬 `push.py`. DB 를 읽기 전용으로 열어 클라이언트에 JSON 으로 준다. 프로토콜은 [PROTOCOL.md](PROTOCOL.md) |
| `src/mfc/` | 윈도우 클라이언트. ssh 터널을 직접 띄워 push 에 접속 |
| `deploy.sh` | 서버 `/opt/streammonitor` 로 복사 + 데몬 재시작 |

### DB (SQLite, 3 테이블)

| 테이블 | 저장 주기 | 컬럼 |
|---|---|---|
| `viewer_samples` | 매분 상위 N행 | `channel_id, ts, viewers` — `ts` 는 60초 단위 epoch |
| `stream_info` | 제목/카테고리가 바뀔 때만 | `channel_id, ts, category, title` |
| `channels` | 채널당 1행 | `channel_id, channel_name, first_seen_ts, last_seen_ts` |

`ts` 를 60초 배수로 저장하므로 1주 전 같은 시각은 `ts - 604800` 로 JOIN 한 번에 나옵니다 (`chzzk/queries.sql`).
50행/분 기준 30일 ≈ 220 MB. 30일 지난 행은 데몬이 매시간 삭제합니다.

## 요구 환경

### Linux (collector, push)

- Ubuntu 20.04+, Python 3.8+ (`python3-venv`)
- CHZZK 개발자센터에서 발급한 Client ID / Secret — 라이브 목록은 유저 로그인 없이 Client 인증만으로 조회됩니다

```bash
sudo mkdir -p /opt/streammonitor && sudo chown $USER /opt/streammonitor
cp -r src/collector/* src/push/* /opt/streammonitor/
cd /opt/streammonitor
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
cp .env.example .env            # CHZZK_CLIENT_ID / SECRET 입력
.venv/bin/python monitor.py --once      # 배관 확인
sudo cp streammonitor.service streammonitor-push.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now streammonitor streammonitor-push
```

`.env` 항목: `CHZZK_CLIENT_ID` `CHZZK_CLIENT_SECRET` `SM_DB`(monitor.db) `SM_TOP_N`(50) `SM_RETAIN_DAYS`(30) `SM_POLL_SEC`(60) `SM_PUSH_PORT`(9002)

### Windows (MFC)

- Visual Studio 2022 (MFC 포함 C++ 데스크톱 워크로드), x64
- vcpkg 의 `rapidjson` (`vcpkg install rapidjson --triplet x64-windows`). 기본 경로 `D:\vcpkg`, 다른 곳이면 `VcpkgRoot` 환경변수
- OpenSSH 클라이언트와 `~/.ssh/config` 의 `oracle` 호스트 항목 (키 인증). 앱이 `ssh -N -L 9002:127.0.0.1:9002 oracle` 을 직접 실행합니다

```
msbuild src\mfc\StreamMonitor.sln /p:Configuration=Release /p:Platform=x64
```

## 클라이언트 동작

- 시작 시 터널 → `hello` → 채널 목록. 1분마다 목록과 (조회 중이면) 차트를 다시 받습니다
- 목록: 초록 `●` = 최근 2분 내 상위권에 관측됨, 회색 `○` = 상위권 밖. 정렬은 최근 7일 최고 시청자수
- 차트: 위 줄 = 지난주, 아래 줄 = 이번주 (일요일 00:00 ~ 토요일 24:00 KST 고정 폭). 아래 줄엔 지난주가 회색으로 겹쳐집니다
- 제목/카테고리 변경 시각에 리더 라인으로 라벨, 마우스 위치의 시각·시청자수 툴팁
- 창 위치/크기는 `HKCU\Software\StreamMonitor` 에 저장

## 로컬 테스트

푸시 데몬은 Windows 에서도 돕니다. 앱은 항상 `127.0.0.1:9002` 로 붙으므로 로컬에 띄워 두면 그쪽에 연결됩니다.

```
set SM_DB=path\to\test.db
python src\push\push.py
```

## 주의

- 상위 N 목록만 기록하므로 "목록에 없음"은 방송 종료가 아니라 "상위 N 밖"입니다
- push 는 `127.0.0.1` 에만 바인딩합니다. 포트를 열지 말고 SSH 터널로만 접속하세요
