#ifndef LIVENESS_AWARE_BUFFER_H
#define LIVENESS_AWARE_BUFFER_H

#include <vector>
#include <map>
#include <cstddef>   // for size_t
#include <algorithm> // for std::min
#include "../DataStructs.h"

/**
 * @file BufferManager.h
 * @brief 定义了一个支持两种模式的智能Buffer模块。
 *
 * 支持模式：
 * 1. SHARED: 所有数据类型共享总容量（DRAM/GLB）
 * 2. INDEPENDENT: 每种数据类型有独立容量（PE）
 */

// --- 核心类型定义 ---

/**
 * @brief 缓冲区模式枚举
 */
enum class BufferMode
{
    SHARED,     // DRAM/GLB: 所有数据类型共享总容量
    INDEPENDENT // PE: 每种数据类型有独立容量
};

// --- 智能Buffer模块 ---

class BufferManager
{
public:
    // 共享模式构造函数（DRAM/GLB）
    BufferManager(size_t capacity);

    // 独立模式构造函数（PE）
    BufferManager(const std::map<DataType, size_t> &type_capacities);

    bool OnDataReceived(DataType type, size_t size_added);

    /**
     * @brief 检查一组必需的数据类型当前是否都存在于缓冲区中。
     */
    bool AreDataTypesReady(const std::vector<DataType> &required_types, size_t size) const;
    bool AreDataTypeReady(const DataType &required_types, size_t size) const;
    bool RemoveData(DataType type, size_t size);

    /**
     * @brief 获取容量
     * @param type 数据类型，仅在INDEPENDENT模式下有效
     * @return 共享模式返回总容量，独立模式返回特定类型容量
     */
    size_t GetCapacity(DataType type = DataType::UNKNOWN) const;

    /**
     * @brief 获取当前使用量
     * @param type 数据类型，UNKNOWN表示总使用量
     */
    size_t GetCurrentSize(DataType type = DataType::UNKNOWN) const;

    size_t GetDataSize(DataType type) const;

    /**
     * @brief 获取缓冲区模式
     */
    BufferMode GetMode() const;

    /**
     * @brief 统一的容量准入判断。
     *
     * 同时计入已提交数据与调用方持有的在途预留（已接收 HEAD、尚未提交
     * TAIL 的数据）。
     * - SHARED 模式：inflight_reservation 必须是整个池所有类型的在途预留
     *   总和，与全池已提交量之和不得超过总容量。
     * - INDEPENDENT 模式：inflight_reservation 必须是 type 对应独立池的
     *   在途预留，与该类型已提交量之和不得超过该类型容量。
     *
     * @param type 数据包的数据类型
     * @param size 本次请求准入的数据大小
     * @param inflight_reservation 与模式匹配的在途预留量
     */
    bool CanReserve(DataType type, size_t size,
                    size_t inflight_reservation) const;

    /**
     * @brief 检查缓冲区是否满载
     * @param type 数据类型，仅在INDEPENDENT模式下有效
     */
    bool IsFull(DataType type = DataType::UNKNOWN) const;

private:
    /**
     * @brief 从缓冲区中驱逐指定类型和大小的数据。
     */
    size_t evict_data(DataType type, size_t size_to_evict);

    BufferMode mode_;
    size_t capacity_; // 共享模式下的总容量
    size_t current_size_;
    std::map<DataType, size_t> internal_buffer_sizes_;
    std::map<DataType, size_t> type_capacities_; // 独立模式下每种类型的容量
};

#endif // LIVENESS_AWARE_BUFFER_