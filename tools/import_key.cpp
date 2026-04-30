// import_key —— 一次性 CLI：把明文私钥加密成 Web3 keystore v3
//
// 用法：
//   ./build/import_key <pk_file> <keystore_out> [<expected_address>]
//
// 例：
//   ./build/import_key /tmp/pk.txt ./keystore.enc 0x356E4c0a80B5Ac2466B7a62A11B35e8DbC7d2196
//
// 流程：
//   1. 读 pk_file（裸 hex 或 0x 前缀，64 hex chars）
//   2. 派生地址（与 expected_address 比对，不一致则中止）
//   3. 提示设置 keystore 密码（≥12 位，二次确认）
//   4. 加密 + 写文件（chmod 0600）
//   5. 打印清理指南（用户手动 shred + history -c）
//
// 安全：
//   - 私钥从文件读，**不接受命令行参数**（避免出现在 ps 输出）
//   - 密码 stdin 无回显
//   - 写入后立即 secure_zero 内存

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>

#include "crypto/wallet.h"

using namespace polymarket;

namespace {

void usage_and_exit(int code) {
    std::cerr << "Usage: import_key <pk_file> <keystore_out> [<expected_address>]\n";
    std::cerr << "\n";
    std::cerr << "  pk_file            Plaintext private key file (64 hex chars,\n";
    std::cerr << "                     optional '0x' prefix, optional trailing newline)\n";
    std::cerr << "  keystore_out       Output path for encrypted keystore (will be 0600)\n";
    std::cerr << "  expected_address   (optional) Verify derived address matches this\n";
    std::exit(code);
}

std::string read_pk_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "ERROR: cannot open " << path << ": " << std::strerror(errno) << "\n";
        std::exit(1);
    }
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    // strip whitespace
    while (!content.empty() && std::isspace(static_cast<unsigned char>(content.back()))) {
        content.pop_back();
    }
    while (!content.empty() && std::isspace(static_cast<unsigned char>(content.front()))) {
        content.erase(content.begin());
    }
    return content;
}

bool address_match_ci(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

bool file_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) usage_and_exit(1);

    std::string pk_file = argv[1];
    std::string keystore_out = argv[2];
    std::string expected_addr = (argc == 4) ? argv[3] : "";

    if (file_exists(keystore_out)) {
        std::cerr << "ERROR: " << keystore_out << " already exists. "
                  << "Move or delete it first.\n";
        return 1;
    }

    // 1. 读私钥
    std::string pk_hex = read_pk_file(pk_file);
    if (pk_hex.empty()) {
        std::cerr << "ERROR: " << pk_file << " is empty\n";
        return 1;
    }

    // 2. 解码为 PrivateKey
    PrivateKey key;
    try {
        key = PrivateKey::from_hex(pk_hex);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: invalid private key in " << pk_file << ": " << e.what() << "\n";
        secure_zero(pk_hex.data(), pk_hex.size());
        return 1;
    }
    secure_zero(pk_hex.data(), pk_hex.size());  // 清擦明文 hex

    // 3. 派生地址
    std::string derived;
    try {
        derived = derive_address(key);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: derive_address failed: " << e.what() << "\n";
        return 1;
    }

    std::cout << "Derived address: " << derived << "\n";

    if (!expected_addr.empty()) {
        if (!address_match_ci(derived, expected_addr)) {
            std::cerr << "\nERROR: derived address does NOT match expected.\n"
                      << "  expected: " << expected_addr << "\n"
                      << "  derived:  " << derived << "\n"
                      << "Aborting—possible wrong private key.\n";
            return 1;
        }
        std::cout << "✓ Matches expected address (case-insensitive)\n";
    }

    // 4. 设置密码（二次确认）
    std::cout << "\n";
    std::string pwd1 = read_password("Set keystore password (>=12 chars, no echo): ");
    if (pwd1.size() < 12) {
        std::cerr << "ERROR: password too short (got " << pwd1.size() << ", need >=12)\n";
        secure_zero(pwd1.data(), pwd1.size());
        return 1;
    }
    std::string pwd2 = read_password("Confirm password: ");
    if (pwd1 != pwd2) {
        std::cerr << "ERROR: passwords do not match\n";
        secure_zero(pwd1.data(), pwd1.size());
        secure_zero(pwd2.data(), pwd2.size());
        return 1;
    }
    secure_zero(pwd2.data(), pwd2.size());

    // 5. 加密
    std::cout << "Encrypting (scrypt N=262144, ~1s)... " << std::flush;
    std::string keystore_json;
    try {
        keystore_json = encrypt_keystore(key, pwd1);
    } catch (const std::exception& e) {
        std::cerr << "\nERROR: encrypt failed: " << e.what() << "\n";
        secure_zero(pwd1.data(), pwd1.size());
        return 1;
    }
    secure_zero(pwd1.data(), pwd1.size());
    std::cout << "done.\n";

    // 6. 写文件 + chmod 0600
    {
        std::ofstream out(keystore_out, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "ERROR: cannot write " << keystore_out << ": "
                      << std::strerror(errno) << "\n";
            return 1;
        }
        out << keystore_json;
    }
    if (::chmod(keystore_out.c_str(), 0600) != 0) {
        std::cerr << "WARN: chmod 0600 failed on " << keystore_out << ": "
                  << std::strerror(errno) << "\n";
    }
    std::cout << "Wrote " << keystore_out << " (0600)\n";

    // 7. 清理提示
    std::cout << "\n=================================================================\n";
    std::cout << "NEXT STEPS (run manually now to scrub plaintext key):\n";
    std::cout << "  1) Destroy the plaintext key file:\n";
    std::cout << "       /usr/bin/srm -m " << pk_file << "    # macOS secure delete\n";
    std::cout << "       # or:  rm -P " << pk_file << "          # macOS \"overwrite then unlink\"\n";
    std::cout << "       # or fallback: shred -u " << pk_file << "\n";
    std::cout << "  2) Clear shell history:\n";
    std::cout << "       history -c && history -w\n";
    std::cout << "  3) Update config/config.json wallet.keystore_path to:\n";
    std::cout << "       \"" << keystore_out << "\"\n";
    std::cout << "     and wallet.address to:\n";
    std::cout << "       \"" << derived << "\"\n";
    std::cout << "=================================================================\n";

    return 0;
}
