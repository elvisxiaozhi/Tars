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
  .positions-empty { color: #7d8590; padding: 20px; text-align: center; }
  .uptime { font-size: 0.8em; color: #7d8590; }
  .refresh { font-size: 0.7em; color: #484f58; }
  .pnl-bar { display: flex; align-items: center; gap: 8px; margin-top: 4px; }
  .pnl-bar-fill { height: 4px; border-radius: 2px; min-width: 2px; }
</style>
</head>
<body>
<div class="header">
  <div>
    <h1>Polymarket BTC 1h Bot</h1>
    <span class="uptime" id="uptime">--</span>
  </div>
  <div>
    <span class="mode" id="mode-badge">--</span>
    <span class="refresh" id="refresh-info">auto-refresh 5s</span>
  </div>
</div>

<div class="grid">
  <div class="card">
    <div class="card-title">Account Balance</div>
    <div class="card-value neutral" id="account-balance">--</div>
    <div style="font-size:0.75em;color:#7d8590;margin-top:4px;">
      BTC: <span id="btc-price">--</span> | Dev: <span id="btc-dev">--</span>
    </div>
  </div>
  <div class="card">
    <div class="card-title">Position Cost</div>
    <div class="card-value neutral" id="total-cost">$0.00</div>
    <div style="font-size:0.75em;color:#7d8590;margin-top:4px;">
      Unrealized: <span id="unrealized-pnl">$0.00</span>
    </div>
  </div>
  <div class="card">
    <div class="card-title">Realized P&L</div>
    <div class="card-value" id="total-pnl">$0.00</div>
    <div style="font-size:0.75em;color:#7d8590;margin-top:4px;">
      Daily: <span id="daily-pnl">$0.00</span>
    </div>
  </div>
  <div class="card">
    <div class="card-title">Win Rate</div>
    <div class="card-value neutral" id="win-rate">--%</div>
    <div style="font-size:0.75em;color:#7d8590;margin-top:4px;">
      <span id="win-loss">0W / 0L</span> of <span id="total-trades">0</span> trades
    </div>
  </div>
</div>

<div class="stats-grid">
  <div class="stat">
    <div class="stat-label">Open Positions</div>
    <div class="stat-value neutral" id="open-pos">0</div>
  </div>
  <div class="stat">
    <div class="stat-label">Volatility</div>
    <div class="stat-value" id="volatility">--</div>
  </div>
  <div class="stat">
    <div class="stat-label">Candle Remaining</div>
    <div class="stat-value" id="remaining">--</div>
  </div>
  <div class="stat">
    <div class="stat-label">Consecutive Losses</div>
    <div class="stat-value" id="consec-losses">0</div>
  </div>
</div>

<div class="section">
  <div class="section-title">Open Positions</div>
  <div id="positions-container">
    <div class="positions-empty">No open positions</div>
  </div>
</div>

<div class="section">
  <div class="section-title">Trade History</div>
  <table>
    <thead>
      <tr>
        <th>Time</th>
        <th>Market</th>
        <th>Side</th>
        <th>Entry</th>
        <th>Exit</th>
        <th>Shares</th>
        <th>Reason</th>
        <th>P&L</th>
      </tr>
    </thead>
    <tbody id="trades-body">
      <tr><td colspan="8" style="text-align:center;color:#7d8590;padding:20px;">No trades yet</td></tr>
    </tbody>
  </table>
</div>

<script>
const API_BASE = '';
const REFRESH_MS = 5000;

function formatPrice(v) { return v ? '$' + Number(v).toLocaleString('en-US', {minimumFractionDigits:2, maximumFractionDigits:2}) : '--'; }
function formatPct(v) { return v !== undefined ? (v >= 0 ? '+' : '') + v.toFixed(2) + '%' : '--'; }
function formatPnl(v) { return (v >= 0 ? '+$' : '-$') + Math.abs(v).toFixed(2); }
function pnlClass(v) { return v > 0 ? 'positive' : v < 0 ? 'negative' : 'neutral'; }
function formatTime(ms) {
  if (!ms) return '--';
  const d = new Date(ms);
  return d.toLocaleTimeString('zh-CN', {hour:'2-digit',minute:'2-digit',second:'2-digit'});
}

async function fetchJSON(path) {
  try { const r = await fetch(API_BASE + path); return await r.json(); }
  catch(e) { return null; }
}

async function refresh() {
  const [status, trades, stats] = await Promise.all([
    fetchJSON('/api/status'),
    fetchJSON('/api/trades'),
    fetchJSON('/api/stats')
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

    // Positions
    const posContainer = document.getElementById('positions-container');
    if (status.positions && status.positions.length > 0) {
      let html = '<table><thead><tr><th>ID</th><th>Side</th><th>Entry</th><th>Current</th><th>Shares</th><th>Unrealized P&L</th><th>TP Progress</th></tr></thead><tbody>';
      for (const p of status.positions) {
        const upnl = (p.current_price - p.entry_price) * p.shares * p.remaining_pct;
        html += '<tr>';
        html += '<td>' + p.id + '</td>';
        html += '<td class="side-' + p.side.toLowerCase() + '">' + p.side + '</td>';
        html += '<td>' + p.entry_price.toFixed(3) + '</td>';
        html += '<td>' + p.current_price.toFixed(3) + '</td>';
        html += '<td>' + p.shares.toFixed(0) + '</td>';
        html += '<td class="' + pnlClass(upnl) + '">' + formatPnl(upnl) + '</td>';
        html += '<td>' + ((1 - p.remaining_pct) * 100).toFixed(0) + '% sold</td>';
        html += '</tr>';
      }
      html += '</tbody></table>';
      posContainer.innerHTML = html;
    } else {
      posContainer.innerHTML = '<div class="positions-empty">No open positions</div>';
    }
  }

  if (stats) {
    const pnlEl = document.getElementById('total-pnl');
    pnlEl.textContent = formatPnl(stats.total_pnl);
    pnlEl.className = 'card-value ' + pnlClass(stats.total_pnl);
    document.getElementById('daily-pnl').textContent = formatPnl(stats.daily_pnl);
    document.getElementById('win-rate').textContent = stats.win_rate.toFixed(1) + '%';
    document.getElementById('win-loss').textContent = stats.wins + 'W / ' + stats.losses + 'L';
    document.getElementById('total-trades').textContent = stats.total_trades;
  }

  if (trades && trades.length > 0) {
    const tbody = document.getElementById('trades-body');
    let html = '';
    for (const t of trades.slice().reverse()) {
      html += '<tr>';
      html += '<td>' + formatTime(t.entry_time) + '</td>';
      html += '<td>' + (t.market.length > 40 ? t.market.substr(0,37)+'...' : t.market) + '</td>';
      html += '<td class="side-' + t.side.toLowerCase() + '">' + t.side + '</td>';
      html += '<td>' + t.entry_price.toFixed(3) + '</td>';
      html += '<td>' + t.exit_price.toFixed(3) + '</td>';
      html += '<td>' + t.shares.toFixed(0) + '</td>';
      html += '<td>' + t.exit_reason + '</td>';
      html += '<td class="' + pnlClass(t.pnl) + '">' + formatPnl(t.pnl) + '</td>';
      html += '</tr>';
    }
    tbody.innerHTML = html;
  }
}

setInterval(refresh, REFRESH_MS);
refresh();
</script>
</body>
</html>
)html";

}  // namespace polymarket
