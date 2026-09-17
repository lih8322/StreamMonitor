"""
라이브 목록 페이지네이션 테스트: size=20 으로 next 커서를 따라가며 총 N개 조회.

  python test_lives.py            # 100개 조회, 상위 5개 상세 출력
  python test_lives.py 20 3       # 20개 조회, 상위 3개 상세 출력
"""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path

import requests

from chzzk.auth import API_HOST, ChzzkAuth
from chzzk.env import load_env

PAGE_SIZE = 20


def fetch_lives(auth: ChzzkAuth, total: int) -> list[dict]:
    lives: list[dict] = []
    next_cursor = None
    while len(lives) < total:
        params = {"size": PAGE_SIZE}
        if next_cursor:
            params["next"] = next_cursor
        r = requests.get(f"{API_HOST}/open/v1/lives", headers=auth.client_headers(),
                         params=params, timeout=10)
        body = r.json()
        if r.status_code != 200 or body.get("code") != 200:
            raise RuntimeError(f"{r.status_code} {body}")
        content = body["content"]
        data = content.get("data", [])
        lives.extend(data)
        next_cursor = (content.get("page") or {}).get("next")
        print(f"page fetched: +{len(data)} (total {len(lives)}) next={next_cursor}", file=sys.stderr)
        if not data or not next_cursor:
            break
        time.sleep(0.2)  # rate limit 여유
    return lives[:total]


def main() -> int:
    load_env()
    auth = ChzzkAuth(
        client_id=os.environ["CHZZK_CLIENT_ID"],
        client_secret=os.environ["CHZZK_CLIENT_SECRET"],
        redirect_uri=os.environ.get("CHZZK_REDIRECT_URI", ""),
    )
    total = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    detail = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    lives = fetch_lives(auth, total)

    # 상위 N개는 필드별 상세 출력
    for i, lv in enumerate(lives[:detail], 1):
        print(f"\n===== #{i} =====")
        width = max(len(k) for k in lv)
        for k, v in lv.items():
            print(f"  {k:<{width}} : {v}")

    print(f"\n총 {len(lives)}개 조회 (상세 {min(detail, len(lives))}개)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
