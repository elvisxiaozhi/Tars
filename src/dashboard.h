#pragma once

#include <string>

namespace polymarket {

inline const std::string DASHBOARD_HTML = R"html(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Polymarket BTC 1h Bot</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body { font-family: 'SF Mono','Menlo','Consolas',monospace; background: #0a0e17; color: #e0e6ed; padding: 20px; }
.header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; border-bottom: 1px solid #1e2d3d; padding-bottom: 12px; }
h1 { font-size: 1.2em; color: #58a6ff; }
.uptime { font-size: 0.75em; color: #7d8590; margin-top: 4px; }
.mode { padding: 4px 10px; border-radius: 4px; font-size: 0.82em; font-weight: bold; }
.mode-dry  { background: #1a3a2a; color: #3fb950; }
.mode-live { background: #3a1a1a; color: #f85149; }
.bot-dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; background: #484f58; flex-shrink: 0; }
.bot-status-text { font-size: 0.8em; color: #7d8590; }
.metrics { display: grid; grid-template-columns: repeat(4,1fr); gap: 1px; background: #1e2d3d; border: 1px solid #1e2d3d; border-radius: 8px; margin-bottom: 12px; overflow: hidden; }
.metric { background: #111827; padding: 12px 16px; }
.metric-label { font-size: 0.65em; color: #7d8590; text-transform: uppercase; letter-spacing: 0.8px; margin-bottom: 5px; }
.metric-value { font-size: 1.5em; font-weight: bold; line-height: 1; }
.metric-sub { font-size: 0.72em; color: #7d8590; margin-top: 4px; }
.bar { display: flex; flex-wrap: wrap; gap: 16px; align-items: center; background: #111827; border: 1px solid #1e2d3d; border-radius: 6px; padding: 9px 14px; margin-bottom: 10px; font-size: 0.82em; }
.lbl { color: #484f58; }
.scope-row { display: flex; align-items: center; gap: 8px; margin-bottom: 12px; font-size: 0.82em; }
.scope-btn, .filter-btn { cursor: pointer; padding: 3px 12px; border: 1px solid #30363d; background: #21262d; color: #c9d1d9; border-radius: 4px; font-size: 0.82em; }
.scope-btn.active, .filter-btn.active { background: #388bfd; color: #fff; border-color: #388bfd; }
.tabs { display: flex; gap: 6px; margin-bottom: 16px; border-bottom: 1px solid #1e2d3d; }
.tab-btn { cursor: pointer; padding: 7px 14px; border: 1px solid #30363d; border-bottom: none; background: #111827; color: #c9d1d9; border-radius: 6px 6px 0 0; font-size: 0.85em; }
.tab-btn.active { background: #1f6feb; color: #fff; border-color: #388bfd; }
.tab-panel { display: none; }
.tab-panel.active { display: block; }
.section { margin-bottom: 18px; }
.sec-title { font-size: 0.88em; color: #58a6ff; margin-bottom: 10px; padding-bottom: 6px; border-bottom: 1px solid #1e2d3d; display: flex; align-items: center; gap: 10px; }
details.dsec > summary { font-size: 0.88em; color: #58a6ff; padding-bottom: 6px; border-bottom: 1px solid #1e2d3d; margin-bottom: 10px; cursor: pointer; list-style: none; user-select: none; display: flex; align-items: center; gap: 6px; }
details.dsec > summary::-webkit-details-marker { display: none; }
details.dsec > summary::before { content: '▸ '; }
details.dsec[open] > summary::before { content: '▾ '; }
.cards { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-top: 10px; }
.card { background: #111827; border: 1px solid #1e2d3d; border-radius: 6px; padding: 14px; }
.card-title { font-size: 0.72em; text-transform: uppercase; letter-spacing: 0.8px; margin-bottom: 10px; }
.card-body { font-size: 0.82em; line-height: 1.9; color: #c9d1d9; }
.card-body .sub { color: #7d8590; font-size: 0.88em; }
.stats-grid { display: grid; grid-template-columns: repeat(4,1fr); gap: 10px; margin-bottom: 14px; }
.stat { background: #111827; border: 1px solid #1e2d3d; border-radius: 6px; padding: 10px; text-align: center; }
.stat-label { font-size: 0.68em; color: #7d8590; margin-bottom: 4px; }
.stat-value { font-size: 1.1em; font-weight: bold; }
table { width: 100%; border-collapse: collapse; font-size: 0.82em; }
th { text-align: left; padding: 7px 10px; color: #7d8590; font-size: 0.72em; text-transform: uppercase; letter-spacing: 0.8px; border-bottom: 1px solid #1e2d3d; }
td { padding: 7px 10px; border-bottom: 1px solid #0d1117; }
tr:hover td { background: #161b22; }
.side-up   { color: #3fb950; font-weight: bold; }
.side-down { color: #f85149; font-weight: bold; }
.positive { color: #3fb950; }
.negative { color: #f85149; }
.neutral  { color: #58a6ff; }
.empty { color: #7d8590; padding: 20px; text-align: center; }
.rb { display: inline-block; padding: 1px 7px; border-radius: 4px; background: #1e2d3d; color: #e0e6ed; font-size: 0.88em; white-space: nowrap; }
.rb-profit  { background: #14351f; color: #56d364; }
.rb-risk    { background: #3a1a1a; color: #f85149; }
.rb-protect { background: #332b13; color: #d29922; }
.split-row td { background: #0d1117; padding: 0 10px 10px; }
.split-det { margin-top: 6px; border: 1px solid #1e2d3d; border-radius: 6px; overflow: hidden; }
.split-det summary { cursor: pointer; color: #58a6ff; padding: 7px 10px; list-style: none; user-select: none; }
.split-det summary::-webkit-details-marker { display: none; }
.split-det[open] summary { border-bottom: 1px solid #1e2d3d; }
@media (max-width: 700px) {
  .metrics { grid-template-columns: repeat(2,1fr); }
  .cards   { grid-template-columns: 1fr; }
  .stats-grid { grid-template-columns: repeat(2,1fr); }
}
</style>
</head>
<body>

<div class="header">
  <div>
    <h1>Polymarket BTC 1h Bot</h1>
    <div class="uptime" id="uptime">--</div>
  </div>
  <div style="display:flex;align-items:center;gap:10px;">
    <span class="mode" id="mode-badge">--</span>
    <span class="bot-dot" id="bot-status-dot"></span>
    <span class="bot-status-text" id="bot-status-text">--</span>
    <button id="bot-control-btn" style="cursor:pointer;padding:5px 12px;border:1px solid #30363d;background:#21262d;color:#c9d1d9;border-radius:4px;font-size:0.8em;font-weight:bold;min-width:90px;">--</button>
  </div>
</div>

<div class="metrics">
  <div class="metric">
    <div class="metric-label">Win Rate</div>
    <div class="metric-value neutral" id="win-rate">--%</div>
    <div class="metric-sub" id="win-loss">0W / 0L</div>
  </div>
  <div class="metric">
    <div class="metric-label">Realized P&amp;L</div>
    <div class="metric-value" id="total-pnl">$0.00</div>
    <div class="metric-sub" id="pnl-sub">0 trades</div>
  </div>
  <div class="metric">
    <div class="metric-label">Balance</div>
    <div class="metric-value neutral" id="account-balance">--</div>
    <div class="metric-sub">BTC <span id="btc-price">--</span></div>
  </div>
  <div class="metric">
    <div class="metric-label">Open Position</div>
    <div class="metric-value neutral" id="open-pos">0</div>
    <div class="metric-sub">UPnL <span id="unrealized-pnl">$0.00</span></div>
  </div>
</div>

<div class="scope-row">
  <span class="lbl">Data:</span>
  <button class="scope-btn" data-scope="session">This Run</button>
  <button class="scope-btn" data-scope="all">All Time</button>
</div>

<div class="bar">
  <span><span class="lbl">BTC</span> <span id="btc-price-bar">--</span></span>
  <span><span class="lbl">偏移</span> <span id="btc-dev">--</span></span>
  <span><span class="lbl">剩余</span> <span id="remaining">--</span> min</span>
  <span><span class="lbl">WS</span> <span id="clob-ws">--</span></span>
  <span id="regime-hint" style="font-weight:bold;color:#484f58;">--</span>
</div>

<div class="bar">
  <span class="lbl">Primary</span>
  <span style="color:#58a6ff;font-weight:bold;" id="market-name">--</span>
  <span>UP <span class="positive" id="up-bid">--</span>/<span class="positive" id="up-ask">--</span></span>
  <span>DN <span class="negative" id="down-bid">--</span>/<span class="negative" id="down-ask">--</span></span>
  <span class="lbl">(bid/ask)</span>
</div>

<div class="tabs">
  <button class="tab-btn" data-tab="main">Main Strategy</button>
  <button class="tab-btn" data-tab="regime">Trend Follow v2</button>
  <button class="tab-btn" data-tab="contra-hold">Contrarian Hold</button>
  <button class="tab-btn" data-tab="crypto-4h">Crypto 4H</button>
  <button class="tab-btn" data-tab="crypto-daily">Crypto Daily</button>
  <button class="tab-btn" data-tab="finance">Finance</button>
</div>

<!-- ===== Main Tab ===== -->
<div id="tab-main" class="tab-panel">

<div class="section">
  <div class="sec-title">Coin P&amp;L</div>
  <div id="main-coin-pnl"></div>
</div>

<details class="dsec section" id="dd-strategy-rules">
  <summary>当前策略规则</summary>
  <div class="cards">
    <div class="card">
      <div class="card-title" style="color:#d29922;">Momentum Follow — BTC 趋势追踪</div>
      <div class="card-body">
        <div class="sub">入场条件</div>
        <div>· Coin：<b>BTC only</b></div>
        <div>· 时间窗口：<b>35 ≤ 剩余 &lt; 45 min</b></div>
        <div>· |BTC偏移| ≥ 0.20%，顺偏移方向</div>
        <div>· 入场价：0.64 – 0.79¢ &nbsp;·&nbsp; 价差 ≤ 1¢</div>
        <div class="sub">止盈</div>
        <div>· TP0: min(0.88, entry+8¢) → 卖 50%</div>
        <div>· TP1: min(0.90, entry+14¢) → 卖剩余 50%</div>
        <div class="sub">止损</div>
        <div>· 价格止损：≤ entry −7¢</div>
        <div>· 动量熄火：偏移反向变化 &gt; 8¢</div>
        <div>· 无启动：2.5min MFE &lt; 3¢ &amp; 浅亏 ≥ 3¢</div>
        <div>· Trailing MFE≥12¢：max(entry+5¢, peak−4¢)</div>
        <div>· Trailing MFE≥8¢：max(entry+2¢, peak−5¢)</div>
        <div>· TP后回落到 entry+2¢ → 保护退出</div>
        <div>· 最后8min &lt; 78¢ 或方向反转 → 清仓</div>
        <div>· 最后5min &lt; 88¢ 或方向反转 → 清仓</div>
      </div>
    </div>
    <div class="card">
      <div class="card-title" style="color:#f85149;">风控</div>
      <div class="card-body">
        <div>· 单仓制：同时最多 1 仓</div>
        <div>· 本 K 线止损后锁仓到下根 K</div>
        <div>· 每日亏损上限保护</div>
        <div class="sub">入场逻辑</div>
        <div>· BTC 涨 → 买 UP；BTC 跌 → 买 DOWN</div>
        <div>· 偏移 &lt; 0.20% → 不入场</div>
        <div>· 不在 35–45min 窗口 → 等待</div>
        <div class="sub">已退役</div>
        <div>· Quiet Reversion（逆势低价）→ 退役</div>
      </div>
    </div>
  </div>
</details>

<div class="section" id="open-positions-section" style="display:none;">
  <div class="sec-title">持仓中</div>
  <div id="positions-container"></div>
</div>

<details class="dsec section" id="dd-trade-history">
  <summary>
    交易记录
    <span id="th-count" style="font-size:0.85em;font-weight:normal;color:#7d8590;margin-left:4px;"></span>
    <span style="margin-left:auto;display:flex;gap:6px;" onclick="event.stopPropagation()">
      <button class="filter-btn" data-filter="all">All</button>
      <button class="filter-btn" data-filter="live">Live</button>
      <button class="filter-btn" data-filter="dry_run">Dry</button>
    </span>
  </summary>
  <table style="margin-top:10px;">
    <thead><tr>
      <th>时间</th><th>Coin</th><th>方向</th><th>入场</th><th>出场</th><th>成本</th><th>盈亏</th><th>退出原因</th><th>时长</th>
    </tr></thead>
    <tbody id="trades-body"><tr><td colspan="9" class="empty">No trades yet</td></tr></tbody>
  </table>
</details>

</div><!-- /tab-main -->

<!-- ===== Trend Follow v2 ===== -->
<div id="tab-regime" class="tab-panel">
<details class="dsec section" id="dd-regime-rules">
  <summary>Strategy Rules — Trend Follow v2</summary>
  <div style="display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px;margin-top:10px;">
    <div class="card">
      <div class="card-title" style="color:#58a6ff;">入场条件</div>
      <div class="card-body">
        <div>· Coin：BTC / ETH / BNB</div>
        <div>· 时间窗口：41 ≤ 剩余 ≤ 45 min</div>
        <div>· |偏移| ≥ 0.18%，连续 2 tick 确认</div>
        <div>· 入场价：0.64–0.67¢（顺偏移）</div>
        <div>· 价差 ≤ 1¢ · 仓位 $1.00</div>
      </div>
    </div>
    <div class="card">
      <div class="card-title" style="color:#3fb950;">止盈</div>
      <div class="card-body">
        <div>· TP0：entry+10¢ → 50%</div>
        <div>· TP1：entry+15¢ → 50%</div>
        <div>· TP2：0.90¢ → 清仓</div>
      </div>
    </div>
    <div class="card">
      <div class="card-title" style="color:#f85149;">止损</div>
      <div class="card-body">
        <div>· 价格止损：≤ entry −10¢</div>
        <div>· 偏移穿零 → stop_btc</div>
        <div>· 5min MFE &lt; 3¢ → 死水退出</div>
        <div>· Trailing MFE≥15¢：max(入+6¢,峰-5¢)</div>
        <div>· Trailing MFE≥8¢：max(入+2¢,峰-6¢)</div>
      </div>
    </div>
  </div>
</details>
<div class="section">
  <div class="sec-title">Trend Follow v2 <span id="trend-v2-exp-summary" style="font-size:0.82em;font-weight:normal;color:#7d8590;"></span></div>
  <div class="stats-grid">
    <div class="stat"><div class="stat-label">Balance</div><div class="stat-value neutral" id="trend-v2-exp-balance">--</div></div>
    <div class="stat"><div class="stat-label">P&amp;L</div><div class="stat-value" id="trend-v2-exp-pnl">--</div></div>
    <div class="stat"><div class="stat-label">Open</div><div class="stat-value neutral" id="trend-v2-exp-open">--</div></div>
    <div class="stat"><div class="stat-label">Trades</div><div class="stat-value neutral" id="trend-v2-exp-trades-count">--</div></div>
  </div>
  <div class="section"><div class="sec-title" style="border:none;margin-bottom:6px;">Coin P&amp;L</div><div id="trend-v2-exp-coin-pnl"></div></div>
  <div id="trend-v2-exp-positions"></div>
  <div id="trend-v2-exp-trades" style="margin-top:10px;"></div>
</div>
</div>

<!-- ===== Contrarian Hold ===== -->
<div id="tab-contra-hold" class="tab-panel">
<details class="dsec section" id="dd-contra-hold-rules">
  <summary>Strategy Rules — Contrarian Hold (shadow，反向·持有到期·近零费 · ⚠️ 低确认验证)</summary>
  <div style="display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px;margin-top:10px;">
    <div class="card">
      <div class="card-title" style="color:#58a6ff;">入场条件</div>
      <div class="card-body">
        <div>· Coin：仅 BTC</div>
        <div>· 时间窗口：剩余 20–40 min（中段）</div>
        <div>· 买 underdog（dev 反方向/便宜侧）</div>
        <div>· |偏移| ≥ 0.08% · 入场价 0.38–0.48 · 价差 ≤ 2¢</div>
      </div>
    </div>
    <div class="card">
      <div class="card-title" style="color:#d29922;">持有到期 + 费用</div>
      <div class="card-body">
        <div>· 不设 TP / 不设止损</div>
        <div>· 持有到 1h 结算（0/1）</div>
        <div>· 入场 maker + 到期免费 → 近零费</div>
      </div>
    </div>
    <div class="card">
      <div class="card-title" style="color:#3fb950;">论点 / 验证（⚠️ 低确认）</div>
      <div class="card-body">
        <div>· held_momentum 镜像：favored 侧 43%&lt;价</div>
        <div>· 赌中段 move 到收盘反转</div>
        <div>· 判据：分桶真实胜率 &gt; 入场价</div>
        <div>· 风险：动量镜像，趋势市会翻负</div>
      </div>
    </div>
  </div>
</details>
<div class="section">
  <div class="sec-title">Contrarian Hold <span id="contra-hold-exp-summary" style="font-size:0.82em;font-weight:normal;color:#7d8590;"></span></div>
  <div class="stats-grid">
    <div class="stat"><div class="stat-label">Balance</div><div class="stat-value neutral" id="contra-hold-exp-balance">--</div></div>
    <div class="stat"><div class="stat-label">P&amp;L</div><div class="stat-value" id="contra-hold-exp-pnl">--</div></div>
    <div class="stat"><div class="stat-label">Open</div><div class="stat-value neutral" id="contra-hold-exp-open">--</div></div>
    <div class="stat"><div class="stat-label">Trades</div><div class="stat-value neutral" id="contra-hold-exp-trades-count">--</div></div>
  </div>
  <div class="section"><div class="sec-title" style="border:none;margin-bottom:6px;">Coin P&amp;L</div><div id="contra-hold-exp-coin-pnl"></div></div>
  <div id="contra-hold-exp-positions"></div>
  <div id="contra-hold-exp-trades" style="margin-top:10px;"></div>
</div>
</div>

<!-- ===== Crypto 4H ===== -->
<div id="tab-crypto-4h" class="tab-panel">
<details class="dsec section" id="dd-crypto4h-rules">
  <summary>Strategy Rules — Crypto 4H Up/Down</summary>
  <div style="margin-top:10px;max-width:500px;">
    <div class="card">
      <div class="card-title" style="color:#3fb950;">Crypto 4H Up/Down</div>
      <div class="card-body">
        <div class="sub">入场条件</div>
        <div>· Coin：BTC / ETH / SOL / BNB / XRP</div>
        <div>· 时间窗口：60 ≤ 剩余 ≤ 210 min</div>
        <div>· |偏移| ≥ 0.35%（BTC/ETH）/ 0.50%（其他）</div>
        <div>· 入场价：0.55–0.70¢，顺偏移，仓位 $0.50</div>
        <div class="sub">止盈 / 止损</div>
        <div>· TP0: entry+8¢(40%)  TP1: entry+15¢(58%)</div>
        <div>· ≤ entry-10¢，偏移穿零，5min 无 MFE → 死水</div>
        <div>· Trailing MFE≥15¢：max(入+6¢,峰-5¢)</div>
      </div>
    </div>
  </div>
</details>
<div class="section">
  <div class="sec-title">Crypto 4H <span id="crypto-4h-exp-summary" style="font-size:0.82em;font-weight:normal;color:#7d8590;"></span></div>
  <div class="stats-grid">
    <div class="stat"><div class="stat-label">Balance</div><div class="stat-value neutral" id="crypto-4h-exp-balance">--</div></div>
    <div class="stat"><div class="stat-label">P&amp;L</div><div class="stat-value" id="crypto-4h-exp-pnl">--</div></div>
    <div class="stat"><div class="stat-label">Open</div><div class="stat-value neutral" id="crypto-4h-exp-open">--</div></div>
    <div class="stat"><div class="stat-label">Trades</div><div class="stat-value neutral" id="crypto-4h-exp-trades-count">--</div></div>
  </div>
  <div class="section"><div class="sec-title" style="border:none;margin-bottom:6px;">Coin P&amp;L</div><div id="crypto-4h-exp-coin-pnl"></div></div>
  <div id="crypto-4h-exp-positions"></div>
  <div id="crypto-4h-exp-trades" style="margin-top:10px;"></div>
</div>
</div>

<!-- ===== Crypto Daily ===== -->
<div id="tab-crypto-daily" class="tab-panel">
<details class="dsec section" id="dd-cryptodaily-rules">
  <summary>Strategy Rules — Crypto Daily Up/Down</summary>
  <div style="margin-top:10px;max-width:500px;">
    <div class="card">
      <div class="card-title" style="color:#56d364;">Crypto Daily Up/Down</div>
      <div class="card-body">
        <div class="sub">入场条件</div>
        <div>· Coin：BTC / ETH / SOL / BNB</div>
        <div>· 时间窗口：180 ≤ 剩余 ≤ 900 min</div>
        <div>· |偏移| ≥ 0.60%（BTC/ETH）/ 0.90%（其他）</div>
        <div>· 入场价：0.58–0.72¢，顺偏移，仓位 $0.50</div>
        <div class="sub">止盈 / 止损</div>
        <div>· TP0: entry+8¢(35%)  TP1: entry+16¢(54%)</div>
        <div>· ≤ entry-10¢，偏移穿零，5min 无 MFE → 死水</div>
        <div>· Trailing MFE≥15¢：max(入+6¢,峰-5¢)</div>
      </div>
    </div>
  </div>
</details>
<div class="section">
  <div class="sec-title">Crypto Daily <span id="crypto-daily-exp-summary" style="font-size:0.82em;font-weight:normal;color:#7d8590;"></span></div>
  <div class="stats-grid">
    <div class="stat"><div class="stat-label">Balance</div><div class="stat-value neutral" id="crypto-daily-exp-balance">--</div></div>
    <div class="stat"><div class="stat-label">P&amp;L</div><div class="stat-value" id="crypto-daily-exp-pnl">--</div></div>
    <div class="stat"><div class="stat-label">Open</div><div class="stat-value neutral" id="crypto-daily-exp-open">--</div></div>
    <div class="stat"><div class="stat-label">Trades</div><div class="stat-value neutral" id="crypto-daily-exp-trades-count">--</div></div>
  </div>
  <div class="section"><div class="sec-title" style="border:none;margin-bottom:6px;">Coin P&amp;L</div><div id="crypto-daily-exp-coin-pnl"></div></div>
  <div id="crypto-daily-exp-positions"></div>
  <div id="crypto-daily-exp-trades" style="margin-top:10px;"></div>
</div>
</div>

<!-- ===== Finance ===== -->
<div id="tab-finance" class="tab-panel">
<details class="dsec section" id="dd-finance-rules">
  <summary>Strategy Rules — Finance（SPX / GOLD）</summary>
  <div style="margin-top:10px;max-width:500px;">
    <div class="card">
      <div class="card-title" style="color:#58a6ff;">Finance（SPX / GOLD）</div>
      <div class="card-body">
        <div class="sub">入场条件</div>
        <div>· 标的：SPX、GOLD  · 时间窗口：20 &lt; 剩余 ≤ 390 min</div>
        <div>· |偏移| ≥ 0.35%（SPX）/ 0.45%（GOLD）</div>
        <div class="sub">Trend 模式（60–330min）</div>
        <div>· 0.55–0.72¢，顺偏移，$0.50</div>
        <div>· TP0: entry+8¢(50%)  TP1: entry+15¢(50%)</div>
        <div class="sub">极端反转（90–300min，|偏移|≥1.0/1.2%）</div>
        <div>· 0.18–0.30¢，逆偏移，$0.25</div>
        <div>· TP0: entry+10¢(60%)  TP1: entry+20¢(all)</div>
        <div class="sub">止损</div>
        <div>· ≤ entry-10¢，偏移穿零，5min 无 MFE → 死水</div>
      </div>
    </div>
  </div>
</details>
<div class="section">
  <div class="sec-title">Finance <span id="finance-exp-summary" style="font-size:0.82em;font-weight:normal;color:#7d8590;"></span></div>
  <div class="stats-grid">
    <div class="stat"><div class="stat-label">Balance</div><div class="stat-value neutral" id="finance-exp-balance">--</div></div>
    <div class="stat"><div class="stat-label">P&amp;L</div><div class="stat-value" id="finance-exp-pnl">--</div></div>
    <div class="stat"><div class="stat-label">Open</div><div class="stat-value neutral" id="finance-exp-open">--</div></div>
    <div class="stat"><div class="stat-label">Trades</div><div class="stat-value neutral" id="finance-exp-trades-count">--</div></div>
  </div>
  <div class="section"><div class="sec-title" style="border:none;margin-bottom:6px;">Asset P&amp;L</div><div id="finance-exp-coin-pnl"></div></div>
  <div id="finance-exp-positions"></div>
  <div id="finance-exp-trades" style="margin-top:10px;"></div>
</div>
</div>

<script>
const REFRESH_MS = 5000;
let currentFilter = localStorage.getItem('dashboard.filter') || 'all';
let currentScope  = localStorage.getItem('dashboard.scope')  || 'session';

document.addEventListener('DOMContentLoaded', () => {
  // Scope
  const scopeBtns = document.querySelectorAll('.scope-btn');
  function applyScope() { scopeBtns.forEach(b => b.classList.toggle('active', b.dataset.scope === currentScope)); }
  applyScope();
  scopeBtns.forEach(btn => btn.addEventListener('click', () => {
    currentScope = btn.dataset.scope;
    localStorage.setItem('dashboard.scope', currentScope);
    applyScope(); refresh();
  }));

  // Filter
  const filterBtns = document.querySelectorAll('.filter-btn');
  function applyFilter() { filterBtns.forEach(b => b.classList.toggle('active', b.dataset.filter === currentFilter)); }
  applyFilter();
  filterBtns.forEach(btn => btn.addEventListener('click', () => {
    currentFilter = btn.dataset.filter;
    localStorage.setItem('dashboard.filter', currentFilter);
    applyFilter(); refresh();
  }));

  // Tabs
  const tabBtns   = document.querySelectorAll('.tab-btn');
  const tabPanels = document.querySelectorAll('.tab-panel');
  function activateTab(tab) {
    tabBtns.forEach(b  => b.classList.toggle('active', b.dataset.tab === tab));
    tabPanels.forEach(p => p.classList.toggle('active', p.id === 'tab-' + tab));
    localStorage.setItem('dashboard.tab', tab);
  }
  const savedTab = localStorage.getItem('dashboard.tab') || 'main';
  activateTab(document.getElementById('tab-' + savedTab) ? savedTab : 'main');
  tabBtns.forEach(btn => btn.addEventListener('click', () => activateTab(btn.dataset.tab)));

  // Details persistence (all collapsed by default)
  ['dd-strategy-rules','dd-trade-history','dd-regime-rules','dd-crypto4h-rules','dd-cryptodaily-rules','dd-finance-rules'].forEach(id => {
    const el = document.getElementById(id);
    if (!el) return;
    const stored = localStorage.getItem('dd.' + id);
    el.open = stored === '1';
    el.addEventListener('toggle', () => localStorage.setItem('dd.' + id, el.open ? '1' : '0'));
  });

  // Bot control
  let _botRunning = false;
  function updateBotUI(s) {
    const dot  = document.getElementById('bot-status-dot');
    const text = document.getElementById('bot-status-text');
    const btn  = document.getElementById('bot-control-btn');
    const colors = {running:'#3fb950',stopped:'#f85149',crashed:'#da3633',starting:'#d29922'};
    const labels = {running:'Running',stopped:'Stopped',crashed:'Crashed',starting:'Starting...'};
    if (dot)  dot.style.background = colors[s] || '#484f58';
    if (text) text.textContent = labels[s] || s;
    _botRunning = s === 'running';
    if (!btn) return;
    if (s === 'running') {
      btn.textContent = '⏹ Stop Bot';
      btn.style.cssText = 'cursor:pointer;padding:5px 12px;border:1px solid #6e1a1a;background:#3a1a1a;color:#f85149;border-radius:4px;font-size:0.8em;font-weight:bold;min-width:90px;';
      btn.disabled = false;
    } else if (s === 'starting') {
      btn.textContent = '⏳ Starting...';
      btn.style.cssText = 'cursor:default;padding:5px 12px;border:1px solid #30363d;background:#21262d;color:#d29922;border-radius:4px;font-size:0.8em;font-weight:bold;min-width:90px;';
      btn.disabled = true;
    } else {
      btn.textContent = '▶ Start Bot';
      btn.style.cssText = 'cursor:pointer;padding:5px 12px;border:1px solid #2ea043;background:#1a3a2a;color:#3fb950;border-radius:4px;font-size:0.8em;font-weight:bold;min-width:90px;';
      btn.disabled = false;
    }
  }
  async function pollManager() {
    try { const r = await fetch('/api/manager/status'); if (r.ok) { const d = await r.json(); updateBotUI(d.bot_status||'stopped'); } } catch(e) {}
  }
  document.getElementById('bot-control-btn').addEventListener('click', async () => {
    const btn = document.getElementById('bot-control-btn');
    if (_botRunning) {
      if (!confirm('确认停止 bot？\n\n会触发 emergency_close_all：\n· cancel 所有未平仓订单\n· SELL FOK 所有持仓平仓\n\n之后程序退出。')) return;
      btn.disabled = true; btn.textContent = '⏳ Stopping...';
      try { await fetch('/api/shutdown', {method:'POST'}); } catch(e) {}
      setTimeout(pollManager,1500); setTimeout(pollManager,3000);
    } else {
      btn.disabled = true; updateBotUI('starting');
      try {
        const res = await fetch('/api/start', {method:'POST'});
        if (!res.ok) { const d=await res.json().catch(()=>({})); alert('Start failed: '+(d.error||res.status)); updateBotUI('stopped'); return; }
      } catch(e) { alert('Start failed: '+e.message); updateBotUI('stopped'); return; }
      let n = 0;
      const poll = setInterval(async () => { await pollManager(); if (_botRunning || ++n > 30) clearInterval(poll); }, 1000);
    }
  });
  setInterval(pollManager, 3000);
  pollManager();
});

// --- Helpers ---
function fmtPrice(v) { return v ? '$'+Number(v).toLocaleString('en-US',{minimumFractionDigits:2,maximumFractionDigits:2}) : '--'; }
function fmtPct(v)   { return v !== undefined ? (v>=0?'+':'')+v.toFixed(2)+'%' : '--'; }
function fmtPnl(v)   { return (v>=0?'+$':'-$')+Math.abs(v).toFixed(2); }
function pnlCls(v)   { return v>0?'positive':v<0?'negative':'neutral'; }
function fmtDur(s)   { return Math.floor(s/60)+'m'+(s%60)+'s'; }
function fmtTime(ms) { if (!ms) return '--'; return new Date(ms).toLocaleString('zh-CN',{month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit'}); }
function esc(s) { return String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])); }
function inferCoin(t) {
  if (t.coin) return String(t.coin).toUpperCase();
  const m = (t.market||'').toLowerCase();
  if (m.includes('ethereum')) return 'ETH';
  if (m.includes('solana'))   return 'SOL';
  if (m.includes('xrp'))      return 'XRP';
  if (m.includes('dogecoin')) return 'DOGE';
  if (m.includes('bnb'))      return 'BNB';
  return 'BTC';
}
function normalizeId(id) { return String(id||'--').replace(/-TP\d+$/i,''); }
function periodKey(t) {
  const coin = inferCoin(t), mkt = String(t.market||'').trim();
  if (mkt) return coin+'|'+mkt;
  const ts = t.entry_time||t.exit_time||0;
  return coin+'|'+(ts ? Math.floor(ts/3600000) : normalizeId(t.id));
}
function reasonInfo(r) {
  const key = String(r||'').toLowerCase();
  const map = {
    tp0:['TP0 止盈','rb-profit'],tp1:['TP1 止盈','rb-profit'],tp2:['TP2 止盈','rb-profit'],
    tp_filled:['止盈成交','rb-profit'],partial_exit:['分批卖出','rb-profit'],
    stop_price:['价格止损','rb-risk'],stop_btc:['方向反转','rb-risk'],
    stop_time:['临近结算','rb-risk'],emergency_close:['紧急平仓','rb-risk'],
    trailing_stop:['移动止盈','rb-protect'],dead_water_exit:['死水退出','rb-protect'],
    no_start_exit:['无启动退出','rb-protect'],qr_breakeven_trail:['保本出场','rb-protect'],
    expired:['到期结算','rb-protect'],manual_close:['手动平仓','rb-protect']
  };
  return map[key]||[r||'--',''];
}
function renderReason(r) { const [l,c]=reasonInfo(r); return '<span class="rb '+c+'" title="'+esc(r||'')+'">'+l+'</span>'; }
function summarizeGroup(items) {
  const sorted = items.slice().sort((a,b)=>(a.exit_time||0)-(b.exit_time||0));
  const first = sorted[0]||{};
  const coin   = inferCoin(first);
  const shares = sorted.reduce((s,t)=>s+(t.shares||0),0);
  const cost   = sorted.reduce((s,t)=>s+(t.shares||0)*(t.entry_price||0),0);
  const pnl    = sorted.reduce((s,t)=>s+(t.pnl||0),0);
  const avgEntry = shares>0 ? cost/shares : (first.entry_price||0);
  const avgExit  = shares>0 ? sorted.reduce((s,t)=>s+(t.shares||0)*(t.exit_price||0),0)/shares : (first.exit_price||0);
  const start  = first.entry_time||sorted[0]?.exit_time||0;
  const end    = sorted[sorted.length-1]?.exit_time||start;
  const sides   = [...new Set(sorted.map(t=>t.side||'').filter(Boolean))];
  const reasons = [...new Set(sorted.map(t=>t.exit_reason||'').filter(Boolean))];
  return { coin, side:sides.length===1?sides[0]:'MIXED', entry_price:avgEntry, exit_price:avgExit, cost, pnl,
    entry_time:start, exit_time:end, hold_sec:Math.max(0,Math.floor((end-start)/1000)),
    exit_reason:reasons.length===1?reasons[0]:'partial_exit', items:sorted };
}
function groupTrades(records) {
  const map = new Map();
  for (const t of records||[]) { const k=periodKey(t); if (!map.has(k)) map.set(k,[]); map.get(k).push(t); }
  return Array.from(map.values()).map(summarizeGroup);
}
function filterByMode(recs) {
  if (currentFilter==='all') return Array.isArray(recs)?recs:[];
  return (Array.isArray(recs)?recs:[]).filter(t=>(t.mode||'dry_run')===currentFilter);
}
function selectScope(session, all) {
  return currentScope==='all' ? (Array.isArray(all)?all:(Array.isArray(session)?session:[])) : (Array.isArray(session)?session:[]);
}
async function fetchJSON(path) {
  try { const r=await fetch(path); return await r.json(); } catch(e) { return null; }
}

function renderCoinPnl(elId, records) {
  const el = document.getElementById(elId); if (!el) return;
  const groups = groupTrades(records||[]);
  const byCoin = new Map();
  for (const g of groups) {
    if (!byCoin.has(g.coin)) byCoin.set(g.coin,{pnl:0,n:0,wins:0,losses:0});
    const r = byCoin.get(g.coin);
    r.pnl+=g.pnl; r.n++;
    if (g.pnl>0) r.wins++; else if (g.pnl<0) r.losses++;
  }
  if (byCoin.size===0) { el.innerHTML='<div class="empty">No closed trades yet</div>'; return; }
  let html='<table><thead><tr><th>Coin</th><th>P&L</th><th>Entries</th><th>Win Rate</th><th>EV</th></tr></thead><tbody>';
  for (const [coin,s] of [...byCoin].sort((a,b)=>a[0].localeCompare(b[0]))) {
    const closed=s.wins+s.losses;
    html+='<tr><td style="font-weight:bold;color:#58a6ff;">'+esc(coin)+'</td>';
    html+='<td class="'+pnlCls(s.pnl)+'">'+fmtPnl(s.pnl)+'</td>';
    html+='<td>'+s.n+'</td>';
    html+='<td>'+(closed>0?(s.wins/closed*100).toFixed(1)+'%':'--')+'</td>';
    html+='<td class="'+pnlCls(s.pnl/s.n)+'">'+fmtPnl(s.pnl/s.n)+'</td></tr>';
  }
  el.innerHTML=html+'</tbody></table>';
}

function renderExperiment(prefix, expStatus, records) {
  const groups = groupTrades(records||[]);
  const wins=groups.filter(g=>g.pnl>0).length, losses=groups.filter(g=>g.pnl<0).length;
  const closed=wins+losses, pnl=groups.reduce((s,g)=>s+g.pnl,0);
  const wr=closed>0?(wins/closed*100).toFixed(1)+'%':'--';
  const summaryEl=document.getElementById(prefix+'-summary');
  if (summaryEl) summaryEl.textContent=wr+(expStatus?' · '+(expStatus.strategy||'exp'):'');
  if (expStatus) {
    document.getElementById(prefix+'-balance').textContent='$'+Number(expStatus.balance||0).toFixed(2);
    const pe=document.getElementById(prefix+'-pnl'); pe.textContent=fmtPnl(pnl); pe.className='stat-value '+pnlCls(pnl);
    document.getElementById(prefix+'-open').textContent=expStatus.open_positions||0;
    document.getElementById(prefix+'-trades-count').textContent=groups.length;
    const posEl=document.getElementById(prefix+'-positions');
    if (posEl) {
      const positions=expStatus.positions||[];
      if (!positions.length) { posEl.innerHTML=''; }
      else {
        let h='<table><thead><tr><th>Coin</th><th>Side</th><th>Entry</th><th>Current</th><th>MFE</th></tr></thead><tbody>';
        for (const p of positions) {
          h+='<tr><td style="color:#58a6ff;font-weight:bold;">'+esc(p.coin||'--')+'</td>';
          h+='<td class="side-'+String(p.side).toLowerCase()+'">'+p.side+'</td>';
          h+='<td>'+Number(p.entry_price||0).toFixed(3)+'</td><td>'+Number(p.current_price||0).toFixed(3)+'</td>';
          h+='<td class="positive">+'+Number(((p.max_price||0)-(p.entry_price||0))*100).toFixed(1)+'c</td></tr>';
        }
        posEl.innerHTML=h+'</tbody></table>';
      }
    }
  }
  renderCoinPnl(prefix+'-coin-pnl', records);
  const tradesEl=document.getElementById(prefix+'-trades');
  if (tradesEl) {
    const recent=groups.slice().sort((a,b)=>(b.exit_time||0)-(a.exit_time||0)).slice(0,30);
    if (!recent.length) { tradesEl.innerHTML='<div class="empty">No trades yet</div>'; return; }
    let h='<table><thead><tr><th>时间</th><th>Coin</th><th>方向</th><th>入场</th><th>出场</th><th>盈亏</th><th>退出原因</th></tr></thead><tbody>';
    for (const t of recent) {
      h+='<tr><td>'+fmtTime(t.exit_time||t.entry_time)+'</td>';
      h+='<td style="color:#58a6ff;font-weight:bold;">'+esc(t.coin)+'</td>';
      h+='<td class="side-'+String(t.side).toLowerCase()+'">'+esc(t.side)+'</td>';
      h+='<td>'+t.entry_price.toFixed(3)+'</td><td>'+t.exit_price.toFixed(3)+'</td>';
      h+='<td class="'+pnlCls(t.pnl)+'">'+fmtPnl(t.pnl)+'</td><td>'+renderReason(t.exit_reason)+'</td></tr>';
    }
    tradesEl.innerHTML=h+'</tbody></table>';
  }
}

async function refresh() {
  const [
    status, trades,
    tvStatus,  tvTrades,  tvAll,
    chStatus,  chTrades,  chAll,
    c4hStatus, c4hTrades, c4hAll,
    cdStatus,  cdTrades,  cdAll,
    finStatus, finTrades, finAll
  ] = await Promise.all([
    fetchJSON('/api/status'), fetchJSON('/api/trades'),
    fetchJSON('/api/experiment-trend-v2/status'),    fetchJSON('/api/experiment-trend-v2/trades'),    fetchJSON('/api/experiment-trend-v2/all-trades'),
    fetchJSON('/api/experiment-contrarian-hold/status'),fetchJSON('/api/experiment-contrarian-hold/trades'),fetchJSON('/api/experiment-contrarian-hold/all-trades'),
    fetchJSON('/api/experiment-crypto-4h/status'),   fetchJSON('/api/experiment-crypto-4h/trades'),   fetchJSON('/api/experiment-crypto-4h/all-trades'),
    fetchJSON('/api/experiment-crypto-daily/status'),fetchJSON('/api/experiment-crypto-daily/trades'),fetchJSON('/api/experiment-crypto-daily/all-trades'),
    fetchJSON('/api/experiment-finance/status'),     fetchJSON('/api/experiment-finance/trades'),     fetchJSON('/api/experiment-finance/all-trades'),
  ]);

  if (status) {
    const badge=document.getElementById('mode-badge');
    badge.textContent=status.mode==='live'?'LIVE':'DRY RUN';
    badge.className='mode '+(status.mode==='live'?'mode-live':'mode-dry');
    document.getElementById('uptime').textContent='Uptime: '+(status.uptime||'--')+' · Tick #'+(status.tick_count||0);
    document.getElementById('account-balance').textContent='$'+Number(status.account_balance||0).toFixed(2);
    document.getElementById('btc-price').textContent=fmtPrice(status.btc_price);
    document.getElementById('btc-price-bar').textContent=fmtPrice(status.btc_price);
    const devEl=document.getElementById('btc-dev');
    devEl.textContent=fmtPct(status.btc_deviation_pct); devEl.className=(status.btc_deviation_pct||0)>=0?'positive':'negative';
    document.getElementById('remaining').textContent=status.minutes_remaining??'--';
    const wsEl=document.getElementById('clob-ws');
    wsEl.textContent=status.clob_ws_connected?'on':'off'; wsEl.className=status.clob_ws_connected?'positive':'negative';
    const fmtP=v=>(v&&v>0)?v.toFixed(3):'--';
    document.getElementById('market-name').textContent=status.market_question||'(no active market)';
    document.getElementById('up-bid').textContent=fmtP(status.up_bid);
    document.getElementById('up-ask').textContent=fmtP(status.up_ask);
    document.getElementById('down-bid').textContent=fmtP(status.down_bid);
    document.getElementById('down-ask').textContent=fmtP(status.down_ask);
    const hintEl=document.getElementById('regime-hint');
    const min=status.minutes_remaining||0, absD=Math.abs(status.btc_deviation_pct||0);
    if (min<=0||min>55)                   { hintEl.textContent='⏳ 等待下一根 K 线'; hintEl.style.color='#484f58'; }
    else if (absD>=0.20&&min>=35&&min<45) { hintEl.textContent='▶ Momentum 入场窗口'; hintEl.style.color='#d29922'; }
    else if (absD<0.20&&min>=35&&min<45)  { hintEl.textContent='⛔ 偏移不足 0.20%'; hintEl.style.color='#7d8590'; }
    else                                   { hintEl.textContent='⏳ 不在入场时间窗口'; hintEl.style.color='#484f58'; }
    document.getElementById('open-pos').textContent=status.open_positions||0;
    const upnlEl=document.getElementById('unrealized-pnl');
    upnlEl.textContent=fmtPnl(status.unrealized_pnl||0); upnlEl.className=pnlCls(status.unrealized_pnl||0);
    const posSection=document.getElementById('open-positions-section');
    const posCont=document.getElementById('positions-container');
    if (status.positions&&status.positions.length>0) {
      let h='<table><thead><tr><th>Coin</th><th>方向</th><th>入场价</th><th>当前价</th><th>份数</th><th>UPnL</th><th>MFE</th><th>MAE</th></tr></thead><tbody>';
      for (const p of status.positions) {
        const upnl=(p.current_price-p.entry_price)*p.shares*p.remaining_pct;
        h+='<tr><td style="font-weight:bold;color:#58a6ff;">'+(p.coin||inferCoin(p))+'</td>';
        h+='<td class="side-'+p.side.toLowerCase()+'">'+p.side+'</td>';
        h+='<td>'+p.entry_price.toFixed(3)+'</td><td>'+p.current_price.toFixed(3)+'</td><td>'+p.shares.toFixed(0)+'</td>';
        h+='<td class="'+pnlCls(upnl)+'">'+fmtPnl(upnl)+'</td>';
        h+='<td class="positive">+'+(p.mfe*100).toFixed(1)+'c</td><td class="negative">-'+(p.mae*100).toFixed(1)+'c</td></tr>';
      }
      posCont.innerHTML=h+'</tbody></table>'; posSection.style.display='';
    } else { posSection.style.display='none'; }
  }

  const allTrades=Array.isArray(trades)?trades:[];
  const startTime=status?Number(status.start_time||0):0;
  const sessionTrades=allTrades.filter(t=>{ const ts=Number(t.exit_time||t.entry_time||0); return !startTime||!ts||ts>=startTime; });
  const scoped=filterByMode(selectScope(sessionTrades,allTrades));
  const groups=groupTrades(scoped).sort((a,b)=>(b.exit_time||0)-(a.exit_time||0));
  const wins=groups.filter(g=>g.pnl>0).length, losses=groups.filter(g=>g.pnl<0).length;
  const closed=wins+losses, totalPnl=groups.reduce((s,g)=>s+g.pnl,0);
  const winRate=closed>0?wins/closed*100:0;
  const wrEl=document.getElementById('win-rate');
  wrEl.textContent=closed>0?winRate.toFixed(1)+'%':'--%';
  wrEl.className='metric-value '+(closed===0?'neutral':winRate>=50?'positive':'negative');
  document.getElementById('win-loss').textContent=wins+'W / '+losses+'L';
  const pnlEl=document.getElementById('total-pnl');
  pnlEl.textContent=fmtPnl(totalPnl); pnlEl.className='metric-value '+pnlCls(totalPnl);
  document.getElementById('pnl-sub').textContent=groups.length+' trades · EV '+(groups.length>0?fmtPnl(totalPnl/groups.length):'--');
  renderCoinPnl('main-coin-pnl', scoped);
  document.getElementById('th-count').textContent=groups.length+' entries · '+scoped.length+' exits';
  const tbody=document.getElementById('trades-body');
  if (!groups.length) { tbody.innerHTML='<tr><td colspan="9" class="empty">No trades yet</td></tr>'; }
  else {
    let h='';
    for (const t of groups) {
      h+='<tr><td>'+fmtTime(t.exit_time||t.entry_time)+'</td>';
      h+='<td style="color:#58a6ff;font-weight:bold;">'+esc(t.coin)+'</td>';
      h+='<td class="side-'+String(t.side).toLowerCase()+'">'+esc(t.side)+'</td>';
      h+='<td>'+t.entry_price.toFixed(3)+'</td><td>'+t.exit_price.toFixed(3)+'</td>';
      h+='<td>$'+t.cost.toFixed(2)+'</td>';
      h+='<td class="'+pnlCls(t.pnl)+'">'+fmtPnl(t.pnl)+'</td>';
      h+='<td>'+renderReason(t.exit_reason)+'</td><td>'+fmtDur(t.hold_sec)+'</td></tr>';
      if (t.items.length>1) {
        h+='<tr class="split-row"><td colspan="9"><details class="split-det"><summary>展开 '+t.items.length+' 笔分批卖出</summary>';
        h+='<table><thead><tr><th>时间</th><th>入场</th><th>出场</th><th>份数</th><th>盈亏</th><th>原因</th></tr></thead><tbody>';
        for (const p of t.items) {
          h+='<tr><td>'+fmtTime(p.exit_time||p.entry_time)+'</td>';
          h+='<td>'+Number(p.entry_price||0).toFixed(3)+'</td><td>'+Number(p.exit_price||0).toFixed(3)+'</td>';
          h+='<td>'+Number(p.shares||0).toFixed(1)+'</td>';
          h+='<td class="'+pnlCls(p.pnl||0)+'">'+fmtPnl(p.pnl||0)+'</td><td>'+renderReason(p.exit_reason)+'</td></tr>';
        }
        h+='</tbody></table></details></td></tr>';
      }
    }
    tbody.innerHTML=h;
  }

  renderExperiment('trend-v2-exp',     tvStatus,  selectScope(Array.isArray(tvTrades)?tvTrades:[],  Array.isArray(tvAll)?tvAll:tvTrades));
  renderExperiment('contra-hold-exp',  chStatus,  selectScope(Array.isArray(chTrades)?chTrades:[],  Array.isArray(chAll)?chAll:chTrades));
  renderExperiment('crypto-4h-exp',    c4hStatus, selectScope(Array.isArray(c4hTrades)?c4hTrades:[], Array.isArray(c4hAll)?c4hAll:c4hTrades));
  renderExperiment('crypto-daily-exp', cdStatus,  selectScope(Array.isArray(cdTrades)?cdTrades:[],  Array.isArray(cdAll)?cdAll:cdTrades));
  renderExperiment('finance-exp',      finStatus, selectScope(Array.isArray(finTrades)?finTrades:[], Array.isArray(finAll)?finAll:finTrades));
}

setInterval(refresh, REFRESH_MS);
refresh();
</script>
</body>
</html>
)html";

}  // namespace polymarket
