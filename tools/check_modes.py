#!/usr/bin/env python3
"""检查 trades.jsonl 中 mode 字段分布 + 列出可疑 LIVE 记录（按 P1/P2/P3 + 时间戳启发）。"""
import json
from datetime import datetime, timezone

PATH = "/Users/theodore/Desktop/polymarket/logs/trades.jsonl"

with_mode_live = []
with_mode_dry  = []
without_mode   = []

for line in open(PATH):
    line = line.strip()
    if not line: continue
    j = json.loads(line)
    if "mode" in j:
        if j["mode"] == "live":
            with_mode_live.append(j)
        else:
            with_mode_dry.append(j)
    else:
        without_mode.append(j)

print(f"=== summary ===")
print(f"  mode=live  : {len(with_mode_live)}")
print(f"  mode=dry_run / 其他: {len(with_mode_dry)}")
print(f"  无 mode 字段（老记录）: {len(without_mode)}")
print(f"  总计: {len(with_mode_live)+len(with_mode_dry)+len(without_mode)}")
print()

if with_mode_live:
    print("=== mode=live 记录 ===")
    for j in with_mode_live:
        dt = datetime.fromtimestamp(j.get('exit_time',0)/1000, tz=timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')
        print(f"  id={j.get('id'):10s} exit={dt}  pnl=${j.get('pnl',0):+.4f}  reason={j.get('exit_reason','')}")
    print()

# 启发式找 5 月 1 日 03/04/08 时间段的 P1/P2/P3 候选（之前 LIVE 但无 mode 字段）
print("=== 候选 LIVE 记录（无 mode 字段，时间在 5/1 03/04/08 时段）===")
for j in without_mode:
    et = j.get('exit_time', 0)
    dt_utc = datetime.fromtimestamp(et/1000, tz=timezone.utc)
    if dt_utc.strftime('%Y-%m-%d') != '2026-05-01': continue
    if dt_utc.hour not in (3, 4, 8): continue
    print(f"  id={j.get('id'):10s} exit={dt_utc.strftime('%H:%M:%S UTC')}  pnl=${j.get('pnl',0):+.4f}  reason={j.get('exit_reason','')}")
