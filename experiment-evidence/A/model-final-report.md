# 新题 A：模型最终回复原文

来源会话：`ecaaf16d-3020-47ad-9fbb-297c7f5a3932`。模型自报结果，未独立验收。

所有修改完成且验证通过。最终总结：

## 诊断到的根因

| 位置 | 缺陷 | 后果 |
|---|---|---|
| `internal_transfer_process` HEAD 准入 | 共享模式用 `GetCurrentSize(data_type)`（单类型已提交量），而非整池已提交量 | 已有 INPUT 64B 时，WEIGHT 64B HEAD 仍被接收（`shared_committed`）；INPUT/OUTPUT 各 64B 在途也同时被接受（`shared_inflight`） |
| 预留只有 main(INPUT+WEIGHT 混算)/output 两个桶 | 独立模式下 WEIGHT 的准入计入了 INPUT 的预留 | `independent` 场景中 WEIGHT 被误阻塞 |
| TAIL 提交 | 无视 `OnDataReceived` 返回值，失败也 `Pop()` 并释放预留 | TAIL 被消费但数据未入库、预留提前消失 |
| `can_accept_direct_packet` | 完全不计在途预留、共享模式只看单类型已提交量 | `direct_committed`/`direct_reserved` 虚报可接收 |
| `receive_direct_packet` | 无视提交返回值，恒返回 true | 报告成功但实际提交失败 |

## 修改内容（仅限接收容量计账 + BufferManager 查询）

**`src/smartbuffer/BufferManager.{h,cpp}`** — 新增只读查询，不改动既有提交/删除逻辑与容量：
- `GetMode()`；
- `CanReserve(type, size, inflight)`：共享模式按 `全池已提交 + 全池在途预留 + 本次 ≤ 总容量`；独立模式按 `该类型已提交 + 该类型在途预留 + 本次 ≤ 该类型容量`。

**`src/ProcessingElement.{h,cpp}`**：
- 新增按类型的 `inflight_reserved_by_type_`（同步维护既有 `main_receiving_size_`/`output_receiving_size_` 统计口径，原 7 个断言不变）；
- 新增 `add/release_inflight_reservation`、`inflight_reservation_for_admission`（共享汇总全池、独立只取本类型）、`can_reserve_receive`；
- HEAD 准入走统一规则，成功才预留并出队；失败保留队首背压、不丢包不预留；
- TAIL 先提交，**提交失败则保留队首且不释放预留**（下次 `internal_transfer_process` 重试），成功才释放预留并 `Pop()`，杜绝吞数据与重复入库；
- `can_accept_direct_packet`/`receive_direct_packet` 与物理 VC 路径共用 `can_reserve_receive`（直接提交因此尊重已有在途预留），并以 `OnDataReceived` 实际返回值为准；`command == -1` 回送语义完全不变；
- reset 路径清零新映射。未修改 Router、未扩大容量、未改丢包/强制提交策略。

## 新增回归（tests/receive_capacity_tests.cpp + 脚本，共 12 场景）

- `interleave`：共享池跨 VC HEAD/HEAD/TAIL 交错，验证 32+32 在途时 33B 被拒、32B 恰好准入，最终 96B 全入库、预留归零；
- `resume`：HEAD 被背压时不预留（无泄漏），腾出容量后同周期重试成功，TAIL 正常入库；
- `direct_mix`：物理预留与直接提交交错（64 在途 + 直接 32 成功、再 +1 失败且 OUTPUT 未被污染），TAIL 后池满仍拒直接提交，驱逐后恢复；
- `independent_direct`：独立池 INPUT 预留不阻塞 WEIGHT/OUTPUT，各池独立限额；
- `tail_commit_retry`：TAIL 提交失败时队首保留、预留保留，腾容量后重试且**只入库一次**。

## 实际验证结果

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug     # 干净重建，0 error
cmake --build build --target noxim run_tests receive_capacity_tests -j 4
./build/run_tests                                         # All tests passed successfully!
./tests/run_receive_capacity.sh ./build/receive_capacity_tests
# 原 7 场景 sanity/shared_committed/shared_inflight/independent/
# independent_same/direct_committed/direct_reserved 全部 PASS
# 新 5 场景 interleave/resume/direct_mix/independent_direct/
# tail_commit_retry 全部 PASS；脚本 exit=0
```

每个入库场景均断言每包只提交一次（BufferManager 计数精确匹配）且结束时 `main_receiving_size_ == output_receiving_size_ == 0`。
