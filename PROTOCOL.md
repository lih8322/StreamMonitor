# StreamMonitor 프로토콜 v1

push(릴레이) ↔ 클라이언트(StreamMonitor MFC). TCP `127.0.0.1:9002`(SSH 터널만), **줄 단위 JSON**(UTF-8, 개행 구분, 줄 64 KB 상한).
규칙: 서버는 필드를 **추가만** 한다. 제거·의미 변경 시 `v` 를 올린다. 모르는 필드는 무시한다.
market_flow 의 PROTOCOL.md 와 같은 형식 — `type` 으로 종류, `id` 로 요청/응답 짝맞춤.

## 클라이언트 → 서버

| 메시지 | 예시 | 비고 |
|---|---|---|
| hello | `{"type":"hello","v":1,"id":"h1"}` | 첫 메시지 |
| channels | `{"type":"channels","id":"c1"}` | 조회 가능한 채널 목록 (viewer_samples 에 있는 채널) |
| samples | `{"type":"samples","id":"q1","channel_id":"75cb…","from":1789000000,"to":1789600000}` | 시청자 시계열. `from`/`to` 생략 시 최근 24h. 최대 30일 |
| stats | `{"type":"stats","id":"st"}` | 진단 |

## 서버 → 클라이언트

| 메시지 | 예시 | 비고 |
|---|---|---|
| hello | `{"type":"hello","v":1,"id":"h1","now":1789673400,"retain_days":30}` | |
| channels | `{"type":"channels","id":"c1","channels":[{"channel_id":"75cb…","channel_name":"한동숙","first_seen":…,"last_seen":…,"samples":1440,"peak":7595}]}` | `samples`/`peak` = 최근 7일 샘플 수 / 최고 시청자수. `peak` 내림차순. 7일간 샘플 없는 채널은 제외 |
| samples | `{"type":"samples","id":"q1","channel_id":"75cb…","from":…,"to":…,"points":[[1789673400,7595],…],"info":[[1789673400,"메이플스토리","메"],…]}` | `points` = [ts, viewers] 오름차순. `info` = 구간 안 제목/카테고리 변경 이력 (+ 구간 시작 시점의 값 1개) |
| ping | `{"type":"ping"}` | 30 s. 응답 불필요 |
| bye | `{"type":"bye"}` | 서버 종료 직전 |
| error | `{"type":"error","id":"q1","code":"bad_arg","message":"…"}` | `code`: `bad_json` `unknown_type` `bad_arg` |

## 서버 정책

- hello 없이 60 s → 끊음. 미완성 줄 64 KB 초과 → 끊음.
- DB 는 읽기 전용(`mode=ro`)으로 연다. 쓰기 주체는 monitor.py 하나.
