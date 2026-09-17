"""
CHZZK Open API 인증 모듈.

흐름:
  1. build_authorize_url()  -> 브라우저에서 열어 로그인, redirectUri 로 code/state 수신
  2. exchange_code()        -> code -> accessToken/refreshToken
  3. refresh()              -> refreshToken 으로 갱신 (refreshToken 은 1회용, 즉시 저장)
  4. revoke()               -> 토큰 폐기

토큰은 TOKEN_FILE(JSON) 에 원자적으로 저장한다. 데몬은 TokenStore.get_access_token()
만 호출하면 만료 전 자동 갱신된다.
"""
from __future__ import annotations

import json
import os
import secrets
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from urllib.parse import urlencode

import requests

AUTH_HOST = "https://chzzk.naver.com"
API_HOST = "https://openapi.chzzk.naver.com"
TOKEN_URL = f"{API_HOST}/auth/v1/token"
REVOKE_URL = f"{API_HOST}/auth/v1/token/revoke"

# 만료 이 시간(초) 전에 미리 갱신
REFRESH_MARGIN = 60 * 60


class ChzzkAuthError(RuntimeError):
    pass


@dataclass
class Token:
    access_token: str
    refresh_token: str
    token_type: str
    expires_at: float  # epoch seconds

    @classmethod
    def from_response(cls, content: dict) -> "Token":
        return cls(
            access_token=content["accessToken"],
            refresh_token=content["refreshToken"],
            token_type=content.get("tokenType", "Bearer"),
            expires_at=time.time() + int(content.get("expiresIn", 86400)),
        )

    def expiring_soon(self) -> bool:
        return time.time() >= self.expires_at - REFRESH_MARGIN


def _post(url: str, body: dict, timeout: float = 10) -> dict:
    r = requests.post(url, json=body, timeout=timeout)
    try:
        data = r.json()
    except ValueError:
        raise ChzzkAuthError(f"non-JSON response {r.status_code}: {r.text[:200]}")
    if r.status_code != 200 or data.get("code") != 200:
        raise ChzzkAuthError(f"{url} -> {r.status_code} {data}")
    return data.get("content") or {}


class ChzzkAuth:
    def __init__(self, client_id: str, client_secret: str, redirect_uri: str):
        self.client_id = client_id
        self.client_secret = client_secret
        self.redirect_uri = redirect_uri

    # ---- 1. 인증 코드 요청 (브라우저) ----
    def build_authorize_url(self, state: str | None = None) -> tuple[str, str]:
        state = state or secrets.token_urlsafe(16)
        q = urlencode({
            "clientId": self.client_id,
            "redirectUri": self.redirect_uri,
            "state": state,
        })
        return f"{AUTH_HOST}/account-interlock?{q}", state

    # ---- 2. 토큰 발급 ----
    def exchange_code(self, code: str, state: str) -> Token:
        content = _post(TOKEN_URL, {
            "grantType": "authorization_code",
            "clientId": self.client_id,
            "clientSecret": self.client_secret,
            "code": code,
            "state": state,
        })
        return Token.from_response(content)

    # ---- 3. 토큰 갱신 ----
    def refresh(self, refresh_token: str) -> Token:
        content = _post(TOKEN_URL, {
            "grantType": "refresh_token",
            "refreshToken": refresh_token,
            "clientId": self.client_id,
            "clientSecret": self.client_secret,
        })
        return Token.from_response(content)

    # ---- 4. 토큰 폐기 ----
    def revoke(self, token: str, hint: str = "refresh_token") -> None:
        _post(REVOKE_URL, {
            "clientId": self.client_id,
            "clientSecret": self.client_secret,
            "token": token,
            "tokenTypeHint": hint,
        })

    # ---- Client 인증 헤더 (유저 토큰 불필요한 API용) ----
    def client_headers(self) -> dict:
        return {"Client-Id": self.client_id, "Client-Secret": self.client_secret}


class TokenStore:
    """토큰을 JSON 파일에 보관하고 필요 시 자동 갱신."""

    def __init__(self, auth: ChzzkAuth, path: Path):
        self.auth = auth
        self.path = Path(path)
        self._token: Token | None = self._load()

    def _load(self) -> Token | None:
        if not self.path.exists():
            return None
        with self.path.open(encoding="utf-8") as f:
            return Token(**json.load(f))

    def save(self, token: Token) -> None:
        # refreshToken 은 1회용이므로 원자적 쓰기 (쓰다 죽으면 재로그인 필요)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=self.path.parent, prefix=".tok-")
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(asdict(token), f)
        os.chmod(tmp, 0o600)
        os.replace(tmp, self.path)
        self._token = token

    @property
    def token(self) -> Token | None:
        return self._token

    def get_access_token(self) -> str:
        if self._token is None:
            raise ChzzkAuthError(f"no token at {self.path}; run login first")
        if self._token.expiring_soon():
            self.save(self.auth.refresh(self._token.refresh_token))
        return self._token.access_token

    def bearer_headers(self) -> dict:
        return {"Authorization": f"Bearer {self.get_access_token()}"}
