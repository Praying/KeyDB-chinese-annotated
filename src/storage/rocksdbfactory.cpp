#include "rocksdb.h"
#include "../version.h"
#include <rocksdb/filter_policy.h>
#include <rocksdb/table.h>
#include <rocksdb/utilities/options_util.h>
#include <rocksdb/sst_file_manager.h>
#include <rocksdb/utilities/convenience.h>
#include <rocksdb/slice_transform.h>
#include "rocksdbfactor_internal.h"
#include <sys/types.h>
#include <sys/stat.h> 
#include <sys/statvfs.h>

/**
 * @brief 创建并返回默认的 RocksDB 配置选项
 *
 * 该函数初始化一组针对通用场景优化的 RocksDB 数据库配置参数，
 * 包含后台任务控制、数据持久化策略、存储格式配置等核心设置。
 *
 * @return rocksdb::Options 返回配置好的数据库选项实例
 */
rocksdb::Options DefaultRocksDBOptions() {
    rocksdb::Options options;

    // 配置后台任务并发度
    options.max_background_compactions = 4;  // 最大并发压缩任务数
    options.max_background_flushes = 2;      // 最大并发刷盘任务数

    // 配置数据持久化策略
    options.bytes_per_sync = 1048576;        // 每1MB写入触发一次同步

    // 配置压缩策略与数据布局
    options.compaction_pri = rocksdb::kMinOverlappingRatio;  // 使用最小重叠率压缩策略
    options.compression = rocksdb::kNoCompression;           // 禁用数据压缩

    // 启用高性能特性
    options.enable_pipelined_write = true;   // 启用流水线写入优化
    options.allow_mmap_reads = true;         // 允许使用内存映射读取
    options.avoid_unnecessary_blocking_io = true;  // 避免不必要的阻塞IO操作

    // 配置前缀迭代优化器
    options.prefix_extractor.reset(rocksdb::NewFixedPrefixTransform(0));  // 使用固定长度前缀提取器

    /**
     * 配置基于块的存储格式参数：
     * - 16KB数据块大小平衡IO效率与内存占用
     * - 启用索引/过滤块缓存提升热点数据访问速度
     * - L0层索引/过滤块常驻内存减少冷启动延迟
     * - 混合索引格式优化查找性能
     * - 格式版本4支持现代存储特性
     */
    rocksdb::BlockBasedTableOptions table_options;
    table_options.block_size = 16 * 1024;
    table_options.cache_index_and_filter_blocks = true;
    table_options.pin_l0_filter_and_index_blocks_in_cache = true;
    table_options.data_block_index_type = rocksdb::BlockBasedTableOptions::kDataBlockBinaryAndHash;
    table_options.checksum = rocksdb::kNoChecksum;
    table_options.format_version = 4;

    // 应用存储格式配置到数据库选项
    options.table_factory.reset(NewBlockBasedTableFactory(table_options));

    return options;
}


IStorageFactory *CreateRocksDBStorageFactory(const char *path, int dbnum, const char *rgchConfig, size_t cchConfig)
{
    return new RocksDBStorageFactory(path, dbnum, rgchConfig, cchConfig);
}

/**
 * @brief 构造并返回配置好的RocksDB选项对象
 *
 * 该函数基于默认选项进行定制化配置，用于初始化RocksDB数据库实例。
 *
 * @return rocksdb::Options 配置好的RocksDB选项对象
 */
rocksdb::Options RocksDBStorageFactory::RocksDbOptions()
{
    rocksdb::Options options = DefaultRocksDBOptions();

    // 设置最大打开文件数限制为字段需求函数返回值
    options.max_open_files = filedsRequired();

    // 绑定SST文件管理器为成员变量持有的文件管理器实例
    options.sst_file_manager = m_pfilemanager;

    // 启用数据库自动创建功能
    options.create_if_missing = true;

    // 启用列族自动创建功能
    options.create_missing_column_families = true;

    // 设置日志输出级别为错误级别（仅记录错误信息）
    options.info_log_level = rocksdb::ERROR_LEVEL;

    // 配置WAL日志总大小上限为1GB
    options.max_total_wal_size = 1 * 1024 * 1024 * 1024;

    return options;
}

/**
 * RocksDBStorageFactory 构造函数
 * 初始化 RocksDB 存储实例，创建指定数量的列族并验证数据库版本
 *
 * 参数:
 *   dbfile - 数据库存储路径
 *   dbnum  - 用户请求的列族数量（实际会+1用于元数据）
 *   rgchConfig - RocksDB 配置选项字符串
 *   cchConfig  - 配置字符串长度
 *
 * 抛出:
 *   std::string 异常描述（数据库打开失败或版本不兼容）
 */
RocksDBStorageFactory::RocksDBStorageFactory(const char *dbfile, int dbnum, const char *rgchConfig, size_t cchConfig)
    : m_path(dbfile)
{
    // 为元数据预留额外的列族空间
    dbnum++;

    // 获取数据库实际存在的列族数量
    std::vector<std::string> vecT;
    auto status = rocksdb::DB::ListColumnFamilies(rocksdb::Options(), dbfile, &vecT);
    // 确保分配的列族数量能容纳现有数据（至少保留两倍冗余）
    if (status.ok() && (int)vecT.size()/2 > dbnum)
        dbnum = (int)vecT.size()/2;

    // 初始化列族描述容器（跳过默认列族）
    std::vector<rocksdb::ColumnFamilyDescriptor> veccoldesc;
    veccoldesc.push_back(rocksdb::ColumnFamilyDescriptor(rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions()));

    // 创建 SST 文件管理器
    m_pfilemanager = std::shared_ptr<rocksdb::SstFileManager>(rocksdb::NewSstFileManager(rocksdb::Env::Default()));

    rocksdb::DB *db = nullptr;

    // 配置基础数据库选项
    auto options = RocksDbOptions();
    options.prefix_extractor.reset(rocksdb::NewFixedPrefixTransform(HASHSLOT_PREFIX_BYTES));

    // 创建指定数量的主列族及其过期时间辅助列族
    for (int idb = 0; idb < dbnum; ++idb)
    {
        rocksdb::ColumnFamilyOptions cf_options(options);
        cf_options.level_compaction_dynamic_level_bytes = true;
        veccoldesc.push_back(rocksdb::ColumnFamilyDescriptor(std::to_string(idb), cf_options));
        veccoldesc.push_back(rocksdb::ColumnFamilyDescriptor(std::to_string(idb) + "_expires", cf_options));
    }

    // 应用用户提供的配置选项
    if (rgchConfig != nullptr)
    {
        std::string options_string(rgchConfig, cchConfig);
        rocksdb::Status status;
        if (!(status = rocksdb::GetDBOptionsFromString(options, options_string, &options)).ok())
        {
            fprintf(stderr, "Failed to parse FLASH options: %s\r\n", status.ToString().c_str());
            exit(EXIT_FAILURE);
        }
    }

    // 打开数据库并获取列族句柄
    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    status = rocksdb::DB::Open(options, dbfile, veccoldesc, &handles, &db);
    if (!status.ok())
        throw status.ToString();

    // 保存数据库实例并分类处理列族句柄
    m_spdb = std::shared_ptr<rocksdb::DB>(db);
    for (auto handle : handles)
    {
        // 过期时间列族标记处理
        if (handle->GetName().size() > 7 && !strncmp(handle->GetName().substr(handle->GetName().size() - 7).c_str(), "expires", 7)) {
            m_vecspexpirecols.emplace_back(handle);
        }
        // 主列族版本验证逻辑
        else {
            std::string strVersion;
            auto status = m_spdb->Get(rocksdb::ReadOptions(), handle, rocksdb::Slice(version_key, sizeof(version_key)), &strVersion);
            if (!status.ok())
            {
                setVersion(handle);
            }
            else
            {
                SymVer ver = parseVersion(strVersion.c_str());
                auto cmp = compareVersion(&ver);
                if (cmp == NewerVersion)
                    throw "Cannot load FLASH database created by newer version of KeyDB";
                if (cmp == IncompatibleVersion)
                    throw "Cannot load FLASH database from before 6.3.4";
                if (cmp == OlderVersion)
                    setVersion(handle);
            }
            m_vecspcols.emplace_back(handle);
        }
    }
}


RocksDBStorageFactory::~RocksDBStorageFactory()
{
    m_spdb->SyncWAL();
}

/**
 * @brief 设置当前存储的版本信息到指定的ColumnFamily中
 *
 * 该函数通过RocksDB的Put操作将版本标识写入底层存储系统，用于标识当前数据库的版本状态。
 * 如果写入操作失败，会抛出包含错误信息的字符串异常。
 *
 * @param handle 指向ColumnFamilyHandle的指针，指定要写入版本信息的列族
 *
 * @throws std::string 当RocksDB写入操作返回非OK状态时，抛出包含错误描述的字符串
 */
void RocksDBStorageFactory::setVersion(rocksdb::ColumnFamilyHandle *handle)
{
    /**
     * 执行版本信息写入操作：
     * 使用默认WriteOptions将版本键值对写入RocksDB
     * version_key作为存储键，KEYDB_REAL_VERSION作为带空终止符的字符串值
     */
    auto status = m_spdb->Put(rocksdb::WriteOptions(), handle,
                             rocksdb::Slice(version_key, sizeof(version_key)),
                             rocksdb::Slice(KEYDB_REAL_VERSION, strlen(KEYDB_REAL_VERSION)+1));

    /**
     * 检查写入操作状态：
     * 如果返回状态不是OK，将错误信息转换为字符串并抛出
     */
    if (!status.ok())
        throw status.ToString();
}


size_t RocksDBStorageFactory::filedsRequired() const {
    return 256;
}

std::string RocksDBStorageFactory::getTempFolder()
{
    auto path = m_path + "/keydb_tmp/";
    if (!m_fCreatedTempFolder) {
        if (!mkdir(path.c_str(), 0700))
            m_fCreatedTempFolder = true;
    }
    return path;
}

IStorage *RocksDBStorageFactory::createMetadataDb()
{
    IStorage *metadataDb = this->create(-1, nullptr, nullptr);
    metadataDb->insert(meta_key, sizeof(meta_key), (void*)METADATA_DB_IDENTIFIER, strlen(METADATA_DB_IDENTIFIER), false);
    return metadataDb;
}

IStorage *RocksDBStorageFactory::create(int db, key_load_iterator iter, void *privdata)
{
    ++db;   // skip default col family
    std::shared_ptr<rocksdb::ColumnFamilyHandle> spcolfamily(m_vecspcols[db].release());
    std::shared_ptr<rocksdb::ColumnFamilyHandle> spexpirecolfamily(m_vecspexpirecols[db].release());
    size_t count = 0;
    bool fUnclean = false;
    
    std::string value;
    auto status = m_spdb->Get(rocksdb::ReadOptions(), spcolfamily.get(), rocksdb::Slice(count_key, sizeof(count_key)), &value);
    if (status.ok() && value.size() == sizeof(size_t))
    {
        count = *reinterpret_cast<const size_t*>(value.data());
        m_spdb->Delete(rocksdb::WriteOptions(), spcolfamily.get(), rocksdb::Slice(count_key, sizeof(count_key)));
    }
    else
    {
        fUnclean = true;
    }
    
    if (fUnclean || iter != nullptr)
    {
        count = 0;
        auto opts = rocksdb::ReadOptions();
        opts.tailing = true;
        std::unique_ptr<rocksdb::Iterator> it = std::unique_ptr<rocksdb::Iterator>(m_spdb->NewIterator(opts, spcolfamily.get()));

        it->SeekToFirst();
        bool fFirstRealKey = true;
        
        for (;it->Valid(); it->Next()) {
            if (FInternalKey(it->key().data(), it->key().size()))
                continue;
            if (fUnclean && it->Valid() && fFirstRealKey)
                printf("\tDatabase %d was not shutdown cleanly, recomputing metrics\n", db);
            fFirstRealKey = false;
            if (iter != nullptr)
                iter(it->key().data()+HASHSLOT_PREFIX_BYTES, it->key().size()-HASHSLOT_PREFIX_BYTES, privdata);
            ++count;
        }
    }
    return new RocksDBStorageProvider(this, m_spdb, spcolfamily, spexpirecolfamily, nullptr, count);
}

const char *RocksDBStorageFactory::name() const
{
    return "flash";
}

size_t RocksDBStorageFactory::totalDiskspaceUsed() const
{
    return m_pfilemanager->GetTotalSize();
}

sdsstring RocksDBStorageFactory::getInfo() const
{
    struct statvfs fiData;
    int status = statvfs(m_path.c_str(), &fiData);
    if ( status == 0 ) {
        return sdsstring(sdscatprintf(sdsempty(),
            "storage_flash_used_bytes:%zu\r\n"
            "storage_flash_total_bytes:%zu\r\n"
            "storage_flash_rocksdb_bytes:%zu\r\n",
            fiData.f_bfree * fiData.f_frsize,
            fiData.f_blocks * fiData.f_frsize,
            totalDiskspaceUsed()));
    } else {
        fprintf(stderr, "Failed to gather FLASH statistics with status: %d\r\n", status);
        return sdsstring(sdsempty());
    }
}
