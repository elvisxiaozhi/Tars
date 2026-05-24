#!/usr/bin/env bash
# 在部署机上开启 core dump，让 bot 段错误能留下可调试的 core 文件。
#
# 背景：2026-05-24 10:00 bot SIGSEGV，但服务器 ulimit -c=0 且 core_pattern 走 apport，
# 没留下 core；二进制又是 Release（无 -g），addr2line 只能拿到函数名。本脚本 + CMake
# 改用 RelWithDebInfo 后，下次崩溃即可定位到具体源码行 + 完整栈。
#
# 幂等，可重复执行。需 root。
set -euo pipefail

CORE_DIR=/root/polymarket/cores
PATTERN="$CORE_DIR/core.%e.%p.%t"

mkdir -p "$CORE_DIR"

# 1) core_pattern：写文件而不是交给 apport（运行时 + 持久化到 sysctl.d）
echo "$PATTERN" > /proc/sys/kernel/core_pattern
echo "kernel.core_pattern=$PATTERN" > /etc/sysctl.d/60-polymarket-core.conf
sysctl --system >/dev/null 2>&1 || true

# 2) 给 manager systemd 服务加 LimitCORE=infinity（drop-in，下次重启 manager 持久生效）
DROPIN=/etc/systemd/system/polymarket-manager.service.d
mkdir -p "$DROPIN"
cat > "$DROPIN/coredump.conf" <<'EOF'
[Service]
LimitCORE=infinity
EOF
systemctl daemon-reload   # 只重载定义，不重启服务

# 3) 立即给当前正在跑的 manager 进程放开 core 软/硬限制（无需重启 manager）。
#    rlimit 在 fork 时继承，所以这之后由 manager 启动的 bot 子进程会带 unlimited core。
MPID="$(pgrep -f 'tools/manager.py' | head -1 || true)"
if [[ -n "${MPID}" ]]; then
    prlimit --pid "${MPID}" --core=unlimited:unlimited
    echo "[coredump] applied unlimited core to running manager pid=${MPID}"
else
    echo "[coredump] WARN: manager 进程未找到；重启 manager 服务后由 LimitCORE 生效"
fi

echo "[coredump] core_pattern = $(cat /proc/sys/kernel/core_pattern)"
echo "[coredump] DONE."
echo
echo "注意：当前正在跑的 bot 是在放开限制【之前】启动的，core 限制仍是 0。"
echo "要让它纳入 core dump，重启一次 bot（之后它会继承 unlimited）："
echo "  curl -s -X POST http://127.0.0.1:9090/api/shutdown && sleep 2 && \\"
echo "  curl -s -X POST http://127.0.0.1:9090/api/start"
echo
echo "崩溃后 core 文件落在：${CORE_DIR}/"
echo "分析：gdb /root/polymarket/build/polymarket-arb ${CORE_DIR}/core.*  →  bt full"
