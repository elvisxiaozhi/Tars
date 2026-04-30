#!/usr/bin/env python3
"""
Dry-sign 对照基线 — 用 py-clob-client-v2 在固定输入下生成 V2 Order 签名 + JSON body，
作为 C++ R-V2.5a 实现的 KAT 参照。**纯本地签名，不发任何网络请求。**

用法：
    cd /Users/theodore/Desktop/polymarket
    python3 -m venv .venv && source .venv/bin/activate
    pip install py-clob-client-v2
    python tools/dry_sign.py

把输出全部贴给我，里面的 salt / signature / 完整 JSON 会被嵌到 C++ KAT 里。

可复现性：
    - timestamp 由 py-clob-client-v2 内部 time.time_ns() 生成（每次跑会变）→ 我们直接采用一次跑的输出作为 frozen KAT
    - salt 同上（库内部 random uint256）
    - 私钥固定 = 0x000...001，EOA 地址固定 = 0x7E5F4552091A69125d5DfCb7b8C2659029395Bdf
"""
import json
import sys

# ── 固定测试输入 ─────────────────────────────────────────────────────────────
EOA_PRIVATE_KEY = "0x0000000000000000000000000000000000000000000000000000000000000001"
PROXY_FUNDER    = "0x356E4c0a80B5Ac2466B7a62A11B35e8DbC7d2196"  # 用真实者 proxy 但仅本地签名
CHAIN_ID        = 137
TOKEN_ID        = "52114319501245915516055106046884209969926127482827954674443846427813813222426"
PRICE           = 0.50  # 50¢
SIZE            = 10.0
SIDE            = "BUY"

# ── import + 依赖检查 ────────────────────────────────────────────────────────
try:
    from py_clob_client_v2.signer import Signer
    from py_clob_client_v2.order_builder.builder import OrderBuilder
    from py_clob_client_v2.clob_types import OrderArgs
    from py_clob_client_v2.order_utils.model.signature_type_v2 import SignatureTypeV2
except ImportError as e:
    print("[!] py-clob-client-v2 not installed:", e, file=sys.stderr)
    print("    pip install py-clob-client-v2", file=sys.stderr)
    sys.exit(1)

# ── 调 SDK ───────────────────────────────────────────────────────────────────
import inspect
from py_clob_client_v2.clob_types import OrderArgsV2, CreateOrderOptions

print("=== OrderArgsV2 ===")
print(inspect.signature(OrderArgsV2))
print()
print("=== CreateOrderOptions ===")
print(inspect.signature(CreateOrderOptions))
print()

signer  = Signer(EOA_PRIVATE_KEY, chain_id=CHAIN_ID)
builder = OrderBuilder(signer, signature_type=SignatureTypeV2.POLY_PROXY, funder=PROXY_FUNDER)

# 试构造 args + options（字段名根据上面的 signature 调）
# OrderArgsV2 应该有 token_id/price/size/side + V2 新字段（可能 expiration/builder_code/metadata）
args = OrderArgsV2(token_id=TOKEN_ID, price=PRICE, size=SIZE, side=SIDE)

# CreateOrderOptions 通常包含 tick_size 和 neg_risk（市场维度参数）
opts = CreateOrderOptions(tick_size="0.01", neg_risk=False)

signed = builder.build_order(args, opts)

# 自检 + 序列化
import inspect
print("=== build_order return type ===")
print(type(signed).__name__)
print()
print("=== build_order return repr ===")
print(repr(signed))
print()

# 转 plain dict — 优先用 dataclasses.asdict（V2 多半是 @dataclass）
import dataclasses
try:
    plain = dataclasses.asdict(signed)
except (TypeError, ValueError):
    # 不是 dataclass → 尝试一层 vars()，不递归
    try:
        plain = dict(vars(signed))
        # 子字段如果还是对象，再转一层
        for k, v in plain.items():
            if dataclasses.is_dataclass(v):
                plain[k] = dataclasses.asdict(v)
            elif hasattr(v, '__dict__') and not callable(v):
                plain[k] = str(v)  # 兜底：转字符串
    except Exception as e:
        plain = {"_error": f"could not convert: {e}", "_repr": repr(signed)}

# ── 输出 ─────────────────────────────────────────────────────────────────────
print("=== INPUTS ===")
print(f"  eoa_addr      : {signer.address()}")
print(f"  funder/maker  : {PROXY_FUNDER}")
print(f"  chain_id      : {CHAIN_ID}")
print(f"  token_id      : {TOKEN_ID}")
print(f"  price         : {PRICE}")
print(f"  size          : {SIZE}")
print(f"  side          : {SIDE}")
print(f"  signature_type: 1 (POLY_PROXY)")
print()

print("=== SIGNED ORDER (dataclass dump, NOT what hits server) ===")
print(json.dumps(plain, indent=2, default=str))
print()

# 真实发到 server 的 body — 用 SDK 自带的 order_to_json_v2 函数包装
from py_clob_client_v2.client import order_to_json_v2
from py_clob_client_v2.clob_types import OrderType

OWNER_API_KEY = "00000000-0000-0000-0000-000000000000"  # 占位，实际是 R5 derive 的 UUID

wrapped = order_to_json_v2(signed, OWNER_API_KEY, OrderType.GTC, post_only=False, defer_exec=False)
print("=== WRAPPED body (REAL POST /order payload) ===")
print(json.dumps(wrapped, indent=2, default=str))
print()

print("=== WRAPPED COMPACT JSON ===")
print(json.dumps(wrapped, separators=(",", ":"), ensure_ascii=False, default=str))
print()

print("=== INNER ORDER COMPACT (matches what polyorder_to_json should output) ===")
print(json.dumps(wrapped["order"], separators=(",", ":"), ensure_ascii=False, default=str))
