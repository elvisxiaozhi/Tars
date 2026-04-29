#include "core/live_trader.h"

#include <stdexcept>

namespace polymarket {

LiveTrader::LiveTrader(const AppConfig& cfg) : cfg_(cfg) {}
LiveTrader::~LiveTrader() = default;

// ===== 钱包（R2）=====
void LiveTrader::init_wallet() {
    throw std::runtime_error("LiveTrader::init_wallet not implemented (R2 pending)");
}

std::string LiveTrader::wallet_address() const {
    if (!wallet_initialized_) {
        throw std::runtime_error("LiveTrader::wallet_address called before init_wallet (R2 pending)");
    }
    return address_;
}

// ===== 链上读（R3）=====
ChainBalance LiveTrader::read_chain_state(const std::vector<std::string>&) {
    throw std::runtime_error("LiveTrader::read_chain_state not implemented (R3 pending)");
}

// ===== CLOB 鉴权（R5）=====
void LiveTrader::ensure_clob_authenticated() {
    throw std::runtime_error("LiveTrader::ensure_clob_authenticated not implemented (R5 pending)");
}

bool LiveTrader::is_authenticated() const {
    return clob_authenticated_;
}

// ===== Approval 校验（R6）=====
bool LiveTrader::check_approvals_sufficient(double) {
    throw std::runtime_error("LiveTrader::check_approvals_sufficient not implemented (R6 pending)");
}

// ===== 下单（R7 / R8）=====
OrderResult LiveTrader::place_entry_order(const EntrySignal&, double) {
    throw std::runtime_error("LiveTrader::place_entry_order not implemented (R7 pending)");
}

OrderResult LiveTrader::place_exit_order(const Position&, double, bool, const std::string&) {
    throw std::runtime_error("LiveTrader::place_exit_order not implemented (R8 pending)");
}

// ===== 启动对账 + 应急平仓（R9）=====
void LiveTrader::reconcile_on_startup() {
    throw std::runtime_error("LiveTrader::reconcile_on_startup not implemented (R9 pending)");
}

void LiveTrader::emergency_close_all(const std::vector<Position>&) {
    throw std::runtime_error("LiveTrader::emergency_close_all not implemented (R9 pending)");
}

}  // namespace polymarket
