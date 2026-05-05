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
  body { font-family: 'SF Mono', 'Menlo', 'Consolas', monospace; background: #0a0e17; color: #e0e6ed; padding: 20px; }
  h1 { font-size: 1.3em; color: #58a6ff; margin-bottom: 8px; }
  .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; border-bottom: 1px solid #1e2d3d; padding-bottom: 12px; }
  .mode { padding: 4px 12px; border-radius: 4px; font-size: 0.85em; font-weight: bold; }
  .mode-dry { background: #1a3a2a; color: #3fb950; }
  .mode-live { background: #3a1a1a; color: #f85149; }
  .grid { display: grid; grid-template-columns: 1fr 1fr 1fr 1fr; gap: 16px; margin-bottom: 20px; }
  .card { background: #111827; border: 1px solid #1e2d3d; border-radius: 8px; padding: 16px; }
  .card-title { font-size: 0.75em; color: #7d8590; text-transform: uppercase; letter-spacing: 1px; margin-bottom: 8px; }
  .card-value { font-size: 1.8em; font-weight: bold; }
  .positive { color: #3fb950; }
  .negative { color: #f85149; }
  .neutral { color: #58a6ff; }
  .stats-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 12px; margin-bottom: 20px; }
  .stat { background: #111827; border: 1px solid #1e2d3d; border-radius: 6px; padding: 12px; text-align: center; }
  .stat-label { font-size: 0.7em; color: #7d8590; margin-bottom: 4px; }
  .stat-value { font-size: 1.2em; font-weight: bold; }
  table { width: 100%; border-collapse: collapse; font-size: 0.85em; }
  th { text-align: left; padding: 8px 12px; color: #7d8590; font-size: 0.75em; text-transform: uppercase; letter-spacing: 1px; border-bottom: 1px solid #1e2d3d; }
  td { padding: 8px 12px; border-bottom: 1px solid #0d1117; }
  tr:hover { background: #161b22; }
  .side-up { color: #3fb950; font-weight: bold; }
  .side-down { color: #f85149; font-weight: bold; }
  .section { margin-bottom: 20px; }
  .section-title { font-size: 0.9em; color: #58a6ff; margin-bottom: 10px; padding-bottom: 6px; border-bottom: 1px solid #1e2d3d; }
  details > summary::-webkit-details-marker { display: none; }
  details[open] > summary > span:first-child::before { content: '▾ '; }
  details:not([open]) > summary > span:first-child::before { content: '▸ '; }
  .positions-empty { color: #7d8590; padding: 20px; text-align: center; }
  .summary-strip { font-size:0.75em; color:#7d8590; margin-bottom:16px; display:flex; flex-wrap:wrap; gap:18px; align-items:center; }
  .reason-badge { display:inline-block; padding:2px 8px; border-radius:4px; background:#1e2d3d; color:#e0e6ed; font-size:0.9em; white-space:nowrap; }
  .reason-profit { background:#14351f; color:#56d364; }
  .reason-risk { background:#3a1a1a; color:#f85149; }
  .reason-protect { background:#332b13; color:#d29922; }
  .trade-detail-row td { background:#0d1117; padding:0 12px 12px 12px; }
  .trade-detail { margin-top:6px; border:1px solid #1e2d3d; border-radius:6px; overflow:hidden; }
  .trade-detail summary { cursor:pointer; color:#58a6ff; padding:8px 10px; list-style:none; user-select:none; }
  .trade-detail summary::-webkit-details-marker { display:none; }
  .trade-detail[open] summary { border-bottom:1px solid #1e2d3d; }
  .uptime { font-size: 0.8em; color: #7d8590; }
  .refresh { font-size: 0.7em; color: #484f58; }
  .pnl-bar { display: flex; align-items: center; gap: 8px; margin-top: 4px; }
  .pnl-bar-fill { height: 4px; border-radius: 2px; min-width: 2px; }

  /* === P0/P1/P2 紧凑布局 === */
  /* metric-row: 顶部一排 8 个紧凑 metric（替代原 grid + stats-grid 双层）*/
  .metric-row { display: grid; grid-template-columns: repeat(8, 1fr); gap: 1px; background: #1e2d3d; border: 1px solid #1e2d3d; border-radius: 8px; margin-bottom: 16px; overflow: hidden; }
  .metric { background: #111827; padding: 10px 14px; min-height: 64px; display: flex; flex-direction: column; justify-content: center; }
  .metric-label { font-size: 0.65em; color: #7d8590; text-transform: uppercase; letter-spacing: 0.8px; margin-bottom: 4px; }
  .metric-value { font-size: 1.15em; font-weight: bold; line-height: 1.1; }
  .metric-sub { font-size: 0.65em; color: #7d8590; margin-top: 2px; }
  .metric-spark { margin-top: 4px; height: 16px; }
  /* 嵌套 details（Analytics 内子折叠）*/
  details details { margin-top: 12px; }
  details details > summary { padding: 6px 0; font-size: 0.85em; color: #58a6ff; cursor: pointer; list-style: none; user-select: none; }
  /* 响应式：窄屏 metric-row 折叠成 2 行 */
  @media (max-width: 900px) {
    .metric-row { grid-template-columns: repeat(4, 1fr); }
  }
  @media (max-width: 500px) {
    .metric-row { grid-template-columns: repeat(2, 1fr); }
  }
</style>
</head>
<body>
<div class="header">
  <div>
    <h1>Polymarket BTC 1h Bot</h1>
    <span class="uptime" id="uptime">--</span>
  </div>
  <div style="display:flex;align-items:center;gap:12px;">
    <span class="mode" id="mode-badge">--</span>
    <span class="refresh" id="refresh-info">auto 5s</span>
    <button id="stop-bot-btn" title="优雅停止 bot（触发 emergency_close_all 兜底）"
      style="cursor:pointer;padding:5px 12px;border:1px solid #6e1a1a;background:#3a1a1a;color:#f85149;border-radius:4px;font-size:0.8em;font-weight:bold;">
      ⏹ Stop Bot
    </button>
  </div>
</div>

<!-- 顶部 8 metric 紧凑一排（P0-1 + P2-5）-->
<div class="metric-row">
  <div class="metric">
    <div class="metric-label">Balance</div>
    <div class="metric-value neutral" id="account-balance">--</div>
    <div class="metric-sub">BTC <span id="btc-price">--</span></div>
  </div>
  <div class="metric">
    <div class="metric-label">Position Cost</div>
    <div class="metric-value neutral" id="total-cost">$0.00</div>
    <div class="metric-sub">UPnL <span id="unrealized-pnl">$0.00</span></div>
  </div>
  <div class="metric">
    <div class="metric-label">Realized P&L</div>
    <div class="metric-value" id="total-pnl">$0.00</div>
    <div class="metric-sub">Daily <span id="daily-pnl">$0.00</span></div>
    <svg class="metric-spark" id="pnl-spark" width="100%" height="16" preserveAspectRatio="none" viewBox="0 0 100 16"></svg>
  </div>
  <div class="metric">
    <div class="metric-label">Win Rate</div>
    <div class="metric-value neutral" id="win-rate">--%</div>
    <div class="metric-sub"><span id="win-loss">0W/0L</span> · <span id="total-trades">0</span></div>
  </div>
  <div class="metric">
    <div class="metric-label">Open</div>
    <div class="metric-value neutral" id="open-pos">0</div>
    <div class="metric-sub">positions</div>
  </div>
  <div class="metric">
    <div class="metric-label">Volatility</div>
    <div class="metric-value" id="volatility">--</div>
    <div class="metric-sub">BTC 1h</div>
  </div>
  <div class="metric">
    <div class="metric-label">Remaining</div>
    <div class="metric-value" id="remaining">--</div>
    <div class="metric-sub">candle</div>
  </div>
  <div class="metric">
    <div class="metric-label">Cons Losses</div>
    <div class="metric-value" id="consec-losses">0</div>
    <div class="metric-sub">streak</div>
  </div>
</div>

<!-- 主市场快照：只保留主交易市场的快速状态，多币种明细在 Markets 表 -->
<div class="summary-strip">
  <span style="color:#484f58;">Primary</span>
  <span id="market-name" style="color:#58a6ff;font-weight:bold;">--</span>
  <span>BTC dev <span id="btc-dev">--</span></span>
  <span>UP <span class="positive" id="up-bid">--</span>/<span class="positive" id="up-ask">--</span></span>
  <span>DN <span class="negative" id="down-bid">--</span>/<span class="negative" id="down-ask">--</span></span>
  <span>WS <span id="clob-ws">--</span></span>
  <span style="color:#484f58;font-size:0.85em;">(bid/ask)</span>
</div>

<div class="section">
  <div class="section-title">Markets</div>
  <div id="markets-container"></div>
</div>

<!-- Open Positions section 仅在有持仓时显示（P1-4）-->
<div class="section" id="open-positions-section" style="display:none;">
  <div class="section-title">Open Positions</div>
  <div id="positions-container"></div>
</div>

<details class="section" id="trade-history-details">
  <summary class="section-title" style="display:flex;align-items:center;gap:12px;cursor:pointer;list-style:none;">
    <span style="user-select:none;">Trade History</span>
    <span id="th-count" style="font-size:0.75em;font-weight:normal;color:#7d8590;">--</span>
    <span style="font-size:0.75em;font-weight:normal;color:#7d8590;margin-left:auto;">filter:</span>
    <button class="mode-filter" data-filter="all"     style="cursor:pointer;padding:2px 10px;border:1px solid #30363d;background:#21262d;color:#c9d1d9;border-radius:4px;" onclick="event.stopPropagation();">All</button>
    <button class="mode-filter" data-filter="live"    style="cursor:pointer;padding:2px 10px;border:1px solid #30363d;background:#21262d;color:#c9d1d9;border-radius:4px;" onclick="event.stopPropagation();">Live</button>
    <button class="mode-filter" data-filter="dry_run" style="cursor:pointer;padding:2px 10px;border:1px solid #30363d;background:#21262d;color:#c9d1d9;border-radius:4px;" onclick="event.stopPropagation();">Dry</button>
  </summary>
  <table>
    <thead>
      <tr>
        <th>Time</th>
        <th>Coin</th>
        <th>Period</th>
        <th>Side</th>
        <th>Entry</th>
        <th>Exit</th>
        <th>Shares</th>
        <th>Cost</th>
        <th>Revenue</th>
        <th>Fee</th>
        <th>Reason</th>
        <th>P&L</th>
        <th>MFE</th>
        <th>MAE</th>
        <th>Duration</th>
      </tr>
    </thead>
    <tbody id="trades-body">
      <tr><td colspan="15" style="text-align:center;color:#7d8590;padding:20px;">No trades yet</td></tr>
    </tbody>
  </table>
</details>

<!-- Analytics 整块默认折叠（P0-2），内部分 4 个子 details（P1-3）-->
<details class="section" id="analytics-details">
  <summary class="section-title" style="cursor:pointer;list-style:none;"><span>Analytics</span></summary>

  <details id="ad-perf">
    <summary><span>Performance &amp; MFE/MAE</span></summary>
    <div class="stats-grid" id="analytics-cards" style="margin-top:8px;">
      <div class="stat"><div class="stat-label">Avg MFE</div><div class="stat-value positive" id="a-avg-mfe">--</div></div>
      <div class="stat"><div class="stat-label">Avg MAE</div><div class="stat-value negative" id="a-avg-mae">--</div></div>
      <div class="stat"><div class="stat-label">MFE:MAE Ratio</div><div class="stat-value neutral" id="a-ratio">--</div></div>
      <div class="stat"><div class="stat-label">Max MFE</div><div class="stat-value positive" id="a-max-mfe">--</div></div>
    </div>
    <div class="stats-grid" id="analytics-duration">
      <div class="stat"><div class="stat-label">Avg Hold</div><div class="stat-value" id="a-avg-dur">--</div></div>
      <div class="stat"><div class="stat-label">Min Hold</div><div class="stat-value" id="a-min-dur">--</div></div>
      <div class="stat"><div class="stat-label">Max Hold</div><div class="stat-value" id="a-max-dur">--</div></div>
      <div class="stat"><div class="stat-label">Spread (W vs L)</div><div class="stat-value" id="a-spread">--</div></div>
    </div>
    <div class="stats-grid" id="analytics-core">
      <div class="stat"><div class="stat-label">Profit Factor</div><div class="stat-value neutral" id="a-pf">--</div></div>
      <div class="stat"><div class="stat-label">EV / Trade</div><div class="stat-value" id="a-ev">--</div></div>
      <div class="stat"><div class="stat-label">Avg Win / Avg Loss</div><div class="stat-value" id="a-wl">--</div></div>
      <div class="stat"><div class="stat-label">Max Drawdown</div><div class="stat-value negative" id="a-dd">--</div></div>
    </div>
    <div class="stats-grid" id="analytics-capture">
      <div class="stat"><div class="stat-label">MFE Capture (All)</div><div class="stat-value neutral" id="a-cap-all">--</div></div>
      <div class="stat"><div class="stat-label">MFE Capture (Win)</div><div class="stat-value positive" id="a-cap-win">--</div></div>
      <div class="stat"><div class="stat-label">MFE Capture (Loss)</div><div class="stat-value negative" id="a-cap-lose">--</div></div>
      <div class="stat"><div class="stat-label">Trailing Stop Avg</div><div class="stat-value" id="a-ts-avg">--</div></div>
    </div>
  </details>

  <details id="ad-distribution">
    <summary><span>Time &amp; Distribution</span></summary>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:8px;margin-bottom:12px;">
      <div class="card">
        <div class="card-title">P&L by Hour (ET)</div>
        <div id="hour-heatmap" style="display:grid;grid-template-columns:repeat(6,1fr);gap:4px;margin-top:8px;"></div>
      </div>
      <div class="card">
        <div class="card-title">Exit Reason Distribution</div>
        <div id="exit-reasons" style="margin-top:8px;"></div>
      </div>
    </div>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:16px;">
      <div class="card">
        <div class="card-title">P&L by Day of Week</div>
        <div id="day-of-week" style="display:grid;grid-template-columns:repeat(7,1fr);gap:4px;margin-top:8px;"></div>
      </div>
      <div class="card">
        <div class="card-title">Streak Analysis</div>
        <div id="streak-analysis" style="margin-top:8px;font-size:0.85em;"></div>
      </div>
    </div>
  </details>

  <details id="ad-direction">
    <summary><span>Direction, TP &amp; Equity</span></summary>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:8px;margin-bottom:12px;">
      <div class="card">
        <div class="card-title">Direction Analysis (Candle Level)</div>
        <div id="direction-analysis" style="margin-top:8px;"></div>
      </div>
      <div class="card">
        <div class="card-title">TP Hit Rates (% of candles)</div>
        <div id="tp-hit-rates" style="margin-top:8px;"></div>
      </div>
    </div>
    <div class="card">
      <div class="card-title">Equity Curve</div>
      <div id="equity-curve" style="margin-top:8px;height:160px;position:relative;"></div>
    </div>
  </details>

  <details id="ad-buckets">
    <summary><span>P&amp;L Buckets</span></summary>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:8px;margin-bottom:12px;">
      <div class="card">
        <div class="card-title">P&L by Entry Price</div>
        <div id="entry-price-buckets" style="margin-top:8px;"></div>
      </div>
      <div class="card">
        <div class="card-title">P&L by BTC Deviation</div>
        <div id="btc-dev-buckets" style="margin-top:8px;"></div>
      </div>
    </div>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:16px;">
      <div class="card">
        <div class="card-title">P&L by Hold Duration</div>
        <div id="duration-buckets" style="margin-top:8px;"></div>
      </div>
      <div class="card">
        <div class="card-title">Trailing Stop vs Price Stop</div>
        <div id="trailing-vs-stop" style="margin-top:8px;font-size:0.85em;"></div>
      </div>
    </div>
  </details>
</details>

<details class="section" id="experiment-details">
  <summary class="section-title" style="cursor:pointer;list-style:none;">
    <span>Experiment</span>
    <span id="exp-summary" style="font-size:0.75em;font-weight:normal;color:#7d8590;margin-left:10px;">--</span>
  </summary>
  <div class="stats-grid" style="margin-top:8px;">
    <div class="stat"><div class="stat-label">Balance</div><div class="stat-value" id="exp-balance">--</div></div>
    <div class="stat"><div class="stat-label">P&L</div><div class="stat-value" id="exp-pnl">--</div></div>
    <div class="stat"><div class="stat-label">Open</div><div class="stat-value" id="exp-open">--</div></div>
    <div class="stat"><div class="stat-label">Trades</div><div class="stat-value" id="exp-trades-count">--</div></div>
  </div>
  <div id="exp-regime-stats" style="margin-top:12px;"></div>
  <div id="exp-positions" style="margin-top:12px;"></div>
  <div id="exp-trades" style="margin-top:12px;"></div>
</details>

<details class="section" id="trend-experiment-details">
  <summary class="section-title" style="cursor:pointer;list-style:none;">
    <span>Trend Experiment</span>
    <span id="trend-exp-summary" style="font-size:0.75em;font-weight:normal;color:#7d8590;margin-left:10px;">--</span>
  </summary>
  <div class="stats-grid" style="margin-top:8px;">
    <div class="stat"><div class="stat-label">Balance</div><div class="stat-value" id="trend-exp-balance">--</div></div>
    <div class="stat"><div class="stat-label">P&L</div><div class="stat-value" id="trend-exp-pnl">--</div></div>
    <div class="stat"><div class="stat-label">Open</div><div class="stat-value" id="trend-exp-open">--</div></div>
    <div class="stat"><div class="stat-label">Trades</div><div class="stat-value" id="trend-exp-trades-count">--</div></div>
  </div>
  <div id="trend-exp-regime-stats" style="margin-top:12px;"></div>
  <div id="trend-exp-positions" style="margin-top:12px;"></div>
  <div id="trend-exp-trades" style="margin-top:12px;"></div>
</details>

<script>
const API_BASE = '';
const REFRESH_MS = 5000;
// 持久化用户偏好（filter / 折叠状态）
let currentFilter = localStorage.getItem('dashboard.filter') || 'all';

document.addEventListener('DOMContentLoaded', () => {
  const buttons = document.querySelectorAll('.mode-filter');
  function applyActive(active) {
    buttons.forEach(b => {
      const on = b === active;
      b.style.background = on ? '#388bfd' : '#21262d';
      b.style.color      = on ? '#ffffff' : '#c9d1d9';
    });
  }
  buttons.forEach(btn => {
    if (btn.dataset.filter === currentFilter) applyActive(btn);  // 恢复上次选择
    btn.addEventListener('click', () => {
      currentFilter = btn.dataset.filter;
      localStorage.setItem('dashboard.filter', currentFilter);
      applyActive(btn);
      if (typeof refresh === 'function') refresh();
    });
  });

  // 折叠状态恢复（多个 details）
  const persistDetails = (id, defaultOpen=false) => {
    const el = document.getElementById(id);
    if (!el) return;
    const stored = localStorage.getItem('dashboard.' + id + '.open');
    el.open = stored === null ? defaultOpen : stored === '1';
    el.addEventListener('toggle', () => {
      localStorage.setItem('dashboard.' + id + '.open', el.open ? '1' : '0');
    });
  };
  persistDetails('trade-history-details', true);   // 主要内容，默认展开
  persistDetails('analytics-details',     false);
  persistDetails('ad-perf',               false);
  persistDetails('ad-distribution',       false);
  persistDetails('ad-direction',          false);
  persistDetails('ad-buckets',            false);
  persistDetails('experiment-details',    false);
  persistDetails('trend-experiment-details', false);

  // Stop Bot 按钮
  const stopBtn = document.getElementById('stop-bot-btn');
  if (stopBtn) {
    stopBtn.addEventListener('click', async () => {
      if (!confirm('确认停止 bot？\n\n会触发 emergency_close_all：\n· cancel 所有未平仓订单\n· SELL FOK 所有持仓平仓\n\n之后程序退出。')) return;
      stopBtn.disabled = true;
      stopBtn.textContent = '⏳ Stopping...';
      try {
        const res = await fetch('/api/shutdown', { method: 'POST' });
        const data = await res.json();
        stopBtn.textContent = '✓ ' + (data.message || 'Stopped');
        stopBtn.style.background = '#1a3a2a';
        stopBtn.style.color = '#3fb950';
      } catch (e) {
        stopBtn.textContent = '✗ Failed';
        alert('Stop failed: ' + e.message);
        stopBtn.disabled = false;
      }
    });
  }
});

function formatPrice(v) { return v ? '$' + Number(v).toLocaleString('en-US', {minimumFractionDigits:2, maximumFractionDigits:2}) : '--'; }
function formatPct(v) { return v !== undefined ? (v >= 0 ? '+' : '') + v.toFixed(2) + '%' : '--'; }
function formatPnl(v) { return (v >= 0 ? '+$' : '-$') + Math.abs(v).toFixed(2); }
function pnlClass(v) { return v > 0 ? 'positive' : v < 0 ? 'negative' : 'neutral'; }
function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }[c]));
}
function inferCoin(t) {
  if (t.coin) return String(t.coin).toUpperCase();
  const m = (t.market || '').toLowerCase();
  if (m.includes('ethereum')) return 'ETH';
  if (m.includes('solana')) return 'SOL';
  if (m.includes('xrp')) return 'XRP';
  if (m.includes('dogecoin')) return 'DOGE';
  if (m.includes('bnb')) return 'BNB';
  if (m.includes('hype')) return 'HYPE';
  return 'BTC';
}
function normalizeTradeId(id) {
  return String(id || '--').replace(/-TP\d+$/i, '');
}
function tradePeriodKey(t) {
  const coin = inferCoin(t);
  const market = String(t.market || '').trim();
  if (market) return coin + '|' + market;
  const ts = t.entry_time || t.exit_time || 0;
  const bucket = ts ? Math.floor(ts / 3600000) : normalizeTradeId(t.id);
  return coin + '|hour|' + bucket;
}
function shortMarketLabel(market, fallbackTime) {
  const text = String(market || '').trim();
  if (!text) return fallbackTime ? formatTime(fallbackTime) : '--';
  const cleaned = text
    .replace(/^will\s+/i, '')
    .replace(/\s+be\s+above\s+/i, ' > ')
    .replace(/\s+on\s+/i, ' ')
    .replace(/\?/g, '');
  return cleaned.length > 38 ? cleaned.slice(0, 35) + '...' : cleaned;
}
function reasonInfo(reason) {
  const r = String(reason || '').toLowerCase();
  const map = {
    tp0: ['第一档止盈', 'reason-profit'],
    tp1: ['第二档止盈', 'reason-profit'],
    tp2: ['最终止盈', 'reason-profit'],
    tp_filled: ['止盈成交', 'reason-profit'],
    stop_price: ['价格止损', 'reason-risk'],
    stop_btc: ['方向反转止损', 'reason-risk'],
    stop_time: ['临近结算撤退', 'reason-risk'],
    expired: ['到期结算', 'reason-protect'],
    trailing_stop: ['移动止盈回撤', 'reason-protect'],
    dead_water_exit: ['长时间无利润退出', 'reason-protect'],
    emergency_close: ['紧急平仓', 'reason-risk'],
    manual_close: ['手动平仓', 'reason-protect'],
    partial_exit: ['分批卖出', 'reason-profit']
  };
  return map[r] || [reason || '--', ''];
}
function renderReason(reason) {
  const [label, cls] = reasonInfo(reason);
  return '<span class="reason-badge ' + cls + '" title="' + (reason || '') + '">' + label + '</span>';
}
function summarizeTradeGroup(items) {
  const sorted = items.slice().sort((a, b) => (a.exit_time || 0) - (b.exit_time || 0));
  const first = sorted[0] || {};
  const coin = inferCoin(first);
  const shares = sorted.reduce((s, t) => s + (t.shares || 0), 0);
  const cost = sorted.reduce((s, t) => s + ((t.shares || 0) * (t.entry_price || 0)), 0);
  const revenue = sorted.reduce((s, t) => s + ((t.shares || 0) * (t.exit_price || 0)), 0);
  const fee = sorted.reduce((s, t) => s + (t.fee || 0), 0);
  const pnl = sorted.reduce((s, t) => s + (t.pnl || 0), 0);
  const avgEntry = shares > 0 ? cost / shares : (first.entry_price || 0);
  const avgExit = shares > 0 ? revenue / shares : (first.exit_price || 0);
  const maxPrice = Math.max(...sorted.map(t => t.max_price || t.entry_price || 0));
  const minPrice = Math.min(...sorted.map(t => t.min_price || t.entry_price || 0));
  const start = first.entry_time || sorted[0]?.exit_time || 0;
  const end = sorted[sorted.length - 1]?.exit_time || start;
  const reasons = [...new Set(sorted.map(t => t.exit_reason || '').filter(Boolean))];
  const sides = [...new Set(sorted.map(t => t.side || '').filter(Boolean))];
  const market = first.market || '';
  return {
    id: normalizeTradeId(first.id),
    label: sorted.length > 1
      ? coin + ' · ' + shortMarketLabel(market, start) + ' · ' + sorted.length + ' sells'
      : normalizeTradeId(first.id),
    coin,
    side: sides.length === 1 ? sides[0] : 'MIXED',
    market,
    entry_price: avgEntry,
    exit_price: avgExit,
    shares,
    cost,
    revenue,
    fee,
    pnl,
    max_price: maxPrice,
    min_price: minPrice,
    entry_time: start,
    exit_time: end,
    hold_duration_sec: Math.max(0, Math.floor((end - start) / 1000)),
    exit_reason: reasons.length === 1 ? reasons[0] : 'partial_exit',
    items: sorted
  };
}
function formatTime(ms) {
  if (!ms) return '--';
  const d = new Date(ms);
  return d.toLocaleTimeString('zh-CN', {hour:'2-digit',minute:'2-digit',second:'2-digit'});
}

async function fetchJSON(path) {
  try { const r = await fetch(API_BASE + path); return await r.json(); }
  catch(e) { return null; }
}

function fmtDur(s) { return Math.floor(s/60) + 'm' + (s%60) + 's'; }

// mini sparkline: 给 SVG 元素填一条 polyline，最后点决定颜色（正绿负红）
function renderSparkline(svgId, values) {
  const svg = document.getElementById(svgId);
  if (!svg) return;
  if (!values || values.length < 2) { svg.innerHTML = ''; return; }
  const min = Math.min(...values), max = Math.max(...values);
  const range = max - min || 1;
  const w = 100, h = 16;
  let path = '';
  for (let i = 0; i < values.length; i++) {
    const x = (i / (values.length - 1)) * w;
    const y = h - ((values[i] - min) / range) * h;
    path += (i === 0 ? 'M' : 'L') + x.toFixed(1) + ',' + y.toFixed(1) + ' ';
  }
  const last = values[values.length - 1];
  const color = last >= 0 ? '#3fb950' : '#f85149';
  svg.innerHTML = '<path d="' + path.trim() + '" stroke="' + color + '" stroke-width="1.4" fill="none"/>';
}

function renderExperiment(prefix, expStatus, expTrades) {
  if (expStatus) {
    const expSummary = document.getElementById(prefix + '-summary');
    if (expSummary) expSummary.textContent = expStatus.enabled ? (expStatus.strategy || 'regime') : 'disabled';
    document.getElementById(prefix + '-balance').textContent = '$' + Number(expStatus.balance || 0).toFixed(2);
    const expPnl = document.getElementById(prefix + '-pnl');
    expPnl.textContent = formatPnl(expStatus.realized_pnl || 0);
    expPnl.className = 'stat-value ' + pnlClass(expStatus.realized_pnl || 0);
    document.getElementById(prefix + '-open').textContent = expStatus.open_positions || 0;
    document.getElementById(prefix + '-trades-count').textContent = expStatus.total_trades || 0;

    const regimeEl = document.getElementById(prefix + '-regime-stats');
    if (regimeEl) {
      let html = '<table><thead><tr><th>Regime</th><th>Trades</th><th>Win Rate</th><th>P&L</th></tr></thead><tbody>';
      const stats = expStatus.regime_stats || {};
      const keys = Object.keys(stats);
      if (keys.length === 0) {
        html += '<tr><td colspan="4" style="text-align:center;color:#7d8590;">No experiment trades yet</td></tr>';
      } else {
        for (const k of keys) {
          const s = stats[k] || {};
          html += '<tr><td>' + k + '</td><td>' + (s.trades || 0) + '</td><td>' + Number(s.win_rate || 0).toFixed(1) + '%</td><td class="' + pnlClass(s.pnl || 0) + '">' + formatPnl(s.pnl || 0) + '</td></tr>';
        }
      }
      html += '</tbody></table>';
      regimeEl.innerHTML = html;
    }

    const expPosEl = document.getElementById(prefix + '-positions');
    if (expPosEl) {
      const positions = expStatus.positions || [];
      if (positions.length === 0) {
        expPosEl.innerHTML = '<div class="positions-empty">No experiment positions</div>';
      } else {
        let html = '<table><thead><tr><th>ID</th><th>Coin</th><th>Regime</th><th>Side</th><th>Entry</th><th>Current</th><th>Remaining</th><th>MFE</th></tr></thead><tbody>';
        for (const p of positions) {
          html += '<tr><td>' + p.id + '</td><td>' + p.coin + '</td><td>' + p.regime + '</td><td class="side-' + String(p.side).toLowerCase() + '">' + p.side + '</td><td>' + Number(p.entry_price || 0).toFixed(3) + '</td><td>' + Number(p.current_price || 0).toFixed(3) + '</td><td>' + Number((p.remaining_pct || 0) * 100).toFixed(0) + '%</td><td class="positive">+' + Number(((p.max_price || 0) - (p.entry_price || 0)) * 100).toFixed(1) + 'c</td></tr>';
        }
        html += '</tbody></table>';
        expPosEl.innerHTML = html;
      }
    }
  }

  if (expTrades) {
    const expTradesEl = document.getElementById(prefix + '-trades');
    if (expTradesEl) {
      let html = '<table><thead><tr><th>Time</th><th>Coin</th><th>Regime</th><th>Side</th><th>Entry</th><th>Exit</th><th>Reason</th><th>P&L</th></tr></thead><tbody>';
      if (expTrades.length === 0) {
        html += '<tr><td colspan="8" style="text-align:center;color:#7d8590;">No experiment trades yet</td></tr>';
      } else {
        for (const t of expTrades.slice().reverse().slice(0, 30)) {
          html += '<tr><td>' + formatTime(t.exit_time || t.entry_time) + '</td><td>' + (t.coin || '--') + '</td><td>' + (t.regime || '') + '</td><td class="side-' + String(t.side).toLowerCase() + '">' + t.side + '</td><td>' + Number(t.entry_price || 0).toFixed(3) + '</td><td>' + Number(t.exit_price || 0).toFixed(3) + '</td><td>' + renderReason(t.exit_reason) + '</td><td class="' + pnlClass(t.pnl || 0) + '">' + formatPnl(t.pnl || 0) + '</td></tr>';
        }
      }
      html += '</tbody></table>';
      expTradesEl.innerHTML = html;
    }
  }
}

async function refresh() {
  let [status, trades, stats, analytics, expStatus, expTrades, trendExpStatus, trendExpTrades] = await Promise.all([
    fetchJSON('/api/status'),
    fetchJSON('/api/trades'),
    fetchJSON('/api/stats'),
    fetchJSON('/api/analytics'),
    fetchJSON('/api/experiment/status'),
    fetchJSON('/api/experiment/trades'),
    fetchJSON('/api/experiment-trend/status'),
    fetchJSON('/api/experiment-trend/trades')
  ]);

  if (status) {
    const badge = document.getElementById('mode-badge');
    badge.textContent = status.mode === 'live' ? 'LIVE' : 'DRY RUN';
    badge.className = 'mode ' + (status.mode === 'live' ? 'mode-live' : 'mode-dry');

    document.getElementById('account-balance').textContent = '$' + status.account_balance.toFixed(2);
    document.getElementById('btc-price').textContent = formatPrice(status.btc_price);
    const devEl = document.getElementById('btc-dev');
    devEl.textContent = formatPct(status.btc_deviation_pct);
    devEl.className = status.btc_deviation_pct >= 0 ? 'positive' : 'negative';

    // 当前 market + UP/DN quotes
    const fmtPrice = v => (v && v > 0) ? v.toFixed(3) : '--';
    document.getElementById('market-name').textContent = status.market_question || '(no active market)';
    document.getElementById('up-bid').textContent   = fmtPrice(status.up_bid);
    document.getElementById('up-ask').textContent   = fmtPrice(status.up_ask);
    document.getElementById('down-bid').textContent = fmtPrice(status.down_bid);
    document.getElementById('down-ask').textContent = fmtPrice(status.down_ask);
    const wsEl = document.getElementById('clob-ws');
    if (wsEl) {
      wsEl.textContent = (status.clob_ws_connected ? 'on' : 'off') + '/' + (status.clob_ws_subscribed || 0);
      wsEl.className = status.clob_ws_connected ? 'positive' : 'negative';
    }

    const marketsContainer = document.getElementById('markets-container');
    if (marketsContainer && status.markets && status.markets.length > 0) {
      let mhtml = '<table><thead><tr><th>Coin</th><th>Market</th><th>Dev</th><th>Remaining</th><th>UP Bid/Ask</th><th>DN Bid/Ask</th></tr></thead><tbody>';
      for (const m of status.markets) {
        const dev = m.deviation_pct || 0;
        mhtml += '<tr>';
        mhtml += '<td style="font-weight:bold;color:#58a6ff;">' + (m.coin || '--') + '</td>';
        mhtml += '<td>' + (m.question || '--') + '</td>';
        mhtml += '<td class="' + (dev >= 0 ? 'positive' : 'negative') + '">' + formatPct(dev) + '</td>';
        mhtml += '<td>' + (m.minutes_remaining ?? '--') + 'min</td>';
        mhtml += '<td><span class="positive">' + fmtPrice(m.up_bid) + '</span>/<span class="positive">' + fmtPrice(m.up_ask) + '</span></td>';
        mhtml += '<td><span class="negative">' + fmtPrice(m.down_bid) + '</span>/<span class="negative">' + fmtPrice(m.down_ask) + '</span></td>';
        mhtml += '</tr>';
      }
      mhtml += '</tbody></table>';
      marketsContainer.innerHTML = mhtml;
    } else if (marketsContainer) {
      let reason = 'No market loaded';
      if ((status.loaded_market_count || 0) > 0) {
        reason = 'No market in entry window or quotes not ready';
      }
      marketsContainer.innerHTML = '<div class="positions-empty">' + reason + '</div>';
    }

    document.getElementById('total-cost').textContent = '$' + status.total_cost.toFixed(2);
    const upnlEl = document.getElementById('unrealized-pnl');
    upnlEl.textContent = formatPnl(status.unrealized_pnl);
    upnlEl.className = pnlClass(status.unrealized_pnl);

    document.getElementById('remaining').textContent = status.minutes_remaining + 'min';
    const volEl = document.getElementById('volatility');
    volEl.textContent = (status.current_vol * 100).toFixed(2) + '%';
    volEl.className = status.current_vol > status.avg_vol ? 'positive' : 'neutral';

    document.getElementById('open-pos').textContent = status.open_positions;
    document.getElementById('consec-losses').textContent = status.consecutive_losses;
    document.getElementById('uptime').textContent = 'Uptime: ' + status.uptime + ' | Tick #' + status.tick_count;

    // Positions — 仅有持仓时显示整个 section（P1-4）
    const posSection   = document.getElementById('open-positions-section');
    const posContainer = document.getElementById('positions-container');
    if (status.positions && status.positions.length > 0) {
      let html = '<table><thead><tr><th>ID</th><th>Coin</th><th>Outcome</th><th>Regime</th><th>Entry</th><th>Current</th><th>Shares</th><th>Unrealized P&L</th><th>TP Progress</th><th>MFE</th><th>MAE</th></tr></thead><tbody>';
      for (const p of status.positions) {
        const upnl = (p.current_price - p.entry_price) * p.shares * p.remaining_pct;
        const coin = p.coin || inferCoin(p);
        html += '<tr>';
        html += '<td>' + p.id + '</td>';
        html += '<td style="font-weight:bold;color:#58a6ff;">' + coin + '</td>';
        html += '<td class="side-' + p.side.toLowerCase() + '">' + coin + ' ' + p.side + '</td>';
        html += '<td>' + (p.regime || '--') + '</td>';
        html += '<td>' + p.entry_price.toFixed(3) + '</td>';
        html += '<td>' + p.current_price.toFixed(3) + '</td>';
        html += '<td>' + p.shares.toFixed(0) + '</td>';
        html += '<td class="' + pnlClass(upnl) + '">' + formatPnl(upnl) + '</td>';
        html += '<td>' + ((1 - p.remaining_pct) * 100).toFixed(0) + '% sold</td>';
        html += '<td class="positive">+' + (p.mfe * 100).toFixed(1) + 'c</td>';
        html += '<td class="negative">-' + (p.mae * 100).toFixed(1) + 'c</td>';
        html += '</tr>';
      }
      html += '</tbody></table>';
      posContainer.innerHTML = html;
      if (posSection) posSection.style.display = '';
    } else {
      if (posSection) posSection.style.display = 'none';
    }
  }

  renderExperiment('exp', expStatus, expTrades);
  renderExperiment('trend-exp', trendExpStatus, trendExpTrades);

  if (stats) {
    // 根据 currentFilter 选择数据源（all = 总计；live/dry_run = 单组）
    const grp = (currentFilter === 'all')
      ? { total_pnl: stats.total_pnl, win_rate: stats.win_rate,
          wins: stats.wins, losses: stats.losses, total_trades: stats.total_trades }
      : (stats.by_mode && stats.by_mode[currentFilter]) || { total_pnl: 0, win_rate: 0, wins: 0, losses: 0, total_trades: 0 };
    const pnlEl = document.getElementById('total-pnl');
    pnlEl.textContent = formatPnl(grp.total_pnl);
    pnlEl.className = 'card-value ' + pnlClass(grp.total_pnl);
    document.getElementById('daily-pnl').textContent = formatPnl(stats.daily_pnl);
    document.getElementById('win-rate').textContent = grp.win_rate.toFixed(1) + '%';
    document.getElementById('win-loss').textContent = grp.wins + 'W / ' + grp.losses + 'L';
    document.getElementById('total-trades').textContent = grp.total_trades;
  }

  if (trades && trades.length > 0) {
    const tbody = document.getElementById('trades-body');
    let html = '';
    const filtered = (currentFilter === 'all')
      ? trades
      : trades.filter(t => (t.mode || 'dry_run') === currentFilter);
    const thCount = document.getElementById('th-count');

    // P&L mini sparkline（cum P&L 按 exit_time 排序）
    let cum = 0;
    const sparkData = filtered.slice()
      .sort((a, b) => (a.exit_time || 0) - (b.exit_time || 0))
      .map(t => (cum += (t.pnl || 0)));
    renderSparkline('pnl-spark', sparkData);

    const groupedMap = new Map();
    for (const t of filtered) {
      const key = tradePeriodKey(t);
      if (!groupedMap.has(key)) groupedMap.set(key, []);
      groupedMap.get(key).push(t);
    }
    const groups = Array.from(groupedMap.values())
      .map(summarizeTradeGroup)
      .sort((a, b) => (b.exit_time || 0) - (a.exit_time || 0));
    if (thCount) thCount.textContent = '(' + groups.length + ' entries / ' + filtered.length + ' exits)';

    if (groups.length === 0) {
      tbody.innerHTML = '<tr><td colspan="15" style="text-align:center;color:#7d8590;padding:20px;">No trades for current filter</td></tr>';
    }

    for (const t of groups) {
      const splitCount = t.items.length;
      html += '<tr>';
      html += '<td>' + formatTime(t.exit_time || t.entry_time) + '</td>';
      html += '<td>' + t.coin + '</td>';
      html += '<td title="' + esc(t.market || t.id) + '">' + esc(t.label) + '</td>';
      html += '<td class="side-' + String(t.side).toLowerCase() + '">' + t.side + '</td>';
      html += '<td>' + t.entry_price.toFixed(3) + '</td>';
      html += '<td>' + t.exit_price.toFixed(3) + '</td>';
      html += '<td>' + t.shares.toFixed(1) + '</td>';
      html += '<td>$' + t.cost.toFixed(3) + '</td>';
      html += '<td>$' + t.revenue.toFixed(3) + '</td>';
      html += '<td>$' + t.fee.toFixed(4) + '</td>';
      html += '<td>' + renderReason(t.exit_reason) + '</td>';
      html += '<td class="' + pnlClass(t.pnl) + '">' + formatPnl(t.pnl) + '</td>';
      const mfe = ((t.max_price||0) - t.entry_price) * 100;
      const mae = (t.entry_price - (t.min_price||t.entry_price)) * 100;
      html += '<td class="positive">+' + mfe.toFixed(1) + 'c</td>';
      html += '<td class="negative">-' + mae.toFixed(1) + 'c</td>';
      const dur = t.hold_duration_sec || 0;
      html += '<td>' + Math.floor(dur/60) + 'm' + (dur%60) + 's</td>';
      html += '</tr>';
      if (splitCount > 1) {
        html += '<tr class="trade-detail-row"><td colspan="15">';
        html += '<details class="trade-detail"><summary>展开 ' + t.coin + ' 当前时间段的 ' + splitCount + ' 笔卖出</summary>';
        html += '<table><thead><tr><th>Time</th><th>ID</th><th>Side</th><th>Entry</th><th>Exit</th><th>Shares</th><th>Revenue</th><th>Fee</th><th>Reason</th><th>P&L</th></tr></thead><tbody>';
        for (const part of t.items) {
          const partRevenue = (part.shares || 0) * (part.exit_price || 0);
          html += '<tr>';
          html += '<td>' + formatTime(part.exit_time || part.entry_time) + '</td>';
          html += '<td>' + normalizeTradeId(part.id) + '</td>';
          html += '<td class="side-' + String(part.side || '').toLowerCase() + '">' + (part.side || '--') + '</td>';
          html += '<td>' + Number(part.entry_price || 0).toFixed(3) + '</td>';
          html += '<td>' + Number(part.exit_price || 0).toFixed(3) + '</td>';
          html += '<td>' + Number(part.shares || 0).toFixed(1) + '</td>';
          html += '<td>$' + partRevenue.toFixed(3) + '</td>';
          html += '<td>$' + Number(part.fee || 0).toFixed(4) + '</td>';
          html += '<td>' + renderReason(part.exit_reason) + '</td>';
          html += '<td class="' + pnlClass(part.pnl || 0) + '">' + formatPnl(part.pnl || 0) + '</td>';
          html += '</tr>';
        }
        html += '</tbody></table></details></td></tr>';
      }
    }
    if (groups.length > 0) tbody.innerHTML = html;
  } else {
    const tbody = document.getElementById('trades-body');
    const thCount = document.getElementById('th-count');
    if (thCount) thCount.textContent = '(0 trades)';
    if (tbody) tbody.innerHTML = '<tr><td colspan="15" style="text-align:center;color:#7d8590;padding:20px;">No trades yet</td></tr>';
    renderSparkline('pnl-spark', []);
  }

  // Analytics section — 根据 currentFilter 选数据源
  // all = 顶层；live/dry_run = analytics.by_mode.X
  if (currentFilter !== 'all' && analytics && analytics.by_mode && analytics.by_mode[currentFilter]) {
    analytics = analytics.by_mode[currentFilter];
  }
  if (analytics && analytics.has_data) {
    const mm = analytics.mfe_mae;
    document.getElementById('a-avg-mfe').textContent = '+' + (mm.avg_mfe * 100).toFixed(1) + 'c';
    document.getElementById('a-avg-mae').textContent = '-' + (mm.avg_mae * 100).toFixed(1) + 'c';
    document.getElementById('a-ratio').textContent = mm.ratio.toFixed(2);
    document.getElementById('a-max-mfe').textContent = '+' + (mm.max_mfe * 100).toFixed(1) + 'c';

    const d = analytics.duration;
    document.getElementById('a-avg-dur').textContent = fmtDur(d.avg);
    document.getElementById('a-min-dur').textContent = fmtDur(d.min);
    document.getElementById('a-max-dur').textContent = fmtDur(d.max);

    const sp = analytics.spread;
    document.getElementById('a-spread').innerHTML =
      '<span class="positive">' + (sp.avg_spread_winners * 100).toFixed(1) + 'c</span> / ' +
      '<span class="negative">' + (sp.avg_spread_losers * 100).toFixed(1) + 'c</span>';

    // Hour heatmap
    const hmap = document.getElementById('hour-heatmap');
    let hhtml = '';
    for (let h = 0; h < 24; h++) {
      const hd = analytics.hours[h.toString()] || {count:0,avg_pnl:0};
      const bg = hd.count === 0 ? '#1e2d3d' : hd.avg_pnl > 0 ? '#1a3a2a' : '#3a1a1a';
      const clr = hd.count === 0 ? '#484f58' : hd.avg_pnl > 0 ? '#3fb950' : '#f85149';
      hhtml += '<div style="background:'+bg+';border-radius:4px;padding:6px;text-align:center;">';
      hhtml += '<div style="font-size:0.65em;color:#7d8590;">'+h+'h</div>';
      hhtml += '<div style="font-size:0.85em;color:'+clr+';font-weight:bold;">'+hd.count+'</div>';
      if (hd.count > 0) hhtml += '<div style="font-size:0.6em;color:'+clr+';">'+formatPnl(hd.avg_pnl)+'</div>';
      hhtml += '</div>';
    }
    hmap.innerHTML = hhtml;

    // Exit reasons
    const erDiv = document.getElementById('exit-reasons');
    const reasons = analytics.exit_reasons;
    const totalR = Object.values(reasons).reduce((a,b)=>a+b, 0);
    let erhtml = '';
    const reasonColors = {stop_price:'#f85149',stop_time:'#da3633',trailing_stop:'#d29922',tp0:'#56d364',tp1:'#3fb950',tp2:'#2ea043',expired:'#58a6ff',tp_filled:'#56d364'};
    for (const [r, c] of Object.entries(reasons).sort((a,b)=>b[1]-a[1])) {
      const pct = (c/totalR*100).toFixed(0);
      const color = reasonColors[r] || '#7d8590';
      const [reasonLabel] = reasonInfo(r);
      erhtml += '<div style="display:flex;align-items:center;gap:8px;margin-bottom:4px;">';
      erhtml += '<span style="font-size:0.8em;width:120px;color:#e0e6ed;" title="'+r+'">'+reasonLabel+'</span>';
      erhtml += '<div style="flex:1;height:16px;background:#1e2d3d;border-radius:3px;overflow:hidden;">';
      erhtml += '<div style="width:'+pct+'%;height:100%;background:'+color+';"></div></div>';
      erhtml += '<span style="font-size:0.75em;color:#7d8590;width:50px;">'+c+' ('+pct+'%)</span>';
      erhtml += '</div>';
    }
    erDiv.innerHTML = erhtml;

    // Day of week
    const dowDiv = document.getElementById('day-of-week');
    const dayOrder = ['Mon','Tue','Wed','Thu','Fri','Sat','Sun'];
    let dowhtml = '';
    for (const dn of dayOrder) {
      const dd = analytics.days[dn] || {count:0,avg_pnl:0};
      const bg = dd.count === 0 ? '#1e2d3d' : dd.avg_pnl > 0 ? '#1a3a2a' : '#3a1a1a';
      const clr = dd.count === 0 ? '#484f58' : dd.avg_pnl > 0 ? '#3fb950' : '#f85149';
      dowhtml += '<div style="background:'+bg+';border-radius:4px;padding:8px;text-align:center;">';
      dowhtml += '<div style="font-size:0.7em;color:#7d8590;">'+dn+'</div>';
      dowhtml += '<div style="font-size:1em;color:'+clr+';font-weight:bold;">'+dd.count+'</div>';
      if (dd.count > 0) dowhtml += '<div style="font-size:0.65em;color:'+clr+';">'+formatPnl(dd.avg_pnl)+'</div>';
      dowhtml += '</div>';
    }
    dowDiv.innerHTML = dowhtml;

    // Streak analysis
    const st = analytics.streak;
    const stDiv = document.getElementById('streak-analysis');
    stDiv.innerHTML =
      '<div style="margin-bottom:8px;">' +
      '<span style="color:#7d8590;">After 2+ losses:</span> ' +
      '<span class="' + pnlClass(st.after_2loss_avg_pnl) + '">' + formatPnl(st.after_2loss_avg_pnl) + '</span>' +
      ' avg (' + st.after_2loss_count + ' trades)</div>' +
      '<div><span style="color:#7d8590;">Normal:</span> ' +
      '<span class="' + pnlClass(st.normal_avg_pnl) + '">' + formatPnl(st.normal_avg_pnl) + '</span>' +
      ' avg (' + st.normal_count + ' trades)</div>';

    // Core metrics
    const pf = analytics.profit_factor;
    document.getElementById('a-pf').textContent = pf.toFixed(2);
    document.getElementById('a-pf').className = 'stat-value ' + (pf >= 1 ? 'positive' : 'negative');
    const ev = analytics.ev_per_trade;
    document.getElementById('a-ev').textContent = formatPnl(ev);
    document.getElementById('a-ev').className = 'stat-value ' + pnlClass(ev);
    document.getElementById('a-wl').innerHTML =
      '<span class="positive">' + formatPnl(analytics.avg_win) + '</span>' +
      ' / <span class="negative">' + formatPnl(analytics.avg_loss) + '</span>' +
      '<div style="font-size:0.6em;color:#7d8590;">ratio ' + analytics.win_loss_ratio.toFixed(2) + '</div>';
    document.getElementById('a-dd').textContent = '-$' + analytics.max_drawdown.toFixed(2) +
      ' (' + analytics.max_drawdown_pct.toFixed(1) + '%)';

    // MFE capture
    const mc = analytics.mfe_capture;
    document.getElementById('a-cap-all').textContent = (mc.avg * 100).toFixed(1) + '%';
    document.getElementById('a-cap-win').textContent = (mc.avg_winners * 100).toFixed(1) + '%';
    document.getElementById('a-cap-lose').textContent = (mc.avg_losers * 100).toFixed(1) + '%';
    const tss = analytics.trailing_stop_stats;
    const tsEl = document.getElementById('a-ts-avg');
    tsEl.textContent = tss.count > 0 ? formatPnl(tss.avg_pnl) + ' (' + tss.count + ')' : 'N/A';
    tsEl.className = 'stat-value ' + (tss.count > 0 ? pnlClass(tss.avg_pnl) : 'neutral');

    // Direction analysis
    const dirDiv = document.getElementById('direction-analysis');
    const dirData = analytics.direction;
    let dirHtml = '<div style="display:grid;grid-template-columns:1fr 1fr;gap:12px;">';
    for (const side of ['UP','DOWN']) {
      const d = dirData[side];
      const clr = side === 'UP' ? '#3fb950' : '#f85149';
      dirHtml += '<div style="background:#0d1117;border-radius:6px;padding:12px;border-left:3px solid '+clr+';">';
      dirHtml += '<div style="font-weight:bold;color:'+clr+';margin-bottom:6px;">'+side+'</div>';
      dirHtml += '<div style="font-size:0.8em;color:#7d8590;">Trades: <span style="color:#e0e6ed;">'+d.count+'</span></div>';
      dirHtml += '<div style="font-size:0.8em;color:#7d8590;">Wins: <span style="color:#e0e6ed;">'+d.wins+' ('+d.win_rate.toFixed(1)+'%)</span></div>';
      dirHtml += '<div style="font-size:0.8em;color:#7d8590;">Avg P&L: <span class="'+pnlClass(d.avg_pnl)+'">'+formatPnl(d.avg_pnl)+'</span></div>';
      dirHtml += '<div style="font-size:0.8em;color:#7d8590;">Total: <span class="'+pnlClass(d.total_pnl)+'">'+formatPnl(d.total_pnl)+'</span></div>';
      dirHtml += '</div>';
    }
    dirHtml += '</div>';
    dirDiv.innerHTML = dirHtml;

    // TP hit rates
    const tpDiv = document.getElementById('tp-hit-rates');
    const tpData = analytics.tp_hit_rates;
    const tpColors = {tp0:'#56d364',tp1:'#3fb950',tp2:'#2ea043',trailing_stop:'#d29922'};
    let tpHtml = '';
    for (const [k, v] of Object.entries(tpData)) {
      const color = tpColors[k] || '#7d8590';
      tpHtml += '<div style="display:flex;align-items:center;gap:8px;margin-bottom:6px;">';
      tpHtml += '<span style="font-size:0.8em;width:100px;color:#e0e6ed;">'+k+'</span>';
      tpHtml += '<div style="flex:1;height:18px;background:#1e2d3d;border-radius:3px;overflow:hidden;">';
      tpHtml += '<div style="width:'+Math.max(v.pct,2)+'%;height:100%;background:'+color+';border-radius:3px;"></div></div>';
      tpHtml += '<span style="font-size:0.75em;color:#7d8590;width:70px;">'+v.count+' ('+v.pct.toFixed(0)+'%)</span>';
      tpHtml += '</div>';
    }
    tpDiv.innerHTML = tpHtml;

    // Equity curve
    const ecDiv = document.getElementById('equity-curve');
    const ecData = analytics.equity_curve;
    if (ecData && ecData.length > 1) {
      const bals = ecData.map(e => e.balance);
      const minB = Math.min(...bals), maxB = Math.max(...bals);
      const range = maxB - minB || 1;
      const w = ecDiv.clientWidth || 600, h = 150;
      const padL = 50, padR = 10, padT = 10, padB = 25;
      const gw = w - padL - padR, gh = h - padT - padB;
      let pts = ecData.map((e, i) => {
        const x = padL + (i / (ecData.length - 1)) * gw;
        const y = padT + (1 - (e.balance - minB) / range) * gh;
        return x.toFixed(1) + ',' + y.toFixed(1);
      }).join(' ');
      const lastBal = bals[bals.length-1];
      const lineColor = lastBal >= bals[0] ? '#3fb950' : '#f85149';
      let svg = '<svg width="'+w+'" height="'+h+'" style="display:block;">';
      // grid lines
      for (let i = 0; i <= 4; i++) {
        const y = padT + (i/4) * gh;
        const val = (maxB - (i/4) * range).toFixed(2);
        svg += '<line x1="'+padL+'" y1="'+y+'" x2="'+(w-padR)+'" y2="'+y+'" stroke="#1e2d3d" stroke-width="1"/>';
        svg += '<text x="'+(padL-4)+'" y="'+(y+4)+'" fill="#7d8590" font-size="10" text-anchor="end">$'+val+'</text>';
      }
      svg += '<polyline points="'+pts+'" fill="none" stroke="'+lineColor+'" stroke-width="2"/>';
      // start/end markers
      const firstPt = pts.split(' ')[0].split(',');
      const lastPt = pts.split(' ').pop().split(',');
      svg += '<circle cx="'+firstPt[0]+'" cy="'+firstPt[1]+'" r="3" fill="#58a6ff"/>';
      svg += '<circle cx="'+lastPt[0]+'" cy="'+lastPt[1]+'" r="3" fill="'+lineColor+'"/>';
      svg += '</svg>';
      ecDiv.innerHTML = svg;
    } else {
      ecDiv.innerHTML = '<div style="color:#7d8590;text-align:center;padding:40px;">Not enough data</div>';
    }

    // Bucket renderer helper
    function renderBuckets(containerId, data) {
      const el = document.getElementById(containerId);
      let html = '';
      for (const [label, b] of Object.entries(data)) {
        if (b.count === 0) continue;
        const clr = b.avg_pnl >= 0 ? '#3fb950' : '#f85149';
        html += '<div style="display:flex;align-items:center;gap:8px;margin-bottom:6px;">';
        html += '<span style="font-size:0.8em;width:70px;color:#e0e6ed;">'+label+'</span>';
        html += '<div style="flex:1;display:flex;align-items:center;gap:6px;">';
        html += '<span style="font-size:0.75em;color:#7d8590;">'+b.count+'trades</span>';
        html += '<span style="font-size:0.75em;color:#7d8590;">WR:'+b.win_rate.toFixed(0)+'%</span>';
        html += '<span style="font-size:0.8em;color:'+clr+';font-weight:bold;">'+formatPnl(b.avg_pnl)+'</span>';
        html += '</div></div>';
      }
      el.innerHTML = html || '<div style="color:#7d8590;">No data</div>';
    }
    renderBuckets('entry-price-buckets', analytics.entry_price_buckets);
    renderBuckets('btc-dev-buckets', analytics.btc_deviation_buckets);
    renderBuckets('duration-buckets', analytics.duration_buckets);

    // Trailing stop vs Price stop
    const tsDiv = document.getElementById('trailing-vs-stop');
    const tsStat = analytics.trailing_stop_stats;
    tsDiv.innerHTML =
      '<div style="margin-bottom:10px;">' +
      '<div style="color:#d29922;font-weight:bold;margin-bottom:4px;">Trailing Stop</div>' +
      '<span style="color:#7d8590;">Count: </span><span>' + tsStat.count + '</span> | ' +
      '<span style="color:#7d8590;">Avg P&L: </span><span class="' + pnlClass(tsStat.avg_pnl) + '">' + formatPnl(tsStat.avg_pnl) + '</span>' +
      '</div><div>' +
      '<div style="color:#f85149;font-weight:bold;margin-bottom:4px;">Price Stop (-50%)</div>' +
      '<span style="color:#7d8590;">Avg P&L: </span><span class="negative">' + formatPnl(tsStat.stop_price_avg_pnl) + '</span>' +
      '</div>';
  }
}

setInterval(refresh, REFRESH_MS);
refresh();
</script>
</body>
</html>
)html";

}  // namespace polymarket
