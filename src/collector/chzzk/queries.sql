-- 비교 쿼리 모음. :channel, :now (epoch sec), :period (폴링 주기 초) 파라미터 사용.
-- KST 변환: datetime(ts, 'unixepoch', '+9 hours')

-- 1) 지금 시각 vs 정확히 1주 전 같은 시각 (샘플 1개 매칭)
SELECT
    datetime(a.ts, 'unixepoch', '+9 hours')      AS now_kst,
    a.viewers                                    AS now_viewers,
    b.viewers                                    AS lastweek_viewers,
    a.viewers - b.viewers                        AS diff,
    ROUND(100.0 * (a.viewers - b.viewers) / NULLIF(b.viewers, 0), 1) AS diff_pct
FROM viewer_samples a
LEFT JOIN viewer_samples b
       ON b.channel_id = a.channel_id AND b.ts = a.ts - 7*86400
WHERE a.channel_id = :channel
  AND a.ts = (SELECT MAX(ts) FROM viewer_samples WHERE channel_id = :channel);

-- 2) 오늘 하루 곡선 vs 지난주 같은 요일 곡선 (시각별 나란히)
--    :day_start = 오늘 00:00 KST 의 epoch
SELECT
    time(a.ts, 'unixepoch', '+9 hours')          AS hhmm,
    a.viewers                                    AS today,
    b.viewers                                    AS last_week
FROM viewer_samples a
LEFT JOIN viewer_samples b
       ON b.channel_id = a.channel_id AND b.ts = a.ts - 7*86400
WHERE a.channel_id = :channel
  AND a.ts >= :day_start AND a.ts < :day_start + 86400
ORDER BY a.ts;

-- 3) 이번 주 vs 지난 주 (7일 합산) 비교
WITH w AS (
    SELECT
        CASE WHEN ts >= :now - 7*86400 THEN 'this' ELSE 'last' END AS wk,
        viewers
    FROM viewer_samples
    WHERE channel_id = :channel AND ts >= :now - 14*86400
)
SELECT wk, COUNT(*) AS samples, MAX(viewers) AS peak, ROUND(AVG(viewers)) AS avg
FROM w GROUP BY wk;

-- 4) 특정 시각의 순위표 (그 분에 저장된 상위 N개) + 그 시각의 제목/카테고리
SELECT c.channel_name, s.viewers, i.category, i.title
FROM viewer_samples s
LEFT JOIN channels c USING (channel_id)
LEFT JOIN stream_info i
       ON i.channel_id = s.channel_id
      AND i.ts = (SELECT MAX(ts) FROM stream_info
                   WHERE channel_id = s.channel_id AND ts <= s.ts)
WHERE s.ts = :ts
ORDER BY s.viewers DESC;

-- 5) 채널의 제목/카테고리 변경 이력
SELECT datetime(ts, 'unixepoch', '+9 hours') AS changed_kst, category, title
FROM stream_info
WHERE channel_id = :channel
ORDER BY ts DESC
LIMIT 20;
