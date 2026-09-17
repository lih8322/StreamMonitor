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

## 이 프로그램은 무엇을 하나

"어떤 스트리머의 시청자 수가 지난주 이 시간과 비교해 어떤가"를 보기 위한 도구입니다.
치지직 API 는 **현재** 시청자 수만 알려 주고 과거를 주지 않으므로, 서버가 매분 값을 받아
쌓아 두고, 클라이언트는 그 기록을 요일·시각을 맞춰 나란히 그립니다. 주식의 "작년 동월 비교"와
같은 발상입니다.

프로세스는 세 개이고 역할이 겹치지 않습니다.

| 프로세스 | 어디서 | 역할 | DB 접근 |
|---|---|---|---|
| **collector** (`monitor.py`) | 서버, systemd `streammonitor` | 매분 치지직 API 를 호출해 상위 50개 방송의 시청자 수를 SQLite 에 기록 | **쓰기 (유일)** |
| **push** (`push.py`) | 서버, systemd `streammonitor-push` | 클라이언트 요청을 받아 DB 를 읽어 JSON 으로 응답. `127.0.0.1:9002` 에만 바인딩 | 읽기 전용 |
| **client** (MFC) | 윈도우 PC | SSH 터널을 띄워 push 에 접속, 채널 목록과 2주 시계열을 받아 차트로 표시 | 없음 |

수집과 조회를 프로세스로 나눈 이유: 클라이언트가 몇 개 붙든, 조회가 얼마나 무겁든 매분 수집이
밀리지 않습니다. push 가 죽어도 데이터는 계속 쌓이고, 반대로 collector 를 재배포해도 클라이언트
연결은 끊기지 않습니다.

## 서버는 어떻게 DB 를 수집하나

collector 의 1분 주기 루프입니다.

```
매분 00초 (ts = now // 60 * 60)
  1. GET https://openapi.chzzk.naver.com/open/v1/lives?size=20        헤더: Client-Id, Client-Secret
     → page.next 커서로 3페이지 이어 받아 상위 50개 (시청자 순)
  2. viewer_samples  ← 50행 INSERT (channel_id, ts, viewers)
  3. channels        ← 50행 UPSERT (channel_name, last_seen_ts 갱신)
  4. stream_info     ← 채널별 직전 행과 (category, title) 이 다를 때만 INSERT
  5. 1 트랜잭션 commit
매시간
  6. ts < now - 30일 인 viewer_samples / stream_info 삭제
```

- **인증**: 라이브 목록은 공개 데이터라 유저 로그인(OAuth) 없이 앱의 Client ID/Secret 헤더만으로
  조회됩니다. 토큰 갱신·만료가 없어 데몬이 무인으로 돌 수 있습니다. (`chzzk/auth.py` 의 OAuth
  코드는 유저 권한 API 가 필요할 때를 위한 것이고 현재 데몬은 쓰지 않습니다.)
- **`ts` 정렬**: 폴링이 몇 초 늦어도 `ts` 는 60초 경계로 내림하므로, 1주 전 같은 시각은 정확히
  `ts - 604800` 행입니다. 비교 쿼리가 JOIN 하나로 끝나는 이유입니다.
- **제목/카테고리는 변경 시에만**: 매분 저장하면 행의 절반 이상이 문자열이라, 바뀔 때 1행만
  남깁니다. 시각 T 의 제목은 "T 이전 마지막 `stream_info` 행"입니다.
- **오류 처리**: API 실패는 로그만 남기고 다음 분에 재시도. 프로세스가 죽으면 systemd 가
  10초 후 재시작. 상위 50 밖으로 나간 채널은 그 분에 행이 없을 뿐, 별도 표시를 남기지 않습니다
  ("목록에 없음 ≠ 방송 종료").

## 클라이언트는 무엇으로 접속하고 무엇을 받나

### 접속

push 는 서버의 루프백에만 열려 있으므로 인터넷에서 직접 닿을 수 없습니다. 클라이언트가 시작할 때
OpenSSH 를 자식 프로세스로 띄웁니다.

```
ssh -N -o BatchMode=yes -o ExitOnForwardFailure=yes -L 9002:127.0.0.1:9002 oracle
```

- 로컬 `127.0.0.1:9002` 로 보낸 것이 서버의 `127.0.0.1:9002` 로 전달됩니다. 인증은 `~/.ssh/config`
  의 `oracle` 항목에 걸린 키가 담당하고, 서버에 포트를 열 필요가 없습니다.
- ssh 프로세스는 Job Object 에 묶여 클라이언트가 어떻게 종료되든 함께 사라집니다.
- 끊기면 5초 후 재접속하고, 붙을 때마다 목록을 새로 받습니다.

### 주고받는 것

TCP 위에 **한 줄 = JSON 하나**. 요청에 `id` 를 붙이면 응답에 같은 `id` 가 돌아옵니다.
([PROTOCOL.md](PROTOCOL.md))

| 언제 | 클라이언트 → 서버 | 서버 → 클라이언트 |
|---|---|---|
| 접속 직후 | `{"type":"hello","v":1}` | `{"type":"hello","now":…,"retain_days":30}` |
| 접속 직후, 이후 매분 | `{"type":"channels"}` | 채널 배열: `channel_id, channel_name, last_seen, peak(7일 최고), samples` — `peak` 내림차순 |
| 조회 버튼, 이후 매분 | `{"type":"samples","channel_id":…,"from":지난주 일요일 00:00,"to":now}` | `points: [[ts, viewers], …]` (2주치, 오름차순) + `info: [[ts, category, title], …]` (구간 안 제목 변경 이력) |
| 30초마다 | — | `{"type":"ping"}` (응답 불필요) |

클라이언트는 `points` 를 받아 지난주 줄과 이번주 줄에 나눠 그리고, 이번주 줄에는 지난주를
`ts + 7일` 로 옮겨 회색으로 겹칩니다. `info` 는 변경 시각에 라벨로, `last_seen` 은 목록의
●/○ 아이콘으로 표시합니다. 클라이언트는 원본 DB 나 API 키를 전혀 알지 못합니다.

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
