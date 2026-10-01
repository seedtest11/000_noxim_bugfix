接收容量一致性实验材料

A/B 最终提交由收集方创建，仅提交模型修改的六个交付文件，未修改实现。
repository.bundle 包含 Git 历史，可使用 git clone A/repository.bundle restored-A 恢复。source.tar.gz 为对应代码快照。
恢复后将 environment/ 内 flake.nix、flake.lock、.envrc 复制到工作目录，在 nix develop "path:$PWD" 环境中按 PROMPT.md 构建验收。
原始 trajectory.jsonl 与 model-final-report.md 保留模型原文。录屏为各约45秒的实际验收展示。
run_demo.sh 是展示脚本，其预编译 bin 依赖未包含；从源码复现应使用 PROMPT.md 的构建与验收命令。
材料不含 API 凭据、客户端配置或人工 GSB 评价。远端发布待确定。
