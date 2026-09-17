"""
StreamMonitor push(릴레이) 데몬: 127.0.0.1:9002 에서 줄 단위 JSON 으로 monitor.db 를 읽어 준다.
SSH 터널로만 접속한다. PROTOCOL.md 참조.

  python push.py               # 데몬
환경변수(.env): SM_DB (기본 monitor.db), SM_PUSH_PORT (기본 9002)
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import signal
import sqlite3
import sys
import time
from pathlib import Path


log = logging.getLogger("push")


def load_env(path: str = ".env") -> None:
    """collector 와 같은 .env 를 읽는다 (collector 패키지에 의존하지 않도록 따로 둔다)."""
    p = Path(path)
    if not p.exists():
        return
    for line in p.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            os.environ.setdefault(k.strip(), v.strip())


def load_env(path: str = ".env") -> None:
    """collector 와 같은 .env 를 읽는다 (collector 패키지에 의존하지 않도록 따로 둔다)."""
    p = Path(path)
    if not p.exists():
        return
    for line in p.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            os.environ.setdefault(k.strip(), v.strip())
PROTO_V = 1
MAX_LINE = 64 * 1024
HELLO_TIMEOUT = 60
PING_INTERVAL = 30
MAX_RANGE = 30 * 86400


class Db:
    """읽기 전용 SQLite. 모든 접근은 executor 스레드에서."""

    def __init__(self, path: Path):
        self.conn = sqlite3.connect(f"file:{path}?mode=ro", uri=True, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row

    def channels(self) -> list[dict]:
        # 최근 1주일 최고 시청자수 내림차순. 1주일간 샘플이 없는 채널은 제외.
        since = int(time.time()) - 7 * 86400
        rows = self.conn.execute("""
            SELECT c.channel_id, c.channel_name, c.first_seen_ts, c.last_seen_ts,
                   COUNT(s.ts) AS samples, MAX(s.viewers) AS peak
            FROM channels c JOIN viewer_samples s ON s.channel_id = c.channel_id AND s.ts >= ?
            GROUP BY c.channel_id
            ORDER BY peak DESC, c.last_seen_ts DESC""", (since,)).fetchall()
        return [{"channel_id": r["channel_id"], "channel_name": r["channel_name"],
                 "first_seen": r["first_seen_ts"], "last_seen": r["last_seen_ts"],
                 "samples": r["samples"], "peak": r["peak"]} for r in rows]

    def samples(self, channel_id: str, t_from: int, t_to: int) -> dict:
        pts = self.conn.execute(
            "SELECT ts, viewers FROM viewer_samples WHERE channel_id=? AND ts>=? AND ts<=? ORDER BY ts",
            (channel_id, t_from, t_to)).fetchall()
        # 구간 시작 시점에 적용 중이던 제목 1개 + 구간 안 변경 이력
        info = self.conn.execute("""
            SELECT ts, category, title FROM stream_info
            WHERE channel_id=? AND ts = (SELECT MAX(ts) FROM stream_info WHERE channel_id=? AND ts<=?)
            UNION ALL
            SELECT ts, category, title FROM stream_info
            WHERE channel_id=? AND ts>? AND ts<=?
            ORDER BY ts""", (channel_id, channel_id, t_from, channel_id, t_from, t_to)).fetchall()
        return {"channel_id": channel_id, "from": t_from, "to": t_to,
                "points": [[r["ts"], r["viewers"]] for r in pts],
                "info": [[r["ts"], r["category"], r["title"]] for r in info]}

    def stats(self) -> dict:
        n = self.conn.execute("SELECT COUNT(*), MIN(ts), MAX(ts) FROM viewer_samples").fetchone()
        return {"samples": n[0], "oldest": n[1], "newest": n[2],
                "channels": self.conn.execute("SELECT COUNT(*) FROM channels").fetchone()[0]}


class Server:
    def __init__(self, db: Db, retain_days: int):
        self.db = db
        self.retain_days = retain_days
        self.clients: set[asyncio.StreamWriter] = set()
        self.loop = asyncio.get_running_loop()

    async def query(self, fn, *args):
        return await self.loop.run_in_executor(None, fn, *args)

    @staticmethod
    def send(w: asyncio.StreamWriter, obj: dict) -> None:
        w.write((json.dumps(obj, ensure_ascii=False, separators=(",", ":")) + "\n").encode())

    def error(self, w, req_id, code, message):
        self.send(w, {"type": "error", "id": req_id, "code": code, "message": message})

    async def handle(self, req: dict, w: asyncio.StreamWriter) -> None:
        t, rid = req.get("type"), req.get("id")
        if t == "hello":
            self.send(w, {"type": "hello", "v": PROTO_V, "id": rid, "now": int(time.time()),
                          "retain_days": self.retain_days})
        elif t == "channels":
            self.send(w, {"type": "channels", "id": rid, "channels": await self.query(self.db.channels)})
        elif t == "samples":
            cid = req.get("channel_id")
            if not isinstance(cid, str) or not cid:
                return self.error(w, rid, "bad_arg", "channel_id 필요")
            now = int(time.time())
            t_to = int(req.get("to") or now)
            t_from = int(req.get("from") or t_to - 86400)
            if t_from > t_to or t_to - t_from > MAX_RANGE:
                return self.error(w, rid, "bad_arg", "구간은 0~30일")
            res = await self.query(self.db.samples, cid, t_from, t_to)
            self.send(w, {"type": "samples", "id": rid, **res})
        elif t == "stats":
            st = await self.query(self.db.stats)
            self.send(w, {"type": "stats", "id": rid, "clients": len(self.clients), **st})
        else:
            self.error(w, rid, "unknown_type", f"type={t!r}")

    async def on_client(self, r: asyncio.StreamReader, w: asyncio.StreamWriter) -> None:
        peer = w.get_extra_info("peername")
        self.clients.add(w)
        log.info("conn %s (총 %d)", peer, len(self.clients))
        pinger = asyncio.ensure_future(self.ping(w))
        try:
            hello_done = False
            while True:
                try:
                    line = await asyncio.wait_for(r.readline(), None if hello_done else HELLO_TIMEOUT)
                except asyncio.TimeoutError:
                    log.info("hello 타임아웃 %s", peer)
                    break
                except (asyncio.LimitOverrunError, ValueError):
                    log.info("줄 초과 %s", peer)
                    break
                if not line:
                    break
                try:
                    req = json.loads(line)
                    if not isinstance(req, dict):
                        raise ValueError
                except ValueError:
                    self.error(w, None, "bad_json", "JSON 객체가 아님")
                    continue
                if req.get("type") == "hello":
                    hello_done = True
                await self.handle(req, w)
                await w.drain()
        except (ConnectionError, asyncio.CancelledError):
            pass
        except Exception:
            log.exception("client %s", peer)
        finally:
            pinger.cancel()
            self.clients.discard(w)
            w.close()
            log.info("close %s (총 %d)", peer, len(self.clients))

    async def ping(self, w: asyncio.StreamWriter) -> None:
        try:
            while True:
                await asyncio.sleep(PING_INTERVAL)
                self.send(w, {"type": "ping"})
                await w.drain()
        except (asyncio.CancelledError, ConnectionError):
            pass

    async def shutdown(self) -> None:
        for w in list(self.clients):
            try:
                self.send(w, {"type": "bye"})
                await w.drain()
                w.close()
            except Exception:
                pass


async def amain() -> None:
    load_env()
    db = Db(Path(os.environ.get("SM_DB", "monitor.db")))
    port = int(os.environ.get("SM_PUSH_PORT", "9002"))
    srv = Server(db, int(os.environ.get("SM_RETAIN_DAYS", "30")))
    server = await asyncio.start_server(srv.on_client, "127.0.0.1", port, limit=MAX_LINE)
    log.info("listening 127.0.0.1:%d", port)

    stop = asyncio.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        try:
            asyncio.get_running_loop().add_signal_handler(sig, stop.set)
        except NotImplementedError:   # Windows (로컬 테스트용)
            pass
    await stop.wait()
    log.info("stopping")
    await srv.shutdown()
    server.close()
    await server.wait_closed()


def main() -> int:
    logging.basicConfig(level=logging.INFO, stream=sys.stdout,
                        format="%(asctime)s %(levelname)s %(message)s")
    loop = asyncio.new_event_loop()          # 3.8(서버)과 3.12+(로컬) 모두에서 동작
    asyncio.set_event_loop(loop)
    loop.run_until_complete(amain())
    return 0


if __name__ == "__main__":
    sys.exit(main())
