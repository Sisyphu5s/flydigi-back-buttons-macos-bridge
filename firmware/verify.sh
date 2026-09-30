#!/bin/sh
# verify.sh — 写固件前的快速验证（纯主机，不需要硬件、不碰系统）
#   1 生成描述符  2 独立解析  3 与苹果模型一致性  4 Python 端到端 dry-run
#   5 C 编译+自测  6 C 与 Python 逐字节交叉比对  7 输入侧解析/映射（真机帧）
#   8 RP2350 固件编译 + USB 描述符离线校验（缺工具链则跳过）
set -e
cd "$(dirname "$0")"
mkdir -p build

echo "=== 1) 生成描述符 ==="
python3 hid_desc_vader2pro.py
python3 hid_desc_sensor.py >/dev/null

echo
echo "=== 2) 独立解析描述符（自研解析器，不用生成器的假设）==="
python3 hid_parse.py > build/parse.txt
tail -3 build/parse.txt

echo
echo "=== 3) 与苹果模型文件一致性（两种 index 解释）==="
python3 model_conform.py > build/conform.txt || { tail -6 build/conform.txt; echo "一致性检查失败 ✘"; exit 1; }
sed -n '/Driver identifier/,$p' build/conform.txt | tail -8
python3 scuf_model_conform.py

echo
echo "=== 4) Python 端到端 dry-run（报文→解析→模型→GC 元素）==="
python3 dryrun_report.py > build/dryrun.txt || { tail -8 build/dryrun.txt; exit 1; }
grep -E '报文:|位级往返|背键结论|dry-run:' build/dryrun.txt

echo
echo "=== 5) C 侧编译 + 自测（clang，主机）==="
clang -std=c11 -Wall -Wextra -O2 -o build/report_pack_test \
      test/report_pack_test.c src/report_pack.c -I. -Isrc
./build/report_pack_test > build/ctest.txt || { cat build/ctest.txt; exit 1; }
grep -E '^REPORT:|结果:' build/ctest.txt

clang -std=c11 -Wall -Wextra -Werror -O2 -o build/rumble_report_test \
      test/rumble_report_test.c src/rumble_report.c -Isrc
./build/rumble_report_test

clang -std=c11 -Wall -Wextra -Werror -O2 -o build/bridge_timing_test \
      test/bridge_timing_test.c src/bridge_timing.c -Isrc
./build/bridge_timing_test

clang -std=c11 -Wall -Wextra -Werror -O2 -o build/pid_probe_report_test \
      test/pid_probe_report_test.c src/pid_probe_report.c -Isrc
./build/pid_probe_report_test

clang -std=c11 -Wall -Wextra -Werror -O2 -o build/bridge_config_test \
      test/bridge_config_test.c src/bridge_config.c -Isrc
./build/bridge_config_test
clang -std=c11 -Wall -Wextra -Werror -O2 -DGENERIC_GAMEPAD -o build/bridge_config_generic_test \
      test/bridge_config_test.c src/bridge_config.c -Isrc
./build/bridge_config_generic_test
clang -std=c11 -Wall -Wextra -Werror -O2 -DGENERIC_GAMEPAD -DCORSAIR_APPLE_IDENTITY -o build/bridge_config_scuf_test \
      test/bridge_config_test.c src/bridge_config.c -Isrc
./build/bridge_config_scuf_test
clang -std=c11 -Wall -Wextra -Werror -O2 -pthread -o build/bridge_config_concurrency_test \
      test/bridge_config_concurrency_test.c src/bridge_config.c -Isrc
./build/bridge_config_concurrency_test

echo
echo "=== 6) C 与 Python 交叉比对 ==="
PY_DESC=$(tr -s ' \n' ' ' < desc_vader2pro.hex | sed 's/ *$//')
C_DESC=$(sed -n 's/^DESC: //p' build/ctest.txt)
if [ "$PY_DESC" = "$C_DESC" ]; then echo "描述符逐字节一致 ✔ (${#C_DESC} 字符)"; else echo "描述符不一致 ✘"; exit 1; fi

PY_REP=$(grep -m1 '报文:' build/dryrun.txt | sed 's/.*报文: //; s/ (.*//')
C_REP=$(sed -n 's/^REPORT: //p' build/ctest.txt)
if [ "$PY_REP" = "$C_REP" ]; then echo "场景报文逐字节一致 ✔ ($C_REP)"; else echo "报文不一致 ✘ py=[$PY_REP] c=[$C_REP]"; exit 1; fi

echo
echo "=== 7) 输入侧解析 + 命令表 + 映射（用真机抓到的扩展帧）==="
clang -std=c11 -Wall -Wextra -Werror -O2 -o build/flydigi_test \
      test/flydigi_test.c src/flydigi_rx.c src/bridge_map.c src/bridge_config.c src/report_pack.c -Isrc
./build/flydigi_test > build/flydigi.txt || { tail -12 build/flydigi.txt; exit 1; }
grep -E '加速度 Z|回报 |M1–M4\+A|PASS:|FAIL:' build/flydigi.txt
clang -std=c11 -Wall -Wextra -Werror -O2 -DGENERIC_GAMEPAD -o build/flydigi_generic_test \
      test/flydigi_test.c src/flydigi_rx.c src/bridge_map.c src/bridge_config.c src/report_pack.c -Isrc
./build/flydigi_generic_test > build/flydigi_generic.txt || { tail -12 build/flydigi_generic.txt; exit 1; }
grep -E '回报 |M1–M4\+A|PASS:|FAIL:' build/flydigi_generic.txt
clang -std=c11 -Wall -Wextra -Werror -O2 -DGENERIC_GAMEPAD -DCORSAIR_APPLE_IDENTITY -o build/flydigi_scuf_test \
      test/flydigi_test.c src/flydigi_rx.c src/bridge_map.c src/bridge_config.c src/report_pack.c -Isrc
./build/flydigi_scuf_test > build/flydigi_scuf.txt || { tail -12 build/flydigi_scuf.txt; exit 1; }
grep -E 'SCUF-order|PASS:|FAIL:' build/flydigi_scuf.txt

echo
echo "=== 8) RP2350 固件编译 + USB 描述符离线校验 ==="
TC_BIN="$HOME/pico/toolchain/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi/bin"
if [ -x "$TC_BIN/arm-none-eabi-gcc" ] && [ -n "$PICO_SDK_PATH" ] && [ -d "$PICO_SDK_PATH" ]; then
    PATH="$TC_BIN:$PATH" cmake -B rp2350/build -S rp2350 -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    PATH="$TC_BIN:$PATH" ninja -C rp2350/build >/dev/null && echo "固件编译 ✔ $(ls -l rp2350/build/flydigi_bridge.uf2 | awk '{print $5" 字节"}')"
    python3 rp2350/test/usb_desc_check.py rp2350/build/flydigi_bridge.elf | tail -5
    PATH="$TC_BIN:$PATH" cmake -B rp2350/build-apple-compatible -S rp2350 -G Ninja \
        -DAPPLE_COMPATIBLE_PROFILE=ON -DCMAKE_BUILD_TYPE=Release >/dev/null
    PATH="$TC_BIN:$PATH" ninja -C rp2350/build-apple-compatible >/dev/null && \
        echo "Apple 兼容固件编译 ✔ $(ls -l rp2350/build-apple-compatible/flydigi_bridge.uf2 | awk '{print $5" 字节"}')"
    python3 rp2350/test/usb_desc_check.py --generic-apple \
        rp2350/build-apple-compatible/flydigi_bridge.elf | tail -5
    PATH="$TC_BIN:$PATH" cmake -B rp2350/build-apple-battery -S rp2350 -G Ninja \
        -DAPPLE_COMPATIBLE_PROFILE=ON -DAPPLE_BATTERY_PROBE=ON -DCMAKE_BUILD_TYPE=Release >/dev/null
    PATH="$TC_BIN:$PATH" ninja -C rp2350/build-apple-battery >/dev/null && \
        echo "Apple 电量探针固件编译 ✔ $(ls -l rp2350/build-apple-battery/flydigi_bridge.uf2 | awk '{print $5" 字节"}')"
    python3 rp2350/test/usb_desc_check.py --generic-apple --apple-battery \
        rp2350/build-apple-battery/flydigi_bridge.elf | tail -5
    PATH="$TC_BIN:$PATH" cmake -B rp2350/build-apple-battery-interface -S rp2350 -G Ninja \
        -DAPPLE_COMPATIBLE_PROFILE=ON -DAPPLE_BATTERY_INTERFACE_PROBE=ON -DCMAKE_BUILD_TYPE=Release >/dev/null
    PATH="$TC_BIN:$PATH" ninja -C rp2350/build-apple-battery-interface >/dev/null && \
        echo "独立 Battery System 探针固件编译 ✔ $(ls -l rp2350/build-apple-battery-interface/flydigi_bridge.uf2 | awk '{print $5" 字节"}')"
    python3 rp2350/test/usb_desc_check.py --generic-apple --apple-battery-interface \
        rp2350/build-apple-battery-interface/flydigi_bridge.elf | tail -6
    SKIP8=""
else
    echo "跳过（未找到工具链或 PICO_SDK_PATH）—— 设好 PICO_SDK_PATH 后重跑"
    SKIP8="（第 8 步已跳过）"
fi

echo
echo "全部验证通过 ✔（未覆盖：苹果侧是否真套用该模型、接收器挂载与扩展帧、板子固件替换后的真机端到端）$SKIP8"
