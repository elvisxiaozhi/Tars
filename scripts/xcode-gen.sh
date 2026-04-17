#!/bin/bash
# 生成 Xcode 项目并打开
set -e
cd "$(dirname "$0")/.."

echo "Generating Xcode project..."
cmake -B build-xcode -G Xcode

echo "Opening Xcode..."
open build-xcode/polymarket-arb.xcodeproj
