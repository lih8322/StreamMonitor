"""
StreamMonitor 데몬: 매분(SM_POLL_SEC) CHZZK 라이브 상위 N개를 SQLite 에 기록한다.

  python monitor.py            # 무한 루프 (systemd 용)
  python monitor.py --once     # 1회 실행 후 종료 (테스트용)

환경변수(.env): CHZZK_CLIENT_ID, CHZZK_CLIENT_SECRET,
               SM_DB (기본 monitor.db), SM_TOP_N (기본 50), SM_RETAIN_DAYS (기본 30), SM_POLL_SEC (기본 60)
"""
from __future__ import annotations

import logging
import os
import signal
import sqlite3
import sys
import time
from pathlib import Path

import requests

from chzzk.auth import API_HOST, ChzzkAuth
from chzzk.env import load_env

log = logging.getLogger("monitor")
PAGE_SIZE = 20
POLL_INTERVAL = int(os.environ.get("SM_POLL_SEC", "60"))  # 폴링 주기(초). ts 는 이 값의 배수로 정렬


def open_db(path: Path) -> sqlite3.Connection:
    db = sqlite3.connect(path, timeout=30)
    db.executescript((Path(__file__).parent / "chzzk" / "schema.sql").read_text(encoding="utf-8"))
    return db


def fetch_top_lives(auth: ChzzkAuth, top_n: int) -> list[dict]:
    lives: list[dict] = []
    next_cursor = None
    while len(lives) < top_n:
        params = {"size": min(PAGE_SIZE, top_n - len(lives))}
        if next_cursor:
            params["next"] = next_cursor
        r = requests.get(f"{API_HOST}/open/v1/lives", headers=auth.client_headers(),
                         params=params, timeout=10)
        body = r.json()
        if r.status_code != 200 or body.get("code") != 200:
            raise RuntimeError(f"lives API {r.status_code}: {body}")
        content = body["content"]
        data = content.get("data", [])
        lives.extend(data)
        next_cursor = (content.get("page") or {}).get("next")
        if not data or not next_cursor:
            break
    return lives[:top_n]


def record(db: sqlite3.Connection, ts: int, lives: list[dict]) -> int:
    """한 번의 폴링 결과를 3개 테이블에 기록. 삽입한 sample 행 수 반환."""
    samples = [(lv["channelId"], ts, lv["concurrentUserCount"]) for lv in lives]
    db.executemany(
        "INSERT OR IGNORE INTO viewer_samples (channel_id, ts, viewers) VALUES (?,?,?)", samples)

    db.executemany("""
        INSERT INTO channels (channel_id, channel_name, first_seen_ts, last_seen_ts)
        VALUES (?,?,?,?)
        ON CONFLICT(channel_id) DO UPDATE SET
            channel_name = excluded.channel_name,
            last_seen_ts = excluded.last_seen_ts""",
        [(lv["channelId"], lv["channelName"], ts, ts) for lv in lives])

    # 제목/카테고리: 직전 값과 다를 때만
    changed = 0
    for lv in lives:
        cat, title = lv.get("liveCategoryValue"), lv.get("liveTitle")
        last = db.execute(
            "SELECT category, title FROM stream_info WHERE channel_id=? ORDER BY ts DESC LIMIT 1",
            (lv["channelId"],)).fetchone()
        if last is None or last != (cat, title):
            db.execute("INSERT OR IGNORE INTO stream_info (channel_id, ts, category, title) VALUES (?,?,?,?)",
                       (lv["channelId"], ts, cat, title))
            changed += 1
    db.commit()
    if changed:
        log.info("stream_info changed: %d", changed)
    return len(samples)


def purge_old(db: sqlite3.Connection, now: int, retain_days: int) -> None:
    cutoff = now - retain_days * 86400
    n = db.execute("DELETE FROM viewer_samples WHERE ts < ?", (cutoff,)).rowcount
    # stream_info 는 각 채널의 최신 1행은 남기고 cutoff 이전 것 삭제
    m = db.execute("""
        DELETE FROM stream_info WHERE ts < ? AND ts < (
            SELECT MAX(ts) FROM stream_info i WHERE i.channel_id = stream_info.channel_id)""",
        (cutoff,)).rowcount
    db.commit()
    if n or m:
        log.info("purged samples=%d stream_info=%d", n, m)


def run_once(auth: ChzzkAuth, db: sqlite3.Connection, top_n: int) -> None:
    ts = int(time.time()) // POLL_INTERVAL * POLL_INTERVAL
    lives = fetch_top_lives(auth, top_n)
    n = record(db, ts, lives)
    top = lives[0] if lives else None
    log.info("ts=%d recorded=%d top=%s(%s)", ts, n,
             top and top["channelName"], top and top["concurrentUserCount"])


def main() -> int:
    load_env()
    global POLL_INTERVAL
    POLL_INTERVAL = int(os.environ.get("SM_POLL_SEC", "60"))
    logging.basicConfig(level=logging.INFO, stream=sys.stdout,
                        format="%(asctime)s %(levelname)s %(message)s")
    auth = ChzzkAuth(os.environ["CHZZK_CLIENT_ID"], os.environ["CHZZK_CLIENT_SECRET"], "")
    db = open_db(Path(os.environ.get("SM_DB", "monitor.db")))
    top_n = int(os.environ.get("SM_TOP_N", "50"))
    retain_days = int(os.environ.get("SM_RETAIN_DAYS", "30"))

    if "--once" in sys.argv:
        run_once(auth, db, top_n)
        return 0

    stop = False

    def on_signal(signum, _frame):
        nonlocal stop
        log.info("signal %d received, stopping", signum)
        stop = True

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    log.info("started: poll=%ds top_n=%d retain_days=%d", POLL_INTERVAL, top_n, retain_days)
    last_purge = 0
    while not stop:
        try:
            run_once(auth, db, top_n)
            if time.time() - last_purge > 3600:
                purge_old(db, int(time.time()), retain_days)
                last_purge = time.time()
        except Exception:
            log.exception("poll failed")
        # 다음 폴링 경계까지 대기 (1초 단위로 stop 확인)
        wake = (int(time.time()) // POLL_INTERVAL + 1) * POLL_INTERVAL
        while not stop and time.time() < wake:
            time.sleep(1)
    db.close()
    log.info("stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
