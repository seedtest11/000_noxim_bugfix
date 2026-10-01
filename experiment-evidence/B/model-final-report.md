# 新题 B：模型最终回复原文

来源会话：`12b0230e-2efc-429e-a11c-e1b15235e6a2`。模型自报结果，未独立验收。

修复完成，全部验收通过。

## 诊断

基线运行确认 7 个场景中 5 个失败，根因均在接收容量计账（`src/ProcessingElement.cpp`）：

1. **HEAD 准入按单类型统计**：用 `GetCurrentSize(data_type)` 而非整池量，共享 96 字节池已有 INPUT 64 时，WEIGHT 64 的 HEAD 仍被接收。
2. **预留只有 `main`(INPUT+WEIGHT)/`output`(OUTPUT) 两个桶**：共享模式下 INPUT 与 OUTPUT 在途互不可见（64+64 同时被接受）；独立模式下 WEIGHT 又被 INPUT 的 main 桶误阻塞。
3. **TAIL 忽略 `OnDataReceived` 返回值**：提交失败照样 Pop flit 并释放预留 → 数据未入库却被消费（丢包）+ 预留泄漏。
4. **direct 路径只看已提交量**：不看物理路径的在途预留，且提交失败也返回 true。

## 改动（6 个文件，未动 Router）

- **`src/smartbuffer/BufferManager.{h,cpp}`**：新增 `GetMode()` 查询，供 PE 区分共享/独立口径。
- **`src/ProcessingElement.h`**：新增按类型的唯一事实来源 `inflight_reserved_by_type_`；`main_/output_receiving_size_` 保留为其派生视图（测试仍可读）；新增 6 个准入/预留 helper。
- **`src/ProcessingElement.cpp`**：
  - `has_capacity_for()` 统一不变量：**共享** = 整池已提交 + 全部类型在途预留 + 本次 ≤ 整池容量；**独立** = 同类型已提交 + 同类型在途预留 + 本次 ≤ 该类型容量。
  - HEAD：通过准入才 Pop 并按整包预留；不足则保留队首背压（VC 级 HOL）。
  - TAIL：先检查 `OnDataReceived` 返回值——成功才释放预留并 Pop（每包只入库一次）；失败则保留队首、保留预留，容量腾出后下一轮自动重试，不丢包、不提前释放。
  - `can_accept_direct_packet`/`receive_direct_packet` 改用同一 `has_capacity_for`（尊重物理在途预留），直接提交检查返回值，失败返回 false；`command == -1` 回送语义原样保留（HEAD 无条件、TAIL 只累加计数、direct 返回 true，均不经 BufferManager）。
  - reset 路径清空全部类型预留。

## 新增回归（`tests/receive_capacity_tests.cpp` + 运行脚本，共 13 场景）

保留原 7 个断言，新增：`cross_vc_interleave`（跨 VC HEAD/TAIL 交错）、`recover_after_free`（释放后背压恢复）、`direct_reserved_interleave`（直接提交与物理预留交错，含边界 32/33）、`independent_direct_other_pool`（独立池不互斥 + 同池背压恢复）、`tail_commit_retry`（TAIL 提交失败保留队首、重试不重复入库）、`return_command_special`（command==-1 特殊语义 + 池满仍接受）。每个场景都断言最终预留归零。

## 验证结果（nix develop 内，C++17，clean 构建）

- `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug`：成功
- `cmake --build build --target noxim run_tests receive_capacity_tests -j 4`：成功
- `./build/run_tests`：exit 0，All tests passed
- `./tests/run_receive_capacity.sh ./build/receive_capacity_tests`：exit 0，**13/13 PASS，无 FAIL**
