#!/usr/bin/env bash
# 서버(/opt/streammonitor)로 배포하고 데몬을 재시작한다.
#   ./deploy.sh collector   # monitor.py + chzzk/ → streammonitor 재시작
#   ./deploy.sh push        # push.py → streammonitor-push 재시작
#   ./deploy.sh all
set -euo pipefail
HOST=${SM_HOST:-oracle}
DEST=${SM_DEST:-/opt/streammonitor}
cd "$(dirname "$0")"

deploy_collector() {
    scp -o BatchMode=yes src/collector/monitor.py src/collector/test_auth.py src/collector/test_lives.py \
        src/collector/requirements.txt src/collector/streammonitor.service "$HOST:$DEST/"
    scp -o BatchMode=yes -r src/collector/chzzk "$HOST:$DEST/"
    ssh "$HOST" "sudo systemctl restart streammonitor && sleep 2 && systemctl is-active streammonitor"
}
deploy_push() {
    scp -o BatchMode=yes src/push/push.py src/push/streammonitor-push.service "$HOST:$DEST/"
    ssh "$HOST" "sudo systemctl restart streammonitor-push && sleep 1 && systemctl is-active streammonitor-push"
}
case "${1:-all}" in
    collector) deploy_collector ;;
    push)      deploy_push ;;
    all)       deploy_collector; deploy_push ;;
    *) echo "usage: $0 {collector|push|all}"; exit 1 ;;
esac
