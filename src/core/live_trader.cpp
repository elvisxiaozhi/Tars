#include "core/live_trader.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "crypto/wallet.h"

namespace polymarket {

LiveTrader::LiveTrader(const AppConfig& cfg) : cfg_(cfg) {}
LiveTrader::~LiveTrader() = default;

// ===== 钱包（R2）=====
//
// 流程：
//   1. 读 keystore.enc 文件（路径来自 cfg_.wallet.keystore_path）
//   2. 拿密码：优先 KEYSTORE_PASSWORD env var；否则 stdin 提示输入
//   3. decrypt_keystore → derive_address
//   4. 与 cfg_.wallet.address 比对（case-insensitive），不一致 throw
//   5. 缓存地址到 address_ + 置 wallet_initialized_=true
//
// 异常会一路抛到 main()，由启动守卫捕获并 return 1
void LiveTrader::init_wallet() {
    if (wallet_initialized_) return;

    const std::string& path = cfg_.wallet.keystore_path;
    if (path.empty()) {
        throw std::runtime_error("init_wallet: cfg.wallet.keystore_path is empty");
    }

    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("init_wallet: cannot open keystore file: " + path);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string keystore_json = ss.str();
    if (keystore_json.empty()) {
        throw std::runtime_error("init_wallet: keystore file is empty: " + path);
    }

    // 密码来源：env var 优先（自动化场景），否则交互式
    std::string password;
    const char* env_pwd = std::getenv("KEYSTORE_PASSWORD");
    if (env_pwd && *env_pwd) {
        password = env_pwd;
        spdlog::info("init_wallet: using password from KEYSTORE_PASSWORD env var");
    } else {
        password = read_password(
            "Enter keystore password for " + path + " (no echo): ");
    }

    if (password.empty()) {
        throw std::runtime_error("init_wallet: empty password");
    }

    PrivateKey key;
    try {
        key = decrypt_keystore(keystore_json, password);
    } catch (...) {
        secure_zero(password.data(), password.size());
        throw;
    }
    secure_zero(password.data(), password.size());

    std::string derived = derive_address(key);

    // 比对 cfg.wallet.address（case-insensitive，因为 EIP-55 大小写易出错）
    if (!cfg_.wallet.address.empty()) {
        bool match = (derived.size() == cfg_.wallet.address.size());
        if (match) {
            for (size_t i = 0; i < derived.size() && match; ++i) {
                char a = derived[i], b = cfg_.wallet.address[i];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
                if (a != b) match = false;
            }
        }
        if (!match) {
            throw std::runtime_error(
                "init_wallet: derived address does NOT match cfg.wallet.address (expected " +
                cfg_.wallet.address + ", got " + derived + ")");
        }
    }

    address_ = derived;
    wallet_initialized_ = true;
    spdlog::info("init_wallet: OK, address={}", address_);
}

std::string LiveTrader::wallet_address() const {
    if (!wallet_initialized_) {
        throw std::runtime_error("LiveTrader::wallet_address called before init_wallet");
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
