# Step 2.5a — 移除波动率入场条件

## 概要

移除 `strategy.cpp` 中的波动率条件（§一.3），因为 K线初期波动率天然为 0，与早期入场窗口（剩余 45+ 分钟）矛盾，导致策略永远无法在最佳时机入场。

## 修改内容

| 操作 | 文件 |
|------|------|
| 修改 | `src/core/strategy.cpp` — 移除 `current_1h_vol <= avg_24h_vol` 条件 |

## 改动详情

**移除前**：
```cpp
// §一.3 波动率条件：当前 > 24h均值
if (btc.current_1h_vol <= btc.avg_24h_vol) {
    sig.reject_reason = "low_vol: ...";
    return sig;
}
```

**移除后**：
```cpp
// §一.3 波动率条件：已移除（K线初期波动率天然为0，与早期入场窗口矛盾）
```

## 原因

1. K线初期（0-5分钟），`high ≈ low ≈ open`，所以 `(high-low)/open ≈ 0`
2. 24h 均值取完整 K线，通常 > 0.003
3. 条件 `current_1h_vol > avg_24h_vol` 在入场窗口（剩余 45+ 分钟）几乎不可能成立
4. 等到波动率积累足够时，剩余时间已不够入场

## 验收结果

```
cmake --build build        # 编译通过，零 warning
./build/polymarket-arb     # 运行正常，tick 完成无异常
```

策略仍有4层过滤：时间窗口 > 30min、方向偏离 > 0.05%、价格 < max_price、红线 < 30¢。
