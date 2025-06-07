/*
 * Copyright (c) 2009-2016, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2019 John Sully <john at eqalpha dot com>
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

#include "server.h"
#include "monotonic.h"
#include "cluster.h"
#include "slowlog.h"
#include "bio.h"
#include "latency.h"
#include "atomicvar.h"
#include "storage.h"
#include "cron.h"
#include <thread>
#include "mt19937-64.h"

#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <sys/utsname.h>
#include <locale.h>
#include <sys/socket.h>
#include <algorithm>
#include <uuid/uuid.h>
#include <condition_variable>
#include "aelocker.h"
#include "motd.h"
#include "t_nhash.h"
#include "readwritelock.h"
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#endif

int g_fTestMode = false;
const char *motd_url = "http://api.keydb.dev/motd/motd_server.txt";
const char *motd_cache_file = "/.keydb-server-motd";

/* 我们共享的“通用”对象 */

struct sharedObjectsStruct shared;

/* 实际用作常量的全局变量。以下 double 值
 * 用于磁盘上的 double 类型序列化，并在运行时初始化
 * 以避免奇怪的编译器优化。 */

double R_Zero, R_PosInf, R_NegInf, R_Nan;

/*================================= 全局变量 ================================= */

/* 全局变量 */
namespace GlobalHidden {
struct redisServer server; /* 服务器全局状态 */
}
redisServer *g_pserver = &GlobalHidden::server;
struct redisServerConst cserver;
thread_local struct redisServerThreadVars *serverTL = NULL;   // 线程本地服务器变量
fastlock time_thread_lock("Time thread lock");
std::condition_variable_any time_thread_cv;
int sleeping_threads = 0;
void wakeTimeThread();

/* 我们的命令表。
 *
 * 每个条目包含以下字段：
 *
 * name:        表示命令名称的字符串。
 *
 * function:    指向实现该命令的 C 函数的指针。
 *
 * arity:       参数数量，可以使用 -N 表示 >= N
 *
 * sflags:      命令标志的字符串形式。有关标志表，请参见下文。
 *
 * flags:       标志的位掩码形式。由 Redis 使用 'sflags' 字段计算得出。
 *
 * get_keys_proc: 一个可选函数，用于从命令中获取键参数。
 *                仅当以下三个字段不足以指定哪些参数是键时才使用此函数。
 *
 * first_key_index: 第一个作为键的参数
 *
 * last_key_index: 最后一个作为键的参数
 *
 * key_step:    从第一个参数到最后一个参数获取所有键的步长。
 *              例如，在 MSET 中，步长为 2，因为参数是 key,val,key,val,...
 *
 * microseconds: 此命令的总执行时间的微秒数。
 *
 * calls:       此命令的总调用次数。
 *
 * id:          用于 ACL 或其他目标的命令位标识符。
 *
 * flags、microseconds 和 calls 字段由 Redis 计算得出，应始终设置为零。
 *
 * 命令标志使用空格分隔的字符串表示，这些字符串会通过 populateCommandTable() 函数转换为实际的标志。
 *
 * 以下是这些标志的含义：
 *
 * write:       写命令（可能会修改键空间）。
 *
 * read-only:   仅从键读取而不更改内容的命令。
 *              请注意，不从键空间读取的命令（例如
 *              TIME、SELECT、INFO、管理命令以及与连接
 *              或事务相关的命令（multi、exec、discard 等））
 *              不会被标记为只读命令，因为它们会以其他方式影响
 *              服务器或连接。
 *
 * use-memory:  调用后可能会增加内存使用量。如果内存不足，则不允许。
 *
 * admin:       管理命令，例如 SAVE 或 SHUTDOWN。
 *
 * pub-sub:     与发布/订阅相关的命令。
 *
 * no-script:   脚本中不允许使用的命令。
 *
 * random:      随机命令。命令不具有确定性，也就是说，对于相同的
 *              命令、相同的参数和相同的键空间，可能会产生不同的结果。
 *              例如，SPOP 和 RANDOMKEY 就是两个随机命令。
 *
 * to-sort:     如果从脚本调用，则对命令输出数组进行排序，以确保
 *              输出具有确定性。使用此标志时（并非总是可行），
 *              则不需要 "random" 标志。
 *
 * ok-loading:  加载数据库时允许执行该命令。
 *
 * ok-stale:    当副本具有过时数据但又不允许提供此数据时，允许执行该命令。
 *              通常情况下，在这种情况下不接受任何命令，只有少数例外。
 *
 * no-monitor:  不要在 MONITOR 上自动传播该命令。
 *
 * no-slowlog:  不要自动将该命令传播到慢日志。
 *
 * cluster-asking: 对此命令执行隐式 ASKING，因此如果插槽标记为
 *              'importing'，则该命令将在集群模式下被接受。
 *
 * fast:        快速命令：O(1) 或 O(log(N)) 命令，只要内核调度程序
 *              给我们时间，就永远不应延迟其执行。请注意，可能会触发
 *              DEL 作为副作用的命令（例如 SET）不是快速命令。
 *
 * may-replicate: 命令可能会产生复制流量，但在不允许写命令的情况下
 *                应该允许。示例包括 PUBLISH（复制 pubsub 消息）和
 *                EVAL（可能执行写命令（这些命令会被复制），或者可能只执行读命令）。
 *                一个命令不能同时标记为 "write" 和 "may-replicate"。
 *
 * 以下附加标志仅用于将命令放入特定的 ACL 类别。命令可以具有多个 ACL 类别。
 *
 * @keyspace, @read, @write, @set, @sortedset, @list, @hash, @string, @bitmap,
 * @hyperloglog, @stream, @admin, @fast, @slow, @pubsub, @blocking, @dangerous,
 * @connection, @transaction, @scripting, @geo, @replication.
 *
 * 请注意：
 *
 * 1) read-only 标志表示 @read ACL 类别。
 * 2) write 标志表示 @write ACL 类别。
 * 3) fast 标志表示 @fast ACL 类别。
 * 4) admin 标志表示 @admin 和 @dangerous ACL 类别。
 * 5) pub-sub 标志表示 @pubsub ACL 类别。
 * 6)缺少 fast 标志表示 @slow ACL 类别。
 * 7) 不明显的 "keyspace" 类别包括与键交互而与
 *    特定数据结构无关的命令，例如：DEL、RENAME、MOVE、SELECT、
 *    TYPE、EXPIRE*、PEXPIRE*、TTL、PTTL 等。
 */

struct redisCommand redisCommandTable[] = {
    {"module",moduleCommand,-2,
     "admin no-script",
     0,NULL,0,0,0,0,0,0},

    {"get",getCommand,2,
     "read-only fast async @string",
     0,NULL,1,1,1,0,0,0},

    {"getex",getexCommand,-2,
     "write fast @string",
     0,NULL,1,1,1,0,0,0},

    {"getdel",getdelCommand,2,
     "write fast @string",
     0,NULL,1,1,1,0,0,0},

    /* Note that we can't flag set as fast, since it may perform an
     * implicit DEL of a large key. */
    {"set",setCommand,-3,
     "write use-memory @string",
     0,NULL,1,1,1,0,0,0},

    {"setnx",setnxCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"setex",setexCommand,4,
     "write use-memory @string",
     0,NULL,1,1,1,0,0,0},

    {"psetex",psetexCommand,4,
     "write use-memory @string",
     0,NULL,1,1,1,0,0,0},

    {"append",appendCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"strlen",strlenCommand,2,
     "read-only fast @string",
     0,NULL,1,1,1,0,0,0},

    {"del",delCommand,-2,
     "write @keyspace",
     0,NULL,1,-1,1,0,0,0},

    {"expdel",delCommand,-2,
     "write @keyspace",
     0,NULL,1,-1,1,0,0,0},

    {"unlink",unlinkCommand,-2,
     "write fast @keyspace",
     0,NULL,1,-1,1,0,0,0},

    {"exists",existsCommand,-2,
     "read-only fast @keyspace",
     0,NULL,1,-1,1,0,0,0},

    {"keydb.mexists",mexistsCommand,-2,
     "read-only fast @keyspace",
     0,NULL,1,-1,1,0,0,0},

    {"setbit",setbitCommand,4,
     "write use-memory @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"getbit",getbitCommand,3,
     "read-only fast @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"bitfield",bitfieldCommand,-2,
     "write use-memory @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"bitfield_ro",bitfieldroCommand,-2,
     "read-only fast @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"setrange",setrangeCommand,4,
     "write use-memory @string",
     0,NULL,1,1,1,0,0,0},

    {"getrange",getrangeCommand,4,
     "read-only @string",
     0,NULL,1,1,1,0,0,0},

    {"substr",getrangeCommand,4,
     "read-only @string",
     0,NULL,1,1,1,0,0,0},

    {"incr",incrCommand,2,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"decr",decrCommand,2,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"mget",mgetCommand,-2,
     "read-only fast async @string",
     0,NULL,1,-1,1,0,0,0},

    {"rpush",rpushCommand,-3,
     "write use-memory fast @list",
     0,NULL,1,1,1,0,0,0},

    {"lpush",lpushCommand,-3,
     "write use-memory fast @list",
     0,NULL,1,1,1,0,0,0},

    {"rpushx",rpushxCommand,-3,
     "write use-memory fast @list",
     0,NULL,1,1,1,0,0,0},

    {"lpushx",lpushxCommand,-3,
     "write use-memory fast @list",
     0,NULL,1,1,1,0,0,0},

    {"linsert",linsertCommand,5,
     "write use-memory @list",
     0,NULL,1,1,1,0,0,0},

    {"rpop",rpopCommand,-2,
     "write fast @list",
     0,NULL,1,1,1,0,0,0},

    {"lpop",lpopCommand,-2,
     "write fast @list",
     0,NULL,1,1,1,0,0,0},

    {"brpop",brpopCommand,-3,
     "write no-script @list @blocking",
     0,NULL,1,-2,1,0,0,0},

    {"brpoplpush",brpoplpushCommand,4,
     "write use-memory no-script @list @blocking",
     0,NULL,1,2,1,0,0,0},

    {"blmove",blmoveCommand,6,
     "write use-memory no-script @list @blocking",
     0,NULL,1,2,1,0,0,0},

    {"blpop",blpopCommand,-3,
     "write no-script @list @blocking",
     0,NULL,1,-2,1,0,0,0},

    {"llen",llenCommand,2,
     "read-only fast @list",
     0,NULL,1,1,1,0,0,0},

    {"lindex",lindexCommand,3,
     "read-only @list",
     0,NULL,1,1,1,0,0,0},

    {"lset",lsetCommand,4,
     "write use-memory @list",
     0,NULL,1,1,1,0,0,0},

    {"lrange",lrangeCommand,4,
     "read-only @list",
     0,NULL,1,1,1,0,0,0},

    {"ltrim",ltrimCommand,4,
     "write @list",
     0,NULL,1,1,1,0,0,0},

    {"lpos",lposCommand,-3,
     "read-only @list",
     0,NULL,1,1,1,0,0,0},

    {"lrem",lremCommand,4,
     "write @list",
     0,NULL,1,1,1,0,0,0},

    {"rpoplpush",rpoplpushCommand,3,
     "write use-memory @list",
     0,NULL,1,2,1,0,0,0},

    {"lmove",lmoveCommand,5,
     "write use-memory @list",
     0,NULL,1,2,1,0,0,0},

    {"sadd",saddCommand,-3,
     "write use-memory fast @set",
     0,NULL,1,1,1,0,0,0},

    {"srem",sremCommand,-3,
     "write fast @set",
     0,NULL,1,1,1,0,0,0},

    {"smove",smoveCommand,4,
     "write fast @set",
     0,NULL,1,2,1,0,0,0},

    {"sismember",sismemberCommand,3,
     "read-only fast @set",
     0,NULL,1,1,1,0,0,0},

    {"smismember",smismemberCommand,-3,
     "read-only fast @set",
     0,NULL,1,1,1,0,0,0},

    {"scard",scardCommand,2,
     "read-only fast @set",
     0,NULL,1,1,1,0,0,0},

    {"spop",spopCommand,-2,
     "write random fast @set",
     0,NULL,1,1,1,0,0,0},

    {"srandmember",srandmemberCommand,-2,
     "read-only random @set",
     0,NULL,1,1,1,0,0,0},

    {"sinter",sinterCommand,-2,
     "read-only to-sort @set",
     0,NULL,1,-1,1,0,0,0},

    {"sinterstore",sinterstoreCommand,-3,
     "write use-memory @set",
     0,NULL,1,-1,1,0,0,0},

    {"sunion",sunionCommand,-2,
     "read-only to-sort @set",
     0,NULL,1,-1,1,0,0,0},

    {"sunionstore",sunionstoreCommand,-3,
     "write use-memory @set",
     0,NULL,1,-1,1,0,0,0},

    {"sdiff",sdiffCommand,-2,
     "read-only to-sort @set",
     0,NULL,1,-1,1,0,0,0},

    {"sdiffstore",sdiffstoreCommand,-3,
     "write use-memory @set",
     0,NULL,1,-1,1,0,0,0},

    {"smembers",sinterCommand,2,
     "read-only to-sort @set",
     0,NULL,1,1,1,0,0,0},

    {"sscan",sscanCommand,-3,
     "read-only random @set",
     0,NULL,1,1,1,0,0,0},

    {"zadd",zaddCommand,-4,
     "write use-memory fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zincrby",zincrbyCommand,4,
     "write use-memory fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrem",zremCommand,-3,
     "write fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zremrangebyscore",zremrangebyscoreCommand,4,
     "write @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zremrangebyrank",zremrangebyrankCommand,4,
     "write @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zremrangebylex",zremrangebylexCommand,4,
     "write @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zunionstore",zunionstoreCommand,-4,
     "write use-memory @sortedset",
     0,zunionInterDiffStoreGetKeys,1,1,1,0,0,0},

    {"zinterstore",zinterstoreCommand,-4,
     "write use-memory @sortedset",
     0,zunionInterDiffStoreGetKeys,1,1,1,0,0,0},

    {"zdiffstore",zdiffstoreCommand,-4,
     "write use-memory @sortedset",
     0,zunionInterDiffStoreGetKeys,1,1,1,0,0,0},

    {"zunion",zunionCommand,-3,
     "read-only @sortedset",
     0,zunionInterDiffGetKeys,0,0,0,0,0,0},

    {"zinter",zinterCommand,-3,
     "read-only @sortedset",
     0,zunionInterDiffGetKeys,0,0,0,0,0,0},

    {"zdiff",zdiffCommand,-3,
     "read-only @sortedset",
     0,zunionInterDiffGetKeys,0,0,0,0,0,0},

    {"zrange",zrangeCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrangestore",zrangestoreCommand,-5,
     "write use-memory @sortedset",
     0,NULL,1,2,1,0,0,0},

    {"zrangebyscore",zrangebyscoreCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrevrangebyscore",zrevrangebyscoreCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrangebylex",zrangebylexCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrevrangebylex",zrevrangebylexCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zcount",zcountCommand,4,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zlexcount",zlexcountCommand,4,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrevrange",zrevrangeCommand,-4,
     "read-only @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zcard",zcardCommand,2,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zscore",zscoreCommand,3,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zmscore",zmscoreCommand,-3,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrank",zrankCommand,3,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zrevrank",zrevrankCommand,3,
     "read-only fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zscan",zscanCommand,-3,
     "read-only random @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zpopmin",zpopminCommand,-2,
     "write fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"zpopmax",zpopmaxCommand,-2,
     "write fast @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"bzpopmin",bzpopminCommand,-3,
     "write no-script fast @sortedset @blocking",
     0,NULL,1,-2,1,0,0,0},

    {"bzpopmax",bzpopmaxCommand,-3,
     "write no-script fast @sortedset @blocking",
     0,NULL,1,-2,1,0,0,0},

    {"zrandmember",zrandmemberCommand,-2,
     "read-only random @sortedset",
     0,NULL,1,1,1,0,0,0},

    {"hset",hsetCommand,-4,
     "write use-memory fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hsetnx",hsetnxCommand,4,
     "write use-memory fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hget",hgetCommand,3,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hmset",hsetCommand,-4,
     "write use-memory fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hmget",hmgetCommand,-3,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hincrby",hincrbyCommand,4,
     "write use-memory fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hincrbyfloat",hincrbyfloatCommand,4,
     "write use-memory fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hdel",hdelCommand,-3,
     "write fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hlen",hlenCommand,2,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hstrlen",hstrlenCommand,3,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hkeys",hkeysCommand,2,
     "read-only to-sort @hash",
     0,NULL,1,1,1,0,0,0},

    {"hvals",hvalsCommand,2,
     "read-only to-sort @hash",
     0,NULL,1,1,1,0,0,0},

    {"hgetall",hgetallCommand,2,
     "read-only random @hash",
     0,NULL,1,1,1,0,0,0},

    {"hexists",hexistsCommand,3,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"hrandfield",hrandfieldCommand,-2,
     "read-only random @hash",
     0,NULL,1,1,1,0,0,0},

    {"hscan",hscanCommand,-3,
     "read-only random @hash",
     0,NULL,1,1,1,0,0,0},

    {"incrby",incrbyCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"decrby",decrbyCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"incrbyfloat",incrbyfloatCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"getset",getsetCommand,3,
     "write use-memory fast @string",
     0,NULL,1,1,1,0,0,0},

    {"mset",msetCommand,-3,
     "write use-memory @string",
     0,NULL,1,-1,2,0,0,0},

    {"msetnx",msetnxCommand,-3,
     "write use-memory @string",
     0,NULL,1,-1,2,0,0,0},

    {"randomkey",randomkeyCommand,1,
     "read-only random @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"select",selectCommand,2,
     "ok-loading fast ok-stale @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"swapdb",swapdbCommand,3,
     "write fast @keyspace @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"move",moveCommand,3,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"copy",copyCommand,-3,
     "write use-memory @keyspace",
     0,NULL,1,2,1,0,0,0},

    /* Like for SET, we can't mark rename as a fast command because
     * overwriting the target key may result in an implicit slow DEL. */
    {"rename",renameCommand,3,
     "write @keyspace",
     0,NULL,1,2,1,0,0,0},

    {"renamenx",renamenxCommand,3,
     "write fast @keyspace",
     0,NULL,1,2,1,0,0,0},

    {"expire",expireCommand,3,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"expireat",expireatCommand,3,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"expiremember", expireMemberCommand, -4,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},
    
    {"expirememberat", expireMemberAtCommand, 4,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},
    
    {"pexpirememberat", pexpireMemberAtCommand, 4,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"pexpire",pexpireCommand,3,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"pexpireat",pexpireatCommand,3,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"keys",keysCommand,2,
     "read-only to-sort @keyspace @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"scan",scanCommand,-2,
     "read-only random @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"dbsize",dbsizeCommand,1,
     "read-only fast @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"auth",authCommand,-2,
     "no-auth no-script ok-loading ok-stale fast @connection",
     0,NULL,0,0,0,0,0,0},

    /* We don't allow PING during loading since in Redis PING is used as
     * failure detection, and a loading server is considered to be
     * not available. */
    {"ping",pingCommand,-1,
     "ok-stale ok-loading fast @connection @replication",
     0,NULL,0,0,0,0,0,0},

     {"replping",pingCommand,-1,
     "ok-stale fast @connection @replication",
     0,NULL,0,0,0,0,0,0},

    {"echo",echoCommand,2,
     "fast @connection",
     0,NULL,0,0,0,0,0,0},

    {"save",saveCommand,1,
     "admin no-script",
     0,NULL,0,0,0,0,0,0},

    {"bgsave",bgsaveCommand,-1,
     "admin no-script",
     0,NULL,0,0,0,0,0,0},

    {"bgrewriteaof",bgrewriteaofCommand,1,
     "admin no-script",
     0,NULL,0,0,0,0,0,0},

    {"shutdown",shutdownCommand,-1,
     "admin no-script ok-loading ok-stale noprop",
     0,NULL,0,0,0,0,0,0},

    {"lastsave",lastsaveCommand,1,
     "random fast ok-loading ok-stale @admin @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"type",typeCommand,2,
     "read-only fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"multi",multiCommand,1,
     "no-script fast ok-loading ok-stale @transaction",
     0,NULL,0,0,0,0,0,0},

    {"exec",execCommand,1,
     "no-script no-slowlog ok-loading ok-stale @transaction",
     0,NULL,0,0,0,0,0,0},

    {"discard",discardCommand,1,
     "no-script fast ok-loading ok-stale @transaction",
     0,NULL,0,0,0,0,0,0},

    {"sync",syncCommand,1,
     "admin no-script @replication",
     0,NULL,0,0,0,0,0,0},

    {"psync",syncCommand,-3,
     "admin no-script @replication",
     0,NULL,0,0,0,0,0,0},

    {"replconf",replconfCommand,-1,
     "admin no-script ok-loading ok-stale @replication",
     0,NULL,0,0,0,0,0,0},

    {"flushdb",flushdbCommand,-1,
     "write @keyspace @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"flushall",flushallCommand,-1,
     "write @keyspace @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"sort",sortCommand,-2,
     "write use-memory @list @set @sortedset @dangerous",
     0,sortGetKeys,1,1,1,0,0,0},

    {"info",infoCommand,-1,
     "ok-loading ok-stale random @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"monitor",monitorCommand,1,
     "admin no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"ttl",ttlCommand,-2,
     "read-only fast random @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"touch",touchCommand,-2,
     "read-only fast @keyspace",
     0,NULL,1,-1,1,0,0,0},

    {"pttl",pttlCommand,-2,
     "read-only fast random @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"persist",persistCommand,-2,
     "write fast @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"slaveof",replicaofCommand,3,
     "admin no-script ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"replicaof",replicaofCommand,-3,
     "admin no-script ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"role",roleCommand,1,
     "ok-loading ok-stale no-script fast @dangerous",
     0,NULL,0,0,0,0,0,0},

    {"debug",debugCommand,-2,
     "admin no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"config",configCommand,-2,
     "admin ok-loading ok-stale no-script",
     0,NULL,0,0,0,0,0,0},

    {"subscribe",subscribeCommand,-2,
     "pub-sub no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"unsubscribe",unsubscribeCommand,-1,
     "pub-sub no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"psubscribe",psubscribeCommand,-2,
     "pub-sub no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"punsubscribe",punsubscribeCommand,-1,
     "pub-sub no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"publish",publishCommand,3,
     "pub-sub ok-loading ok-stale fast may-replicate",
     0,NULL,0,0,0,0,0,0},

    {"pubsub",pubsubCommand,-2,
     "pub-sub ok-loading ok-stale random",
     0,NULL,0,0,0,0,0,0},

    {"watch",watchCommand,-2,
     "no-script fast ok-loading ok-stale @transaction",
     0,NULL,1,-1,1,0,0,0},

    {"unwatch",unwatchCommand,1,
     "no-script fast ok-loading ok-stale @transaction",
     0,NULL,0,0,0,0,0,0},

    {"cluster",clusterCommand,-2,
     "admin ok-stale random",
     0,NULL,0,0,0,0,0,0},

    {"restore",restoreCommand,-4,
     "write use-memory @keyspace @dangerous",
     0,NULL,1,1,1,0,0,0},

    {"restore-asking",restoreCommand,-4,
    "write use-memory cluster-asking @keyspace @dangerous",
    0,NULL,1,1,1,0,0,0},

    {"migrate",migrateCommand,-6,
     "write random @keyspace @dangerous",
     0,migrateGetKeys,3,3,1,0,0,0},

    {"asking",askingCommand,1,
     "fast @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"readonly",readonlyCommand,1,
     "fast @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"readwrite",readwriteCommand,1,
     "fast @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"dump",dumpCommand,2,
     "read-only random @keyspace",
     0,NULL,1,1,1,0,0,0},

    {"object",objectCommand,-2,
     "read-only random @keyspace",
     0,NULL,2,2,1,0,0,0},

    {"memory",memoryCommand,-2,
     "random read-only",
     0,memoryGetKeys,0,0,0,0,0,0},

    {"client",clientCommand,-2,
     "admin no-script random ok-loading ok-stale @connection",
     0,NULL,0,0,0,0,0,0},

    {"hello",helloCommand,-1,
     "no-auth no-script fast ok-loading ok-stale @connection",
     0,NULL,0,0,0,0,0,0},

    /* EVAL can modify the dataset, however it is not flagged as a write
     * command since we do the check while running commands from Lua.
     * 
     * EVAL and EVALSHA also feed monitors before the commands are executed,
     * as opposed to after.
      */
    {"eval",evalCommand,-3,
     "no-script no-monitor may-replicate @scripting",
     0,evalGetKeys,0,0,0,0,0,0},

    {"evalsha",evalShaCommand,-3,
     "no-script no-monitor may-replicate @scripting",
     0,evalGetKeys,0,0,0,0,0,0},

    {"slowlog",slowlogCommand,-2,
     "admin random ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"script",scriptCommand,-2,
     "no-script may-replicate @scripting",
     0,NULL,0,0,0,0,0,0},

    {"time",timeCommand,1,
     "random fast ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"bitop",bitopCommand,-4,
     "write use-memory @bitmap",
     0,NULL,2,-1,1,0,0,0},

    {"bitcount",bitcountCommand,-2,
     "read-only @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"bitpos",bitposCommand,-3,
     "read-only @bitmap",
     0,NULL,1,1,1,0,0,0},

    {"wait",waitCommand,3,
     "no-script @keyspace",
     0,NULL,0,0,0,0,0,0},

    {"command",commandCommand,-1,
     "ok-loading ok-stale random @connection",
     0,NULL,0,0,0,0,0,0},

    {"geoadd",geoaddCommand,-5,
     "write use-memory @geo",
     0,NULL,1,1,1,0,0,0},

    /* GEORADIUS has store options that may write. */
    {"georadius",georadiusCommand,-6,
     "write use-memory @geo",
     0,georadiusGetKeys,1,1,1,0,0,0},

    {"georadius_ro",georadiusroCommand,-6,
     "read-only @geo",
     0,NULL,1,1,1,0,0,0},

    {"georadiusbymember",georadiusbymemberCommand,-5,
     "write use-memory @geo",
     0,georadiusGetKeys,1,1,1,0,0,0},

    {"georadiusbymember_ro",georadiusbymemberroCommand,-5,
     "read-only @geo",
     0,NULL,1,1,1,0,0,0},

    {"geohash",geohashCommand,-2,
     "read-only @geo",
     0,NULL,1,1,1,0,0,0},

    {"geopos",geoposCommand,-2,
     "read-only @geo",
     0,NULL,1,1,1,0,0,0},

    {"geodist",geodistCommand,-4,
     "read-only @geo",
     0,NULL,1,1,1,0,0,0},

    {"geosearch",geosearchCommand,-7,
     "read-only @geo",
      0,NULL,1,1,1,0,0,0},

    {"geosearchstore",geosearchstoreCommand,-8,
     "write use-memory @geo",
      0,NULL,1,2,1,0,0,0},

    {"pfselftest",pfselftestCommand,1,
     "admin @hyperloglog",
      0,NULL,0,0,0,0,0,0},

    {"pfadd",pfaddCommand,-2,
     "write use-memory fast @hyperloglog",
     0,NULL,1,1,1,0,0,0},

    /* Technically speaking PFCOUNT may change the key since it changes the
     * final bytes in the HyperLogLog representation. However in this case
     * we claim that the representation, even if accessible, is an internal
     * affair, and the command is semantically read only. */
    {"pfcount",pfcountCommand,-2,
     "read-only may-replicate @hyperloglog",
     0,NULL,1,-1,1,0,0,0},

    {"pfmerge",pfmergeCommand,-2,
     "write use-memory @hyperloglog",
     0,NULL,1,-1,1,0,0,0},

    /* Unlike PFCOUNT that is considered as a read-only command (although
     * it changes a bit), PFDEBUG may change the entire key when converting
     * from sparse to dense representation */
    {"pfdebug",pfdebugCommand,-3,
     "admin write use-memory @hyperloglog",
     0,NULL,2,2,1,0,0,0},

    {"xadd",xaddCommand,-5,
     "write use-memory fast random @stream",
     0,NULL,1,1,1,0,0,0},

    {"xrange",xrangeCommand,-4,
     "read-only @stream",
     0,NULL,1,1,1,0,0,0},

    {"xrevrange",xrevrangeCommand,-4,
     "read-only @stream",
     0,NULL,1,1,1,0,0,0},

    {"xlen",xlenCommand,2,
     "read-only fast @stream",
     0,NULL,1,1,1,0,0,0},

    {"xread",xreadCommand,-4,
     "read-only @stream @blocking",
     0,xreadGetKeys,0,0,0,0,0,0},

    {"xreadgroup",xreadCommand,-7,
     "write @stream @blocking",
     0,xreadGetKeys,0,0,0,0,0,0},

    {"xgroup",xgroupCommand,-2,
     "write use-memory @stream",
     0,NULL,2,2,1,0,0,0},

    {"xsetid",xsetidCommand,3,
     "write use-memory fast @stream",
     0,NULL,1,1,1,0,0,0},

    {"xack",xackCommand,-4,
     "write fast random @stream",
     0,NULL,1,1,1,0,0,0},

    {"xpending",xpendingCommand,-3,
     "read-only random @stream",
     0,NULL,1,1,1,0,0,0},

    {"xclaim",xclaimCommand,-6,
     "write random fast @stream",
     0,NULL,1,1,1,0,0,0},

    {"xautoclaim",xautoclaimCommand,-6,
     "write random fast @stream",
     0,NULL,1,1,1,0,0,0},

    {"xinfo",xinfoCommand,-2,
     "read-only random @stream",
     0,NULL,2,2,1,0,0,0},

    {"xdel",xdelCommand,-3,
     "write fast @stream",
     0,NULL,1,1,1,0,0,0},

    {"xtrim",xtrimCommand,-4,
     "write random @stream",
     0,NULL,1,1,1,0,0,0},

    {"post",securityWarningCommand,-1,
     "ok-loading ok-stale read-only",
     0,NULL,0,0,0,0,0,0},

    {"host:",securityWarningCommand,-1,
     "ok-loading ok-stale read-only",
     0,NULL,0,0,0,0,0,0},

    {"latency",latencyCommand,-2,
     "admin no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"acl",aclCommand,-2,
     "admin no-script ok-loading ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"rreplay",replicaReplayCommand,-3,
     "read-only fast noprop ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"keydb.cron",cronCommand,-5,
     "write use-memory",
     0,NULL,1,1,1,0,0,0},

    {"keydb.hrename", hrenameCommand, 4,
     "write fast @hash",
     0,NULL,0,0,0,0,0,0},
    
    {"stralgo",stralgoCommand,-2,
     "read-only @string",
     0,lcsGetKeys,0,0,0,0,0,0},

    {"keydb.nhget",nhgetCommand,-2,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},
    
    {"keydb.nhset",nhsetCommand,-3,
     "read-only fast @hash",
     0,NULL,1,1,1,0,0,0},

    {"KEYDB.MVCCRESTORE",mvccrestoreCommand, 5,
     "write use-memory @keyspace @dangerous",
     0,NULL,1,1,1,0,0,0},

    {"reset",resetCommand,1,
     "no-script ok-stale ok-loading fast @connection",
     0,NULL,0,0,0,0,0,0},

    {"failover",failoverCommand,-1,
     "admin no-script ok-stale",
     0,NULL,0,0,0,0,0,0},

    {"lfence", lfenceCommand,1,
     "read-only random ok-stale",
     0,NULL,0,0,0,0,0,0}
};

/*============================ 实用函数 ============================ */

/* 我们使用一个 fork 安全的私有 localtime 实现。
 * Redis 的日志记录函数可能会从其他线程调用。 */
extern "C" void nolocks_localtime(struct tm *tmp, time_t t, time_t tz, int dst);
extern "C" pid_t gettid();

void processClients();

/* 底层日志记录。仅用于非常大的消息，否则
 * 优先使用 serverLog()。 */
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
__attribute__((no_sanitize("thread")))
#  endif
#endif
void serverLogRaw(int level, const char *msg) {
    const int syslogLevelMap[] = { LOG_DEBUG, LOG_INFO, LOG_NOTICE, LOG_WARNING };
    const char *c = ".-*#                                                             ";
    FILE *fp;
    char buf[64];
    int rawmode = (level & LL_RAW);
    int log_to_stdout = g_pserver->logfile[0] == '\0';

    level &= 0xff; /* clear flags */
    if (level < cserver.verbosity) return;

    fp = log_to_stdout ? stdout : fopen(g_pserver->logfile,"a");
    if (!fp) return;

    if (rawmode) {
        fprintf(fp,"%s",msg);
    } else {
        int off;
        struct timeval tv;
        int role_char;
        pid_t pid = getpid();

        gettimeofday(&tv,NULL);
        struct tm tm;
        int daylight_active;
        __atomic_load(&g_pserver->daylight_active, &daylight_active, __ATOMIC_RELAXED);
        nolocks_localtime(&tm,tv.tv_sec,g_pserver->timezone,daylight_active);
        off = strftime(buf,sizeof(buf),"%d %b %Y %H:%M:%S.",&tm);
        snprintf(buf+off,sizeof(buf)-off,"%03d",(int)tv.tv_usec/1000);
        if (g_pserver->sentinel_mode) {
            role_char = 'X'; /* Sentinel. */
        } else if (pid != cserver.pid) {
            role_char = 'C'; /* RDB / AOF writing child. */
        } else {
            role_char = (listLength(g_pserver->masters) ? 'S':'M'); /* Slave or Master. */
        }
        fprintf(fp,"%d:%d:%c %s %c %s\n",
            (int)getpid(),(int)gettid(),role_char, buf,c[level],msg);
    }
    fflush(fp);

    if (!log_to_stdout) fclose(fp);
    if (g_pserver->syslog_enabled) syslog(syslogLevelMap[level], "%s", msg);
}

/* 类似于 serverLogRaw() 但支持类 printf 功能。这是在整个代码中使用的函数。
 * 原始版本仅用于在崩溃时转储 INFO 输出。 */
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
__attribute__((no_sanitize("thread")))
#  endif
#endif
void _serverLog(int level, const char *fmt, ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    serverLogRaw(level,msg);
}

/* 以信号处理程序安全的方式记录固定消息，不支持类 printf 功能。
 *
 * 我们实际上仅将其用于从 Redis 的角度来看不是致命的信号。
 * 无论如何都会终止服务器并且我们需要类 printf 功能的信号由 serverLog() 处理。 */
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
__attribute__((no_sanitize("thread")))
#  endif
#endif
void serverLogFromHandler(int level, const char *msg) {
    int fd;
    int log_to_stdout = g_pserver->logfile[0] == '\0';
    char buf[64];

    if ((level&0xff) < cserver.verbosity || (log_to_stdout && cserver.daemonize))
        return;
    fd = log_to_stdout ? STDOUT_FILENO :
                         open(g_pserver->logfile, O_APPEND|O_CREAT|O_WRONLY, 0644);
    if (fd == -1) return;
    ll2string(buf,sizeof(buf),getpid());
    if (write(fd,buf,strlen(buf)) == -1) goto err;
    if (write(fd,":signal-handler (",17) == -1) goto err;
    ll2string(buf,sizeof(buf),time(NULL));
    if (write(fd,buf,strlen(buf)) == -1) goto err;
    if (write(fd,") ",2) == -1) goto err;
    if (write(fd,msg,strlen(msg)) == -1) goto err;
    if (write(fd,"\n",1) == -1) goto err;
err:
    if (!log_to_stdout) close(fd);
}

/* 返回 UNIX 时间（微秒） */
long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}

/* 返回 UNIX 时间（毫秒） */
mstime_t mstime(void) {
    return ustime()/1000;
}

/* RDB 转储或 AOF 重写后，我们使用 _exit() 而不是 exit() 从子进程退出，
 * 因为后者可能与父进程使用的相同文件对象进行交互。
 * 但是，如果我们正在测试覆盖率，则使用正常的 exit() 以获取正确的覆盖率信息。 */
void exitFromChild(int retcode) {
#ifdef COVERAGE_TEST
    exit(retcode);
#else
    _exit(retcode);
#endif
}

/*====================== 哈希表类型实现  ==================== */

/* 这是一种哈希表类型，它使用 SDS 动态字符串库作为键，
 * 并使用 redis 对象作为值（对象可以容纳 SDS 字符串、列表、集合）。 */

void dictVanillaFree(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    zfree(val);
}

void dictListDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    listRelease((list*)val);
}

int dictSdsKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    int l1,l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

void dictSdsNOPDestructor(void *, void *) {}

void dictDbKeyDestructor(void *privdata, void *key)
{
    DICT_NOTUSED(privdata);
    sdsfree((sds)key);
}

/* A case insensitive version used for the command lookup table and other
 * places where case insensitive non binary-safe comparison is needed. */
int dictSdsKeyCaseCompare(void *privdata, const void *key1,
        const void *key2)
{
    DICT_NOTUSED(privdata);

    return strcasecmp((const char*)key1, (const char*)key2) == 0;
}

void dictObjectDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    if (val == NULL) return; /* Lazy freeing will set value to NULL. */
    decrRefCount((robj*)val);
}

void dictSdsDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    sdsfree((sds)val);
}

int dictObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    const robj *o1 = (const robj*)key1, *o2 = (const robj*)key2;
    return dictSdsKeyCompare(privdata,ptrFromObj(o1),ptrFromObj(o2));
}

uint64_t dictObjHash(const void *key) {
    const robj *o = (const robj*)key;
    void *ptr = ptrFromObj(o);
    return dictGenHashFunction(ptr, sdslen((sds)ptr));
}

uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

uint64_t dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char*)key, sdslen((char*)key));
}

int dictEncObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    robj *o1 = (robj*) key1, *o2 = (robj*) key2;
    int cmp;

    if (o1->encoding == OBJ_ENCODING_INT &&
        o2->encoding == OBJ_ENCODING_INT)
            return ptrFromObj(o1) == ptrFromObj(o2);

    /* Due to OBJ_STATIC_REFCOUNT, we avoid calling getDecodedObject() without
     * good reasons, because it would incrRefCount() the object, which
     * is invalid. So we check to make sure dictFind() works with static
     * objects as well. */
    if (o1->getrefcount() != OBJ_STATIC_REFCOUNT) o1 = getDecodedObject(o1);
    if (o2->getrefcount() != OBJ_STATIC_REFCOUNT) o2 = getDecodedObject(o2);
    cmp = dictSdsKeyCompare(privdata,ptrFromObj(o1),ptrFromObj(o2));
    if (o1->getrefcount() != OBJ_STATIC_REFCOUNT) decrRefCount(o1);
    if (o2->getrefcount() != OBJ_STATIC_REFCOUNT) decrRefCount(o2);
    return cmp;
}

uint64_t dictEncObjHash(const void *key) {
    robj *o = (robj*) key;

    if (sdsEncodedObject(o)) {
        return dictGenHashFunction(ptrFromObj(o), sdslen((sds)ptrFromObj(o)));
    } else if (o->encoding == OBJ_ENCODING_INT) {
        char buf[32];
        int len;

        len = ll2string(buf,32,(long)ptrFromObj(o));
        return dictGenHashFunction((unsigned char*)buf, len);
    } else {
        serverPanic("Unknown string encoding");
    }
}

/* Return 1 if currently we allow dict to expand. Dict may allocate huge
 * memory to contain hash buckets when dict expands, that may lead redis
 * rejects user's requests or evicts some keys, we can stop dict to expand
 * provisionally if used memory will be over maxmemory after dict expands,
 * but to guarantee the performance of redis, we still allow dict to expand
 * if dict load factor exceeds HASHTABLE_MAX_LOAD_FACTOR. */
int dictExpandAllowed(size_t moreMem, double usedRatio) {
    if (usedRatio <= HASHTABLE_MAX_LOAD_FACTOR) {
        return !overMaxmemoryAfterAlloc(moreMem);
    } else {
        return 1;
    }
}

void dictGCAsyncFree(dictAsyncRehashCtl *async);

/* 通用哈希表类型，其中键是 Redis 对象，值是空指针。 */
dictType objectKeyPointerValueDictType = {
    dictEncObjHash,            /* 哈希函数 */
    NULL,                      /* 键复制 */
    NULL,                      /* 值复制 */
    dictEncObjKeyCompare,      /* 键比较 */
    dictObjectDestructor,      /* 键析构器 */
    NULL,                      /* 值析构器 */
    NULL                       /* 允许扩展 */
};

/* 类似于 objectKeyPointerValueDictType()，但如果值不为 NULL，则可以通过调用 zfree() 来销毁。 */
dictType objectKeyHeapPointerValueDictType = {
    dictEncObjHash,            /* 哈希函数 */
    NULL,                      /* 键复制 */
    NULL,                      /* 值复制 */
    dictEncObjKeyCompare,      /* 键比较 */
    dictObjectDestructor,      /* 键析构器 */
    dictVanillaFree,           /* 值析构器 */
    NULL                       /* 允许扩展 */
};

/* 集合字典类型。键是 SDS 字符串，值未使用。 */
dictType setDictType = {
    dictSdsHash,               /* 哈希函数 */
    NULL,                      /* 键复制 */
    NULL,                      /* 值复制 */
    dictSdsKeyCompare,         /* 键比较 */
    dictSdsDestructor,         /* 键析构器 */
    NULL                       /* 值析构器 */
};

/* 有序集合哈希（注意：除了哈希表之外，还使用了跳表） */
dictType zsetDictType = {
    dictSdsHash,               /* 哈希函数 */
    NULL,                      /* 键复制 */
    NULL,                      /* 值复制 */
    dictSdsKeyCompare,         /* 键比较 */
    NULL,                      /* 注意：SDS 字符串由跳表共享和释放 */
    NULL,                      /* 值析构器 */
    NULL                       /* 允许扩展 */
};

/* db->dict，键是 sds 字符串，值是 Redis 对象。 */
dictType dbDictType = {
    dictSdsHash,                /* 哈希函数 */
    NULL,                       /* 键复制 */
    NULL,                       /* 值复制 */
    dictSdsKeyCompare,          /* 键比较 */
    dictDbKeyDestructor,        /* 键析构器 */
    dictObjectDestructor,       /* 值析构器 */
    dictExpandAllowed,           /* 允许扩展 */
    dictGCAsyncFree             /* 异步释放析构器 */
};

dictType dbExpiresDictType = {
        dictSdsHash,                /* 哈希函数 */
        NULL,                       /* 键复制 */
        NULL,                       /* 值复制 */
        dictSdsKeyCompare,          /* 键比较 */
        NULL,                       /* 键析构器 */
        NULL,                       /* 值析构器 */
        dictExpandAllowed           /* 允许扩展 */
    };

/* db->pdict，键是 sds 字符串，值是 Redis 对象。 */
dictType dbTombstoneDictType = {
    dictSdsHash,                /* 哈希函数 */
    NULL,                       /* 键复制 */
    NULL,                       /* 值复制 */
    dictSdsKeyCompare,          /* 键比较 */
    dictDbKeyDestructor,        /* 键析构器 */
    NULL,                       /* 值析构器 */
    dictExpandAllowed           /* 允许扩展 */
};

dictType dbSnapshotDictType = {
    dictSdsHash,
    NULL,
    NULL,
    dictSdsKeyCompare,
    dictSdsNOPDestructor,
    dictObjectDestructor,
    dictExpandAllowed           /* 允许扩展 */
};

/* g_pserver->lua_scripts sha（作为 sds 字符串）-> scripts（作为 robj）缓存。 */
dictType shaScriptObjectDictType = {
    dictSdsCaseHash,            /* 哈希函数 */
    NULL,                       /* 键复制 */
    NULL,                       /* 值复制 */
    dictSdsKeyCaseCompare,      /* 键比较 */
    dictSdsDestructor,          /* 键析构器 */
    dictObjectDestructor,       /* 值析构器 */
    NULL                        /* 允许扩展 */
};

/* 命令表。sds 字符串 -> 命令结构体指针。 */
dictType commandTableDictType = {
    dictSdsCaseHash,            /* 哈希函数 */
    NULL,                       /* 键复制 */
    NULL,                       /* 值复制 */
    dictSdsKeyCaseCompare,      /* 键比较 */
    dictSdsDestructor,          /* 键析构器 */
    NULL,                       /* 值析构器 */
    NULL                        /* 允许扩展 */
};

/* 哈希类型哈希表（注意，小哈希用压缩列表表示） */
dictType hashDictType = {
    dictSdsHash,                /* 哈希函数 */
    NULL,                       /* 键复制 */
    NULL,                       /* 值复制 */
    dictSdsKeyCompare,          /* 键比较 */
    dictSdsDestructor,          /* 键析构器 */
    dictSdsDestructor,          /* 值析构器 */
    NULL                        /* 允许扩展 */
};

/* 没有析构器的 Dict 类型 */
dictType sdsReplyDictType = {
    dictSdsHash,                /* 哈希函数 */
    NULL,                       /* 键复制 */
    NULL,                       /* 值复制 */
    dictSdsKeyCompare,          /* 键比较 */
    NULL,                       /* 键析构器 */
    NULL,                       /* 值析构器 */
    NULL                        /* 允许扩展 */
};

/* 键列表哈希表类型以未编码的 redis 对象作为键，以列表作为值。
 * 它用于阻塞操作 (BLPOP) 以及将交换的键映射到等待加载这些键的客户端列表。 */
dictType keylistDictType = {
    dictObjHash,                /* 哈希函数 */
    NULL,                       /* 键复制 */
    NULL,                       /* 值复制 */
    dictObjKeyCompare,          /* 键比较 */
    dictObjectDestructor,       /* 键析构器 */
    dictListDestructor,         /* 值析构器 */
    NULL                        /* 允许扩展 */
};

/* 集群节点哈希表，将节点地址 1.2.3.4:6379 映射到 clusterNode 结构。 */
dictType clusterNodesDictType = {
    dictSdsHash,                /* 哈希函数 */
    NULL,                       /* 键复制 */
    NULL,                       /* 值复制 */
    dictSdsKeyCompare,          /* 键比较 */
    dictSdsDestructor,          /* 键析构器 */
    NULL,                       /* 值析构器 */
    NULL                        /* 允许扩展 */
};

/* 集群重新添加黑名单。这将节点 ID 映射到我们可以重新添加此节点的时间。
 * 目的是避免在一段时间内重新添加已删除的节点。 */
dictType clusterNodesBlackListDictType = {
    dictSdsCaseHash,            /* 哈希函数 */
    NULL,                       /* 键复制 */
    NULL,                       /* 值复制 */
    dictSdsKeyCaseCompare,      /* 键比较 */
    dictSdsDestructor,          /* 键析构器 */
    NULL,                       /* 值析构器 */
    NULL                        /* 允许扩展 */
};

/* 模块系统字典类型。键是模块名称，值是指向 RedisModule 结构的指针。 */
dictType modulesDictType = {
    dictSdsCaseHash,            /* 哈希函数 */
    NULL,                       /* 键复制 */
    NULL,                       /* 值复制 */
    dictSdsKeyCaseCompare,      /* 键比较 */
    dictSdsDestructor,          /* 键析构器 */
    NULL,                       /* 值析构器 */
    NULL                        /* 允许扩展 */
};

/* 迁移缓存 dict 类型。 */
dictType migrateCacheDictType = {
    dictSdsHash,                /* 哈希函数 */
    NULL,                       /* 键复制 */
    NULL,                       /* 值复制 */
    dictSdsKeyCompare,          /* 键比较 */
    dictSdsDestructor,          /* 键析构器 */
    NULL,                       /* 值析构器 */
    NULL                        /* 允许扩展 */
};

/* 复制缓存脚本 dict (g_pserver->repl_scriptcache_dict)。
 * 键是 sds SHA1 字符串，而值在当前实现中根本未使用。 */
dictType replScriptCacheDictType = {
    dictSdsCaseHash,            /* 哈希函数 */
    NULL,                       /* 键复制 */
    NULL,                       /* 值复制 */
    dictSdsKeyCaseCompare,      /* 键比较 */
    dictSdsDestructor,          /* 键析构器 */
    NULL,                       /* 值析构器 */
    NULL                        /* 允许扩展 */
};

int htNeedsResize(dict *dict) {
    long long size, used;

    size = dictSlots(dict);
    used = dictSize(dict);
    return (size > DICT_HT_INITIAL_SIZE &&
            (used*100/size < HASHTABLE_MIN_FILL));
}

/* If the percentage of used slots in the HT reaches HASHTABLE_MIN_FILL
 * we resize the hash table to save memory */
void tryResizeHashTables(int dbid) {
    g_pserver->db[dbid]->tryResize();
}

/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use 1 millisecond
 * of CPU time at every call of this function to perform some rehashing.
 *
 * The function returns the number of rehashes if some rehashing was performed, otherwise 0
 * is returned. */
int redisDbPersistentData::incrementallyRehash() {
    /* Keys dictionary */
    int result = 0;
    if (dictIsRehashing(m_pdict))
        result += dictRehashMilliseconds(m_pdict,1);
    if (dictIsRehashing(m_pdictTombstone))
        dictRehashMilliseconds(m_pdictTombstone,1); // don't count this
    return result; /* already used our millisecond for this loop... */
}

/* This function is called once a background process of some kind terminates,
 * as we want to avoid resizing the hash tables when there is a child in order
 * to play well with copy-on-write (otherwise when a resize happens lots of
 * memory pages are copied). The goal of this function is to update the ability
 * for dict.c to resize the hash tables accordingly to the fact we have an
 * active fork child running. */
void updateDictResizePolicy(void) {
    if (!hasActiveChildProcess())
        dictEnableResize();
    else
        dictDisableResize();
}

const char *strChildType(int type) {
    switch(type) {
        case CHILD_TYPE_RDB: return "RDB";
        case CHILD_TYPE_AOF: return "AOF";
        case CHILD_TYPE_LDB: return "LDB";
        case CHILD_TYPE_MODULE: return "MODULE";
        default: return "Unknown";
    }
}

/* Return true if there are active children processes doing RDB saving,
 * AOF rewriting, or some side process spawned by a loaded module. */
int hasActiveChildProcess() {
    return g_pserver->child_pid != -1;
}

int hasActiveChildProcessOrBGSave() {
    return g_pserver->FRdbSaveInProgress() || hasActiveChildProcess();
}

void resetChildState() {
    g_pserver->child_type = CHILD_TYPE_NONE;
    g_pserver->child_pid = -1;
    g_pserver->stat_current_cow_bytes = 0;
    g_pserver->stat_current_cow_updated = 0;
    g_pserver->stat_current_save_keys_processed = 0;
    g_pserver->stat_module_progress = 0;
    g_pserver->stat_current_save_keys_total = 0;
    updateDictResizePolicy();
    closeChildInfoPipe();
    moduleFireServerEvent(REDISMODULE_EVENT_FORK_CHILD,
                          REDISMODULE_SUBEVENT_FORK_CHILD_DIED,
                          NULL);
}

/* Return if child type is mutual exclusive with other fork children */
int isMutuallyExclusiveChildType(int type) {
    return type == CHILD_TYPE_RDB || type == CHILD_TYPE_AOF || type == CHILD_TYPE_MODULE;
}

/* Return true if this instance has persistence completely turned off:
 * both RDB and AOF are disabled. */
int allPersistenceDisabled(void) {
    return g_pserver->saveparamslen == 0 && g_pserver->aof_state == AOF_OFF;
}

/* ======================= Cron: called every 100 ms ======================== */

/* Add a sample to the operations per second array of samples. */
void trackInstantaneousMetric(int metric, long long current_reading) {
    long long now = mstime();
    long long t = now - g_pserver->inst_metric[metric].last_sample_time;
    long long ops = current_reading -
                    g_pserver->inst_metric[metric].last_sample_count;
    long long ops_sec;

    ops_sec = t > 0 ? (ops*1000/t) : 0;

    g_pserver->inst_metric[metric].samples[g_pserver->inst_metric[metric].idx] =
        ops_sec;
    g_pserver->inst_metric[metric].idx++;
    g_pserver->inst_metric[metric].idx %= STATS_METRIC_SAMPLES;
    g_pserver->inst_metric[metric].last_sample_time = now;
    g_pserver->inst_metric[metric].last_sample_count = current_reading;
}

/* Return the mean of all the samples. */
long long getInstantaneousMetric(int metric) {
    int j;
    long long sum = 0;

    for (j = 0; j < STATS_METRIC_SAMPLES; j++)
        sum += g_pserver->inst_metric[metric].samples[j];
    return sum / STATS_METRIC_SAMPLES;
}

/* 客户端查询缓冲区是一个 sds.c 字符串，其末尾可能有很多未使用的空闲空间，
 * 此函数在需要时回收空间。
 *
 * 该函数始终返回 0，因为它从不终止客户端。 */
int clientsCronResizeQueryBuffer(client *c) {
    AssertCorrectThread(c);
    size_t querybuf_size = sdsAllocSize(c->querybuf);
    time_t idletime = g_pserver->unixtime - c->lastinteraction;

    /* 调整查询缓冲区大小有两个条件：
     * 1) 查询缓冲区 > BIG_ARG 并且对于最新的峰值来说太大了。
     * 2) 查询缓冲区 > BIG_ARG 并且客户端空闲。 */
    if (querybuf_size > PROTO_MBULK_BIG_ARG &&
         ((querybuf_size/(c->querybuf_peak+1)) > 2 ||
          idletime > 2))
    {
        /* 仅当查询缓冲区实际浪费至少几千字节时才调整其大小。 */
        if (sdsavail(c->querybuf) > 1024*4) {
            c->querybuf = sdsRemoveFreeSpace(c->querybuf);
        }
    }
    /* 再次重置峰值以捕获下一个周期中的峰值内存使用情况。 */
    c->querybuf_peak = 0;

    /* 代表主节点的客户端也使用一个“待处理查询缓冲区”，
     * 它是我们正在读取的流中尚未应用的部分。这样的缓冲区
     * 也需要不时调整大小，否则在非常大的传输（一个巨大的值或一个大的 MIGRATE 操作）之后，
     * 它将继续使用大量内存。 */
    if (c->flags & CLIENT_MASTER) {
        /* 调整待处理查询缓冲区大小有两个条件：
         * 1) 待处理查询缓冲区 > LIMIT_PENDING_QUERYBUF。
         * 2) 已用长度小于 pending_querybuf_size/2 */
        size_t pending_querybuf_size = sdsAllocSize(c->pending_querybuf);
        if(pending_querybuf_size > LIMIT_PENDING_QUERYBUF &&
           sdslen(c->pending_querybuf) < (pending_querybuf_size/2))
        {
            c->pending_querybuf = sdsRemoveFreeSpace(c->pending_querybuf);
        }
    }
    return 0;
}

SymVer parseVersion(const char *version)
{
    SymVer ver = {-1,-1,-1};
    long versions[3] = {-1,-1,-1};
    const char *start = version;
    const char *end = nullptr;

    for (int iver = 0; iver < 3; ++iver)
    {
        end = start;
        while (*end != '\0' && *end != '.')
            ++end;

        if (start >= end)
            return ver;

        if (!string2l(start, end - start, versions + iver))
            return ver;
        if (*end != '\0')
            start = end+1;
        else
            break;
    }
    ver.major = versions[0];
    ver.minor = versions[1];
    ver.build = versions[2];
    
    return ver;
}

VersionCompareResult compareVersion(SymVer *pver)
{
    SymVer symVerThis = parseVersion(KEYDB_REAL_VERSION);
    // 特殊情况，0.0.0 等于任何版本
    if ((symVerThis.major == 0 && symVerThis.minor == 0 && symVerThis.build == 0)
        || (pver->major == 0 && pver->minor == 0 && pver->build == 0))
        return VersionCompareResult::EqualVersion;

    if (pver->major <= 6 && pver->minor <= 3 && pver->build <= 3)
        return VersionCompareResult::IncompatibleVersion;
    
    for (int iver = 0; iver < 3; ++iver)
    {
        long verThis, verOther;
        switch (iver)
        {
        case 0:
            verThis = symVerThis.major; verOther = pver->major;
            break;
        case 1:
            verThis = symVerThis.minor; verOther = pver->minor;
            break;
        case 2:
            verThis = symVerThis.build; verOther = pver->build;
        }
        
        if (verThis < verOther)
            return VersionCompareResult::NewerVersion;
        if (verThis > verOther)
            return VersionCompareResult::OlderVersion;
    }
    return VersionCompareResult::EqualVersion;
}

/* 此函数用于跟踪在最近几秒钟内使用最大内存量的客户端。
 * 这样我们就可以在 INFO 输出（客户端部分）中提供此类信息，
 * 而不必对所有客户端进行 O(N) 扫描。
 *
 * 它是这样工作的。我们有一个包含 CLIENTS_PEAK_MEM_USAGE_SLOTS 个槽的数组，
 * 我们在其中跟踪每个槽中观察到的最大客户端输出和输入缓冲区。
 * 每个槽对应于最近几秒钟中的一秒，因为该数组通过执行 UNIXTIME % CLIENTS_PEAK_MEM_USAGE_SLOTS 来索引。
 *
 * 当我们想知道最近的峰值内存使用情况时，我们只需扫描这几个槽以查找最大值。 */
#define CLIENTS_PEAK_MEM_USAGE_SLOTS 8
size_t ClientsPeakMemInput[CLIENTS_PEAK_MEM_USAGE_SLOTS] = {0};
size_t ClientsPeakMemOutput[CLIENTS_PEAK_MEM_USAGE_SLOTS] = {0};

int clientsCronTrackExpansiveClients(client *c, int time_idx) {
    size_t in_usage = sdsZmallocSize(c->querybuf) + c->argv_len_sum() +
	              (c->argv ? zmalloc_size(c->argv) : 0);
    size_t out_usage = getClientOutputBufferMemoryUsage(c);

    /* 跟踪此槽中迄今为止观察到的最大值。 */
    if (in_usage > ClientsPeakMemInput[time_idx]) ClientsPeakMemInput[time_idx] = in_usage;
    if (out_usage > ClientsPeakMemOutput[time_idx]) ClientsPeakMemOutput[time_idx] = out_usage;

    return 0; /* 此函数从不终止客户端。 */
}

/* 在 getMemoryOverheadData() 中迭代所有客户端速度太慢，
 * 反过来又会使 INFO 命令速度太慢。因此，我们增量执行此计算，
 * 并以更增量的方式（取决于 g_pserver->hz）使用 clinetsCron()
 * 跟踪客户端使用的（非瞬时但更新到秒的）总内存。 */
int clientsCronTrackClientsMemUsage(client *c) {
    size_t mem = 0;
    int type = getClientType(c);
    mem += getClientOutputBufferMemoryUsage(c);
    mem += sdsZmallocSize(c->querybuf);
    mem += zmalloc_size(c);
    mem += c->argv_len_sum();
    if (c->argv) mem += zmalloc_size(c->argv);
    /* 现在我们有了客户端使用的内存，从旧类别中删除旧值，然后将其添加回来。 */
    g_pserver->stat_clients_type_memory[c->client_cron_last_memory_type] -=
        c->client_cron_last_memory_usage;
    g_pserver->stat_clients_type_memory[type] += mem;
    /* 记住我们添加了什么以及在哪里添加的，以便下次删除它。 */
    c->client_cron_last_memory_usage = mem;
    c->client_cron_last_memory_type = type;
    return 0;
}

/* 返回由 clientsCronTrackExpansiveClients() 函数跟踪的客户端内存使用情况中的最大样本。 */
void getExpansiveClientsInfo(size_t *in_usage, size_t *out_usage) {
    size_t i = 0, o = 0;
    for (int j = 0; j < CLIENTS_PEAK_MEM_USAGE_SLOTS; j++) {
        if (ClientsPeakMemInput[j] > i) i = ClientsPeakMemInput[j];
        if (ClientsPeakMemOutput[j] > o) o = ClientsPeakMemOutput[j];
    }
    *in_usage = i;
    *out_usage = o;
}

int closeClientOnOverload(client *c) {
    if (g_pserver->overload_closed_clients > MAX_CLIENTS_SHED_PER_PERIOD) return false;
    if (!g_pserver->is_overloaded) return false;
    // 不要关闭主节点、副本或发布/订阅客户端
    if (c->flags & (CLIENT_MASTER | CLIENT_SLAVE | CLIENT_PENDING_WRITE | CLIENT_PUBSUB | CLIENT_BLOCKED)) return false;
    freeClient(c);
    ++g_pserver->overload_closed_clients;
    return true;
}

/* 此函数由 serverCron() 调用，用于对客户端执行重要的持续操作。
 * 例如，我们使用此函数在超时后断开客户端连接，
 * 包括在某些具有非零超时的阻塞命令中阻塞的客户端。
 *
 * 该函数会尽力每秒处理所有客户端，即使这不能严格保证，
 * 因为在发生诸如慢速命令之类的延迟事件时，serverCron()
 * 的实际调用频率可能低于 g_pserver->hz。
 *
 * 对于此函数及其调用的函数来说，速度非常快非常重要：
 * 有时 Redis 有成百上千个连接的客户端，而默认的 g_pserver->hz 值为 10，
 * 因此有时我们在这里需要每秒处理数千个客户端，从而使此函数成为延迟的来源。
 */
#define CLIENTS_CRON_MIN_ITERATIONS 5
void clientsCron(int iel) {
    /* 尝试每次调用至少处理 numclients/g_pserver->hz 个客户端。
     * 由于通常（如果没有大的延迟事件）此函数每秒调用 g_pserver->hz 次，
     *因此在平均情况下，我们会在 1 秒内处理所有客户端。 */
    int numclients = listLength(g_pserver->clients);
    int iterations = numclients/g_pserver->hz;
    mstime_t now = mstime();

    /* 在我们处理的时候，至少处理一些客户端，即使我们需要处理少于 CLIENTS_CRON_MIN_ITERATIONS
     * 个客户端才能满足我们每秒处理每个客户端一次的约定。 */
    if (iterations < CLIENTS_CRON_MIN_ITERATIONS)
        iterations = (numclients < CLIENTS_CRON_MIN_ITERATIONS) ?
                     numclients : CLIENTS_CRON_MIN_ITERATIONS;


    int curr_peak_mem_usage_slot = g_pserver->unixtime % CLIENTS_PEAK_MEM_USAGE_SLOTS;
    /* 始终将下一个样本归零，这样当我们切换到那一秒时，
     * 我们将只记录在该秒中较大的样本，而不考虑该槽的历史记录。
     *
     * 注意：如果由于某种原因 serverCron() 未以正常频率调用，
     * 例如由于某些慢速命令需要几秒钟才能执行，则我们的索引可能会跳转到任何随机位置。
     * 在这种情况下，我们的数组最终可能包含可能比 CLIENTS_PEAK_MEM_USAGE_SLOTS 秒更早的数据：
     * 然而，这不是问题，因为我们在这里只想跟踪“最近”是否存在从内存使用角度来看非常昂贵的客户端。 */
    int zeroidx = (curr_peak_mem_usage_slot+1) % CLIENTS_PEAK_MEM_USAGE_SLOTS;
    ClientsPeakMemInput[zeroidx] = 0;
    ClientsPeakMemOutput[zeroidx] = 0;


    while(listLength(g_pserver->clients) && iterations--) {
        client *c;
        listNode *head;
        /* 旋转列表，获取当前头部，进行处理。
         * 这样，如果必须从列表中删除客户端，它就是第一个元素，
         * 我们不会产生 O(N) 计算。 */
        listRotateTailToHead(g_pserver->clients);
        head = (listNode*)listFirst(g_pserver->clients);
        c = (client*)listNodeValue(head);
        if (c->iel == iel)
        {
            fastlock_lock(&c->lock);
            /* 以下函数对客户端执行不同的服务检查。
            * 协议是如果客户端已终止，它们将返回非零值。 */
            if (clientsCronHandleTimeout(c,now)) continue;  // 客户端已释放，因此不要释放锁
            if (clientsCronResizeQueryBuffer(c)) goto LContinue;
            if (clientsCronTrackExpansiveClients(c, curr_peak_mem_usage_slot)) goto LContinue;
            if (clientsCronTrackClientsMemUsage(c)) goto LContinue;
            if (closeClientOnOutputBufferLimitReached(c, 0)) continue; // 客户端也已释放
            if (closeClientOnOverload(c)) continue;
        LContinue:
            fastlock_unlock(&c->lock);
        }
    }

    /* 释放所有待处理的客户端 */
    freeClientsInAsyncFreeQueue(iel);
}

bool expireOwnKeys()
{
    if (iAmMaster()) {
        return true;
    } else if (!g_pserver->fActiveReplica && (listLength(g_pserver->masters) == 1)) {
        redisMaster *mi = (redisMaster*)listNodeValue(listFirst(g_pserver->masters));
        if (mi->isActive)
            return true;
    }
    return false;
}

int hash_spin_worker() {
    auto ctl = serverTL->rehashCtl;
    return dictRehashSomeAsync(ctl, 1);
}

/* 此函数处理我们需要在 Redis 数据库中增量执行的“后台”操作，
 * 例如活动键过期、调整大小、重新哈希。 */
void databasesCron(bool fMainThread) {
    serverAssert(GlobalLocksAcquired());

    if (fMainThread) {
        /* 通过随机抽样使密钥过期。从服务器不需要，
        * 因为主服务器会为我们合成 DEL。 */
        if (g_pserver->active_expire_enabled) {
            if (expireOwnKeys()) {
                activeExpireCycle(ACTIVE_EXPIRE_CYCLE_SLOW);
            } else {
                expireSlaveKeys();
            }
        }

        /* 逐步整理密钥。 */
        activeDefragCycle();
    }

    /* 如果需要，执行哈希表重新哈希，但前提是没有其他进程将数据库保存在磁盘上。
     * 否则，重新哈希是不好的，因为它会导致大量内存页的写时复制。 */
    if (!hasActiveChildProcess()) {
        /* 我们使用全局计数器，因此如果我们在给定的数据库处停止计算，
         * 我们将能够从下一个 cron 循环迭代中的后续计数器开始。 */
        static unsigned int resize_db = 0;
        static unsigned int rehash_db = 0;
        static int rehashes_per_ms;
        static int async_rehashes;
        int dbs_per_call = CRON_DBS_PER_CALL;
        int j;

        /* 不要测试比我们拥有的数据库更多的数据库。 */
        if (dbs_per_call > cserver.dbnum) dbs_per_call = cserver.dbnum;

        if (fMainThread) {
            /* 调整大小 */
            for (j = 0; j < dbs_per_call; j++) {
                tryResizeHashTables(resize_db % cserver.dbnum);
                resize_db++;
            }
        }

        /* 重新哈希 */
        if (g_pserver->activerehashing) {
            for (j = 0; j < dbs_per_call; j++) {
                if (serverTL->rehashCtl != nullptr) {
                    if (!serverTL->rehashCtl->done.load(std::memory_order_relaxed)) {
                        aeReleaseLock();
                        if (dictRehashSomeAsync(serverTL->rehashCtl, rehashes_per_ms)) {
                            aeAcquireLock();
                            break;
                        }
                        aeAcquireLock();
                    }

                    if (serverTL->rehashCtl->done.load(std::memory_order_relaxed)) {
                        dictCompleteRehashAsync(serverTL->rehashCtl, true /*fFree*/);
                        serverTL->rehashCtl = nullptr;
                    }
                }

                serverAssert(serverTL->rehashCtl == nullptr);
                ::dict *dict = g_pserver->db[rehash_db]->dictUnsafeKeyOnly();
                /* 我们是否在异步重新哈希？如果是，是否是时候重新校准了？ */
                /* 重新校准限制是一个素数，以确保线程之间的平衡 */
                if (g_pserver->enable_async_rehash && rehashes_per_ms > 0 && async_rehashes < 131 && !cserver.active_defrag_enabled && cserver.cthreads > 1 && dictSize(dict) > 2048 && dictIsRehashing(dict) && !g_pserver->loading && aeLockContention() > 1) {
                    serverTL->rehashCtl = dictRehashAsyncStart(dict, rehashes_per_ms * ((1000 / g_pserver->hz) / 10));  // 估计 10% 的 CPU 时间用于锁争用
                    if (serverTL->rehashCtl)
                        ++async_rehashes;
                }
                if (serverTL->rehashCtl)
                    break;

                // 在开始任何新的操作之前，我们能否结束阻塞线程的重新哈希？
                while (dict->asyncdata != nullptr) {
                    auto asyncdata = dict->asyncdata;
                    if (asyncdata->done) {
                        dictCompleteRehashAsync(asyncdata, false /*fFree*/);    // 不要释放，因为我们不拥有指针
                        serverAssert(dict->asyncdata != asyncdata);
                    } else {
                        break;
                    }
                }

                if (dict->asyncdata)
                    break;

                rehashes_per_ms = g_pserver->db[rehash_db]->incrementallyRehash();
                async_rehashes = 0;
                if (rehashes_per_ms > 0) {
                    /* 如果函数做了一些工作，就在这里停止，我们将在下一个 cron 循环中做更多的工作。 */
                    if (!cserver.active_defrag_enabled) {
                        serverLog(LL_VERBOSE, "Calibrated rehashes per ms: %d", rehashes_per_ms);
                    }
                    break;
                } else if (dict->asyncdata == nullptr) {
                    /* 如果此数据库不需要重新哈希并且我们没有正在进行的重新哈希，我们将尝试下一个。 */
                    rehash_db++;
                    rehash_db %= cserver.dbnum;
                }
            }
        }
    }

    if (serverTL->rehashCtl) {
        setAeLockSetThreadSpinWorker(hash_spin_worker);
    } else {
        setAeLockSetThreadSpinWorker(nullptr);
    }
}

/* 我们在全局状态中获取 unix 时间的缓存值，因为使用虚拟内存和老化时，
 * 每次访问对象时都必须存储当前时间，并且不需要精度。
 * 访问全局变量比调用 time(NULL) 快得多。
 *
 * 此函数应该很快，因为它在 call() 中的每个命令执行时都会被调用，
 * 因此可以使用 'update_daylight_info' 参数来决定是否更新夏令时信息。
 * 通常我们仅在从 serverCron() 调用此函数时更新此类信息，
 * 而不是在从 call() 调用它时更新。 */
void updateCachedTime() {
    long long t = ustime();
    __atomic_store(&g_pserver->ustime, &t, __ATOMIC_RELAXED);
    t /= 1000;
    __atomic_store(&g_pserver->mstime, &t, __ATOMIC_RELAXED);
    t /= 1000;
    g_pserver->unixtime = t;

    /* 要获取有关夏令时的信息，我们需要调用 localtime_r 并缓存结果。
     * 但是，在此上下文中调用 localtime_r 是安全的，因为我们永远不会在主线程中 fork()。
     * 日志记录函数将调用一个线程安全的 localtime 版本，该版本没有锁。 */
    struct tm tm;
    time_t ut = g_pserver->unixtime;
    localtime_r(&ut,&tm);
    __atomic_store(&g_pserver->daylight_active, &tm.tm_isdst, __ATOMIC_RELAXED);
}

void checkChildrenDone(void) {
    int statloc = 0;
    pid_t pid;

    if (g_pserver->FRdbSaveInProgress() && !cserver.fForkBgSave)
    {
        void *rval = nullptr;
        int err = EAGAIN;
        if (!g_pserver->rdbThreadVars.fDone || (err = pthread_join(g_pserver->rdbThreadVars.rdb_child_thread, &rval)))
        {
            if (err != EBUSY && err != EAGAIN)
                serverLog(LL_WARNING, "Error joining the background RDB save thread: %s\n", strerror(errno));
        }
        else
        {
            int exitcode = (int)reinterpret_cast<ptrdiff_t>(rval);
            backgroundSaveDoneHandler(exitcode,g_pserver->rdbThreadVars.fRdbThreadCancel);
            g_pserver->rdbThreadVars.fRdbThreadCancel = false;
            g_pserver->rdbThreadVars.fDone = false;
            if (exitcode == 0) receiveChildInfo();
            closeChildInfoPipe();
        }
    }
    else if ((pid = waitpid(-1, &statloc, WNOHANG)) != 0) {
        int exitcode = WIFEXITED(statloc) ? WEXITSTATUS(statloc) : -1;
        int bysignal = 0;

        if (WIFSIGNALED(statloc)) bysignal = WTERMSIG(statloc);

        /* sigKillChildHandler 捕获信号并调用 exit()，但我们必须确保不要错误地标记 lastbgsave_status 等。
         * 我们可以直接通过 SIGUSR1 终止子进程而不处理它 */
        if (exitcode == SERVER_CHILD_NOERROR_RETVAL) {
            bysignal = SIGUSR1;
            exitcode = 1;
        }

        if (pid == -1) {
            serverLog(LL_WARNING,"waitpid() returned an error: %s. "
                "child_type: %s, child_pid = %d",
                strerror(errno),
                strChildType(g_pserver->child_type),
                (int) g_pserver->child_pid);
        } else if (pid == g_pserver->child_pid) {
            if (g_pserver->child_type == CHILD_TYPE_RDB) {
                backgroundSaveDoneHandler(exitcode, bysignal);
            } else if (g_pserver->child_type == CHILD_TYPE_AOF) {
                backgroundRewriteDoneHandler(exitcode, bysignal);
            } else if (g_pserver->child_type == CHILD_TYPE_MODULE) {
                ModuleForkDoneHandler(exitcode, bysignal);
            } else {
                serverPanic("Unknown child type %d for child pid %d", g_pserver->child_type, g_pserver->child_pid);
                exit(1);
            }
            if (!bysignal && exitcode == 0) receiveChildInfo();
            resetChildState();
        } else {
            if (!ldbRemoveChild(pid)) {
                serverLog(LL_WARNING,
                          "Warning, detected child with unmatched pid: %ld",
                          (long) pid);
            }
        }

        /* 立即启动任何挂起的 fork。 */
        replicationStartPendingFork();
    }
}

/* 由 serverCron 和 loadingCron 调用以更新缓存的内存指标。 */
void cronUpdateMemoryStats() {
    /* 记录自服务器启动以来使用的最大内存。 */
    if (zmalloc_used_memory() > g_pserver->stat_peak_memory)
        g_pserver->stat_peak_memory = zmalloc_used_memory();

    run_with_period(100) {
        /* 在此处对 RSS 和其他指标进行采样，因为这是一个相对较慢的调用。
         * 我们必须在获取 rss 的同时对 zmalloc_used 进行采样，否则
         *碎片率计算可能会出现偏差（不同时间两个样本的比率） */
        g_pserver->cron_malloc_stats.process_rss = zmalloc_get_rss();
        g_pserver->cron_malloc_stats.zmalloc_used = zmalloc_used_memory();
        /* 对分配器信息进行采样也可能很慢。
         * 它显示的碎片率可能更准确，
         *它排除了其他 RSS 页面，例如：共享库、LUA 和其他非 zmalloc
         *分配，以及可以清除的分配器保留页面（所有这些都不是实际碎片） */
        zmalloc_get_allocator_info(&g_pserver->cron_malloc_stats.allocator_allocated,
                                   &g_pserver->cron_malloc_stats.allocator_active,
                                   &g_pserver->cron_malloc_stats.allocator_resident);
        /* 如果分配器未提供这些统计信息，则伪造它们，以便
         * 碎片信息仍显示一些（不准确的指标） */
        if (!g_pserver->cron_malloc_stats.allocator_resident) {
            /* LUA 内存不是 zmalloc_used 的一部分，但它是进程 RSS 的一部分，
             * 因此我们必须扣除它才能计算出正确的
             * “分配器碎片”率 */
            size_t lua_memory = lua_gc(g_pserver->lua,LUA_GCCOUNT,0)*1024LL;
            g_pserver->cron_malloc_stats.allocator_resident = g_pserver->cron_malloc_stats.process_rss - lua_memory;
        }
        if (!g_pserver->cron_malloc_stats.allocator_active)
            g_pserver->cron_malloc_stats.allocator_active = g_pserver->cron_malloc_stats.allocator_resident;
        if (!g_pserver->cron_malloc_stats.allocator_allocated)
            g_pserver->cron_malloc_stats.allocator_allocated = g_pserver->cron_malloc_stats.zmalloc_used;

        if (g_pserver->force_eviction_percent) {
            g_pserver->cron_malloc_stats.sys_available = getMemAvailable();
        }
    }
}

static std::atomic<bool> s_fFlushInProgress { false };
void flushStorageWeak()
{
    bool fExpected = false;
    if (s_fFlushInProgress.compare_exchange_strong(fExpected, true /* desired */, std::memory_order_seq_cst, std::memory_order_relaxed))
    {
        g_pserver->asyncworkqueue->AddWorkFunction([]{
            aeAcquireLock();
            mstime_t storage_process_latency;
            latencyStartMonitor(storage_process_latency);
            std::vector<redisDb*> vecdb;
            for (int idb = 0; idb < cserver.dbnum; ++idb) {
                if (g_pserver->db[idb]->processChanges(true))
                    vecdb.push_back(g_pserver->db[idb]);
            }
            latencyEndMonitor(storage_process_latency);
            latencyAddSampleIfNeeded("storage-process-changes", storage_process_latency);
            aeReleaseLock();

            std::vector<const redisDbPersistentDataSnapshot*> vecsnapshotFree;
            vecsnapshotFree.resize(vecdb.size());
            for (size_t idb = 0; idb < vecdb.size(); ++idb)
                vecdb[idb]->commitChanges(&vecsnapshotFree[idb]);

            for (size_t idb = 0; idb < vecsnapshotFree.size(); ++idb) {
                if (vecsnapshotFree[idb] != nullptr)
                    vecdb[idb]->endSnapshotAsync(vecsnapshotFree[idb]);
            }
            s_fFlushInProgress = false;
        }, true /* fHiPri */);
    }
    else
    {
        serverLog(LOG_INFO, "Missed storage flush due to existing flush still in flight.  Consider increasing storage-weak-flush-period");
    }
}

/* 这是我们的定时器中断，每秒调用 g_pserver->hz 次。
 * 我们在这里执行许多需要异步完成的事情。
 * 例如：
 *
 * - 活动过期密钥收集（它也在查找时以惰性方式执行）。
 * - 软件看门狗。
 * - 更新一些统计数据。
 * - 数据库哈希表的增量重新哈希。
 * - 触发 BGSAVE / AOF 重写，并处理已终止的子进程。
 * - 不同类型的客户端超时。
 * - 复制重新连接。
 * - 还有更多...
 *
 * 这里直接调用的所有内容都将每秒调用 g_pserver->hz 次，
 * 因此为了限制我们希望不那么频繁执行的事情的执行，
 * 使用了一个宏：run_with_period(milliseconds) { .... }
 */

void unblockChildThreadIfNecessary();
int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    int j;
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    if (g_pserver->maxmemory && g_pserver->m_pstorageFactory)
        performEvictions(false);

    /* 如果其他线程解除了我们某个客户端的阻塞，并且此线程一直处于空闲状态，
        那么 beforeSleep 将没有机会处理解除阻塞。因此，我们也在 cron 作业中处理它们，
        以确保它们不会饿死。
    */
    if (listLength(g_pserver->rgthreadvar[IDX_EVENT_LOOP_MAIN].unblocked_clients))
    {
        processUnblockedClients(IDX_EVENT_LOOP_MAIN);
    }
        
    /* 软件看门狗：如果我们返回不够快，则传递将到达信号处理程序的 SIGALRM。 */
    if (g_pserver->watchdog_period) watchdogScheduleSignal(g_pserver->watchdog_period);

    g_pserver->hz = g_pserver->config_hz;
    /* 使 g_pserver->hz 值适应已配置客户端的数量。如果我们有很多客户端，
     * 我们希望以更高的频率调用 serverCron()。 */
    if (g_pserver->dynamic_hz) {
        while (listLength(g_pserver->clients) / g_pserver->hz >
               MAX_CLIENTS_PER_CLOCK_TICK)
        {
            g_pserver->hz += g_pserver->hz; // *= 2
            if (g_pserver->hz > CONFIG_MAX_HZ) {
                g_pserver->hz = CONFIG_MAX_HZ;
                break;
            }
        }
    }

    /* 一个已取消的子线程可能因为等待我们从管道读取而挂起 */
    unblockChildThreadIfNecessary();

    run_with_period(100) {
        long long stat_net_input_bytes, stat_net_output_bytes;
        stat_net_input_bytes = g_pserver->stat_net_input_bytes.load(std::memory_order_relaxed);
        stat_net_output_bytes = g_pserver->stat_net_output_bytes.load(std::memory_order_relaxed);

        long long stat_numcommands;
        __atomic_load(&g_pserver->stat_numcommands, &stat_numcommands, __ATOMIC_RELAXED);
        trackInstantaneousMetric(STATS_METRIC_COMMAND,stat_numcommands);
        trackInstantaneousMetric(STATS_METRIC_NET_INPUT,
                stat_net_input_bytes);
        trackInstantaneousMetric(STATS_METRIC_NET_OUTPUT,
                stat_net_output_bytes);
    }

    /* 我们每个对象只有 LRU_BITS 位用于 LRU 信息。
     * 所以我们使用一个（最终会回绕的）LRU 时钟。
     *
     * 请注意，即使计数器回绕也不是大问题，
     * 一切仍将正常工作，但某些对象在 Redis 看来会更年轻。
     * 然而，要发生这种情况，某个给定的对象必须在计数器回绕所需的全部时间内都未被触及，
     * 这不太可能。
     *
     * 请注意，您可以通过更改 LRU_CLOCK_RESOLUTION 定义来更改分辨率。 */
    g_pserver->lruclock = getLRUClock();

    cronUpdateMemoryStats();

    /* 我们收到了一个 SIGTERM，正在以安全的方式在此处关闭，
     * 因为在信号处理程序中这样做是不合适的。 */
    if (g_pserver->shutdown_asap) {
        if (prepareForShutdown(SHUTDOWN_NOFLAGS) == C_OK) throw ShutdownException();
        serverLog(LL_WARNING,"SIGTERM received but errors trying to shut down the server, check the logs for more information");
        g_pserver->shutdown_asap = 0;
    }

    /* 显示有关非空数据库的一些信息 */
    if (cserver.verbosity <= LL_VERBOSE) {
        run_with_period(5000) {
            for (j = 0; j < cserver.dbnum; j++) {
                long long size, used, vkeys;

                size = g_pserver->db[j]->slots();
                used = g_pserver->db[j]->size();
                vkeys = g_pserver->db[j]->expireSize();
                if (used || vkeys) {
                    serverLog(LL_VERBOSE,"DB %d: %lld keys (%lld volatile) in %lld slots HT.",j,used,vkeys,size);
                }
            }
        }
    }

    /* 显示有关已连接客户端的信息 */
    if (!g_pserver->sentinel_mode) {
        run_with_period(5000) {
            serverLog(LL_DEBUG,
                "%lu clients connected (%lu replicas), %zu bytes in use",
                listLength(g_pserver->clients)-listLength(g_pserver->slaves),
                listLength(g_pserver->slaves),
                zmalloc_used_memory());
        }
    }

    /* 我们需要异步地对客户端执行一些操作。 */
    clientsCron(IDX_EVENT_LOOP_MAIN);

    /* 处理 Redis 数据库上的后台操作。 */
    databasesCron(true /* fMainThread */);

    /* 如果用户在 BGSAVE 正在进行时请求，则启动计划的 AOF 重写。 */
    if (!hasActiveChildProcessOrBGSave() &&
        g_pserver->aof_rewrite_scheduled)
    {
        rewriteAppendOnlyFileBackground();
    }

    /* 检查正在进行的后台保存或 AOF 重写是否已终止。 */
    if (hasActiveChildProcessOrBGSave() || ldbPendingChildren())
    {
        run_with_period(1000) receiveChildInfo();
        checkChildrenDone();
    } else {
        /* 如果没有正在进行的后台保存/重写，请检查我们现在是否必须保存/重写。 */
        for (j = 0; j < g_pserver->saveparamslen; j++) {
            struct saveparam *sp = g_pserver->saveparams+j;

            /* 如果我们达到了给定的更改量、给定的秒数，
             * 并且最新的 bgsave 成功，或者如果发生错误，
             * 至少已经过去了 CONFIG_BGSAVE_RETRY_DELAY 秒，则保存。 */
            if (g_pserver->dirty >= sp->changes &&
                g_pserver->unixtime-g_pserver->lastsave > sp->seconds &&
                (g_pserver->unixtime-g_pserver->lastbgsave_try >
                 CONFIG_BGSAVE_RETRY_DELAY ||
                 g_pserver->lastbgsave_status == C_OK))
            {
                // 确保重新哈希已完成
                bool fRehashInProgress = false;
                if (g_pserver->activerehashing) {
                    for (int idb = 0; idb < cserver.dbnum && !fRehashInProgress; ++idb) {
                        if (g_pserver->db[idb]->FRehashing())
                            fRehashInProgress = true;
                    }
                }

                if (!fRehashInProgress) {
                    serverLog(LL_NOTICE,"%d changes in %d seconds. Saving...",
                        sp->changes, (int)sp->seconds);
                    rdbSaveInfo rsi, *rsiptr;
                    rsiptr = rdbPopulateSaveInfo(&rsi);
                    rdbSaveBackground(rsiptr);
                }
                break;
            }
        }

        /* 如果需要，触发 AOF 重写。 */
        if (g_pserver->aof_state == AOF_ON &&
            !hasActiveChildProcessOrBGSave() &&
            g_pserver->aof_rewrite_perc &&
            g_pserver->aof_current_size > g_pserver->aof_rewrite_min_size)
        {
            long long base = g_pserver->aof_rewrite_base_size ?
                g_pserver->aof_rewrite_base_size : 1;
            long long growth = (g_pserver->aof_current_size*100/base) - 100;
            if (growth >= g_pserver->aof_rewrite_perc) {
                serverLog(LL_NOTICE,"Starting automatic rewriting of AOF on %lld%% growth",growth);
                rewriteAppendOnlyFileBackground();
            }
        }
    }
    /* 仅仅为了防御性编程，以避免在需要时忘记调用此函数。 */
    updateDictResizePolicy();


    /* AOF 延迟刷新：在每个 cron 周期尝试慢速 fsync 是否已完成。 */
    if (g_pserver->aof_state == AOF_ON && g_pserver->aof_flush_postponed_start)
        flushAppendOnlyFile(0);

    /* AOF 写入错误：在这种情况下，我们也有一个缓冲区要刷新，
     * 并在成功时清除 AOF 错误以使数据库再次可写，
     * 但是如果“hz”设置为更高的频率，则每秒尝试一次就足够了。 */
    run_with_period(1000) {
        if (g_pserver->aof_state == AOF_ON && g_pserver->aof_last_write_status == C_ERR)
            flushAppendOnlyFile(0);
    }

    /* 如果需要，清除暂停的客户端状态。 */
    checkClientPauseTimeoutAndReturnIfPaused();

    /* 复制 cron 函数——用于重新连接到主服务器、检测传输失败、
     * 启动后台 RDB 传输等等。
     *
     * 如果 Redis 正在尝试故障转移，则更快地运行复制 cron，
     * 以便握手过程更快地进行。 */
    if (g_pserver->failover_state != NO_FAILOVER) {
        run_with_period(100) replicationCron();
    } else {
        run_with_period(1000) replicationCron();
    }

    /* 运行 Redis 集群 cron。 */
    run_with_period(100) {
        if (g_pserver->cluster_enabled) clusterCron();
    }

    /* 如果我们处于哨兵模式，则运行哨兵计时器。 */
    if (g_pserver->sentinel_mode) sentinelTimer();

    /* 清理过期的 MIGRATE 缓存套接字。 */
    run_with_period(1000) {
        migrateCloseTimedoutSockets();
    }

    /* 检查 CPU 过载 */
    run_with_period(10'000) {
        g_pserver->is_overloaded = false;
        g_pserver->overload_closed_clients = 0;
        static clock_t last = 0;
        if (g_pserver->overload_protect_threshold > 0) {
            clock_t cur = clock();
            double perc = static_cast<double>(cur - last) / (CLOCKS_PER_SEC*10);
            perc /= cserver.cthreads;
            perc *= 100.0;
            serverLog(LL_WARNING, "CPU Used: %.2f", perc);
            if (perc > g_pserver->overload_protect_threshold) {
                serverLog(LL_WARNING, "\tWARNING: CPU overload detected.");
                g_pserver->is_overloaded = true;
            }
            last = cur;
        }
    }

    /* 根据 CPU 负载调整 fastlock */
    run_with_period(30000) {
        /* 根据 CPU 负载调整 fastlock */
        fastlock_auto_adjust_waits();
    }

    /* 如果需要，重新加载 TLS 证书。如果磁盘上的证书已更改，
     * 但 KeyDB 服务器尚未收到通知，则此操作实际上会轮换证书。 */
    run_with_period(1000){
        tlsReload();
    }

    /* 如果需要，调整跟踪密钥表的大小。这也在每个命令执行时完成，
     * 但我们希望确保如果最后执行的命令通过 CONFIG SET 更改了值，
     * 即使服务器完全空闲，服务器也将执行该操作。 */
    if (g_pserver->tracking_clients) trackingLimitUsedSlots();

    /* 如果设置了相应的标志，则启动计划的 BGSAVE。
     * 当我们因为 AOF 重写正在进行而被迫推迟 BGSAVE 时，这很有用。
     *
     * 注意：此代码必须在上面的 replicationCron() 调用之后，
     * 因此在重构此文件时请确保保持此顺序。这很有用，
     * 因为我们希望优先为复制进行 RDB 保存。 */
    if (!hasActiveChildProcessOrBGSave() &&
        g_pserver->rdb_bgsave_scheduled &&
        (g_pserver->unixtime-g_pserver->lastbgsave_try > CONFIG_BGSAVE_RETRY_DELAY ||
         g_pserver->lastbgsave_status == C_OK))
    {
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        if (rdbSaveBackground(rsiptr) == C_OK)
            g_pserver->rdb_bgsave_scheduled = 0;
    }

    if (cserver.storage_memory_model == STORAGE_WRITEBACK && g_pserver->m_pstorageFactory && !g_pserver->loading) {
        run_with_period(g_pserver->storage_flush_period) {
            flushStorageWeak();
        }
    }

    /* Fire the cron loop modules event. */
    RedisModuleCronLoopV1 ei = {REDISMODULE_CRON_LOOP_VERSION,g_pserver->hz};
    moduleFireServerEvent(REDISMODULE_EVENT_CRON_LOOP,
                          0,
                          &ei);


    /* CRON functions may trigger async writes, so do this last */
    ProcessPendingAsyncWrites();

    // Measure lock contention from a different thread to be more accurate
    g_pserver->asyncworkqueue->AddWorkFunction([]{
        g_pserver->rglockSamples[g_pserver->ilockRingHead] = (uint16_t)aeLockContention();
        ++g_pserver->ilockRingHead;
        if (g_pserver->ilockRingHead >= redisServer::s_lockContentionSamples)
            g_pserver->ilockRingHead = 0;
    });

    run_with_period(10) {
        if (!g_pserver->garbageCollector.empty()) {
            // Server threads don't free the GC, but if we don't have a
            //  a bgsave or some other async task then we'll hold onto the
            //  data for too long
            g_pserver->asyncworkqueue->AddWorkFunction([]{
                auto epoch = g_pserver->garbageCollector.startEpoch();
                g_pserver->garbageCollector.endEpoch(epoch);
            });
        }
    }

    if (g_pserver->soft_shutdown) {
        /* Loop through our clients list and see if there are any active clients */
        listIter li;
        listNode *ln;
        listRewind(g_pserver->clients, &li);
        bool fActiveClient = false;
        while ((ln = listNext(&li)) && !fActiveClient) {
            client *c = (client*)listNodeValue(ln);
            if (c->flags & CLIENT_IGNORE_SOFT_SHUTDOWN)
                continue;
            fActiveClient = true;
        }
        if (!fActiveClient) {
            if (prepareForShutdown(SHUTDOWN_NOFLAGS) == C_OK) {
                serverLog(LL_WARNING, "All active clients have disconnected while a soft shutdown is pending.  Shutting down now.");
                throw ShutdownException();
            }
        }
    }

    g_pserver->cronloops++;
    return 1000/g_pserver->hz;
}

// serverCron for worker threads other than the main thread
int serverCronLite(struct aeEventLoop *eventLoop, long long id, void *clientData)
{
    UNUSED(id);
    UNUSED(clientData);

    if (g_pserver->maxmemory && g_pserver->m_pstorageFactory)
        performEvictions(false);

    int iel = ielFromEventLoop(eventLoop);
    serverAssert(iel != IDX_EVENT_LOOP_MAIN);

    /* If another threads unblocked one of our clients, and this thread has been idle
        then beforeSleep won't have a chance to process the unblocking.  So we also
        process them here in the cron job to ensure they don't starve.
    */
    if (listLength(g_pserver->rgthreadvar[iel].unblocked_clients))
    {
        processUnblockedClients(iel);
    }

    /* Handle background operations on Redis databases. */
    databasesCron(false /* fMainThread */);

    /* Unpause clients if enough time has elapsed */
    checkClientPauseTimeoutAndReturnIfPaused();
    
    ProcessPendingAsyncWrites();    // A bug but leave for now, events should clean up after themselves
    clientsCron(iel);

    return 1000/g_pserver->hz;
}

extern "C" void asyncFreeDictTable(dictEntry **de)
{
    if (de == nullptr || serverTL == nullptr || serverTL->gcEpoch.isReset()) {
        zfree(de);
    } else {
        g_pserver->garbageCollector.enqueueCPtr(serverTL->gcEpoch, de);
    }
}

void blockingOperationStarts() {
    if(!g_pserver->blocking_op_nesting++){
        __atomic_load(&g_pserver->mstime, &g_pserver->blocked_last_cron, __ATOMIC_ACQUIRE);
    }
}

void blockingOperationEnds() {
    if(!(--g_pserver->blocking_op_nesting)){
        g_pserver->blocked_last_cron = 0;
    }
}

/* 此函数在 RDB 或 AOF 加载期间以及阻塞脚本期间扮演 serverCron 的角色。
 * 它尝试以与配置的 g_pserver->hz 相似的速率执行其职责，
 * 并更新 cronloops 变量，以便与 serverCron 类似地使用 run_with_period。 */
void whileBlockedCron() {
    /* 在这里，我们可能希望执行一些 cron 作业（通常每秒执行 g_pserver->hz 次）。 */

    /* 由于此函数依赖于对 blockingOperationStarts 的调用，因此我们确保已完成该调用。 */
    serverAssert(g_pserver->blocked_last_cron);

    /* 如果我们被调用得太早，则立即离开。这样，下面循环之后的一次性作业就不需要 if 了。
     * 如果此函数被调用得太频繁，我们也不会费心启动延迟监视器。 */
    if (g_pserver->blocked_last_cron >= g_pserver->mstime)
        return;

    mstime_t latency;
    latencyStartMonitor(latency);

    /* 在某些情况下，我们可能会以较大的时间间隔被调用，因此我们可能需要在此处执行额外的工作。
     * 这是因为 serverCron 中的某些函数依赖于这样一个事实：它大约每 10 毫秒执行一次。
     * 例如，如果 activeDefragCycle 需要使用 25% 的 CPU，它将使用 2.5 毫秒，因此我们需要多次调用它。 */
    long hz_ms = 1000/g_pserver->hz;
    while (g_pserver->blocked_last_cron < g_pserver->mstime) {

        /* 逐步对密钥进行碎片整理。 */
        activeDefragCycle();

        g_pserver->blocked_last_cron += hz_ms;

        /* 增加 cronloop 以便 run_with_period 工作。 */
        g_pserver->cronloops++;
    }

    /* 其他 cron 作业不需要在循环中完成。无需检查 g_pserver->blocked_last_cron，
     * 因为我们在顶部有一个提前退出的判断。 */

    /* 在加载期间更新内存统计信息（不包括阻塞的脚本） */
    if (g_pserver->loading) cronUpdateMemoryStats();

    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("while-blocked-cron",latency);
}

extern __thread int ProcessingEventsWhileBlocked;

/* 每当 Redis 进入事件驱动库的主循环时，即在为就绪文件描述符休眠之前，都会调用此函数。
 *
 * 注意：此函数（目前）由两个函数调用：
 * 1. aeMain - 服务器主循环
 * 2. processEventsWhileBlocked - 在 RDB/AOF 加载期间处理客户端
 *
 * 如果它是从 processEventsWhileBlocked 调用的，我们不想执行所有操作
 * （例如，我们不想让密钥过期），但我们确实需要执行一些操作。
 *
 * 最重要的是 freeClientsInAsyncFreeQueue，但我们也会调用一些其他低风险函数。 */
void beforeSleep(struct aeEventLoop *eventLoop) {
    AeLocker locker;
    int iel = ielFromEventLoop(eventLoop);

    tlsProcessPendingData();

    locker.arm();

    /* 结束由快速异步命令创建的任何快照 */
    for (int idb = 0; idb < cserver.dbnum; ++idb) {
        if (serverTL->rgdbSnapshot[idb] != nullptr && serverTL->rgdbSnapshot[idb]->FStale()) {
            g_pserver->db[idb]->endSnapshot(serverTL->rgdbSnapshot[idb]);
            serverTL->rgdbSnapshot[idb] = nullptr;
        }
    }

    size_t zmalloc_used = zmalloc_used_memory();
    if (zmalloc_used > g_pserver->stat_peak_memory)
        g_pserver->stat_peak_memory = zmalloc_used;
    
    serverAssert(g_pserver->repl_batch_offStart < 0);

    runAndPropogateToReplicas(processClients);

    /* 如果我们从 processEventsWhileBlocked() 重新进入事件循环，则只调用一部分重要函数。
     * 注意，在这种情况下，我们会跟踪正在处理的事件数量，
     * 因为如果不再有要处理的事件，processEventsWhileBlocked() 希望尽快停止。 */
    if (ProcessingEventsWhileBlocked) {
        uint64_t processed = 0;
        int aof_state = g_pserver->aof_state;
        locker.disarm();
        processed += handleClientsWithPendingWrites(iel, aof_state);
        locker.arm();
        processed += freeClientsInAsyncFreeQueue(iel);
        g_pserver->events_processed_while_blocked += processed;
        return;
    }

    /* 处理阻塞客户端的精确超时。 */
    handleBlockedClientsTimeout();

    /* 如果 tls 仍有待处理的未读数据，则根本不休眠。 */
    aeSetDontWait(eventLoop, tlsHasPendingData());

    /* 调用 Redis 集群休眠前函数。请注意，此函数可能会更改 Redis 集群的状态
     * （从正常到失败，反之亦然），因此最好在此函数稍后为未阻塞的客户端提供服务之前调用它。 */
    if (g_pserver->cluster_enabled) clusterBeforeSleep();

    /* 运行快速过期周期（如果不需要快速周期，则被调用函数将尽快返回）。 */
    if (g_pserver->active_expire_enabled && (listLength(g_pserver->masters) == 0 || g_pserver->fActiveReplica))
        activeExpireCycle(ACTIVE_EXPIRE_CYCLE_FAST);

    /* 解除在 WAIT 中为同步复制而阻塞的所有客户端的阻塞。 */
    if (listLength(g_pserver->clients_waiting_acks))
        processClientsWaitingReplicas();

    /* 检查是否有由实现阻塞命令的模块解除阻塞的客户端。 */
    if (moduleCount()) moduleHandleBlockedClients(ielFromEventLoop(eventLoop));

    /* 尝试为刚刚解除阻塞的客户端处理待处理的命令。 */
    if (listLength(g_pserver->rgthreadvar[iel].unblocked_clients))
    {
        processUnblockedClients(iel);
    }

    /* 如果在前一个事件循环迭代期间至少有一个客户端被阻塞，则向所有从属服务器发送 ACK 请求。
     * 请注意，我们在 processUnblockedClients() 之后执行此操作，因此如果存在多个流水线的 WAIT，
     * 并且刚刚解除阻塞的 WAIT 再次被阻塞，则在没有其他事件循环事件的情况下，
     * 我们不必等待一个服务器 cron 周期。请参见 #6623。
     *
     * 当客户端暂停时，我们也不会发送 ACK，因为这会增加复制积压，
     * 如果我们仍然是主服务器，它们将在暂停后发送。 */
    if (g_pserver->get_ack_from_slaves && !checkClientPauseTimeoutAndReturnIfPaused()) {
        robj *argv[3];

        argv[0] = shared.replconf;
        argv[1] = shared.getack;
        argv[2] = shared.special_asterick; /* 未使用的参数。 */
        replicationFeedSlaves(g_pserver->slaves, g_pserver->replicaseldb, argv, 3);
        g_pserver->get_ack_from_slaves = 0;
    }

    /* 我们可能已经从客户端收到了有关其当前偏移量的更新。注意：
     * 这不能在收到 ACK 的地方完成，因为故障转移会断开我们客户端的连接。 */
    if (iel == IDX_EVENT_LOOP_MAIN)
        updateFailoverStatus();

    /* 将无效消息发送到以广播 (BCAST) 模式参与客户端缓存协议的客户端。 */
    trackingBroadcastInvalidationMessages();

    /* 将 AOF 缓冲区写入磁盘 */
    if (g_pserver->aof_state == AOF_ON)
        flushAppendOnlyFile(0);

    static thread_local bool fFirstRun = true;
    // 注意：我们还复制了 DB 指针，以防在释放锁时完成 DB 交换
    std::vector<redisDb*> vecdb;    // 注意：我们缓存了数据库指针，以防在释放锁时完成数据库交换
    if (cserver.storage_memory_model == STORAGE_WRITETHROUGH && !g_pserver->loading)
    {
        if (!fFirstRun) {
            mstime_t storage_process_latency;
            latencyStartMonitor(storage_process_latency);
            for (int idb = 0; idb < cserver.dbnum; ++idb) {
                if (g_pserver->db[idb]->processChanges(false))
                    vecdb.push_back(g_pserver->db[idb]);
            }
            latencyEndMonitor(storage_process_latency);
            latencyAddSampleIfNeeded("storage-process-changes", storage_process_latency);
        } else {
            fFirstRun = false;
        }
    }

    int aof_state = g_pserver->aof_state;

    mstime_t commit_latency;
    latencyStartMonitor(commit_latency);
    if (g_pserver->m_pstorageFactory != nullptr)
    {
        locker.disarm();
        for (redisDb *db : vecdb)
            db->commitChanges();
        locker.arm();
    }
    latencyEndMonitor(commit_latency);
    latencyAddSampleIfNeeded("storage-commit", commit_latency);

    /* 我们尝试在最后处理写入，这样就不必重新获取锁，
        但是如果存在挂起的异步关闭，我们需要确保写入首先发生，
        因此在此处执行 */
    bool fSentReplies = false;

    std::unique_lock<fastlock> ul(g_lockasyncfree);
    if (listLength(g_pserver->clients_to_close)) {
        ul.unlock();
        locker.disarm();
        handleClientsWithPendingWrites(iel, aof_state);
        locker.arm();
        fSentReplies = true;
    } else {
        ul.unlock();
    }
    
    if (!serverTL->gcEpoch.isReset())
        g_pserver->garbageCollector.endEpoch(serverTL->gcEpoch, true /*fNoFree*/);
    serverTL->gcEpoch.reset();

    /* 异步关闭需要关闭的客户端 */
    freeClientsInAsyncFreeQueue(iel);

    if (!serverTL->gcEpoch.isReset())
        g_pserver->garbageCollector.endEpoch(serverTL->gcEpoch, true /*fNoFree*/);
    serverTL->gcEpoch.reset();

    /* 每隔一段时间尝试处理阻塞的客户端。例如：一个模块从计时器回调中调用 RM_SignalKeyAsReady（因此我们根本不会访问 processCommand()）。 */
    handleClientsBlockedOnKeys();

    /* 在我们进入休眠之前，通过释放 GIL 让线程访问数据集。
     * Redis 主线程此时不会触碰任何东西。 */
    serverAssert(g_pserver->repl_batch_offStart < 0);
    locker.disarm();
    if (!fSentReplies)
        handleClientsWithPendingWrites(iel, aof_state);

    aeThreadOffline();
    // 作用域锁守卫
    {
        std::unique_lock<fastlock> lock(time_thread_lock);
        sleeping_threads++;
        serverAssert(sleeping_threads <= cserver.cthreads);
    }

    if (!g_pserver->garbageCollector.empty()) {
        // 服务器线程不会释放 GC，但是如果我们没有 bgsave 或其他一些异步任务，
        // 那么我们会将数据保留太长时间
        g_pserver->asyncworkqueue->AddWorkFunction([]{
            auto epoch = g_pserver->garbageCollector.startEpoch();
            g_pserver->garbageCollector.endEpoch(epoch);
        }, true /*fHiPri*/);
    }
    
    /* 在休眠之前确定模块是否已启用，并在此处和唤醒后使用该结果，以避免双重获取或释放 GIL */
    serverTL->modulesEnabledThisAeLoop = !!moduleCount();
    if (serverTL->modulesEnabledThisAeLoop) moduleReleaseGIL(TRUE /*fServerThread*/);

    /* 不要在 moduleReleaseGIL 下方添加任何内容 !!! */
}

/* 此函数在事件循环多路复用 API 返回后立即调用，
 * 并且控制权很快就会通过调用不同的事件回调返回给 Redis。 */
void afterSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);
    /* 不要在 moduleAcquireGIL 上方添加任何内容 !!! */

    /* 获取模块 GIL，以便它们的线程不会触碰任何东西。
       此处不要检查模块是否已启用，而是使用 beforeSleep 的结果
       否则您可能会双重获取 GIL 并导致模块死锁 */
    if (!ProcessingEventsWhileBlocked) {
        if (serverTL->modulesEnabledThisAeLoop) moduleAcquireGIL(TRUE /*fServerThread*/);
        aeThreadOnline();
        wakeTimeThread();

        serverAssert(serverTL->gcEpoch.isReset());
        serverTL->gcEpoch = g_pserver->garbageCollector.startEpoch();

        aeAcquireLock();
        for (int idb = 0; idb < cserver.dbnum; ++idb)
            g_pserver->db[idb]->trackChanges(false);
        aeReleaseLock();

        serverTL->disable_async_commands = false;
    }
}

/* =========================== 服务器初始化 ======================== */

void createSharedObjects(void) {
    int j;

    /* 共享命令响应 */
    shared.crlf = makeObjectShared(createObject(OBJ_STRING,sdsnew("\r\n")));
    shared.ok = makeObjectShared(createObject(OBJ_STRING,sdsnew("+OK\r\n")));
    shared.emptybulk = makeObjectShared(createObject(OBJ_STRING,sdsnew("$0\r\n\r\n")));
    shared.czero = makeObjectShared(createObject(OBJ_STRING,sdsnew(":0\r\n")));
    shared.cone = makeObjectShared(createObject(OBJ_STRING,sdsnew(":1\r\n")));
    shared.emptyarray = makeObjectShared(createObject(OBJ_STRING,sdsnew("*0\r\n")));
    shared.pong = makeObjectShared(createObject(OBJ_STRING,sdsnew("+PONG\r\n")));
    shared.queued = makeObjectShared(createObject(OBJ_STRING,sdsnew("+QUEUED\r\n")));
    shared.emptyscan = makeObjectShared(createObject(OBJ_STRING,sdsnew("*2\r\n$1\r\n0\r\n*0\r\n")));
    shared.space = makeObjectShared(createObject(OBJ_STRING,sdsnew(" ")));
    shared.colon = makeObjectShared(createObject(OBJ_STRING,sdsnew(":")));
    shared.plus = makeObjectShared(createObject(OBJ_STRING,sdsnew("+")));
    shared.nullbulk = makeObjectShared(createObject(OBJ_STRING,sdsnew("$0\r\n\r\n")));
    
    /* 共享命令错误响应 */
    shared.wrongtypeerr = makeObjectShared(createObject(OBJ_STRING,sdsnew(
        "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")));
    shared.err = makeObjectShared(createObject(OBJ_STRING,sdsnew("-ERR\r\n")));
    shared.nokeyerr = makeObjectShared(createObject(OBJ_STRING,sdsnew(
        "-ERR no such key\r\n")));
    shared.syntaxerr = makeObjectShared(createObject(OBJ_STRING,sdsnew(
        "-ERR syntax error\r\n")));
    shared.sameobjecterr = makeObjectShared(createObject(OBJ_STRING,sdsnew(
        "-ERR source and destination objects are the same\r\n")));
    shared.outofrangeerr = makeObjectShared(createObject(OBJ_STRING,sdsnew(
        "-ERR index out of range\r\n")));
    shared.noscripterr = makeObjectShared(createObject(OBJ_STRING,sdsnew(
        "-NOSCRIPT No matching script. Please use EVAL.\r\n")));
    shared.loadingerr = makeObjectShared(createObject(OBJ_STRING,sdsnew(
        "-LOADING KeyDB is loading the dataset in memory\r\n")));
    shared.slowscripterr = makeObjectShared(createObject(OBJ_STRING,sdsnew(
        "-BUSY KeyDB is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.\r\n")));
    shared.masterdownerr = makeObjectShared(createObject(OBJ_STRING,sdsnew(
        "-MASTERDOWN Link with MASTER is down and replica-serve-stale-data is set to 'no'.\r\n")));
    shared.bgsaveerr = makeObjectShared(createObject(OBJ_STRING,sdsnew(
        "-MISCONF KeyDB is configured to save RDB snapshots, but it is currently not able to persist on disk. Commands that may modify the data set are disabled, because this instance is configured to report errors during writes if RDB snapshotting fails (stop-writes-on-bgsave-error option). Please check the KeyDB logs for details about the RDB error.\r\n")));
    shared.roslaveerr = makeObjectShared(createObject(OBJ_STRING,sdsnew(
        "-READONLY You can't write against a read only replica.\r\n")));
    shared.noautherr = makeObjectShared(createObject(OBJ_STRING,sdsnew(
        "-NOAUTH Authentication required.\r\n")));
    shared.oomerr = makeObjectShared(createObject(OBJ_STRING,sdsnew(
        "-OOM command not allowed when used memory > 'maxmemory'.\r\n")));
    shared.execaborterr = makeObjectShared(createObject(OBJ_STRING,sdsnew(
        "-EXECABORT Transaction discarded because of previous errors.\r\n")));
    shared.noreplicaserr = makeObjectShared(createObject(OBJ_STRING,sdsnew(
        "-NOREPLICAS Not enough good replicas to write.\r\n")));
    shared.busykeyerr = makeObjectShared(createObject(OBJ_STRING,sdsnew(
        "-BUSYKEY Target key name already exists.\r\n")));
    

    /* 共享的 NULL 取决于协议版本。 */
    shared.null[0] = NULL;
    shared.null[1] = NULL;
    shared.null[2] = makeObjectShared(createObject(OBJ_STRING,sdsnew("$-1\r\n")));
    shared.null[3] = makeObjectShared(createObject(OBJ_STRING,sdsnew("_\r\n")));

    shared.nullarray[0] = NULL;
    shared.nullarray[1] = NULL;
    shared.nullarray[2] = makeObjectShared(createObject(OBJ_STRING,sdsnew("*-1\r\n")));
    shared.nullarray[3] = makeObjectShared(createObject(OBJ_STRING,sdsnew("_\r\n")));

    shared.emptymap[0] = NULL;
    shared.emptymap[1] = NULL;
    shared.emptymap[2] = createObject(OBJ_STRING,sdsnew("*0\r\n"));
    shared.emptymap[3] = createObject(OBJ_STRING,sdsnew("%0\r\n"));

    shared.emptyset[0] = NULL;
    shared.emptyset[1] = NULL;
    shared.emptyset[2] = createObject(OBJ_STRING,sdsnew("*0\r\n"));
    shared.emptyset[3] = createObject(OBJ_STRING,sdsnew("~0\r\n"));

    for (j = 0; j < PROTO_SHARED_SELECT_CMDS; j++) {
        char dictid_str[64];
        int dictid_len;

        dictid_len = ll2string(dictid_str,sizeof(dictid_str),j);
        shared.select[j] = makeObjectShared(createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),
                "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
                dictid_len, dictid_str)));
    }
    shared.messagebulk = makeObjectShared("$7\r\nmessage\r\n",13);
    shared.pmessagebulk = makeObjectShared("$8\r\npmessage\r\n",14);
    shared.subscribebulk = makeObjectShared("$9\r\nsubscribe\r\n",15);
    shared.unsubscribebulk = makeObjectShared("$11\r\nunsubscribe\r\n",18);
    shared.psubscribebulk = makeObjectShared("$10\r\npsubscribe\r\n",17);
    shared.punsubscribebulk = makeObjectShared("$12\r\npunsubscribe\r\n",19);

    /* 共享命令名称 */
    shared.del = makeObjectShared("DEL",3);
    shared.unlink = makeObjectShared("UNLINK",6);
    shared.rpop = makeObjectShared("RPOP",4);
    shared.lpop = makeObjectShared("LPOP",4);
    shared.lpush = makeObjectShared("LPUSH",5);
    shared.rpoplpush = makeObjectShared("RPOPLPUSH",9);
    shared.lmove = makeObjectShared("LMOVE",5);
    shared.blmove = makeObjectShared("BLMOVE",6);
    shared.zpopmin = makeObjectShared("ZPOPMIN",7);
    shared.zpopmax = makeObjectShared("ZPOPMAX",7);
    shared.multi = makeObjectShared("MULTI",5);
    shared.exec = makeObjectShared("EXEC",4);
    shared.hset = makeObjectShared("HSET",4);
    shared.srem = makeObjectShared("SREM",4);
    shared.xgroup = makeObjectShared("XGROUP",6);
    shared.xclaim = makeObjectShared("XCLAIM",6);
    shared.script = makeObjectShared("SCRIPT",6);
    shared.replconf = makeObjectShared("REPLCONF",8);
    shared.pexpireat = makeObjectShared("PEXPIREAT",9);
    shared.pexpire = makeObjectShared("PEXPIRE",7);
    shared.persist = makeObjectShared("PERSIST",7);
    shared.set = makeObjectShared("SET",3);
    shared.eval = makeObjectShared("EVAL",4);

    /* 共享命令参数 */
    shared.left = makeObjectShared("left",4);
    shared.right = makeObjectShared("right",5);
    shared.pxat = makeObjectShared("PXAT", 4);
    shared.px = makeObjectShared("PX",2);
    shared.time = makeObjectShared("TIME",4);
    shared.retrycount = makeObjectShared("RETRYCOUNT",10);
    shared.force = makeObjectShared("FORCE",5);
    shared.justid = makeObjectShared("JUSTID",6);
    shared.lastid = makeObjectShared("LASTID",6);
    shared.default_username = makeObjectShared("default",7);
    shared.ping = makeObjectShared("ping",4);
    shared.replping = makeObjectShared("replping", 8);
    shared.setid = makeObjectShared("SETID",5);
    shared.keepttl = makeObjectShared("KEEPTTL",7);
    shared.load = makeObjectShared("LOAD",4);
    shared.createconsumer = makeObjectShared("CREATECONSUMER",14);
    shared.getack = makeObjectShared("GETACK",6);
    shared.special_asterick = makeObjectShared("*",1);
    shared.special_equals = makeObjectShared("=",1);
    shared.redacted = makeObjectShared("(redacted)",10);

    /* KeyDB 特定 */
    shared.hdel = makeObjectShared(createStringObject("HDEL", 4));
    shared.zrem = makeObjectShared(createStringObject("ZREM", 4));
    shared.mvccrestore = makeObjectShared(createStringObject("KEYDB.MVCCRESTORE", 17));
    shared.pexpirememberat = makeObjectShared(createStringObject("PEXPIREMEMBERAT",15));

    for (j = 0; j < OBJ_SHARED_INTEGERS; j++) {
        shared.integers[j] =
            makeObjectShared(createObject(OBJ_STRING,(void*)(long)j));
        shared.integers[j]->encoding = OBJ_ENCODING_INT;
    }
    for (j = 0; j < OBJ_SHARED_BULKHDR_LEN; j++) {
        shared.mbulkhdr[j] = makeObjectShared(createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),"*%d\r\n",j)));
        shared.bulkhdr[j] = makeObjectShared(createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),"$%d\r\n",j)));
    }
    /* 以下两个共享对象 minstring 和 maxstrings 实际上并非用于其值，
     * 而是作为特殊对象，分别表示 ZRANGEBYLEX 命令中字符串比较的最小可能字符串和最大可能字符串。 */
    shared.minstring = sdsnew("minstring");
    shared.maxstring = sdsnew("maxstring");
}

void initMasterInfo(redisMaster *master)
{
    if (cserver.default_masterauth)
        master->masterauth = sdsdup(cserver.default_masterauth);
    else
        master->masterauth = NULL;

    if (cserver.default_masteruser)
        master->masteruser = zstrdup(cserver.default_masteruser);
    else
        master->masteruser = NULL;

    master->masterport = 6379;
    master->master = NULL;
    master->cached_master = NULL;
    master->master_initial_offset = -1;
    
    master->isActive = false;

    master->repl_state = REPL_STATE_NONE;
    master->repl_down_since = 0; /* 从未连接，复制从一开始就已关闭。 */
    master->mvccLastSync = 0;
}

void initServerConfig(void) {
    int j;

    updateCachedTime();
    getRandomHexChars(g_pserver->runid,CONFIG_RUN_ID_SIZE);
    g_pserver->runid[CONFIG_RUN_ID_SIZE] = '\0';
    changeReplicationId();
    clearReplicationId2();
    g_pserver->hz = CONFIG_DEFAULT_HZ; /* 尽快初始化它，即使稍后加载配置后可能会更新它。
                                      此值可能在服务器初始化之前使用。 */
    g_pserver->clients = listCreate();
    g_pserver->slaves = listCreate();
    g_pserver->monitors = listCreate();
    g_pserver->clients_timeout_table = raxNew();
    g_pserver->events_processed_while_blocked = 0;
    g_pserver->timezone = getTimeZone(); /* 由 tzset() 初始化。 */
    cserver.configfile = NULL;
    cserver.executable = NULL;
    g_pserver->hz = g_pserver->config_hz = CONFIG_DEFAULT_HZ;
    g_pserver->bindaddr_count = 0;
    g_pserver->unixsocket = NULL;
    g_pserver->unixsocketperm = CONFIG_DEFAULT_UNIX_SOCKET_PERM;
    g_pserver->sofd = -1;
    g_pserver->active_expire_enabled = 1;
    cserver.skip_checksum_validation = 0;
    g_pserver->saveparams = NULL;
    g_pserver->loading = 0;
    g_pserver->loading_rdb_used_mem = 0;
    g_pserver->logfile = zstrdup(CONFIG_DEFAULT_LOGFILE);
    g_pserver->syslog_facility = LOG_LOCAL0;
    cserver.supervised = 0;
    cserver.supervised_mode = SUPERVISED_NONE;
    g_pserver->aof_state = AOF_OFF;
    g_pserver->aof_rewrite_base_size = 0;
    g_pserver->aof_rewrite_scheduled = 0;
    g_pserver->aof_flush_sleep = 0;
    g_pserver->aof_last_fsync = time(NULL);
    atomicSet(g_pserver->aof_bio_fsync_status,C_OK);
    g_pserver->aof_rewrite_time_last = -1;
    g_pserver->aof_rewrite_time_start = -1;
    g_pserver->aof_lastbgrewrite_status = C_OK;
    g_pserver->aof_delayed_fsync = 0;
    g_pserver->aof_fd = -1;
    g_pserver->aof_selected_db = -1; /* 确保第一次不匹配 */
    g_pserver->aof_flush_postponed_start = 0;
    cserver.pidfile = NULL;
    g_pserver->rdb_filename = NULL;
    g_pserver->rdb_s3bucketpath = NULL;
    g_pserver->active_defrag_running = 0;
    g_pserver->notify_keyspace_events = 0;
    g_pserver->blocked_clients = 0;
    memset(g_pserver->blocked_clients_by_type,0,
           sizeof(g_pserver->blocked_clients_by_type));
    g_pserver->shutdown_asap = 0;
    g_pserver->cluster_enabled = 0;
    g_pserver->cluster_configfile = zstrdup(CONFIG_DEFAULT_CLUSTER_CONFIG_FILE);
    g_pserver->migrate_cached_sockets = dictCreate(&migrateCacheDictType,NULL);
    g_pserver->next_client_id = 1; /* 客户端 ID，从 1 开始。*/

    g_pserver->lruclock = getLRUClock();
    resetServerSaveParams();

    appendServerSaveParams(60*60,1);  /* 1 小时 1 次更改后保存 */
    appendServerSaveParams(300,100);  /* 5 分钟 100 次更改后保存 */
    appendServerSaveParams(60,10000); /* 1 分钟 10000 次更改后保存 */

    /* 复制相关 */
    g_pserver->masters = listCreate();
    g_pserver->enable_multimaster = CONFIG_DEFAULT_ENABLE_MULTIMASTER;
    g_pserver->repl_syncio_timeout = CONFIG_REPL_SYNCIO_TIMEOUT;
    g_pserver->master_repl_offset = 0;
    g_pserver->repl_lowest_off.store(-1, std::memory_order_seq_cst);

    /* 复制部分重同步积压 */
    g_pserver->repl_backlog = NULL;
    g_pserver->repl_backlog_histlen = 0;
    g_pserver->repl_backlog_idx = 0;
    g_pserver->repl_backlog_off = 0;
    g_pserver->repl_no_slaves_since = time(NULL);

    /* 故障转移相关 */
    g_pserver->failover_end_time = 0;
    g_pserver->force_failover = 0;
    g_pserver->target_replica_host = NULL;
    g_pserver->target_replica_port = 0;
    g_pserver->failover_state = NO_FAILOVER;

    /* 客户端输出缓冲区限制 */
    for (j = 0; j < CLIENT_TYPE_OBUF_COUNT; j++)
        cserver.client_obuf_limits[j] = clientBufferLimitsDefaults[j];

    /* Linux OOM 分数配置 */
    for (j = 0; j < CONFIG_OOM_COUNT; j++)
        g_pserver->oom_score_adj_values[j] = configOOMScoreAdjValuesDefaults[j];

    /* 双精度常量初始化 */
    R_Zero = 0.0;
    R_PosInf = 1.0/R_Zero;
    R_NegInf = -1.0/R_Zero;
    R_Nan = R_Zero/R_Zero;

    /* 命令表——我们在这里初始化它，因为它是初始配置的一部分，
     * 因为命令名称可以通过 keydb.conf 使用 rename-command 指令进行更改。 */
    g_pserver->commands = dictCreate(&commandTableDictType,NULL);
    g_pserver->orig_commands = dictCreate(&commandTableDictType,NULL);
    populateCommandTable();
    cserver.delCommand = lookupCommandByCString("del");
    cserver.multiCommand = lookupCommandByCString("multi");
    cserver.lpushCommand = lookupCommandByCString("lpush");
    cserver.lpopCommand = lookupCommandByCString("lpop");
    cserver.rpopCommand = lookupCommandByCString("rpop");
    cserver.zpopminCommand = lookupCommandByCString("zpopmin");
    cserver.zpopmaxCommand = lookupCommandByCString("zpopmax");
    cserver.sremCommand = lookupCommandByCString("srem");
    cserver.execCommand = lookupCommandByCString("exec");
    cserver.expireCommand = lookupCommandByCString("expire");
    cserver.pexpireCommand = lookupCommandByCString("pexpire");
    cserver.xclaimCommand = lookupCommandByCString("xclaim");
    cserver.xgroupCommand = lookupCommandByCString("xgroup");
    cserver.rreplayCommand = lookupCommandByCString("rreplay");
    cserver.rpoplpushCommand = lookupCommandByCString("rpoplpush");
    cserver.hdelCommand = lookupCommandByCString("hdel");
    cserver.zremCommand = lookupCommandByCString("zrem");
    cserver.lmoveCommand = lookupCommandByCString("lmove");

    /* 调试 */
    g_pserver->watchdog_period = 0;

    /* 默认情况下，我们希望脚本始终通过效果（脚本执行的单个命令）进行复制，
     * 而不是通过将脚本发送到副本/AOF。这是从 Redis 5 开始的新方法。
     * 但是，可以通过 keydb.conf 对其进行还原。 */
    g_pserver->lua_always_replicate_commands = 1;

    /* 多线程 */
    cserver.cthreads = CONFIG_DEFAULT_THREADS;
    cserver.fThreadAffinity = CONFIG_DEFAULT_THREAD_AFFINITY;

    // 这将在第二阶段初始化（我们拥有真正的数据库计数）之前被解引用
    // 因此请确保它为零并已初始化
    g_pserver->db = (redisDb**)zcalloc(sizeof(redisDb*)*std::max(cserver.dbnum, 1), MALLOC_LOCAL);

    cserver.threadAffinityOffset = 0;

    /* 客户端暂停相关 */
    g_pserver->client_pause_type = CLIENT_PAUSE_OFF;
    g_pserver->client_pause_end_time = 0;
    initConfigValues();
}

extern char **environ;

/* 重新启动服务器，执行启动此实例的相同可执行文件，
 * 使用相同的参数和配置文件。
 *
 * 该函数设计为直接调用 execve()，以便新的服务器实例将保留前一个实例的 PID。
 *
 * 标志列表（可以按位或运算组合）会改变此函数的行为：
 *
 * RESTART_SERVER_NONE 无标志。
 * RESTART_SERVER_GRACEFULLY 在重新启动之前执行正常的关闭。
 * RESTART_SERVER_CONFIG_REWRITE 在重新启动之前重写配置文件。
 *
 * 成功时，该函数不会返回，因为进程会变成另一个进程。出错时返回 C_ERR。 */
int restartServer(int flags, mstime_t delay) {
    int j;

    /* 检查我们是否仍然可以访问启动此服务器实例的可执行文件。 */
    if (access(cserver.executable,X_OK) == -1) {
        serverLog(LL_WARNING,"无法重新启动：此进程没有执行 %s 的权限", cserver.executable);
        return C_ERR;
    }

    /* 配置重写。 */
    if (flags & RESTART_SERVER_CONFIG_REWRITE &&
        cserver.configfile &&
        rewriteConfig(cserver.configfile, 0) == -1)
    {
        serverLog(LL_WARNING,"无法重新启动：配置重写过程失败");
        return C_ERR;
    }

    /* 执行正常的关闭。 */
    if (flags & RESTART_SERVER_GRACEFULLY &&
        prepareForShutdown(SHUTDOWN_NOFLAGS) != C_OK)
    {
        serverLog(LL_WARNING,"无法重新启动：准备关闭时出错");
        return C_ERR;
    }

    /* 关闭所有文件描述符，但 stdin、stdout、stderr 除外，
     * 如果我们重新启动一个非守护进程的 Redis 服务器，这些描述符会很有用。 */
    for (j = 3; j < (int)g_pserver->maxclients + 1024; j++) {
        /* 在关闭描述符之前测试其有效性，否则 Valgrind 会在 close() 上发出警告。 */
        if (fcntl(j,F_GETFD) != -1)
        {
            /* 此用户在此处仅调用 close()，但清理程序检测到这是一个 FD 竞争。
                由于我们即将调用 exec()，因此竞争无关紧要，但是我们希望减少干扰，
                因此我们要求内核在调用 exec() 时关闭，如果失败，我们才自己关闭。 */
            if (fcntl(j, F_SETFD, FD_CLOEXEC) == -1)
            {
                close(j);   // 未能设置在 exec 时关闭，在此处关闭
            }
        }
    }

    if (flags & RESTART_SERVER_GRACEFULLY) {
        if (g_pserver->m_pstorageFactory) {
            for (int idb = 0; idb < cserver.dbnum; ++idb) {
                g_pserver->db[idb]->storageProviderDelete();
            }
            delete g_pserver->metadataDb;
        }
    }

    /* 使用原始命令行执行服务器。 */
    if (delay) usleep(delay*1000);
    zfree(cserver.exec_argv[0]);
    cserver.exec_argv[0] = zstrdup(cserver.executable);
    execve(cserver.executable,cserver.exec_argv,environ);

    /* 如果此处发生错误，我们无能为力，只能退出。 */
    _exit(1);

    return C_ERR; /* 永远不会到达。 */
}

static void readOOMScoreAdj(void) {
#ifdef HAVE_PROC_OOM_SCORE_ADJ
    char buf[64];
    int fd = open("/proc/self/oom_score_adj", O_RDONLY);

    if (fd < 0) return;
    if (read(fd, buf, sizeof(buf)) > 0)
        g_pserver->oom_score_adj_base = atoi(buf);
    close(fd);
#endif
}

/* 此函数将根据用户指定的配置来配置当前进程的 oom_score_adj。
 * 目前仅在 Linux 上实现。
 *
 * process_class 值为 -1 表示 OOM_CONFIG_MASTER 或 OOM_CONFIG_REPLICA，
 * 具体取决于当前角色。
 */
int setOOMScoreAdj(int process_class) {

    if (g_pserver->oom_score_adj == OOM_SCORE_ADJ_NO) return C_OK;
    if (process_class == -1)
        process_class = (listLength(g_pserver->masters) ? CONFIG_OOM_REPLICA : CONFIG_OOM_MASTER);

    serverAssert(process_class >= 0 && process_class < CONFIG_OOM_COUNT);

#ifdef HAVE_PROC_OOM_SCORE_ADJ
    int fd;
    int val;
    char buf[64];

    val = g_pserver->oom_score_adj_values[process_class];
    if (g_pserver->oom_score_adj == OOM_SCORE_RELATIVE)
        val += g_pserver->oom_score_adj_base;
    if (val > 1000) val = 1000;
    if (val < -1000) val = -1000;

    snprintf(buf, sizeof(buf) - 1, "%d\n", val);

    fd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (fd < 0 || write(fd, buf, strlen(buf)) < 0) {
        serverLog(LL_WARNING, "Unable to write oom_score_adj: %s", strerror(errno));
        if (fd != -1) close(fd);
        return C_ERR;
    }

    close(fd);
    return C_OK;
#else
    /* 不支持 */
    return C_ERR;
#endif
}

/* 此函数将尝试根据配置的最大客户端数来提高最大打开文件数。
 * 它还为持久化、侦听套接字、日志文件等额外操作保留了一些文件描述符 (CONFIG_MIN_RESERVED_FDS)。
 *
 * 如果无法根据配置的最大客户端数设置限制，
 * 该函数将反向操作，将 g_pserver->maxclients 设置为我们实际可以处理的值。 */
void adjustOpenFilesLimit(void) {
    rlim_t maxfiles = g_pserver->maxclients+CONFIG_MIN_RESERVED_FDS;
    if (g_pserver->m_pstorageFactory)
        maxfiles += g_pserver->m_pstorageFactory->filedsRequired();
    struct rlimit limit;

    if (getrlimit(RLIMIT_NOFILE,&limit) == -1) {
        serverLog(LL_WARNING,"Unable to obtain the current NOFILE limit (%s), assuming 1024 and setting the max clients configuration accordingly.",
            strerror(errno));
        g_pserver->maxclients = 1024-CONFIG_MIN_RESERVED_FDS;
    } else {
        rlim_t oldlimit = limit.rlim_cur;

        /* 如果当前限制不足以满足我们的需求，则设置最大文件数。 */
        if (oldlimit < maxfiles) {
            rlim_t bestlimit;
            int setrlimit_error = 0;

            /* 尝试将文件限制设置为与“maxfiles”匹配，或者至少设置为支持的低于 maxfiles 的更高值。 */
            bestlimit = maxfiles;
            while(bestlimit > oldlimit) {
                rlim_t decr_step = 16;

                limit.rlim_cur = bestlimit;
                limit.rlim_max = bestlimit;
                if (setrlimit(RLIMIT_NOFILE,&limit) != -1) break;
                setrlimit_error = errno;

                /* 我们未能将文件限制设置为“bestlimit”。尝试使用较小的限制，每次迭代减少几个 FD。 */
                if (bestlimit < decr_step) break;
                bestlimit -= decr_step;
            }

            /* 假设如果我们最后一次尝试的值甚至更低，那么我们最初获得的限制仍然有效。 */
            if (bestlimit < oldlimit) bestlimit = oldlimit;

            if (bestlimit < maxfiles) {
                unsigned int old_maxclients = g_pserver->maxclients;
                g_pserver->maxclients = bestlimit-CONFIG_MIN_RESERVED_FDS;
                /* maxclients is unsigned so may overflow: in order
                 * to check if maxclients is now logically less than 1
                 * we test indirectly via bestlimit. */
                if (bestlimit <= CONFIG_MIN_RESERVED_FDS) {
                    serverLog(LL_WARNING,"Your current 'ulimit -n' "
                        "of %llu is not enough for the server to start. "
                        "Please increase your open file limit to at least "
                        "%llu. Exiting.",
                        (unsigned long long) oldlimit,
                        (unsigned long long) maxfiles);
                    exit(1);
                }
                serverLog(LL_WARNING,"You requested maxclients of %d "
                    "requiring at least %llu max file descriptors.",
                    old_maxclients,
                    (unsigned long long) maxfiles);
                serverLog(LL_WARNING,"Server can't set maximum open files "
                    "to %llu because of OS error: %s.",
                    (unsigned long long) maxfiles, strerror(setrlimit_error));
                serverLog(LL_WARNING,"Current maximum open files is %llu. "
                    "maxclients has been reduced to %d to compensate for "
                    "low ulimit. "
                    "If you need higher maxclients increase 'ulimit -n'.",
                    (unsigned long long) bestlimit, g_pserver->maxclients);
            } else {
                serverLog(LL_NOTICE,"Increased maximum number of open files "
                    "to %llu (it was originally set to %llu).",
                    (unsigned long long) maxfiles,
                    (unsigned long long) oldlimit);
            }
        }
    }
}

/* 检查 g_pserver->tcp_backlog 是否可以根据 /proc/sys/net/core/somaxconn 的值在 Linux 中实际强制执行，或者对此发出警告。 */
void checkTcpBacklogSettings(void) {
#ifdef HAVE_PROC_SOMAXCONN
    FILE *fp = fopen("/proc/sys/net/core/somaxconn","r");
    char buf[1024];
    if (!fp) return;
    if (fgets(buf,sizeof(buf),fp) != NULL) {
        int somaxconn = atoi(buf);
        if (somaxconn > 0 && somaxconn < g_pserver->tcp_backlog) {
            serverLog(LL_WARNING,"警告：无法强制执行 TCP backlog 设置 %d，因为 /proc/sys/net/core/somaxconn 设置为较低的值 %d。", g_pserver->tcp_backlog, somaxconn);
        }
    }
    fclose(fp);
#endif
}

void closeSocketListeners(socketFds *sfd) {
    int j;

    for (j = 0; j < sfd->count; j++) {
        if (sfd->fd[j] == -1) continue;

        aeDeleteFileEvent(serverTL->el, sfd->fd[j], AE_READABLE);
        close(sfd->fd[j]);
    }

    sfd->count = 0;
}

/* 为 TCP 或 TLS 域套接字中的新连接创建事件处理程序。
 * 这对所有套接字 fd 原子地起作用 */
int createSocketAcceptHandler(socketFds *sfd, aeFileProc *accept_handler) {
    int j;

    for (j = 0; j < sfd->count; j++) {
        if (aeCreateFileEvent(serverTL->el, sfd->fd[j], AE_READABLE, accept_handler,NULL) == AE_ERR) {
            /* 回滚 */
            for (j = j-1; j >= 0; j--) aeDeleteFileEvent(serverTL->el, sfd->fd[j], AE_READABLE);
            return C_ERR;
        }
    }
    return C_OK;
}

/* 初始化一组文件描述符以侦听指定的“端口”，
 * 绑定 Redis 服务器配置中指定的地址。
 *
 * 侦听文件描述符存储在整数数组“fds”中，
 * 其数量在“*count”中设置。
 *
 * 要绑定的地址在全局 g_pserver->bindaddr 数组中指定，
 * 其数量为 g_pserver->bindaddr_count。如果服务器配置
 * 不包含要绑定的特定地址，则此函数将尝试为 IPv4 和 IPv6 协议绑定 *（所有地址）。
 *
 * 成功时，函数返回 C_OK。
 *
 * 出错时，函数返回 C_ERR。对于函数出错的情况，
 * 至少有一个 g_pserver->bindaddr 地址无法绑定，
 * 或者服务器配置中未指定绑定地址，但该函数至少无法为 IPv4 或 IPv6 协议中的一个绑定 *。 */
int listenToPort(int port, socketFds *sfd, int fReusePort, int fFirstListen) {
    int j;
    const char **bindaddr = (const char**)g_pserver->bindaddr;
    int bindaddr_count = g_pserver->bindaddr_count;
    const char *default_bindaddr[2] = {"*", "-::*"};

    /* 如果未指定绑定地址，则强制绑定 0.0.0.0。 */
    if (g_pserver->bindaddr_count == 0) {
        bindaddr_count = 2;
        bindaddr = default_bindaddr;
    }

    for (j = 0; j < bindaddr_count; j++) {
        const char* addr = bindaddr[j];
        int optional = *addr == '-';
        if (optional) addr++;
        if (strchr(addr,':')) {
            /* 绑定 IPv6 地址。 */
            sfd->fd[sfd->count] = anetTcp6Server(serverTL->neterr,port,addr,g_pserver->tcp_backlog,fReusePort,fFirstListen);
        } else {
            /* 绑定 IPv4 地址。 */
            sfd->fd[sfd->count] = anetTcpServer(serverTL->neterr,port,addr,g_pserver->tcp_backlog,fReusePort,fFirstListen);
        }
        if (sfd->fd[sfd->count] == ANET_ERR) {
            int net_errno = errno;
            serverLog(LL_WARNING,
                "警告：无法创建服务器 TCP 侦听套接字 %s:%d: %s",
                addr, port, serverTL->neterr);
            if (net_errno == EADDRNOTAVAIL && optional)
                continue;
            if (net_errno == ENOPROTOOPT     || net_errno == EPROTONOSUPPORT ||
                net_errno == ESOCKTNOSUPPORT || net_errno == EPFNOSUPPORT ||
                net_errno == EAFNOSUPPORT)
                continue;

            /* 在退出前回滚成功的侦听 */
            closeSocketListeners(sfd);
            return C_ERR;
        }
        anetNonBlock(NULL,sfd->fd[sfd->count]);
        anetCloexec(sfd->fd[sfd->count]);
        sfd->count++;
    }
    return C_OK;
}

/* 重置我们通过 INFO 或其他方式公开的、并且我们希望通过 CONFIG RESETSTAT 重置的统计信息。
 * 该函数还用于在服务器启动时在 initServer() 中初始化这些字段。 */
void resetServerStats(void) {
    int j;

    g_pserver->stat_numcommands = 0;
    g_pserver->stat_numconnections = 0;
    g_pserver->stat_expiredkeys = 0;
    g_pserver->stat_expired_stale_perc = 0;
    g_pserver->stat_expired_time_cap_reached_count = 0;
    g_pserver->stat_expire_cycle_time_used = 0;
    g_pserver->stat_evictedkeys = 0;
    g_pserver->stat_keyspace_misses = 0;
    g_pserver->stat_keyspace_hits = 0;
    g_pserver->stat_active_defrag_hits = 0;
    g_pserver->stat_active_defrag_misses = 0;
    g_pserver->stat_active_defrag_key_hits = 0;
    g_pserver->stat_active_defrag_key_misses = 0;
    g_pserver->stat_active_defrag_scanned = 0;
    g_pserver->stat_fork_time = 0;
    g_pserver->stat_fork_rate = 0;
    g_pserver->stat_total_forks = 0;
    g_pserver->stat_rejected_conn = 0;
    g_pserver->stat_sync_full = 0;
    g_pserver->stat_sync_partial_ok = 0;
    g_pserver->stat_sync_partial_err = 0;
    g_pserver->stat_total_reads_processed = 0;
    g_pserver->stat_total_writes_processed = 0;
    for (j = 0; j < STATS_METRIC_COUNT; j++) {
        g_pserver->inst_metric[j].idx = 0;
        g_pserver->inst_metric[j].last_sample_time = mstime();
        g_pserver->inst_metric[j].last_sample_count = 0;
        memset(g_pserver->inst_metric[j].samples,0,
            sizeof(g_pserver->inst_metric[j].samples));
    }
    g_pserver->stat_net_input_bytes = 0;
    g_pserver->stat_net_output_bytes = 0;
    g_pserver->stat_unexpected_error_replies = 0;
    for (int iel = 0; iel < cserver.cthreads; ++iel)
        g_pserver->rgthreadvar[iel].stat_total_error_replies = 0;
    g_pserver->stat_dump_payload_sanitizations = 0;
    g_pserver->aof_delayed_fsync = 0;
}

/* 将线程设置为随时可终止，以便终止线程的函数（如崩溃报告中使用的快速内存测试所依赖的pthread_cancel）能够可靠工作。
 * （默认的线程可取消类型为 PTHREAD_CANCEL_DEFERRED，即延迟取消模式）*/
void makeThreadKillable(void) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
}

/**
 * @brief 初始化网络线程，配置TCP/TLS监听套接字及事件处理机制
 *
 * 该函数为指定事件循环索引初始化网络监听功能，支持端口复用特性。
 * 根据参数配置创建TCP/TLS监听套接字，并注册连接接受事件处理器。
 * 若初始化失败将触发程序终止。
 *
 * @param iel 事件循环索引，用于定位线程本地存储的网络资源
 * @param fReusePort 标志位指示是否启用SO_REUSEPORT特性
 * @return 无返回值（失败时直接exit()终止程序）
 */
static void initNetworkingThread(int iel, int fReusePort)
{
    /* 打开 TCP 侦听套接字以供用户命令使用。 */
    if (fReusePort || (iel == IDX_EVENT_LOOP_MAIN))
    {
        /*
         * 主线程或启用端口复用时：
         * 1. 为TCP端口创建监听套接字
         * 2. 为TLS端口创建监听套接字
         * 任一失败都将记录警告日志并终止程序
         */
        if (g_pserver->port != 0 &&
            listenToPort(g_pserver->port,&g_pserver->rgthreadvar[iel].ipfd, fReusePort, (iel == IDX_EVENT_LOOP_MAIN)) == C_ERR) {
            serverLog(LL_WARNING, "Failed listening on port %u (TCP), aborting.", g_pserver->port);
            exit(1);
        }
        if (g_pserver->tls_port != 0 &&
            listenToPort(g_pserver->tls_port,&g_pserver->rgthreadvar[iel].tlsfd, fReusePort, (iel == IDX_EVENT_LOOP_MAIN)) == C_ERR) {
            serverLog(LL_WARNING, "Failed listening on port %u (TLS), aborting.", g_pserver->port);
            exit(1);
        }
    }
    else
    {
        /*
         * 子线程复用主线程TCP套接字：
         * 复制主线程的IP文件描述符结构体
         * 保持计数器字段的显式同步
         */
        memcpy(&g_pserver->rgthreadvar[iel].ipfd, &g_pserver->rgthreadvar[IDX_EVENT_LOOP_MAIN].ipfd, sizeof(socketFds));
        g_pserver->rgthreadvar[iel].ipfd.count = g_pserver->rgthreadvar[IDX_EVENT_LOOP_MAIN].ipfd.count;
    }

    /*
     * 注册TCP连接接受事件：
     * 为每个TCP监听套接字注册READABLE事件处理器
     * 使用线程安全标志确保多线程环境下的安全性
     */
    for (int j = 0; j < g_pserver->rgthreadvar[iel].ipfd.count; j++) {
        if (aeCreateFileEvent(g_pserver->rgthreadvar[iel].el, g_pserver->rgthreadvar[iel].ipfd.fd[j], AE_READABLE|AE_READ_THREADSAFE,
            acceptTcpHandler,NULL) == AE_ERR)
            {
                serverPanic(
                    "Unrecoverable error creating g_pserver->ipfd file event.");
            }
    }

    makeThreadKillable();

    /*
     * 注册TLS连接接受事件：
     * 为每个TLS监听套接字注册READABLE事件处理器
     * 使用线程安全标志确保多线程环境下的安全性
     */
    for (int j = 0; j < g_pserver->rgthreadvar[iel].tlsfd.count; j++) {
        if (aeCreateFileEvent(g_pserver->rgthreadvar[iel].el, g_pserver->rgthreadvar[iel].tlsfd.fd[j], AE_READABLE|AE_READ_THREADSAFE,
            acceptTLSHandler,NULL) == AE_ERR)
            {
                serverPanic(
                    "Unrecoverable error creating g_pserver->tlsfd file event.");
            }
    }
}

/**
 * 初始化网络子系统，设置主线程的网络配置并创建监听套接字。
 *
 * 参数:
 *   fReusePort  整数标志，指示是否启用端口复用功能（SO_REUSEPORT）。
 *
 * 返回值:
 *   无。函数通过exit()在初始化失败时终止程序。
 */
static void initNetworking(int fReusePort)
{
    // 我们在这里只初始化主线程，因为 RDB 加载是一种特殊情况，
    // 它在我们的服务器线程启动之前处理客户端。
    initNetworkingThread(IDX_EVENT_LOOP_MAIN, fReusePort);

    /* 创建并配置Unix域套接字 */
    if (g_pserver->unixsocket != NULL) {
        unlink(g_pserver->unixsocket); /* 不关心unlink失败情况 */
        g_pserver->sofd = anetUnixServer(serverTL->neterr,g_pserver->unixsocket,
            g_pserver->unixsocketperm, g_pserver->tcp_backlog);
        if (g_pserver->sofd == ANET_ERR) {
            serverLog(LL_WARNING, "Opening Unix socket: %s", serverTL->neterr);
            exit(1);
        }
        anetNonBlock(NULL,g_pserver->sofd);
    }

    /**
     * 确保至少存在一个监听套接字（IPv4/IPv6/TLS/Unix域）。
     * 如果完全未配置监听地址，直接终止程序。
     */
    if (g_pserver->rgthreadvar[IDX_EVENT_LOOP_MAIN].ipfd.count == 0 && g_pserver->rgthreadvar[IDX_EVENT_LOOP_MAIN].tlsfd.count == 0 && g_pserver->sofd < 0) {
        serverLog(LL_WARNING, "Configured to not listen anywhere, exiting.");
        exit(1);
    }

    /**
     * 注册Unix域套接字的可读事件处理器。
     * 该事件处理将由专用线程处理传入的客户端连接。
     */
    if (g_pserver->sofd > 0 && aeCreateFileEvent(g_pserver->rgthreadvar[IDX_EVENT_LOOP_MAIN].el,g_pserver->sofd,AE_READABLE|AE_READ_THREADSAFE,
        acceptUnixHandler,NULL) == AE_ERR) serverPanic("Unrecoverable error creating g_pserver->sofd file event.");
}

static void initServerThread(struct redisServerThreadVars *pvar, int fMain)
{
    pvar->unblocked_clients = listCreate();
    pvar->clients_pending_asyncwrite = listCreate();
    pvar->ipfd.count = 0;
    pvar->tlsfd.count = 0;
    pvar->cclients = 0;
    pvar->in_eval = 0;
    pvar->in_exec = 0;
    pvar->el = aeCreateEventLoop(g_pserver->maxclients+CONFIG_FDSET_INCR);
    pvar->current_client = nullptr;
    pvar->fRetrySetAofEvent = false;
    if (pvar->el == NULL) {
        serverLog(LL_WARNING,
            "Failed creating the event loop. Error message: '%s'",
            strerror(errno));
        exit(1);
    }
    aeSetBeforeSleepProc(pvar->el, beforeSleep, AE_SLEEP_THREADSAFE);
    aeSetAfterSleepProc(pvar->el, afterSleep, AE_SLEEP_THREADSAFE);

    fastlock_init(&pvar->lockPendingWrite, "lockPendingWrite");

    if (!fMain)
    {
        if (aeCreateTimeEvent(pvar->el, 1, serverCronLite, NULL, NULL) == AE_ERR) {
            serverPanic("Can't create event loop timers.");
            exit(1);
        }
    }

    /* Register a readable event for the pipe used to awake the event loop
     * when a blocked client in a module needs attention. */
    if (aeCreateFileEvent(pvar->el, g_pserver->module_blocked_pipe[0], AE_READABLE,
        moduleBlockedClientPipeReadable,NULL) == AE_ERR) {
            serverPanic(
                "Error registering the readable event for the module "
                "blocked clients subsystem.");
    }
}

void initServer(void) {
    signal(SIGHUP, SIG_IGN);// 忽略SIGHUP信号
    signal(SIGPIPE, SIG_IGN);// 忽略SIGPIPE信号（防止在写入到已关闭连接时服务器崩溃）
    setupSignalHandlers();// 设置其它信号处理器
    makeThreadKillable(); // 确保线程可以被干净地终止

    zfree(g_pserver->db);   // initServerConfig created a dummy array, free that now
    g_pserver->db = (redisDb**)zmalloc(sizeof(redisDb*)*cserver.dbnum, MALLOC_LOCAL);// 为每个数据库分配内存并初始化

    /* Create the Redis databases, and initialize other internal state. */
    if (g_pserver->m_pstorageFactory == nullptr) { // 根据是否配置了存储工厂来创建不同类型的数据库
        for (int j = 0; j < cserver.dbnum; j++) {    // 纯内存模式
            g_pserver->db[j] = new (MALLOC_LOCAL) redisDb();
            g_pserver->db[j]->initialize(j);
        }
    } else {    // 带持久化存储的模式
        // Read FLASH metadata and load the appropriate storage dbid into each databse index, as each DB index can have different storage dbid mapped due to the swapdb command.
        g_pserver->metadataDb = g_pserver->m_pstorageFactory->createMetadataDb();    // 从元数据中恢复数据库映射关系
        for (int idb = 0; idb < cserver.dbnum; ++idb)
        {
            int storage_dbid = idb;
            std::string dbid_key = "db-" + std::to_string(idb);
            g_pserver->metadataDb->retrieve(dbid_key.c_str(), dbid_key.length(), [&](const char *, size_t, const void *data, size_t){
                storage_dbid = *(int*)data;
            });

            g_pserver->db[idb] = new (MALLOC_LOCAL) redisDb();
            g_pserver->db[idb]->initialize(idb, storage_dbid);
        }
    }

    for (int i = 0; i < MAX_EVENT_LOOPS; ++i)
    {
        g_pserver->rgthreadvar[i].rgdbSnapshot = (const redisDbPersistentDataSnapshot**)zcalloc(sizeof(redisDbPersistentDataSnapshot*)*cserver.dbnum, MALLOC_LOCAL);
        serverAssert(g_pserver->rgthreadvar[i].rgdbSnapshot != nullptr);
    }// 模块线程也需要数据库快照
    g_pserver->modulethreadvar.rgdbSnapshot = (const redisDbPersistentDataSnapshot**)zcalloc(sizeof(redisDbPersistentDataSnapshot*)*cserver.dbnum, MALLOC_LOCAL);
    serverAssert(g_pserver->modulethreadvar.rgdbSnapshot != nullptr);

    serverAssert(g_pserver->rgthreadvar[0].rgdbSnapshot != nullptr);

    /* Fixup Master Client Database */
    listIter li;
    listNode *ln;
    listRewind(g_pserver->masters, &li);
    while ((ln = listNext(&li)))
    {
        redisMaster *mi = (redisMaster*)listNodeValue(ln);
        serverAssert(mi->master == nullptr);
        if (mi->cached_master != nullptr)
            selectDb(mi->cached_master, 0);
    }

    g_pserver->aof_state = g_pserver->aof_enabled ? AOF_ON : AOF_OFF;// AOF状态
    g_pserver->hz = g_pserver->config_hz;// 设置服务器时钟频率
    cserver.pid = getpid();// 记录进程ID
    g_pserver->in_fork_child = CHILD_TYPE_NONE;
    cserver.main_thread_id = pthread_self();
    g_pserver->errors = raxNew();// 错误追踪
    g_pserver->clients_index = raxNew();// 客户端索引
    g_pserver->clients_to_close = listCreate();// 待关闭客户端列表
    g_pserver->replicaseldb = -1; /* Force to emit the first SELECT command. */
    g_pserver->ready_keys = listCreate();
    g_pserver->clients_waiting_acks = listCreate();
    g_pserver->get_ack_from_slaves = 0;
    cserver.system_memory_size = zmalloc_get_memory_size();
    g_pserver->paused_clients = listCreate();
    g_pserver->events_processed_while_blocked = 0;
    g_pserver->blocked_last_cron = 0;
    g_pserver->replication_allowed = 1;
    g_pserver->blocking_op_nesting = 0;
    g_pserver->rdb_pipe_read = -1;
    g_pserver->client_pause_type = CLIENT_PAUSE_OFF;


    if ((g_pserver->tls_port || g_pserver->tls_replication || g_pserver->tls_cluster)
            && tlsConfigure(&g_pserver->tls_ctx_config) == C_ERR) {
        serverLog(LL_WARNING, "Failed to configure TLS. Check logs for more info.");
        exit(1);
    }

    createSharedObjects();// 创建共享对象（如常用回复）
    adjustOpenFilesLimit();// 调整文件描述符限制
    const char *clk_msg = monotonicInit();// 初始化单调时钟
    serverLog(LL_NOTICE, "monotonic clock: %s", clk_msg);

    evictionPoolAlloc(); /* Initialize the LRU keys pool. */ // 初始化LRU驱逐池
    g_pserver->pubsub_channels = dictCreate(&keylistDictType,NULL);// 发布/订阅频道
    g_pserver->pubsub_patterns = dictCreate(&keylistDictType,NULL);// 发布/订阅模式
    g_pserver->cronloops = 0;// 初始化众多统计计数器
    g_pserver->child_pid = -1;
    g_pserver->child_type = CHILD_TYPE_NONE;
    g_pserver->rdbThreadVars.fRdbThreadCancel = false;
    g_pserver->rdb_child_type = RDB_CHILD_TYPE_NONE;
    g_pserver->rdb_pipe_conns = NULL;
    g_pserver->rdb_pipe_numconns = 0;
    g_pserver->rdb_pipe_numconns_writing = 0;
    g_pserver->rdb_pipe_buff = NULL;
    g_pserver->rdb_pipe_bufflen = 0;
    g_pserver->rdb_bgsave_scheduled = 0;
    g_pserver->child_info_pipe[0] = -1;
    g_pserver->child_info_pipe[1] = -1;
    g_pserver->child_info_nread = 0;
    aofRewriteBufferReset();
    g_pserver->aof_buf = sdsempty();
    g_pserver->lastsave = time(NULL); /* At startup we consider the DB saved. */
    g_pserver->lastbgsave_try = 0;    /* At startup we never tried to BGSAVE. */
    g_pserver->rdb_save_time_last = -1;
    g_pserver->rdb_save_time_start = -1;
    g_pserver->dirty = 0;
    resetServerStats();
    /* A few stats we don't want to reset: server startup time, and peak mem. */
    cserver.stat_starttime = time(NULL);
    g_pserver->stat_peak_memory = 0;
    g_pserver->stat_current_cow_bytes = 0;
    g_pserver->stat_current_cow_updated = 0;
    g_pserver->stat_current_save_keys_processed = 0;
    g_pserver->stat_current_save_keys_total = 0;
    g_pserver->stat_rdb_cow_bytes = 0;
    g_pserver->stat_aof_cow_bytes = 0;
    g_pserver->stat_module_cow_bytes = 0;
    g_pserver->stat_module_progress = 0;
    for (int j = 0; j < CLIENT_TYPE_COUNT; j++)
        g_pserver->stat_clients_type_memory[j] = 0;
    g_pserver->cron_malloc_stats.zmalloc_used = 0;
    g_pserver->cron_malloc_stats.process_rss = 0;
    g_pserver->cron_malloc_stats.allocator_allocated = 0;
    g_pserver->cron_malloc_stats.allocator_active = 0;
    g_pserver->cron_malloc_stats.allocator_resident = 0;
    g_pserver->cron_malloc_stats.sys_available = 0;
    g_pserver->cron_malloc_stats.sys_total = g_pserver->force_eviction_percent ? getMemTotal() : 0;
    g_pserver->lastbgsave_status = C_OK;
    g_pserver->aof_last_write_status = C_OK;
    g_pserver->aof_last_write_errno = 0;
    g_pserver->repl_good_slaves_count = 0;

    g_pserver->mvcc_tstamp = 0;


    /* Create the timer callback, this is our way to process many background
     * operations incrementally, like clients timeout, eviction of unaccessed
     * expired keys and so forth. */// 创建服务器周期性任务的定时器（处理超时、过期键等）
    if (aeCreateTimeEvent(g_pserver->rgthreadvar[IDX_EVENT_LOOP_MAIN].el, 1, serverCron, NULL, NULL) == AE_ERR) {
        serverPanic("Can't create event loop timers.");
        exit(1);
    }

    /* Open the AOF file if needed. */
    if (g_pserver->aof_state == AOF_ON) { //如果启用了AOF，打开AOF文件：
        g_pserver->aof_fd = open(g_pserver->aof_filename,
                               O_WRONLY|O_APPEND|O_CREAT,0644);
        if (g_pserver->aof_fd == -1) {
            serverLog(LL_WARNING, "Can't open the append-only file: %s",
                strerror(errno));
            exit(1);
        }
    }

    /* 32 bit instances are limited to 4GB of address space, so if there is
     * no explicit limit in the user provided configuration we set a limit
     * at 3 GB using maxmemory with 'noeviction' policy'. This avoids
     * useless crashes of the Redis instance for out of memory. */
    if (sizeof(void*) == 4 && g_pserver->maxmemory == 0) { //对32位系统进行内存限制保护：设置3GB的默认限制以避免OOM
        serverLog(LL_WARNING,"Warning: 32 bit instance detected but no memory limit set. Setting 3 GB maxmemory limit with 'noeviction' policy now.");
        g_pserver->maxmemory = 3072LL*(1024*1024); /* 3 GB */
        g_pserver->maxmemory_policy = MAXMEMORY_NO_EVICTION;
    }

    /* Generate UUID */// 生成服务器唯一ID
    static_assert(sizeof(uuid_t) == sizeof(cserver.uuid), "UUIDs are standardized at 16-bytes");
    uuid_generate((unsigned char*)cserver.uuid);

    if (g_pserver->cluster_enabled) clusterInit();// 集群初始化
    replicationScriptCacheInit();// 复制脚本缓存初始化
    scriptingInit(1);// Lua脚本引擎初始化
    slowlogInit();// 慢查询日志初始化
    latencyMonitorInit();// 延迟监控初始化

    if (g_pserver->m_pstorageFactory) {
        if (g_pserver->metadataDb) {
            g_pserver->metadataDb->retrieve("repl-id", 7, [&](const char *, size_t, const void *data, size_t cb){
                if (cb == sizeof(g_pserver->replid)) {
                    memcpy(g_pserver->replid, data, cb);
                }
            });
            g_pserver->metadataDb->retrieve("repl-offset", 11, [&](const char *, size_t, const void *data, size_t cb){
                if (cb == sizeof(g_pserver->master_repl_offset)) {
                    g_pserver->master_repl_offset = *(long long*)data;
                }
            });

            int repl_stream_db = -1;
            g_pserver->metadataDb->retrieve("repl-stream-db", 14, [&](const char *, size_t, const void *data, size_t){
                repl_stream_db = *(int*)data;
            });

            /* !!! AFTER THIS POINT WE CAN NO LONGER READ FROM THE META DB AS IT WILL BE OVERWRITTEN !!! */
            // replicationCacheMasterUsingMyself triggers the overwrite 

            listIter li;
            listNode *ln;
            listRewind(g_pserver->masters, &li);
            while ((ln = listNext(&li)))
            {
                redisMaster *mi = (redisMaster*)listNodeValue(ln);
                /* If we are a replica, create a cached master from this
                * information, in order to allow partial resynchronizations
                * with masters. */
                replicationCacheMasterUsingMyself(mi);
                selectDb(mi->cached_master, repl_stream_db);
            }
        }
    }

    saveMasterStatusToStorage(false); // eliminate the repl-offset field
    
    /* Initialize ACL default password if it exists */
    ACLUpdateDefaultUserPassword(g_pserver->requirepass);
}

/* Some steps in server initialization need to be done last (after modules
 * are loaded).
 * Specifically, creation of threads due to a race bug in ld.so, in which
 * Thread Local Storage initialization collides with dlopen call.
 * see: https://sourceware.org/bugzilla/show_bug.cgi?id=19329 */
void InitServerLast() {

    /* We have to initialize storage providers after the cluster has been initialized */
    moduleFireServerEvent(REDISMODULE_EVENT_LOADING, REDISMODULE_SUBEVENT_LOADING_FLASH_START, NULL);
    for (int idb = 0; idb < cserver.dbnum; ++idb)
    {   // 存储提供器初始化
        g_pserver->db[idb]->storageProviderInitialize();
    }
    moduleFireServerEvent(REDISMODULE_EVENT_LOADING, REDISMODULE_SUBEVENT_LOADING_ENDED, NULL);

    bioInit();// 后台I/O子系统初始化
    set_jemalloc_bg_thread(cserver.jemalloc_bg_thread);// 内存分配器后台线程设置
    g_pserver->initial_memory_usage = zmalloc_used_memory();// 异步工作队列创建

    g_pserver->asyncworkqueue = new (MALLOC_LOCAL) AsyncWorkQueue(cserver.cthreads);

    // Allocate the repl backlog
    
}

/* Parse the flags string description 'strflags' and set them to the
 * command 'c'. If the flags are all valid C_OK is returned, otherwise
 * C_ERR is returned (yet the recognized flags are set in the command). */
int populateCommandTableParseFlags(struct redisCommand *c, const char *strflags) {
    int argc;
    sds *argv;

    /* Split the line into arguments for processing. */
    argv = sdssplitargs(strflags,&argc);
    if (argv == NULL) return C_ERR;

    for (int j = 0; j < argc; j++) {
        char *flag = argv[j];
        if (!strcasecmp(flag,"write")) {
            c->flags |= CMD_WRITE|CMD_CATEGORY_WRITE;
        } else if (!strcasecmp(flag,"read-only")) {
            c->flags |= CMD_READONLY|CMD_CATEGORY_READ;
        } else if (!strcasecmp(flag,"use-memory")) {
            c->flags |= CMD_DENYOOM;
        } else if (!strcasecmp(flag,"admin")) {
            c->flags |= CMD_ADMIN|CMD_CATEGORY_ADMIN|CMD_CATEGORY_DANGEROUS;
        } else if (!strcasecmp(flag,"pub-sub")) {
            c->flags |= CMD_PUBSUB|CMD_CATEGORY_PUBSUB;
        } else if (!strcasecmp(flag,"no-script")) {
            c->flags |= CMD_NOSCRIPT;
        } else if (!strcasecmp(flag,"random")) {
            c->flags |= CMD_RANDOM;
        } else if (!strcasecmp(flag,"to-sort")) {
            c->flags |= CMD_SORT_FOR_SCRIPT;
        } else if (!strcasecmp(flag,"ok-loading")) {
            c->flags |= CMD_LOADING;
        } else if (!strcasecmp(flag,"ok-stale")) {
            c->flags |= CMD_STALE;
        } else if (!strcasecmp(flag,"no-monitor")) {
            c->flags |= CMD_SKIP_MONITOR;
        } else if (!strcasecmp(flag,"no-slowlog")) {
            c->flags |= CMD_SKIP_SLOWLOG;
        } else if (!strcasecmp(flag,"cluster-asking")) {
            c->flags |= CMD_ASKING;
        } else if (!strcasecmp(flag,"fast")) {
            c->flags |= CMD_FAST | CMD_CATEGORY_FAST;
        } else if (!strcasecmp(flag,"noprop")) {
            c->flags |= CMD_SKIP_PROPOGATE;
        } else if (!strcasecmp(flag,"no-auth")) {
            c->flags |= CMD_NO_AUTH;
        } else if (!strcasecmp(flag,"may-replicate")) {
            c->flags |= CMD_MAY_REPLICATE;
        } else if (!strcasecmp(flag,"async")) {
            c->flags |= CMD_ASYNC_OK;
        } else {
            /* Parse ACL categories here if the flag name starts with @. */
            uint64_t catflag;
            if (flag[0] == '@' &&
                (catflag = ACLGetCommandCategoryFlagByName(flag+1)) != 0)
            {
                c->flags |= catflag;
            } else {
                sdsfreesplitres(argv,argc);
                return C_ERR;
            }
        }
    }
    /* If it's not @fast is @slow in this binary world. */
    if (!(c->flags & CMD_CATEGORY_FAST)) c->flags |= CMD_CATEGORY_SLOW;

    sdsfreesplitres(argv,argc);
    return C_OK;
}

/* Populates the KeyDB Command Table starting from the hard coded list
 * we have on top of server.cpp file. */
void populateCommandTable(void) {
    int j;
    int numcommands = sizeof(redisCommandTable)/sizeof(struct redisCommand);

    for (j = 0; j < numcommands; j++) {
        struct redisCommand *c = redisCommandTable+j;
        int retval1, retval2;

        /* Translate the command string flags description into an actual
         * set of flags. */
        if (populateCommandTableParseFlags(c,c->sflags) == C_ERR)
            serverPanic("Unsupported command flag");

        c->id = ACLGetCommandID(c->name); /* Assign the ID used for ACL. */
        retval1 = dictAdd(g_pserver->commands, sdsnew(c->name), c);
        /* Populate an additional dictionary that will be unaffected
         * by rename-command statements in keydb.conf. */
        retval2 = dictAdd(g_pserver->orig_commands, sdsnew(c->name), c);
        serverAssert(retval1 == DICT_OK && retval2 == DICT_OK);
    }
}

void resetCommandTableStats(void) {
    struct redisCommand *c;
    dictEntry *de;
    dictIterator *di;

    di = dictGetSafeIterator(g_pserver->commands);
    while((de = dictNext(di)) != NULL) {
        c = (struct redisCommand *) dictGetVal(de);
        c->microseconds = 0;
        c->calls = 0;
        c->rejected_calls = 0;
        c->failed_calls = 0;
    }
    dictReleaseIterator(di);

}

static void zfree_noconst(void *p) {
    zfree(p);
}

void fuzzOutOfMemoryHandler(size_t allocation_size) {
    serverLog(LL_WARNING,"Out Of Memory allocating %zu bytes!",
        allocation_size);
    exit(EXIT_FAILURE); // don't crash because it causes false positives
}

void resetErrorTableStats(void) {
    raxFreeWithCallback(g_pserver->errors, zfree_noconst);
    g_pserver->errors = raxNew();
}

/* ========================== Redis OP Array API ============================ */

void redisOpArrayInit(redisOpArray *oa) {
    oa->ops = NULL;
    oa->numops = 0;
}

int redisOpArrayAppend(redisOpArray *oa, struct redisCommand *cmd, int dbid,
                       robj **argv, int argc, int target)
{
    redisOp *op;

    oa->ops = (redisOp*)zrealloc(oa->ops,sizeof(redisOp)*(oa->numops+1), MALLOC_LOCAL);
    op = oa->ops+oa->numops;
    op->cmd = cmd;
    op->dbid = dbid;
    op->argv = argv;
    op->argc = argc;
    op->target = target;
    oa->numops++;
    return oa->numops;
}

void redisOpArrayFree(redisOpArray *oa) {
    while(oa->numops) {
        int j;
        redisOp *op;

        oa->numops--;
        op = oa->ops+oa->numops;
        for (j = 0; j < op->argc; j++)
            decrRefCount(op->argv[j]);
        zfree(op->argv);
    }
    zfree(oa->ops);
    oa->ops = NULL;
}

/* ====================== Commands lookup and execution ===================== */

struct redisCommand *lookupCommand(sds name) {
    return (struct redisCommand*)dictFetchValue(g_pserver->commands, name);
}

struct redisCommand *lookupCommandByCString(const char *s) {
    struct redisCommand *cmd;
    sds name = sdsnew(s);

    cmd = (struct redisCommand*)dictFetchValue(g_pserver->commands, name);
    sdsfree(name);
    return cmd;
}

/* Lookup the command in the current table, if not found also check in
 * the original table containing the original command names unaffected by
 * keydb.conf rename-command statement.
 *
 * This is used by functions rewriting the argument vector such as
 * rewriteClientCommandVector() in order to set client->cmd pointer
 * correctly even if the command was renamed. */
struct redisCommand *lookupCommandOrOriginal(sds name) {
    struct redisCommand *cmd = (struct redisCommand*)dictFetchValue(g_pserver->commands, name);

    if (!cmd) cmd = (struct redisCommand*)dictFetchValue(g_pserver->orig_commands,name);
    return cmd;
}

/* Propagate the specified command (in the context of the specified database id)
 * to AOF and Slaves.
 *
 * flags are an xor between:
 * + PROPAGATE_NONE (no propagation of command at all)
 * + PROPAGATE_AOF (propagate into the AOF file if is enabled)
 * + PROPAGATE_REPL (propagate into the replication link)
 *
 * This should not be used inside commands implementation since it will not
 * wrap the resulting commands in MULTI/EXEC. Use instead alsoPropagate(),
 * preventCommandPropagation(), forceCommandPropagation().
 *
 * However for functions that need to (also) propagate out of the context of a
 * command execution, for example when serving a blocked client, you
 * want to use propagate().
 */
void propagate(struct redisCommand *cmd, int dbid, robj **argv, int argc,
               int flags)
{
    serverAssert(GlobalLocksAcquired());
    if (!g_pserver->replication_allowed)
        return;

    /* Propagate a MULTI request once we encounter the first command which
     * is a write command.
     * This way we'll deliver the MULTI/..../EXEC block as a whole and
     * both the AOF and the replication link will have the same consistency
     * and atomicity guarantees. */
    if (serverTL->in_exec && !serverTL->propagate_in_transaction)
        execCommandPropagateMulti(dbid);

    /* This needs to be unreachable since the dataset should be fixed during 
     * client pause, otherwise data may be lossed during a failover. */
    serverAssert(!(areClientsPaused() && !serverTL->client_pause_in_transaction));

    if (g_pserver->aof_state != AOF_OFF && flags & PROPAGATE_AOF)
        feedAppendOnlyFile(cmd,dbid,argv,argc);
    if (flags & PROPAGATE_REPL)
        replicationFeedSlaves(g_pserver->slaves,dbid,argv,argc);
}

/* Used inside commands to schedule the propagation of additional commands
 * after the current command is propagated to AOF / Replication.
 *
 * 'cmd' must be a pointer to the Redis command to replicate, dbid is the
 * database ID the command should be propagated into.
 * Arguments of the command to propagate are passed as an array of redis
 * objects pointers of len 'argc', using the 'argv' vector.
 *
 * The function does not take a reference to the passed 'argv' vector,
 * so it is up to the caller to release the passed argv (but it is usually
 * stack allocated).  The function automatically increments ref count of
 * passed objects, so the caller does not need to. */
void alsoPropagate(struct redisCommand *cmd, int dbid, robj **argv, int argc,
                   int target)
{
    robj **argvcopy;
    int j;

    if (g_pserver->loading) return; /* No propagation during loading. */

    argvcopy = (robj**)zmalloc(sizeof(robj*)*argc, MALLOC_LOCAL);
    for (j = 0; j < argc; j++) {
        argvcopy[j] = argv[j];
        incrRefCount(argv[j]);
    }
    redisOpArrayAppend(&g_pserver->also_propagate,cmd,dbid,argvcopy,argc,target);
}

/* It is possible to call the function forceCommandPropagation() inside a
 * Redis command implementation in order to to force the propagation of a
 * specific command execution into AOF / Replication. */
void forceCommandPropagation(client *c, int flags) {
    serverAssert(c->cmd->flags & (CMD_WRITE | CMD_MAY_REPLICATE));
    if (flags & PROPAGATE_REPL) c->flags |= CLIENT_FORCE_REPL;
    if (flags & PROPAGATE_AOF) c->flags |= CLIENT_FORCE_AOF;
}

/* Avoid that the executed command is propagated at all. This way we
 * are free to just propagate what we want using the alsoPropagate()
 * API. */
void preventCommandPropagation(client *c) {
    c->flags |= CLIENT_PREVENT_PROP;
}

/* AOF specific version of preventCommandPropagation(). */
void preventCommandAOF(client *c) {
    c->flags |= CLIENT_PREVENT_AOF_PROP;
}

/* Replication specific version of preventCommandPropagation(). */
void preventCommandReplication(client *c) {
    c->flags |= CLIENT_PREVENT_REPL_PROP;
}

/* Log the last command a client executed into the slowlog. */
void slowlogPushCurrentCommand(client *c, struct redisCommand *cmd, ustime_t duration) {
    /* Some commands may contain sensitive data that should not be available in the slowlog. */
    if (cmd->flags & CMD_SKIP_SLOWLOG)
        return;

    /* If command argument vector was rewritten, use the original
     * arguments. */
    robj **argv = c->original_argv ? c->original_argv : c->argv;
    int argc = c->original_argv ? c->original_argc : c->argc;
    slowlogPushEntryIfNeeded(c,argv,argc,duration);
}

/* Call() is the core of Redis execution of a command.
 *
 * The following flags can be passed:
 * CMD_CALL_NONE        No flags.
 * CMD_CALL_SLOWLOG     Check command speed and log in the slow log if needed.
 * CMD_CALL_STATS       Populate command stats.
 * CMD_CALL_PROPAGATE_AOF   Append command to AOF if it modified the dataset
 *                          or if the client flags are forcing propagation.
 * CMD_CALL_PROPAGATE_REPL  Send command to slaves if it modified the dataset
 *                          or if the client flags are forcing propagation.
 * CMD_CALL_PROPAGATE   Alias for PROPAGATE_AOF|PROPAGATE_REPL.
 * CMD_CALL_FULL        Alias for SLOWLOG|STATS|PROPAGATE.
 *
 * The exact propagation behavior depends on the client flags.
 * Specifically:
 *
 * 1. If the client flags CLIENT_FORCE_AOF or CLIENT_FORCE_REPL are set
 *    and assuming the corresponding CMD_CALL_PROPAGATE_AOF/REPL is set
 *    in the call flags, then the command is propagated even if the
 *    dataset was not affected by the command.
 * 2. If the client flags CLIENT_PREVENT_REPL_PROP or CLIENT_PREVENT_AOF_PROP
 *    are set, the propagation into AOF or to slaves is not performed even
 *    if the command modified the dataset.
 *
 * Note that regardless of the client flags, if CMD_CALL_PROPAGATE_AOF
 * or CMD_CALL_PROPAGATE_REPL are not set, then respectively AOF or
 * slaves propagation will never occur.
 *
 * Client flags are modified by the implementation of a given command
 * using the following API:
 *
 * forceCommandPropagation(client *c, int flags);
 * preventCommandPropagation(client *c);
 * preventCommandAOF(client *c);
 * preventCommandReplication(client *c);
 *
 */
void call(client *c, int flags) {
    long long dirty;
    monotime call_timer;
    int client_old_flags = c->flags;
    struct redisCommand *real_cmd = c->cmd;
    serverAssert(((flags & CMD_CALL_ASYNC) && (c->cmd->flags & CMD_READONLY)) || GlobalLocksAcquired());

    /* We need to transfer async writes before a client's repl state gets changed.  Otherwise
        we won't be able to propogate them correctly. */
    if (c->cmd->flags & CMD_CATEGORY_REPLICATION) {
        flushReplBacklogToClients();
        ProcessPendingAsyncWrites();
    }

    /* Initialization: clear the flags that must be set by the command on
     * demand, and initialize the array for additional commands propagation. */
    c->flags &= ~(CLIENT_FORCE_AOF|CLIENT_FORCE_REPL|CLIENT_PREVENT_PROP);
    redisOpArray prev_also_propagate;
    if (!(flags & CMD_CALL_ASYNC)) {
        prev_also_propagate = g_pserver->also_propagate;
        redisOpArrayInit(&g_pserver->also_propagate);
    }

    /* Call the command. */
    dirty = g_pserver->dirty;
    serverTL->prev_err_count = serverTL->stat_total_error_replies;
    g_pserver->fixed_time_expire++;
    incrementMvccTstamp();
    elapsedStart(&call_timer);
    try {
        c->cmd->proc(c);
    } catch (robj_roptr o) {
        addReply(c, o);
    } catch (robj *o) {
        addReply(c, o);
    } catch (const char *sz) {
        addReplyError(c, sz);
    }
    serverTL->commandsExecuted++;
    const long duration = elapsedUs(call_timer);
    c->duration = duration;
    if (flags & CMD_CALL_ASYNC)
        dirty = 0;  // dirty is bogus in this case as there's no synchronization
    else
        dirty = g_pserver->dirty-dirty;
    if (dirty < 0) dirty = 0;

    if (dirty)
        c->mvccCheckpoint = getMvccTstamp();

    /* Update failed command calls if required.
     * We leverage a static variable (prev_err_count) to retain
     * the counter across nested function calls and avoid logging
     * the same error twice. */
    if ((serverTL->stat_total_error_replies - serverTL->prev_err_count) > 0) {
        real_cmd->failed_calls++;
    }

    /* After executing command, we will close the client after writing entire
     * reply if it is set 'CLIENT_CLOSE_AFTER_COMMAND' flag. */
    if (c->flags & CLIENT_CLOSE_AFTER_COMMAND) {
        c->flags &= ~CLIENT_CLOSE_AFTER_COMMAND;
        c->flags |= CLIENT_CLOSE_AFTER_REPLY;
    }

    /* When EVAL is called loading the AOF we don't want commands called
     * from Lua to go into the slowlog or to populate statistics. */
    if (g_pserver->loading && c->flags & CLIENT_LUA)
        flags &= ~(CMD_CALL_SLOWLOG | CMD_CALL_STATS);

    /* If the caller is Lua, we want to force the EVAL caller to propagate
     * the script if the command flag or client flag are forcing the
     * propagation. */
    if (c->flags & CLIENT_LUA && g_pserver->lua_caller) {
        if (c->flags & CLIENT_FORCE_REPL)
            g_pserver->lua_caller->flags |= CLIENT_FORCE_REPL;
        if (c->flags & CLIENT_FORCE_AOF)
            g_pserver->lua_caller->flags |= CLIENT_FORCE_AOF;
    }

    /* Note: the code below uses the real command that was executed
     * c->cmd and c->lastcmd may be different, in case of MULTI-EXEC or
     * re-written commands such as EXPIRE, GEOADD, etc. */

    /* Record the latency this command induced on the main thread.
     * unless instructed by the caller not to log. (happens when processing
     * a MULTI-EXEC from inside an AOF). */
    if (flags & CMD_CALL_SLOWLOG) {
        const char *latency_event = (real_cmd->flags & CMD_FAST) ?
                               "fast-command" : "command";
        latencyAddSampleIfNeeded(latency_event,duration/1000);
    }

    /* Log the command into the Slow log if needed.
     * If the client is blocked we will handle slowlog when it is unblocked. */
    if ((flags & CMD_CALL_SLOWLOG) && !(c->flags & CLIENT_BLOCKED)) {
        if (duration >= g_pserver->slowlog_log_slower_than) {
            AeLocker locker;
            locker.arm(c);
            slowlogPushCurrentCommand(c, real_cmd, duration);
        }
    }

    /* Send the command to clients in MONITOR mode if applicable.
     * Administrative commands are considered too dangerous to be shown. */
    if (!(c->cmd->flags & (CMD_SKIP_MONITOR|CMD_ADMIN))) {
        robj **argv = c->original_argv ? c->original_argv : c->argv;
        int argc = c->original_argv ? c->original_argc : c->argc;
        replicationFeedMonitors(c,g_pserver->monitors,c->db->id,argv,argc);
    }

    /* Clear the original argv.
     * If the client is blocked we will handle slowlog when it is unblocked. */
    if (!(c->flags & CLIENT_BLOCKED))
        freeClientOriginalArgv(c);

    /* populate the per-command statistics that we show in INFO commandstats. */
    if (flags & CMD_CALL_STATS) {
        __atomic_fetch_add(&real_cmd->microseconds, duration, __ATOMIC_RELAXED);
        __atomic_fetch_add(&real_cmd->calls, 1, __ATOMIC_RELAXED);
    }

    /* Propagate the command into the AOF and replication link */
    if (flags & CMD_CALL_PROPAGATE &&
        (c->flags & CLIENT_PREVENT_PROP) != CLIENT_PREVENT_PROP)
    {
        int propagate_flags = PROPAGATE_NONE;

        /* Check if the command operated changes in the data set. If so
         * set for replication / AOF propagation. */
        if (dirty) propagate_flags |= (PROPAGATE_AOF|PROPAGATE_REPL);

        /* If the client forced AOF / replication of the command, set
         * the flags regardless of the command effects on the data set. */
        if (c->flags & CLIENT_FORCE_REPL) propagate_flags |= PROPAGATE_REPL;
        if (c->flags & CLIENT_FORCE_AOF) propagate_flags |= PROPAGATE_AOF;

        /* However prevent AOF / replication propagation if the command
         * implementation called preventCommandPropagation() or similar,
         * or if we don't have the call() flags to do so. */
        if (c->flags & CLIENT_PREVENT_REPL_PROP ||
            !(flags & CMD_CALL_PROPAGATE_REPL))
                propagate_flags &= ~PROPAGATE_REPL;
        if (c->flags & CLIENT_PREVENT_AOF_PROP ||
            !(flags & CMD_CALL_PROPAGATE_AOF))
                propagate_flags &= ~PROPAGATE_AOF;

        if ((c->cmd->flags & CMD_SKIP_PROPOGATE) && g_pserver->fActiveReplica)
            propagate_flags &= ~PROPAGATE_REPL;

        /* Call propagate() only if at least one of AOF / replication
         * propagation is needed. Note that modules commands handle replication
         * in an explicit way, so we never replicate them automatically. */
        if (propagate_flags != PROPAGATE_NONE && !(c->cmd->flags & CMD_MODULE))
            propagate(c->cmd,c->db->id,c->argv,c->argc,propagate_flags);
    }

    /* Restore the old replication flags, since call() can be executed
     * recursively. */
    c->flags &= ~(CLIENT_FORCE_AOF|CLIENT_FORCE_REPL|CLIENT_PREVENT_PROP);
    c->flags |= client_old_flags &
        (CLIENT_FORCE_AOF|CLIENT_FORCE_REPL|CLIENT_PREVENT_PROP);

    if (!(flags & CMD_CALL_ASYNC)) {
        /* Handle the alsoPropagate() API to handle commands that want to propagate
        * multiple separated commands. Note that alsoPropagate() is not affected
        * by CLIENT_PREVENT_PROP flag. */
        if (g_pserver->also_propagate.numops) {
            int j;
            redisOp *rop;

            if (flags & CMD_CALL_PROPAGATE) {
                bool multi_emitted = false;
                /* Wrap the commands in g_pserver->also_propagate array,
                * but don't wrap it if we are already in MULTI context,
                * in case the nested MULTI/EXEC.
                *
                * And if the array contains only one command, no need to
                * wrap it, since the single command is atomic. */
                if (g_pserver->also_propagate.numops > 1 &&
                    !(c->cmd->flags & CMD_MODULE) &&
                    !(c->flags & CLIENT_MULTI) &&
                    !(flags & CMD_CALL_NOWRAP))
                {
                    execCommandPropagateMulti(c->db->id);
                    multi_emitted = true;
                }
                
                for (j = 0; j < g_pserver->also_propagate.numops; j++) {
                    rop = &g_pserver->also_propagate.ops[j];
                    int target = rop->target;
                    /* Whatever the command wish is, we honor the call() flags. */
                    if (!(flags&CMD_CALL_PROPAGATE_AOF)) target &= ~PROPAGATE_AOF;
                    if (!(flags&CMD_CALL_PROPAGATE_REPL)) target &= ~PROPAGATE_REPL;
                    if (target)
                        propagate(rop->cmd,rop->dbid,rop->argv,rop->argc,target);
                }

                if (multi_emitted) {
                    execCommandPropagateExec(c->db->id);
                }
            }
            redisOpArrayFree(&g_pserver->also_propagate);
        }
        
        g_pserver->also_propagate = prev_also_propagate;
    }

    /* Client pause takes effect after a transaction has finished. This needs
     * to be located after everything is propagated. */
    if (!serverTL->in_exec && serverTL->client_pause_in_transaction) {
        serverTL->client_pause_in_transaction = 0;
    }

    /* If the client has keys tracking enabled for client side caching,
     * make sure to remember the keys it fetched via this command. */
    if (c->cmd->flags & CMD_READONLY) {
        client *caller = (c->flags & CLIENT_LUA && g_pserver->lua_caller) ?
                            g_pserver->lua_caller : c;
        if (caller->flags & CLIENT_TRACKING &&
            !(caller->flags & CLIENT_TRACKING_BCAST))
        {
            trackingRememberKeys(caller);
        }
    }

    __atomic_fetch_add(&g_pserver->stat_numcommands, 1, __ATOMIC_RELAXED);
    serverTL->fixed_time_expire--;
    serverTL->prev_err_count = serverTL->stat_total_error_replies;

    if (!(flags & CMD_CALL_ASYNC)) {
        /* Record peak memory after each command and before the eviction that runs
        * before the next command. */
        size_t zmalloc_used = zmalloc_used_memory();
        if (zmalloc_used > g_pserver->stat_peak_memory)
            g_pserver->stat_peak_memory = zmalloc_used;
    }
}

/* Used when a command that is ready for execution needs to be rejected, due to
 * varios pre-execution checks. it returns the appropriate error to the client.
 * If there's a transaction is flags it as dirty, and if the command is EXEC,
 * it aborts the transaction.
 * Note: 'reply' is expected to end with \r\n */
void rejectCommand(client *c, robj *reply, int severity = ERR_CRITICAL) {
    flagTransaction(c);
    if (c->cmd) c->cmd->rejected_calls++;
    if (c->cmd && c->cmd->proc == execCommand) {
        execCommandAbort(c, szFromObj(reply));
    }
    else {
        /* using addReplyError* rather than addReply so that the error can be logged. */
        addReplyErrorObject(c, reply, severity);
    }
}

void lfenceCommand(client *c) {
    c->mvccCheckpoint = getMvccTstamp();
    addReply(c, shared.ok);
}

void rejectCommandFormat(client *c, const char *fmt, ...) {
    if (c->cmd) c->cmd->rejected_calls++;
    flagTransaction(c);
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted (The args come from the user, they may contain any character). */
    sdsmapchars(s, "\r\n", "  ",  2);
    if (c->cmd && c->cmd->proc == execCommand) {
        execCommandAbort(c, s);
        sdsfree(s);
    } else {
        /* The following frees 's'. */
        addReplyErrorSds(c, s);
    }
}

/* Returns 1 for commands that may have key names in their arguments, but have
 * no pre-determined key positions. */
static int cmdHasMovableKeys(struct redisCommand *cmd) {
    return (cmd->getkeys_proc && !(cmd->flags & CMD_MODULE)) ||
            cmd->flags & CMD_MODULE_GETKEYS;
}

/* If this function gets called we already read a whole
 * command, arguments are in the client argv/argc fields.
 * processCommand() execute the command or prepare the
 * server for a bulk read from the client.
 *
 * If C_OK is returned the client is still alive and valid and
 * other operations can be performed by the caller. Otherwise
 * if C_ERR is returned the client was destroyed (i.e. after QUIT). */
int processCommand(client *c, int callFlags) {
    AssertCorrectThread(c);
    serverAssert((callFlags & CMD_CALL_ASYNC) || GlobalLocksAcquired());
    if (!g_pserver->lua_timedout) {
        /* Both EXEC and EVAL call call() directly so there should be
         * no way in_exec or in_eval or propagate_in_transaction is 1.
         * That is unless lua_timedout, in which case client may run
         * some commands. Also possible that some other thread set
         * propagate_in_transaction if this is an async command. */
        serverAssert(!serverTL->propagate_in_transaction);
        serverAssert(!serverTL->in_exec);
        serverAssert(!serverTL->in_eval);
    }

    if (moduleHasCommandFilters())
    {
        moduleCallCommandFilters(c);
    }

    /* The QUIT command is handled separately. Normal command procs will
     * go through checking for replication and QUIT will cause trouble
     * when FORCE_REPLICATION is enabled and would be implemented in
     * a regular command proc. */
    if (!strcasecmp((const char*)ptrFromObj(c->argv[0]),"quit")) {
        addReply(c,shared.ok);
        c->flags |= CLIENT_CLOSE_AFTER_REPLY;
        return C_ERR;
    }

    /* Now lookup the command and check ASAP about trivial error conditions
     * such as wrong arity, bad command name and so forth. */
    c->cmd = c->lastcmd = lookupCommand((sds)ptrFromObj(c->argv[0]));
    if (!c->cmd) {
        sds args = sdsempty();
        int i;
        for (i=1; i < c->argc && sdslen(args) < 128; i++)
            args = sdscatprintf(args, "`%.*s`, ", 128-(int)sdslen(args), (char*)ptrFromObj(c->argv[i]));
        rejectCommandFormat(c,"unknown command `%s`, with args beginning with: %s",
            (char*)ptrFromObj(c->argv[0]), args);
        sdsfree(args);
        return C_OK;
    } else if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) ||
               (c->argc < -c->cmd->arity)) {
        rejectCommandFormat(c,"wrong number of arguments for '%s' command",
            c->cmd->name);
        return C_OK;
    }

    int is_read_command = (c->cmd->flags & CMD_READONLY) ||
                           (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_READONLY));
    int is_write_command = (c->cmd->flags & CMD_WRITE) ||
                           (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_WRITE));
    int is_denyoom_command = (c->cmd->flags & CMD_DENYOOM) ||
                             (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_DENYOOM));
    int is_denystale_command = !(c->cmd->flags & CMD_STALE) ||
                               (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_STALE));
    int is_denyloading_command = !(c->cmd->flags & CMD_LOADING) ||
                                 (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_LOADING));
    int is_may_replicate_command = (c->cmd->flags & (CMD_WRITE | CMD_MAY_REPLICATE)) ||
                                   (c->cmd->proc == execCommand && (c->mstate.cmd_flags & (CMD_WRITE | CMD_MAY_REPLICATE)));

    if (authRequired(c)) {
        /* AUTH and HELLO and no auth commands are valid even in
         * non-authenticated state. */
        if (!(c->cmd->flags & CMD_NO_AUTH)) {
            rejectCommand(c,shared.noautherr);
            return C_OK;
        }
    }

    /* Check if the user can run this command according to the current
     * ACLs. */
    int acl_errpos;
    int acl_retval = ACLCheckAllPerm(c,&acl_errpos);
    if (acl_retval != ACL_OK) {
        addACLLogEntry(c,acl_retval,acl_errpos,NULL);
        switch (acl_retval) {
        case ACL_DENIED_CMD:
            rejectCommandFormat(c,
                "-NOPERM this user has no permissions to run "
                "the '%s' command or its subcommand", c->cmd->name);
            break;
        case ACL_DENIED_KEY:
            rejectCommandFormat(c,
                "-NOPERM this user has no permissions to access "
                "one of the keys used as arguments");
            break;
        case ACL_DENIED_CHANNEL:
            rejectCommandFormat(c,
                "-NOPERM this user has no permissions to access "
                "one of the channels used as arguments");
            break;
        default:
            rejectCommandFormat(c, "no permission");
            break;
        }
        return C_OK;
    }

    /* If cluster is enabled perform the cluster redirection here.
     * However we don't perform the redirection if:
     * 1) The sender of this command is our master.
     * 2) The command has no key arguments. */
    if (g_pserver->cluster_enabled &&
        !(c->flags & CLIENT_MASTER) &&
        !(c->flags & CLIENT_LUA &&
          g_pserver->lua_caller->flags & CLIENT_MASTER) &&
        !(!cmdHasMovableKeys(c->cmd) && c->cmd->firstkey == 0 &&
          c->cmd->proc != execCommand))
    {
        int hashslot;
        int error_code;
        clusterNode *n = getNodeByQuery(c,c->cmd,c->argv,c->argc,
                                        &hashslot,&error_code);
        if (n == NULL || n != g_pserver->cluster->myself) {
            if (c->cmd->proc == execCommand) {
                discardTransaction(c);
            } else {
                flagTransaction(c);
            }
            clusterRedirectClient(c,n,hashslot,error_code);
            c->cmd->rejected_calls++;
            return C_OK;
        }
    }

    /* Handle the maxmemory directive.
     *
     * Note that we do not want to reclaim memory if we are here re-entering
     * the event loop since there is a busy Lua script running in timeout
     * condition, to avoid mixing the propagation of scripts with the
     * propagation of DELs due to eviction. */
    if (g_pserver->maxmemory && !g_pserver->lua_timedout && !(callFlags & CMD_CALL_ASYNC)) {
        int out_of_memory = (performEvictions(false /*fPreSnapshot*/) == EVICT_FAIL);
        /* freeMemoryIfNeeded may flush replica output buffers. This may result
         * into a replica, that may be the active client, to be freed. */
        if (serverTL->current_client == NULL) return C_ERR;

        int reject_cmd_on_oom = is_denyoom_command;
        /* If client is in MULTI/EXEC context, queuing may consume an unlimited
         * amount of memory, so we want to stop that.
         * However, we never want to reject DISCARD, or even EXEC (unless it
         * contains denied commands, in which case is_denyoom_command is already
         * set. */
        if (c->flags & CLIENT_MULTI &&
            c->cmd->proc != execCommand &&
            c->cmd->proc != discardCommand &&
            c->cmd->proc != resetCommand) {
            reject_cmd_on_oom = 1;
        }

        if (out_of_memory && reject_cmd_on_oom) {
            rejectCommand(c, shared.oomerr);
            return C_OK;
        }

        /* Save out_of_memory result at script start, otherwise if we check OOM
         * until first write within script, memory used by lua stack and
         * arguments might interfere. */
        if (c->cmd->proc == evalCommand || c->cmd->proc == evalShaCommand) {
            g_pserver->lua_oom = out_of_memory;
        }
    }

    /* Make sure to use a reasonable amount of memory for client side
     * caching metadata. */
    if (g_pserver->tracking_clients) trackingLimitUsedSlots();

    
    /* Don't accept write commands if there are problems persisting on disk
        * and if this is a master instance. */
    int deny_write_type = writeCommandsDeniedByDiskError();
    if (deny_write_type != DISK_ERROR_TYPE_NONE &&
        listLength(g_pserver->masters) == 0 &&
        (is_write_command ||c->cmd->proc == pingCommand))
    {
        if (deny_write_type == DISK_ERROR_TYPE_RDB)
            rejectCommand(c, shared.bgsaveerr);
        else
            rejectCommandFormat(c,
                "-MISCONF Errors writing to the AOF file: %s",
                strerror(g_pserver->aof_last_write_errno));
        return C_OK;
    }    

    /* Don't accept write commands if there are not enough good slaves and
    * user configured the min-slaves-to-write option. */
    if (listLength(g_pserver->masters) == 0 &&
        g_pserver->repl_min_slaves_to_write &&
        g_pserver->repl_min_slaves_max_lag &&
        is_write_command &&
        g_pserver->repl_good_slaves_count < g_pserver->repl_min_slaves_to_write)
    {
        rejectCommand(c, shared.noreplicaserr);
        return C_OK;
    }

    /* Don't accept write commands if this is a read only replica. But
    * accept write commands if this is our master. */
    if (listLength(g_pserver->masters) && g_pserver->repl_slave_ro &&
        !(c->flags & CLIENT_MASTER) &&
        is_write_command)
    {
        rejectCommand(c, shared.roslaveerr);
        return C_OK;
    }

    /* Only allow a subset of commands in the context of Pub/Sub if the
     * connection is in RESP2 mode. With RESP3 there are no limits. */
    if ((c->flags & CLIENT_PUBSUB && c->resp == 2) &&
        c->cmd->proc != pingCommand &&
        c->cmd->proc != subscribeCommand &&
        c->cmd->proc != unsubscribeCommand &&
        c->cmd->proc != psubscribeCommand &&
        c->cmd->proc != punsubscribeCommand &&
        c->cmd->proc != resetCommand) {
        rejectCommandFormat(c,
            "Can't execute '%s': only (P)SUBSCRIBE / "
            "(P)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context",
            c->cmd->name);
        return C_OK;
    }

    if (listLength(g_pserver->masters))
    {
        /* Only allow commands with flag "t", such as INFO, SLAVEOF and so on,
        * when replica-serve-stale-data is no and we are a replica with a broken
        * link with master. */
        if (FBrokenLinkToMaster() &&
            g_pserver->repl_serve_stale_data == 0 &&
            is_denystale_command &&
            !(g_pserver->fActiveReplica && c->cmd->proc == syncCommand)
            && !FInReplicaReplay())
        {
            rejectCommand(c, shared.masterdownerr);
            return C_OK;
        }
    }

    /* Loading DB? Return an error if the command has not the
     * CMD_LOADING flag. */
    if (g_pserver->loading && is_denyloading_command) {
        /* Active Replicas can execute read only commands, and optionally write commands */
        if (!(g_pserver->loading == LOADING_REPLICATION && g_pserver->fActiveReplica && ((c->cmd->flags & CMD_READONLY) || g_pserver->fWriteDuringActiveLoad)))
        {
            rejectCommand(c, shared.loadingerr, ERR_WARNING);
            return C_OK;
        }
    }

    /* Lua script too slow? Only allow a limited number of commands.
     * Note that we need to allow the transactions commands, otherwise clients
     * sending a transaction with pipelining without error checking, may have
     * the MULTI plus a few initial commands refused, then the timeout
     * condition resolves, and the bottom-half of the transaction gets
     * executed, see Github PR #7022. */
    if (g_pserver->lua_timedout &&
          c->cmd->proc != authCommand &&
          c->cmd->proc != helloCommand &&
          c->cmd->proc != replconfCommand &&
          c->cmd->proc != multiCommand &&
          c->cmd->proc != discardCommand &&
          c->cmd->proc != watchCommand &&
          c->cmd->proc != unwatchCommand &&
          c->cmd->proc != resetCommand &&
        !(c->cmd->proc == shutdownCommand &&
          c->argc == 2 &&
          tolower(((char*)ptrFromObj(c->argv[1]))[0]) == 'n') &&
        !(c->cmd->proc == scriptCommand &&
          c->argc == 2 &&
          tolower(((char*)ptrFromObj(c->argv[1]))[0]) == 'k'))
    {
        rejectCommand(c, shared.slowscripterr);
        return C_OK;
    }

    /* Prevent a replica from sending commands that access the keyspace.
     * The main objective here is to prevent abuse of client pause check
     * from which replicas are exempt. */
    if ((c->flags & CLIENT_SLAVE) && (is_may_replicate_command || is_write_command || is_read_command)) {
        rejectCommandFormat(c, "Replica can't interract with the keyspace");
        return C_OK;
    }

    /* If the server is paused, block the client until
     * the pause has ended. Replicas are never paused. */
    if (!(c->flags & CLIENT_SLAVE) && 
        ((g_pserver->client_pause_type == CLIENT_PAUSE_ALL) ||
        (g_pserver->client_pause_type == CLIENT_PAUSE_WRITE && is_may_replicate_command)))
    {
        c->bpop.timeout = 0;
        blockClient(c,BLOCKED_PAUSE);
        return C_OK;       
    }

    /* Exec the command */
    if (c->flags & CLIENT_MULTI &&
        c->cmd->proc != execCommand && c->cmd->proc != discardCommand &&
        c->cmd->proc != multiCommand && c->cmd->proc != watchCommand &&
        c->cmd->proc != resetCommand)
    {
        queueMultiCommand(c);
        addReply(c,shared.queued);
    } else {
        /* If the command was replication or admin related we *must* flush our buffers first.  This is in case
            something happens which would modify what we would send to replicas */
        if (c->cmd->flags & (CMD_MODULE | CMD_ADMIN))
            flushReplBacklogToClients();

        if (c->flags & CLIENT_AUDIT_LOGGING){
            getKeysResult result = GETKEYS_RESULT_INIT;
            int numkeys = getKeysFromCommand(c->cmd, c->argv, c->argc, &result);
            int *keyindex = result.keys;

            sds str = sdsempty();
            for (int j = 0; j < numkeys; j++) {
                str = sdscatsds(str, (sds)ptrFromObj(c->argv[keyindex[j]]));
                str = sdscat(str, " ");
            }
        
            if (numkeys > 0)
            {
                serverLog(LL_NOTICE, "Audit Log: %s, cmd %s, keys: %s", c->fprint, c->cmd->name, str);
            } else {
                serverLog(LL_NOTICE, "Audit Log: %s, cmd %s", c->fprint, c->cmd->name);
            }
            sdsfree(str);
        }

        call(c,callFlags);
        c->woff = g_pserver->master_repl_offset;

        if (c->cmd->flags & (CMD_MODULE | CMD_ADMIN))
            flushReplBacklogToClients();
        
        if (listLength(g_pserver->ready_keys))
            handleClientsBlockedOnKeys();
    }

    return C_OK;
}

bool client::postFunction(std::function<void(client *)> fn, bool fLock) {
    this->casyncOpsPending++;
    return aePostFunction(g_pserver->rgthreadvar[this->iel].el, [this, fn]{
        std::lock_guard<decltype(this->lock)> lock(this->lock);
        fn(this);
        --casyncOpsPending;
    }, fLock) == AE_OK;
}

std::vector<robj_sharedptr> clientArgs(client *c) {
    std::vector<robj_sharedptr> args;
    for (int j = 0; j < c->argc; j++) {
        args.push_back(robj_sharedptr(c->argv[j]));
    }
    return args;
}

bool client::asyncCommand(std::function<void(const redisDbPersistentDataSnapshot *, const std::vector<robj_sharedptr> &)> &&mainFn, 
                            std::function<void(const redisDbPersistentDataSnapshot *)> &&postFn) 
{
    serverAssert(FCorrectThread(this));
    if (serverTL->in_eval)
        return false;   // we cannot block clients in EVAL
    const redisDbPersistentDataSnapshot *snapshot = nullptr;
    if (!(this->flags & (CLIENT_MULTI | CLIENT_BLOCKED)))
        snapshot = this->db->createSnapshot(this->mvccCheckpoint, false /* fOptional */);
    if (snapshot == nullptr) {
        return false;
    }
    aeEventLoop *el = serverTL->el;
    blockClient(this, BLOCKED_ASYNC);
    g_pserver->asyncworkqueue->AddWorkFunction([el, this, mainFn, postFn, snapshot] {
        std::vector<robj_sharedptr> args = clientArgs(this);
        aePostFunction(el, [this, mainFn, postFn, snapshot, args] {
            aeReleaseLock();
            std::unique_lock<decltype(this->lock)> lock(this->lock);
            AeLocker locker;
            locker.arm(this);
            unblockClient(this);
            mainFn(snapshot, args);
            locker.disarm();
            lock.unlock();
            if (postFn)
                postFn(snapshot);
            this->db->endSnapshotAsync(snapshot);
            aeAcquireLock();
        });
    });
    return true;
}

/* ====================== Error lookup and execution ===================== */

void incrementErrorCount(const char *fullerr, size_t namelen) {
    struct redisError *error = (struct redisError*)raxFind(g_pserver->errors,(unsigned char*)fullerr,namelen);
    if (error == raxNotFound) {
        error = (struct redisError*)zmalloc(sizeof(*error));
        error->count = 0;
        raxInsert(g_pserver->errors,(unsigned char*)fullerr,namelen,error,NULL);
    }
    error->count++;
}

/*================================== Shutdown =============================== */

/* Close listening sockets. Also unlink the unix domain socket if
 * unlink_unix_socket is non-zero. */
void closeListeningSockets(int unlink_unix_socket) {
    int j;

    for (int iel = 0; iel < cserver.cthreads; ++iel)
    {
        for (j = 0; j < g_pserver->rgthreadvar[iel].ipfd.count; j++) 
            close(g_pserver->rgthreadvar[iel].ipfd.fd[j]);
        for (j = 0; j < g_pserver->rgthreadvar[iel].tlsfd.count; j++)
            close(g_pserver->rgthreadvar[iel].tlsfd.fd[j]);
    }
    if (g_pserver->sofd != -1) close(g_pserver->sofd);
    if (g_pserver->cluster_enabled)
        for (j = 0; j < g_pserver->cfd.count; j++) close(g_pserver->cfd.fd[j]);
    if (unlink_unix_socket && g_pserver->unixsocket) {
        serverLog(LL_NOTICE,"Removing the unix socket file.");
        unlink(g_pserver->unixsocket); /* don't care if this fails */
    }
}

int prepareForShutdown(int flags) {
    /* 当服务器正在将数据集加载到内存中时调用 SHUTDOWN，
     * 我们需要确保在关闭时不尝试保存数据集
     * （否则可能会用半读取的数据覆盖当前的数据库）。
     *
     * 另外，在 Sentinel 模式下，清除 SAVE 标志并强制 NOSAVE。 */
    if (g_pserver->loading || g_pserver->sentinel_mode)
        flags = (flags & ~SHUTDOWN_SAVE) | SHUTDOWN_NOSAVE;

    int save = flags & SHUTDOWN_SAVE;
    int nosave = flags & SHUTDOWN_NOSAVE;

    serverLog(LL_WARNING,"User requested shutdown...");
    if (cserver.supervised_mode == SUPERVISED_SYSTEMD)
        redisCommunicateSystemd("STOPPING=1\n");

    /* 杀死所有 Lua 调试器派生的会话。 */
    ldbKillForkedSessions();

    /* 如果有后台保存正在进行，则杀死保存子进程。
       我们希望避免竞争条件，例如我们的保存子进程可能会
       覆盖 SHUTDOWN 执行的同步保存。 */
    if (g_pserver->FRdbSaveInProgress()) {
        serverLog(LL_WARNING,"There is a child saving an .rdb. Killing it!");
        killRDBChild();
        /* 注意，在 killRDBChild 中，通常由 backgroundSaveDoneHandler
         * 执行清理工作，但在这种情况下，此代码不会被执行，
         * 因此我们需要调用 rdbRemoveTempFile，它将在后台线程中关闭 fd
         * （以便实际取消链接文件）。
         * 当 redis 快速退出时，临时 rdb 文件 fd 可能不会关闭，
         * 但操作系统会在进程退出时关闭此 fd。 */
        rdbRemoveTempFile(g_pserver->rdbThreadVars.tmpfileNum, 0);
    }

    /* 如果有模块子进程，则杀死它。 */
    if (g_pserver->child_type == CHILD_TYPE_MODULE) {
        serverLog(LL_WARNING,"There is a module fork child. Killing it!");
        TerminateModuleForkChild(g_pserver->child_pid,0);
    }

    if (g_pserver->aof_state != AOF_OFF) {
        /* 杀死 AOF 保存子进程，因为我们已有的 AOF 可能更长，
         * 但无论如何都包含完整的数据集。 */
        if (g_pserver->child_type == CHILD_TYPE_AOF) {
            /* 如果我们启用了 AOF 但尚未写入 AOF，则不要关闭，
             * 否则数据集将丢失。 */
            if (g_pserver->aof_state == AOF_WAIT_REWRITE) {
                serverLog(LL_WARNING, "Writing initial AOF, can't exit.");
                return C_ERR;
            }
            serverLog(LL_WARNING,
                "There is a child rewriting the AOF. Killing it!");
            killAppendOnlyChild();
        }
        /* 仅追加文件：在退出时刷新缓冲区并对 AOF 执行 fsync() */
        serverLog(LL_NOTICE,"Calling fsync() on the AOF file.");
        flushAppendOnlyFile(1);
        if (redis_fsync(g_pserver->aof_fd) == -1) {
            serverLog(LL_WARNING,"Fail to fsync the AOF file: %s.",
                                 strerror(errno));
        }
    }

    /* 在退出前创建一个新的 RDB 文件。 */
    if ((g_pserver->saveparamslen > 0 && !nosave) || save) {
        serverLog(LL_NOTICE,"Saving the final RDB snapshot before exiting.");
        if (cserver.supervised_mode == SUPERVISED_SYSTEMD)
            redisCommunicateSystemd("STATUS=Saving the final RDB snapshot\n");
        /* 快照。执行同步保存并退出 */
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        if (rdbSave(nullptr, rsiptr) != C_OK) {
            /* 糟糕.. 保存错误！我们能做的最好的事情就是继续操作。
             * 请注意，如果存在后台保存进程，
             * 在下一个 cron() 中，Redis 将收到后台保存中止的通知，
             * 处理诸如等待同步的从属服务器之类的特殊情况... */
            serverLog(LL_WARNING,"Error trying to save the DB, can't exit.");
            if (cserver.supervised_mode == SUPERVISED_SYSTEMD)
                redisCommunicateSystemd("STATUS=Error trying to save the DB, can't exit.\n");
            return C_ERR;
        }

        // 如果适用，也转储到 FLASH
        for (int idb = 0; idb < cserver.dbnum; ++idb) {
            if (g_pserver->db[idb]->processChanges(false))
                g_pserver->db[idb]->commitChanges();
        }
        saveMasterStatusToStorage(true);
    }

    /* 触发关闭模块事件。 */
    moduleFireServerEvent(REDISMODULE_EVENT_SHUTDOWN,0,NULL);

    /* 如果可能且需要，则删除 pid 文件。 */
    if (cserver.daemonize || cserver.pidfile) {
        serverLog(LL_NOTICE,"正在删除 pid 文件。");
        unlink(cserver.pidfile);
    }

    if (g_pserver->repl_batch_idxStart >= 0) {
        flushReplBacklogToClients();
        g_pserver->repl_batch_offStart = -1;
        g_pserver->repl_batch_idxStart = -1;
    }

    /* 尽力刷新副本输出缓冲区，以便我们有希望向它们发送待处理的写入。 */
    flushSlavesOutputBuffers();
    g_pserver->repl_batch_idxStart = -1;
    g_pserver->repl_batch_offStart = -1;

    /* 关闭侦听套接字。显然，这可以加快重启速度。 */
    closeListeningSockets(1);

    if (g_pserver->asyncworkqueue)
    {
        aeReleaseLock();
        g_pserver->asyncworkqueue->shutdown();
        aeAcquireLock();
    }

    for (int iel = 0; iel < cserver.cthreads; ++iel)
    {
        aePostFunction(g_pserver->rgthreadvar[iel].el, [iel]{
            g_pserver->rgthreadvar[iel].el->stop = 1;
        });
    }

    serverLog(LL_WARNING,"%s 现在准备退出，再见...",
        g_pserver->sentinel_mode ? "Sentinel" : "KeyDB");

    return C_OK;
}

/*================================== 命令 =============================== */

/* 有时 Redis 无法接受写命令，因为 RDB 或 AOF 文件存在持久化错误，
 * 并且 Redis 配置为在这种情况下停止接受写入。
 * 此函数返回此种情况是否处于活动状态以及情况的类型。
 *
 * 函数返回值：
 *
 * DISK_ERROR_TYPE_NONE:    没有问题，我们可以接受写入。
 * DISK_ERROR_TYPE_AOF:     不接受写入：AOF 错误。
 * DISK_ERROR_TYPE_RDB:     不接受写入：RDB 错误。
 */
int writeCommandsDeniedByDiskError(void) {
    if (g_pserver->stop_writes_on_bgsave_err &&
        g_pserver->saveparamslen > 0 &&
        g_pserver->lastbgsave_status == C_ERR)
    {
        return DISK_ERROR_TYPE_RDB;
    } else if (g_pserver->aof_state != AOF_OFF) {
        if (g_pserver->aof_last_write_status == C_ERR) {
            return DISK_ERROR_TYPE_AOF;
        }
        /* AOF fsync error. */
        int aof_bio_fsync_status;
        atomicGet(g_pserver->aof_bio_fsync_status,aof_bio_fsync_status);
        if (aof_bio_fsync_status == C_ERR) {
            atomicGet(g_pserver->aof_bio_fsync_errno,g_pserver->aof_last_write_errno);
            return DISK_ERROR_TYPE_AOF;
        }
    }

    return DISK_ERROR_TYPE_NONE;
}

/* PING 命令。如果客户端处于发布/订阅模式，则其工作方式有所不同。 */
void pingCommand(client *c) {
    /* 该命令接受零个或一个参数。 */
    if (c->argc > 2) {
        addReplyErrorFormat(c,"wrong number of arguments for '%s' command",
            c->cmd->name);
        return;
    }

    if (g_pserver->soft_shutdown && !(c->flags & CLIENT_IGNORE_SOFT_SHUTDOWN)) {
        addReplyError(c, "-SHUTDOWN PENDING");
        return;
    }

    if (c->flags & CLIENT_PUBSUB && c->resp == 2) {
        addReply(c,shared.mbulkhdr[2]);
        addReplyBulkCBuffer(c,"pong",4);
        if (c->argc == 1)
            addReplyBulkCBuffer(c,"",0);
        else
            addReplyBulk(c,c->argv[1]);
    } else {
        if (c->argc == 1)
            addReply(c,shared.pong);
        else
            addReplyBulk(c,c->argv[1]);
    }
}

void echoCommand(client *c) {
    addReplyBulk(c,c->argv[1]);
}

void timeCommand(client *c) {
    struct timeval tv;

    /* gettimeofday() 仅当 &tv 是无效地址时才会失败，因此我们不检查错误。 */
    gettimeofday(&tv,NULL);
    addReplyArrayLen(c,2);
    addReplyBulkLongLong(c,tv.tv_sec);
    addReplyBulkLongLong(c,tv.tv_usec);
}

/* addReplyCommand() 的辅助函数，用于输出标志。 */
int addReplyCommandFlag(client *c, struct redisCommand *cmd, int f, const char *reply) {
    if (cmd->flags & f) {
        addReplyStatus(c, reply);
        return 1;
    }
    return 0;
}

/* 输出 Redis 命令的表示形式。由 COMMAND 命令使用。 */
void addReplyCommand(client *c, struct redisCommand *cmd) {
    if (!cmd) {
        addReplyNull(c);
    } else {
        /* We are adding: command name, arg count, flags, first, last, offset, categories */
        addReplyArrayLen(c, 7);
        addReplyBulkCString(c, cmd->name);
        addReplyLongLong(c, cmd->arity);

        int flagcount = 0;
        void *flaglen = addReplyDeferredLen(c);
        flagcount += addReplyCommandFlag(c,cmd,CMD_WRITE, "write");
        flagcount += addReplyCommandFlag(c,cmd,CMD_READONLY, "readonly");
        flagcount += addReplyCommandFlag(c,cmd,CMD_DENYOOM, "denyoom");
        flagcount += addReplyCommandFlag(c,cmd,CMD_ADMIN, "admin");
        flagcount += addReplyCommandFlag(c,cmd,CMD_PUBSUB, "pubsub");
        flagcount += addReplyCommandFlag(c,cmd,CMD_NOSCRIPT, "noscript");
        flagcount += addReplyCommandFlag(c,cmd,CMD_RANDOM, "random");
        flagcount += addReplyCommandFlag(c,cmd,CMD_SORT_FOR_SCRIPT,"sort_for_script");
        flagcount += addReplyCommandFlag(c,cmd,CMD_LOADING, "loading");
        flagcount += addReplyCommandFlag(c,cmd,CMD_STALE, "stale");
        flagcount += addReplyCommandFlag(c,cmd,CMD_SKIP_MONITOR, "skip_monitor");
        flagcount += addReplyCommandFlag(c,cmd,CMD_SKIP_SLOWLOG, "skip_slowlog");
        flagcount += addReplyCommandFlag(c,cmd,CMD_ASKING, "asking");
        flagcount += addReplyCommandFlag(c,cmd,CMD_FAST, "fast");
        flagcount += addReplyCommandFlag(c,cmd,CMD_NO_AUTH, "no_auth");
        flagcount += addReplyCommandFlag(c,cmd,CMD_MAY_REPLICATE, "may_replicate");
        if (cmdHasMovableKeys(cmd)) {
            addReplyStatus(c, "movablekeys");
            flagcount += 1;
        }
        setDeferredSetLen(c, flaglen, flagcount);

        addReplyLongLong(c, cmd->firstkey);
        addReplyLongLong(c, cmd->lastkey);
        addReplyLongLong(c, cmd->keystep);

        addReplyCommandCategories(c,cmd);
    }
}

/* COMMAND <subcommand> <args> */
void commandCommand(client *c) {
    dictIterator *di;
    dictEntry *de;

    if (c->argc == 2 && !strcasecmp((const char*)ptrFromObj(c->argv[1]),"help")) {
        const char *help[] = {
"(no subcommand)",
"    Return details about all KeyDB commands.",
"COUNT",
"    Return the total number of commands in this KeyDB server.",
"GETKEYS <full-command>",
"    Return the keys from a full KeyDB command.",
"INFO [<command-name> ...]",
"    Return details about multiple KeyDB commands.",
NULL
        };
        addReplyHelp(c, help);
    } else if (c->argc == 1) {
        addReplyArrayLen(c, dictSize(g_pserver->commands));
        di = dictGetIterator(g_pserver->commands);
        while ((de = dictNext(di)) != NULL) {
            addReplyCommand(c, (redisCommand*)dictGetVal(de));
        }
        dictReleaseIterator(di);
    } else if (!strcasecmp((const char*)ptrFromObj(c->argv[1]), "info")) {
        int i;
        addReplyArrayLen(c, c->argc-2);
        for (i = 2; i < c->argc; i++) {
            addReplyCommand(c, (redisCommand*)dictFetchValue(g_pserver->commands, ptrFromObj(c->argv[i])));
        }
    } else if (!strcasecmp((const char*)ptrFromObj(c->argv[1]), "count") && c->argc == 2) {
        addReplyLongLong(c, dictSize(g_pserver->commands));
    } else if (!strcasecmp((const char*)ptrFromObj(c->argv[1]),"getkeys") && c->argc >= 3) {
        struct redisCommand *cmd = (redisCommand*)lookupCommand((sds)ptrFromObj(c->argv[2]));
        getKeysResult result = GETKEYS_RESULT_INIT;
        int j;

        if (!cmd) {
            addReplyError(c,"Invalid command specified");
            return;
        } else if (cmd->getkeys_proc == NULL && cmd->firstkey == 0) {
            addReplyError(c,"The command has no key arguments");
            return;
        } else if ((cmd->arity > 0 && cmd->arity != c->argc-2) ||
                   ((c->argc-2) < -cmd->arity))
        {
            addReplyError(c,"Invalid number of arguments specified for command");
            return;
        }

        if (!getKeysFromCommand(cmd,c->argv+2,c->argc-2,&result)) {
            addReplyError(c,"Invalid arguments specified for command");
        } else {
            addReplyArrayLen(c,result.numkeys);
            for (j = 0; j < result.numkeys; j++) addReplyBulk(c,c->argv[result.keys[j]+2]);
        }
        getKeysFreeResult(&result);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

/* Convert an amount of bytes into a human readable string in the form
 * of 100B, 2G, 100M, 4K, and so forth. */
void bytesToHuman(char *s, unsigned long long n, size_t bufsize) {
    double d;

    if (n < 1024) {
        /* Bytes */
        snprintf(s,bufsize,"%lluB",n);
    } else if (n < (1024*1024)) {
        d = (double)n/(1024);
        snprintf(s,bufsize,"%.2fK",d);
    } else if (n < (1024LL*1024*1024)) {
        d = (double)n/(1024*1024);
        snprintf(s,bufsize,"%.2fM",d);
    } else if (n < (1024LL*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024);
        snprintf(s,bufsize,"%.2fG",d);
    } else if (n < (1024LL*1024*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024*1024);
        snprintf(s,bufsize,"%.2fT",d);
    } else if (n < (1024LL*1024*1024*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024*1024*1024);
        snprintf(s,bufsize,"%.2fP",d);
    } else {
        /* Let's hope we never need this */
        snprintf(s,bufsize,"%lluB",n);
    }
}

/* Characters we sanitize on INFO output to maintain expected format. */
static char unsafe_info_chars[] = "#:\n\r";
static char unsafe_info_chars_substs[] = "____";   /* Must be same length as above */

/* Returns a sanitized version of s that contains no unsafe info string chars.
 * If no unsafe characters are found, simply returns s. Caller needs to
 * free tmp if it is non-null on return.
 */
const char *getSafeInfoString(const char *s, size_t len, char **tmp) {
    *tmp = NULL;
    if (mempbrk(s, len, unsafe_info_chars,sizeof(unsafe_info_chars)-1)
        == NULL) return s;
    char *_new = *tmp = (char*)zmalloc(len + 1);
    memcpy(_new, s, len);
    _new[len] = '\0';
    return memmapchars(_new, len, unsafe_info_chars, unsafe_info_chars_substs,
                       sizeof(unsafe_info_chars)-1);
}

/* Create the string returned by the INFO command. This is decoupled
 * by the INFO command itself as we need to report the same information
 * on memory corruption problems. */
sds genRedisInfoString(const char *section) {
    sds info = sdsempty();
    time_t uptime = g_pserver->unixtime-cserver.stat_starttime;
    int j;
    int allsections = 0, defsections = 0, everything = 0, modules = 0;
    int sections = 0;

    if (section == NULL) section = "default";
    allsections = strcasecmp(section,"all") == 0;
    defsections = strcasecmp(section,"default") == 0;
    everything = strcasecmp(section,"everything") == 0;
    modules = strcasecmp(section,"modules") == 0;
    if (everything) allsections = 1;

    /* Server */
    if (allsections || defsections || !strcasecmp(section,"server")) {
        static int call_uname = 1;
        static struct utsname name;
        const char *mode;
        const char *supervised;

        if (g_pserver->cluster_enabled) mode = "cluster";
        else if (g_pserver->sentinel_mode) mode = "sentinel";
        else mode = "standalone";

        if (cserver.supervised) {
            if (cserver.supervised_mode == SUPERVISED_UPSTART) supervised = "upstart";
            else if (cserver.supervised_mode == SUPERVISED_SYSTEMD) supervised = "systemd";
            else supervised = "unknown";
        } else {
            supervised = "no";
        }

        if (sections++) info = sdscat(info,"\r\n");

        if (call_uname) {
            /* Uname can be slow and is always the same output. Cache it. */
            uname(&name);
            call_uname = 0;
        }

        unsigned int lruclock = g_pserver->lruclock.load();
        ustime_t ustime;
        __atomic_load(&g_pserver->ustime, &ustime, __ATOMIC_RELAXED);
        info = sdscatfmt(info,
            "# Server\r\n"
            "redis_version:%s\r\n"
            "redis_git_sha1:%s\r\n"
            "redis_git_dirty:%i\r\n"
            "redis_build_id:%s\r\n"
            "redis_mode:%s\r\n"
            "os:%s %s %s\r\n"
            "arch_bits:%i\r\n"
            "multiplexing_api:%s\r\n"
            "atomicvar_api:%s\r\n"
            "gcc_version:%i.%i.%i\r\n"
            "process_id:%I\r\n"
            "process_supervised:%s\r\n"
            "run_id:%s\r\n"
            "tcp_port:%i\r\n"
            "server_time_usec:%I\r\n"
            "uptime_in_seconds:%I\r\n"
            "uptime_in_days:%I\r\n"
            "hz:%i\r\n"
            "configured_hz:%i\r\n"
            "lru_clock:%u\r\n"
            "executable:%s\r\n"
            "config_file:%s\r\n"
            "availability_zone:%s\r\n"
            "features:%s\r\n",
            KEYDB_SET_VERSION,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            redisBuildIdString(),
            mode,
            name.sysname, name.release, name.machine,
            (int)sizeof(void*)*8,
            aeGetApiName(),
            REDIS_ATOMIC_API,
#ifdef __GNUC__
            __GNUC__,__GNUC_MINOR__,__GNUC_PATCHLEVEL__,
#else
            0,0,0,
#endif
            (int64_t) getpid(),
            supervised,
            g_pserver->runid,
            g_pserver->port ? g_pserver->port : g_pserver->tls_port,
            (int64_t)ustime,
            (int64_t)uptime,
            (int64_t)(uptime/(3600*24)),
            g_pserver->hz.load(),
            g_pserver->config_hz,
            lruclock,
            cserver.executable ? cserver.executable : "",
            cserver.configfile ? cserver.configfile : "",
            g_pserver->sdsAvailabilityZone,
            "cluster_mget");
    }

    /* Clients */
    if (allsections || defsections || !strcasecmp(section,"clients")) {
        size_t maxin, maxout;
        getExpansiveClientsInfo(&maxin,&maxout);
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Clients\r\n"
            "connected_clients:%lu\r\n"
            "cluster_connections:%lu\r\n"
            "maxclients:%u\r\n"
            "client_recent_max_input_buffer:%zu\r\n"
            "client_recent_max_output_buffer:%zu\r\n"
            "blocked_clients:%d\r\n"
            "tracking_clients:%d\r\n"
            "clients_in_timeout_table:%" PRIu64 "\r\n"
            "current_client_thread:%d\r\n",
            listLength(g_pserver->clients)-listLength(g_pserver->slaves),
            getClusterConnectionsCount(),
            g_pserver->maxclients,
            maxin, maxout,
            g_pserver->blocked_clients,
            g_pserver->tracking_clients,
            raxSize(g_pserver->clients_timeout_table),
            static_cast<int>(serverTL - g_pserver->rgthreadvar));
        for (int ithread = 0; ithread < cserver.cthreads; ++ithread)
        {
            info = sdscatprintf(info,
                "thread_%d_clients:%d\r\n",
                ithread, g_pserver->rgthreadvar[ithread].cclients);
        }
    }

    /* Memory */
    if (allsections || defsections || !strcasecmp(section,"memory")) {
        char hmem[64];
        char peak_hmem[64];
        char total_system_hmem[64];
        char used_memory_lua_hmem[64];
        char used_memory_scripts_hmem[64];
        char used_memory_rss_hmem[64];
        char maxmemory_hmem[64];
        size_t zmalloc_used = zmalloc_used_memory();
        size_t total_system_mem = cserver.system_memory_size;
        const char *evict_policy = evictPolicyToString();
        long long memory_lua = g_pserver->lua ? (long long)lua_gc(g_pserver->lua,LUA_GCCOUNT,0)*1024 : 0;
        struct redisMemOverhead *mh = getMemoryOverheadData();
        char available_system_mem[64] = "unavailable";

        /* Peak memory is updated from time to time by serverCron() so it
         * may happen that the instantaneous value is slightly bigger than
         * the peak value. This may confuse users, so we update the peak
         * if found smaller than the current memory usage. */
        /* 峰值内存会由 serverCron() 定期更新，因此瞬时值可能略大于
         * 峰值。这可能会让用户感到困惑，所以如果发现峰值
         * 小于当前内存使用量，我们会更新峰值。 */
        if (zmalloc_used > g_pserver->stat_peak_memory) // 如果当前已分配内存大于记录的峰值内存
            g_pserver->stat_peak_memory = zmalloc_used; // 更新峰值内存为当前已分配内存

        if (g_pserver->cron_malloc_stats.sys_available) { // 如果系统可用内存信息存在 (cron_malloc_stats.sys_available 非零)
            snprintf(available_system_mem, 64, "%lu", g_pserver->cron_malloc_stats.sys_available); // 将系统可用内存格式化为字符串
        }

        bytesToHuman(hmem,zmalloc_used,sizeof(hmem)); // 将已用内存大小 (zmalloc_used) 转换为人类可读格式 (存入 hmem)
        bytesToHuman(peak_hmem,g_pserver->stat_peak_memory,sizeof(peak_hmem)); // 将峰值内存大小 (g_pserver->stat_peak_memory) 转换为人类可读格式 (存入 peak_hmem)
        bytesToHuman(total_system_hmem,total_system_mem,sizeof(total_system_hmem)); // 将系统总内存大小 (total_system_mem) 转换为人类可读格式 (存入 total_system_hmem)
        bytesToHuman(used_memory_lua_hmem,memory_lua,sizeof(used_memory_lua_hmem)); // 将 Lua 使用的内存大小 (memory_lua) 转换为人类可读格式 (存入 used_memory_lua_hmem)
        bytesToHuman(used_memory_scripts_hmem,mh->lua_caches,sizeof(used_memory_scripts_hmem)); // 将 Lua 脚本缓存使用的内存大小 (mh->lua_caches) 转换为人类可读格式 (存入 used_memory_scripts_hmem)
        bytesToHuman(used_memory_rss_hmem,g_pserver->cron_malloc_stats.process_rss,sizeof(used_memory_rss_hmem)); // 将进程 RSS 内存大小 (g_pserver->cron_malloc_stats.process_rss) 转换为人类可读格式 (存入 used_memory_rss_hmem)
        bytesToHuman(maxmemory_hmem,g_pserver->maxmemory,sizeof(maxmemory_hmem)); // 将最大内存限制 (g_pserver->maxmemory) 转换为人类可读格式 (存入 maxmemory_hmem)

        if (sections++) info = sdscat(info,"\r\n"); // 如果 sections 非零 (表示之前已有其他信息段)，则在 info 字符串后追加换行符
        info = sdscatprintf(info, // 将格式化后的内存信息追加到 info 字符串
            "# Memory\r\n" // 内存信息段的标题
            "used_memory:%zu\r\n" // 已用内存 (字节)
            "used_memory_human:%s\r\n" // 已用内存 (人类可读格式)
            "used_memory_rss:%zu\r\n" // 进程 RSS 内存 (字节)
            "used_memory_rss_human:%s\r\n"
            "used_memory_peak:%zu\r\n"
            "used_memory_peak_human:%s\r\n"
            "used_memory_peak_perc:%.2f%%\r\n"
            "used_memory_overhead:%zu\r\n"
            "used_memory_startup:%zu\r\n"
            "used_memory_dataset:%zu\r\n"
            "used_memory_dataset_perc:%.2f%%\r\n"
            "allocator_allocated:%zu\r\n"
            "allocator_active:%zu\r\n"
            "allocator_resident:%zu\r\n"
            "total_system_memory:%lu\r\n"
            "total_system_memory_human:%s\r\n"
            "used_memory_lua:%lld\r\n"
            "used_memory_lua_human:%s\r\n"
            "used_memory_scripts:%lld\r\n"
            "used_memory_scripts_human:%s\r\n"
            "number_of_cached_scripts:%lu\r\n"
            "maxmemory:%lld\r\n"
            "maxmemory_human:%s\r\n"
            "maxmemory_policy:%s\r\n"
            "allocator_frag_ratio:%.2f\r\n"
            "allocator_frag_bytes:%zu\r\n"
            "allocator_rss_ratio:%.2f\r\n"
            "allocator_rss_bytes:%zd\r\n"
            "rss_overhead_ratio:%.2f\r\n"
            "rss_overhead_bytes:%zd\r\n"
            "mem_fragmentation_ratio:%.2f\r\n"
            "mem_fragmentation_bytes:%zd\r\n"
            "mem_not_counted_for_evict:%zu\r\n"
            "mem_replication_backlog:%zu\r\n"
            "mem_clients_slaves:%zu\r\n"
            "mem_clients_normal:%zu\r\n"
            "mem_aof_buffer:%zu\r\n"
            "mem_allocator:%s\r\n"
            "active_defrag_running:%d\r\n"
            "lazyfree_pending_objects:%zu\r\n"
            "lazyfreed_objects:%zu\r\n"
            "storage_provider:%s\r\n"
            "available_system_memory:%s\r\n",
            zmalloc_used,
            hmem,
            g_pserver->cron_malloc_stats.process_rss,
            used_memory_rss_hmem,
            g_pserver->stat_peak_memory,
            peak_hmem,
            mh->peak_perc,
            mh->overhead_total,
            mh->startup_allocated,
            mh->dataset,
            mh->dataset_perc,
            g_pserver->cron_malloc_stats.allocator_allocated,
            g_pserver->cron_malloc_stats.allocator_active,
            g_pserver->cron_malloc_stats.allocator_resident,
            (unsigned long)total_system_mem,
            total_system_hmem,
            memory_lua,
            used_memory_lua_hmem,
            (long long) mh->lua_caches,
            used_memory_scripts_hmem,
            dictSize(g_pserver->lua_scripts),
            g_pserver->maxmemory,
            maxmemory_hmem,
            evict_policy,
            mh->allocator_frag,
            mh->allocator_frag_bytes,
            mh->allocator_rss,
            mh->allocator_rss_bytes,
            mh->rss_extra,
            mh->rss_extra_bytes,
            mh->total_frag,       /* This is the total RSS overhead, including
                                     fragmentation, but not just it. This field
                                     (and the next one) is named like that just
                                     for backward compatibility. */
            mh->total_frag_bytes,
            freeMemoryGetNotCountedMemory(),
            mh->repl_backlog,
            mh->clients_slaves,
            mh->clients_normal,
            mh->aof_buffer,
            ZMALLOC_LIB,
            g_pserver->active_defrag_running,
            lazyfreeGetPendingObjectsCount(),
            lazyfreeGetFreedObjectsCount(),
            g_pserver->m_pstorageFactory ? g_pserver->m_pstorageFactory->name() : "none",
            available_system_mem
        );
        freeMemoryOverheadData(mh);
    }

    /* Persistence */
    if (allsections || defsections || !strcasecmp(section,"persistence")) {
        if (sections++) info = sdscat(info,"\r\n");
        double fork_perc = 0;
        if (g_pserver->stat_module_progress) {
            fork_perc = g_pserver->stat_module_progress * 100;
        } else if (g_pserver->stat_current_save_keys_total) {
            fork_perc = ((double)g_pserver->stat_current_save_keys_processed / g_pserver->stat_current_save_keys_total) * 100;
        }
        int aof_bio_fsync_status;
        atomicGet(g_pserver->aof_bio_fsync_status,aof_bio_fsync_status);

        info = sdscatprintf(info,
            "# Persistence\r\n"
            "loading:%d\r\n"
            "current_cow_size:%zu\r\n"
            "current_cow_size_age:%lu\r\n"
            "current_fork_perc:%.2f\r\n"
            "current_save_keys_processed:%zu\r\n"
            "current_save_keys_total:%zu\r\n"
            "rdb_changes_since_last_save:%lld\r\n"
            "rdb_bgsave_in_progress:%d\r\n"
            "rdb_last_save_time:%jd\r\n"
            "rdb_last_bgsave_status:%s\r\n"
            "rdb_last_bgsave_time_sec:%jd\r\n"
            "rdb_current_bgsave_time_sec:%jd\r\n"
            "rdb_last_cow_size:%zu\r\n"
            "aof_enabled:%d\r\n"
            "aof_rewrite_in_progress:%d\r\n"
            "aof_rewrite_scheduled:%d\r\n"
            "aof_last_rewrite_time_sec:%jd\r\n"
            "aof_current_rewrite_time_sec:%jd\r\n"
            "aof_last_bgrewrite_status:%s\r\n"
            "aof_last_write_status:%s\r\n"
            "aof_last_cow_size:%zu\r\n"
            "module_fork_in_progress:%d\r\n"
            "module_fork_last_cow_size:%zu\r\n",
            !!g_pserver->loading.load(std::memory_order_relaxed),   /* Note: libraries expect 1 or 0 here so coerce our enum */
            g_pserver->stat_current_cow_bytes,
            g_pserver->stat_current_cow_updated ? (unsigned long) elapsedMs(g_pserver->stat_current_cow_updated) / 1000 : 0,
            fork_perc,
            g_pserver->stat_current_save_keys_processed,
            g_pserver->stat_current_save_keys_total,
            g_pserver->dirty,
            g_pserver->FRdbSaveInProgress(),
            (intmax_t)g_pserver->lastsave,
            (g_pserver->lastbgsave_status == C_OK) ? "ok" : "err",
            (intmax_t)g_pserver->rdb_save_time_last,
            (intmax_t)(g_pserver->FRdbSaveInProgress() ?
                time(NULL)-g_pserver->rdb_save_time_start : -1),
            g_pserver->stat_rdb_cow_bytes,
            g_pserver->aof_state != AOF_OFF,
            g_pserver->child_type == CHILD_TYPE_AOF,
            g_pserver->aof_rewrite_scheduled,
            (intmax_t)g_pserver->aof_rewrite_time_last,
            (intmax_t)((g_pserver->child_type != CHILD_TYPE_AOF) ?
                -1 : time(NULL)-g_pserver->aof_rewrite_time_start),
            (g_pserver->aof_lastbgrewrite_status == C_OK) ? "ok" : "err",
            (g_pserver->aof_last_write_status == C_OK &&
                aof_bio_fsync_status == C_OK) ? "ok" : "err",
            g_pserver->stat_aof_cow_bytes,
            g_pserver->child_type == CHILD_TYPE_MODULE,
            g_pserver->stat_module_cow_bytes);

        if (g_pserver->aof_enabled) {
            info = sdscatprintf(info,
                "aof_current_size:%lld\r\n"
                "aof_base_size:%lld\r\n"
                "aof_pending_rewrite:%d\r\n"
                "aof_buffer_length:%zu\r\n"
                "aof_rewrite_buffer_length:%lu\r\n"
                "aof_pending_bio_fsync:%llu\r\n"
                "aof_delayed_fsync:%lu\r\n",
                (long long) g_pserver->aof_current_size,
                (long long) g_pserver->aof_rewrite_base_size,
                g_pserver->aof_rewrite_scheduled,
                sdslen(g_pserver->aof_buf),
                aofRewriteBufferSize(),
                bioPendingJobsOfType(BIO_AOF_FSYNC),
                g_pserver->aof_delayed_fsync);
        }

        if (g_pserver->loading) {
            double perc = 0;
            time_t eta, elapsed;
            off_t remaining_bytes = 1;

            if (g_pserver->loading_total_bytes) {
                perc = ((double)g_pserver->loading_loaded_bytes / g_pserver->loading_total_bytes) * 100;
                remaining_bytes = g_pserver->loading_total_bytes - g_pserver->loading_loaded_bytes;
            } else if(g_pserver->loading_rdb_used_mem) {
                perc = ((double)g_pserver->loading_loaded_bytes / g_pserver->loading_rdb_used_mem) * 100;
                remaining_bytes = g_pserver->loading_rdb_used_mem - g_pserver->loading_loaded_bytes;
                /* used mem is only a (bad) estimation of the rdb file size, avoid going over 100% */
                if (perc > 99.99) perc = 99.99;
                if (remaining_bytes < 1) remaining_bytes = 1;
            }

            elapsed = time(NULL)-g_pserver->loading_start_time;
            if (elapsed == 0) {
                eta = 1; /* A fake 1 second figure if we don't have
                            enough info */
            } else {
                eta = (elapsed*remaining_bytes)/(g_pserver->loading_loaded_bytes+1);
            }

            info = sdscatprintf(info,
                "loading_start_time:%jd\r\n"
                "loading_total_bytes:%llu\r\n"
                "loading_rdb_used_mem:%llu\r\n"
                "loading_loaded_bytes:%llu\r\n"
                "loading_loaded_perc:%.2f\r\n"
                "loading_eta_seconds:%jd\r\n",
                (intmax_t) g_pserver->loading_start_time,
                (unsigned long long) g_pserver->loading_total_bytes,
                (unsigned long long) g_pserver->loading_rdb_used_mem,
                (unsigned long long) g_pserver->loading_loaded_bytes,
                perc,
                (intmax_t)eta
            );
        }
        if (g_pserver->m_pstorageFactory)
        {
            info = sdscat(info, g_pserver->m_pstorageFactory->getInfo().get());
        }
    }

    /* Stats */
    if (allsections || defsections || !strcasecmp(section,"stats")) {
        double avgLockContention = 0;
        for (unsigned i = 0; i < redisServer::s_lockContentionSamples; ++i)
            avgLockContention += g_pserver->rglockSamples[i];
        avgLockContention /= redisServer::s_lockContentionSamples;

        long long stat_total_reads_processed, stat_total_writes_processed;
        long long stat_net_input_bytes, stat_net_output_bytes;
        stat_total_reads_processed = g_pserver->stat_total_reads_processed.load(std::memory_order_relaxed);
        stat_total_writes_processed = g_pserver->stat_total_writes_processed.load(std::memory_order_relaxed);
        stat_net_input_bytes = g_pserver->stat_net_input_bytes.load(std::memory_order_relaxed);
        stat_net_output_bytes = g_pserver->stat_net_output_bytes.load(std::memory_order_relaxed);

        long long stat_total_error_replies = 0;
        for (int iel = 0; iel < cserver.cthreads; ++iel)
            stat_total_error_replies += g_pserver->rgthreadvar[iel].stat_total_error_replies;

        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Stats\r\n"
            "total_connections_received:%lld\r\n"
            "total_commands_processed:%lld\r\n"
            "instantaneous_ops_per_sec:%lld\r\n"
            "total_net_input_bytes:%lld\r\n"
            "total_net_output_bytes:%lld\r\n"
            "instantaneous_input_kbps:%.2f\r\n"
            "instantaneous_output_kbps:%.2f\r\n"
            "rejected_connections:%lld\r\n"
            "sync_full:%lld\r\n"
            "sync_partial_ok:%lld\r\n"
            "sync_partial_err:%lld\r\n"
            "expired_keys:%lld\r\n"
            "expired_stale_perc:%.2f\r\n"
            "expired_time_cap_reached_count:%lld\r\n"
            "expire_cycle_cpu_milliseconds:%lld\r\n"
            "evicted_keys:%lld\r\n"
            "keyspace_hits:%lld\r\n"
            "keyspace_misses:%lld\r\n"
            "pubsub_channels:%ld\r\n"
            "pubsub_patterns:%lu\r\n"
            "latest_fork_usec:%lld\r\n"
            "total_forks:%lld\r\n"
            "migrate_cached_sockets:%ld\r\n"
            "slave_expires_tracked_keys:%zu\r\n"
            "active_defrag_hits:%lld\r\n"
            "active_defrag_misses:%lld\r\n"
            "active_defrag_key_hits:%lld\r\n"
            "active_defrag_key_misses:%lld\r\n"
            "tracking_total_keys:%lld\r\n"
            "tracking_total_items:%lld\r\n"
            "tracking_total_prefixes:%lld\r\n"
            "unexpected_error_replies:%lld\r\n"
            "total_error_replies:%lld\r\n"
            "dump_payload_sanitizations:%lld\r\n"
            "total_reads_processed:%lld\r\n"
            "total_writes_processed:%lld\r\n"
            "instantaneous_lock_contention:%d\r\n"
            "avg_lock_contention:%f\r\n"
            "storage_provider_read_hits:%lld\r\n"
            "storage_provider_read_misses:%lld\r\n",
            g_pserver->stat_numconnections,
            g_pserver->stat_numcommands,
            getInstantaneousMetric(STATS_METRIC_COMMAND),
            stat_net_input_bytes,
            stat_net_output_bytes,
            (float)getInstantaneousMetric(STATS_METRIC_NET_INPUT)/1024,
            (float)getInstantaneousMetric(STATS_METRIC_NET_OUTPUT)/1024,
            g_pserver->stat_rejected_conn,
            g_pserver->stat_sync_full,
            g_pserver->stat_sync_partial_ok,
            g_pserver->stat_sync_partial_err,
            g_pserver->stat_expiredkeys,
            g_pserver->stat_expired_stale_perc*100,
            g_pserver->stat_expired_time_cap_reached_count,
            g_pserver->stat_expire_cycle_time_used/1000,
            g_pserver->stat_evictedkeys,
            g_pserver->stat_keyspace_hits,
            g_pserver->stat_keyspace_misses,
            dictSize(g_pserver->pubsub_channels),
            dictSize(g_pserver->pubsub_patterns),
            g_pserver->stat_fork_time,
            g_pserver->stat_total_forks,
            dictSize(g_pserver->migrate_cached_sockets),
            getSlaveKeyWithExpireCount(),
            g_pserver->stat_active_defrag_hits,
            g_pserver->stat_active_defrag_misses,
            g_pserver->stat_active_defrag_key_hits,
            g_pserver->stat_active_defrag_key_misses,
            (unsigned long long) trackingGetTotalKeys(),
            (unsigned long long) trackingGetTotalItems(),
            (unsigned long long) trackingGetTotalPrefixes(),
            g_pserver->stat_unexpected_error_replies,
            stat_total_error_replies,
            g_pserver->stat_dump_payload_sanitizations,
            stat_total_reads_processed,
            stat_total_writes_processed,
            aeLockContention(),
            avgLockContention,
            g_pserver->stat_storage_provider_read_hits,
            g_pserver->stat_storage_provider_read_misses);
    }

    /* Replication */
    if (allsections || defsections || !strcasecmp(section,"replication")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Replication\r\n"
            "role:%s\r\n",
            listLength(g_pserver->masters) == 0 ? "master" 
                : g_pserver->fActiveReplica ? "active-replica" : "slave");
        if (listLength(g_pserver->masters)) {
            int connectedMasters = 0;
            info = sdscatprintf(info, "master_global_link_status:%s\r\n",
                FBrokenLinkToMaster(&connectedMasters) ? "down" : "up");

            info = sdscatprintf(info, "connected_masters:%d\r\n", connectedMasters);

            int cmasters = 0;
            listIter li;
            listNode *ln;
            listRewind(g_pserver->masters, &li);
            while ((ln = listNext(&li)))
            {
                long long slave_repl_offset = 1;
                long long slave_read_repl_offset = 1;
                redisMaster *mi = (redisMaster*)listNodeValue(ln);

                if (mi->master){
                    slave_repl_offset = mi->master->reploff;
                    slave_read_repl_offset = mi->master->read_reploff;
                } else if (mi->cached_master){
                    slave_repl_offset = mi->cached_master->reploff;
                    slave_read_repl_offset = mi->cached_master->read_reploff;
                }

                char master_prefix[128] = "";
                if (cmasters != 0) {
                    snprintf(master_prefix, sizeof(master_prefix), "_%d", cmasters);
                }

                info = sdscatprintf(info,
                    "master%s_host:%s\r\n"
                    "master%s_port:%d\r\n"
                    "master%s_link_status:%s\r\n"
                    "master%s_last_io_seconds_ago:%d\r\n"
                    "master%s_sync_in_progress:%d\r\n"
                    "slave_read_repl_offset:%lld\r\n"
                    "slave_repl_offset:%lld\r\n"
                    ,master_prefix, mi->masterhost,
                    master_prefix, mi->masterport,
                    master_prefix, (mi->repl_state == REPL_STATE_CONNECTED) ?
                        "up" : "down",
                    master_prefix, mi->master ?
                    ((int)(g_pserver->unixtime-mi->master->lastinteraction)) : -1,
                    master_prefix, mi->repl_state == REPL_STATE_TRANSFER,
                    slave_read_repl_offset, 
                    slave_repl_offset
                );

                if (mi->repl_state == REPL_STATE_TRANSFER) {
                    double perc = 0;
                    if (mi->repl_transfer_size) {
                        perc = ((double)mi->repl_transfer_read / mi->repl_transfer_size) * 100;
                    }
                    info = sdscatprintf(info,
                        "master%s_sync_total_bytes:%lld\r\n"
                        "master%s_sync_read_bytes:%lld\r\n"
                        "master%s_sync_left_bytes:%lld\r\n"
                        "master%s_sync_perc:%.2f\r\n"
                        "master%s_sync_last_io_seconds_ago:%d\r\n",
                        master_prefix, (long long) mi->repl_transfer_size,
                        master_prefix, (long long) mi->repl_transfer_read,
                        master_prefix, (long long) (mi->repl_transfer_size - mi->repl_transfer_read),
                        master_prefix, perc,
                        master_prefix, (int)(g_pserver->unixtime-mi->repl_transfer_lastio)
                    );
                }

                if (mi->repl_state != REPL_STATE_CONNECTED) {
                    info = sdscatprintf(info,
                        "master%s_link_down_since_seconds:%jd\r\n",
                        master_prefix, mi->repl_down_since ? 
                            (intmax_t)(g_pserver->unixtime-mi->repl_down_since) : -1);
                }
                ++cmasters;
            }
            info = sdscatprintf(info,
                "slave_priority:%d\r\n"
                "slave_read_only:%d\r\n"
                "replica_announced:%d\r\n",
                g_pserver->slave_priority,
                g_pserver->repl_slave_ro,
                g_pserver->replica_announced);
        }

        info = sdscatprintf(info,
            "connected_slaves:%lu\r\n",
            listLength(g_pserver->slaves));

        /* If min-slaves-to-write is active, write the number of slaves
         * currently considered 'good'. */
        if (g_pserver->repl_min_slaves_to_write &&
            g_pserver->repl_min_slaves_max_lag) {
            info = sdscatprintf(info,
                "min_slaves_good_slaves:%d\r\n",
                g_pserver->repl_good_slaves_count);
        }

        if (listLength(g_pserver->slaves)) {
            int slaveid = 0;
            listNode *ln;
            listIter li;

            listRewind(g_pserver->slaves,&li);
            while((ln = listNext(&li))) {
                client *replica = (client*)listNodeValue(ln);
                const char *state = NULL;
                char ip[NET_IP_STR_LEN], *slaveip = replica->slave_addr;
                int port;
                long lag = 0;

                if (!slaveip) {
                    if (connPeerToString(replica->conn,ip,sizeof(ip),&port) == -1)
                        continue;
                    slaveip = ip;
                }
                switch(replica->replstate) {
                case SLAVE_STATE_WAIT_BGSAVE_START:
                case SLAVE_STATE_WAIT_BGSAVE_END:
                    state = "wait_bgsave";
                    break;
                case SLAVE_STATE_SEND_BULK:
                    state = "send_bulk";
                    break;
                case SLAVE_STATE_ONLINE:
                    state = "online";
                    break;
                }
                if (state == NULL) continue;
                if (replica->replstate == SLAVE_STATE_ONLINE)
                    lag = time(NULL) - replica->repl_ack_time;

                info = sdscatprintf(info,
                    "slave%d:ip=%s,port=%d,state=%s,"
                    "offset=%lld,lag=%ld\r\n",
                    slaveid,slaveip,replica->slave_listening_port,state,
                    (replica->repl_ack_off), lag);
                slaveid++;
            }
        }
        info = sdscatprintf(info,
            "master_failover_state:%s\r\n"
            "master_replid:%s\r\n"
            "master_replid2:%s\r\n"
            "master_repl_offset:%lld\r\n"
            "second_repl_offset:%lld\r\n"
            "repl_backlog_active:%d\r\n"
            "repl_backlog_size:%lld\r\n"
            "repl_backlog_first_byte_offset:%lld\r\n"
            "repl_backlog_histlen:%lld\r\n",
            getFailoverStateString(),
            g_pserver->replid,
            g_pserver->replid2,
            g_pserver->master_repl_offset,
            g_pserver->second_replid_offset,
            g_pserver->repl_backlog != NULL,
            g_pserver->repl_backlog_size,
            g_pserver->repl_backlog_off,
            g_pserver->repl_backlog_histlen);
    }

    /* CPU */
    if (allsections || defsections || !strcasecmp(section,"cpu")) {
        if (sections++) info = sdscat(info,"\r\n");

        struct rusage self_ru, c_ru;
        getrusage(RUSAGE_SELF, &self_ru);
        getrusage(RUSAGE_CHILDREN, &c_ru);
        info = sdscatprintf(info,
        "# CPU\r\n"
        "used_cpu_sys:%ld.%06ld\r\n"
        "used_cpu_user:%ld.%06ld\r\n"
        "used_cpu_sys_children:%ld.%06ld\r\n"
        "used_cpu_user_children:%ld.%06ld\r\n"
        "server_threads:%d\r\n"
        "long_lock_waits:%" PRIu64 "\r\n",
        (long)self_ru.ru_stime.tv_sec, (long)self_ru.ru_stime.tv_usec,
        (long)self_ru.ru_utime.tv_sec, (long)self_ru.ru_utime.tv_usec,
        (long)c_ru.ru_stime.tv_sec, (long)c_ru.ru_stime.tv_usec,
        (long)c_ru.ru_utime.tv_sec, (long)c_ru.ru_utime.tv_usec,
        cserver.cthreads,
        fastlock_getlongwaitcount());
#ifdef RUSAGE_THREAD
        struct rusage m_ru;
        getrusage(RUSAGE_THREAD, &m_ru);
        info = sdscatprintf(info,
            "used_cpu_sys_main_thread:%ld.%06ld\r\n"
            "used_cpu_user_main_thread:%ld.%06ld\r\n",
            (long)m_ru.ru_stime.tv_sec, (long)m_ru.ru_stime.tv_usec,
            (long)m_ru.ru_utime.tv_sec, (long)m_ru.ru_utime.tv_usec);
#endif  /* RUSAGE_THREAD */
    }

    /* Modules */
    if (allsections || defsections || !strcasecmp(section,"modules")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,"# Modules\r\n");
        info = genModulesInfoString(info);
    }

    /* Command statistics */
    if (allsections || !strcasecmp(section,"commandstats")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Commandstats\r\n");

        struct redisCommand *c;
        dictEntry *de;
        dictIterator *di;
        di = dictGetSafeIterator(g_pserver->commands);
        while((de = dictNext(di)) != NULL) {
            char *tmpsafe;
            c = (struct redisCommand *) dictGetVal(de);
            if (!c->calls && !c->failed_calls && !c->rejected_calls)
                continue;
            info = sdscatprintf(info,
                "cmdstat_%s:calls=%lld,usec=%lld,usec_per_call=%.2f"
                ",rejected_calls=%lld,failed_calls=%lld\r\n",
                getSafeInfoString(c->name, strlen(c->name), &tmpsafe), c->calls, c->microseconds,
                (c->calls == 0) ? 0 : ((float)c->microseconds/c->calls),
                c->rejected_calls, c->failed_calls);
            if (tmpsafe != NULL) zfree(tmpsafe);
        }
        dictReleaseIterator(di);
    }
    /* Error statistics */
    if (allsections || defsections || !strcasecmp(section,"errorstats")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscat(info, "# Errorstats\r\n");
        raxIterator ri;
        raxStart(&ri,g_pserver->errors);
        raxSeek(&ri,"^",NULL,0);
        struct redisError *e;
        while(raxNext(&ri)) {
            char *tmpsafe;
            e = (struct redisError *) ri.data;
            info = sdscatprintf(info,
                "errorstat_%.*s:count=%lld\r\n",
                (int)ri.key_len, getSafeInfoString((char *) ri.key, ri.key_len, &tmpsafe), e->count);
            if (tmpsafe != NULL) zfree(tmpsafe);
        }
        raxStop(&ri);
    }

    /* Cluster */
    if (allsections || defsections || !strcasecmp(section,"cluster")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
        "# Cluster\r\n"
        "cluster_enabled:%d\r\n",
        g_pserver->cluster_enabled);
    }

    /* Key space */
    if (allsections || defsections || !strcasecmp(section,"keyspace")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Keyspace\r\n");
        for (j = 0; j < cserver.dbnum; j++) {
            long long keys, vkeys, cachedKeys;

            keys = g_pserver->db[j]->size();
            vkeys = g_pserver->db[j]->expireSize();
            cachedKeys = g_pserver->db[j]->size(true /* fCachedOnly */);

            // Adjust TTL by the current time
            mstime_t mstime;
            __atomic_load(&g_pserver->mstime, &mstime, __ATOMIC_ACQUIRE);
            g_pserver->db[j]->avg_ttl -= (mstime - g_pserver->db[j]->last_expire_set);
            if (g_pserver->db[j]->avg_ttl < 0)
                g_pserver->db[j]->avg_ttl = 0;
            g_pserver->db[j]->last_expire_set = mstime;
            
            if (keys || vkeys) {
                info = sdscatprintf(info,
                    "db%d:keys=%lld,expires=%lld,avg_ttl=%lld,cached_keys=%lld\r\n",
                    j, keys, vkeys, static_cast<long long>(g_pserver->db[j]->avg_ttl), cachedKeys);
            }
        }
    }

    if (allsections || defsections || !strcasecmp(section,"keydb")) {
        // Compute the MVCC depth
        int mvcc_depth = 0;
        for (int idb = 0; idb < cserver.dbnum; ++idb) {
            mvcc_depth = std::max(mvcc_depth, g_pserver->db[idb]->snapshot_depth());
        }

        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, 
            "# KeyDB\r\n"
            "mvcc_depth:%d\r\n",
            mvcc_depth
        );
    }

    /* Get info from modules.
     * if user asked for "everything" or "modules", or a specific section
     * that's not found yet. */
    if (everything || modules ||
        (!allsections && !defsections && sections==0)) {
        info = modulesCollectInfo(info,
                                  everything || modules ? NULL: section,
                                  0, /* not a crash report */
                                  sections);
    }
    return info;
}

void infoCommand(client *c) {
    const char *section = c->argc == 2 ? (const char*)ptrFromObj(c->argv[1]) : "default";

    if (c->argc > 2) {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }
    sds info = genRedisInfoString(section);
    addReplyVerbatim(c,info,sdslen(info),"txt");
    sdsfree(info);
}

void monitorCommand(client *c) {
    serverAssert(GlobalLocksAcquired());

    if (c->flags & CLIENT_DENY_BLOCKING) {
        /**
         * A client that has CLIENT_DENY_BLOCKING flag on
         * expects a reply per command and so can't execute MONITOR. */
        addReplyError(c, "MONITOR isn't allowed for DENY BLOCKING client");
        return;
    }

    /* ignore MONITOR if already slave or in monitor mode */
    if (c->flags & CLIENT_SLAVE) return;

    c->flags |= (CLIENT_SLAVE|CLIENT_MONITOR);
    listAddNodeTail(g_pserver->monitors,c);
    addReply(c,shared.ok);
}

/* =================================== Main! ================================ */

int checkIgnoreWarning(const char *warning) {
    int argc, j;
    sds *argv = sdssplitargs(g_pserver->ignore_warnings, &argc);
    if (argv == NULL)
        return 0;

    for (j = 0; j < argc; j++) {
        char *flag = argv[j];
        if (!strcasecmp(flag, warning))
            break;
    }
    sdsfreesplitres(argv,argc);
    return j < argc;
}

#ifdef __linux__
int linuxOvercommitMemoryValue(void) {
    FILE *fp = fopen("/proc/sys/vm/overcommit_memory","r");
    char buf[64];

    if (!fp) return -1;
    if (fgets(buf,64,fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    return atoi(buf);
}

void linuxMemoryWarnings(void) {
    if (linuxOvercommitMemoryValue() == 0) {
        serverLog(LL_WARNING,"WARNING overcommit_memory is set to 0! Background save may fail under low memory condition. To fix this issue add 'vm.overcommit_memory = 1' to /etc/sysctl.conf and then reboot or run the command 'sysctl vm.overcommit_memory=1' for this to take effect.");
    }
    if (THPIsEnabled() && THPDisable()) {
        serverLog(LL_WARNING,"WARNING you have Transparent Huge Pages (THP) support enabled in your kernel. This will create latency and memory usage issues with KeyDB. To fix this issue run the command 'echo madvise > /sys/kernel/mm/transparent_hugepage/enabled' as root, and add it to your /etc/rc.local in order to retain the setting after a reboot. KeyDB must be restarted after THP is disabled (set to 'madvise' or 'never').");
    }
}

#ifdef __arm64__

/* Get size in kilobytes of the Shared_Dirty pages of the calling process for the
 * memory map corresponding to the provided address, or -1 on error. */
static int smapsGetSharedDirty(unsigned long addr) {
    int ret, in_mapping = 0, val = -1;
    unsigned long from, to;
    char buf[64];
    FILE *f;

    f = fopen("/proc/self/smaps", "r");
    serverAssert(f);

    while (1) {
        if (!fgets(buf, sizeof(buf), f))
            break;

        ret = sscanf(buf, "%lx-%lx", &from, &to);
        if (ret == 2)
            in_mapping = from <= addr && addr < to;

        if (in_mapping && !memcmp(buf, "Shared_Dirty:", 13)) {
            ret = sscanf(buf, "%*s %d", &val);
            serverAssert(ret == 1);
            break;
        }
    }

    fclose(f);
    return val;
}

/* Older arm64 Linux kernels have a bug that could lead to data corruption
 * during background save in certain scenarios. This function checks if the
 * kernel is affected.
 * The bug was fixed in commit ff1712f953e27f0b0718762ec17d0adb15c9fd0b
 * titled: "arm64: pgtable: Ensure dirty bit is preserved across pte_wrprotect()"
 * Return 1 if the kernel seems to be affected, and 0 otherwise. */
int linuxMadvFreeForkBugCheck(void) {
    int ret, pipefd[2];
    pid_t pid;
    char *p, *q, bug_found = 0;
    const long map_size = 3 * 4096;

    /* Create a memory map that's in our full control (not one used by the allocator). */
    p = (char*)mmap(NULL, map_size, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    serverAssert(p != MAP_FAILED);

    q = p + 4096;

    /* Split the memory map in 3 pages by setting their protection as RO|RW|RO to prevent
     * Linux from merging this memory map with adjacent VMAs. */
    ret = mprotect(q, 4096, PROT_READ | PROT_WRITE);
    serverAssert(!ret);

    /* Write to the page once to make it resident */
    *(volatile char*)q = 0;

    /* Tell the kernel that this page is free to be reclaimed. */
#ifndef MADV_FREE
#define MADV_FREE 8
#endif
    ret = madvise(q, 4096, MADV_FREE);
    serverAssert(!ret);

    /* Write to the page after being marked for freeing, this is supposed to take
     * ownership of that page again. */
    *(volatile char*)q = 0;

    /* Create a pipe for the child to return the info to the parent. */
    ret = pipe(pipefd);
    serverAssert(!ret);

    /* Fork the process. */
    pid = fork();
    serverAssert(pid >= 0);
    if (!pid) {
        /* Child: check if the page is marked as dirty, expecing 4 (kB).
         * A value of 0 means the kernel is affected by the bug. */
        if (!smapsGetSharedDirty((unsigned long)q))
            bug_found = 1;

        ret = write(pipefd[1], &bug_found, 1);
        serverAssert(ret == 1);

        exit(0);
    } else {
        /* Read the result from the child. */
        ret = read(pipefd[0], &bug_found, 1);
        serverAssert(ret == 1);

        /* Reap the child pid. */
        serverAssert(waitpid(pid, NULL, 0) == pid);
    }

    /* Cleanup */
    ret = close(pipefd[0]);
    serverAssert(!ret);
    ret = close(pipefd[1]);
    serverAssert(!ret);
    ret = munmap(p, map_size);
    serverAssert(!ret);

    return bug_found;
}
#endif /* __arm64__ */
#endif /* __linux__ */

void createPidFile(void) {
    /* If pidfile requested, but no pidfile defined, use
     * default pidfile path */
    if (!cserver.pidfile) cserver.pidfile = zstrdup(CONFIG_DEFAULT_PID_FILE);

    /* Try to write the pid file in a best-effort way. */
    FILE *fp = fopen(cserver.pidfile,"w");
    if (fp) {
        fprintf(fp,"%d\n",(int)getpid());
        fclose(fp);
    }
}

void daemonize(void) {
    int fd;

    if (fork() != 0) exit(0); /* parent exits */
    setsid(); /* create a new session */

    /* Every output goes to /dev/null. If Redis is daemonized but
     * the 'logfile' is set to 'stdout' in the configuration file
     * it will not log at all. */
    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
}

void version(void) {
    printf("KeyDB server v=%s sha=%s:%d malloc=%s bits=%d build=%llx\n",
        KEYDB_REAL_VERSION,
        redisGitSHA1(),
        atoi(redisGitDirty()) > 0,
        ZMALLOC_LIB,
        sizeof(long) == 4 ? 32 : 64,
        (unsigned long long) redisBuildId());
    exit(0);
}

void usage(void) {
    fprintf(stderr,"Usage: ./keydb-server [/path/to/keydb.conf] [options] [-]\n");
    fprintf(stderr,"       ./keydb-server - (read config from stdin)\n");
    fprintf(stderr,"       ./keydb-server -v or --version\n");
    fprintf(stderr,"       ./keydb-server -h or --help\n");
    fprintf(stderr,"       ./keydb-server --test-memory <megabytes>\n\n");
    fprintf(stderr,"Examples:\n");
    fprintf(stderr,"       ./keydb-server (run the server with default conf)\n");
    fprintf(stderr,"       ./keydb-server /etc/keydb/6379.conf\n");
    fprintf(stderr,"       ./keydb-server --port 7777\n");
    fprintf(stderr,"       ./keydb-server --port 7777 --replicaof 127.0.0.1 8888\n");
    fprintf(stderr,"       ./keydb-server /etc/mykeydb.conf --loglevel verbose -\n");
    fprintf(stderr,"       ./keydb-server /etc/mykeydb.conf --loglevel verbose\n\n");
    fprintf(stderr,"Sentinel mode:\n");
    fprintf(stderr,"       ./keydb-server /etc/sentinel.conf --sentinel\n");
    exit(1);
}

void redisAsciiArt(void) {
#include "asciilogo.h"
    size_t bufsize = 1024*16;
    char *buf = (char*)zmalloc(bufsize, MALLOC_LOCAL);
    const char *mode;

    if (g_pserver->cluster_enabled) mode = "cluster";
    else if (g_pserver->sentinel_mode) mode = "sentinel";
    else mode = "standalone";

    /* Show the ASCII logo if: log file is stdout AND stdout is a
     * tty AND syslog logging is disabled. Also show logo if the user
     * forced us to do so via keydb.conf. */
    int show_logo = ((!g_pserver->syslog_enabled &&
                      g_pserver->logfile[0] == '\0' &&
                      isatty(fileno(stdout))) ||
                     g_pserver->always_show_logo);

    if (!show_logo) {
        serverLog(LL_NOTICE,
            "Running mode=%s, port=%d.",
            mode, g_pserver->port ? g_pserver->port : g_pserver->tls_port
        );
    } else {
        sds motd = fetchMOTD(true, cserver.enable_motd);
        snprintf(buf,bufsize,ascii_logo,
            KEYDB_REAL_VERSION,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            (sizeof(long) == 8) ? "64" : "32",
            mode, g_pserver->port ? g_pserver->port : g_pserver->tls_port,
            (long) getpid(),
            motd ? motd : ""
        );
        if (motd)
            freeMOTD(motd);
        serverLogRaw(LL_NOTICE|LL_RAW,buf);
    }

    zfree(buf);
}

int changeBindAddr(sds *addrlist, int addrlist_len, bool fFirstCall) {
    int i;
    int result = C_OK;

    char *prev_bindaddr[CONFIG_BINDADDR_MAX];
    int prev_bindaddr_count;

    /* Close old TCP and TLS servers */
    closeSocketListeners(&serverTL->ipfd);
    closeSocketListeners(&serverTL->tlsfd);

    /* Keep previous settings */
    prev_bindaddr_count = g_pserver->bindaddr_count;
    memcpy(prev_bindaddr, g_pserver->bindaddr, sizeof(g_pserver->bindaddr));

    /* Copy new settings */
    memset(g_pserver->bindaddr, 0, sizeof(g_pserver->bindaddr));
    for (i = 0; i < addrlist_len; i++) {
        g_pserver->bindaddr[i] = zstrdup(addrlist[i]);
    }
    g_pserver->bindaddr_count = addrlist_len;

    /* Bind to the new port */
    if ((g_pserver->port != 0 && listenToPort(g_pserver->port, &serverTL->ipfd, (cserver.cthreads > 1), fFirstCall) != C_OK) ||
        (g_pserver->tls_port != 0 && listenToPort(g_pserver->tls_port, &serverTL->tlsfd, (cserver.cthreads > 1), fFirstCall) != C_OK)) {
        serverLog(LL_WARNING, "Failed to bind, trying to restore old listening sockets.");

        /* Restore old bind addresses */
        for (i = 0; i < addrlist_len; i++) {
            zfree(g_pserver->bindaddr[i]);
        }
        memcpy(g_pserver->bindaddr, prev_bindaddr, sizeof(g_pserver->bindaddr));
        g_pserver->bindaddr_count = prev_bindaddr_count;

        /* Re-Listen TCP and TLS */
        serverTL->ipfd.count = 0;
        if (g_pserver->port != 0 && listenToPort(g_pserver->port, &serverTL->ipfd, (cserver.cthreads > 1), false) != C_OK) {
            serverPanic("Failed to restore old listening sockets.");
        }

        serverTL->tlsfd.count = 0;
        if (g_pserver->tls_port != 0 && listenToPort(g_pserver->tls_port, &serverTL->tlsfd, (cserver.cthreads > 1), false) != C_OK) {
            serverPanic("Failed to restore old listening sockets.");
        }

        result = C_ERR;
    } else {
        /* Free old bind addresses */
        for (i = 0; i < prev_bindaddr_count; i++) {
            zfree(prev_bindaddr[i]);
        }
    }

    /* Create TCP and TLS event handlers */
    if (createSocketAcceptHandler(&serverTL->ipfd, acceptTcpHandler) != C_OK) {
        serverPanic("Unrecoverable error creating TCP socket accept handler.");
    }
    if (createSocketAcceptHandler(&serverTL->tlsfd, acceptTLSHandler) != C_OK) {
        serverPanic("Unrecoverable error creating TLS socket accept handler.");
    }

    if (cserver.set_proc_title && fFirstCall) redisSetProcTitle(NULL);

    return result;
}

int changeListenPort(int port, socketFds *sfd, aeFileProc *accept_handler, bool fFirstCall) {
    socketFds new_sfd = {{0}};

    /* Just close the server if port disabled */
    if (port == 0) {
        closeSocketListeners(sfd);
        if (cserver.set_proc_title && fFirstCall) redisSetProcTitle(NULL);
        return C_OK;
    }

    /* Bind to the new port */
    if (listenToPort(port, &new_sfd, (cserver.cthreads > 1), fFirstCall) != C_OK) {
        return C_ERR;
    }

    /* Create event handlers */
    if (createSocketAcceptHandler(&new_sfd, accept_handler) != C_OK) {
        closeSocketListeners(&new_sfd);
        return C_ERR;
    }

    /* Close old servers */
    closeSocketListeners(sfd);

    /* Copy new descriptors */
    sfd->count = new_sfd.count;
    memcpy(sfd->fd, new_sfd.fd, sizeof(new_sfd.fd));

    if (cserver.set_proc_title && fFirstCall) redisSetProcTitle(NULL);

    return C_OK;
}

static void sigShutdownHandler(int sig) {
    const char *msg;

    switch (sig) {
    case SIGINT:
        msg = "Received SIGINT scheduling shutdown...";
        break;
    case SIGTERM:
        msg = "Received SIGTERM scheduling shutdown...";
        break;
    default:
        msg = "Received shutdown signal, scheduling shutdown...";
    };

    /* SIGINT is often delivered via Ctrl+C in an interactive session.
     * If we receive the signal the second time, we interpret this as
     * the user really wanting to quit ASAP without waiting to persist
     * on disk. */
    if ((g_pserver->shutdown_asap || g_pserver->soft_shutdown) && sig == SIGINT) {
        serverLogFromHandler(LL_WARNING, "You insist... exiting now.");
        rdbRemoveTempFile(g_pserver->rdbThreadVars.tmpfileNum, 1);
        g_pserver->garbageCollector.shutdown();
        _Exit(1); /* Exit with an error since this was not a clean shutdown. */
    } else if (g_pserver->loading) {
        serverLogFromHandler(LL_WARNING, "Received shutdown signal during loading, exiting now.");
        _Exit(0);   // calling dtors is undesirable, exit immediately
    }

    serverLogFromHandler(LL_WARNING, msg);
    if (g_pserver->config_soft_shutdown)
        g_pserver->soft_shutdown = true;
    else
        g_pserver->shutdown_asap = 1;
}

void setupSignalHandlers(void) {
    struct sigaction act;

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used.
     * Otherwise, sa_handler is used. */
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigShutdownHandler;
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT, &act, NULL);

    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESETHAND | SA_SIGINFO;
    act.sa_sigaction = sigsegvHandler;
    if(g_pserver->crashlog_enabled) {
        sigaction(SIGSEGV, &act, NULL);
        sigaction(SIGBUS, &act, NULL);
        sigaction(SIGFPE, &act, NULL);
        sigaction(SIGILL, &act, NULL);
        sigaction(SIGABRT, &act, NULL);
    }
    return;
}

void removeSignalHandlers(void) {
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESETHAND;
    act.sa_handler = SIG_DFL;
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGFPE, &act, NULL);
    sigaction(SIGILL, &act, NULL);
    sigaction(SIGABRT, &act, NULL);
}

/* 这是子进程的信号处理程序。它目前用于跟踪 SIGUSR1 信号，我们发送此信号给子进程以使其干净地终止，而不会让父进程检测到错误并因写入错误条件而停止接受写入。 */
static void sigKillChildHandler(int sig) {
    UNUSED(sig);
    int level = g_pserver->in_fork_child == CHILD_TYPE_MODULE? LL_VERBOSE: LL_WARNING;
    serverLogFromHandler(level, "Received SIGUSR1 in child, exiting now.");
    exitFromChild(SERVER_CHILD_NOERROR_RETVAL);
}

void setupChildSignalHandlers(void) {
    struct sigaction act;

    /* 当 sa_flags 中设置了 SA_SIGINFO 标志时，将使用 sa_sigaction。否则，使用 sa_handler。 */
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigKillChildHandler;
    sigaction(SIGUSR1, &act, NULL);
    return;
}

/* fork 之后，子进程将继承父进程的资源，例如 fd（套接字或文件锁）等。应该关闭子进程不使用的资源，这样即使子进程可能仍在运行，父进程重启时也可以进行绑定/锁定。 */
void closeChildUnusedResourceAfterFork() {
    closeListeningSockets(0);
    if (g_pserver->cluster_enabled && g_pserver->cluster_config_file_lock_fd != -1)
        close(g_pserver->cluster_config_file_lock_fd);  /* 不关心这是否失败 */

    for (int iel = 0; iel < cserver.cthreads; ++iel) {
        aeClosePipesForForkChild(g_pserver->rgthreadvar[iel].el);
    }
    aeClosePipesForForkChild(g_pserver->modulethreadvar.el);

    /* 清除 cserver.pidfile，这是父进程的 pidfile，子进程（在退出/崩溃时）不应接触（或删除）它 */
    zfree(cserver.pidfile);
    cserver.pidfile = NULL;
}

void executeWithoutGlobalLock(std::function<void()> func) {
    serverAssert(GlobalLocksAcquired());

    std::vector<client*> vecclients;
    listIter li;
    listNode *ln;
    listRewind(g_pserver->clients, &li);

    // 所有客户端锁必须在重新获取全局锁*之后*获取，以防止死锁
    //  所以在这里解锁，并保存它们以便稍后重新获取
    while ((ln = listNext(&li)) != nullptr)
    {
        client *c = (client*)listNodeValue(ln);
        if (c->lock.fOwnLock()) {
            serverAssert(c->flags & CLIENT_PROTECTED || c->flags & CLIENT_EXECUTING_COMMAND);  // 如果客户端未受保护，我们无法保证它们不会在事件循环中被释放
            c->lock.unlock();
            vecclients.push_back(c);
        }
    }
    
    /* 因为我们即将释放锁，所以需要刷新复制积压队列 */
    bool fReplBacklog = g_pserver->repl_batch_offStart >= 0;
    if (fReplBacklog) {
        flushReplBacklogToClients();
        g_pserver->repl_batch_idxStart = -1;
        g_pserver->repl_batch_offStart = -1;
    }

    aeReleaseLock();
    serverAssert(!GlobalLocksAcquired());
    try {
        func();
    }
    catch (...) {
        // 调用者期望我们已锁定，因此修复并重新抛出
        AeLocker locker;
        locker.arm(nullptr);
        locker.release();
        for (client *c : vecclients)
            c->lock.lock();
        throw;
    }
    
    AeLocker locker;
    locker.arm(nullptr);
    locker.release();

    // 恢复它，以免调用代码混淆
    if (fReplBacklog) {
        g_pserver->repl_batch_idxStart = g_pserver->repl_backlog_idx;
        g_pserver->repl_batch_offStart = g_pserver->master_repl_offset;
    }

    for (client *c : vecclients)
        c->lock.lock();
}

/* purpose 是 CHILD_TYPE_ 类型之一 */
int redisFork(int purpose) {
    int childpid;
    long long start = ustime();
    
    if (isMutuallyExclusiveChildType(purpose)) {
        if (hasActiveChildProcess())
            return -1;

        openChildInfoPipe();
    }
    long long startWriteLock = ustime();
    aeAcquireForkLock();
    latencyAddSampleIfNeeded("fork-lock",(ustime()-startWriteLock)/1000);
    if ((childpid = fork()) == 0) {
        /* Child */
        aeForkLockInChild();
        aeReleaseForkLock();
        g_pserver->in_fork_child = purpose;
        setOOMScoreAdj(CONFIG_OOM_BGCHILD);
        setupChildSignalHandlers();
        closeChildUnusedResourceAfterFork();
    } else {
        /* Parent */
        aeReleaseForkLock();
        g_pserver->stat_total_forks++;
        g_pserver->stat_fork_time = ustime()-start;
        g_pserver->stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / g_pserver->stat_fork_time / (1024*1024*1024); /* GB per second. */
        latencyAddSampleIfNeeded("fork",g_pserver->stat_fork_time/1000);
        if (childpid == -1) {
            if (isMutuallyExclusiveChildType(purpose)) closeChildInfoPipe();
            return -1;
        }

        /* child_pid 和 child_type 仅适用于互斥的子进程。
         * 其他子进程类型应在专用变量中处理和存储其 PID。
         *
         * 目前，我们允许 CHILD_TYPE_LDB 与其他 fork 类型并行运行：
         * - 它不用于生产环境，因此不会降低服务器效率
         * - 用于调试，我们不希望在其他 fork（如 RDB 和 AOF）运行时阻止它运行 */
        if (isMutuallyExclusiveChildType(purpose)) {
            g_pserver->child_pid = childpid;
            g_pserver->child_type = purpose;
            g_pserver->stat_current_cow_bytes = 0;
            g_pserver->stat_current_cow_updated = 0;
            g_pserver->stat_current_save_keys_processed = 0;
            g_pserver->stat_module_progress = 0;
            g_pserver->stat_current_save_keys_total = dbTotalServerKeyCount();
        }

        updateDictResizePolicy();
        moduleFireServerEvent(REDISMODULE_EVENT_FORK_CHILD,
                              REDISMODULE_SUBEVENT_FORK_CHILD_BORN,
                              NULL);
    }
    return childpid;
}

void sendChildCowInfo(childInfoType info_type, const char *pname) {
    sendChildInfoGeneric(info_type, 0, -1, pname);
}

void sendChildInfo(childInfoType info_type, size_t keys, const char *pname) {
    sendChildInfoGeneric(info_type, keys, -1, pname);
}

extern "C" void memtest(size_t megabytes, int passes);

/* 如果参数中包含 --sentinel 或者 argv[0] 包含 "keydb-sentinel"，则返回 1。 */
int checkForSentinelMode(int argc, char **argv) {
    int j;

    if (strstr(argv[0],"keydb-sentinel") != NULL) return 1;
    for (j = 1; j < argc; j++)
        if (!strcmp(argv[j],"--sentinel")) return 1;
    return 0;
}

/* 启动时调用的函数，用于将 RDB 或 AOF 文件加载到内存中。 */
void loadDataFromDisk(void) {
    long long start = ustime();

    if (g_pserver->m_pstorageFactory)
    {
        for (int idb = 0; idb < cserver.dbnum; ++idb)
        {
            if (g_pserver->db[idb]->size() > 0)
            {
                serverLog(LL_NOTICE, "Not loading the RDB because a storage provider is set and the database is not empty");
                return;
            }
        }
        serverLog(LL_NOTICE, "Loading the RDB even though we have a storage provider because the database is empty");
    }
    
    serverTL->gcEpoch = g_pserver->garbageCollector.startEpoch();
    if (g_pserver->aof_state == AOF_ON) {
        if (loadAppendOnlyFile(g_pserver->aof_filename) == C_OK)
            serverLog(LL_NOTICE,"DB loaded from append only file: %.3f seconds",(float)(ustime()-start)/1000000);
    } else if (g_pserver->rdb_filename != NULL || g_pserver->rdb_s3bucketpath != NULL) {
        rdbSaveInfo rsi;
        rsi.fForceSetKey = false;
        errno = 0; /* 防止过时的值影响错误检查 */
        if (rdbLoad(&rsi,RDBFLAGS_NONE) == C_OK) {
            serverLog(LL_NOTICE,"DB loaded from disk: %.3f seconds",
                (float)(ustime()-start)/1000000);

            /* 从 RDB 文件中恢复复制 ID / 偏移量。 */
            if ((listLength(g_pserver->masters) ||
                (g_pserver->cluster_enabled &&
                nodeIsSlave(g_pserver->cluster->myself))) &&
                rsi.repl_id_is_set &&
                rsi.repl_offset != -1 &&
                /* 注意，较旧的实现可能会以错误的方式在 RDB 文件中保存 -1 的 repl_stream_db，更多信息请参见函数 rdbPopulateSaveInfo。 */
                rsi.repl_stream_db != -1)
            {
                memcpy(g_pserver->replid,rsi.repl_id,sizeof(g_pserver->replid));
                g_pserver->master_repl_offset = rsi.repl_offset;
                if (g_pserver->repl_batch_offStart >= 0)
                    g_pserver->repl_batch_offStart = g_pserver->master_repl_offset;
            }
            updateActiveReplicaMastersFromRsi(&rsi);
            if (!g_pserver->fActiveReplica && listLength(g_pserver->masters)) {
                redisMaster *mi = (redisMaster*)listNodeValue(listFirst(g_pserver->masters));
                /* 如果我们是从库，则根据此信息创建一个缓存的主库，以便允许与主库进行部分重新同步。 */
                replicationCacheMasterUsingMyself(mi);
                selectDb(mi->cached_master,rsi.repl_stream_db);
            }
        } else if (errno != ENOENT) {
            serverLog(LL_WARNING,"Fatal error loading the DB: %s. Exiting.",strerror(errno));
            exit(1);
        }
    }
    g_pserver->garbageCollector.endEpoch(serverTL->gcEpoch);
    serverTL->gcEpoch.reset();
}

void redisOutOfMemoryHandler(size_t allocation_size) {
    serverLog(LL_WARNING,"Out Of Memory allocating %zu bytes!",
        allocation_size);
    serverPanic("KeyDB aborting for OUT OF MEMORY. Allocating %zu bytes!", 
        allocation_size);
}

/* proc-title-template 上 sdstemplate 的回调函数。有关支持的变量，请参见 redis.conf。
 */
static sds redisProcTitleGetVariable(const sds varname, void *arg)
{
    if (!strcmp(varname, "title")) {
        return sdsnew((const char*)arg);
    } else if (!strcmp(varname, "listen-addr")) {
        if (g_pserver->port || g_pserver->tls_port)
            return sdscatprintf(sdsempty(), "%s:%u",
                                g_pserver->bindaddr_count ? g_pserver->bindaddr[0] : "*",
                                g_pserver->port ? g_pserver->port : g_pserver->tls_port);
        else
            return sdscatprintf(sdsempty(), "unixsocket:%s", g_pserver->unixsocket);
    } else if (!strcmp(varname, "server-mode")) {
        if (g_pserver->cluster_enabled) return sdsnew("[cluster]");
        else if (g_pserver->sentinel_mode) return sdsnew("[sentinel]");
        else return sdsempty();
    } else if (!strcmp(varname, "config-file")) {
        return sdsnew(cserver.configfile ? cserver.configfile : "-");
    } else if (!strcmp(varname, "port")) {
        return sdscatprintf(sdsempty(), "%u", g_pserver->port);
    } else if (!strcmp(varname, "tls-port")) {
        return sdscatprintf(sdsempty(), "%u", g_pserver->tls_port);
    } else if (!strcmp(varname, "unixsocket")) {
        return sdsnew(g_pserver->unixsocket);
    } else
        return NULL;    /* 未知的变量名 */
}

/* 展开指定的 proc-title-template 字符串并返回新分配的 sds，或返回 NULL。 */
static sds expandProcTitleTemplate(const char *_template, const char *title) {
    sds res = sdstemplate(_template, redisProcTitleGetVariable, (void *) title);
    if (!res)
        return NULL;
    return sdstrim(res, " ");
}
/* 验证指定的模板，如果有效则返回 1，否则返回 0。 */
int validateProcTitleTemplate(const char *_template) {
    int ok = 1;
    sds res = expandProcTitleTemplate(_template, "");
    if (!res)
        return 0;
    if (sdslen(res) == 0) ok = 0;
    sdsfree(res);
    return ok;
}

int redisSetProcTitle(const char *title) {
#ifdef USE_SETPROCTITLE
    if (!title) title = cserver.exec_argv[0];
    sds proc_title = expandProcTitleTemplate(cserver.proc_title_template, title);
    if (!proc_title) return C_ERR;  /* Not likely, proc_title_template is validated */

    setproctitle("%s", proc_title);
    sdsfree(proc_title);
#else
    UNUSED(title);
#endif

    return C_OK;
}

void redisSetCpuAffinity(const char *cpulist) {
#ifdef USE_SETCPUAFFINITY
    setcpuaffinity(cpulist);
#else
    UNUSED(cpulist);
#endif
}

/* Send a notify message to systemd. Returns sd_notify return code which is
 * a positive number on success. */
int redisCommunicateSystemd(const char *sd_notify_msg) {
#ifdef HAVE_LIBSYSTEMD
    int ret = sd_notify(0, sd_notify_msg);

    if (ret == 0)
        serverLog(LL_WARNING, "systemd supervision error: NOTIFY_SOCKET not found!");
    else if (ret < 0)
        serverLog(LL_WARNING, "systemd supervision error: sd_notify: %d", ret);
    return ret;
#else
    UNUSED(sd_notify_msg);
    return 0;
#endif
}

/* Attempt to set up upstart supervision. Returns 1 if successful. */
static int redisSupervisedUpstart(void) {
    const char *upstart_job = getenv("UPSTART_JOB");

    if (!upstart_job) {
        serverLog(LL_WARNING,
                "upstart supervision requested, but UPSTART_JOB not found!");
        return 0;
    }

    serverLog(LL_NOTICE, "supervised by upstart, will stop to signal readiness.");
    raise(SIGSTOP);
    unsetenv("UPSTART_JOB");
    return 1;
}

/* Attempt to set up systemd supervision. Returns 1 if successful. */
static int redisSupervisedSystemd(void) {
#ifndef HAVE_LIBSYSTEMD
    serverLog(LL_WARNING,
            "systemd supervision requested or auto-detected, but Redis is compiled without libsystemd support!");
    return 0;
#else
    if (redisCommunicateSystemd("STATUS=Redis is loading...\n") <= 0)
        return 0;
    serverLog(LL_NOTICE,
        "Supervised by systemd. Please make sure you set appropriate values for TimeoutStartSec and TimeoutStopSec in your service unit.");
    return 1;
#endif
}

int redisIsSupervised(int mode) {
    int ret = 0;

    if (mode == SUPERVISED_AUTODETECT) {
        if (getenv("UPSTART_JOB")) {
            serverLog(LL_VERBOSE, "Upstart supervision detected.");
            mode = SUPERVISED_UPSTART;
        } else if (getenv("NOTIFY_SOCKET")) {
            serverLog(LL_VERBOSE, "Systemd supervision detected.");
            mode = SUPERVISED_SYSTEMD;
        }
    } else if (mode == SUPERVISED_UPSTART) {
        return redisSupervisedUpstart();
    } else if (mode == SUPERVISED_SYSTEMD) {
        serverLog(LL_WARNING,
            "WARNING supervised by systemd - you MUST set appropriate values for TimeoutStartSec and TimeoutStopSec in your service unit.");
        return redisCommunicateSystemd("STATUS=KeyDB is loading...\n");
    }

    switch (mode) {
        case SUPERVISED_UPSTART:
            ret = redisSupervisedUpstart();
            break;
        case SUPERVISED_SYSTEMD:
            ret = redisSupervisedSystemd();
            break;
        default:
            break;
    }

    if (ret)
        cserver.supervised_mode = mode;

    return ret;
}

/**
 * @brief 获取MVCC时间戳的原子读取操作
 *
 * 该函数通过原子加载指令安全地读取全局服务器实例的MVCC时间戳值，
 * 确保在并发访问时的内存可见性与操作顺序。
 *
 * @param 无
 * @return uint64_t 返回原子加载的MVCC时间戳值
 */
uint64_t getMvccTstamp()
{
    uint64_t rval;

    // 使用原子加载指令读取mvcc_tstamp值，采用ACQUIRE内存序：
    // 1. 保证加载操作不会被重排序到当前指令之后
    // 2. 建立内存同步屏障，确保后续操作能看到其他线程通过RELEASE内存序发布的变更
    __atomic_load(&g_pserver->mvcc_tstamp, &rval, __ATOMIC_ACQUIRE);

    return rval;
}


/**
 * @brief 增量更新MVCC时间戳，确保其不低于当前系统时间戳
 *
 * 该函数通过原子操作维护MVCC时间戳的单调递增性，用于解决多版本并发控制中的
 * 时间戳同步问题。当检测到MVCC时间戳落后于当前系统时间时，会将其提升到当
 * 前时间；当检测到时间戳溢出或超前时，会进行安全的递增操作。
 *
 * @param 无
 * @return 无
 */
void incrementMvccTstamp()
{
    uint64_t msPrev;
    // 原子加载当前MVCC时间戳并转换为毫秒单位
    __atomic_load(&g_pserver->mvcc_tstamp, &msPrev, __ATOMIC_ACQUIRE);
    msPrev >>= MVCC_MS_SHIFT;  // convert to milliseconds

    // 原子加载当前系统时间戳
    long long mst;
    __atomic_load(&g_pserver->mstime, &mst, __ATOMIC_ACQUIRE);

    // 比较时间戳并执行相应更新策略
    if (msPrev >= (uint64_t)mst)  // 处理时间戳溢出或超前情况
    {
        // 安全递增MVCC时间戳
        __atomic_fetch_add(&g_pserver->mvcc_tstamp, 1, __ATOMIC_RELEASE);
    }
    else  // 正常时间推进情况
    {
        // 将系统时间转换为MVCC时间戳格式并更新
        uint64_t val = ((uint64_t)mst) << MVCC_MS_SHIFT;
        __atomic_store(&g_pserver->mvcc_tstamp, &val, __ATOMIC_RELEASE);
    }
}


void OnTerminate()
{
    /* Any uncaught exception will call std::terminate().
        We want this handled like a segfault (printing the stack trace etc).
        The easiest way to achieve that is to acutally segfault, so we assert
        here.
    */
    auto exception = std::current_exception();
    if (exception != nullptr)
    {
        try
        {
            std::rethrow_exception(exception);
        }
        catch (const char *szErr)
        {
            serverLog(LL_WARNING, "Crashing on uncaught exception: %s", szErr);
        }
        catch (std::string str)
        {
            serverLog(LL_WARNING, "Crashing on uncaught exception: %s", str.c_str());
        }
        catch (...)
        {
            // NOP
        }
    }

    serverPanic("std::teminate() called");
}

void wakeTimeThread() {
    updateCachedTime();
    aeThreadOffline();
    std::unique_lock<fastlock> lock(time_thread_lock);
    aeThreadOnline();
    if (sleeping_threads >= cserver.cthreads)
        time_thread_cv.notify_one();
    sleeping_threads--;
    serverAssert(sleeping_threads >= 0);
}

/**
 * @brief 管理时间更新的线程主循环
 *
 * 该线程负责周期性更新时间缓存，协调休眠线程唤醒机制，
 * 并通过条件变量实现线程状态同步。
 *
 * @param arg 保留参数（未使用）
 * @return void* 线程退出状态（始终为NULL）
 */
void *timeThreadMain(void*) {
    timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 100;
    int cycle_count = 0;
    aeThreadOnline();

    while (true) {
        {
            // 进入临界区前切换到离线状态
            aeThreadOffline();
            std::unique_lock<fastlock> lock(time_thread_lock);
            // 获取锁后切换到在线状态
            aeThreadOnline();
            /**
             * 当所有线程都处于休眠状态时：
             * 1. 切换到离线状态
             * 2. 等待条件变量唤醒
             * 3. 唤醒后重置循环计数器
             */
            if (sleeping_threads >= cserver.cthreads) {
                aeThreadOffline();
                time_thread_cv.wait(lock);
                aeThreadOnline();
                cycle_count = 0;
            }
        }

        // 更新全局时间缓存
        updateCachedTime();
        /**
         * 达到最大循环次数时：
         * 1. 切换到离线状态释放资源
         * 2. 立即恢复在线状态
         * 3. 重置循环计数器
         */
        if (cycle_count == MAX_CYCLES_TO_HOLD_FORK_LOCK) {
            aeThreadOffline();
            aeThreadOnline();
            cycle_count = 0;
        }
        // 根据平台选择高精度休眠方式
#if defined(__APPLE__)
        nanosleep(&delay, nullptr);
#else
        clock_nanosleep(CLOCK_MONOTONIC, 0, &delay, NULL);
#endif
        cycle_count++;
    }
    // 线程退出前切换到离线状态
    aeThreadOffline();
}

void *workerThreadMain(void *parg)
{
    int iel = (int)((int64_t)parg);
    serverLog(LL_NOTICE, "Thread %d alive.", iel);
    serverTL = g_pserver->rgthreadvar+iel;  // set the TLS threadsafe global
    tlsInitThread();

    if (iel != IDX_EVENT_LOOP_MAIN)
    {
        aeThreadOnline();
        aeAcquireLock();
        initNetworkingThread(iel, cserver.cthreads > 1);
        aeReleaseLock();
        aeThreadOffline();
    }

    moduleAcquireGIL(true); // Normally afterSleep acquires this, but that won't be called on the first run
    aeThreadOnline();
    aeEventLoop *el = g_pserver->rgthreadvar[iel].el;
    try
    {
        aeMain(el);
    }
    catch (ShutdownException)
    {
    }
    aeThreadOffline();
    moduleReleaseGIL(true);
    serverAssert(!GlobalLocksAcquired());
    aeDeleteEventLoop(el);

    tlsCleanupThread();
    return NULL;
}

static void validateConfiguration()
{
    updateMasterAuth();
    
    if (cserver.cthreads > (int)std::thread::hardware_concurrency()) {
        serverLog(LL_WARNING, "WARNING: server-threads is greater than this machine's core count.  Truncating to %u threads", std::thread::hardware_concurrency());
        cserver.cthreads = (int)std::thread::hardware_concurrency();
        cserver.cthreads = std::max(cserver.cthreads, 1);	// in case of any weird sign overflows
    }

    if (g_pserver->enable_multimaster && !g_pserver->fActiveReplica) {
        serverLog(LL_WARNING, "ERROR: Multi Master requires active replication to be enabled.");
        serverLog(LL_WARNING, "\tKeyDB will now exit.  Please update your configuration file.");
        exit(EXIT_FAILURE);
    }

    g_pserver->repl_backlog_size = g_pserver->repl_backlog_config_size; // this is normally set in the update logic, but not on initial config
}

int iAmMaster(void) {
    return ((!g_pserver->cluster_enabled && (listLength(g_pserver->masters) == 0 || g_pserver->fActiveReplica)) ||
            (g_pserver->cluster_enabled && nodeIsMaster(g_pserver->cluster->myself)));
}

bool initializeStorageProvider(const char **err);

#ifdef REDIS_TEST
typedef int redisTestProc(int argc, char **argv, int accurate);
struct redisTest {
    char *name;
    redisTestProc *proc;
    int failed;
} redisTests[] = {
    {"ziplist", ziplistTest},
    {"quicklist", quicklistTest},
    {"intset", intsetTest},
    {"zipmap", zipmapTest},
    {"sha1test", sha1Test},
    {"util", utilTest},
    {"endianconv", endianconvTest},
    {"crc64", crc64Test},
    {"zmalloc", zmalloc_test},
    {"sds", sdsTest},
    {"dict", dictTest}
};
redisTestProc *getTestProcByName(const char *name) {
    int numtests = sizeof(redisTests)/sizeof(struct redisTest);
    for (int j = 0; j < numtests; j++) {
        if (!strcasecmp(name,redisTests[j].name)) {
            return redisTests[j].proc;
        }
    }
    return NULL;
}
#endif

int main(int argc, char **argv) {
    struct timeval tv;
    int j;
    char config_from_stdin = 0;

    std::set_terminate(OnTerminate);

    {
    SymVer version;
    version = parseVersion(KEYDB_REAL_VERSION);
    serverAssert(version.major >= 0 && version.minor >= 0 && version.build >= 0);
    serverAssert(compareVersion(&version) == VersionCompareResult::EqualVersion);
    }

#ifdef USE_MEMKIND
    storage_init(NULL, 0);
#endif

#ifdef REDIS_TEST
    if (argc >= 3 && !strcasecmp(argv[1], "test")) {
        int accurate = 0;
        for (j = 3; j < argc; j++) {
            if (!strcasecmp(argv[j], "--accurate")) {
                accurate = 1;
            }
        }

        if (!strcasecmp(argv[2], "all")) {
            int numtests = sizeof(redisTests)/sizeof(struct redisTest);
            for (j = 0; j < numtests; j++) {
                redisTests[j].failed = (redisTests[j].proc(argc,argv,accurate) != 0);
            }

            /* 报告测试结果 */
            int failed_num = 0;
            for (j = 0; j < numtests; j++) {
                if (redisTests[j].failed) {
                    failed_num++;
                    printf("[failed] Test - %s\n", redisTests[j].name);
                } else {
                    printf("[ok] Test - %s\n", redisTests[j].name);
                }
            }

            printf("%d tests, %d passed, %d failed\n", numtests,
                   numtests-failed_num, failed_num);

            return failed_num == 0 ? 0 : 1;
        } else {
            redisTestProc *proc = getTestProcByName(argv[2]);
            if (!proc) return -1; /* 未找到测试 */
            return proc(argc,argv,accurate);
        }

        return 0;
    }
#endif

    /* 我们需要初始化我们的库和服务器配置。 */
#ifdef INIT_SETPROCTITLE_REPLACEMENT
    spt_init(argc, argv);
#endif
    setlocale(LC_COLLATE,"");
    tzset(); /* 填充 'timezone' 全局变量。 */
    zmalloc_set_oom_handler(redisOutOfMemoryHandler);
    srand(time(NULL)^getpid());
    srandom(time(NULL)^getpid());
    gettimeofday(&tv,NULL);
    init_genrand64(((long long) tv.tv_sec * 1000000 + tv.tv_usec) ^ getpid());
    crc64_init();

    /* 存储 umask 值。因为 umask(2) 只提供设置和获取 API，所以我们必须重置它并恢复它。我们尽早执行此操作以避免与可能正在创建文件或目录的线程发生潜在的竞争条件。
     */
    umask(g_pserver->umask = umask(0777));
    
    serverAssert(g_pserver->repl_batch_offStart < 0);

    uint8_t hashseed[16];
    getRandomHexChars((char*)hashseed,sizeof(hashseed));
    dictSetHashFunctionSeed(hashseed);
    g_pserver->sentinel_mode = checkForSentinelMode(argc,argv);
    initServerConfig();
    serverTL = &g_pserver->rgthreadvar[IDX_EVENT_LOOP_MAIN];
    aeThreadOnline();
    aeAcquireLock();    // 我们在启动时拥有锁
    ACLInit(); /* ACL 子系统必须尽快初始化，因为基础网络代码和客户端创建依赖于它。 */
    moduleInitModulesSystem();
    tlsInit();

    /* 将可执行文件路径和参数存储在安全的地方，以便以后能够重新启动服务器。 */
    cserver.executable = getAbsolutePath(argv[0]);
    cserver.exec_argv = (char**)zmalloc(sizeof(char*)*(argc+1), MALLOC_LOCAL);
    cserver.exec_argv[argc] = NULL;
    for (j = 0; j < argc; j++) cserver.exec_argv[j] = zstrdup(argv[j]);

    /* 我们现在需要初始化 sentinel，因为在 sentinel 模式下解析配置文件会将 sentinel 数据结构填充到要监视的主节点中。 */
    if (g_pserver->sentinel_mode) {
        initSentinelConfig();
        initSentinel();
    }

    /* 检查我们是否需要在 keydb-check-rdb/aof 模式下启动。我们只执行程序 main。但是，该程序是 Redis 可执行文件的一部分，因此我们可以轻松地在加载错误时执行 RDB 检查。 */
    if (strstr(argv[0],"keydb-check-rdb") != NULL)
        redis_check_rdb_main(argc,(const char**)argv,NULL);
    else if (strstr(argv[0],"keydb-check-aof") != NULL)
        redis_check_aof_main(argc,argv);

    if (argc >= 2) {
        j = 1; /* argv[] 中要解析的第一个选项 */
        sds options = sdsempty();

        /* 处理特殊选项 --help 和 --version */
        if (strcmp(argv[1], "-v") == 0 ||
            strcmp(argv[1], "--version") == 0) version();
        if (strcmp(argv[1], "--help") == 0 ||
            strcmp(argv[1], "-h") == 0) usage();
        if (strcmp(argv[1], "--test-memory") == 0) {
            if (argc == 3) {
                memtest(atoi(argv[2]),50);
                exit(0);
            } else {
                fprintf(stderr,"Please specify the amount of memory to test in megabytes.\n");
                fprintf(stderr,"Example: ./keydb-server --test-memory 4096\n\n");
                exit(1);
            }
        }
        /* 解析命令行选项
         * 优先顺序：文件、标准输入、显式选项——最后一个配置才是重要的。
         *
         * 第一个参数是配置文件名吗？ */
        if (argv[1][0] != '-') {
            /* 将 g_pserver->exec_argv 中的配置文件替换为其绝对路径。 */
            cserver.configfile = getAbsolutePath(argv[1]);
            zfree(cserver.exec_argv[1]);
            cserver.exec_argv[1] = zstrdup(cserver.configfile);
            j = 2; // 解析选项时跳过此参数
        }
        while(j < argc) {
            /* 第一个或最后一个参数——我们应该从 stdin 读取配置吗？ */
            if (argv[j][0] == '-' && argv[j][1] == '\0' && (j == 1 || j == argc-1)) {
                config_from_stdin = 1;
            }
            /* 所有其他选项都会被解析并概念上附加到配置文件中。例如 --port 6380 将生成字符串 "port 6380\n"，以便在解析实际配置文件和 stdin 输入（如果存在）之后进行解析。 */
            else if (argv[j][0] == '-' && argv[j][1] == '-') {
                /* 选项名称 */
                if (sdslen(options)) options = sdscat(options,"\n");
                options = sdscat(options,argv[j]+2);
                options = sdscat(options," ");
            } else {
                /* 选项参数 */
                options = sdscatrepr(options,argv[j],strlen(argv[j]));
                options = sdscat(options," ");
            }
            j++;
        }

        loadServerConfig(cserver.configfile, config_from_stdin, options);
        if (g_pserver->sentinel_mode) loadSentinelConfigFromQueue();
        sdsfree(options);
    }

    if (g_pserver->syslog_enabled) {
        openlog(g_pserver->syslog_ident, LOG_PID | LOG_NDELAY | LOG_NOWAIT,
            g_pserver->syslog_facility);
    }
    
    if (g_pserver->sentinel_mode) sentinelCheckConfigFile();

    cserver.supervised = redisIsSupervised(cserver.supervised_mode);
    int background = cserver.daemonize && !cserver.supervised;
    if (background) daemonize();

    serverLog(LL_WARNING, "oO0OoO0OoO0Oo KeyDB is starting oO0OoO0OoO0Oo");
    serverLog(LL_WARNING,
        "KeyDB version=%s, bits=%d, commit=%s, modified=%d, pid=%d, just started",
            KEYDB_REAL_VERSION,
            (sizeof(long) == 8) ? 64 : 32,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            (int)getpid());

    if (argc == 1) {
        serverLog(LL_WARNING, "Warning: no config file specified, using the default config. In order to specify a config file use %s /path/to/keydb.conf", argv[0]);
    } else {
        serverLog(LL_WARNING, "Configuration loaded");
    }

    validateConfiguration();

    if (!g_pserver->sentinel_mode) {
    #ifdef __linux__
        linuxMemoryWarnings();
    #if defined (__arm64__) // 如果定义了 __arm64__
        int ret;
        if ((ret = linuxMadvFreeForkBugCheck())) {
            if (ret == 1)
                serverLog(LL_WARNING,"WARNING Your kernel has a bug that could lead to data corruption during background save. "
                                        "Please upgrade to the latest stable kernel.");
            else
                serverLog(LL_WARNING, "Failed to test the kernel for a bug that could lead to data corruption during background save. "
                                        "Your system could be affected, please report this error.");
            if (!checkIgnoreWarning("ARM64-COW-BUG")) {
                serverLog(LL_WARNING,"KeyDB will now exit to prevent data corruption. "
                                        "Note that it is possible to suppress this warning by setting the following config: ignore-warnings ARM64-COW-BUG");
                exit(1);
            }
        }
    #endif /* __arm64__ */ // 结束 __arm64__ 条件编译
    #endif /* __linux__ */ // 结束 __linux__ 条件编译
    }


    const char *err;
    if (!initializeStorageProvider(&err))
    {
        serverLog(LL_WARNING, "Failed to initialize storage provider: %s",err);
        exit(EXIT_FAILURE);
    }

    for (int iel = 0; iel < cserver.cthreads; ++iel)
    {
        initServerThread(g_pserver->rgthreadvar+iel, iel == IDX_EVENT_LOOP_MAIN); // 初始化服务器线程，主事件循环线程特殊处理
    }

    initServerThread(&g_pserver->modulethreadvar, false);
    readOOMScoreAdj();
    initServer();
    initNetworking(cserver.cthreads > 1 /* fReusePort */); // 初始化网络，如果线程数大于1，则 fReusePort 为 true

    if (background || cserver.pidfile) createPidFile();
    if (cserver.set_proc_title) redisSetProcTitle(NULL);
    redisAsciiArt();
    checkTcpBacklogSettings();

    if (!g_pserver->sentinel_mode) {
        /* 在 Sentinel 模式下运行时不需要的东西。 */
        serverLog(LL_WARNING,"Server initialized");
        moduleInitModulesSystemLast();
        moduleLoadFromQueue();
        ACLLoadUsersAtStartup();

        // FUZZING 的特殊情况：从 stdin 加载然后退出
        if (argc > 1 && strstr(argv[1],"rdbfuzz-mode") != NULL)
        {
            zmalloc_set_oom_handler(fuzzOutOfMemoryHandler);
#ifdef __AFL_HAVE_MANUAL_CONTROL
            __AFL_INIT();
#endif
            rio rdb;
            rdbSaveInfo rsi;
            startLoadingFile(stdin, (char*)"stdin", 0);
            rioInitWithFile(&rdb,stdin);
            rdbLoadRio(&rdb,0,&rsi);
            stopLoading(true);
            return EXIT_SUCCESS;
        }

        InitServerLast();

        try {
            loadDataFromDisk();
        } catch (ShutdownException) {
            _Exit(EXIT_SUCCESS);
        }

        if (g_pserver->cluster_enabled) {
            if (verifyClusterConfigWithData() == C_ERR) {
                serverLog(LL_WARNING,
                    "You can't have keys in a DB different than DB 0 when in "
                    "Cluster mode. Exiting.");
                exit(1);
            }
        }
        if (g_pserver->rgthreadvar[IDX_EVENT_LOOP_MAIN].ipfd.count > 0 && g_pserver->rgthreadvar[IDX_EVENT_LOOP_MAIN].tlsfd.count > 0)
            serverLog(LL_NOTICE,"Ready to accept connections");
        if (g_pserver->sofd > 0)
            serverLog(LL_NOTICE,"The server is now ready to accept connections at %s", g_pserver->unixsocket);
        if (cserver.supervised_mode == SUPERVISED_SYSTEMD) {
            if (!listLength(g_pserver->masters)) {
                redisCommunicateSystemd("STATUS=Ready to accept connections\n");
            } else {
                redisCommunicateSystemd("STATUS=Ready to accept connections in read-only mode. Waiting for MASTER <-> REPLICA sync\n");
            }
            redisCommunicateSystemd("READY=1\n");
        }
    } else {
        ACLLoadUsersAtStartup();
        InitServerLast();
        sentinelIsRunning();
        if (cserver.supervised_mode == SUPERVISED_SYSTEMD) {
            redisCommunicateSystemd("STATUS=Ready to accept connections\n");
            redisCommunicateSystemd("READY=1\n");
        }
    }

    if (g_pserver->rdb_filename == nullptr)
    {
        if (g_pserver->rdb_s3bucketpath == nullptr)
            g_pserver->rdb_filename = zstrdup(CONFIG_DEFAULT_RDB_FILENAME);
        else
            g_pserver->repl_diskless_sync = TRUE;
    }

    if (cserver.cthreads > 4) {
        serverLog(LL_WARNING, "Warning: server-threads is set to %d.  This is above the maximum recommend value of 4, please ensure you've verified this is actually faster on your machine.", cserver.cthreads);
    }

    /* Warning the user about suspicious maxmemory setting. */
    if (g_pserver->maxmemory > 0 && g_pserver->maxmemory < 1024*1024) {
        serverLog(LL_WARNING,"WARNING: You specified a maxmemory value that is less than 1MB (current value is %llu bytes). Are you sure this is what you really want?", g_pserver->maxmemory);
    }

    redisSetCpuAffinity(g_pserver->server_cpulist);
    aeReleaseLock();    //最终我们可以释放锁
    aeThreadOffline();
    moduleReleaseGIL(true);
    
    setOOMScoreAdj(-1);
    serverAssert(cserver.cthreads > 0 && cserver.cthreads <= MAX_EVENT_LOOPS);

    pthread_create(&cserver.time_thread_id, nullptr, timeThreadMain, nullptr);
    if (cserver.time_thread_priority) {
        struct sched_param time_thread_priority;
        time_thread_priority.sched_priority = sched_get_priority_max(SCHED_FIFO);
        pthread_setschedparam(cserver.time_thread_id, SCHED_FIFO, &time_thread_priority);
    }

    pthread_attr_t tattr;
    pthread_attr_init(&tattr);
    pthread_attr_setstacksize(&tattr, 1 << 23); // 8 MB
    for (int iel = 0; iel < cserver.cthreads; ++iel)
    {
        pthread_create(g_pserver->rgthread + iel, &tattr, workerThreadMain, (void*)((int64_t)iel));
        if (cserver.fThreadAffinity)
        {
#ifdef __linux__
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(iel + cserver.threadAffinityOffset, &cpuset);
            if (pthread_setaffinity_np(g_pserver->rgthread[iel], sizeof(cpu_set_t), &cpuset) == 0)
            {
                serverLog(LL_NOTICE, "Binding thread %d to cpu %d", iel, iel + cserver.threadAffinityOffset + 1);
            }
#else
			serverLog(LL_WARNING, "CPU pinning not available on this platform");
#endif
        }
    }

    /* 阻止此线程接收 SIGALRM 信号，它应该只在服务器线程上接收 */
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &sigset, nullptr);

    /* 主线程休眠直到所有工作线程完成。
        这样可以使所有工作线程在其启动/关闭过程中保持正交 */
    void *pvRet;
    for (int iel = 0; iel < cserver.cthreads; ++iel)
        pthread_join(g_pserver->rgthread[iel], &pvRet);

    /* 释放我们的数据库 */
    bool fLockAcquired = aeTryAcquireLock(false);
    g_pserver->shutdown_asap = true;    // 标记我们处于关闭状态
    if (!fLockAcquired)
        g_fInCrash = true;  // 我们实际上不会立即崩溃，因为我们想同步任何存储提供程序
    
    saveMasterStatusToStorage(true);
    for (int idb = 0; idb < cserver.dbnum; ++idb) {
        g_pserver->db[idb]->storageProviderDelete();
    }
    delete g_pserver->metadataDb;

    // 如果我们无法获取全局锁，这意味着某些东西没有关闭，我们很可能会死锁
    serverAssert(fLockAcquired);

    g_pserver->garbageCollector.shutdown();
    delete g_pserver->m_pstorageFactory;

    // 不要返回，因为我们不想运行任何全局析构函数
    _Exit(EXIT_SUCCESS);
    return 0;   // 确保我们格式良好，即使这不会被执行
}

/* The End */
