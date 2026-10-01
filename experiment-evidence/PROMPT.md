请修复这个 C++/SystemC 层次化 NoC 模拟器中，ProcessingElement 接收数据时的容量预留和提交一致性问题。

真实背景：GLB/DRAM 使用共享 BufferManager，ROLE_BUFFER 按 INPUT/WEIGHT/OUTPUT 分别使用独立容量。多个 VC 的 HEAD/TAIL 可交错到达，ideal_transport 还会直接提交整个包。容量判断必须同时计入已提交数据和已接收 HEAD、尚未提交 TAIL 的数据。

提供的真实 PE 复现显示：共享容量 96 字节、已有 INPUT 64 字节时，WEIGHT 64 字节 HEAD 仍被接收，随后 TAIL 被消费而 WEIGHT 数据未入库；共享模式在途 INPUT 64 与 OUTPUT 64 同样会被同时接受。独立模式 INPUT 与 WEIGHT 各有 64 字节容量时，第二种类型却被第一种类型的预留误阻塞。直接接收路径也会报告接受成功而实际提交失败，或侵占已预留容量。

核心要求：
1. 建立正确的容量准入不变量：共享模式按整个池的已提交量与在途预留判断；独立模式仅按对应类型的已提交量与预留判断。同池不能超额接收，不同独立池不能互相误阻塞。
2. 普通包 HEAD 成功接收时预留、TAIL 成功提交时释放预留；背压保留队首数据，腾出容量后可继续，不能丢包、重复入库、预留泄漏或提前释放。提交失败必须正确处理，不能忽略返回值并消费数据。
3. can_accept_direct_packet / receive_direct_packet 与物理 VC 路径使用一致的容量规则，直接提交必须尊重已有在途预留；返回成功须代表实际提交成功。保持 command == -1 的输出回送特殊语义。
4. 保持现有数据就绪、驱逐、发送、握手和包顺序语义。修改范围仅限接收容量计账及必要的 BufferManager 查询/关联状态，不修改 Router，不扩大容量、不删除测试、不改成丢弃包或强制提交。

环境已固定：本地 flake.nix、flake.lock、.envrc 不进入 Git。若尚未在环境中，使用 nix develop "path:$PWD" --command bash。按 C++17 构建。

构建与验收：
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
  cmake --build build --target noxim run_tests receive_capacity_tests -j 4
  ./build/run_tests
  ./tests/run_receive_capacity.sh ./build/receive_capacity_tests

测试直接调用真实 ProcessingElement 的接收方法，不是完整网络时序仿真。七个场景为 sanity、shared_committed、shared_inflight、independent、independent_same、direct_committed、direct_reserved。保持这些断言，修复后全部 PASS；自行补充跨 VC 的 HEAD/TAIL 交错、容量释放后恢复接收和直接提交与物理预留交错的必要回归，验证每包只入库一次、最终预留归零。

请首轮独立完成诊断、修复、测试，在最终回复中说明改动与实际验证结果。不要读取其他实验目录或复用其他会话的产物。
