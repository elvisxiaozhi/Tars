#!/usr/bin/env python3
"""
扫 bot.log 里的 LIVE close 事件，匹配 trades.jsonl 的 (exit_time, pnl, reason) 改 mode=live。

时区假设：bot.log 时间戳是 Asia/Shanghai (UTC+8)，trades.jsonl exit_time 是 UTC ms。
匹配规则：exit_time ±120s + pnl 近似 ±0.5（fee 计算可能微差）+ exit_reason 一致。
不动已标 mode=live 的记录；备份原文件到 trades.jsonl.bak_modefix。
"""
import re
import json
import shutil
from datetime import datetime, timezone, timedelta
from pathlib import Path

LOG_PATH   = "/Users/theodore/Desktop/polymarket/logs/bot.log"
JSONL_PATH = "/Users/theodore/Desktop/polymarket/logs/trades.jsonl"
BACKUP     = JSONL_PATH + ".bak_modefix"

LOCAL_TZ = timezone(timedelta(hours=8))  # Asia/Shanghai

# ── 1. 从 bot.log 抓 LIVE close 事件 ─────────────────────────────────────────
# 出场点：LIVE STOP SELL ok / LIVE TPn SELL ok 后的下一个 CLOSE [Pn] event
text = Path(LOG_PATH).read_text(errors="replace")
lines = text.splitlines()

CLOSE_RE = re.compile(
    r'\[([0-9-]+ [0-9:.]+)\].*CLOSE \[(P\d+)\] (\w+) reason=(\w+) \| pnl=\$([+-]?[\d.]+)'
)
LIVE_TRIGGER_RE = re.compile(r'LIVE (STOP SELL|TP\d SELL) ok')

live_closes = []
for i, line in enumerate(lines):
    if not LIVE_TRIGGER_RE.search(line):
        continue
    # 后续 5 行内找 CLOSE event
    for j in range(i + 1, min(i + 6, len(lines))):
        m = CLOSE_RE.search(lines[j])
        if not m:
            continue
        ts_local_str = m.group(1)
        try:
            dt_local = datetime.strptime(ts_local_str, "%Y-%m-%d %H:%M:%S.%f").replace(tzinfo=LOCAL_TZ)
        except ValueError:
            break
        ms_utc = int(dt_local.timestamp() * 1000)
        live_closes.append({
            "ms_utc": ms_utc,
            "id":     m.group(2),
            "side":   m.group(3),
            "reason": m.group(4),
            "pnl":    float(m.group(5)),
        })
        break

print(f"=== bot.log 找到 {len(live_closes)} 个 LIVE close 事件 ===")
for c in live_closes:
    dt = datetime.fromtimestamp(c["ms_utc"] / 1000, tz=timezone.utc)
    print(f"  utc={dt.strftime('%Y-%m-%d %H:%M:%S')} {c['side']} {c['reason']} pnl=${c['pnl']:+.4f}")
print()

# ── 2. 读 jsonl 找匹配 ───────────────────────────────────────────────────────
records = []
for line in open(JSONL_PATH):
    line = line.strip()
    if not line:
        continue
    records.append(json.loads(line))

print(f"=== trades.jsonl 共 {len(records)} 条记录 ===")
print(f"  mode=live  : {sum(1 for r in records if r.get('mode') == 'live')}")
print(f"  其它       : {sum(1 for r in records if r.get('mode') != 'live')}")
print()

# 备份
shutil.copyfile(JSONL_PATH, BACKUP)
print(f"备份已写入 {BACKUP}")

# 匹配并 patch
matched = 0
for c in live_closes:
    best = None
    for r in records:
        if r.get("mode") == "live":
            continue  # 已标记
        if abs(r.get("exit_time", 0) - c["ms_utc"]) > 120_000:
            continue
        if abs(r.get("pnl", 0) - c["pnl"]) > 0.5:
            continue
        if r.get("exit_reason") != c["reason"]:
            continue
        # 优先时间最近的
        if best is None or abs(r["exit_time"] - c["ms_utc"]) < abs(best["exit_time"] - c["ms_utc"]):
            best = r
    if best is not None:
        best["mode"] = "live"
        matched += 1
        dt = datetime.fromtimestamp(best["exit_time"] / 1000, tz=timezone.utc)
        print(f"  ✓ matched id={best.get('id')} exit={dt.strftime('%Y-%m-%d %H:%M')} pnl=${best.get('pnl'):+.4f}")

print()
print(f"=== 标记 {matched} 条为 mode=live ===")

# 写回
with open(JSONL_PATH, "w") as f:
    for r in records:
        f.write(json.dumps(r, ensure_ascii=False) + "\n")
print(f"写入 {len(records)} 条到 {JSONL_PATH}")
print(f"如有问题，恢复：cp {BACKUP} {JSONL_PATH}")
