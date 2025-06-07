#pragma once
#include <functional>
#include "sds.h"
#include <string>

#define METADATA_DB_IDENTIFIER "c299fde0-6d42-4ec4-b939-34f680ffe39f"

class IStorageFactory
{
public:
    typedef void (*key_load_iterator)(const char *rgchKey, size_t cchKey, void *privdata);

    virtual ~IStorageFactory() {}
    virtual class IStorage *create(int db, key_load_iterator itr, void *privdata) = 0;
    virtual class IStorage *createMetadataDb() = 0;
    virtual const char *name() const = 0;
    virtual size_t totalDiskspaceUsed() const = 0;
    virtual sdsstring getInfo() const = 0;
    virtual bool FSlow() const = 0;
    virtual size_t filedsRequired() const { return 0; }
};

class IStorage
{
public:
    /// @brief 回调函数类型，用于处理键值对数据
    /// @param key 键的指针
    /// @param cchKey 键的长度（字节数）
    /// @param data 数据的指针
    /// @param cb 数据的大小（字节数）
    /// @return 返回true继续遍历，false终止遍历
    typedef std::function<bool(const char *, size_t, const void *, size_t)> callback;

    /// @brief 单次操作回调函数类型
    /// @param key 键的指针
    /// @param cchKey 键的长度（字节数）
    /// @param data 数据的指针
    /// @param cb 数据的大小（字节数）
    typedef std::function<void(const char *, size_t, const void *, size_t)> callbackSingle;

    /// @brief 虚析构函数，确保派生类对象析构时能正确释放资源
    virtual ~IStorage();

    /// @brief 插入键值对到存储中
    /// @param key 键的指针
    /// @param cchKey 键的长度（字节数）
    /// @param data 数据的指针
    /// @param cb 数据的大小（字节数）
    /// @param fOverwire 是否允许覆盖已存在的键
    virtual void insert(const char *key, size_t cchKey, void *data, size_t cb, bool fOverwire) = 0;

    /// @brief 从存储中删除指定键
    /// @param key 键的指针
    /// @param cchKey 键的长度（字节数）
    /// @return 返回是否成功删除（键存在且被移除）
    virtual bool erase(const char *key, size_t cchKey) = 0;

    /// @brief 检索指定键对应的数据并通过回调处理
    /// @param key 键的指针
    /// @param cchKey 键的长度（字节数）
    /// @param fn 单次回调函数，用于处理检索到的数据
    virtual void retrieve(const char *key, size_t cchKey, callbackSingle fn) const = 0;

    /// @brief 清除存储中的所有元素
    /// @return 返回被清除的元素数量
    virtual size_t clear() = 0;

    /// @brief 遍历存储中的所有键值对并调用回调函数处理
    /// @param fn 回调函数，参数依次为键、键长、值、值长，返回false停止遍历
    /// @return 返回是否成功完成遍历
    virtual bool enumerate(callback fn) const = 0;

    /// @brief 遍历指定哈希槽中的键值对并调用回调函数处理
    /// @param fn 回调函数，参数格式同enumerate
    /// @param hashslot 哈希槽索引
    /// @return 返回是否成功完成遍历
    virtual bool enumerate_hashslot(callback fn, unsigned int hashslot) const = 0;

    /// @brief 获取存储中当前元素的总数
    /// @return 返回元素数量
    virtual size_t count() const = 0;

    /// @brief 批量插入元素的默认实现
    /// @param rgkeys 键数组的指针数组
    /// @param rgcbkeys 各键长度的数组
    /// @param rgvals 数据值数组的指针数组
    /// @param rgcbvals 各数据大小的数组
    /// @param celem 要插入的元素数量
    /// @note 默认实现通过循环调用insert完成插入，begin/endWriteBatch可用于优化批处理性能
    virtual void bulkInsert(char **rgkeys, size_t *rgcbkeys, char **rgvals, size_t *rgcbvals, size_t celem) {
        beginWriteBatch();
        for (size_t ielem = 0; ielem < celem; ++ielem) {
            insert(rgkeys[ielem], rgcbkeys[ielem], rgvals[ielem], rgcbvals[ielem], false);
        }
        endWriteBatch();
    }

    /// @brief 获取指定数量的过期候选键
    /// @param count 请求的候选键数量
    /// @return 返回包含过期候选键的字符串向量
    virtual std::vector<std::string> getExpirationCandidates(unsigned int count) = 0;

    /// @brief 获取指定数量的驱逐候选键
    /// @param count 请求的候选键数量
    /// @return 返回包含驱逐候选键的字符串向量
    virtual std::vector<std::string> getEvictionCandidates(unsigned int count) = 0;

    /// @brief 为指定键设置过期时间
    /// @param key 键的指针
    /// @param cchKey 键的长度（字节数）
    /// @param expire 过期时间戳（单位依赖具体实现）
    virtual void setExpire(const char *key, size_t cchKey, long long expire) = 0;

    /// @brief 移除指定键的过期时间设置
    /// @param key 键的指针
    /// @param cchKey 键的长度（字节数）
    /// @param expire 过期时间戳（可能用于校验）
    virtual void removeExpire(const char *key, size_t cchKey, long long expire) = 0;

    /// @brief 开始写入批处理操作
    /// @note 默认实现为空，派生类可重写以优化批量操作性能
    virtual void beginWriteBatch() {}

    /// @brief 结束写入批处理操作
    /// @note 默认实现为空，派生类可在此提交或刷新批处理数据
    virtual void endWriteBatch() {}

    /// @brief 批处理写入锁操作
    /// @note 默认实现为空，派生类可根据需要实现锁机制
    virtual void batch_lock() {}

    /// @brief 批处理写入解锁操作
    /// @note 默认实现为空，派生类可根据需要实现锁机制
    virtual void batch_unlock() {}

    /// @brief 将所有挂起的更改刷新到持久化存储中
    virtual void flush() = 0;

    /// @brief 创建当前存储对象的浅拷贝
    /// @note 返回的对象由调用者负责释放
    /// @return 返回新创建的IStorage对象指针
    virtual const IStorage *clone() const = 0;
};
