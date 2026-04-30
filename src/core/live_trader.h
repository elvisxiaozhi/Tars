#pragma once

// LiveTrader：实盘下单/对账门面类
//
// 设计目标：把所有「真金白银」的副作用集中在一个类里，让 main.cpp 的策略主循环
// 在 mode=="live" 时统一委托给本类，dry_run 路径完全不受影响。
//
// 实现进度（按 step 拆分）：
//   R1 [当前]：骨架；所有方法 throw std::runtime_error("... not implemented (Rx pending)")
//   R2：init_wallet / wallet_address —— keystore.enc 解密 + secp256k1 派生地址
//   R3：read_chain_state —— Polygon eth_call 读 USDC/CTF/allowance
//   R4：（无新方法，但 EIP-712 模块到位后 R5/R7/R8 才能实现）
//   R5：ensure_clob_authenticated —— 用 EIP-712 签名调 CLOB /auth/api-key
//   R6：check_approvals_sufficient —— 启动期校验 allowance 已到位
//   R7：place_entry_order —— 入场限价单（maker, ask-1¢）
//   R8：place_exit_order  —— 出场（partial TP 用 limit/market，止损用 market）
//   R9：reconcile_on_startup / emergency_close_all —— 对账 + SIGINT 应急平仓

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/strategy.h"   // EntrySignal, Position
#include "crypto/wallet.h"   // PrivateKey
#include "utils/config.h"

namespace polymarket {

// 单笔订单执行结果
struct OrderResult {
    bool success = false;
    std::string order_id;          // CLOB 返回的订单 ID
    double filled_shares = 0;      // 实际成交数量（可能 < 请求量）
    double filled_avg_price = 0;   // 实际加权成交价
    double fee_paid = 0;           // 实际收取的费用（USDC）
    int64_t fill_time_ms = 0;
    std::string error;             // 失败原因（success=false 时填）
};

// 链上状态快照（一次 read_chain_state 同步拿全套）
struct ChainBalance {
    double usdc = 0;
    std::map<std::string, double> ctf_balances;  // token_id → shares
    double allowance_usdc = 0;                   // USDC → CTFExchange 的授权额度
    int64_t snapshot_ms = 0;
};

// V2 vault ledger 余额（GET /balance-allowance）。
// V2 升级后用户的可用资金不在 proxy 钱包里，而是 ledger 数额，必须经此 API 读取。
struct PolymarketBalance {
    double  cash_pusd     = 0;  // pUSD 余额（USD 等价，6 decimals）
    double  allowance     = 0;  // ledger 已授权给 exchange 的份额
    int64_t snapshot_ms   = 0;
};

// 启动对账拿到的未平仓订单（GET /data/orders）。
struct OpenOrder {
    std::string order_id;
    std::string token_id;
    std::string side;          // "BUY" / "SELL"
    double      price = 0;
    double      size  = 0;     // 原始 size
    double      size_matched = 0;
    std::string status;        // "live" / "matched" / etc
};

class LiveTrader {
public:
    explicit LiveTrader(const AppConfig& cfg);
    ~LiveTrader();

    // ===== 钱包（R2）=====
    void init_wallet();
    std::string wallet_address() const;

    // ===== 链上读（R3）=====
    ChainBalance read_chain_state(
        const std::vector<std::string>& ctf_token_ids = {});

    // ===== CLOB 鉴权（R5）=====
    void ensure_clob_authenticated();
    bool is_authenticated() const;

    // ===== Polymarket V2 cash balance（R-V2.3）=====
    // GET /balance-allowance（L2 HMAC）→ pUSD ledger 数额；V2 升级后必须经此读余额。
    PolymarketBalance read_polymarket_balance();

    // ===== 取消订单（R-V2.5c）=====
    // DELETE /order with body {"orderID":"..."}（L2 HMAC）。返回 true=成功。
    bool cancel_order(const std::string& order_id);

    // ===== Approval 校验（R6）=====
    bool check_approvals_sufficient(double min_usdc_allowance);

    // ===== 下单（R7 / R8）=====
    OrderResult place_entry_order(const EntrySignal& sig, double shares);
    OrderResult place_exit_order(const Position& pos, double shares, double price,
                                 bool is_taker, const std::string& reason);

    // ===== 启动对账 + 应急平仓（R9-V2）=====
    // GET /data/orders 列出 user 当前未平仓订单（含状态/已成交量），供 main loop
    // 决定是否 cancel 残留 / 等待 fill 等。返回空 vector 表示无遗留订单。
    std::vector<OpenOrder> reconcile_on_startup();

    // 紧急平仓：对每个 position 发 SELL FOK @ price=0.01 (floor) 让 server 按对手最佳 bid 吃。
    // 不阻塞、不等 fill 确认；SIGINT 紧急退出时调用。
    void emergency_close_all(const std::vector<Position>& positions);

private:
    // V2 通用下单 helper —— BUY/SELL 共享（is_buy 决定 side 与 maker/taker amount swap）
    OrderResult send_v2_order(
        const std::string& token_id, double price, double shares,
        bool is_buy, bool is_taker, const std::string& tag);

    const AppConfig& cfg_;

    // R2: wallet
    bool       wallet_initialized_ = false;
    PrivateKey key_;            // stays alive after init_wallet for signing (R5/R7/R8)
    std::string address_;       // EOA "0x…"

    // R5: CLOB API credentials
    bool        clob_authenticated_ = false;
    std::string clob_api_key_;
    std::string clob_secret_;
    std::string clob_passphrase_;
};

}  // namespace polymarket
