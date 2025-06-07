/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __REDIS_H
#define __REDIS_H

#define TRUE 1
#define FALSE 0

#include "fmacros.h"
#include "config.h"
#include "solarisfixes.h"
#include "rio.h"
#include "atomicvar.h"

#include &lt;cstddef&gt;
#include <concurrentqueue.h>
#include <blockingconcurrentqueue.h>

#include <stdio.h>
#include <stdlib.h>
#include <cmath>
#include <string.h>
#include &lt;cstddef&gt;
#include <string>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <syslog.h>
#include <netinet/in.h>
#include <atomic>
#include <vector>
#include <algorithm>
#include <memory>
#include <set>
#include <map>
#include <string>
#include <mutex>
#include <unordered_set>
#ifdef __cplusplus
extern "C" {
#include <lua.h>
}
#else
#include <lua.h>
#endif
#include <sys/socket.h>
#include <signal.h>

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#endif

typedef long long mstime_t; /* 毫秒时间类型 */
typedef long long ustime_t; /* 微秒时间类型 */

#include "fastlock.h"
#include "ae.h"      /* 事件驱动编程库 */
#include "sds.h"     /* 动态安全字符串 */
#include "dict.h"    /* 哈希表 */
#include "adlist.h"  /* 链表 */
#include "zmalloc.h" /* 带内存总占用统计的 malloc/free */
#include "anet.h"    /* 简化网络开发 */
#include "ziplist.h" /* 紧凑列表数据结构 */
#include "intset.h"  /* 紧凑整数集合结构 */
#include "version.h" /* 版本宏定义 */
#include "util.h"    /* 实用工具函数 */
#include "latency.h" /* 延迟监控API */
#include "sparkline.h" /* ASCII 图形API */
#include "quicklist.h"  /* 列表被编码为 N 元素扁平数组的链表 */
#include "rax.h"     /* 基数树(Radix tree) */
#include "uuid.h"
#include "semiorderedset.h"
#include "connection.h" /* 连接抽象 */
#include "serverassert.h"
#include "expire.h"
#include "readwritelock.h"

#define REDISMODULE_CORE 1
#include "redismodule.h"    /* Redis modules API defines. */

/* Following includes allow test functions to be called from Redis main() */
#include "zipmap.h"
#include "sha1.h"
#include "endianconv.h"
#include "crc64.h"
#include "IStorage.h"
#include "StorageCache.h"
#include "AsyncWorkQueue.h"
#include "gc.h"

#define FImplies(x, y) (!(x) || (y))

#define LOADING_BOOT 1
#define LOADING_REPLICATION 2

#define OVERLOAD_PROTECT_PERIOD_MS 10'000 // 10秒
#define MAX_CLIENTS_SHED_PER_PERIOD (OVERLOAD_PROTECT_PERIOD_MS / 10)  // 每10毫秒限制只剔除一个客户端

extern int g_fTestMode;
extern struct redisServer *g_pserver;

struct redisObject;
class robj_roptr
{
    const redisObject *m_ptr;

public:
    robj_roptr()
        : m_ptr(nullptr)
        {}
    robj_roptr(const redisObject *ptr)
        : m_ptr(ptr)
        {}
    robj_roptr(const robj_roptr&) = default;
    robj_roptr(robj_roptr&&) = default;

    robj_roptr &operator=(const robj_roptr&) = default;
    robj_roptr &operator=(const redisObject *ptr)
    {
        m_ptr = ptr;
        return *this;
    }

    bool operator==(const robj_roptr &other) const
    {
        return m_ptr == other.m_ptr;
    }

    bool operator!=(const robj_roptr &other) const
    {
        return m_ptr != other.m_ptr;
    }

    const redisObject* operator->() const
    {
        return m_ptr;
    }

    const redisObject& operator*() const
    {
        return *m_ptr;
    }

    bool operator!() const
    {
        return !m_ptr;
    }

    operator bool() const{
        return !!m_ptr;
    }

    redisObject *unsafe_robjcast()
    {
        return (redisObject*)m_ptr;
    }
};

class unique_sds_ptr
{
    sds m_str;

public:
    unique_sds_ptr()
        : m_str(nullptr)
        {}
    explicit unique_sds_ptr(sds str)
        : m_str(str)
        {}
    
    ~unique_sds_ptr()
    {
        if (m_str)
            sdsfree(m_str);
    }

    unique_sds_ptr(unique_sds_ptr &&other)
    {
        m_str = other.m_str;
        other.m_str = nullptr;
    }

    bool operator==(const unique_sds_ptr &other) const
    {
        return m_str == other.m_str;
    }

    bool operator!=(const unique_sds_ptr &other) const
    {
        return m_str != other.m_str;
    }

    sds operator->() const
    {
        return m_str;
    }

    bool operator!() const
    {
        return !m_str;
    }

    bool operator<(const unique_sds_ptr &other) const { return m_str < other.m_str; }

    sds get() const { return m_str; }
};

void decrRefCount(robj_roptr o);
void incrRefCount(robj_roptr o);
class robj_sharedptr
{
    redisObject *m_ptr;

public:
    robj_sharedptr()
    : m_ptr(nullptr)
    {}
    explicit robj_sharedptr(redisObject *ptr)
    : m_ptr(ptr)
    {
        if(m_ptr)
            incrRefCount(ptr);
    }
    ~robj_sharedptr()
    {
        if (m_ptr)
            decrRefCount(m_ptr);
    }
    robj_sharedptr(const robj_sharedptr& other)
    : m_ptr(other.m_ptr)
    {        
        if(m_ptr)
            incrRefCount(m_ptr);
    }

    robj_sharedptr(robj_sharedptr&& other)
    {
        m_ptr = other.m_ptr;
        other.m_ptr = nullptr;
    }

    robj_sharedptr &operator=(const robj_sharedptr& other)
    {
        robj_sharedptr tmp(other);
        using std::swap;
        swap(m_ptr, tmp.m_ptr);
        return *this;
    }
    robj_sharedptr &operator=(redisObject *ptr)
    {
        robj_sharedptr tmp(ptr);
        using std::swap;
        swap(m_ptr, tmp.m_ptr);
        return *this;
    }
    
    redisObject* operator->() const
    {
        return m_ptr;
    }

    bool operator!() const
    {
        return !m_ptr;
    }

    explicit operator bool() const{
        return !!m_ptr;
    }

    operator redisObject *()
    {
        return (redisObject*)m_ptr;
    }

    redisObject *get() { return m_ptr; }
    const redisObject *get() const { return m_ptr; }
};

inline bool operator==(const robj_sharedptr &lhs, const robj_sharedptr &rhs)
{
    return lhs.get() == rhs.get();
}

inline bool operator!=(const robj_sharedptr &lhs, const robj_sharedptr &rhs)
{
    return !(lhs == rhs);
}

inline bool operator==(const robj_sharedptr &lhs, const void *p)
{
    return lhs.get() == p;
}

inline bool operator==(const void *p, const robj_sharedptr &rhs)
{
    return rhs == p;
}

inline bool operator!=(const robj_sharedptr &lhs, const void *p)
{
    return !(lhs == p);
}

inline bool operator!=(const void *p, const robj_sharedptr &rhs)
{
    return !(rhs == p);
}

/* Error codes */
#define C_OK                    0
#define C_ERR                   -1

/* Static server configuration */
#define CONFIG_DEFAULT_HZ        10             /* 每秒定时中断调用次数 */
#define CONFIG_MIN_HZ            1
#define CONFIG_MAX_HZ            500
#define MAX_CLIENTS_PER_CLOCK_TICK 200          /* HZ 的调整依据 */
#define CONFIG_MAX_LINE    1024
#define CRON_DBS_PER_CALL 16
#define NET_MAX_WRITES_PER_EVENT (1024*64)
#define PROTO_SHARED_SELECT_CMDS 10
#define OBJ_SHARED_INTEGERS 10000
#define OBJ_SHARED_BULKHDR_LEN 32
#define LOG_MAX_LEN    1024 /* syslog 消息的默认最大长度 */
#define AOF_REWRITE_ITEMS_PER_CMD 64
#define AOF_READ_DIFF_INTERVAL_BYTES (1024*10)
#define CONFIG_AUTHPASS_MAX_LEN 512
#define CONFIG_RUN_ID_SIZE 40
#define RDB_EOF_MARK_SIZE 40
#define CONFIG_REPL_BACKLOG_MIN_SIZE (1024*16)          /* 16k */
#define CONFIG_BGSAVE_RETRY_DELAY 5 /* 失败后等待几秒再试 */
#define CONFIG_DEFAULT_PID_FILE "/var/run/keydb.pid"
#define CONFIG_DEFAULT_CLUSTER_CONFIG_FILE "nodes.conf"
#define CONFIG_DEFAULT_UNIX_SOCKET_PERM 0
#define CONFIG_DEFAULT_LOGFILE ""
#define NET_HOST_STR_LEN 256 /* 有效主机名的最大长度 */
#define NET_IP_STR_LEN 46 /* INET6_ADDRSTRLEN 是 46, 需确保足够 */
#define NET_ADDR_STR_LEN (NET_IP_STR_LEN+32) /* ip:port 字符串需足够长 */
#define NET_HOST_PORT_STR_LEN (NET_HOST_STR_LEN+32) /* hostname:port 的字符串长度 */
#define CONFIG_BINDADDR_MAX 16
#define CONFIG_MIN_RESERVED_FDS 32
#define CONFIG_DEFAULT_THREADS 1
#define CONFIG_DEFAULT_THREAD_AFFINITY 0
#define CONFIG_DEFAULT_PROC_TITLE_TEMPLATE "{title} {listen-addr} {server-mode}"

#define CONFIG_DEFAULT_ACTIVE_REPLICA 0
#define CONFIG_DEFAULT_ENABLE_MULTIMASTER 0

#define ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP 64 /* 每个周期的查询次数 */
#define ACTIVE_EXPIRE_CYCLE_SUBKEY_LOOKUPS_PER_LOOP 16384 /* 子键每个周期的查询次数 */
#define ACTIVE_EXPIRE_CYCLE_FAST_DURATION 1000 /* 微秒 */
#define ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC 25 /* CPU用于过期key收集的最大百分比 */
#define ACTIVE_EXPIRE_CYCLE_SLOW 0
#define ACTIVE_EXPIRE_CYCLE_FAST 1

/* 子进程会以此状态码退出，表示无错误终止：用于终止保存子进程（RDB或AOF），
 * 而不会在父进程中触发通常因写入错误而启用的写保护机制。
 * 通常被 SIGUSR1 终止的子进程将以该特殊代码退出。 */
#define SERVER_CHILD_NOERROR_RETVAL    255

/* 读取写时复制（COW）信息有时开销较大，可能会拖慢持续报告的子进程。
 * 需要根据获取COW信息的耗时进行节流。*/
#define CHILD_COW_DUTY_CYCLE           100

/* 即时统计指标跟踪 */
#define STATS_METRIC_SAMPLES 16     /* 每个指标采样点数量 */
#define STATS_METRIC_COMMAND 0      /* 执行的命令数量 */
#define STATS_METRIC_NET_INPUT 1    /* 网络读取的字节数 */
#define STATS_METRIC_NET_OUTPUT 2   /* 网络写入的字节数 */
#define STATS_METRIC_COUNT 3

/* 协议与I/O相关定义 */
#define PROTO_IOBUF_LEN         (1024*16)  /* 通用I/O缓冲区大小 */
#define PROTO_REPLY_CHUNK_BYTES (16*1024) /* 16k输出缓冲区 */
#define PROTO_ASYNC_REPLY_CHUNK_BYTES (1024)
#define PROTO_INLINE_MAX_SIZE   (1024*64) /* 内联读取的最大大小 */
#define PROTO_MBULK_BIG_ARG     (1024*32)
#define LONG_STR_SIZE      21          /* long转字符串+结尾符所需字节数 */
#define REDIS_AUTOSYNC_BYTES (1024*1024*32) /* 每写32MB做一次fdatasync */

#define LIMIT_PENDING_QUERYBUF (4*1024*1024) /* 4MB */

/* 配置事件循环时，总文件描述符数为g_pserver->maxclients + RESERVED_FDS +
 * 再加一些冗余。因为RESERVED_FDS默认32，实际再加96，
 * 保证不会超配超过128个fd。*/
#define CONFIG_FDSET_INCR (CONFIG_MIN_RESERVED_FDS+96)

/* OOM Score Adjustment classes. */
#define CONFIG_OOM_MASTER 0
#define CONFIG_OOM_REPLICA 1
#define CONFIG_OOM_BGCHILD 2
#define CONFIG_OOM_COUNT 3

extern int configOOMScoreAdjValuesDefaults[CONFIG_OOM_COUNT];

/* 哈希表相关参数 */
#define HASHTABLE_MIN_FILL        10      /* 哈希表最小填充10% */
#define HASHTABLE_MAX_LOAD_FACTOR 1.618   /* 哈希表最大负载因子 */

/* 命令标志。关于各个flag的含义请参见 server.cpp 中的命令表。*/
#define CMD_WRITE (1ULL<<0)            /* "写" 标志 */
#define CMD_READONLY (1ULL<<1)         /* "只读" 标志 */
#define CMD_DENYOOM (1ULL<<2)          /* "使用内存" 标志 */
#define CMD_MODULE (1ULL<<3)           /* 模块导出的命令 */
#define CMD_ADMIN (1ULL<<4)            /* "管理员" 标志 */
#define CMD_PUBSUB (1ULL<<5)           /* "发布订阅" 标志 */
#define CMD_NOSCRIPT (1ULL<<6)         /* "非脚本" 标志 */
#define CMD_RANDOM (1ULL<<7)           /* "随机" 标志 */
#define CMD_SORT_FOR_SCRIPT (1ULL<<8)  /* "排序脚本" 标志 */
#define CMD_LOADING (1ULL<<9)          /* "可loading" 标志 */
#define CMD_STALE (1ULL<<10)           /* "可stale" 标志 */
#define CMD_SKIP_MONITOR (1ULL<<11)    /* "不监控" 标志 */
#define CMD_SKIP_SLOWLOG (1ULL<<12)    /* "不慢日志" 标志 */
#define CMD_ASKING (1ULL<<13)          /* "集群ASKING" 标志 */
#define CMD_FAST (1ULL<<14)            /* "快速" 标志 */
#define CMD_NO_AUTH (1ULL<<15)         /* "无需认证" 标志 */
#define CMD_MAY_REPLICATE (1ULL<<16)   /* "可能复制" 标志 */

/* 模块系统用的命令标志 */
#define CMD_MODULE_GETKEYS (1ULL<<17)  /* 使用模块getkeys接口 */
#define CMD_MODULE_NO_CLUSTER (1ULL<<18) /* 禁止在集群中使用 */

/* 表示ACL类别的命令标志位 */
#define CMD_CATEGORY_KEYSPACE (1ULL<<18)
#define CMD_CATEGORY_READ (1ULL<<19)
#define CMD_CATEGORY_WRITE (1ULL<<20)
#define CMD_CATEGORY_SET (1ULL<<21)
#define CMD_CATEGORY_SORTEDSET (1ULL<<22)
#define CMD_CATEGORY_LIST (1ULL<<23)
#define CMD_CATEGORY_HASH (1ULL<<24)
#define CMD_CATEGORY_STRING (1ULL<<25)
#define CMD_CATEGORY_BITMAP (1ULL<<26)
#define CMD_CATEGORY_HYPERLOGLOG (1ULL<<27)
#define CMD_CATEGORY_GEO (1ULL<<28)
#define CMD_CATEGORY_STREAM (1ULL<<29)
#define CMD_CATEGORY_PUBSUB (1ULL<<30)
#define CMD_CATEGORY_ADMIN (1ULL<<31)
#define CMD_CATEGORY_FAST (1ULL<<32)
#define CMD_CATEGORY_SLOW (1ULL<<33)
#define CMD_CATEGORY_BLOCKING (1ULL<<34)
#define CMD_CATEGORY_DANGEROUS (1ULL<<35)
#define CMD_CATEGORY_CONNECTION (1ULL<<36)
#define CMD_CATEGORY_TRANSACTION (1ULL<<37)
#define CMD_CATEGORY_SCRIPTING (1ULL<<38)
#define CMD_CATEGORY_REPLICATION (1ULL<<39)
#define CMD_SKIP_PROPOGATE (1ULL<<40)  /* "不传播" 标志 */
#define CMD_ASYNC_OK (1ULL<<41) /* 此命令无需加锁也安全 */

/* AOF states */
#define AOF_OFF 0             /* AOF is off */
#define AOF_ON 1              /* AOF is on */
#define AOF_WAIT_REWRITE 2    /* AOF waits rewrite to start appending */

/* Client flags */
#define CLIENT_SLAVE (1<<0)   /* This client is a replica */
#define CLIENT_MASTER (1<<1)  /* This client is a master */
#define CLIENT_MONITOR (1<<2) /* This client is a replica monitor, see MONITOR */
#define CLIENT_MULTI (1<<3)   /* This client is in a MULTI context */
#define CLIENT_BLOCKED (1<<4) /* The client is waiting in a blocking operation */
#define CLIENT_DIRTY_CAS (1<<5) /* Watched keys modified. EXEC will fail. */
#define CLIENT_CLOSE_AFTER_REPLY (1<<6) /* Close after writing entire reply. */
#define CLIENT_UNBLOCKED (1<<7) /* This client was unblocked and is stored in
                                  g_pserver->unblocked_clients */
#define CLIENT_LUA (1<<8) /* 这是一个由 Lua 使用的非连接式客户端 */
#define CLIENT_ASKING (1<<9)     /* 客户端发出了 ASKING 命令 */
#define CLIENT_CLOSE_ASAP (1<<10)/* 尽快关闭该客户端 */
#define CLIENT_UNIX_SOCKET (1<<11) /* 客户端通过 Unix 域套接字连接 */
#define CLIENT_DIRTY_EXEC (1<<12)  /* 若在排队期间出现错误，EXEC 会失败 */
#define CLIENT_MASTER_FORCE_REPLY (1<<13)  /* 即使是主库也依旧排队回复 */
#define CLIENT_FORCE_AOF (1<<14)   /* 强制将当前命令写入 AOF */
#define CLIENT_FORCE_REPL (1<<15)  /* 强制复制当前命令给副本 */
#define CLIENT_PRE_PSYNC (1<<16)   /* 该实例不理解 PSYNC 协议 */
#define CLIENT_READONLY (1<<17)    /* 集群客户端处于只读状态 */
#define CLIENT_PUBSUB (1<<18)      /* 客户端处于 Pub/Sub 模式 */
#define CLIENT_PREVENT_AOF_PROP (1<<19)  /* 不传播到 AOF。*/
#define CLIENT_PREVENT_REPL_PROP (1<<20)  /* 不传播到从节点。*/
#define CLIENT_PREVENT_PROP (CLIENT_PREVENT_AOF_PROP|CLIENT_PREVENT_REPL_PROP)
#define CLIENT_IGNORE_SOFT_SHUTDOWN (CLIENT_MASTER | CLIENT_SLAVE | CLIENT_BLOCKED | CLIENT_MONITOR)
#define CLIENT_PENDING_WRITE (1<<21) /* 客户端有输出要发送，但写入处理程序尚未安装。*/
#define CLIENT_REPLY_OFF (1<<22)   /* 不向客户端发送回复。*/
#define CLIENT_REPLY_SKIP_NEXT (1<<23)  /* 为下一个命令设置 CLIENT_REPLY_SKIP */
#define CLIENT_REPLY_SKIP (1<<24)  /* 不发送此回复。*/
#define CLIENT_LUA_DEBUG (1<<25)  /* 在调试模式下运行 EVAL。*/
#define CLIENT_LUA_DEBUG_SYNC (1<<26)  /* 不使用 fork() 进行 EVAL 调试 */
#define CLIENT_MODULE (1<<27) /* 某些模块使用的未连接客户端。*/
#define CLIENT_PROTECTED (1<<28) /* 客户端暂时不应被释放。*/
#define CLIENT_PENDING_COMMAND (1<<29) /* 指示客户端有一个已完全解析并准备好执行的命令。*/
#define CLIENT_EXECUTING_COMMAND (1<<30) /* 用于处理 processCommandWhileBlocked 中的重入情况，以确保我们不会处理已经在执行的客户端 */
#define CLIENT_TRACKING (1ULL<<31) /* 客户端启用了键跟踪，以便执行客户端缓存。*/
#define CLIENT_TRACKING_BROKEN_REDIR (1ULL<<32) /* 目标客户端无效。*/
#define CLIENT_TRACKING_BCAST (1ULL<<33) /* BCAST 模式下的跟踪。*/
#define CLIENT_TRACKING_OPTIN (1ULL<<34)  /* opt-in 模式下的跟踪。*/
#define CLIENT_TRACKING_OPTOUT (1ULL<<35) /* opt-out 模式下的跟踪。*/
#define CLIENT_TRACKING_CACHING (1ULL<<36) /* 根据 opt-in/opt-out 模式，给定了 CACHING yes/no。*/
#define CLIENT_TRACKING_NOLOOP (1ULL<<37) /* 不要发送关于我自己执行的写入的失效消息。*/
#define CLIENT_IN_TO_TABLE (1ULL<<38) /* 此客户端在超时表中。*/
#define CLIENT_PROTOCOL_ERROR (1ULL<<39) /* 与其通信时发生协议错误。*/
#define CLIENT_CLOSE_AFTER_COMMAND (1ULL<<40) /* 执行命令并写入完整回复后关闭。*/
#define CLIENT_DENY_BLOCKING (1ULL<<41) /* 指示不应阻塞客户端。目前在 MULTI、Lua、RM_Call 和 AOF 客户端内部开启 */
#define CLIENT_REPL_RDBONLY (1ULL<<42) /* 此客户端是一个副本，仅需要 RDB 而不需要复制缓冲区。*/
#define CLIENT_FORCE_REPLY (1ULL<<44) /* 是否应强制 addReply 写入文本？ */
#define CLIENT_AUDIT_LOGGING (1ULL<<45) /* 客户端命令需要审计日志 */

/* 客户端阻塞类型（客户端结构中的 btype 字段）
 * 如果设置了 CLIENT_BLOCKED 标志。*/
#define BLOCKED_NONE 0    /* 未阻塞，未设置 CLIENT_BLOCKED 标志。*/
#define BLOCKED_LIST 1    /* BLPOP 等。*/
#define BLOCKED_WAIT 2    /* 等待同步复制。*/
#define BLOCKED_MODULE 3  /* 被可加载模块阻塞。*/
#define BLOCKED_STREAM 4  /* XREAD。*/
#define BLOCKED_ZSET 5    /* BZPOP 等。*/
#define BLOCKED_PAUSE 6   /* 被 CLIENT PAUSE 阻塞 */
#define BLOCKED_ASYNC 7
#define BLOCKED_NUM 8     /* 阻塞状态的数量。*/

/* 客户端请求类型 */
#define PROTO_REQ_INLINE 1
#define PROTO_REQ_MULTIBULK 2

/* 用于客户端限制的客户端类，目前仅用于 max-client-output-buffer 限制实现。*/
#define CLIENT_TYPE_NORMAL 0 /* 普通请求-回复客户端 + MONITOR */
#define CLIENT_TYPE_SLAVE 1  /* 从节点。*/
#define CLIENT_TYPE_PUBSUB 2 /* 订阅了 PubSub 频道的客户端。*/
#define CLIENT_TYPE_MASTER 3 /* 主节点。*/
#define CLIENT_TYPE_COUNT 4  /* 客户端类型总数。*/
#define CLIENT_TYPE_OBUF_COUNT 3 /* 要公开给输出缓冲区配置的客户端数量。仅前三种：普通、副本、pubsub。*/

/* 从节点复制状态。在 g_pserver->repl_state 中使用，供从节点记住接下来要做什么。*/
typedef enum {
    REPL_STATE_NONE = 0,            /* 无活动复制 */
    REPL_STATE_CONNECT,             /* 必须连接到主节点 */
    REPL_STATE_CONNECTING,          /* 正在连接到主节点 */
    REPL_STATE_RETRY_NOREPLPING,    /* 主节点不支持 REPLPING，使用 PING 重试 */
    /* --- 握手状态，必须有序 --- */
    REPL_STATE_RECEIVE_PING_REPLY,  /* 等待 PING 回复 */
    REPL_STATE_SEND_HANDSHAKE,      /*向主节点发送握手序列 */
    REPL_STATE_RECEIVE_AUTH_REPLY,  /* 等待 AUTH 回复 */
    REPL_STATE_RECEIVE_PORT_REPLY,  /* 等待 REPLCONF 回复 */
    REPL_STATE_RECEIVE_IP_REPLY,    /* 等待 REPLCONF 回复 */
    REPL_STATE_RECEIVE_CAPA_REPLY,  /* 等待 REPLCONF 回复 */
    REPL_STATE_RECEIVE_UUID,        /* 他们应该用自己的 UUID 进行确认 */
    REPL_STATE_SEND_PSYNC,          /* 发送 PSYNC */
    REPL_STATE_RECEIVE_PSYNC_REPLY, /* 等待 PSYNC 回复 */
    /* --- 握手状态结束 --- */
    REPL_STATE_TRANSFER,        /* 从主节点接收 .rdb 文件 */
    REPL_STATE_CONNECTED,       /* 已连接到主节点 */
} repl_state;

/* 正在进行的协调故障转移的状态 */
typedef enum {
    NO_FAILOVER = 0,        /* 没有正在进行的故障转移 */
    FAILOVER_WAIT_FOR_SYNC, /* 等待目标副本赶上进度 */
    FAILOVER_IN_PROGRESS    /* 等待目标副本接受 PSYNC FAILOVER 请求。*/
} failover_state;

/* 从主节点的角度看从节点的状态。在 client->replstate 中使用。
 * 在 SEND_BULK 和 ONLINE 状态下，副本在其输出队列中接收新的更新。
 * 在 WAIT_BGSAVE 状态下，服务器正在等待启动下一个后台保存，以便向其发送更新。*/
#define SLAVE_STATE_WAIT_BGSAVE_START 6 /* 我们需要生成一个新的 RDB 文件。*/
#define SLAVE_STATE_WAIT_BGSAVE_END 7 /* 等待 RDB 文件创建完成。*/
#define SLAVE_STATE_SEND_BULK 8 /* 将 RDB 文件发送到副本。*/
#define SLAVE_STATE_ONLINE 9 /* RDB 文件已传输，仅发送更新。*/
#define SLAVE_STATE_FASTSYNC_TX 10
#define SLAVE_STATE_FASTSYNC_DONE 11

/* 从节点能力。*/
#define SLAVE_CAPA_NONE 0
#define SLAVE_CAPA_EOF (1<<0)    /* 可以解析 RDB EOF 流格式。*/
#define SLAVE_CAPA_PSYNC2 (1<<1) /* 支持 PSYNC2 协议。*/
#define SLAVE_CAPA_ACTIVE_EXPIRE (1<<2) /* 从节点是否会自行执行过期操作？（不发送删除）*/
#define SLAVE_CAPA_KEYDB_FASTSYNC (1<<3)

/* 同步读取超时 - 副本端 */
#define CONFIG_REPL_SYNCIO_TIMEOUT 5

/* 列表相关内容 */
#define LIST_HEAD 0
#define LIST_TAIL 1
#define ZSET_MIN 0
#define ZSET_MAX 1

/* 排序操作 */
#define SORT_OP_GET 0

/* 日志级别 */
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define LL_RAW (1<<10) /* 不带时间戳记录日志的修饰符 */

/* 错误严重级别 */
#define ERR_CRITICAL 0
#define ERR_ERROR 1
#define ERR_WARNING 2
#define ERR_NOTICE 3

/* 监控选项 */
#define SUPERVISED_NONE 0
#define SUPERVISED_AUTODETECT 1
#define SUPERVISED_SYSTEMD 2
#define SUPERVISED_UPSTART 3

/* 反警告宏... */
#define UNUSED(V) ((void) V)

#define ZSKIPLIST_MAXLEVEL 32 /* 对于 2^64 个元素应该足够了 */
#define ZSKIPLIST_P 0.25      /* 跳表 P = 1/4 */

/*仅追加定义 */
#define AOF_FSYNC_NO 0
#define AOF_FSYNC_ALWAYS 1
#define AOF_FSYNC_EVERYSEC 2

/* 复制无盘加载定义 */
#define REPL_DISKLESS_LOAD_DISABLED 0
#define REPL_DISKLESS_LOAD_WHEN_DB_EMPTY 1
#define REPL_DISKLESS_LOAD_SWAPDB 2

/* 存储内存模型定义 */
#define STORAGE_WRITEBACK 0
#define STORAGE_WRITETHROUGH 1

/* TLS 客户端身份验证 */
#define TLS_CLIENT_AUTH_NO 0
#define TLS_CLIENT_AUTH_YES 1
#define TLS_CLIENT_AUTH_OPTIONAL 2

/* 清理转储负载 */
#define SANITIZE_DUMP_NO 0
#define SANITIZE_DUMP_YES 1
#define SANITIZE_DUMP_CLIENTS 2

/* 集合操作码 */
#define SET_OP_UNION 0
#define SET_OP_DIFF 1
#define SET_OP_INTER 2

/* oom-score-adj 定义 */
#define OOM_SCORE_ADJ_NO 0
#define OOM_SCORE_RELATIVE 1
#define OOM_SCORE_ADJ_ABSOLUTE 2

/* Redis maxmemory 策略。我们不使用递增数字作为此处的定义，而是使用一组标志，以便更快地测试多个策略共有的某些属性。*/
#define MAXMEMORY_FLAG_LRU (1<<0)
#define MAXMEMORY_FLAG_LFU (1<<1)
#define MAXMEMORY_FLAG_ALLKEYS (1<<2)
#define MAXMEMORY_FLAG_NO_SHARED_INTEGERS \
    (MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_LFU)

#define MAXMEMORY_VOLATILE_LRU ((0<<8)|MAXMEMORY_FLAG_LRU)
#define MAXMEMORY_VOLATILE_LFU ((1<<8)|MAXMEMORY_FLAG_LFU)
#define MAXMEMORY_VOLATILE_TTL (2<<8)
#define MAXMEMORY_VOLATILE_RANDOM (3<<8)
#define MAXMEMORY_ALLKEYS_LRU ((4<<8)|MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_ALLKEYS_LFU ((5<<8)|MAXMEMORY_FLAG_LFU|MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_ALLKEYS_RANDOM ((6<<8)|MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_NO_EVICTION (7<<8)

/* Units */
#define UNIT_SECONDS 0
#define UNIT_MILLISECONDS 1

/* SHUTDOWN flags */
#define SHUTDOWN_NOFLAGS 0      /* No flags. */
#define SHUTDOWN_SAVE 1         /* Force SAVE on SHUTDOWN even if no save
                                   points are configured. */
#define SHUTDOWN_NOSAVE 2       /* Don't SAVE on SHUTDOWN. */

/* Command call flags, see call() function */
#define CMD_CALL_NONE 0
#define CMD_CALL_SLOWLOG (1<<0)
#define CMD_CALL_STATS (1<<1)
#define CMD_CALL_PROPAGATE_AOF (1<<2)
#define CMD_CALL_PROPAGATE_REPL (1<<3)
#define CMD_CALL_PROPAGATE (CMD_CALL_PROPAGATE_AOF|CMD_CALL_PROPAGATE_REPL)
#define CMD_CALL_FULL (CMD_CALL_SLOWLOG | CMD_CALL_STATS | CMD_CALL_PROPAGATE | CMD_CALL_NOWRAP)
#define CMD_CALL_NOWRAP (1<<4)  /* Don't wrap also propagate array into
                                   MULTI/EXEC: the caller will handle it.  */
#define CMD_CALL_ASYNC (1<<5)

/* Command propagation flags, see propagate() function */
#define PROPAGATE_NONE 0
#define PROPAGATE_AOF 1
#define PROPAGATE_REPL 2

/* Client pause types, larger types are more restrictive
 * pause types than smaller pause types. */
typedef enum {
    CLIENT_PAUSE_OFF = 0, /* Pause no commands */
    CLIENT_PAUSE_WRITE,   /* Pause write commands */
    CLIENT_PAUSE_ALL      /* Pause all commands */
} pause_type;

/* RDB active child save type. */
#define RDB_CHILD_TYPE_NONE 0
#define RDB_CHILD_TYPE_DISK 1     /* RDB is written to disk. */
#define RDB_CHILD_TYPE_SOCKET 2   /* RDB is written to replica socket. */

/* Keyspace changes notification classes. Every class is associated with a
 * character for configuration purposes. */
#define NOTIFY_KEYSPACE (1<<0)    /* K */
#define NOTIFY_KEYEVENT (1<<1)    /* E */
#define NOTIFY_GENERIC (1<<2)     /* g */
#define NOTIFY_STRING (1<<3)      /* $ */
#define NOTIFY_LIST (1<<4)        /* l */
#define NOTIFY_SET (1<<5)         /* s */
#define NOTIFY_HASH (1<<6)        /* h */
#define NOTIFY_ZSET (1<<7)        /* z */
#define NOTIFY_EXPIRED (1<<8)     /* x */
#define NOTIFY_EVICTED (1<<9)     /* e */
#define NOTIFY_STREAM (1<<10)     /* t */
#define NOTIFY_KEY_MISS (1<<11)   /* m (Note: This one is excluded from NOTIFY_ALL on purpose) */
#define NOTIFY_LOADED (1<<12)     /* module only key space notification, indicate a key loaded from rdb */
#define NOTIFY_MODULE (1<<13)     /* d, module key space notification */
#define NOTIFY_ALL (NOTIFY_GENERIC | NOTIFY_STRING | NOTIFY_LIST | NOTIFY_SET | NOTIFY_HASH | NOTIFY_ZSET | NOTIFY_EXPIRED | NOTIFY_EVICTED | NOTIFY_STREAM | NOTIFY_MODULE) /* A flag */

/* Get the first bind addr or NULL */
#define NET_FIRST_BIND_ADDR (g_pserver->bindaddr_count ? g_pserver->bindaddr[0] : NULL)

/* Using the following macro you can run code inside serverCron() with the
 * specified period, specified in milliseconds.
 * The actual resolution depends on g_pserver->hz. */
#define run_with_period(_ms_) if ((_ms_ <= 1000/g_pserver->hz) || !(g_pserver->cronloops%((_ms_)/(1000/g_pserver->hz))))

/*-----------------------------------------------------------------------------
 * 数据类型
 *----------------------------------------------------------------------------*/

/* Redis对象：可用于存储字符串、列表、集合等的数据类型 */

/* 具体的 Redis 对象类型 */
#define OBJ_STRING 0     /* 字符串对象 */
#define OBJ_LIST 1       /* 列表对象 */
#define OBJ_SET 2        /* 集合对象 */
#define OBJ_ZSET 3       /* 有序集合对象 */
#define OBJ_HASH 4       /* 哈希对象 */

/* “模块”对象类型是特殊类型，表示对象由 Redis 模块直接管理。
 * 此时 value 指向 moduleValue 结构体（仅被模块自己管理），
 * 并包括 RedisModuleType 结构体用于列出序列化、反序列化、
 * AOF重写和释放对象的函数指针。
 *
 * 在 RDB 文件中，模块类型用 OBJ_MODULE 加 64 位模块类型ID
 * 编码。高54位为签名用来分辨模块，低10位为编码版本号。 */
#define OBJ_MODULE 5     /* 模块对象 */
#define OBJ_STREAM 6     /* Stream流对象 */
#define OBJ_CRON 7       /* CRON 任务对象 */
#define OBJ_NESTEDHASH 8 /* 嵌套哈希对象 */

/* 从模块类型ID中获取编码版本和签名 */
#define REDISMODULE_TYPE_ENCVER_BITS 10
#define REDISMODULE_TYPE_ENCVER_MASK ((1<<REDISMODULE_TYPE_ENCVER_BITS)-1)
#define REDISMODULE_TYPE_ENCVER(id) (id & REDISMODULE_TYPE_ENCVER_MASK)
#define REDISMODULE_TYPE_SIGN(id) ((id & ~((uint64_t)REDISMODULE_TYPE_ENCVER_MASK)) >>REDISMODULE_TYPE_ENCVER_BITS)

/* moduleTypeAuxSaveFunc 的位标志 */
#define REDISMODULE_AUX_BEFORE_RDB (1<<0)
#define REDISMODULE_AUX_AFTER_RDB (1<<1)

/* Time线程在放弃fork锁之前的最大循环数 */
#define MAX_CYCLES_TO_HOLD_FORK_LOCK 10

struct RedisModule;
struct RedisModuleIO;
struct RedisModuleDigest;
struct RedisModuleCtx;
struct redisObject;
struct RedisModuleDefragCtx;

/* 每个模块类型实现都应该导出一组方法，以便在 RDB 文件中序列化和反序列化值，重写 AOF 日志，为 "DEBUG DIGEST" 创建摘要，并在删除键时释放值。*/
typedef void *(*moduleTypeLoadFunc)(struct RedisModuleIO *io, int encver);
typedef void (*moduleTypeSaveFunc)(struct RedisModuleIO *io, void *value);
typedef int (*moduleTypeAuxLoadFunc)(struct RedisModuleIO *rdb, int encver, int when);
typedef void (*moduleTypeAuxSaveFunc)(struct RedisModuleIO *rdb, int when);
typedef void (*moduleTypeRewriteFunc)(struct RedisModuleIO *io, struct redisObject *key, void *value);
typedef void (*moduleTypeDigestFunc)(struct RedisModuleDigest *digest, void *value);
typedef size_t (*moduleTypeMemUsageFunc)(const void *value);
typedef void (*moduleTypeFreeFunc)(void *value);
typedef size_t (*moduleTypeFreeEffortFunc)(struct redisObject *key, const void *value);
typedef void (*moduleTypeUnlinkFunc)(struct redisObject *key, void *value);
typedef void *(*moduleTypeCopyFunc)(struct redisObject *fromkey, struct redisObject *tokey, const void *value);
typedef int (*moduleTypeDefragFunc)(struct RedisModuleDefragCtx *ctx, struct redisObject *key, void **value);

/* 每当通过模块 API 进行身份验证的用户与不同的用户关联或断开连接时，moduleNotifyUserChanged() 都会调用此回调类型。需要公开此回调类型，因为不能将函数指针强制转换为 (void *)。*/
typedef void (*RedisModuleUserChangedFunc) (uint64_t client_id, void *privdata);


/* 模块类型，在给定类型的每个值中引用，定义了方法并链接到导出该类型的模块。*/
typedef struct RedisModuleType {
    uint64_t id; /* 类型 ID 的高 54 位 + 编码版本的高 10 位。*/
    struct RedisModule *module;
    moduleTypeLoadFunc rdb_load;
    moduleTypeSaveFunc rdb_save;
    moduleTypeRewriteFunc aof_rewrite;
    moduleTypeMemUsageFunc mem_usage;
    moduleTypeDigestFunc digest;
    moduleTypeFreeFunc free;
    moduleTypeFreeEffortFunc free_effort;
    moduleTypeUnlinkFunc unlink;
    moduleTypeCopyFunc copy;
    moduleTypeDefragFunc defrag;
    moduleTypeAuxLoadFunc aux_load;
    moduleTypeAuxSaveFunc aux_save;
    int aux_save_triggers;
    char name[10]; /* 9 字节名称 + 空终止符。字符集：A-Z a-z 0-9 _- */
} moduleType;

/* 在 OBJ_MODULE 类型的 Redis 对象 'robj' 结构中，值指针设置为以下结构，引用 moduleType 结构以便处理该值，同时提供一个指向该值的原始指针，该指针由操作模块类型的模块命令创建。
 *
 * 因此，例如，为了释放这样一个值，可以使用以下代码：
 *
 *  if (robj->type == OBJ_MODULE) {
 *      moduleValue *mt = robj->ptr;
 *      mt->type->free(mt->value);
 *      zfree(mt); // 我们也需要释放这个中间结构。
 *  }
 */
typedef struct moduleValue {
    moduleType *type;
    void *value;
} moduleValue;

/* 这是 Redis 中 rdb.c 内部使用的 'rio' 流的包装器，这样用户就不必获取写入字节的总数，也不必关心错误情况。*/
typedef struct RedisModuleIO {
    size_t bytes;       /* 到目前为止读取/写入的字节数。*/
    rio *prio;           /* Rio 流。*/
    moduleType *type;   /* 执行操作的模块类型。*/
    int error;          /* 如果发生错误情况，则为 True。*/
    int ver;            /* 模块序列化版本：1（旧版），
                         * 2（带有操作码注释的当前版本）。*/
    struct RedisModuleCtx *ctx; /* 可选上下文，请参阅 RM_GetContextFromIO() */
    struct redisObject *key;    /* 处理的可选键名 */
} RedisModuleIO;

/* 初始化 IO 上下文的宏。请注意，'ver' 字段在 rdb.c 内部根据要加载的值的版本进行填充。*/
#define moduleInitIOContext(iovar,mtype,rioptr,keyptr) do { \
    iovar.prio = rioptr; \
    iovar.type = mtype; \
    iovar.bytes = 0; \
    iovar.error = 0; \
    iovar.ver = 0; \
    iovar.key = keyptr; \
    iovar.ctx = NULL; \
} while(0)

/* 这是一个用于向 Redis 模块导出 DEBUG DIGEST 功能的结构。我们希望捕获数据结构的有序和无序元素，以便可以创建一种正确反映值的摘要。有关更多背景信息，请参阅 DEBUG DIGEST 命令实现。*/
typedef struct RedisModuleDigest {
    unsigned char o[20];    /* 有序元素。*/
    unsigned char x[20];    /* 异或元素。*/
} RedisModuleDigest;

/* 只需从一个由全零字节组成的摘要开始。*/
#define moduleInitDigestContext(mdvar) do { \
    memset(mdvar.o,0,sizeof(mdvar.o)); \
    memset(mdvar.x,0,sizeof(mdvar.x)); \
} while(0)

/* 对象编码。某些类型的对象（如字符串和哈希）可以在内部以多种方式表示。对象的 'encoding' 字段设置为此对象的这些字段之一。*/
#define OBJ_ENCODING_RAW 0     /* 原始表示 */
#define OBJ_ENCODING_INT 1     /* 编码为整数 */
#define OBJ_ENCODING_HT 2      /* 编码为哈希表 */
#define OBJ_ENCODING_ZIPMAP 3  /* 编码为 zipmap */
#define OBJ_ENCODING_LINKEDLIST 4 /* 不再使用：旧的列表编码。*/
#define OBJ_ENCODING_ZIPLIST 5 /* 编码为 ziplist */
#define OBJ_ENCODING_INTSET 6  /* 编码为 intset */
#define OBJ_ENCODING_SKIPLIST 7  /* 编码为 skiplist */
#define OBJ_ENCODING_EMBSTR 8  /* 嵌入式 sds 字符串编码 */
#define OBJ_ENCODING_QUICKLIST 9 /* 编码为 ziplist 的链表 */
#define OBJ_ENCODING_STREAM 10 /* 编码为 listpack 的基数树 */

#define LRU_BITS 24
#define LRU_CLOCK_MAX ((1<<LRU_BITS)-1) /* obj->lru 的最大值 */
#define LRU_CLOCK_RESOLUTION 1000 /* LRU 时钟分辨率（毫秒）*/

#define OBJ_SHARED_REFCOUNT (0x7FFFFFFF)
#define OBJ_STATIC_REFCOUNT (OBJ_SHARED_REFCOUNT-1)
#define OBJ_FIRST_SPECIAL_REFCOUNT OBJ_STATIC_REFCOUNT
#define OBJ_MVCC_INVALID (0xFFFFFFFFFFFFFFFFULL)

#define MVCC_MS_SHIFT 20

// 需要时，此结构将在 ROBJ 之前分配
struct redisObjectExtended {
    uint64_t mvcc_tstamp;
};

typedef struct redisObject {
    friend redisObject *createEmbeddedStringObject(const char *ptr, size_t len);
    friend redisObject *createObject(int type, void *ptr);
protected:
    redisObject() {}

public:
    unsigned type:4;
    unsigned encoding:4;
    unsigned lru:LRU_BITS; /* LRU 时间（相对于全局 lru_clock）或 LFU 数据（最低有效 8 位频率和最高有效 16 位访问时间）。*/
private:
    mutable std::atomic<unsigned> refcount {0};
public:
    expireEntry expire;
    void *m_ptr;

    inline bool FExpires() const { return refcount.load(std::memory_order_relaxed) >> 31; }
    void SetFExpires(bool fExpires);

    void setrefcount(unsigned ref);
    unsigned getrefcount(std::memory_order order = std::memory_order_relaxed) const { return (refcount.load(order) & ~(1U << 31)); }
    void addref() const { refcount.fetch_add(1, std::memory_order_relaxed); }
    unsigned release() const { return refcount.fetch_sub(1, std::memory_order_seq_cst) & ~(1U << 31); }
} robj;
static_assert(sizeof(redisObject) <= 24, "对象大小至关重要，请勿增加");

class redisObjectStack : public redisObjectExtended, public redisObject
{
public:
    redisObjectStack();
};

uint64_t mvccFromObj(robj_roptr o);
void setMvccTstamp(redisObject *o, uint64_t mvcc);
void *allocPtrFromObj(robj_roptr o);
robj *objFromAllocPtr(void *pv);

__attribute__((always_inline)) inline const void *ptrFromObj(robj_roptr &o)
{
    if (o->encoding == OBJ_ENCODING_EMBSTR)
        return ((char*)&(o)->m_ptr) + sizeof(struct sdshdr8);
    return o->m_ptr;
}

__attribute__((always_inline)) inline void *ptrFromObj(const robj *o)
{
    if (o->encoding == OBJ_ENCODING_EMBSTR)
        return ((char*)&((robj*)o)->m_ptr) + sizeof(struct sdshdr8);
    return o->m_ptr;
}

__attribute__((always_inline)) inline const char *szFromObj(robj_roptr o)
{
    return (const char*)ptrFromObj(o);
}

__attribute__((always_inline)) inline char *szFromObj(const robj *o)
{
    return (char*)ptrFromObj(o);
}

/* 对象类型的字符串名称，如上所列
 * 原生类型与 OBJ_STRING、OBJ_LIST、OBJ_* 定义进行检查，
 * 模块类型返回其注册名称。*/
const char *getObjectTypeName(robj_roptr o);

/* 用于初始化在栈上分配的 Redis 对象的宏。
 * 请注意，此宏位于结构定义附近，以确保在结构更改时我们会更新它，
 * 以避免像这样引入的 bug #85 之类的错误。*/
#define initStaticStringObject(_var,_ptr) do { \
    _var.setrefcount(OBJ_STATIC_REFCOUNT); \
    _var.type = OBJ_STRING; \
    _var.encoding = OBJ_ENCODING_RAW; \
    _var.m_ptr = _ptr; \
} while(0)

struct evictionPoolEntry; /* 在 evict.c 中定义 */

/* 此结构用于表示客户端的输出缓冲区，
 * 它实际上是类似这样的块的链表，即：client->reply。*/
typedef struct clientReplyBlock {
    size_t size, used;
#ifndef __cplusplus
    char buf[];
#else
    __attribute__((always_inline)) char *buf()
    {
        return reinterpret_cast<char*>(this+1);
    }
#endif
} clientReplyBlock;

struct dictEntry;
class dict_const_iter
{
    friend struct redisDb;
    friend class redisDbPersistentData;
protected:
    dictEntry *de;
public:
    explicit dict_const_iter(dictEntry *de)
        : de(de)
    {}

    const char *key() const { return de ? (const char*)dictGetKey(de) : nullptr; }
    robj_roptr val() const { return de ? (robj*)dictGetVal(de) : nullptr; }
    const robj* operator->() const { return de ? (robj*)dictGetVal(de) : nullptr; }
    operator robj_roptr() const { return de ? (robj*)dictGetVal(de) : nullptr; }

    bool operator==(std::nullptr_t) const { return de == nullptr; }
    bool operator!=(std::nullptr_t) const { return de != nullptr; }
    bool operator==(const dict_const_iter &other) { return de == other.de; }
};
class dict_iter : public dict_const_iter
{
    dict *m_dict = nullptr;
public:
    dict_iter()
        : dict_const_iter(nullptr)
    {}
    explicit dict_iter(std::nullptr_t)
        : dict_const_iter(nullptr)
    {}
    explicit dict_iter(dict *d, dictEntry *de)
        : dict_const_iter(de), m_dict(d)
    {}
    sds key() { return de ? (sds)dictGetKey(de) : nullptr; }
    robj *val() { return de ? (robj*)dictGetVal(de) : nullptr; }
    robj *operator->() { return de ? (robj*)dictGetVal(de) : nullptr; }
    operator robj*() const { return de ? (robj*)dictGetVal(de) : nullptr; }

    void setval(robj *val) {
        dictSetVal(m_dict, de, val);
    }
};

class redisDbPersistentDataSnapshot;
/**
 * @brief 类redisDbPersistentData用于管理Redis数据库的持久化数据。
 * 提供键值对存储、快照管理、内存优化及线程安全的数据操作功能。
 * 通过protected继承限制进一步派生类的访问。
 */
class redisDbPersistentData
{
    friend void dictDbKeyDestructor(void *privdata, void *key);
    friend class redisDbPersistentDataSnapshot;

public:
    /**
     * @brief 构造函数，初始化持久化数据结构
     */
    redisDbPersistentData();

    /**
     * @brief 析构函数，释放资源
     */
    virtual ~redisDbPersistentData();

    // 禁止拷贝和移动构造
    redisDbPersistentData(const redisDbPersistentData &) = delete;
    redisDbPersistentData(redisDbPersistentData &&) = delete;

    /**
     * @brief 获取哈希表槽位数量
     * @return 返回当前哈希表的槽位数
     */
    size_t slots() const { return dictSlots(m_pdict); }

    /**
     * @brief 获取数据库元素数量
     * @param fCachedOnly 是否仅统计缓存数据
     * @return 返回数据库中键值对的数量
     */
    size_t size(bool fCachedOnly = false) const;

    /**
     * @brief 扩展哈希表容量
     * @param slots 新的槽位数量
     */
    void expand(uint64_t slots) {
        if (m_spstorage)
            m_spstorage->expand(slots);
        else
            dictExpand(m_pdict, slots);
    }

    /**
     * @brief 跟踪键的使用情况（带对象参数）
     * @param o 键对象
     * @param fUpdate 是否为更新操作
     */
    void trackkey(robj_roptr o, bool fUpdate)
    {
        trackkey(szFromObj(o), fUpdate);
    }

    /**
     * @brief 跟踪键的使用情况（带字符串参数）
     * @param key 键名称
     * @param fUpdate 是否为更新操作
     */
    void trackkey(const char *key, bool fUpdate);

    /**
     * @brief 查找指定键的迭代器
     * @param key 键名称
     * @return 返回指向该键的迭代器
     */
    dict_iter find(const char *key)
    {
        dictEntry *de = dictFind(m_pdict, key);
        ensure(key, &de);
        return dict_iter(m_pdict, de);
    }

    /**
     * @brief 通过对象查找键（重载版本）
     * @param key 键对象
     * @return 返回对应的迭代器
     */
    dict_iter find(robj_roptr key)
    {
        return find(szFromObj(key));
    }

    /**
     * @brief 获取随机键的迭代器
     * @return 返回随机键的迭代器
     */
    dict_iter random();

    /**
     * @brief 获取随机过期键的信息
     * @param key 输出参数，返回随机过期键的名称
     * @return 返回过期条目指针或nullptr
     */
    const expireEntry *random_expire(sds *key)
    {
        auto itr = random();
        if (itr->FExpires()) {
            *key = itr.key();
            return &itr->expire;
        }
        return nullptr;
    }

    /**
     * @brief 获取空迭代器（非const版本）
     * @return 返回空迭代器
     */
    dict_iter end()  { return dict_iter(nullptr, nullptr); }

    /**
     * @brief 获取空迭代器（const版本）
     * @return 返回const类型的空迭代器
     */
    dict_const_iter end() const { return dict_const_iter(nullptr); }

    /**
     * @brief 获取统计信息字符串
     * @param buf 输出缓冲区
     * @param bufsize 缓冲区大小
     */
    void getStats(char *buf, size_t bufsize) { dictGetStats(buf, bufsize, m_pdict); }

    /**
     * @brief 插入或更新键值对
     * @param k 键名称
     * @param o 值对象
     * @param fAssumeNew 是否假设为新键
     * @param existing 输出参数，返回已存在的条目
     * @return 插入/更新是否成功
     */
    bool insert(char *k, robj *o, bool fAssumeNew = false, dict_iter *existing = nullptr);

    /**
     * @brief 尝试调整哈希表大小
     */
    void tryResize();

    /**
     * @brief 增量式重新哈希
     * @return 返回处理的条目数
     */
    int incrementallyRehash();

    /**
     * @brief 更新键的值
     * @param itr 迭代器位置
     * @param val 新值对象
     */
    void updateValue(dict_iter itr, robj *val);

    /**
     * @brief 同步删除键
     * @param key 键对象
     * @return 删除是否成功
     */
    bool syncDelete(robj *key);

    /**
     * @brief 异步删除键
     * @param key 键对象
     * @return 删除是否成功
     */
    bool asyncDelete(robj *key);

    /**
     * @brief 获取过期条目数量
     * @return 返回过期条目数
     */
    size_t expireSize() const { return m_numexpires; }

    /**
     * @brief 移除键的过期时间
     * @param key 键对象
     * @param itr 迭代器位置
     * @return 移除是否成功
     */
    int removeExpire(robj *key, dict_iter itr);

    /**
     * @brief 移除子键的过期时间
     * @param key 主键对象
     * @param subkey 子键对象
     * @return 移除是否成功
     */
    int removeSubkeyExpire(robj *key, robj *subkey);

    /**
     * @brief 清空数据库
     * @param callback 清理时的回调函数
     */
    void clear(void(callback)(void*));

    /**
     * @brief 异步清空数据库
     */
    void emptyDbAsync();

    /**
     * @brief 遍历数据库键值对
     * @param fn 回调函数，处理每个键值对
     * @return 遍历是否完成
     */
    bool iterate(std::function<bool(const char*, robj*)> fn);

    /**
     * @brief 设置键的过期时间
     * @param key 键对象
     * @param subkey 子键对象（可选）
     * @param when 过期时间戳
     */
    void setExpire(robj *key, robj *subkey, long long when);

    /**
     * @brief 设置键的过期条目（移动语义）
     * @param key 键名称
     * @param e 过期条目对象
     */
    void setExpire(const char *key, expireEntry &&e);

    /**
     * @brief 初始化数据库结构
     */
    void initialize();

    /**
     * @brief 准备在快照模式下覆盖指定键的数据时，标记墓碑条目
     * @param key 需要处理的键名
     */
    void prepOverwriteForSnapshot(char *key);

    /**
     * @brief 检查是否正在进行哈希表重哈希
     * @return 返回布尔值
     */
    bool FRehashing() const { return dictIsRehashing(m_pdict) || dictIsRehashing(m_pdictTombstone); }

    /**
     * @brief 设置存储提供者
     * @param pstorage 存储缓存指针
     */
    void setStorageProvider(StorageCache *pstorage);

    /**
     * @brief 结束存储提供者
     */
    void endStorageProvider();

    /**
     * @brief 开启变更跟踪
     * @param fBulk 是否批量操作
     * @param sizeHint 预分配大小提示
     */
    void trackChanges(bool fBulk, size_t sizeHint = 0);

    /**
     * @brief 检查是否正在跟踪变更
     * @return 返回跟踪状态
     */
    bool FTrackingChanges() const { return !!m_fTrackingChanges; }

    /**
     * @brief 处理二级存储变更（分阶段提交）
     * @param fSnapshot 是否关联快照
     * @return 返回处理结果
     */
    bool processChanges(bool fSnapshot);

    /**
     * @brief 异步处理变更
     * @param pendingJobs 待处理任务计数器
     */
    void processChangesAsync(std::atomic<int> &pendingJobs);

    /**
     * @brief 提交变更到持久化存储
     * @param psnapshotFree 可选快照释放列表
     */
    void commitChanges(const redisDbPersistentDataSnapshot **psnapshotFree = nullptr);

    /**
     * @brief 获取原始键字典（仅限键操作）
     * @return 返回键字典指针
     */
    dict *dictUnsafeKeyOnly() { return m_pdict; }

    /**
     * @brief 创建数据库快照
     * @param mvccCheckpoint MVCC检查点标识
     * @param fOptional 是否可选快照
     * @return 返回快照指针
     */
    const redisDbPersistentDataSnapshot *createSnapshot(uint64_t mvccCheckpoint, bool fOptional);

    /**
     * @brief 结束指定快照
     * @param psnapshot 快照指针
     */
    void endSnapshot(const redisDbPersistentDataSnapshot *psnapshot);

    /**
     * @brief 异步结束快照
     * @param psnapshot 快照指针
     */
    void endSnapshotAsync(const redisDbPersistentDataSnapshot *psnapshot);

    /**
     * @brief 恢复指定快照
     * @param psnapshot 快照指针
     */
    void restoreSnapshot(const redisDbPersistentDataSnapshot *psnapshot);

    /**
     * @brief 检查是否存在存储提供者
     * @return 返回存在状态
     */
    bool FStorageProvider() { return m_spstorage != nullptr; }

    /**
     * @brief 移除缓存中的键值
     * @param key 键名
     * @param ppde 输出参数，返回被移除的字典条目
     * @return 返回移除是否成功
     */
    bool removeCachedValue(const char *key, dictEntry **ppde = nullptr);

    /**
     * @brief 移除所有缓存值
     */
    void removeAllCachedValues();

    /**
     * @brief 禁用键缓存
     */
    void disableKeyCache();

    /**
     * @brief 检查键缓存是否启用
     * @return 返回启用状态
     */
    bool keycacheIsEnabled();

    /**
     * @brief 异步预取指定键
     * @param c 客户端指针
     * @param command 解析后的命令
     */
    void prefetchKeysAsync(client *c, struct parsed_command &command);

    /**
     * @brief 检查是否存在快照
     * @return 返回快照存在状态
     */
    bool FSnapshot() const { return m_spdbSnapshotHOLDER != nullptr; }

    /**
     * @brief 克隆存储缓存
     * @return 返回克隆的存储缓存指针
     */
    std::unique_ptr<const StorageCache> CloneStorageCache() { return std::unique_ptr<const StorageCache>(m_spstorage->clone()); }

    /**
     * @brief 获取存储缓存共享指针
     * @return 返回存储缓存智能指针
     */
    std::shared_ptr<StorageCache> getStorageCache() { return m_spstorage; }

    /**
     * @brief 批量直接插入存储
     * @param rgKeys 键数组
     * @param rgcbKeys 键长度数组
     * @param rgVals 值数组
     * @param rgcbVals 值长度数组
     * @param celem 元素数量
     */
    void bulkDirectStorageInsert(char **rgKeys, size_t *rgcbKeys, char **rgVals, size_t *rgcbVals, size_t celem);

    /**
     * @brief 线程安全地查找缓存键
     * @param key 键名
     * @return 返回找到的迭代器
     */
    dict_iter find_cached_threadsafe(const char *key) const;

    /**
     * @brief 活动过期周期核心实现
     * @param type 过期类型
     */
    static void activeExpireCycleCore(int type);

protected:
    uint64_t m_mvccCheckpoint = 0;  ///< MVCC检查点标识

private:
    /**
     * @brief 序列化并存储变更
     * @param storage 存储缓存指针
     * @param db 数据库指针
     * @param key 键名
     * @param fUpdate 是否为更新操作
     */
    static void serializeAndStoreChange(StorageCache *storage, redisDbPersistentData *db, const char *key, bool fUpdate);

    /**
     * @brief 确保键存在（无参数重载）
     * @param key 键名
     */
    void ensure(const char *key);

    /**
     * @brief 确保键存在（带输出参数）
     * @param key 键名
     * @param de 输出参数，返回字典条目
     */
    void ensure(const char *key, dictEntry **de);

    /**
     * @brief 存储整个数据库
     */
    void storeDatabase();

    /**
     * @brief 存储单个键值对
     * @param key 键对象
     * @param o 值对象
     * @param fOverwrite 是否覆盖
     */
    void storeKey(sds key, robj *o, bool fOverwrite);

    /**
     * @brief 递归释放快照
     * @param psnapshot 快照指针
     */
    void recursiveFreeSnapshots(redisDbPersistentDataSnapshot *psnapshot);

    // 键空间相关成员
    dict *m_pdict = nullptr;                 ///< 主键空间字典
    dict *m_pdictTombstone = nullptr;         ///< 快照模式下的墓碑字典
    std::atomic<int> m_fTrackingChanges {0};  ///< 变更跟踪标志
    std::atomic<int> m_fAllChanged {0};
    dict *m_dictChanged = nullptr;             ///< 变更记录字典
    size_t m_cnewKeysPending = 0;             ///< 待处理的新键数量
    std::shared_ptr<StorageCache> m_spstorage;///< 存储缓存智能指针

    // 过期管理相关
    size_t m_numexpires = 0;  ///< 当前过期条目数

    // 快照相关指针
    const redisDbPersistentDataSnapshot *m_pdbSnapshot = nullptr;          ///< 主快照指针
    std::unique_ptr<redisDbPersistentDataSnapshot> m_spdbSnapshotHOLDER;   ///< 快照持有者智能指针
    const redisDbPersistentDataSnapshot *m_pdbSnapshotASYNC = nullptr;     ///< 异步快照指针

    const redisDbPersistentDataSnapshot *m_pdbSnapshotStorageFlush = nullptr;
    dict *m_dictChangedStorageFlush = nullptr;

    int m_refCount = 0;  ///< 引用计数
};


/**
 * @brief 类redisDbPersistentDataSnapshot用于管理Redis数据库持久化数据的快照。
 * 提供线程安全的数据迭代、快照生命周期管理及MVCC检查点功能。
 * 继承自redisDbPersistentData，通过protected继承限制进一步派生类的访问。
 */
class redisDbPersistentDataSnapshot : protected redisDbPersistentData
{
    friend class redisDbPersistentData;
private:
    /**
     * @brief 核心线程安全迭代方法，遍历数据库中的键值对。
     * @param fn 用户提供的回调函数，参数为键和对象指针，返回true继续迭代。
     * @param fKeyOnly 若为true，则仅处理键，不处理值对象。
     * @param fCacheOnly 若为true，则仅遍历缓存中的数据。
     * @param fTop 若为true，则在顶层结构进行操作。
     * @return 返回回调函数最终返回值，用于控制迭代是否继续。
     */
    bool iterate_threadsafe_core(std::function<bool(const char*, robj_roptr o)> &fn, bool fKeyOnly, bool fCacheOnly, bool fTop) const;

protected:
    /**
     * @brief 垃圾回收器用于释放指定快照对象的资源。
     * @param psnapshot 需要释放的redisDbPersistentDataSnapshot实例指针。
     */
    static void gcDisposeSnapshot(redisDbPersistentDataSnapshot *psnapshot);
    /**
     * @brief 清理墓碑标记的对象，释放其占用的资源。
     * @param depth 清理深度，控制递归清理的层级。
     * @return 若成功释放至少一个对象返回true，否则返回false。
     */
    bool freeTombstoneObjects(int depth);

public:
    /**
     * @brief 获取当前快照的深度值，用于标识快照的层级或版本。
     * @return 返回快照深度整数值。
     */
    int snapshot_depth() const;
    /**
     * @brief 调试用方法，检查当前实例是否会释放子对象。
     * @return 若存在关联的快照持有者则返回true，表示将释放子对象。
     */
    bool FWillFreeChildDebug() const { return m_spdbSnapshotHOLDER != nullptr; }

    /**
     * @brief 线程安全地遍历数据库中的键值对，封装核心迭代逻辑。
     * @param fn 回调函数，接受键和对象指针，返回true继续迭代。
     * @param fKeyOnly 可选参数，默认false，若为true则仅处理键。
     * @param fCacheOnly 可选参数，默认false，若为true则仅遍历缓存数据。
     * @return 返回迭代结果，由回调函数链式决定是否继续。
     */
    bool iterate_threadsafe(std::function<bool(const char*, robj_roptr o)> fn, bool fKeyOnly = false, bool fCacheOnly = false) const;
    /**
     * @brief 线程安全扫描数据库中的键，按指定条件收集到列表中。
     * @param iterator 起始迭代器位置。
     * @param count 最大收集键数量。
     * @param type 匹配的键类型过滤条件。
     * @param keys 输出参数，存储结果键的列表。
     * @return 返回下一次迭代的位置索引。
     */
    unsigned long scan_threadsafe(unsigned long iterator, long count, sds type, list *keys) const;

    using redisDbPersistentData::createSnapshot;
    using redisDbPersistentData::endSnapshot;
    using redisDbPersistentData::endSnapshotAsync;
    using redisDbPersistentData::end;
    using redisDbPersistentData::find_cached_threadsafe;
    using redisDbPersistentData::FSnapshot;

    /**
     * @brief 获取随机缓存项的迭代器，用于缓存抽样分析。
     * @param fPrimaryOnly 若为true，则仅从主缓存中选取。
     * @return 返回dict_iter类型的缓存迭代器。
     */
    dict_iter random_cache_threadsafe(bool fPrimaryOnly = false) const;

    expireEntry *getExpire(robj_roptr key) { return getExpire(szFromObj(key)); }
    /**
     * @brief 获取指定键的过期条目（通过C字符串）。
     * @param key 键的C字符串表示。
     * @return 返回关联的expireEntry指针，若不存在则为nullptr。
     */
    expireEntry *getExpire(const char *key);
    /**
     * @brief 获取指定键的过期条目（通过C字符串）的常量版本。
     * @param key 键的C字符串表示。
     * @return 返回关联的expireEntry常量指针，若不存在则为nullptr。
     */
    const expireEntry *getExpire(const char *key) const;
    const expireEntry *getExpire(robj_roptr key) const { return getExpire(szFromObj(key)); }

    /**
     * @brief 获取当前MVCC检查点标识值，用于事务一致性控制。
     * @return 返回64位无符号整型检查点值。
     */
    uint64_t mvccCheckpoint() const { return m_mvccCheckpoint; }

    /**
     * @brief 检查当前快照是否已过时，可能需要更新或重建。
     * @return 若快照过时返回true，否则返回false。
     */
    bool FStale() const;

    // These need to be fixed
    using redisDbPersistentData::size;
    using redisDbPersistentData::expireSize;
};


/* Redis 数据库表示。有多个数据库，由从 0（默认数据库）到最大配置数据库的整数标识。数据库编号是结构中的 'id' 字段。*/
struct redisDb : public redisDbPersistentDataSnapshot
{
    // 遗留 C API，请勿添加更多
    friend void tryResizeHashTables(int);
    friend int dbSyncDelete(redisDb *db, robj *key);
    friend int dbAsyncDelete(redisDb *db, robj *key);
    friend long long emptyDb(int dbnum, int flags, void(callback)(void*));
    friend void scanGenericCommand(struct client *c, robj_roptr o, unsigned long cursor);
    friend int dbSwapDatabases(int id1, int id2);
    friend int removeExpire(redisDb *db, robj *key);
    friend void setExpire(struct client *c, redisDb *db, robj *key, robj *subkey, long long when);
    friend void setExpire(client *c, redisDb *db, robj *key, expireEntry &&e);
    friend int evictionPoolPopulate(int dbid, redisDb *db, bool fVolatile, struct evictionPoolEntry *pool);
    friend void activeDefragCycle(void);
    friend void activeExpireCycle(int);
    friend void expireSlaveKeys(void);
    friend int performEvictions(bool fPreSnapshot);

    typedef ::dict_const_iter const_iter;
    typedef ::dict_iter iter;

    redisDb() = default;

    void initialize(int id, int storage_id=-1 /* 默认无存储 */);
    void storageProviderInitialize();
    void storageProviderDelete();
    virtual ~redisDb();

    void dbOverwriteCore(redisDb::iter itr, sds keySds, robj *val, bool fUpdateMvcc, bool fRemoveExpire);

    bool FKeyExpires(const char *key);
    size_t clear(bool fAsync, void(callback)(void*));

    // 从 redisDbPersistentData 导入被 redisDbPersistentDataSnapshot 隐藏的方法
    using redisDbPersistentData::slots;
    using redisDbPersistentData::size;
    using redisDbPersistentData::expand;
    using redisDbPersistentData::trackkey;
    using redisDbPersistentData::find;
    using redisDbPersistentData::random;
    using redisDbPersistentData::random_expire;
    using redisDbPersistentData::end;
    using redisDbPersistentData::getStats;
    using redisDbPersistentData::insert;
    using redisDbPersistentData::tryResize;
    using redisDbPersistentData::incrementallyRehash;
    using redisDbPersistentData::updateValue;
    using redisDbPersistentData::syncDelete;
    using redisDbPersistentData::asyncDelete;
    using redisDbPersistentData::expireSize;
    using redisDbPersistentData::removeExpire;
    using redisDbPersistentData::removeSubkeyExpire;
    using redisDbPersistentData::clear;
    using redisDbPersistentData::emptyDbAsync;
    using redisDbPersistentData::iterate;
    using redisDbPersistentData::setExpire;
    using redisDbPersistentData::trackChanges;
    using redisDbPersistentData::processChanges;
    using redisDbPersistentData::processChangesAsync;
    using redisDbPersistentData::commitChanges;
    using redisDbPersistentData::endSnapshot;
    using redisDbPersistentData::restoreSnapshot;
    using redisDbPersistentData::removeAllCachedValues;
    using redisDbPersistentData::disableKeyCache;
    using redisDbPersistentData::keycacheIsEnabled;
    using redisDbPersistentData::dictUnsafeKeyOnly;
    using redisDbPersistentData::prefetchKeysAsync;
    using redisDbPersistentData::prepOverwriteForSnapshot;
    using redisDbPersistentData::FRehashing;
    using redisDbPersistentData::FTrackingChanges;
    using redisDbPersistentData::CloneStorageCache;
    using redisDbPersistentData::getStorageCache;
    using redisDbPersistentData::bulkDirectStorageInsert;

public:
    const redisDbPersistentDataSnapshot *createSnapshot(uint64_t mvccCheckpoint, bool fOptional) {
        auto psnapshot = redisDbPersistentData::createSnapshot(mvccCheckpoint, fOptional);
        if (psnapshot != nullptr)
            mvccLastSnapshot = psnapshot->mvccCheckpoint();
        return psnapshot;
    }

    unsigned long expires_cursor = 0;
    dict *blocking_keys;        /* 具有等待数据的客户端的键 (BLPOP) */
    dict *ready_keys;           /* 收到 PUSH 的被阻塞键 */
    dict *watched_keys;         /* 用于 MULTI/EXEC CAS 的 WATCHED 键 */
    int id;                     /* 数据库 ID */
    int storage_id;             /* 映射的存储提供程序数据库 ID，与上面的 redisdb ID 相同。但是，当数据库交换时，上面的 redisdb ID 可能会被交换以与数据库索引 (id <-> g_pserver->db[index]) 保持一致，但是 storage_id 保持不变，以维护与底层存储提供程序数据库的正确映射。这仅在设置了存储提供程序时有效。*/
    long long last_expire_set;  /* 上次设置过期时间的时间 */
    double avg_ttl;             /* 平均 TTL，仅用于统计 */
    list *defrag_later;         /* 逐个尝试整理的键名列表，逐渐进行。*/
    uint64_t mvccLastSnapshot = 0;
};

/* 声明数据库备份，包括 Redis 主数据库和槽到键的映射。
 * 定义在 db.c 中。我们不能在这里定义它，因为我们在 cluster.h 中定义了 CLUSTER_SLOTS。*/
typedef struct dbBackup dbBackup;

/* 声明数据库备份，包括 Redis 主数据库和槽到键的映射。
 * 定义在 db.c 中。我们不能在这里定义它，因为我们在 cluster.h 中定义了 CLUSTER_SLOTS。*/
typedef struct dbBackup dbBackup;

/* 客户端 MULTI/EXEC 状态 */
typedef struct multiCmd {
    robj **argv;
    int argc;
    struct redisCommand *cmd;
} multiCmd;

typedef struct multiState {
    multiCmd *commands;     /* MULTI 命令数组 */
    int count;              /* MULTI 命令总数 */
    int cmd_flags;          /* 累积的命令标志（OR 运算）。
                               因此，如果至少有一个命令具有给定标志，则它将在此字段中设置。*/
    int cmd_inv_flags;      /* 与 cmd_flags 相同，对 ~flags 进行 OR 运算。以便可以知道所有命令是否都具有某个标志。*/
} multiState;

struct listPos {
    int wherefrom;      /* 从哪里弹出 */
    int whereto;        /* 推送到哪里 */
};                      /* src/dst 列表中我们想要为 BLPOP、BRPOP 和 BLMOVE 弹出/推送元素的位置。*/

/* 此结构保存客户端的阻塞操作状态。
 * 使用的字段取决于 client->btype。*/
typedef struct blockingState {
    /* 通用字段。*/
    mstime_t timeout;       /* 阻塞操作超时。如果 UNIX 当前时间 > 超时，则操作超时。*/

    /* BLOCKED_LIST、BLOCKED_ZSET 和 BLOCKED_STREAM */
    ::dict *keys;             /* 我们等待终止阻塞操作（例如 BLPOP 或 XREAD）的键。或 NULL。*/
    robj *target;           /* 应该接收元素的键，用于 BLMOVE。*/

    listPos listpos;

    /* BLOCK_STREAM */
    size_t xread_count;     /* XREAD COUNT 选项。*/
    robj *xread_group;      /* XREADGROUP 组名。*/
    robj *xread_consumer;   /* XREADGROUP 消费者名称。*/
    int xread_group_noack;

    /* BLOCKED_WAIT */
    int numreplicas;        /* 我们等待 ACK 的副本数量。*/
    long long reploffset;   /* 要达到的复制偏移量。*/

    /* BLOCKED_MODULE */
    void *module_blocked_handle; /* RedisModuleBlockedClient 结构。
                                    对于 Redis 内核是不透明的，仅在 module.c 中处理。*/
} blockingState;

/* 以下结构表示 g_pserver->ready_keys 列表中的一个节点，
 * 我们在该列表中累积所有因阻塞操作（如 B[LR]POP）而被阻塞的客户端，
 * 但在上次执行的命令的上下文中收到了新数据。
 *
 * 在执行每个命令或脚本之后，我们运行此列表以检查是否应因此向被阻塞的客户端提供数据，从而解除阻塞。
 * 请注意，g_pserver->ready_keys 不会有重复项，因为在表示 Redis 数据库的每个结构中也有一个名为 ready_keys 的字典，
 * 我们确保记住给定键是否已添加到 g_pserver->ready_keys 列表中。*/
typedef struct readyList {
    redisDb *db;
    robj *key;
} readyList;

/* 此结构表示 Redis 用户。这对于 ACL很有用，
 * 用户在连接经过身份验证后与连接关联。
 * 如果没有关联的用户，则连接使用默认用户。*/
#define USER_COMMAND_BITS_COUNT 1024    /* 用户结构中命令位的总数。我们可以在用户中设置的最后一个有效命令 ID 是 USER_COMMAND_BITS_COUNT-1。*/
#define USER_FLAG_ENABLED (1<<0)        /* 用户处于活动状态。*/
#define USER_FLAG_DISABLED (1<<1)       /* 用户被禁用。*/
#define USER_FLAG_ALLKEYS (1<<2)        /* 用户可以提及任何键。*/
#define USER_FLAG_ALLCOMMANDS (1<<3)    /* 用户可以运行所有命令。*/
#define USER_FLAG_NOPASS      (1<<4)    /* 用户不需要密码，任何提供的密码都有效。对于默认用户，这也意味着不需要 AUTH，并且每个连接都会立即进行身份验证。*/
#define USER_FLAG_ALLCHANNELS (1<<5)    /* 用户可以提及任何 Pub/Sub 频道。*/
#define USER_FLAG_SANITIZE_PAYLOAD (1<<6)       /* 用户需要深度 RESTORE 负载清理。*/
#define USER_FLAG_SANITIZE_PAYLOAD_SKIP (1<<7)  /* 用户应跳过 RESTORE 负载的深度清理。*/

typedef struct {
    sds name;       /* 用户名（SDS 字符串）。*/
    uint64_t flags; /* 请参阅 USER_FLAG_* */

    /* 如果此用户有权执行此命令，则 allowed_commands 中的位被设置。在具有子命令的命令中，如果此位被设置，则所有子命令也可用。
     *
     * 如果给定命令的位未设置并且该命令具有子命令，Redis 还将检查 allowed_subcommands 以了解该命令是否可以执行。*/
    uint64_t allowed_commands[USER_COMMAND_BITS_COUNT/64];

    /* 此数组为每个命令 ID（对应于 allowed_commands 中设置的命令位）指向一个 SDS 字符串数组（以 NULL 指针终止），其中包含可为此命令执行的所有子命令。当不使用子命令匹配时，该字段仅设置为 NULL 以避免分配 USER_COMMAND_BITS_COUNT 指针。*/
    sds **allowed_subcommands;
    list *passwords; /* 此用户的 SDS 有效密码列表。*/
    list *patterns;  /* 允许的键模式列表。如果此字段为 NULL，则用户不能在命令中提及任何键，除非在用户中设置了 ALLKEYS 标志。*/
    list *channels;  /* 允许的 Pub/Sub 通道模式列表。如果此字段为 NULL，则用户不能在 `PUBLISH` 或 [P][UNSUBSCRIBE] 命令中提及任何通道，除非在用户中设置了 ALLCHANNELS 标志。*/
} user;

/* 对于多路复用，我们需要获取每个客户端的状态。
 * 客户端在链表中获取。*/

#define CLIENT_ID_AOF (UINT64_MAX) /* 为 AOF 客户端保留的 ID。如果您需要更多保留 ID，请使用 UINT64_MAX-1、-2 等。*/

struct parsed_command {
    robj** argv = nullptr;
    int argc = 0;
    int argcMax;
    long long reploff = 0;
    size_t argv_len_sum = 0;    /* Sum of lengths of objects in argv list. */

    parsed_command(int maxargs) {
        argv = (robj**)zmalloc(sizeof(robj*)*maxargs);
        argcMax = maxargs;
    }

    parsed_command &operator=(parsed_command &&o) {
        argv = o.argv;
        argc = o.argc;
        argcMax = o.argcMax;
        reploff = o.reploff;
        o.argv = nullptr;
        o.argc = 0;
        o.argcMax = 0;
        o.reploff = 0;
        return *this;
    }

    parsed_command(parsed_command &o) = delete;
    parsed_command(parsed_command &&o) {
        argv = o.argv;
        argc = o.argc;
        argcMax = o.argcMax;
        reploff = o.reploff;
        o.argv = nullptr;
        o.argc = 0;
        o.argcMax = 0;
        o.reploff = 0;
    }

    ~parsed_command() {
        if (argv != nullptr) {
            for (int i = 0; i < argc; ++i) {
                decrRefCount(argv[i]);
            }
            zfree(argv);
        }
    }
};

struct client {
    uint64_t id;            /* Client incremental unique ID. */
    connection *conn;
    int resp;               /* RESP protocol version. Can be 2 or 3. */
    redisDb *db;            /* Pointer to currently SELECTed DB. */
    robj *name;             /* As set by CLIENT SETNAME. */
    sds querybuf;           /* Buffer we use to accumulate client queries. */
    size_t qb_pos;          /* The position we have read in querybuf. */
    sds pending_querybuf;   /* If this client is flagged as master, this buffer
                               represents the yet not applied portion of the
                               replication stream that we are receiving from
                               the master. */
    size_t querybuf_peak;   /* Recent (100ms or more) peak of querybuf size. */
    int original_argc;      /* Num of arguments of original command if arguments were rewritten. */
    robj **original_argv;   /* Arguments of original command if arguments were rewritten. */
    struct redisCommand *cmd, *lastcmd;  /* Last command executed. */
    ::user *user;             /* User associated with this connection. If the
                               user is set to NULL the connection can do
                               anything (admin). */
    int reqtype;            /* Request protocol type: PROTO_REQ_* */
    int multibulklen;       /* Number of multi bulk arguments left to read. */
    long bulklen;           /* Length of bulk argument in multi bulk request. */
    list *reply;            /* List of reply objects to send to the client. */
    unsigned long long reply_bytes; /* Tot bytes of objects in reply list. */
    size_t sentlen;         /* Amount of bytes already sent in the current
                               buffer or object being sent. */
    time_t ctime;           /* Client creation time. */
    long duration;          /* Current command duration. Used for measuring latency of blocking/non-blocking cmds */
    time_t lastinteraction; /* Time of the last interaction, used for timeout */
    time_t obuf_soft_limit_reached_time;
    std::atomic<uint64_t> flags;              /* Client flags: CLIENT_* macros. */
    int casyncOpsPending;
    int fPendingAsyncWrite; /* NOTE: Not a flag because it is written to outside of the client lock (locked by the global lock instead) */
    std::atomic<bool> fPendingAsyncWriteHandler;
    int authenticated;      /* Needed when the default user requires auth. */
    int replstate;          /* Replication state if this is a replica. */
    int repl_put_online_on_ack; /* Install replica write handler on ACK. */
    int repldbfd;           /* Replication DB file descriptor. */
    off_t repldboff;        /* Replication DB file offset. */
    off_t repldbsize;       /* Replication DB file size. */
    sds replpreamble;       /* Replication DB preamble. */
    time_t repl_down_since; /* When client lost connection. */
    long long read_reploff; /* Read replication offset if this is a master. */
    long long reploff;      /* Applied replication offset if this is a master. */
    long long reploff_cmd;  /* The replication offset of the executing command, reploff gets set to this after the execution completes */
    long long repl_ack_off; /* Replication ack offset, if this is a replica. */
    long long repl_ack_time;/* Replication ack time, if this is a replica. */
    long long repl_last_partial_write; /* The last time the server did a partial write from the RDB child pipe to this replica  */
    long long psync_initial_offset; /* FULLRESYNC reply offset other slaves
                                       copying this replica output buffer
                                       should use. */
                                       
    long long repl_curr_off = -1;/* Replication offset of the replica, also where in the backlog we need to start from
                                  * when sending data to this replica. */
    long long repl_end_off = -1; /* Replication offset to write to, stored in the replica, as opposed to using the global offset 
                                  * to prevent needing the global lock */

    char replid[CONFIG_RUN_ID_SIZE+1]; /* Master replication ID (if master). */
    int slave_listening_port; /* As configured with: REPLCONF listening-port */
    char *slave_addr;       /* Optionally given by REPLCONF ip-address */
    int slave_capa;         /* Slave capabilities: SLAVE_CAPA_* bitwise OR. */
    multiState mstate;      /* MULTI/EXEC state */
    int btype;              /* Type of blocking op if CLIENT_BLOCKED. */
    blockingState bpop;     /* blocking state */
    long long woff;         /* Last write global replication offset. */
    list *watched_keys;     /* Keys WATCHED for MULTI/EXEC CAS */
    ::dict *pubsub_channels;  /* channels a client is interested in (SUBSCRIBE) */
    list *pubsub_patterns;  /* patterns a client is interested in (SUBSCRIBE) */
    sds peerid;             /* Cached peer ID. */
    sds sockname;           /* Cached connection target address. */
    listNode *client_list_node; /* list node in client list */
    listNode *paused_list_node; /* list node within the pause list */
    RedisModuleUserChangedFunc auth_callback; /* Module callback to execute
                                               * when the authenticated user
                                               * changes. */
    void *auth_callback_privdata; /* Private data that is passed when the auth
                                   * changed callback is executed. Opaque for
                                   * Redis Core. */
    void *auth_module;      /* The module that owns the callback, which is used
                             * to disconnect the client if the module is
                             * unloaded for cleanup. Opaque for Redis Core.*/

    /* UUID announced by the client (default nil) - used to detect multiple connections to/from the same peer */
    /* compliant servers will announce their UUIDs when a replica connection is started, and return when asked */
    /* UUIDs are transient and lost when the server is shut down */
    unsigned char uuid[UUID_BINARY_LEN];

    /* If this client is in tracking mode and this field is non zero,
     * invalidation messages for keys fetched by this client will be send to
     * the specified client ID. */
    uint64_t client_tracking_redirection;
    rax *client_tracking_prefixes; /* A dictionary of prefixes we are already
                                      subscribed to in BCAST mode, in the
                                      context of client side caching. */
    /* In clientsCronTrackClientsMemUsage() we track the memory usage of
     * each client and add it to the sum of all the clients of a given type,
     * however we need to remember what was the old contribution of each
     * client, and in which categoty the client was, in order to remove it
     * before adding it the new value. */
    uint64_t client_cron_last_memory_usage;
    int      client_cron_last_memory_type;
    /* Response buffer */
    int bufpos;
    char buf[PROTO_REPLY_CHUNK_BYTES];

    /* Async Response Buffer - other threads write here */
    clientReplyBlock *replyAsync;

    uint64_t mvccCheckpoint = 0;    // the MVCC checkpoint of our last write

    int iel; /* the event loop index we're registered with */
    struct fastlock lock {"client"};
    int master_error;
    std::vector<parsed_command> vecqueuedcmd;
    int argc;
    robj **argv;
    size_t argv_len_sumActive = 0;

    bool FPendingReplicaWrite() const {
        return repl_curr_off != repl_end_off && replstate == SLAVE_STATE_ONLINE;
    }

    // post a function from a non-client thread to run on its client thread
    bool postFunction(std::function<void(client *)> fn, bool fLock = true);
    size_t argv_len_sum() const;
    bool asyncCommand(std::function<void(const redisDbPersistentDataSnapshot *, const std::vector<robj_sharedptr> &)> &&mainFn, 
                        std::function<void(const redisDbPersistentDataSnapshot *)> &&postFn = nullptr);
    char* fprint;
};

struct saveparam {
    time_t seconds;
    int changes;
};

struct moduleLoadQueueEntry {
    sds path;
    int argc;
    robj **argv;
};

struct sentinelLoadQueueEntry {
    int argc;
    sds *argv;
    int linenum;
    sds line;
};

struct sentinelConfig {
    list *pre_monitor_cfg;
    list *monitor_cfg;
    list *post_monitor_cfg;
};

struct sharedObjectsStruct {
    robj *crlf, *ok, *err, *emptybulk, *czero, *cone, *pong, *space,
    *colon, *queued, *nullbulk, *null[4], *nullarray[4], *emptymap[4], *emptyset[4],
    *emptyarray, *wrongtypeerr, *nokeyerr, *syntaxerr, *sameobjecterr,
    *outofrangeerr, *noscripterr, *loadingerr, *slowscripterr, *bgsaveerr,
    *masterdownerr, *roslaveerr, *execaborterr, *noautherr, *noreplicaserr,
    *busykeyerr, *oomerr, *plus, *messagebulk, *pmessagebulk, *subscribebulk,
    *unsubscribebulk, *psubscribebulk, *punsubscribebulk, *del, *unlink,
    *rpop, *lpop, *lpush, *rpoplpush, *lmove, *blmove, *zpopmin, *zpopmax,
    *emptyscan, *multi, *exec, *left, *right, *hset, *srem, *xgroup, *xclaim,  
    *script, *replconf, *eval, *persist, *set, *pexpireat, *pexpire, 
    *time, *pxat, *px, *retrycount, *force, *justid, 
    *lastid, *ping, *replping, *setid, *keepttl, *load, *createconsumer,
    *getack, *special_asterick, *special_equals, *default_username,
    *hdel, *zrem, *mvccrestore, *pexpirememberat, *redacted,
    *select[PROTO_SHARED_SELECT_CMDS],
    *integers[OBJ_SHARED_INTEGERS],
    *mbulkhdr[OBJ_SHARED_BULKHDR_LEN], /* "*<value>\r\n" */
    *bulkhdr[OBJ_SHARED_BULKHDR_LEN];  /* "$<value>\r\n" */
    sds minstring, maxstring;
};

/* ZSETs use a specialized version of Skiplists */
struct zskiplistLevel {
        struct zskiplistNode *forward;
        unsigned long span;
};
typedef struct zskiplistNode {
    sds ele;
    double score;
    struct zskiplistNode *backward;
    
#ifdef __cplusplus
    zskiplistLevel *level(size_t idx) {
        return reinterpret_cast<zskiplistLevel*>(this+1) + idx;
    }
#else
    struct zskiplistLevel level[];
#endif
} zskiplistNode;

typedef struct zskiplist {
    struct zskiplistNode *header, *tail;
    unsigned long length;
    int level;
} zskiplist;

typedef struct zset {
    ::dict *dict;
    zskiplist *zsl;
} zset;

typedef struct clientBufferLimitsConfig {
    unsigned long long hard_limit_bytes;
    unsigned long long soft_limit_bytes;
    time_t soft_limit_seconds;
} clientBufferLimitsConfig;

extern clientBufferLimitsConfig clientBufferLimitsDefaults[CLIENT_TYPE_OBUF_COUNT];

/* The redisOp structure defines a Redis Operation, that is an instance of
 * a command with an argument vector, database ID, propagation target
 * (PROPAGATE_*), and command pointer.
 *
 * Currently only used to additionally propagate more commands to AOF/Replication
 * after the propagation of the executed command. */
typedef struct redisOp {
    robj **argv;
    int argc, dbid, target;
    struct redisCommand *cmd;
} redisOp;

/* Defines an array of Redis operations. There is an API to add to this
 * structure in an easy way.
 *
 * redisOpArrayInit();
 * redisOpArrayAppend();
 * redisOpArrayFree();
 */
typedef struct redisOpArray {
    redisOp *ops;
    int numops;
} redisOpArray;

/* This structure is returned by the getMemoryOverheadData() function in
 * order to return memory overhead information. */
struct redisMemOverhead {
    size_t peak_allocated;
    size_t total_allocated;
    size_t startup_allocated;
    size_t repl_backlog;
    size_t clients_slaves;
    size_t clients_normal;
    size_t aof_buffer;
    size_t lua_caches;
    size_t overhead_total;
    size_t dataset;
    size_t total_keys;
    size_t bytes_per_key;
    float dataset_perc;
    float peak_perc;
    float total_frag;
    ssize_t total_frag_bytes;
    float allocator_frag;
    ssize_t allocator_frag_bytes;
    float allocator_rss;
    ssize_t allocator_rss_bytes;
    float rss_extra;
    size_t rss_extra_bytes;
    size_t num_dbs;
    struct {
        size_t dbid;
        size_t overhead_ht_main;
        size_t overhead_ht_expires;
    } *db;
};


struct redisMaster {
    char *masteruser;               /* AUTH with this user and masterauth with master */
    char *masterauth;               /* AUTH with this password with master */
    char *masterhost;               /* Hostname of master */
    int masterport;                 /* Port of master */
    client *cached_master;          /* Cached master to be reused for PSYNC. */
    client *master;
    /* The following two fields is where we store master PSYNC replid/offset
     * while the PSYNC is in progress. At the end we'll copy the fields into
     * the server->master client structure. */
    char master_replid[CONFIG_RUN_ID_SIZE+1];  /* Master PSYNC runid. */
    long long master_initial_offset;           /* Master PSYNC offset. */

    bool isActive = false;
    bool isKeydbFastsync = false;
    int repl_state;          /* Replication status if the instance is a replica */
    off_t repl_transfer_size; /* Size of RDB to read from master during sync. */
    off_t repl_transfer_read; /* Amount of RDB read from master during sync. */
    off_t repl_transfer_last_fsync_off; /* Offset when we fsync-ed last time. */
    connection *repl_transfer_s;     /* Replica -> Master SYNC socket */
    int repl_transfer_fd;    /* Replica -> Master SYNC temp file descriptor */
    char *repl_transfer_tmpfile; /* Replica-> master SYNC temp file name */
    time_t repl_transfer_lastio; /* Unix time of the latest read, for timeout */
    time_t repl_down_since; /* Unix time at which link with master went down */

    class SnapshotPayloadParseState *parseState;
    sds bulkreadBuffer = nullptr;

    unsigned char master_uuid[UUID_BINARY_LEN];  /* Used during sync with master, this is our master's UUID */
                                                /* After we've connected with our master use the UUID in g_pserver->master */
    uint64_t mvccLastSync;
    /* During a handshake the server may have stale keys, we track these here to share once a reciprocal connection is made */
    std::map<int, std::vector<robj_sharedptr>> *staleKeyMap;
    int ielReplTransfer = -1;
};

struct MasterSaveInfo {
    MasterSaveInfo() {
        memcpy(master_replid, "0000000000000000000000000000000000000000", sizeof(master_replid));
    }
    MasterSaveInfo(const redisMaster &mi) {
        memcpy(master_replid, mi.master_replid, sizeof(mi.master_replid));
        if (mi.master) {
            master_initial_offset = mi.master->reploff;
            selected_db = mi.master->db->id;
        } else if (mi.cached_master) {
            master_initial_offset = mi.cached_master->reploff;
            selected_db = mi.cached_master->db->id;
        } else {
            master_initial_offset = -1;
            selected_db = 0;
        }
        masterport = mi.masterport;
        if (mi.masterhost)
            masterhost = sdsstring(sdsdup(mi.masterhost));
        masterport = mi.masterport;
    }
    MasterSaveInfo(const MasterSaveInfo &other) {
        masterhost = other.masterhost;
        masterport = other.masterport;
        memcpy(master_replid, other.master_replid, sizeof(master_replid));
        master_initial_offset = other.master_initial_offset;
    }

    MasterSaveInfo &operator=(const MasterSaveInfo &other) {
        masterhost = other.masterhost;
        masterport = other.masterport;
        memcpy(master_replid, other.master_replid, sizeof(master_replid));
        master_initial_offset = other.master_initial_offset;
        return *this;
    }

    sdsstring masterhost;
    int masterport;
    char master_replid[CONFIG_RUN_ID_SIZE+1];
    long long master_initial_offset;
    int selected_db;
};

/* This structure can be optionally passed to RDB save/load functions in
 * order to implement additional functionalities, by storing and loading
 * metadata to the RDB file.
 *
 * Currently the only use is to select a DB at load time, useful in
 * replication in order to make sure that chained slaves (slaves of slaves)
 * select the correct DB and are able to accept the stream coming from the
 * top-level master. */
/**
 * @class rdbSaveInfo
 * @brief 管理RDB持久化过程中的复制相关信息
 *
 * 该类用于在RDB文件保存和加载过程中维护复制相关的元数据，
 * 包括复制ID、偏移量、主从节点关系等关键信息。
 */
class rdbSaveInfo {
public:
    /**
     * @brief 默认构造函数
     *
     * 初始化复制相关参数为默认值：
     * - repl_stream_db 设置为-1表示无效数据库索引
     * - repl_id 使用全零字符串填充
     * - repl_offset 设置为-1表示无效偏移量
     * - 强制设置键标志置为TRUE
     * - MVCC最小阈值初始化为0
     */
    rdbSaveInfo() {
        repl_stream_db = -1;
        repl_id_is_set = 0;
        memcpy(repl_id, "0000000000000000000000000000000000000000", sizeof(repl_id));
        repl_id[CONFIG_RUN_ID_SIZE] = '\0';
        repl_offset = -1;
        fForceSetKey = TRUE;
        mvccMinThreshold = 0;
    }

    /**
     * @brief 拷贝构造函数
     *
     * @param other 要复制的源对象
     * 深度复制所有成员变量，包括：
     * - 复制ID状态
     * - 复制偏移量
     * - 主从节点关系向量
     * - MVCC阈值
     */
    rdbSaveInfo(const rdbSaveInfo &other) {
        repl_stream_db = other.repl_stream_db;
        repl_id_is_set = other.repl_id_is_set;
        memcpy(repl_id, other.repl_id, sizeof(repl_id));
        repl_offset = other.repl_offset;
        fForceSetKey = other.fForceSetKey;
        mvccMinThreshold = other.mvccMinThreshold;
        vecmastersaveinfo = other.vecmastersaveinfo;
        master_repl_offset = other.master_repl_offset;
        mi = other.mi;
    }

    /**
     * @brief 赋值运算符
     *
     * @param other 要复制的源对象
     * @return rdbSaveInfo& 当前对象引用
     *
     * 复制所有成员变量并返回自身引用，
     * 保持与拷贝构造函数一致的复制逻辑
     */
    rdbSaveInfo &operator=(const rdbSaveInfo &other) {
        repl_stream_db = other.repl_stream_db;
        repl_id_is_set = other.repl_id_is_set;
        memcpy(repl_id, other.repl_id, sizeof(repl_id));
        repl_offset = other.repl_offset;
        fForceSetKey = other.fForceSetKey;
        mvccMinThreshold = other.mvccMinThreshold;
        vecmastersaveinfo = other.vecmastersaveinfo;
        master_repl_offset = other.master_repl_offset;
        mi = other.mi;

        return *this;
    }

    /**
     * @brief 添加主节点保存信息
     *
     * @param si 要添加的主节点信息对象
     * 将指定的主节点信息追加到vecmastersaveinfo向量末尾
     */
    void addMaster(const MasterSaveInfo &si) {
        vecmastersaveinfo.push_back(si);
    }

    /**
     * @brief 获取主节点数量
     *
     * @return size_t 当前存储的主节点信息数量
     * 返回vecmastersaveinfo向量的元素个数
     */
    size_t numMasters() {
        return vecmastersaveinfo.size();
    }

    /* Used saving and loading. */
    /**
     * @brief 目标数据库索引
     *
     * 指定在主客户端中需要选择的数据库索引号
     * 仅在RDB保存和加载过程中使用
     */
    int repl_stream_db;  /* DB to select in g_pserver->master client. */

    /* Used only loading. */
    /**
     * @brief 复制ID有效性标志
     *
     * 标示repl_id字段是否已被正确设置
     * 非零值表示复制ID有效
     */
    int repl_id_is_set;  /* True if repl_id field is set. */

    /**
     * @brief 复制ID字符串
     *
     * 存储40字节长度的复制ID（含终止符）
     * 符合CONFIG_RUN_ID_SIZE配置定义的长度要求
     */
    char repl_id[CONFIG_RUN_ID_SIZE+1];     /* Replication ID. */

    /**
     * @brief 复制偏移量
     *
     * 记录当前复制进度的64位整数偏移量
     * -1表示无效偏移量
     */
    long long repl_offset;                  /* Replication offset. */

    /**
     * @brief 强制设置键标志
     *
     * 控制是否强制设置键值的布尔标志
     * TRUE表示忽略常规条件强制进行设置
     */
    int fForceSetKey;

    /* Used In Save */
    /**
     * @brief 主节点复制偏移量
     *
     * 保存过程中记录主节点的复制偏移量
     * 用于维护复制链的一致性
     */
    long long master_repl_offset;

    /**
     * @brief MVCC最小版本阈值
     *
     * 64位无符号整数，控制多版本并发控制的最小版本号
     * 用于优化快照隔离级别的内存管理
     */
    uint64_t mvccMinThreshold;

    /**
     * @brief 主节点信息向量
     *
     * 存储所有关联主节点的保存信息
     * 支持多级复制拓扑的持久化记录
     */
    std::vector<MasterSaveInfo> vecmastersaveinfo;

    /**
     * @brief Redis主节点指针
     *
     * 指向关联的redisMaster结构体
     * 默认初始化为空指针
     */
    struct redisMaster *mi = nullptr;
};

struct malloc_stats {
    size_t zmalloc_used;
    size_t process_rss;
    size_t allocator_allocated;
    size_t allocator_active;
    size_t allocator_resident;
    size_t sys_total;
    size_t sys_available;
};

typedef struct socketFds {
    int fd[CONFIG_BINDADDR_MAX];
    int count;
} socketFds;

/*-----------------------------------------------------------------------------
 * TLS Context Configuration
 *----------------------------------------------------------------------------*/

typedef struct redisTLSContextConfig {
    char *cert_file;                /* 服务器端以及可选的客户端证书文件名 */
    char *key_file;                 /* cert_file 的私钥文件名 */
    char *key_file_pass;            /* key_file 的可选密码 */
    char *client_cert_file;         /* 作为客户端使用的证书；如果无，则使用 cert_file */
    char *client_key_file;          /* client_cert_file 的私钥文件名 */
    char *client_key_file_pass;     /* client_key_file 的可选密码 */
    char *dh_params_file;
    char *ca_cert_file;
    char *ca_cert_dir;
    char *protocols;
    char *ciphers;
    char *ciphersuites;
    int prefer_server_ciphers;
    int session_caching;
    int session_cache_size;
    int session_cache_timeout;
    time_t cert_file_last_modified;
    time_t key_file_last_modified;
    time_t client_cert_file_last_modified;
    time_t client_key_file_last_modified;
    time_t ca_cert_file_last_modified;
    time_t ca_cert_dir_last_modified;
} redisTLSContextConfig;

/*-----------------------------------------------------------------------------
 * Global server state
 *----------------------------------------------------------------------------*/

struct clusterState;

/* AIX 将 hz 定义为 __hz，我们不使用此定义，为了允许
 * Redis 在 AIX 上构建，我们需要取消定义它。 */
#ifdef _AIX
#undef hz
#endif

#define CHILD_TYPE_NONE 0
#define CHILD_TYPE_RDB 1
#define CHILD_TYPE_AOF 2
#define CHILD_TYPE_LDB 3
#define CHILD_TYPE_MODULE 4

typedef enum childInfoType {
    CHILD_INFO_TYPE_CURRENT_INFO,
    CHILD_INFO_TYPE_AOF_COW_SIZE,
    CHILD_INFO_TYPE_RDB_COW_SIZE,
    CHILD_INFO_TYPE_MODULE_COW_SIZE
} childInfoType;

#define MAX_EVENT_LOOPS 16
#define IDX_EVENT_LOOP_MAIN 0

class GarbageCollectorCollection
{
    GarbageCollector<redisDbPersistentDataSnapshot> garbageCollectorSnapshot;
    GarbageCollector<ICollectable> garbageCollectorGeneric;

    class CPtrCollectable : public ICollectable 
    {
        void *m_pv;

    public:
        CPtrCollectable(void *pv) 
            : m_pv(pv)
            {}

        CPtrCollectable(CPtrCollectable &&move) {
            m_pv = move.m_pv;
            move.m_pv = nullptr;
        }

        virtual ~CPtrCollectable() {
            zfree(m_pv);
        }
    };

public:
    struct Epoch
    {
        uint64_t epochSnapshot = 0;
        uint64_t epochGeneric = 0;

        void reset() {
            epochSnapshot = 0;
            epochGeneric = 0;
        }

        Epoch() = default;

        Epoch (const Epoch &other) {
            epochSnapshot = other.epochSnapshot;
            epochGeneric = other.epochGeneric;
        }

        Epoch &operator=(const Epoch &other) {
            serverAssert(isReset());
            epochSnapshot = other.epochSnapshot;
            epochGeneric = other.epochGeneric;
            return *this;
        }

        bool isReset() const {
            return epochSnapshot == 0 && epochGeneric == 0;
        }
    };

    Epoch startEpoch()
    {
        Epoch e;
        e.epochSnapshot = garbageCollectorSnapshot.startEpoch();
        e.epochGeneric = garbageCollectorGeneric.startEpoch();
        return e;
    }

    void endEpoch(Epoch &e, bool fNoFree = false)
    {
        auto epochSnapshot = e.epochSnapshot;
        auto epochGeneric = e.epochGeneric;
        e.reset();  // We must do this early as GC'd dtors can themselves try to enqueue more data
        garbageCollectorSnapshot.endEpoch(epochSnapshot, fNoFree);
        garbageCollectorGeneric.endEpoch(epochGeneric, fNoFree);
    }

    bool empty()
    {
        return garbageCollectorGeneric.empty() && garbageCollectorSnapshot.empty();
    }

    void shutdown()
    {
        garbageCollectorSnapshot.shutdown();
        garbageCollectorGeneric.shutdown();
    }

    void enqueue(Epoch e, std::unique_ptr<redisDbPersistentDataSnapshot> &&sp)
    {
        garbageCollectorSnapshot.enqueue(e.epochSnapshot, std::move(sp));
    }

    void enqueue(Epoch e, std::unique_ptr<ICollectable> &&sp)
    {
        garbageCollectorGeneric.enqueue(e.epochGeneric, std::move(sp));
    }

    template<typename T>
    void enqueueCPtr(Epoch e, T p)
    {
        auto sp = std::make_unique<CPtrCollectable>(reinterpret_cast<void*>(p));
        enqueue(e, std::move(sp));
    }
};

// 可在没有锁的情况下访问的每线程变量
struct redisServerThreadVars {
    aeEventLoop *el = nullptr;
    socketFds ipfd;             /* TCP 套接字文件描述符 */
    socketFds tlsfd;            /* TLS 套接字文件描述符 */
    int in_eval;                /* 我们是否在 EVAL 内部？ */
    int in_exec;                /* 我们是否在 EXEC 内部？ */
    std::vector<client*> clients_pending_write; /* 有要写入或安装处理程序。 */
    list *unblocked_clients;     /* 在下一个循环之前要取消阻塞的客户端列表 非线程安全 */
    list *clients_pending_asyncwrite;
    int cclients;
    int cclientsReplica = 0;
    client *current_client; /* 当前客户端 */
    long fixed_time_expire = 0;     /* 如果 > 0，则根据 server.mstime 使密钥过期。 */
    client *lua_client = nullptr;   /* 用于从 Lua 查询 Redis 的“伪客户端” */
    struct fastlock lockPendingWrite { "thread pending write" };
    char neterr[ANET_ERR_LEN];   /* anet.c 的错误缓冲区 */
    long unsigned commandsExecuted = 0; /* 执行的命令总数 */
    GarbageCollectorCollection::Epoch gcEpoch;
    const redisDbPersistentDataSnapshot **rgdbSnapshot = nullptr;
    long long stat_total_error_replies; /* 发出的错误回复总数（命令 + 拒绝的错误） */
    long long prev_err_count; /* 调用期间现有错误的每线程标记 */
    bool fRetrySetAofEvent = false;
    bool modulesEnabledThisAeLoop = false; /* 在 aeMain 的这个循环中，模块是否在之前启用了
                                              线程进入睡眠状态？ */
    bool disable_async_commands = false; /* 这仅对 AE 循环的一个周期有效，并在 afterSleep 中重置 */
    
    int propagate_in_transaction = 0;  /* 确保我们不传播嵌套的 MULTI/EXEC */
    int client_pause_in_transaction = 0; /* 在此 Exec 期间是否执行了客户端暂停？ */
    std::vector<client*> vecclientsProcess;
    dictAsyncRehashCtl *rehashCtl = nullptr;

    int getRdbKeySaveDelay();
private:
    int rdb_key_save_delay = -1; // 线程本地缓存
};

// 工作线程启动后，常量变量不会更改
struct redisServerConst {
    pid_t pid;                  /* 主进程 pid。 */
    time_t stat_starttime;          /* 服务器启动时间 */
    pthread_t main_thread_id;         /* 主线程 id */
    pthread_t time_thread_id;
    char *configfile;           /* 绝对配置文件路径，或 NULL */
    char *executable;           /* 绝对可执行文件路径。 */
    char **exec_argv;           /* 可执行文件 argv 向量（副本）。 */

    int cthreads;               /* 主工作线程数 */
    int fThreadAffinity;        /* 我们应该将线程固定到核心吗？ */
    int threadAffinityOffset = 0; /* 我们应该从哪里开始固定它们？ */
    char *pidfile;              /* PID 文件路径 */

    /* 指向经常查找的命令的快速指针 */
    struct redisCommand *delCommand, *multiCommand, *lpushCommand,
                        *lpopCommand, *rpopCommand, *zpopminCommand,
                        *zpopmaxCommand, *sremCommand, *execCommand,
                        *expireCommand, *pexpireCommand, *xclaimCommand,
                        *xgroupCommand, *rreplayCommand, *rpoplpushCommand,
                        *hdelCommand, *zremCommand, *lmoveCommand;

    /* 配置 */
    char *default_masteruser;               /* 使用此用户向主服务器进行 AUTH 身份验证，并使用 masterauth */
    char *default_masterauth;               /* 使用此密码向主服务器进行 AUTH 身份验证 */
    int verbosity;                  /* keydb.conf 中的日志级别 */
    int maxidletime;                /* 客户端超时时间（秒） */
    int tcpkeepalive;               /* 如果非零，则设置 SO_KEEPALIVE。 */
    int active_expire_enabled;      /* 出于测试目的可以禁用。 */
    int active_defrag_enabled;
    int jemalloc_bg_thread;         /* 启用 jemalloc 后台线程 */
    size_t active_defrag_ignore_bytes; /* 启动主动碎片的最小碎片浪费量 */
    int active_defrag_threshold_lower; /* 启动主动碎片的最小碎片百分比 */
    int active_defrag_threshold_upper; /* 我们使用最大努力时的最大碎片百分比 */
    int active_defrag_cycle_min;       /* CPU 百分比中碎片整理的最小努力 */
    int active_defrag_cycle_max;       /* CPU 百分比中碎片整理的最大努力 */
    unsigned long active_defrag_max_scan_fields; /* 从主字典扫描中处理的 set/hash/zset/list 的最大字段数 */
    size_t client_max_querybuf_len; /* 客户端查询缓冲区长度限制 */
    int dbnum = 0;                      /* 配置的数据库总数 */
    int supervised;                 /* 如果受监管则为 1，否则为 0。 */
    int supervised_mode;            /* 参见 SUPERVISED_* */
    int daemonize;                  /* 如果作为守护进程运行则为 True */
    int sanitize_dump_payload;      /* 在 RDB 和 RESTORE 中启用 ziplist 和 listpack 的深度清理。 */
    int skip_checksum_validation;   /* 禁用 RDB 和 RESTORE 有效负载的校验和验证。 */
    int set_proc_title;             /* 如果更改进程标题则为 True */
    char *proc_title_template;      /* 进程标题模板格式 */
    clientBufferLimitsConfig client_obuf_limits[CLIENT_TYPE_OBUF_COUNT];

    /* 系统硬件信息 */
    size_t system_memory_size;  /* 操作系统报告的系统总内存 */

    unsigned char uuid[UUID_BINARY_LEN];         /* 此服务器的 UUID - 在启动时填充 */

    int enable_motd;            /* 使用 CURL 请求检索今日消息的标志*/

    int delete_on_evict = false;   // 仅当设置了存储提供程序时有效
    int thread_min_client_threshold = 50;
    int multimaster_no_forward;
    int storage_memory_model = STORAGE_WRITETHROUGH;
    char *storage_conf = nullptr;
    int fForkBgSave = false;
    int time_thread_priority = false;
    long long repl_backlog_disk_size = 0;
    int force_backlog_disk = 0;
};

struct redisServer {
    /* 通用 */
    int dynamic_hz;             /* 根据客户端数量更改 hz 值。 */
    int config_hz;              /* 配置的 HZ 值。如果启用了 dynamic-hz，
                                   可能与实际的 'hz' 字段值不同。 */
    mode_t umask;               /* 进程启动时的 umask 值 */
    std::atomic<int> hz;        /* serverCron() 调用频率 (赫兹) */
    int in_fork_child;          /* 表明这是一个 fork 子进程 */
    IStorage *metadataDb = nullptr;
    redisDb **db = nullptr;
    dict *commands;             /* 命令表 */
    dict *orig_commands;        /* 命令重命名之前的命令表。 */


    struct redisServerThreadVars rgthreadvar[MAX_EVENT_LOOPS];
    struct redisServerThreadVars modulethreadvar; /* 供模块线程使用的服务器线程本地变量 */
    pthread_t rgthread[MAX_EVENT_LOOPS];

    std::atomic<unsigned int> lruclock;      /* LRU 逐出时钟 */
    std::atomic<int> shutdown_asap;          /* 需要尽快关机 (SHUTDOWN needed ASAP) */
    rax *errors;                /* 错误表 */
    int activerehashing;        /* serverCron() 中的增量 rehash */
    int active_defrag_running;  /* 主动碎片整理正在运行 (保存当前扫描的积极程度) */
    int enable_async_rehash = 1;    /* 我们应该使用异步 rehash 功能吗？ */
    int cronloops;              /* cron 函数运行次数 */
    char runid[CONFIG_RUN_ID_SIZE+1];  /* 每次执行时 ID 都不同。 */
    int sentinel_mode;          /* 如果此实例是 Sentinel 则为 True。 */
    size_t initial_memory_usage; /* 初始化后使用的字节数。 */
    int always_show_logo;       /* 即使对于非 stdout 日志记录也显示徽标。 */
    char *ignore_warnings;      /* 配置：应忽略的警告。 */
    pause_type client_pause_type;      /* 如果客户端当前已暂停则为 True */
    /* 模块 */
    ::dict *moduleapi;            /* 模块的导出核心 API 字典。 */
    ::dict *sharedapi;            /* 类似 moduleapi，但包含模块之间共享的 API。
                                    */
    list *loadmodule_queue;     /* 启动时要加载的模块列表。 */
    pid_t child_pid;            /* 当前子进程的 PID */
    int child_type;             /* 当前子进程的类型 */
    client *module_client;      /* 用于从模块调用 Redis 的“伪”客户端 */
    /* 网络 */
    int port;                   /* TCP 监听端口 */
    int tls_port;               /* TLS 监听端口 */
    int tcp_backlog;            /* TCP listen() backlog */
    char *bindaddr[CONFIG_BINDADDR_MAX]; /* 我们应该绑定到的地址 */
    int bindaddr_count;         /* g_pserver->bindaddr[] 中的地址数量 */
    char *unixsocket;           /* UNIX 套接字路径 */
    mode_t unixsocketperm;      /* UNIX 套接字权限 */
    int sofd;                   /* Unix 套接字文件描述符 */
    socketFds cfd;              /* 集群总线监听套接字 */
    list *clients;              /* 活动客户端列表 */
    list *clients_to_close;     /* 需要异步关闭的客户端 */
    list *slaves, *monitors;    /* 从服务器和 MONITOR 客户端列表 */
    rax *clients_timeout_table; /* 用于阻塞客户端超时的基数树。 */
    long fixed_time_expire;     /* 如果 > 0，则根据 server.mstime 使键过期。 */
    rax *clients_index;         /* 按客户端 ID 索引的活动客户端字典。 */
    list *paused_clients;       /* 已暂停客户端列表 */
    mstime_t client_pause_end_time; /* 解除客户端暂停的时间 */
    ::dict *migrate_cached_sockets;/* MIGRATE 缓存的套接字 */
    std::atomic<uint64_t> next_client_id; /* 下一个客户端唯一 ID。递增。 */
    int protected_mode;         /* 不接受外部连接。 */
    long long events_processed_while_blocked; /* processEventsWhileBlocked() 处理的事件数 */

    /* RDB / AOF 加载信息 */
    std::atomic<int> loading; /* 如果为 true，表示正在从磁盘加载数据 */
    off_t loading_total_bytes;
    off_t loading_rdb_used_mem;
    off_t loading_loaded_bytes;
    time_t loading_start_time;
    unsigned long loading_process_events_interval_bytes;
    unsigned int loading_process_events_interval_keys;

    int active_expire_enabled;      /* 可以禁用以进行测试。 */
    int active_expire_effort;       /* 从 1 (默认) 到 10，主动过期的努力程度。 */

    int replicaIsolationFactor = 1;

    /* 仅用于统计的字段 */
    long long stat_numcommands;     /* 已处理命令的数量 */
    long long stat_numconnections;  /* 收到的连接数 */
    long long stat_expiredkeys;     /* 过期键的数量 */
    double stat_expired_stale_perc; /* 可能已过期键的百分比 */
    long long stat_expired_time_cap_reached_count; /* 提前过期周期停止的次数。*/
    long long stat_expire_cycle_time_used; /* 累积使用的微秒数。 */
    long long stat_evictedkeys;     /* 因 maxmemory 而被逐出的键数量 */
    long long stat_keyspace_hits;   /* 成功查找键的次数 */
    long long stat_keyspace_misses; /* 失败查找键的次数 */
    long long stat_active_defrag_hits;      /* 移动的分配数量 */
    long long stat_active_defrag_misses;    /* 已扫描但未移动的分配数量 */
    long long stat_active_defrag_key_hits;  /* 具有已移动分配的键数量 */
    long long stat_active_defrag_key_misses;/* 已扫描且未移动的键数量 */
    long long stat_active_defrag_scanned;   /* 已扫描的 dictEntries 数量 */
    size_t stat_peak_memory;        /* 最大已用内存记录 */
    long long stat_fork_time;       /* 执行最近一次 fork() 所需的时间 */
    double stat_fork_rate;          /* Fork 速率 (GB/秒)。 */
    long long stat_total_forks;     /* fork 总次数。 */
    long long stat_rejected_conn;   /* 因 maxclients 而被拒绝的客户端数量 */
    long long stat_sync_full;       /* 与从服务器完全同步的次数。 */
    long long stat_sync_partial_ok; /* 接受的 PSYNC 请求数量。 */
    long long stat_sync_partial_err;/* 未接受的 PSYNC 请求数量。 */
    list *slowlog;                  /* SLOWLOG 命令列表 */
    long long slowlog_entry_id;     /* SLOWLOG 当前条目 ID */
    long long slowlog_log_slower_than; /* SLOWLOG 时间限制 (用于记录) */
    unsigned long slowlog_max_len;     /* SLOWLOG 记录的最大条目数 */
    struct malloc_stats cron_malloc_stats; /* 在 serverCron() 中采样。 */
    std::atomic<long long> stat_net_input_bytes; /* 从网络读取的字节数。 */
    std::atomic<long long> stat_net_output_bytes; /* 写入网络的字节数。 */
    size_t stat_current_cow_bytes;  /* 子进程活动时写时复制 (Copy-On-Write) 的字节数。 */
    monotime stat_current_cow_updated;  /* stat_current_cow_bytes 的最后更新时间 */
    size_t stat_current_save_keys_processed;  /* 子进程活动时已处理的键数量。 */
    size_t stat_current_save_keys_total;  /* 子进程启动时的键数量。 */
    size_t stat_rdb_cow_bytes;      /* RDB 保存期间写时复制的字节数。 */
    size_t stat_aof_cow_bytes;      /* AOF 重写期间写时复制的字节数。 */
    size_t stat_module_cow_bytes;   /* 模块 fork 期间写时复制的字节数。 */
    double stat_module_progress;   /* 模块保存进度。 */
    uint64_t stat_clients_type_memory[CLIENT_TYPE_COUNT];/* 按类型划分的内存使用量 */
    long long stat_unexpected_error_replies; /* 意外错误回复的数量 (aof-loading, 从服务器到主服务器等) */
    long long stat_dump_payload_sanitizations; /* 深度转储有效负载完整性验证的次数。 */
    std::atomic<long long> stat_total_reads_processed; /* 已处理的读取事件总数 */
    std::atomic<long long> stat_total_writes_processed; /* 已处理的写入事件总数 */
    long long stat_storage_provider_read_hits;
    long long stat_storage_provider_read_misses;
    /* 以下两个用于跟踪瞬时指标，例如
     * 每秒操作数、网络流量。 */
    struct {
        long long last_sample_time; /* 上次采样的时间戳 (毫秒) */
        long long last_sample_count;/* 上次采样中的计数 */
        long long samples[STATS_METRIC_SAMPLES];
        int idx;
    } inst_metric[STATS_METRIC_COUNT];
    /* AOF 持久化 */
    int aof_enabled;                /* AOF 配置 */
    int aof_state;                  /* AOF_(ON|OFF|WAIT_REWRITE) (打开/关闭/等待重写) */
    int aof_fsync;                  /* fsync() 策略类型 */
    char *aof_filename;             /* AOF 文件名 */
    int aof_no_fsync_on_rewrite;    /* 如果重写正在进行，则不进行 fsync。 */
    int aof_rewrite_perc;           /* 如果增长百分比 > M 则重写 AOF，并且... */
    off_t aof_rewrite_min_size;     /* ...AOF 文件至少为 N 字节。 */
    off_t aof_rewrite_base_size;    /* 最近一次启动或重写时的 AOF 大小。 */
    off_t aof_current_size;         /* AOF 当前大小。 */
    off_t aof_fsync_offset;         /* 已同步到磁盘的 AOF 偏移量。 */
    int aof_flush_sleep;            /* flush 前休眠的微秒数 (用于测试)。 */
    int aof_rewrite_scheduled;      /* BGSAVE 终止后安排一次重写。 */
    list *aof_rewrite_buf_blocks;   /* 在 AOF 重写期间保存更改的缓冲区块。 */
    sds aof_buf;      /* AOF 缓冲区，在进入事件循环前写入 */
    int aof_fd;       /* 当前选定 AOF 文件的文件描述符 */
    int aof_selected_db; /* AOF 中当前选定的数据库 */
    time_t aof_flush_postponed_start; /* 延迟 AOF flush 的 UNIX 时间。 */
    time_t aof_last_fsync;            /* 上次 fsync() 的 UNIX 时间。 */
    time_t aof_rewrite_time_last;   /* 上次 AOF 重写运行所用时间。 */
    time_t aof_rewrite_time_start;  /* 当前 AOF 重写开始时间。 */
    int aof_lastbgrewrite_status;   /* C_OK 或 C_ERR */
    unsigned long aof_delayed_fsync;  /* 延迟的 AOF fsync() 计数器 */
    int aof_rewrite_incremental_fsync;/* AOF 重写时是否增量 fsync？ */
    int rdb_save_incremental_fsync;   /* RDB 保存时是否增量 fsync？ */
    int aof_last_write_status;      /* C_OK 或 C_ERR */
    int aof_last_write_errno;       /* 如果 aof 写入/fsync 状态为 ERR 则有效 */
    int aof_load_truncated;         /* 不因意外的 AOF EOF 而停止。 */
    int aof_use_rdb_preamble;       /* AOF 重写时使用 RDB 前导码。 */
    redisAtomic int aof_bio_fsync_status; /* bio 任务中 AOF fsync 的状态。 */
    redisAtomic int aof_bio_fsync_errno;  /* bio 任务中 AOF fsync 的错误号。 */
    /* AOF 管道，用于在重写期间父子进程通信。 */
    int aof_pipe_write_data_to_child;
    int aof_pipe_read_data_from_parent;
    int aof_pipe_write_ack_to_parent;
    int aof_pipe_read_ack_from_child;
    aeEventLoop *el_alf_pip_read_ack_from_child;
    int aof_pipe_write_ack_to_child;
    int aof_pipe_read_ack_from_parent;
    int aof_stop_sending_diff;     /* 如果为 true，则停止向子进程发送累积的 diff。
                                      */
    sds aof_child_diff;             /* AOF diff 累积器 (子进程侧)。 */
    int aof_rewrite_pending = 0;    /* aofChildWriteDiffData 的调用是否已排队？ */
    /* RDB 持久化 */
    int allowRdbResizeOp;           /* 调试场景下，我们可能希望 rehash 正在发生，因此忽略 resize */
    long long dirty;                /* 自上次保存以来数据库的更改数 */
    long long dirty_before_bgsave;  /* 用于在 BGSAVE 失败时恢复 dirty 值 */
    struct _rdbThreadVars
    {
        std::atomic<bool> fRdbThreadCancel {false};
        std::atomic<bool> fDone {false};
        int tmpfileNum = 0;
        pthread_t rdb_child_thread;
        int fRdbThreadActive = false;
    } rdbThreadVars;
    struct saveparam *saveparams;   /* RDB 的保存点数组 */
    int saveparamslen;              /* 保存点数量 */
    char *rdb_filename;             /* RDB 文件名 */
    char *rdb_s3bucketpath;         /* RDB 文件的 AWS S3 备份路径 */
    int rdb_compression;            /* 是否在 RDB 中使用压缩？ */
    int rdb_checksum;               /* 是否使用 RDB 校验和？ */
    int rdb_del_sync_files;         /* 如果实例不使用持久化，是否删除仅用于 SYNC 的 RDB 文件。
                                       */
    time_t lastsave;                /* 上次成功保存的 Unix 时间 */
    time_t lastbgsave_try;          /* 上次尝试 bgsave 的 Unix 时间 */
    time_t rdb_save_time_last;      /* 上次 RDB 保存运行所用时间。 */
    time_t rdb_save_time_start;     /* 当前 RDB 保存开始时间。 */
    mstime_t rdb_save_latency;      /* 用于跟踪 rdb 保存的端到端延迟 */
    pid_t rdb_child_pid = -1;       /* 仅在 fork bgsave 期间使用 */
    int rdb_bgsave_scheduled;       /* 如果为 true，则尽可能进行 BGSAVE。 */
    int rdb_child_type;             /* 活动子进程的保存类型。 */
    int lastbgsave_status;          /* C_OK 或 C_ERR */
    int stop_writes_on_bgsave_err;  /* 如果无法 BGSAVE，则禁止写入 */
    int rdb_pipe_read;              /* RDB 管道，用于在无盘复制中将 rdb 数据传输
                                    到父进程。 */
    int rdb_child_exit_pipe;        /* 由无盘父进程使用，以允许子进程退出。 */
    connection **rdb_pipe_conns;    /* 当前作为无盘 rdb fork 子进程目标的连接。 */
    int rdb_pipe_numconns;          /* */
    int rdb_pipe_numconns_writing;  /* 具有待写入的 rdb 连接数。 */
    char *rdb_pipe_buff;            /* 在无盘复制中，此缓冲区保存从 rdb 管道读取的数据。
                                     */
    int rdb_pipe_bufflen;           /* */
    int rdb_key_save_delay;         /* 写入 RDB 时键之间的延迟 (微秒)。(用于测试)。
                                     * 负值表示微秒的分数 (平均而言)。 */
    int key_load_delay;             /* 加载 aof 或 rdb 时键之间的延迟 (微秒)。(用于测试)。
                                     * 负值表示微秒的分数 (平均而言)。 */
    /* 用于子父进程信息共享的管道和数据结构。 */
    int child_info_pipe[2];         /* 用于写入 child_info_data 的管道。 */
    int child_info_nread;           /* 上次从管道读取的字节数 */
    /* AOF / 复制中的命令传播 */
    redisOpArray also_propagate;    /* 需要额外传播的命令。 */
    int replication_allowed;        /* 我们是否允许复制？ */
    /* 日志记录 */
    char *logfile;                  /* 日志文件路径 */
    int syslog_enabled;             /* 是否启用 syslog？ */
    char *syslog_ident;             /* Syslog 标识符 */
    int syslog_facility;            /* Syslog facility */
    int crashlog_enabled;           /* 为崩溃日志启用信号处理器。
                                     * 禁用以获取干净的核心转储。 */
    int memcheck_enabled;           /* 崩溃时启用内存检查。 */
    int use_exit_on_panic;          /* 在 panic 和 assert 时使用 exit() 而不是 abort()。
                                     * 对 Valgrind 有用。 */
    /* 复制 (主节点) */
    char replid[CONFIG_RUN_ID_SIZE+1];  /* 我的当前复制 ID。 */
    char replid2[CONFIG_RUN_ID_SIZE+1]; /* 从主节点继承的 replid */
    long long master_repl_offset;   /* 我的当前复制偏移量 */
    long long second_replid_offset; /* 为 replid2 接受此值之前的偏移量。 */
    int replicaseldb;                 /* 复制输出中上次选择的数据库 */
    int repl_ping_slave_period;     /* 主节点 ping 从节点的周期 (秒) */
    char *repl_backlog;             /* 部分同步的复制积压缓冲区 */
    char *repl_backlog_disk = nullptr;
    long long repl_backlog_size;    /* 积压循环缓冲区大小 */
    long long repl_backlog_config_size; /* 复制积压可能会增长，但我们想知道用户设置的大小 */
    long long repl_backlog_histlen; /* 积压实际数据长度 */
    long long repl_backlog_idx;     /* 积压循环缓冲区的当前偏移量，
                                       即我们将要写入的下一个字节的位置。*/
    long long repl_backlog_off;     /* 复制积压缓冲区中第一个字节的复制“主偏移量”。
                                       */
    long long repl_backlog_start;   /* 用于从偏移量计算索引，
                                       基本上，index = (offset - start) % size */
    fastlock repl_backlog_lock {"replication backlog"};
    time_t repl_backlog_time_limit; /* 在没有从节点的情况下，积压缓冲区释放前的时间限制。
                                       */
    time_t repl_no_slaves_since;    /* 从此时间开始没有从节点。
                                       仅当 g_pserver->slaves 长度为 0 时有效。 */
    int repl_min_slaves_to_write;   /* 写入操作所需的最小从节点数。 */
    int repl_min_slaves_max_lag;    /* <count> 个从节点可接受的最大延迟。 */
    int repl_good_slaves_count;     /* 延迟 <= max_lag 的从节点数量。 */
    int repl_diskless_sync;         /* 主节点直接将 RDB 发送到从节点套接字。 */
    int repl_diskless_load;         /* 从节点直接从套接字解析 RDB。
                                     * 参见 REPL_DISKLESS_LOAD_* 枚举 */
    int repl_diskless_sync_delay;   /* 启动无盘复制 BGSAVE 的延迟。 */
    std::atomic <long long> repl_lowest_off; /* 所有副本中的最低偏移量
                                                如果没有副本，则为 -1 */
    /* 复制 (从节点) */
    list *masters;
    int enable_multimaster;
    int repl_timeout;               /* 主节点空闲 N 秒后超时 */
    int repl_syncio_timeout; /* 同步 I/O 调用的超时时间 */
    int repl_disable_tcp_nodelay;   /* SYNC 后禁用 TCP_NODELAY？ */
    int repl_serve_stale_data; /* 链路断开时是否提供旧数据？ */
    int repl_quorum;           /* 对于多主复制，我们认为什么是法定数量？-1 表示所有主节点都必须在线 */
    int repl_slave_ro;          /* 从节点是否只读？ */
    int repl_slave_ignore_maxmemory;    /* 如果为 true，从节点不进行数据淘汰。 */
    int slave_priority;             /* 在 INFO 中报告并由 Sentinel 使用。 */
    int replica_announced;          /* 如果为 true，则副本由 Sentinel 宣布 */
    int slave_announce_port;        /* 将此监听端口告知主节点。 */
    char *slave_announce_ip;        /* 将此 IP 地址告知主节点。 */
    int repl_slave_lazy_flush;          /* 加载数据库前是否延迟 FLUSHALL？ */
    /* 复制脚本缓存。 */
    ::dict *repl_scriptcache_dict;        /* 所有从节点都知道的 SHA1。 */
    list *repl_scriptcache_fifo;        /* 先进先出 LRU 逐出队列。 */
    unsigned int repl_scriptcache_size; /* 最大元素数量。 */
    /* 同步复制。 */
    list *clients_waiting_acks;         /* 在 WAIT 命令中等待的客户端。 */
    int get_ack_from_slaves;            /* 如果为 true，我们发送 REPLCONF GETACK。 */
    /* 限制 */
    unsigned int maxclients;            /* 最大并发客户端数 */
    unsigned int maxclientsReserved;    /* 为健康检查保留的数量 (本地连接) */
    unsigned long long maxmemory;   /* 最大使用内存字节数 */
    unsigned long long maxstorage;  /* 存储提供程序中最大使用的字节数 */
    int maxmemory_policy;           /* 键逐出策略 */
    int maxmemory_samples;          /* 随机采样的精度 */
    int maxmemory_eviction_tenacity;/* 逐出处理的强度 */
    int force_eviction_percent;     /* 当系统内存剩余此百分比时强制逐出 */
    int lfu_log_factor;             /* LFU 对数计数器因子。 */
    int lfu_decay_time;             /* LFU 计数器衰减因子。 */
    long long proto_max_bulk_len;   /* 协议批量长度最大值。 */
    int oom_score_adj_base;         /* 启动时观察到的 oom_score_adj 基准值 */
    int oom_score_adj_values[CONFIG_OOM_COUNT];   /* Linux oom_score_adj 配置 */
    int oom_score_adj;                            /* 如果为 true，则管理 oom_score_adj */
    int disable_thp;                              /* 如果为 true，则通过 syscall 禁用 THP */
    /* 阻塞的客户端 */
    unsigned int blocked_clients;   /* 执行阻塞命令的客户端数量。*/
    unsigned int blocked_clients_by_type[BLOCKED_NUM];
    list *ready_keys;        /* BLPOP 等命令的 readyList 结构列表 */
    /* 客户端缓存。 */
    unsigned int tracking_clients;  /* 启用了跟踪的客户端数量。*/
    size_t tracking_table_max_keys; /* 跟踪表中的最大键数。 */
    /* 排序参数 - qsort_r() 仅在 BSD 下可用，因此我们
     * 必须将此状态设为全局，以便传递给 sortCompare() */
    int sort_desc;
    int sort_alpha;
    int sort_bypattern;
    int sort_store;
    /* Zip 结构配置，详见 keydb.conf  */
    size_t hash_max_ziplist_entries;
    size_t hash_max_ziplist_value;
    size_t set_max_intset_entries;
    size_t zset_max_ziplist_entries;
    size_t zset_max_ziplist_value;
    size_t hll_sparse_max_bytes;
    size_t stream_node_max_bytes;
    long long stream_node_max_entries;
    /* 列表参数 */
    int list_max_ziplist_size;
    int list_compress_depth;
    /* 时间缓存 */
    std::atomic<time_t> unixtime;    /* 每个 cron 周期采样的 Unix 时间。 */
    time_t timezone;            /* 缓存的时区。由 tzset() 设置。 */
    int daylight_active;        /* 当前是否为夏令时。 */
    mstime_t mstime;            /* 'unixtime' (毫秒)。 */
    ustime_t ustime;            /* 'unixtime' (微秒)。 */
    size_t blocking_op_nesting; /* 阻塞操作的嵌套级别，用于重置 blocked_last_cron。 */
    long long blocked_last_cron; /* 指示上次从阻塞操作执行 cron 作业的 mstime */
    /* 发布/订阅 */
    dict *pubsub_channels;  /* 将频道映射到已订阅客户端列表 */
    dict *pubsub_patterns;  /* pubsub_patterns 字典 */
    int notify_keyspace_events; /* 通过 Pub/Sub 传播的事件。这是
                                   NOTIFY_... 标志的异或值。 */
    /* 集群 */
    int cluster_enabled;      /* 集群是否已启用？ */
    mstime_t cluster_node_timeout; /* 集群节点超时时间。 */
    char *cluster_configfile; /* 集群自动生成的配置文件名。 */
    struct clusterState *cluster;  /* 集群状态 */
    int cluster_migration_barrier; /* 集群副本迁移屏障。 */
    int cluster_allow_replica_migration; /* 自动将副本迁移到孤立主节点和从空主节点迁出 */
    int cluster_slave_validity_factor; /* 故障转移的从节点最大数据年龄。 */
    int cluster_require_full_coverage; /* 如果为 true，则在至少有一个未覆盖的槽时
                                          使集群下线。*/
    int cluster_slave_no_failover;  /* 防止副本在主节点处于故障状态时
                                       启动故障转移。 */
    char *cluster_announce_ip;  /* 在集群总线上宣布的 IP 地址。 */
    int cluster_announce_port;     /* 在集群总线上宣布的基础端口。 */
    int cluster_announce_tls_port; /* 在集群总线上宣布的 TLS 端口。 */
    int cluster_announce_bus_port; /* 在集群总线上宣布的总线端口。 */
    int cluster_module_flags;      /* Redis 模块能够设置的一组标志，
                                      用于抑制某些本机 Redis 集群功能。
                                      检查 REDISMODULE_CLUSTER_FLAG_*. */
    int cluster_allow_reads_when_down; /* 集群下线时是否允许读取？
                                        */
    int cluster_config_file_lock_fd;   /* 集群配置文件锁 fd，将被 flock */
    /* 脚本 */
    lua_State *lua; /* Lua 解释器。我们为所有客户端仅使用一个 */
    client *lua_caller = nullptr;   /* 当前正在运行 EVAL 的客户端，或为 NULL */
    char* lua_cur_script = nullptr; /* 当前正在运行脚本的 SHA1，或为 NULL */
    ::dict *lua_scripts;         /* SHA1 -> Lua 脚本的字典 */
    unsigned long long lua_scripts_mem;  /* 缓存脚本的内存 + 开销 */
    mstime_t lua_time_limit;  /* 脚本超时时间 (毫秒) */
    monotime lua_time_start;  /* 用于检测脚本超时的单调计时器 */
    mstime_t lua_time_snapshot; /* 脚本启动时的 mstime 快照 */
    int lua_write_dirty;  /* 如果在当前脚本执行期间调用了写命令，则为 True。
                             */
    int lua_random_dirty; /* 如果在当前脚本执行期间调用了随机命令，则为 True。
                             */
    int lua_replicate_commands; /* 如果我们正在执行单个命令复制，则为 True。 */
    int lua_multi_emitted;/* 如果我们已经传播了 MULTI，则为 True。 */
    int lua_repl;         /* 用于 redis.set_repl() 的脚本复制标志。 */
    int lua_timedout;     /* 如果我们达到了脚本执行的时间限制，则为 True。
                             */
    int lua_kill;         /* 如果为 true，则终止脚本。 */
    int lua_always_replicate_commands; /* 默认复制类型。 */
    int lua_oom;          /* 脚本启动时检测到 OOM？ */
    /* 惰性释放 */
    int lazyfree_lazy_eviction;
    int lazyfree_lazy_expire;
    int lazyfree_lazy_server_del;
    int lazyfree_lazy_user_del;
    int lazyfree_lazy_user_flush;
    /* 延迟监视器 */
    long long latency_monitor_threshold;
    ::dict *latency_events;
    /* ACL */
    char *acl_filename;           /* ACL 用户文件。如果未配置，则为 NULL。 */
    unsigned long acllog_max_len; /* ACL LOG 列表的最大长度。 */
    sds requirepass;              /* 记住通过旧的 "requirepass" 指令设置的明文密码，
                                     以便与 Redis <= 5 向后兼容。 */
    int acl_pubsub_default;      /* 默认 ACL 发布/订阅通道标志 */
    /* 断言和错误报告 */
    int watchdog_period;  /* 软件看门狗周期 (毫秒)。0 = 关闭 */

    int fActiveReplica;                          /* 此副本是否也可以是主副本？ */
    int fWriteDuringActiveLoad;                  /* 此活动副本是否可以在 RDB 加载期间写入？ */
    int fEnableFastSync = false;

    // 格式说明：
    // 低20位：在同一毫秒内执行的命令计数器（每执行一条指令递增）
    // 高44位：毫秒时间戳（取最低44位有效位），自添加之日起约500年内不会发生回绕
    uint64_t mvcc_tstamp;

    AsyncWorkQueue *asyncworkqueue;
    /* 系统硬件信息 */
    size_t system_memory_size;  /* 操作系统报告的系统总内存大小 */

    GarbageCollectorCollection garbageCollector;  // 垃圾回收器集合

    IStorageFactory *m_pstorageFactory = nullptr; // 存储工厂接口指针
    int storage_flush_period;   // CRON作业中执行存储刷新的时间间隔

    long long snapshot_slip = 500;   // 允许快照落后当前数据库的时间量（毫秒）

    /* TLS 配置参数 */
    int tls_cluster;            // 集群通信TLS启用标志
    int tls_replication;        // 复制连接TLS启用标志
    int tls_auth_clients;       // 客户端认证TLS启用标志
    int tls_rotation;           // TLS证书轮换策略

    std::set<sdsstring> tls_auditlog_blocklist; /* 可从审计日志中排除的证书列表 */
    std::set<sdsstring> tls_allowlist;          // TLS允许列表
    redisTLSContextConfig tls_ctx_config;       // TLS上下文配置

    /* CPU 亲和性设置 */
    char *server_cpulist; /* Redis服务器主线程/I/O线程的CPU亲和性列表 */
    char *bio_cpulist;    /* 后台I/O(bio)线程的CPU亲和性列表 */
    char *aof_rewrite_cpulist; /* AOF重写进程的CPU亲和性列表 */
    char *bgsave_cpulist;      /* 后台保存(bgsave)进程的CPU亲和性列表 */

    int prefetch_enabled = 1;    // 预取功能启用标志
    /* Sentinel (哨兵) 配置 */
    struct sentinelConfig *sentinel_config; /* 启动时加载的哨兵配置 */
    /* 故障转移协调信息 */
    mstime_t failover_end_time; /* 故障转移命令执行的截止时间 */
    int force_failover; /* 如果为true，则在截止时间强制执行故障转移；
                         * 否则将中止故障转移 */
    char *target_replica_host; /* 故障转移目标主机。在故障转移过程中如果为null，
                                * 则可以使用任意副本 */
    int target_replica_port;   /* 故障转移目标端口 */
    int failover_state;        /* 当前故障转移状态 */
    int enable_async_commands;
    int multithread_load_enabled = 0;
    int active_client_balancing = 1;

    long long repl_batch_offStart = -1;
    long long repl_batch_idxStart = -1;

    long long rand_total_threshold;

    int config_soft_shutdown = false;
    bool soft_shutdown = false;

    int flash_disable_key_cache = false;

    /* Lock Contention Ring Buffer */
    static const size_t s_lockContentionSamples = 64;
    uint16_t rglockSamples[s_lockContentionSamples];
    unsigned ilockRingHead = 0;


    sds sdsAvailabilityZone;
    int overload_protect_threshold = 0;
    int is_overloaded = 0;
    int overload_closed_clients = 0;

        int module_blocked_pipe[2]; /* Pipe used to awake the event loop if a
                            client blocked on a module command needs
                            to be processed. */

    bool FRdbSaveInProgress() const { return g_pserver->rdbThreadVars.fRdbThreadActive; }
};

inline int redisServerThreadVars::getRdbKeySaveDelay() {
    if (rdb_key_save_delay < 0) {
        __atomic_load(&g_pserver->rdb_key_save_delay, &rdb_key_save_delay, __ATOMIC_ACQUIRE);
    }
    return rdb_key_save_delay;
}


#define MAX_KEYS_BUFFER 256

/*
 * 用于多个getkeys函数调用的返回结果结构体。
 * 该结构体将键（keys）表示为所提供argv参数的索引列表。
 */
typedef struct {
    int keysbuf[MAX_KEYS_BUFFER];       /* 预分配缓冲区，用于避免堆内存分配 */
    int *keys;                          /* 键索引数组，指向keysbuf或堆内存 */
    int numkeys;                        /* 返回的键索引数量 */
    int size;                           /* 当前可用数组容量 */
} getKeysResult;
#define GETKEYS_RESULT_INIT { {0}, NULL, 0, MAX_KEYS_BUFFER }  // 结构体初始化宏

typedef void redisCommandProc(client *c);
typedef int redisGetKeysProc(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
struct redisCommand {
    const char *name;
    redisCommandProc *proc;
    int arity;
    const char *sflags;   /* 标志的字符串表示形式，每个标志一个字符。 */
    uint64_t flags; /* 从 'sflags' 字段获取的实际标志。 */
    /* 使用一个函数来确定命令行中的键参数。
     * 用于 Redis 集群重定向。 */
    redisGetKeysProc *getkeys_proc;
    /* 调用此命令时，应在后台加载哪些键？ */
    int firstkey; /* 第一个作为键的参数（0 = 没有键） */
    int lastkey;  /* 最后一个作为键的参数 */
    int keystep;  /* 第一个键和最后一个键之间的步长 */
    long long microseconds, calls, rejected_calls, failed_calls;
    int id;     /* 命令 ID。这是一个从 0 开始的渐进式 ID，在运行时分配，
                   用于检查 ACL。如果与连接关联的用户在允许的命令位图中
                   设置了此命令位，则连接能够执行给定的命令。 */
};

struct redisError {
    long long count;
};

struct redisFunctionSym {
    char *name;
    unsigned long pointer;
};

typedef struct _redisSortObject {
    robj *obj;
    union {
        double score;
        robj *cmpobj;
    } u;
} redisSortObject;

typedef struct _redisSortOperation {
    int type;
    robj *pattern;
} redisSortOperation;

/* 用于列表迭代的抽象结构 */
typedef struct {
    robj_roptr subject;
    unsigned char encoding;
    unsigned char direction; /* 迭代方向 */
    quicklistIter *iter;
} listTypeIterator;

/* 用于在列表迭代过程中表示单个条目的结构 */
typedef struct {
    listTypeIterator *li;
    quicklistEntry entry; /* quicklist中的条目 */
} listTypeEntry;

/* 用于集合(set)迭代的抽象结构 */
typedef struct {
    robj_roptr subject;
    int encoding;
    int ii; /* intset迭代器 */
    dictIterator *di;
} setTypeIterator;

/* 用于哈希(hash)迭代的抽象结构。注意哈希迭代
 * 涉及字段(field)和值(value)。由于可能不需要同时获取两者，
 * 迭代器中存储指针以避免不必要的字段/值内存分配 */
typedef struct {
    robj_roptr subject;
    int encoding;

    unsigned char *fptr, *vptr;

    dictIterator *di;
    dictEntry *de;
} hashTypeIterator;

#include "stream.h"  /* 流数据类型的头文件 */

#define OBJ_HASH_KEY 1
#define OBJ_HASH_VALUE 2

/* 在evict.cpp中使用 */
enum class EvictReason {
    User,       /* 用户内存超过限制 */
    System      /* 系统内存超过限制 */
};

/*-----------------------------------------------------------------------------
 * Extern declarations
 *----------------------------------------------------------------------------*/

//extern struct redisServer server;
extern struct redisServerConst cserver;
extern thread_local struct redisServerThreadVars *serverTL;   // thread local server vars
extern struct sharedObjectsStruct shared;
extern dictType objectKeyPointerValueDictType;
extern dictType objectKeyHeapPointerValueDictType;
extern dictType setDictType;
extern dictType zsetDictType;
extern dictType clusterNodesDictType;
extern dictType clusterNodesBlackListDictType;
extern dictType dbDictType;
extern dictType dbTombstoneDictType;
extern dictType dbSnapshotDictType;
extern dictType shaScriptObjectDictType;
extern double R_Zero, R_PosInf, R_NegInf, R_Nan;
extern dictType hashDictType;
extern dictType keylistDictType;
extern dictType replScriptCacheDictType;
extern dictType dbExpiresDictType;
extern dictType modulesDictType;
extern dictType sdsReplyDictType;
extern fastlock g_lockasyncfree;

/*-----------------------------------------------------------------------------
 * Functions prototypes
 *----------------------------------------------------------------------------*/

/* Modules */
void moduleInitModulesSystem(void);
void moduleInitModulesSystemLast(void);
int moduleLoad(const char *path, void **argv, int argc);
void moduleLoadFromQueue(void);
int moduleGetCommandKeysViaAPI(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
moduleType *moduleTypeLookupModuleByID(uint64_t id);
void moduleTypeNameByID(char *name, uint64_t moduleid);
const char *moduleTypeModuleName(moduleType *mt);
void moduleFreeContext(struct RedisModuleCtx *ctx, bool propogate = true);
void unblockClientFromModule(client *c);
void moduleHandleBlockedClients(int iel);
void moduleBlockedClientTimedOut(client *c);
void moduleBlockedClientPipeReadable(aeEventLoop *el, int fd, void *privdata, int mask);
size_t moduleCount(void);
void moduleAcquireGIL(int fServerThread, int fExclusive = FALSE);
int moduleTryAcquireGIL(bool fServerThread, int fExclusive = FALSE);
void moduleReleaseGIL(int fServerThread, int fExclusive = FALSE);
void moduleNotifyKeyspaceEvent(int type, const char *event, robj *key, int dbid);
void moduleCallCommandFilters(client *c);
int moduleHasCommandFilters();
void ModuleForkDoneHandler(int exitcode, int bysignal);
int TerminateModuleForkChild(int child_pid, int wait);
ssize_t rdbSaveModulesAux(rio *rdb, int when);
int moduleAllDatatypesHandleErrors();
sds modulesCollectInfo(sds info, const char *section, int for_crash_report, int sections);
void moduleFireServerEvent(uint64_t eid, int subid, void *data);
void processModuleLoadingProgressEvent(int is_aof);
int moduleTryServeClientBlockedOnKey(client *c, robj *key);
void moduleUnblockClient(client *c);
int moduleBlockedClientMayTimeout(client *c);
int moduleClientIsBlockedOnKeys(client *c);
void moduleNotifyUserChanged(client *c);
void moduleNotifyKeyUnlink(robj *key, robj *val);
robj *moduleTypeDupOrReply(client *c, robj *fromkey, robj *tokey, robj *value);
int moduleDefragValue(robj *key, robj *obj, long *defragged);
int moduleLateDefrag(robj *key, robj *value, unsigned long *cursor, long long endtime, long long *defragged);
long moduleDefragGlobals(void);

/* Utils */
long long ustime(void);
long long mstime(void);
extern "C" void getRandomHexChars(char *p, size_t len);
extern "C" void getRandomBytes(unsigned char *p, size_t len);
uint64_t crc64(uint64_t crc, const unsigned char *s, uint64_t l);
void exitFromChild(int retcode);
long long redisPopcount(const void *s, long count);
int redisSetProcTitle(const char *title);
int validateProcTitleTemplate(const char *_template);
int redisCommunicateSystemd(const char *sd_notify_msg);
void redisSetCpuAffinity(const char *cpulist);
void saveMasterStatusToStorage(bool fShutdown);

/* networking.c -- Networking and Client related operations */
client *createClient(connection *conn, int iel);
void closeTimedoutClients(void);
bool freeClient(client *c);
void freeClientAsync(client *c);
void resetClient(client *c);
void freeClientOriginalArgv(client *c);
void sendReplyToClient(connection *conn);
void *addReplyDeferredLen(client *c);
void setDeferredArrayLen(client *c, void *node, long length);
void setDeferredMapLen(client *c, void *node, long length);
void setDeferredSetLen(client *c, void *node, long length);
void setDeferredAttributeLen(client *c, void *node, long length);
void setDeferredPushLen(client *c, void *node, long length);
void processInputBuffer(client *c, bool fParse, int callFlags);
void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptTLSHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void readQueryFromClient(connection *conn);
void addReplyNull(client *c);
void addReplyNullArray(client *c);
void addReplyBool(client *c, int b);
void addReplyVerbatim(client *c, const char *s, size_t len, const char *ext);
void addReplyProto(client *c, const char *s, size_t len);
void addReplyProtoCString(client *c, const char *s);
void addReplyBulk(client *c, robj_roptr obj);
void AddReplyFromClient(client *c, client *src);
void addReplyBulkCString(client *c, const char *s);
void addReplyBulkCBuffer(client *c, const void *p, size_t len);
void addReplyBulkLongLong(client *c, long long ll);
void addReply(client *c, robj_roptr obj);
void addReplySds(client *c, sds s);
void addReplyBulkSds(client *c, sds s);
void setDeferredReplyBulkSds(client *c, void *node, sds s);
void addReplyErrorObject(client *c, robj *err, int severity = 0);
void addReplyErrorSds(client *c, sds err);
void addReplyError(client *c, const char *err);
void addReplyStatus(client *c, const char *status);
void addReplyDouble(client *c, double d);
void addReplyBigNum(client *c, const char* num, size_t len);
void addReplyHumanLongDouble(client *c, long double d);
void addReplyLongLong(client *c, long long ll);
#ifdef __cplusplus
void addReplyLongLongWithPrefix(client *c, long long ll, char prefix);
#endif
void addReplyArrayLen(client *c, long length);
void addReplyMapLen(client *c, long length);
void addReplySetLen(client *c, long length);
void addReplyAttributeLen(client *c, long length);
void addReplyPushLen(client *c, long length);
void addReplyHelp(client *c, const char **help);
void addReplySubcommandSyntaxError(client *c);
void addReplyLoadedModules(client *c);
void copyClientOutputBuffer(client *dst, client *src);
size_t sdsZmallocSize(sds s);
size_t getStringObjectSdsUsedMemory(robj *o);
void freeClientReplyValue(const void *o);
void *dupClientReplyValue(void *o);
void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer);
char *getClientPeerId(client *client);
char *getClientSockName(client *client);
sds catClientInfoString(sds s, client *client);
sds getAllClientsInfoString(int type);
void rewriteClientCommandVector(client *c, int argc, ...);
void rewriteClientCommandArgument(client *c, int i, robj *newval);
void replaceClientCommandVector(client *c, int argc, robj **argv);
void redactClientCommandArgument(client *c, int argc);
unsigned long getClientOutputBufferMemoryUsage(client *c);
int freeClientsInAsyncFreeQueue(int iel);
int closeClientOnOutputBufferLimitReached(client *c, int async);
int getClientType(client *c);
int getClientTypeByName(const char *name);
const char *getClientTypeName(int cclass);
void flushSlavesOutputBuffers(void);
void disconnectSlaves(void);
void disconnectSlavesExcept(unsigned char *uuid);
int listenToPort(int port, socketFds *fds, int fReusePort, int fFirstListen);
void pauseClients(mstime_t duration, pause_type type);
void unpauseClients(void);
int areClientsPaused(void);
int checkClientPauseTimeoutAndReturnIfPaused(void);
void processEventsWhileBlocked(int iel);
void loadingCron(void);
void whileBlockedCron();
void blockingOperationStarts();
void blockingOperationEnds();
int handleClientsWithPendingWrites(int iel, int aof_state);
int clientHasPendingReplies(client *c);
void unlinkClient(client *c);
int writeToClient(client *c, int handler_installed);
void linkClient(client *c);
void protectClient(client *c);
void unprotectClient(client *c);

void ProcessPendingAsyncWrites(void);
client *lookupClientByID(uint64_t id);
int authRequired(client *c);

#ifdef __GNUC__
void addReplyErrorFormat(client *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void addReplyStatusFormat(client *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void addReplyErrorFormat(client *c, const char *fmt, ...);
void addReplyStatusFormat(client *c, const char *fmt, ...);
#endif

/* Client side caching (tracking mode) */
void enableTracking(client *c, uint64_t redirect_to, uint64_t options, robj **prefix, size_t numprefix);
void disableTracking(client *c);
void trackingRememberKeys(client *c);
void trackingInvalidateKey(client *c, robj *keyobj);
void trackingInvalidateKeysOnFlush(int async);
void freeTrackingRadixTree(rax *rt);
void freeTrackingRadixTreeAsync(rax *rt);
void trackingLimitUsedSlots(void);
uint64_t trackingGetTotalItems(void);
uint64_t trackingGetTotalKeys(void);
uint64_t trackingGetTotalPrefixes(void);
void trackingBroadcastInvalidationMessages(void);
int checkPrefixCollisionsOrReply(client *c, robj **prefix, size_t numprefix);

/* List data type */
void listTypeTryConversion(robj *subject, robj *value);
void listTypePush(robj *subject, robj *value, int where);
robj *listTypePop(robj *subject, int where);
unsigned long listTypeLength(robj_roptr subject);
listTypeIterator *listTypeInitIterator(robj_roptr subject, long index, unsigned char direction);
void listTypeReleaseIterator(listTypeIterator *li);
int listTypeNext(listTypeIterator *li, listTypeEntry *entry);
robj *listTypeGet(listTypeEntry *entry);
void listTypeInsert(listTypeEntry *entry, robj *value, int where);
int listTypeEqual(listTypeEntry *entry, robj *o);
void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry);
void listTypeConvert(robj *subject, int enc);
robj *listTypeDup(robj *o);
void unblockClientWaitingData(client *c);
void popGenericCommand(client *c, int where);
void listElementsRemoved(client *c, robj *key, int where, robj *o, long count);

/* MULTI/EXEC/WATCH... */
void unwatchAllKeys(client *c);
void initClientMultiState(client *c);
void freeClientMultiState(client *c);
void queueMultiCommand(client *c);
void touchWatchedKey(redisDb *db, robj *key);
int isWatchedKeyExpired(client *c);
void touchAllWatchedKeysInDb(redisDb *emptied, redisDb *replaced_with);
void updateDBWatchedKey(int dbid, client *c);
void discardTransaction(client *c);
void flagTransaction(client *c);
void execCommandAbort(client *c, sds error);
void execCommandPropagateMulti(int dbid);
void execCommandPropagateExec(int dbid);
void beforePropagateMulti();
void afterPropagateExec();

/* Redis object implementation */
void decrRefCount(robj_roptr o);
void decrRefCountVoid(const void *o);
void incrRefCount(robj_roptr o);
robj *makeObjectShared(robj *o);
robj *makeObjectShared(const char *rgch, size_t cch);
robj *resetRefCount(robj *obj);
void freeStringObject(robj *o);
void freeListObject(robj *o);
void freeSetObject(robj *o);
void freeZsetObject(robj *o);
void freeHashObject(robj *o);
robj *createObject(int type, void *ptr);
robj *createStringObject(const char *ptr, size_t len);
robj *createRawStringObject(const char *ptr, size_t len);
robj *createEmbeddedStringObject(const char *ptr, size_t len);
robj *tryCreateRawStringObject(const char *ptr, size_t len);
robj *tryCreateStringObject(const char *ptr, size_t len);
robj *dupStringObject(const robj *o);
int isSdsRepresentableAsLongLong(const char *s, long long *llval);
int isObjectRepresentableAsLongLong(robj *o, long long *llongval);
robj *tryObjectEncoding(robj *o);
robj *getDecodedObject(robj *o);
robj_roptr getDecodedObject(robj_roptr o);
size_t stringObjectLen(robj_roptr o);
robj *createStringObjectFromLongLong(long long value);
robj *createStringObjectFromLongLongForValue(long long value);
robj *createStringObjectFromLongDouble(long double value, int humanfriendly);
robj *createQuicklistObject(void);
robj *createZiplistObject(void);
robj *createSetObject(void);
robj *createIntsetObject(void);
robj *createHashObject(void);
robj *createZsetObject(void);
robj *createZsetZiplistObject(void);
robj *createStreamObject(void);
robj *createModuleObject(moduleType *mt, void *value);
int getLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg);
int getUnsignedLongLongFromObjectOrReply(client *c, robj *o, uint64_t *target, const char *msg);
int getPositiveLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg);
int getRangeLongFromObjectOrReply(client *c, robj *o, long min, long max, long *target, const char *msg);
int checkType(client *c, robj_roptr o, int type);
int getLongLongFromObjectOrReply(client *c, robj *o, long long *target, const char *msg);
int getDoubleFromObjectOrReply(client *c, robj *o, double *target, const char *msg);
int getDoubleFromObject(const robj *o, double *target);
int getLongLongFromObject(robj *o, long long *target);
int getUnsignedLongLongFromObject(robj *o, uint64_t *target);
int getLongDoubleFromObject(robj *o, long double *target);
int getLongDoubleFromObjectOrReply(client *c, robj *o, long double *target, const char *msg);
int getIntFromObjectOrReply(client *c, robj *o, int *target, const char *msg);
const char *strEncoding(int encoding);
int compareStringObjects(robj *a, robj *b);
int collateStringObjects(robj *a, robj *b);
int equalStringObjects(robj *a, robj *b);
unsigned long long estimateObjectIdleTime(robj_roptr o);
void trimStringObjectIfNeeded(robj *o);

robj *deserializeStoredObject(const void *data, size_t cb);
std::unique_ptr<expireEntry> deserializeExpire(const char *str, size_t cch, size_t *poffset);
sds serializeStoredObject(robj_roptr o, sds sdsPrefix = nullptr);
sds serializeStoredObjectAndExpire(robj_roptr o);

#define sdsEncodedObject(objptr) (objptr->encoding == OBJ_ENCODING_RAW || objptr->encoding == OBJ_ENCODING_EMBSTR)

/* Synchronous I/O with timeout */
ssize_t syncWrite(int fd, const char *ptr, ssize_t size, long long timeout);
ssize_t syncRead(int fd, char *ptr, ssize_t size, long long timeout);
ssize_t syncReadLine(int fd, char *ptr, ssize_t size, long long timeout);

/* Replication */
void initMasterInfo(struct redisMaster *master);
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc);
void replicationFeedSlavesFromMasterStream(char *buf, size_t buflen);
void replicationFeedMonitors(client *c, list *monitors, int dictid, robj **argv, int argc);
void updateSlavesWaitingBgsave(int bgsaveerr, int type);
void replicationCron(void);
void replicationStartPendingFork(void);
void replicationHandleMasterDisconnection(struct redisMaster *mi);
void replicationCacheMaster(struct redisMaster *mi, client *c);
void replicationCreateCachedMasterClone(redisMaster *mi);
void resizeReplicationBacklog(long long newsize);
struct redisMaster *replicationAddMaster(char *ip, int port);
void replicationUnsetMaster(struct redisMaster *mi);
void refreshGoodSlavesCount(void);
void replicationScriptCacheInit(void);
void replicationScriptCacheFlush(void);
void replicationScriptCacheAdd(sds sha1);
int replicationScriptCacheExists(sds sha1);
void processClientsWaitingReplicas(void);
void unblockClientWaitingReplicas(client *c);
int replicationCountAcksByOffset(long long offset);
void replicationSendNewlineToMaster(struct redisMaster *mi);
long long replicationGetSlaveOffset(struct redisMaster *mi);
char *replicationGetSlaveName(client *c);
long long getPsyncInitialOffset(void);
int replicationSetupSlaveForFullResync(client *replica, long long offset);
void changeReplicationId(void);
void clearReplicationId2(void);
void chopReplicationBacklog(void);
void replicationCacheMasterUsingMyself(struct redisMaster *mi);
void replicationCacheMasterUsingMaster(struct redisMaster *mi);
void feedReplicationBacklog(const void *ptr, size_t len);
void updateMasterAuth();
void showLatestBacklog();
void rdbPipeReadHandler(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
void rdbPipeWriteHandlerConnRemoved(struct connection *conn);
void replicationNotifyLoadedKey(redisDb *db, robj_roptr key, robj_roptr val, long long expire);
void replicateSubkeyExpire(redisDb *db, robj_roptr key, robj_roptr subkey, long long expire);
void clearFailoverState(void);
void updateFailoverStatus(void);
void abortFailover(redisMaster *mi, const char *err);
const char *getFailoverStateString();
int canFeedReplicaReplBuffer(client *replica);
void trimReplicationBacklog();

/* Generic persistence functions */
void startLoadingFile(FILE* fp, const char * filename, int rdbflags);
void startLoading(size_t size, int rdbflags);
void loadingProgress(off_t pos);
void stopLoading(int success);
void startSaving(int rdbflags);
void stopSaving(int success);
int allPersistenceDisabled(void);

#define DISK_ERROR_TYPE_AOF 1       /* Don't accept writes: AOF errors. */
#define DISK_ERROR_TYPE_RDB 2       /* Don't accept writes: RDB errors. */
#define DISK_ERROR_TYPE_NONE 0      /* No problems, we can accept writes. */
int writeCommandsDeniedByDiskError(void);

/* RDB persistence */
#include "rdb.h"
void killRDBChild(bool fSynchronous = false);
int bg_unlink(const char *filename);


/* AOF persistence */
void flushAppendOnlyFile(int force);
void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc);
void aofRemoveTempFile(pid_t childpid);
int rewriteAppendOnlyFileBackground(void);
int loadAppendOnlyFile(char *filename);
void stopAppendOnly(void);
int startAppendOnly(void);
void backgroundRewriteDoneHandler(int exitcode, int bysignal);
void aofRewriteBufferReset(void);
unsigned long aofRewriteBufferSize(void);
ssize_t aofReadDiffFromParent(void);
void killAppendOnlyChild(void);
void restartAOFAfterSYNC();

/* Child info */
void openChildInfoPipe(void);
void closeChildInfoPipe(void);
void sendChildInfoGeneric(childInfoType info_type, size_t keys, double progress, const char *pname);
void sendChildCowInfo(childInfoType info_type, const char *pname);
void sendChildInfo(childInfoType info_type, size_t keys, const char *pname);
void receiveChildInfo(void);

/* Fork helpers */
void executeWithoutGlobalLock(std::function<void()> func);
int redisFork(int type);
int hasActiveChildProcess();
int hasActiveChildProcessOrBGSave();
void resetChildState();
int isMutuallyExclusiveChildType(int type);

/* acl.c -- Authentication related prototypes. */
extern rax *Users;
extern user *DefaultUser;
void ACLInit(void);
/* Return values for ACLCheckAllPerm(). */
#define ACL_OK 0
#define ACL_DENIED_CMD 1
#define ACL_DENIED_KEY 2
#define ACL_DENIED_AUTH 3 /* Only used for ACL LOG entries. */
#define ACL_DENIED_CHANNEL 4 /* Only used for pub/sub commands */
int ACLCheckUserCredentials(robj *username, robj *password);
int ACLAuthenticateUser(client *c, robj *username, robj *password);
unsigned long ACLGetCommandID(const char *cmdname);
void ACLClearCommandID(void);
user *ACLGetUserByName(const char *name, size_t namelen);
int ACLCheckAllPerm(client *c, int *idxptr);
int ACLSetUser(user *u, const char *op, ssize_t oplen);
sds ACLDefaultUserFirstPassword(void);
uint64_t ACLGetCommandCategoryFlagByName(const char *name);
int ACLAppendUserForLoading(sds *argv, int argc, int *argc_err);
const char *ACLSetUserStringError(void);
int ACLLoadConfiguredUsers(void);
sds ACLDescribeUser(user *u);
void ACLLoadUsersAtStartup(void);
void addReplyCommandCategories(client *c, struct redisCommand *cmd);
user *ACLCreateUnlinkedUser();
void ACLFreeUserAndKillClients(user *u);
void addACLLogEntry(client *c, int reason, int keypos, sds username);
void ACLUpdateDefaultUserPassword(sds password);

/* 有序集合数据类型 */

/* 输入标志。 */
#define ZADD_IN_NONE 0
#define ZADD_IN_INCR (1<<0)    /* 增加分数而不是设置分数。 */
#define ZADD_IN_NX (1<<1)      /* 不要接触不存在的元素。 */
#define ZADD_IN_XX (1<<2)      /* 只接触已经存在的元素。 */
#define ZADD_IN_GT (1<<3)      /* 仅当新分数更高时才更新现有分数。 */
#define ZADD_IN_LT (1<<4)      /* 仅当新分数更低时才更新现有分数。 */

/* 输出标志。 */
#define ZADD_OUT_NOP (1<<0)     /* 由于条件限制，操作未执行。*/
#define ZADD_OUT_NAN (1<<1)     /* 只接触已经存在的元素。 */
#define ZADD_OUT_ADDED (1<<2)   /* 元素是新的并且已被添加。 */
#define ZADD_OUT_UPDATED (1<<3) /* 元素已存在，分数已更新。 */

/* 用于按分数比较来保存包含/排除范围规范的结构体。 */
typedef struct {
    double min, max;
    int minex, maxex; /* min 或 max 是否为排除的？ */
} zrangespec;

/* 用于按字典顺序比较来保存包含/排除范围规范的结构体。 */
typedef struct {
    sds min, max;     /* 可以设置为 shared.(minstring|maxstring) */
    int minex, maxex; /* min 或 max 是否为排除的？ */
} zlexrangespec;

zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
zskiplistNode *zslInsert(zskiplist *zsl, double score, sds ele);
unsigned char *zzlInsert(unsigned char *zl, sds ele, double score);
int zslDelete(zskiplist *zsl, double score, sds ele, zskiplistNode **node);
zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range);
zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range);
double zzlGetScore(unsigned char *sptr);
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range);
unsigned char *zzlLastInRange(unsigned char *zl, zrangespec *range);
unsigned long zsetLength(robj_roptr zobj);
void zsetConvert(robj *zobj, int encoding);
void zsetConvertToZiplistIfNeeded(robj *zobj, size_t maxelelen, size_t totelelen);
int zsetScore(robj_roptr zobj, sds member, double *score);
unsigned long zslGetRank(zskiplist *zsl, double score, sds o);
int zsetAdd(robj *zobj, double score, sds ele, int in_flags, int *out_flags, double *newscore);
long zsetRank(robj_roptr zobj, sds ele, int reverse);
int zsetDel(robj *zobj, sds ele);
robj *zsetDup(robj *o);
int zsetZiplistValidateIntegrity(unsigned char *zl, size_t size, int deep);
void genericZpopCommand(client *c, robj **keyv, int keyc, int where, int emitkey, robj *countarg);
sds ziplistGetObject(unsigned char *sptr);
int zslValueGteMin(double value, zrangespec *spec);
int zslValueLteMax(double value, zrangespec *spec);
void zslFreeLexRange(zlexrangespec *spec);
int zslParseLexRange(robj *min, robj *max, zlexrangespec *spec);
unsigned char *zzlFirstInLexRange(unsigned char *zl, zlexrangespec *range);
unsigned char *zzlLastInLexRange(unsigned char *zl, zlexrangespec *range);
zskiplistNode *zslFirstInLexRange(zskiplist *zsl, zlexrangespec *range);
zskiplistNode *zslLastInLexRange(zskiplist *zsl, zlexrangespec *range);
int zzlLexValueGteMin(unsigned char *p, zlexrangespec *spec);
int zzlLexValueLteMax(unsigned char *p, zlexrangespec *spec);
int zslLexValueGteMin(sds value, zlexrangespec *spec);
int zslLexValueLteMax(sds value, zlexrangespec *spec);

/* Core functions */
int getMaxmemoryState(size_t *total, size_t *logical, size_t *tofree, float *level, EvictReason *reason=nullptr, bool fQuickCycle=false, bool fPreSnapshot=false);
size_t freeMemoryGetNotCountedMemory();
int overMaxmemoryAfterAlloc(size_t moremem);
int processCommand(client *c, int callFlags);
int processPendingCommandsAndResetClient(client *c, int flags);
void setupSignalHandlers(void);
void removeSignalHandlers(void);
int createSocketAcceptHandler(socketFds *sfd, aeFileProc *accept_handler);
int changeListenPort(int port, socketFds *sfd, aeFileProc *accept_handler, bool fFirstCall);
int changeBindAddr(sds *addrlist, int addrlist_len, bool fFirstCall);
struct redisCommand *lookupCommand(sds name);
struct redisCommand *lookupCommandByCString(const char *s);
struct redisCommand *lookupCommandOrOriginal(sds name);
void call(client *c, int flags);
void propagate(struct redisCommand *cmd, int dbid, robj **argv, int argc, int flags);
void alsoPropagate(struct redisCommand *cmd, int dbid, robj **argv, int argc, int target);
void redisOpArrayInit(redisOpArray *oa);
void redisOpArrayFree(redisOpArray *oa);
void forceCommandPropagation(client *c, int flags);
void preventCommandPropagation(client *c);
void preventCommandAOF(client *c);
void preventCommandReplication(client *c);
void slowlogPushCurrentCommand(client *c, struct redisCommand *cmd, ustime_t duration);
int prepareForShutdown(int flags);
#ifdef __GNUC__
void _serverLog(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void _serverLog(int level, const char *fmt, ...);
#endif
void serverLogRaw(int level, const char *msg);
void serverLogFromHandler(int level, const char *msg);
void usage(void);
void updateDictResizePolicy(void);
int htNeedsResize(dict *dict);
void populateCommandTable(void);
void resetCommandTableStats(void);
void resetErrorTableStats(void);
void adjustOpenFilesLimit(void);
void incrementErrorCount(const char *fullerr, size_t namelen);
void closeListeningSockets(int unlink_unix_socket);
void updateCachedTime(void);
void resetServerStats(void);
void activeDefragCycle(void);
unsigned int getLRUClock(void);
unsigned int LRU_CLOCK(void);
const char *evictPolicyToString(void);
struct redisMemOverhead *getMemoryOverheadData(void);
void freeMemoryOverheadData(struct redisMemOverhead *mh);
void checkChildrenDone(void);
int setOOMScoreAdj(int process_class);
void rejectCommandFormat(client *c, const char *fmt, ...);
extern "C" void *activeDefragAlloc(void *ptr);
robj *activeDefragStringOb(robj* ob, long *defragged);

#define RESTART_SERVER_NONE 0
#define RESTART_SERVER_GRACEFULLY (1<<0)     /* Do proper shutdown. */
#define RESTART_SERVER_CONFIG_REWRITE (1<<1) /* CONFIG REWRITE before restart.*/
int restartServer(int flags, mstime_t delay);

/* Set data type */
robj *setTypeCreate(const char *value);
int setTypeAdd(robj *subject, const char *value);
int setTypeRemove(robj *subject, const char *value);
int setTypeIsMember(robj_roptr subject, const char *value);
setTypeIterator *setTypeInitIterator(robj_roptr subject);
void setTypeReleaseIterator(setTypeIterator *si);
int setTypeNext(setTypeIterator *si, const char **sdsele, int64_t *llele);
sds setTypeNextObject(setTypeIterator *si);
int setTypeRandomElement(robj *setobj, sds *sdsele, int64_t *llele);
unsigned long setTypeRandomElements(robj *set, unsigned long count, robj *aux_set);
unsigned long setTypeSize(robj_roptr subject);
void setTypeConvert(robj *subject, int enc);
robj *setTypeDup(robj *o);

/* Hash data type */
#define HASH_SET_TAKE_FIELD (1<<0)
#define HASH_SET_TAKE_VALUE (1<<1)
#define HASH_SET_COPY 0

void hashTypeConvert(robj *o, int enc);
void hashTypeTryConversion(robj *subject, robj **argv, int start, int end);
int hashTypeExists(robj_roptr o, const char *key);
int hashTypeDelete(robj *o, sds key);
unsigned long hashTypeLength(robj_roptr o);
hashTypeIterator *hashTypeInitIterator(robj_roptr subject);
void hashTypeReleaseIterator(hashTypeIterator *hi);
int hashTypeNext(hashTypeIterator *hi);
void hashTypeCurrentFromZiplist(hashTypeIterator *hi, int what,
                                unsigned char **vstr,
                                unsigned int *vlen,
                                long long *vll);
sds hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what);
void hashTypeCurrentObject(hashTypeIterator *hi, int what, unsigned char **vstr, unsigned int *vlen, long long *vll);
sds hashTypeCurrentObjectNewSds(hashTypeIterator *hi, int what);
robj *hashTypeLookupWriteOrCreate(client *c, robj *key);
robj *hashTypeGetValueObject(robj_roptr o, sds field);
int hashTypeSet(robj *o, sds field, sds value, int flags);
robj *hashTypeDup(robj *o);
int hashZiplistValidateIntegrity(unsigned char *zl, size_t size, int deep);

/* Pub / Sub */
int pubsubUnsubscribeAllChannels(client *c, int notify);
int pubsubUnsubscribeAllPatterns(client *c, int notify);
int pubsubPublishMessage(robj *channel, robj *message);
void addReplyPubsubMessage(client *c, robj *channel, robj *msg);

/* Keyspace events notification */
void notifyKeyspaceEvent(int type, const char *event, robj *key, int dbid);
int keyspaceEventsStringToFlags(char *classes);
sds keyspaceEventsFlagsToString(int flags);

/* Configuration */
void loadServerConfig(char *filename, char config_from_stdin, char *options);
void appendServerSaveParams(time_t seconds, int changes);
void resetServerSaveParams(void);
struct rewriteConfigState; /* Forward declaration to export API. */
void rewriteConfigRewriteLine(struct rewriteConfigState *state, const char *option, sds line, int force);
void rewriteConfigMarkAsProcessed(struct rewriteConfigState *state, const char *option);
int rewriteConfig(char *path, int force_all);
void initConfigValues();

/* db.c -- Keyspace access API */
class AeLocker;
int removeExpire(redisDb *db, robj *key);
int removeSubkeyExpire(redisDb *db, robj *key, robj *subkey);
void propagateExpire(redisDb *db, robj *key, int lazy);
void propagateSubkeyExpire(redisDb *db, int type, robj *key, robj *subkey);
void deleteExpiredKeyAndPropagate(redisDb *db, robj *keyobj);
int keyIsExpired(const redisDbPersistentDataSnapshot *db, robj *key);
int expireIfNeeded(redisDb *db, robj *key);
void setExpire(client *c, redisDb *db, robj *key, robj *subkey, long long when);
void setExpire(client *c, redisDb *db, robj *key, expireEntry &&entry);
robj_roptr lookupKeyRead(redisDb *db, robj *key, uint64_t mvccCheckpoint, AeLocker &locker);
robj_roptr lookupKeyRead(redisDb *db, robj *key);
int checkAlreadyExpired(long long when);
robj *lookupKeyWrite(redisDb *db, robj *key);
robj_roptr lookupKeyReadOrReply(client *c, robj *key, robj *reply, AeLocker &locker);
robj_roptr lookupKeyReadOrReply(client *c, robj *key, robj *reply);
robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply);
robj_roptr lookupKeyReadWithFlags(redisDb *db, robj *key, int flags);
robj *lookupKeyWriteWithFlags(redisDb *db, robj *key, int flags);
robj_roptr objectCommandLookup(client *c, robj *key);
robj_roptr objectCommandLookupOrReply(client *c, robj *key, robj *reply);
void SentReplyOnKeyMiss(client *c, robj *reply);
int objectSetLRUOrLFU(robj *val, long long lfu_freq, long long lru_idle,
                       long long lru_clock, int lru_multiplier);
#define LOOKUP_NONE 0
#define LOOKUP_NOTOUCH (1<<0)
#define LOOKUP_NONOTIFY (1<<1)
#define LOOKUP_UPDATEMVCC (1<<2)
void dbAdd(redisDb *db, robj *key, robj *val);
void dbOverwrite(redisDb *db, robj *key, robj *val, bool fRemoveExpire = false, dict_iter *pitrExisting = nullptr);
int dbMerge(redisDb *db, sds key, robj *val, int fReplace);
void genericSetKey(client *c, redisDb *db, robj *key, robj *val, int keepttl, int signal);
void setKey(client *c, redisDb *db, robj *key, robj *val);
robj *dbRandomKey(redisDb *db);
int dbSyncDelete(redisDb *db, robj *key);
int dbDelete(redisDb *db, robj *key);
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o);
int dbnumFromDb(redisDb *db);

#define EMPTYDB_NO_FLAGS 0      /* No flags. */
#define EMPTYDB_ASYNC (1<<0)    /* Reclaim memory in another thread. */
long long emptyDb(int dbnum, int flags, void(callback)(void*));
long long emptyDbStructure(redisDb **dbarray, int dbnum, int flags, void(callback)(void*));
void flushAllDataAndResetRDB(int flags);
long long dbTotalServerKeyCount();
const dbBackup *backupDb(void);
void restoreDbBackup(const dbBackup *buckup);
void discardDbBackup(const dbBackup *buckup, int flags, void(callback)(void*));


int selectDb(client *c, int id);
void signalModifiedKey(client *c, redisDb *db, robj *key);
void signalFlushedDb(int dbid, int async);
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count);
unsigned int countKeysInSlot(unsigned int hashslot);
unsigned int delKeysInSlot(unsigned int hashslot);
int verifyClusterConfigWithData(void);
void scanGenericCommand(client *c, robj_roptr o, unsigned long cursor);
int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor);
void slotToKeyAdd(sds key);
void slotToKeyDel(sds key);
int dbAsyncDelete(redisDb *db, robj *key);
void slotToKeyFlush(int async);
size_t lazyfreeGetPendingObjectsCount(void);
size_t lazyfreeGetFreedObjectsCount(void);
void freeObjAsync(robj *key, robj *obj);
void freeSlotsToKeysMapAsync(rax *rt);
void freeSlotsToKeysMap(rax *rt, int async);


/* API to get key arguments from commands */
int *getKeysPrepareResult(getKeysResult *result, int numkeys);
int getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
void getKeysFreeResult(getKeysResult *result);
int zunionInterDiffGetKeys(struct redisCommand *cmd,robj **argv, int argc, getKeysResult *result);
int zunionInterDiffStoreGetKeys(struct redisCommand *cmd,robj **argv, int argc, getKeysResult *result);
int evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int xreadGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int memoryGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int lcsGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

/* Cluster */
void clusterInit(void);
extern "C" unsigned short crc16(const char *buf, int len);
unsigned int keyHashSlot(const char *key, int keylen);
void clusterCron(void);
void clusterPropagatePublish(robj *channel, robj *message);
void migrateCloseTimedoutSockets(void);
void clusterBeforeSleep(void);
int clusterSendModuleMessageToTarget(const char *target, uint64_t module_id, uint8_t type, unsigned char *payload, uint32_t len);
void createDumpPayload(rio *payload, robj_roptr o, robj *key);

/* Sentinel */
void initSentinelConfig(void);
void initSentinel(void);
void sentinelTimer(void);
const char *sentinelHandleConfiguration(char **argv, int argc);
void queueSentinelConfig(sds *argv, int argc, int linenum, sds line);
void loadSentinelConfigFromQueue(void);
void sentinelIsRunning(void);
void sentinelCheckConfigFile(void);

/* keydb-check-rdb & aof */
int redis_check_rdb(const char *rdbfilename, FILE *fp);
int redis_check_rdb_main(int argc, const char **argv, FILE *fp);
int redis_check_aof_main(int argc, char **argv);

/* Scripting */
void scriptingInit(int setup);
int ldbRemoveChild(pid_t pid);
void ldbKillForkedSessions(void);
int ldbPendingChildren(void);
sds luaCreateFunction(client *c, lua_State *lua, robj *body);
void freeLuaScriptsAsync(dict *lua_scripts);

/* Blocked clients */
void processUnblockedClients(int iel);
void blockClient(client *c, int btype);
void unblockClient(client *c);
void queueClientForReprocessing(client *c);
void replyToBlockedClientTimedOut(client *c);
int getTimeoutFromObjectOrReply(client *c, robj *object, mstime_t *timeout, int unit);
void disconnectAllBlockedClients(void);
void handleClientsBlockedOnKeys(void);
void signalKeyAsReady(redisDb *db, robj *key, int type);
void signalKeyAsReady(redisDb *db, sds key, int type);
void blockForKeys(client *c, int btype, robj **keys, int numkeys, mstime_t timeout, robj *target, struct listPos *listpos, streamID *ids);
void updateStatsOnUnblock(client *c, long blocked_us, long reply_us);

/* timeout.c -- Blocked clients timeout and connections timeout. */
void addClientToTimeoutTable(client *c);
void removeClientFromTimeoutTable(client *c);
void handleBlockedClientsTimeout(void);
int clientsCronHandleTimeout(client *c, mstime_t now_ms);

/* timeout.c -- Blocked clients timeout and connections timeout. */
void addClientToTimeoutTable(client *c);
void removeClientFromTimeoutTable(client *c);
void handleBlockedClientsTimeout(void);
int clientsCronHandleTimeout(client *c, mstime_t now_ms);

/* expire.c -- Handling of expired keys */
void activeExpireCycle(int type);
void expireSlaveKeys(void);
void rememberSlaveKeyWithExpire(redisDb *db, robj *key);
void flushSlaveKeysWithExpireList(void);
size_t getSlaveKeyWithExpireCount(void);

/* evict.c -- maxmemory handling and LRU eviction. */
void evictionPoolAlloc(void);
#define LFU_INIT_VAL 5
unsigned long LFUGetTimeInMinutes(void);
uint8_t LFULogIncr(uint8_t value);
unsigned long LFUDecrAndReturn(robj_roptr o);
#define EVICT_OK 0
#define EVICT_RUNNING 1
#define EVICT_FAIL 2
int performEvictions(bool fPreSnapshot);

/* meminfo.cpp -- get memory info from /proc/memoryinfo for linux distros */
size_t getMemAvailable();
size_t getMemTotal();

/* Keys hashing / comparison functions for dict.c hash tables. */
uint64_t dictSdsHash(const void *key);
int dictSdsKeyCompare(void *privdata, const void *key1, const void *key2);
void dictSdsDestructor(void *privdata, void *val);

/* Git SHA1 */
extern "C" char *redisGitSHA1(void);
extern "C" char *redisGitDirty(void);
extern "C" uint64_t redisBuildId(void);
extern "C" char *redisBuildIdString(void);

int parseUnitString(const char *sz);

/* Commands prototypes */
void authCommand(client *c);
void pingCommand(client *c);
void echoCommand(client *c);
void commandCommand(client *c);
void setCommand(client *c);
void setnxCommand(client *c);
void setexCommand(client *c);
void psetexCommand(client *c);
void getCommand(client *c);
void getexCommand(client *c);
void getdelCommand(client *c);
void delCommand(client *c);
void unlinkCommand(client *c);
void existsCommand(client *c);
void mexistsCommand(client *c);
void setbitCommand(client *c);
void getbitCommand(client *c);
void bitfieldCommand(client *c);
void bitfieldroCommand(client *c);
void setrangeCommand(client *c);
void getrangeCommand(client *c);
void incrCommand(client *c);
void decrCommand(client *c);
void incrbyCommand(client *c);
void decrbyCommand(client *c);
void incrbyfloatCommand(client *c);
void selectCommand(client *c);
void swapdbCommand(client *c);
void randomkeyCommand(client *c);
void keysCommand(client *c);
void scanCommand(client *c);
void dbsizeCommand(client *c);
void lastsaveCommand(client *c);
void saveCommand(client *c);
void bgsaveCommand(client *c);
void bgrewriteaofCommand(client *c);
void shutdownCommand(client *c);
void moveCommand(client *c);
void copyCommand(client *c);
void renameCommand(client *c);
void renamenxCommand(client *c);
void lpushCommand(client *c);
void rpushCommand(client *c);
void lpushxCommand(client *c);
void rpushxCommand(client *c);
void linsertCommand(client *c);
void lpopCommand(client *c);
void rpopCommand(client *c);
void llenCommand(client *c);
void lindexCommand(client *c);
void lrangeCommand(client *c);
void ltrimCommand(client *c);
void typeCommand(client *c);
void lsetCommand(client *c);
void saddCommand(client *c);
void sremCommand(client *c);
void smoveCommand(client *c);
void sismemberCommand(client *c);
void smismemberCommand(client *c);
void scardCommand(client *c);
void spopCommand(client *c);
void srandmemberCommand(client *c);
void sinterCommand(client *c);
void sinterstoreCommand(client *c);
void sunionCommand(client *c);
void sunionstoreCommand(client *c);
void sdiffCommand(client *c);
void sdiffstoreCommand(client *c);
void sscanCommand(client *c);
void syncCommand(client *c);
void flushdbCommand(client *c);
void flushallCommand(client *c);
void sortCommand(client *c);
void lremCommand(client *c);
void lposCommand(client *c);
void rpoplpushCommand(client *c);
void lmoveCommand(client *c);
void infoCommand(client *c);
void mgetCommand(client *c);
void monitorCommand(client *c);
void expireCommand(client *c);
void expireatCommand(client *c);
void expireMemberCommand(client *c);
void expireMemberAtCommand(client *c);
void pexpireMemberAtCommand(client *c);
void pexpireCommand(client *c);
void pexpireatCommand(client *c);
void getsetCommand(client *c);
void ttlCommand(client *c);
void touchCommand(client *c);
void pttlCommand(client *c);
void persistCommand(client *c);
void replicaofCommand(client *c);
void roleCommand(client *c);
void debugCommand(client *c);
void msetCommand(client *c);
void msetnxCommand(client *c);
void zaddCommand(client *c);
void zincrbyCommand(client *c);
void zrangeCommand(client *c);
void zrangebyscoreCommand(client *c);
void zrevrangebyscoreCommand(client *c);
void zrangebylexCommand(client *c);
void zrevrangebylexCommand(client *c);
void zcountCommand(client *c);
void zlexcountCommand(client *c);
void zrevrangeCommand(client *c);
void zcardCommand(client *c);
void zremCommand(client *c);
void zscoreCommand(client *c);
void zmscoreCommand(client *c);
void zremrangebyscoreCommand(client *c);
void zremrangebylexCommand(client *c);
void zpopminCommand(client *c);
void zpopmaxCommand(client *c);
void bzpopminCommand(client *c);
void bzpopmaxCommand(client *c);
void zrandmemberCommand(client *c);
void multiCommand(client *c);
void execCommand(client *c);
void discardCommand(client *c);
void blpopCommand(client *c);
void brpopCommand(client *c);
void brpoplpushCommand(client *c);
void blmoveCommand(client *c);
void appendCommand(client *c);
void strlenCommand(client *c);
void zrankCommand(client *c);
void zrevrankCommand(client *c);
void hsetCommand(client *c);
void hsetnxCommand(client *c);
void hgetCommand(client *c);
void hmsetCommand(client *c);
void hmgetCommand(client *c);
void hdelCommand(client *c);
void hlenCommand(client *c);
void hstrlenCommand(client *c);
void zremrangebyrankCommand(client *c);
void zunionstoreCommand(client *c);
void zinterstoreCommand(client *c);
void zdiffstoreCommand(client *c);
void zunionCommand(client *c);
void zinterCommand(client *c);
void zrangestoreCommand(client *c);
void zdiffCommand(client *c);
void zscanCommand(client *c);
void hkeysCommand(client *c);
void hvalsCommand(client *c);
void hgetallCommand(client *c);
void hexistsCommand(client *c);
void hscanCommand(client *c);
void hrandfieldCommand(client *c);
void configCommand(client *c);
void hincrbyCommand(client *c);
void hincrbyfloatCommand(client *c);
void subscribeCommand(client *c);
void unsubscribeCommand(client *c);
void psubscribeCommand(client *c);
void punsubscribeCommand(client *c);
void publishCommand(client *c);
void pubsubCommand(client *c);
void watchCommand(client *c);
void unwatchCommand(client *c);
void clusterCommand(client *c);
void restoreCommand(client *c);
void mvccrestoreCommand(client *c);
void migrateCommand(client *c);
void askingCommand(client *c);
void readonlyCommand(client *c);
void readwriteCommand(client *c);
void dumpCommand(client *c);
void objectCommand(client *c);
void memoryCommand(client *c);
void clientCommand(client *c);
void helloCommand(client *c);
void evalCommand(client *c);
void evalShaCommand(client *c);
void scriptCommand(client *c);
void timeCommand(client *c);
void bitopCommand(client *c);
void bitcountCommand(client *c);
void bitposCommand(client *c);
void replconfCommand(client *c);
void waitCommand(client *c);
void geoencodeCommand(client *c);
void geodecodeCommand(client *c);
void georadiusbymemberCommand(client *c);
void georadiusbymemberroCommand(client *c);
void georadiusCommand(client *c);
void georadiusroCommand(client *c);
void geoaddCommand(client *c);
void geohashCommand(client *c);
void geoposCommand(client *c);
void geodistCommand(client *c);
void geosearchCommand(client *c);
void geosearchstoreCommand(client *c);
void pfselftestCommand(client *c);
void pfaddCommand(client *c);
void pfcountCommand(client *c);
void pfmergeCommand(client *c);
void pfdebugCommand(client *c);
void latencyCommand(client *c);
void moduleCommand(client *c);
void securityWarningCommand(client *c);
void xaddCommand(client *c);
void xrangeCommand(client *c);
void xrevrangeCommand(client *c);
void xlenCommand(client *c);
void xreadCommand(client *c);
void xgroupCommand(client *c);
void xsetidCommand(client *c);
void xackCommand(client *c);
void xpendingCommand(client *c);
void xclaimCommand(client *c);
void xautoclaimCommand(client *c);
void xinfoCommand(client *c);
void xdelCommand(client *c);
void xtrimCommand(client *c);
void aclCommand(client *c);
void replicaReplayCommand(client *c);
void hrenameCommand(client *c);
void stralgoCommand(client *c);
void resetCommand(client *c);
void failoverCommand(client *c);
void lfenceCommand(client *c);


int FBrokenLinkToMaster(int *pconnectMasters = nullptr);
int FActiveMaster(client *c);
struct redisMaster *MasterInfoFromClient(client *c);
bool FInReplicaReplay();
void updateActiveReplicaMastersFromRsi(rdbSaveInfo *rsi);

/* MVCC */
uint64_t getMvccTstamp();
void incrementMvccTstamp();

#if __GNUC__ >= 7 && !defined(NO_DEPRECATE_FREE) && !defined(ALPINE)
 [[deprecated]]
void *calloc(size_t count, size_t size) noexcept;
 [[deprecated]]
void free(void *ptr) noexcept;
 [[deprecated]]
void *malloc(size_t size) noexcept;
 [[deprecated]]
void *realloc(void *ptr, size_t size) noexcept;
#endif

/* Debugging stuff */
void bugReportStart(void);
void serverLogObjectDebugInfo(robj_roptr o);
void sigsegvHandler(int sig, siginfo_t *info, void *secret);
const char *getSafeInfoString(const char *s, size_t len, char **tmp);
sds genRedisInfoString(const char *section);
sds genModulesInfoString(sds info);
void enableWatchdog(int period);
void disableWatchdog(void);
void watchdogScheduleSignal(int period);
void serverLogHexDump(int level, const char *descr, void *value, size_t len);
extern "C" int memtest_preserving_test(unsigned long *m, size_t bytes, int passes);
void mixDigest(unsigned char *digest, const void *ptr, size_t len);
void xorDigest(unsigned char *digest, const void *ptr, size_t len);
int populateCommandTableParseFlags(struct redisCommand *c, const char *strflags);



int moduleGILAcquiredByModule(void);
extern int g_fInCrash;
static inline int GlobalLocksAcquired(void)  // Used in asserts to verify all global locks are correctly acquired for a server-thread to operate
{
    return aeThreadOwnsLock() || moduleGILAcquiredByModule() || g_fInCrash;
}

inline int ielFromEventLoop(const aeEventLoop *eventLoop)
{
    int iel = 0;
    for (; iel < cserver.cthreads; ++iel)
    {
        if (g_pserver->rgthreadvar[iel].el == eventLoop)
            break;
    }
    serverAssert(iel < cserver.cthreads);
    return iel;
}

inline bool FFastSyncEnabled() {
    return g_pserver->fEnableFastSync && !g_pserver->fActiveReplica;
}

inline int FCorrectThread(client *c)
{
    return (c->conn == nullptr)
        || (c->iel == IDX_EVENT_LOOP_MAIN && moduleGILAcquiredByModule())
        || (serverTL != NULL && (g_pserver->rgthreadvar[c->iel].el == serverTL->el));
}
#define AssertCorrectThread(c) serverAssert(FCorrectThread(c))

void flushReplBacklogToClients();

template<typename FN_PTR, class ...TARGS>
void runAndPropogateToReplicas(FN_PTR *pfn, TARGS... args) {
    // 存储复制积压缓冲区(replication backlog)的起始参数，用于计算写入的数据量
    // 这些是TLS(线程本地存储)变量，因为当需要扩展缓冲区时需要更新它们
    bool fNestedProcess = (g_pserver->repl_batch_idxStart >= 0);
    if (!fNestedProcess) {
        g_pserver->repl_batch_offStart = g_pserver->master_repl_offset;
        g_pserver->repl_batch_idxStart = g_pserver->repl_backlog_idx;
    }

    pfn(args...);

    if (!fNestedProcess) {
        flushReplBacklogToClients();
        g_pserver->repl_batch_offStart = -1;
        g_pserver->repl_batch_idxStart = -1;
    }
}

void debugDelay(int usec);
void killIOThreads(void);
void killThreads(void);
void makeThreadKillable(void);

/* 使用宏检查日志级别，避免在因级别过低需要忽略日志时计算参数 */
#define serverLog(level, ...) do {\
        if (((level)&0xff) < cserver.verbosity) break;\
        _serverLog(level, __VA_ARGS__);\
    } while(0)

/* TLS (传输层安全)相关函数 */
void tlsInit(void);                      // 初始化TLS系统
void tlsInitThread();                    // 初始化线程级TLS
void tlsCleanupThread();                 // 清理线程级TLS
void tlsCleanup(void);                   // 清理TLS系统
int tlsConfigure(redisTLSContextConfig *ctx_config); // 配置TLS上下文
void tlsReload(void);                    // 重新加载TLS配置                // 重新加载TLS配置


class ShutdownException
{};

#define redisDebug(fmt, ...) \
    printf("DEBUG %s:%d > " fmt "\n", __FILE__, __LINE__, __VA_ARGS__)
#define redisDebugMark() \
    printf("-- MARK %s:%d --\n", __FILE__, __LINE__)

int iAmMaster(void);

#endif


