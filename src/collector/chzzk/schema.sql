-- StreamMonitor SQLite schema
-- 매분 /open/v1/lives 상위 N개를 저장한다. 숫자(viewers)는 매 폴링, 제목/카테고리는 바뀔 때만.
PRAGMA journal_mode = WAL;

-- 폴링(1분)마다 상위 N개 스트림 = N행. (channel_id, ts) 로 유일.
CREATE TABLE IF NOT EXISTS viewer_samples (
    channel_id TEXT    NOT NULL,             -- CHZZK channelId
    ts         INTEGER NOT NULL,             -- epoch sec, 폴링 주기(60초) 단위로 내림 정렬
    viewers    INTEGER NOT NULL,             -- concurrentUserCount
    PRIMARY KEY (channel_id, ts)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_samples_ts ON viewer_samples(ts);

-- 제목/카테고리 변경 이력. 직전 값과 다를 때만 1행. ts = 이 값이 적용되기 시작한 시각.
-- 시각 T 의 제목 = channel_id 같고 ts <= T 인 행 중 ts 최대인 것.
CREATE TABLE IF NOT EXISTS stream_info (
    channel_id TEXT    NOT NULL,
    ts         INTEGER NOT NULL,
    category   TEXT,                         -- liveCategoryValue
    title      TEXT,                         -- liveTitle
    PRIMARY KEY (channel_id, ts)
) WITHOUT ROWID;

-- 목록에서 본 채널의 최신 이름 (조회 시 이름 표시용). 데몬이 UPSERT.
CREATE TABLE IF NOT EXISTS channels (
    channel_id    TEXT PRIMARY KEY,
    channel_name  TEXT NOT NULL,
    first_seen_ts INTEGER NOT NULL,
    last_seen_ts  INTEGER NOT NULL
);
