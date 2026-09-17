# StreamMonitor

치지직(CHZZK) 라이브 상위 채널의 시청자 수를 매분 기록하고, 지난주 같은 요일·같은 시각과
비교해 보는 개인용 도구입니다. 리눅스(Oracle Cloud)에서 24시간 수집하고, 윈도우 MFC 앱이
SSH 터널로 붙어 차트를 그립니다.

```
┌─ Linux (Oracle Cloud) ───────────────────┐        ┌─ Windows ─────────────────────┐
│  collector  ── CHZZK Open API 매분 상위 50 │        │                               │
│      │ SQLite (WAL)                      │  SSH   │                               │
│      ▼                                   │◀─터널─▶│  StreamMonitor (MFC)          │
│  push  ── 127.0.0.1:9002 줄 단위 JSON      │  9002  │                               │
└──────────────────────────────────────────┘        └───────────────────────────────┘
```

프로세스는 세 개이고 역할이 겹치지 않습니다.

| 프로세스 | 어디서 | 역할 | DB 접근 |
|---|---|---|---|
| **collector** (`monitor.py`) | 서버, systemd `streammonitor` | 매분 치지직 API 를 호출해 상위 50개 방송의 시청자 수를 SQLite 에 기록 | **쓰기 (유일)** |
| **push** (`push.py`) | 서버, systemd `streammonitor-push` | 클라이언트 요청을 받아 DB 를 읽어 JSON 으로 응답. `127.0.0.1:9002` 에만 바인딩 | 읽기 전용 |
| **client** (MFC) | 윈도우 PC | SSH 터널을 띄워 push 에 접속, 채널 목록과 2주 시계열을 받아 차트로 표시 | 없음 |
