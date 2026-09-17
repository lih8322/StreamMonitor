"""
CHZZK 인증 API 수동 테스트.

  python test_auth.py login     # 브라우저 URL 출력 -> 로그인 후 code/state 입력 -> 토큰 저장
  python test_auth.py me        # 저장된 토큰으로 /open/v1/users/me 호출 (만료시 자동 갱신)
  python test_auth.py refresh   # 강제 갱신
  python test_auth.py client    # Client 인증만으로 공개 API(라이브 목록) 호출
  python test_auth.py revoke    # 토큰 폐기 + 파일 삭제

환경변수(.env 또는 export): CHZZK_CLIENT_ID, CHZZK_CLIENT_SECRET, CHZZK_REDIRECT_URI, CHZZK_TOKEN_FILE
"""
import os
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import requests

from chzzk.auth import API_HOST, ChzzkAuth, TokenStore
from chzzk.env import load_env


def main() -> int:
    load_env()
    auth = ChzzkAuth(
        client_id=os.environ["CHZZK_CLIENT_ID"],
        client_secret=os.environ["CHZZK_CLIENT_SECRET"],
        redirect_uri=os.environ.get("CHZZK_REDIRECT_URI", "http://localhost:8080/callback"),
    )
    store = TokenStore(auth, Path(os.environ.get("CHZZK_TOKEN_FILE", "token.json")))
    cmd = sys.argv[1] if len(sys.argv) > 1 else "me"

    if cmd == "login":
        url, state = auth.build_authorize_url()
        print("브라우저에서 열기:\n", url)
        raw = input("\n리다이렉트된 전체 URL 또는 code 입력: ").strip()
        if raw.startswith("http"):
            q = parse_qs(urlparse(raw).query)
            code, state_back = q["code"][0], q.get("state", [state])[0]
        else:
            code, state_back = raw, state
        if state_back != state:
            print("state 불일치 (CSRF?)", state, state_back)
            return 1
        tok = auth.exchange_code(code, state_back)
        store.save(tok)
        print("저장됨:", store.path, "expires_at:", tok.expires_at)

    elif cmd == "refresh":
        tok = auth.refresh(store.token.refresh_token)
        store.save(tok)
        print("갱신됨. 새 refreshToken 앞 8자:", tok.refresh_token[:8])

    elif cmd == "me":
        r = requests.get(f"{API_HOST}/open/v1/users/me", headers=store.bearer_headers(), timeout=10)
        print(r.status_code, r.json())

    elif cmd == "client":
        r = requests.get(f"{API_HOST}/open/v1/lives", headers=auth.client_headers(),
                         params={"size": 3}, timeout=10)
        print(r.status_code, r.json())

    elif cmd == "revoke":
        auth.revoke(store.token.refresh_token, "refresh_token")
        store.path.unlink(missing_ok=True)
        print("폐기 완료")

    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
