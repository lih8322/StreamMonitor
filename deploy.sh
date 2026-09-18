#!/usr/bin/env bash
# 서버(/opt/streammonitor)로 배포하고 데몬을 재시작한다.
#   ./deploy.sh cpp         # src/cpp 를 서버에서 빌드 → bin/ 설치 → 두 데몬 재시작 (운영)
#   ./deploy.sh collector   # Python 판 monitor.py + chzzk/ 복사 (예비)
#   ./deploy.sh push        # Python 판 push.py 복사 (예비)
set -euo pipefail
HOST=${SM_HOST:-oracle}
DEST=${SM_DEST:-/opt/streammonitor}
cd "$(dirname "$0")"

deploy_cpp() {
    ssh "$HOST" "mkdir -p $DEST/cpp $DEST/bin"
    scp -o BatchMode=yes -r src/cpp/CMakeLists.txt src/cpp/common src/cpp/collector src/cpp/push src/cpp/systemd "$HOST:$DEST/cpp/"
    ssh "$HOST" "set -e; cd $DEST/cpp
        cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
        cmake --build build -j2 2>&1 | grep -E 'warning|error|Built target' || true
        # 실행 중인 바이너리는 덮어쓸 수 없으니(ETXTBSY) 새 파일로 쓰고 rename 으로 교체
        for b in sm_collector sm_push; do cp build/\$b $DEST/bin/\$b.new && mv -f $DEST/bin/\$b.new $DEST/bin/\$b; done
        sudo cp systemd/streammonitor.service systemd/streammonitor-push.service /etc/systemd/system/
        sudo systemctl daemon-reload
        sudo systemctl restart streammonitor streammonitor-push
        sleep 3; systemctl is-active streammonitor streammonitor-push"
}
deploy_collector_py() {
    scp -o BatchMode=yes src/collector/monitor.py src/collector/test_auth.py src/collector/test_lives.py \
        src/collector/requirements.txt "$HOST:$DEST/"
    scp -o BatchMode=yes -r src/collector/chzzk "$HOST:$DEST/"
}
deploy_push_py() {
    scp -o BatchMode=yes src/push/push.py "$HOST:$DEST/"
}
case "${1:-cpp}" in
    cpp)       deploy_cpp ;;
    collector) deploy_collector_py ;;
    push)      deploy_push_py ;;
    *) echo "usage: $0 {cpp|collector|push}"; exit 1 ;;
esac
