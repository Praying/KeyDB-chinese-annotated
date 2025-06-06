/* Redis Object implementation.
 *
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

#include "server.h"
#include "cron.h"
#include "t_nhash.h"
#include <math.h>
#include <ctype.h>
#include <mutex>

#ifdef __CYGWIN__
#define strtold(a,b) ((long double)strtod((a),(b)))
#endif

/* ===================== Creation and parsing of objects ==================== */

robj *createObject(int type, void *ptr) {
    size_t mvccExtraBytes = g_pserver->fActiveReplica ? sizeof(redisObjectExtended) : 0;
    char *oB = (char*)zcalloc(sizeof(robj)+mvccExtraBytes, MALLOC_SHARED);
    robj *o = reinterpret_cast<robj*>(oB + mvccExtraBytes);
    
    new (o) redisObject;
    o->type = type;
    o->encoding = OBJ_ENCODING_RAW;
    o->m_ptr = ptr;
    o->setrefcount(1);
    setMvccTstamp(o, OBJ_MVCC_INVALID);

    /* Set the LRU to the current lruclock (minutes resolution), or
     * alternatively the LFU counter. */
    if (g_pserver->maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        o->lru = (LFUGetTimeInMinutes()<<8) | LFU_INIT_VAL;
    } else {
        o->lru = LRU_CLOCK();
    }
    return o;
}

/* Set a special refcount in the object to make it "shared":
 * incrRefCount and decrRefCount() will test for this special refcount
 * and will not touch the object. This way it is free to access shared
 * objects such as small integers from different threads without any
 * mutex.
 *
 * A common patter to create shared objects:
 *
 * robj *myobject = makeObjectShared(createObject(...));
 *
 */
robj *makeObjectShared(robj *o) {
    serverAssert(o->getrefcount(std::memory_order_relaxed) == 1);
    serverAssert(!o->FExpires());
    o->setrefcount(OBJ_SHARED_REFCOUNT);
    return o;
}

robj *makeObjectShared(const char *rgch, size_t cch)
{
    robj *o = createObject(OBJ_STRING,sdsnewlen(rgch, cch));
    return makeObjectShared(o);
}

/* Create a string object with encoding OBJ_ENCODING_RAW, that is a plain
 * string object where ptrFromObj(o) points to a proper sds string. */
robj *createRawStringObject(const char *ptr, size_t len) {
    return createObject(OBJ_STRING, sdsnewlen(ptr,len));
}

/* Create a string object with encoding OBJ_ENCODING_EMBSTR, that is
 * an object where the sds string is actually an unmodifiable string
 * allocated in the same chunk as the object itself. */
robj *createEmbeddedStringObject(const char *ptr, size_t len) {
    serverAssert(len <= UINT8_MAX);
    // Note: If the size changes update serializeStoredStringObject
    size_t allocsize = sizeof(struct sdshdr8)+len+1;
    if (allocsize < sizeof(void*))
        allocsize = sizeof(void*);

    size_t mvccExtraBytes = g_pserver->fActiveReplica ? sizeof(redisObjectExtended) : 0;
    char *oB = (char*)zmalloc(sizeof(robj)+allocsize-sizeof(redisObject::m_ptr)+mvccExtraBytes, MALLOC_SHARED);
    robj *o = reinterpret_cast<robj*>(oB + mvccExtraBytes);
    struct sdshdr8 *sh = (sdshdr8*)(&o->m_ptr);

    new (o) redisObject;
    o->type = OBJ_STRING;
    o->encoding = OBJ_ENCODING_EMBSTR;
    o->setrefcount(1);
    setMvccTstamp(o, OBJ_MVCC_INVALID);

    if (g_pserver->maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        o->lru = (LFUGetTimeInMinutes()<<8) | LFU_INIT_VAL;
    } else {
        o->lru = LRU_CLOCK();
    }

    sh->len = len;
    sh->alloc = len;
    sh->flags = SDS_TYPE_8;
    if (ptr == SDS_NOINIT)
        sh->buf()[len] = '\0';
    else if (ptr) {
        memcpy(sh->buf(),ptr,len);
        sh->buf()[len] = '\0';
    } else {
        memset(sh->buf(),0,len+1);
    }
    return o;
}

/* Create a string object with EMBSTR encoding if it is smaller than
 * OBJ_ENCODING_EMBSTR_SIZE_LIMIT, otherwise the RAW encoding is
 * used.
 *
 * The current limit of 52 is chosen so that the biggest string object
 * we allocate as EMBSTR will still fit into the 64 byte arena of jemalloc. */
size_t OBJ_ENCODING_EMBSTR_SIZE_LIMIT = 52;

robj *createStringObject(const char *ptr, size_t len) {
    if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT)
        return createEmbeddedStringObject(ptr,len);
    else
        return createRawStringObject(ptr,len);
}

/* Same as CreateRawStringObject, can return NULL if allocation fails */
robj *tryCreateRawStringObject(const char *ptr, size_t len) {
    sds str = sdstrynewlen(ptr,len);
    if (!str) return NULL;
    return createObject(OBJ_STRING, str);
}

/* Same as createStringObject, can return NULL if allocation fails */
robj *tryCreateStringObject(const char *ptr, size_t len) {
    if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT)
        return createEmbeddedStringObject(ptr,len);
    else
        return tryCreateRawStringObject(ptr,len);
}

/* Create a string object from a long long value. When possible returns a
 * shared integer object, or at least an integer encoded one.
 *
 * If valueobj is non zero, the function avoids returning a shared
 * integer, because the object is going to be used as value in the Redis key
 * space (for instance when the INCR command is used), so we want LFU/LRU
 * values specific for each key. */
robj *createStringObjectFromLongLongWithOptions(long long value, int valueobj) {
    robj *o;

    if (g_pserver->maxmemory == 0 ||
        !(g_pserver->maxmemory_policy & MAXMEMORY_FLAG_NO_SHARED_INTEGERS))
    {
        /* If the maxmemory policy permits, we can still return shared integers
         * even if valueobj is true. */
        valueobj = 0;
    }

    if (value >= 0 && value < OBJ_SHARED_INTEGERS && valueobj == 0) {
        incrRefCount(shared.integers[value]);
        o = shared.integers[value];
    } else {
        if (value >= LONG_MIN && value <= LONG_MAX) {
            o = createObject(OBJ_STRING, NULL);
            o->encoding = OBJ_ENCODING_INT;
            o->m_ptr = (void*)((long)value);
        } else {
            o = createObject(OBJ_STRING,sdsfromlonglong(value));
        }
    }
    return o;
}

/* Wrapper for createStringObjectFromLongLongWithOptions() always demanding
 * to create a shared object if possible. */
robj *createStringObjectFromLongLong(long long value) {
    return createStringObjectFromLongLongWithOptions(value,0);
}

/* Wrapper for createStringObjectFromLongLongWithOptions() avoiding a shared
 * object when LFU/LRU info are needed, that is, when the object is used
 * as a value in the key space, and Redis is configured to evict based on
 * LFU/LRU. */
robj *createStringObjectFromLongLongForValue(long long value) {
    return createStringObjectFromLongLongWithOptions(value,1);
}

/* Create a string object from a long double. If humanfriendly is non-zero
 * it does not use exponential format and trims trailing zeroes at the end,
 * however this results in loss of precision. Otherwise exp format is used
 * and the output of snprintf() is not modified.
 *
 * The 'humanfriendly' option is used for INCRBYFLOAT and HINCRBYFLOAT. */
robj *createStringObjectFromLongDouble(long double value, int humanfriendly) {
    char buf[MAX_LONG_DOUBLE_CHARS];
    int len = ld2string(buf,sizeof(buf),value,humanfriendly? LD_STR_HUMAN: LD_STR_AUTO);
    return createStringObject(buf,len);
}

/* Duplicate a string object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * This function also guarantees that duplicating a small integer object
 * (or a string object that contains a representation of a small integer)
 * will always result in a fresh object that is unshared (refcount == 1).
 *
 * The resulting object always has refcount set to 1. */
robj *dupStringObject(const robj *o) {
    robj *d;

    serverAssert(o->type == OBJ_STRING);

    switch(o->encoding) {
    case OBJ_ENCODING_RAW:
        return createRawStringObject(szFromObj(o),sdslen(szFromObj(o)));
    case OBJ_ENCODING_EMBSTR:
        return createEmbeddedStringObject(szFromObj(o),sdslen(szFromObj(o)));
    case OBJ_ENCODING_INT:
        d = createObject(OBJ_STRING, NULL);
        d->encoding = OBJ_ENCODING_INT;
        d->m_ptr = ptrFromObj(o);
        return d;
    default:
        serverPanic("Wrong encoding.");
        break;
    }
}

robj *createQuicklistObject(void) {
    quicklist *l = quicklistCreate();
    robj *o = createObject(OBJ_LIST,l);
    o->encoding = OBJ_ENCODING_QUICKLIST;
    return o;
}

robj *createZiplistObject(void) {
    unsigned char *zl = ziplistNew();
    robj *o = createObject(OBJ_LIST,zl);
    o->encoding = OBJ_ENCODING_ZIPLIST;
    return o;
}

robj *createSetObject(void) {
    dict *d = dictCreate(&setDictType,NULL);
    robj *o = createObject(OBJ_SET,d);
    o->encoding = OBJ_ENCODING_HT;
    return o;
}

robj *createIntsetObject(void) {
    intset *is = intsetNew();
    robj *o = createObject(OBJ_SET,is);
    o->encoding = OBJ_ENCODING_INTSET;
    return o;
}

robj *createHashObject(void) {
    unsigned char *zl = ziplistNew();
    robj *o = createObject(OBJ_HASH, zl);
    o->encoding = OBJ_ENCODING_ZIPLIST;
    return o;
}

robj *createZsetObject(void) {
    zset *zs = (zset*)zmalloc(sizeof(*zs), MALLOC_SHARED);
    robj *o;

    zs->dict = dictCreate(&zsetDictType,NULL);
    zs->zsl = zslCreate();
    o = createObject(OBJ_ZSET,zs);
    o->encoding = OBJ_ENCODING_SKIPLIST;
    return o;
}

robj *createZsetZiplistObject(void) {
    unsigned char *zl = ziplistNew();
    robj *o = createObject(OBJ_ZSET,zl);
    o->encoding = OBJ_ENCODING_ZIPLIST;
    return o;
}

robj *createStreamObject(void) {
    stream *s = streamNew();
    robj *o = createObject(OBJ_STREAM,s);
    o->encoding = OBJ_ENCODING_STREAM;
    return o;
}

robj *createModuleObject(moduleType *mt, void *value) {
    moduleValue *mv = (moduleValue*)zmalloc(sizeof(*mv), MALLOC_SHARED);
    mv->type = mt;
    mv->value = value;
    return createObject(OBJ_MODULE,mv);
}

void freeStringObject(robj_roptr o) {
    if (o->encoding == OBJ_ENCODING_RAW) {
        sdsfree(szFromObj(o));
    }
}

void freeListObject(robj_roptr o) {
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistRelease((quicklist*)ptrFromObj(o));
    } else if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        zfree(ptrFromObj(o));
    } else {
        serverPanic("Unknown list encoding type: %d", o->encoding);
    }
}

void freeSetObject(robj_roptr o) {
    switch (o->encoding) {
    case OBJ_ENCODING_HT:
        dictRelease((dict*) ptrFromObj(o));
        break;
    case OBJ_ENCODING_INTSET:
        zfree(ptrFromObj(o));
        break;
    default:
        serverPanic("Unknown set encoding type");
    }
}

void freeZsetObject(robj_roptr o) {
    zset *zs;
    switch (o->encoding) {
    case OBJ_ENCODING_SKIPLIST:
        zs = (zset*)ptrFromObj(o);
        dictRelease(zs->dict);
        zslFree(zs->zsl);
        zfree(zs);
        break;
    case OBJ_ENCODING_ZIPLIST:
        zfree(ptrFromObj(o));
        break;
    default:
        serverPanic("Unknown sorted set encoding");
    }
}

void freeHashObject(robj_roptr o) {
    switch (o->encoding) {
    case OBJ_ENCODING_HT:
        dictRelease((dict*) ptrFromObj(o));
        break;
    case OBJ_ENCODING_ZIPLIST:
        zfree(ptrFromObj(o));
        break;
    default:
        serverPanic("Unknown hash encoding type");
        break;
    }
}

void freeModuleObject(robj_roptr o) {
    moduleValue *mv = (moduleValue*)ptrFromObj(o);
    mv->type->free(mv->value);
    zfree(mv);
}

void freeStreamObject(robj_roptr o) {
    freeStream((stream*)ptrFromObj(o));
}

void incrRefCount(robj_roptr o) {
    auto refcount = o->getrefcount(std::memory_order_relaxed);
    if (refcount < OBJ_FIRST_SPECIAL_REFCOUNT) {
        o->addref();
    } else {
        if (refcount == OBJ_SHARED_REFCOUNT) {
            /* Nothing to do: this refcount is immutable. */
        } else if (refcount == OBJ_STATIC_REFCOUNT) {
            serverPanic("You tried to retain an object allocated in the stack");
        }
    }
}

void decrRefCount(robj_roptr o) {
    if (o->getrefcount(std::memory_order_relaxed) == OBJ_SHARED_REFCOUNT)
        return;
    unsigned prev = o->release();
    if (prev == 1) {
        switch(o->type) {
        case OBJ_STRING: freeStringObject(o); break;
        case OBJ_LIST: freeListObject(o); break;
        case OBJ_SET: freeSetObject(o); break;
        case OBJ_ZSET: freeZsetObject(o); break;
        case OBJ_HASH: freeHashObject(o); break;
        case OBJ_MODULE: freeModuleObject(o); break;
        case OBJ_STREAM: freeStreamObject(o); break;
        case OBJ_CRON: freeCronObject(o); break;
        case OBJ_NESTEDHASH: freeNestedHashObject(o); break;
        default: serverPanic("Unknown object type"); break;
        }
        o->~redisObject();
        if (g_pserver->fActiveReplica) {
            zfree(reinterpret_cast<redisObjectExtended*>(o.unsafe_robjcast())-1);
        } else {
            zfree(o.unsafe_robjcast());
        }
    } else {
        if (prev <= 0) serverPanic("decrRefCount against refcount <= 0");
    }
}

/* This variant of decrRefCount() gets its argument as void, and is useful
 * as free method in data structures that expect a 'void free_object(void*)'
 * prototype for the free method. */
void decrRefCountVoid(const void *o) {
    decrRefCount((robj*)o);
}

int checkType(client *c, robj_roptr o, int type) {
    /* A NULL is considered an empty key */
    if (o && o->type != type) {
        addReplyErrorObject(c,shared.wrongtypeerr);
        return 1;
    }
    return 0;
}

int isSdsRepresentableAsLongLong(const char *s, long long *llval) {
    return string2ll(s,sdslen(s),llval) ? C_OK : C_ERR;
}

int isObjectRepresentableAsLongLong(robj *o, long long *llval) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
    if (o->encoding == OBJ_ENCODING_INT) {
        if (llval) *llval = (long) ptrFromObj(o);
        return C_OK;
    } else {
        return isSdsRepresentableAsLongLong(szFromObj(o),llval);
    }
}

/* Optimize the SDS string inside the string object to require little space,
 * in case there is more than 10% of free space at the end of the SDS
 * string. This happens because SDS strings tend to overallocate to avoid
 * wasting too much time in allocations when appending to the string. */
void trimStringObjectIfNeeded(robj *o) {
    if (o->encoding == OBJ_ENCODING_RAW &&
        sdsavail(szFromObj(o)) > sdslen(szFromObj(o))/10)
    {
        o->m_ptr = sdsRemoveFreeSpace(szFromObj(o));
    }
}

/* Try to encode a string object in order to save space */
/**
 * 尝试对字符串对象进行内存优化编码转换
 *
 * 参数:
 *   o - 需要编码优化的字符串对象，必须为OBJ_STRING类型
 *
 * 返回值:
 *   robj* - 编码优化后的对象指针（可能为原对象或新创建的对象）
 *           返回类型可能包含：
 *           1. 原始对象（未满足优化条件）
 *           2. 整数编码对象（OBJ_ENCODING_INT）
 *           3. 嵌入式字符串对象（OBJ_ENCODING_EMBSTR）
 *           4. 共享整数对象（共享池中的标准对象）
 */
robj *tryObjectEncoding(robj *o) {
    long value;
    sds s = szFromObj(o);
    size_t len;

    /* Make sure this is a string object, the only type we encode
     * in this function. Other types use encoded memory efficient
     * representations but are handled by the commands implementing
     * the type.
     * 确保输入对象为字符串类型
     * Redis的其他数据类型（如列表、集合等）由各自的命令实现处理
     */
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);

    /* We try some specialized encoding only for objects that are
     * RAW or EMBSTR encoded, in other words objects that are still
     * in represented by an actually array of chars.
     * 仅处理原始字符串编码类型：
     * OBJ_ENCODING_RAW（普通SDS）和OBJ_ENCODING_EMBSTR（嵌入式SDS）
     */
    if (!sdsEncodedObject(o)) return o;

    /* It's not safe to encode shared objects: shared objects can be shared
     * everywhere in the "object space" of Redis and may end in places where
     * they are not handled. We handle them only as values in the keyspace.
     * 避免修改被多处引用的共享对象
     * 共享对象可能存在于全局多个位置，修改会导致不可预知的问题
     */
     if (o->getrefcount(std::memory_order_relaxed) > 1) return o;

    /* Check if we can represent this string as a long integer.
     * Note that we are sure that a string larger than 20 chars is not
     * representable as a 32 nor 64 bit integer.
     * 尝试将字符串转换为整数类型：
     * 1. 长度超过20字符的字符串不可能是64位整数
     * 2. 可转换成功时优先使用共享整数池（节省内存）
     * 3. maxmemory启用时禁用共享整数（需要独立的LRU信息）
     */
    len = sdslen(s);
    if (len <= 20 && string2l(s,len,&value)) {
        /* This object is encodable as a long. Try to use a shared object.
         * Note that we avoid using shared integers when maxmemory is used
         * because every object needs to have a private LRU field for the LRU
         * algorithm to work well. */
        if ((g_pserver->maxmemory == 0 ||
            !(g_pserver->maxmemory_policy & MAXMEMORY_FLAG_NO_SHARED_INTEGERS)) &&
            value >= 0 &&
            value < OBJ_SHARED_INTEGERS)
        {
            decrRefCount(o);
            incrRefCount(shared.integers[value]);
            return shared.integers[value];
        } else {
            if (o->encoding == OBJ_ENCODING_RAW) {
                sdsfree(szFromObj(o));
                o->encoding = OBJ_ENCODING_INT;
                o->m_ptr = (void*) value;
                return o;
            } else if (o->encoding == OBJ_ENCODING_EMBSTR) {
                decrRefCount(o);
                return createStringObjectFromLongLongForValue(value);
            }
        }
    }

    /* If the string is small and is still RAW encoded,
     * try the EMBSTR encoding which is more efficient.
     * In this representation the object and the SDS string are allocated
     * in the same chunk of memory to save space and cache misses.
     * 小字符串优化：转换为嵌入式字符串格式
     * 将robj和SDS放在同一内存块，减少内存分配次数和缓存缺失
     */
    if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT) {
        robj *emb;

        if (o->encoding == OBJ_ENCODING_EMBSTR) return o;
        emb = createEmbeddedStringObject(s,sdslen(s));
        decrRefCount(o);
        return emb;
    }

    /* We can't encode the object...
     *
     * Do the last try, and at least optimize the SDS string inside
     * the string object to require little space, in case there
     * is more than 10% of free space at the end of the SDS string.
     *
     * We do that only for relatively large strings as this branch
     * is only entered if the length of the string is greater than
     * OBJ_ENCODING_EMBSTR_SIZE_LIMIT.
     * 大字符串内存优化：
     * 当SDS空闲空间超过10%时，进行内存收缩
     * 此分支仅处理超过嵌入式字符串大小限制的对象
     */
    trimStringObjectIfNeeded(o);

    /* Return the original object. */
    return o;
}

/* Get a decoded version of an encoded object (returned as a new object).
 * If the object is already raw-encoded just increment the ref count. */
robj *getDecodedObject(robj *o) {
    robj *dec;

    if (sdsEncodedObject(o)) {
        incrRefCount(o);
        return o;
    }
    if (o->type == OBJ_STRING && o->encoding == OBJ_ENCODING_INT) {
        char buf[32];

        ll2string(buf,32,(long)ptrFromObj(o));
        dec = createStringObject(buf,strlen(buf));
        return dec;
    } else {
        serverPanic("Unknown encoding type");
    }
}

robj_roptr getDecodedObject(robj_roptr o) {
    return getDecodedObject(o.unsafe_robjcast());
}

/* Compare two string objects via strcmp() or strcoll() depending on flags.
 * Note that the objects may be integer-encoded. In such a case we
 * use ll2string() to get a string representation of the numbers on the stack
 * and compare the strings, it's much faster than calling getDecodedObject().
 *
 * Important note: when REDIS_COMPARE_BINARY is used a binary-safe comparison
 * is used. */

#define REDIS_COMPARE_BINARY (1<<0)
#define REDIS_COMPARE_COLL (1<<1)

int compareStringObjectsWithFlags(robj *a, robj *b, int flags) {
    serverAssertWithInfo(NULL,a,a->type == OBJ_STRING && b->type == OBJ_STRING);
    char bufa[128], bufb[128], *astr, *bstr;
    size_t alen, blen, minlen;

    if (a == b) return 0;
    if (sdsEncodedObject(a)) {
        astr = szFromObj(a);
        alen = sdslen(astr);
    } else {
        alen = ll2string(bufa,sizeof(bufa),(long) ptrFromObj(a));
        astr = bufa;
    }
    if (sdsEncodedObject(b)) {
        bstr = szFromObj(b);
        blen = sdslen(bstr);
    } else {
        blen = ll2string(bufb,sizeof(bufb),(long) ptrFromObj(b));
        bstr = bufb;
    }
    if (flags & REDIS_COMPARE_COLL) {
        return strcoll(astr,bstr);
    } else {
        int cmp;

        minlen = (alen < blen) ? alen : blen;
        cmp = memcmp(astr,bstr,minlen);
        if (cmp == 0) return alen-blen;
        return cmp;
    }
}

/* Wrapper for compareStringObjectsWithFlags() using binary comparison. */
int compareStringObjects(robj *a, robj *b) {
    return compareStringObjectsWithFlags(a,b,REDIS_COMPARE_BINARY);
}

/* Wrapper for compareStringObjectsWithFlags() using collation. */
int collateStringObjects(robj *a, robj *b) {
    return compareStringObjectsWithFlags(a,b,REDIS_COMPARE_COLL);
}

/* Equal string objects return 1 if the two objects are the same from the
 * point of view of a string comparison, otherwise 0 is returned. Note that
 * this function is faster then checking for (compareStringObject(a,b) == 0)
 * because it can perform some more optimization. */
int equalStringObjects(robj *a, robj *b) {
    if (a->encoding == OBJ_ENCODING_INT &&
        b->encoding == OBJ_ENCODING_INT){
        /* If both strings are integer encoded just check if the stored
         * long is the same. */
        return a->m_ptr == b->m_ptr;
    } else {
        return compareStringObjects(a,b) == 0;
    }
}

size_t stringObjectLen(robj_roptr o) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
    if (sdsEncodedObject(o)) {
        return sdslen(szFromObj(o));
    } else {
        return sdigits10((long)ptrFromObj(o));
    }
}

int getDoubleFromObject(const robj *o, double *target) {
    double value;

    if (o == NULL) {
        value = 0;
    } else {
        serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
        if (sdsEncodedObject(o)) {
            if (!string2d(szFromObj(o), sdslen(szFromObj(o)), &value))
                return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) {
            value = (long)ptrFromObj(o);
        } else {
            serverPanic("Unknown string encoding");
        }
    }
    *target = value;
    return C_OK;
}

int getDoubleFromObjectOrReply(client *c, robj *o, double *target, const char *msg) {
    double value;
    if (getDoubleFromObject(o, &value) != C_OK) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is not a valid float");
        }
        return C_ERR;
    }
    *target = value;
    return C_OK;
}

int getLongDoubleFromObject(robj *o, long double *target) {
    long double value;

    if (o == NULL) {
        value = 0;
    } else {
        serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
        if (sdsEncodedObject(o)) {
            if (!string2ld(szFromObj(o), sdslen(szFromObj(o)), &value))
                return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) {
            value = (long)szFromObj(o);
        } else {
            serverPanic("Unknown string encoding");
        }
    }
    *target = value;
    return C_OK;
}

int getLongDoubleFromObjectOrReply(client *c, robj *o, long double *target, const char *msg) {
    long double value;
    if (getLongDoubleFromObject(o, &value) != C_OK) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is not a valid float");
        }
        return C_ERR;
    }
    *target = value;
    return C_OK;
}

int getLongLongFromObject(robj *o, long long *target) {
    long long value;

    if (o == NULL) {
        value = 0;
    } else {
        serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
        if (sdsEncodedObject(o)) {
            if (string2ll(szFromObj(o),sdslen(szFromObj(o)),&value) == 0) return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) {
            value = (long)ptrFromObj(o);
        } else {
            serverPanic("Unknown string encoding");
        }
    }
    if (target) *target = value;
    return C_OK;
}

int getUnsignedLongLongFromObject(robj *o, uint64_t *target) {
    uint64_t value;

    if (o == NULL) {
        value = 0;
    } else {
        serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
        if (sdsEncodedObject(o)) {
            char *pchEnd = nullptr;
            errno = 0;
            value = strtoull(szFromObj(o), &pchEnd, 10);
            if (value == 0) {
                // potential error
                if (errno != 0)
                    return C_ERR;
                if (pchEnd == szFromObj(o))
                    return C_ERR;
            }
        } else if (o->encoding == OBJ_ENCODING_INT) {
            value = (long)ptrFromObj(o);
        } else {
            serverPanic("Unknown string encoding");
        }
    }
    if (target) *target = value;
    return C_OK;
}

int getLongLongFromObjectOrReply(client *c, robj *o, long long *target, const char *msg) {
    long long value;
    if (getLongLongFromObject(o, &value) != C_OK) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is not an integer or out of range");
        }
        return C_ERR;
    }
    *target = value;
    return C_OK;
}

int getUnsignedLongLongFromObjectOrReply(client *c, robj *o, uint64_t *target, const char *msg) {
    uint64_t value;
    if (getUnsignedLongLongFromObject(o, &value) != C_OK) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is not an integer or out of range");
        }
        return C_ERR;
    }
    *target = value;
    return C_OK;
}

int getLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg) {
    long long value;

    if (getLongLongFromObjectOrReply(c, o, &value, msg) != C_OK) return C_ERR;
    if (value < LONG_MIN || value > LONG_MAX) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is out of range");
        }
        return C_ERR;
    }
    *target = value;
    return C_OK;
}

int getRangeLongFromObjectOrReply(client *c, robj *o, long min, long max, long *target, const char *msg) {
    if (getLongFromObjectOrReply(c, o, target, msg) != C_OK) return C_ERR;
    if (*target < min || *target > max) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyErrorFormat(c,"value is out of range, value must between %ld and %ld", min, max);
        }
        return C_ERR;
    }
    return C_OK;
}

int getPositiveLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg) {
    if (msg) {
        return getRangeLongFromObjectOrReply(c, o, 0, LONG_MAX, target, msg);
    } else {
        return getRangeLongFromObjectOrReply(c, o, 0, LONG_MAX, target, "value is out of range, must be positive");
    }
}

int getIntFromObjectOrReply(client *c, robj *o, int *target, const char *msg) {
    long value;

    if (getRangeLongFromObjectOrReply(c, o, INT_MIN, INT_MAX, &value, msg) != C_OK)
        return C_ERR;

    *target = value;
    return C_OK;
}

const char *strEncoding(int encoding) {
    switch(encoding) {
    case OBJ_ENCODING_RAW: return "raw";
    case OBJ_ENCODING_INT: return "int";
    case OBJ_ENCODING_HT: return "hashtable";
    case OBJ_ENCODING_QUICKLIST: return "quicklist";
    case OBJ_ENCODING_ZIPLIST: return "ziplist";
    case OBJ_ENCODING_INTSET: return "intset";
    case OBJ_ENCODING_SKIPLIST: return "skiplist";
    case OBJ_ENCODING_EMBSTR: return "embstr";
    case OBJ_ENCODING_STREAM: return "stream";
    default: return "unknown";
    }
}

/* =========================== Memory introspection ========================= */


/* This is an helper function with the goal of estimating the memory
 * size of a radix tree that is used to store Stream IDs.
 *
 * Note: to guess the size of the radix tree is not trivial, so we
 * approximate it considering 16 bytes of data overhead for each
 * key (the ID), and then adding the number of bare nodes, plus some
 * overhead due by the data and child pointers. This secret recipe
 * was obtained by checking the average radix tree created by real
 * workloads, and then adjusting the constants to get numbers that
 * more or less match the real memory usage.
 *
 * Actually the number of nodes and keys may be different depending
 * on the insertion speed and thus the ability of the radix tree
 * to compress prefixes. */
size_t streamRadixTreeMemoryUsage(rax *rax) {
    size_t size;
    size = rax->numele * sizeof(streamID);
    size += rax->numnodes * sizeof(raxNode);
    /* Add a fixed overhead due to the aux data pointer, children, ... */
    size += rax->numnodes * sizeof(long)*30;
    return size;
}

/* Returns the size in bytes consumed by the key's value in RAM.
 * Note that the returned value is just an approximation, especially in the
 * case of aggregated data types where only "sample_size" elements
 * are checked and averaged to estimate the total size. */
#define OBJ_COMPUTE_SIZE_DEF_SAMPLES 5 /* Default sample size. */
size_t objectComputeSize(robj_roptr o, size_t sample_size) {
    sds ele, ele2;
    dict *d;
    dictIterator *di;
    struct dictEntry *de;
    size_t asize = 0, elesize = 0, samples = 0;

    if (o->type == OBJ_STRING) {
        if(o->encoding == OBJ_ENCODING_INT) {
            asize = sizeof(*o);
        } else if(o->encoding == OBJ_ENCODING_RAW) {
            asize = sdsZmallocSize((sds)szFromObj(o))+sizeof(*o);
        } else if(o->encoding == OBJ_ENCODING_EMBSTR) {
            asize = sdslen(szFromObj(o))+2+sizeof(*o);
        } else {
            serverPanic("Unknown string encoding");
        }
    } else if (o->type == OBJ_LIST) {
        if (o->encoding == OBJ_ENCODING_QUICKLIST) {
            quicklist *ql = (quicklist*)ptrFromObj(o);
            quicklistNode *node = ql->head;
            asize = sizeof(*o)+sizeof(quicklist);
            do {
                elesize += sizeof(quicklistNode)+ziplistBlobLen(node->zl);
                samples++;
            } while ((node = node->next) && samples < sample_size);
            asize += (double)elesize/samples*ql->len;
        } else if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            asize = sizeof(*o)+ziplistBlobLen((unsigned char*)ptrFromObj(o));
        } else {
            serverPanic("Unknown list encoding");
        }
    } else if (o->type == OBJ_SET) {
        if (o->encoding == OBJ_ENCODING_HT) {
            d = (dict*)ptrFromObj(o);
            di = dictGetIterator(d);
            asize = sizeof(*o)+sizeof(dict)+(sizeof(struct dictEntry*)*dictSlots(d));
            while((de = dictNext(di)) != NULL && samples < sample_size) {
                ele = (sds)dictGetKey(de);
                elesize += sizeof(struct dictEntry) + sdsZmallocSize(ele);
                samples++;
            }
            dictReleaseIterator(di);
            if (samples) asize += (double)elesize/samples*dictSize(d);
        } else if (o->encoding == OBJ_ENCODING_INTSET) {
            intset *is = (intset*)ptrFromObj(o);
            asize = sizeof(*o)+sizeof(*is)+(size_t)is->encoding*is->length;
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (o->type == OBJ_ZSET) {
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            asize = sizeof(*o)+(ziplistBlobLen((unsigned char*)ptrFromObj(o)));
        } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
            d = ((zset*)ptrFromObj(o))->dict;
            zskiplist *zsl = ((zset*)ptrFromObj(o))->zsl;
            zskiplistNode *znode = zsl->header->level(0)->forward;
            asize = sizeof(*o)+sizeof(zset)+sizeof(zskiplist)+sizeof(dict)+
                    (sizeof(struct dictEntry*)*dictSlots(d))+
                    zmalloc_size(zsl->header);
            while(znode != NULL && samples < sample_size) {
                elesize += sdsZmallocSize(znode->ele);
                elesize += sizeof(struct dictEntry) + zmalloc_size(znode);
                samples++;
                znode = znode->level(0)->forward;
            }
            if (samples) asize += (double)elesize/samples*dictSize(d);
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else if (o->type == OBJ_HASH) {
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            asize = sizeof(*o)+(ziplistBlobLen((unsigned char*)ptrFromObj(o)));
        } else if (o->encoding == OBJ_ENCODING_HT) {
            d = (dict*)ptrFromObj(o);
            di = dictGetIterator(d);
            asize = sizeof(*o)+sizeof(dict)+(sizeof(struct dictEntry*)*dictSlots(d));
            while((de = dictNext(di)) != NULL && samples < sample_size) {
                ele = (sds)dictGetKey(de);
                ele2 = (sds)dictGetVal(de);
                elesize += sdsZmallocSize(ele) + sdsZmallocSize(ele2);
                elesize += sizeof(struct dictEntry);
                samples++;
            }
            dictReleaseIterator(di);
            if (samples) asize += (double)elesize/samples*dictSize(d);
        } else {
            serverPanic("Unknown hash encoding");
        }
    } else if (o->type == OBJ_STREAM) {
        stream *s = (stream*)ptrFromObj(o);
        asize = sizeof(*o)+sizeof(*s);
        asize += streamRadixTreeMemoryUsage(s->rax);

        /* Now we have to add the listpacks. The last listpack is often non
         * complete, so we estimate the size of the first N listpacks, and
         * use the average to compute the size of the first N-1 listpacks, and
         * finally add the real size of the last node. */
        raxIterator ri;
        raxStart(&ri,s->rax);
        raxSeek(&ri,"^",NULL,0);
        size_t lpsize = 0, samples = 0;
        while(samples < sample_size && raxNext(&ri)) {
            unsigned char *lp = (unsigned char*)ri.data;
            lpsize += lpBytes(lp);
            samples++;
        }
        if (s->rax->numele <= samples) {
            asize += lpsize;
        } else {
            if (samples) lpsize /= samples; /* Compute the average. */
            asize += lpsize * (s->rax->numele-1);
            /* No need to check if seek succeeded, we enter this branch only
             * if there are a few elements in the radix tree. */
            raxSeek(&ri,"$",NULL,0);
            raxNext(&ri);
            asize += lpBytes((unsigned char*)ri.data);
        }
        raxStop(&ri);

        /* Consumer groups also have a non trivial memory overhead if there
         * are many consumers and many groups, let's count at least the
         * overhead of the pending entries in the groups and consumers
         * PELs. */
        if (s->cgroups) {
            raxStart(&ri,s->cgroups);
            raxSeek(&ri,"^",NULL,0);
            while(raxNext(&ri)) {
                streamCG *cg = (streamCG*)ri.data;
                asize += sizeof(*cg);
                asize += streamRadixTreeMemoryUsage(cg->pel);
                asize += sizeof(streamNACK)*raxSize(cg->pel);

                /* For each consumer we also need to add the basic data
                 * structures and the PEL memory usage. */
                raxIterator cri;
                raxStart(&cri,cg->consumers);
                raxSeek(&cri,"^",NULL,0);
                while(raxNext(&cri)) {
                    streamConsumer *consumer = (streamConsumer*)cri.data;
                    asize += sizeof(*consumer);
                    asize += sdslen(consumer->name);
                    asize += streamRadixTreeMemoryUsage(consumer->pel);
                    /* Don't count NACKs again, they are shared with the
                     * consumer group PEL. */
                }
                raxStop(&cri);
            }
            raxStop(&ri);
        }
    } else if (o->type == OBJ_MODULE) {
        moduleValue *mv = (moduleValue*)ptrFromObj(o);
        moduleType *mt = mv->type;
        if (mt->mem_usage != NULL) {
            asize = mt->mem_usage(mv->value);
        } else {
            asize = 0;
        }
    } else {
        serverPanic("Unknown object type");
    }
    return asize;
}

/* Release data obtained with getMemoryOverheadData(). */
void freeMemoryOverheadData(struct redisMemOverhead *mh) {
    zfree(mh->db);
    zfree(mh);
}

/* Return a struct redisMemOverhead filled with memory overhead
 * information used for the MEMORY OVERHEAD and INFO command. The returned
 * structure pointer should be freed calling freeMemoryOverheadData(). */
struct redisMemOverhead *getMemoryOverheadData(void) {
    serverAssert(GlobalLocksAcquired());
    int j;
    size_t mem_total = 0;
    size_t mem = 0;
    size_t zmalloc_used = zmalloc_used_memory();
    struct redisMemOverhead *mh = (redisMemOverhead*)zcalloc(sizeof(*mh), MALLOC_LOCAL);

    mh->total_allocated = zmalloc_used;
    mh->startup_allocated = g_pserver->initial_memory_usage;
    mh->peak_allocated = g_pserver->stat_peak_memory;
    mh->total_frag =
        (float)g_pserver->cron_malloc_stats.process_rss / g_pserver->cron_malloc_stats.zmalloc_used;
    mh->total_frag_bytes =
        g_pserver->cron_malloc_stats.process_rss - g_pserver->cron_malloc_stats.zmalloc_used;
    mh->allocator_frag =
        (float)g_pserver->cron_malloc_stats.allocator_active / g_pserver->cron_malloc_stats.allocator_allocated;
    mh->allocator_frag_bytes =
        g_pserver->cron_malloc_stats.allocator_active - g_pserver->cron_malloc_stats.allocator_allocated;
    mh->allocator_rss =
        (float)g_pserver->cron_malloc_stats.allocator_resident / g_pserver->cron_malloc_stats.allocator_active;
    mh->allocator_rss_bytes =
        g_pserver->cron_malloc_stats.allocator_resident - g_pserver->cron_malloc_stats.allocator_active;
    mh->rss_extra =
        (float)g_pserver->cron_malloc_stats.process_rss / g_pserver->cron_malloc_stats.allocator_resident;
    mh->rss_extra_bytes =
        g_pserver->cron_malloc_stats.process_rss - g_pserver->cron_malloc_stats.allocator_resident;

    mem_total += g_pserver->initial_memory_usage;

    mem = 0;
    if (g_pserver->repl_backlog && g_pserver->repl_backlog != g_pserver->repl_backlog_disk)
        mem += zmalloc_size(g_pserver->repl_backlog);
    mh->repl_backlog = mem;
    mem_total += mem;

    /* Computing the memory used by the clients would be O(N) if done
     * here online. We use our values computed incrementally by
     * clientsCronTrackClientsMemUsage(). */
    mh->clients_slaves = g_pserver->stat_clients_type_memory[CLIENT_TYPE_SLAVE];
    mh->clients_normal = g_pserver->stat_clients_type_memory[CLIENT_TYPE_MASTER]+
                         g_pserver->stat_clients_type_memory[CLIENT_TYPE_PUBSUB]+
                         g_pserver->stat_clients_type_memory[CLIENT_TYPE_NORMAL];
    mem_total += mh->clients_slaves;
    mem_total += mh->clients_normal;

    mem = 0;
    if (g_pserver->aof_state != AOF_OFF) {
        mem += sdsZmallocSize(g_pserver->aof_buf);
        mem += aofRewriteBufferSize();
    }
    mh->aof_buffer = mem;
    mem_total+=mem;

    mem = g_pserver->lua_scripts_mem;
    mem += dictSize(g_pserver->lua_scripts) * sizeof(dictEntry) +
        dictSlots(g_pserver->lua_scripts) * sizeof(dictEntry*);
    mem += dictSize(g_pserver->repl_scriptcache_dict) * sizeof(dictEntry) +
        dictSlots(g_pserver->repl_scriptcache_dict) * sizeof(dictEntry*);
    if (listLength(g_pserver->repl_scriptcache_fifo) > 0) {
        mem += listLength(g_pserver->repl_scriptcache_fifo) * (sizeof(listNode) + 
            sdsZmallocSize((sds)listNodeValue(listFirst(g_pserver->repl_scriptcache_fifo))));
    }
    mh->lua_caches = mem;
    mem_total+=mem;

    for (j = 0; j < cserver.dbnum; j++) {
        redisDb *db = g_pserver->db[j];
        long long keyscount = db->size();
        if (keyscount==0) continue;

        mh->total_keys += keyscount;
        mh->db = (decltype(mh->db))zrealloc(mh->db,sizeof(mh->db[0])*(mh->num_dbs+1), MALLOC_LOCAL);
        mh->db[mh->num_dbs].dbid = j;

        mem = db->size() * sizeof(dictEntry) +
              db->slots() * sizeof(dictEntry*) +
              db->size() * sizeof(robj);
        mh->db[mh->num_dbs].overhead_ht_main = mem;
        mem_total+=mem;
        
        mh->db[mh->num_dbs].overhead_ht_expires = 0;

        mh->num_dbs++;
    }

    mh->overhead_total = mem_total;
    mh->dataset = zmalloc_used - mem_total;
    mh->peak_perc = (float)zmalloc_used*100/mh->peak_allocated;

    /* Metrics computed after subtracting the startup memory from
     * the total memory. */
    size_t net_usage = 1;
    if (zmalloc_used > mh->startup_allocated)
        net_usage = zmalloc_used - mh->startup_allocated;
    mh->dataset_perc = (float)mh->dataset*100/net_usage;
    mh->bytes_per_key = mh->total_keys ? (net_usage / mh->total_keys) : 0;

    return mh;
}

/* Helper for "MEMORY allocator-stats", used as a callback for the jemalloc
 * stats output. */
void inputCatSds(void *result, const char *str) {
    /* result is actually a (sds *), so re-cast it here */
    sds *info = (sds *)result;
    *info = sdscat(*info, str);
}

/* This implements MEMORY DOCTOR. An human readable analysis of the Redis
 * memory condition. */
sds getMemoryDoctorReport(void) {
    serverAssert(GlobalLocksAcquired());
    int empty = 0;          /* Instance is empty or almost empty. */
    int big_peak = 0;       /* Memory peak is much larger than used mem. */
    int high_frag = 0;      /* High fragmentation. */
    int high_alloc_frag = 0;/* High allocator fragmentation. */
    int high_proc_rss = 0;  /* High process rss overhead. */
    int high_alloc_rss = 0; /* High rss overhead. */
    int big_slave_buf = 0;  /* Slave buffers are too big. */
    int big_client_buf = 0; /* Client buffers are too big. */
    int many_scripts = 0;   /* Script cache has too many scripts. */
    int num_reports = 0;
    struct redisMemOverhead *mh = getMemoryOverheadData();

    if (mh->total_allocated < (1024*1024*5)) {
        empty = 1;
        num_reports++;
    } else {
        /* Peak is > 150% of current used memory? */
        if (((float)mh->peak_allocated / mh->total_allocated) > 1.5) {
            big_peak = 1;
            num_reports++;
        }

        /* Fragmentation is higher than 1.4 and 10MB ?*/
        if (mh->total_frag > 1.4 && mh->total_frag_bytes > 10<<20) {
            high_frag = 1;
            num_reports++;
        }

        /* External fragmentation is higher than 1.1 and 10MB? */
        if (mh->allocator_frag > 1.1 && mh->allocator_frag_bytes > 10<<20) {
            high_alloc_frag = 1;
            num_reports++;
        }

        /* Allocator rss is higher than 1.1 and 10MB ? */
        if (mh->allocator_rss > 1.1 && mh->allocator_rss_bytes > 10<<20) {
            high_alloc_rss = 1;
            num_reports++;
        }

        /* Non-Allocator rss is higher than 1.1 and 10MB ? */
        if (mh->rss_extra > 1.1 && mh->rss_extra_bytes > 10<<20) {
            high_proc_rss = 1;
            num_reports++;
        }

        /* Clients using more than 200k each average? */
        long numslaves = listLength(g_pserver->slaves);
        long numclients = listLength(g_pserver->clients)-numslaves;
        if ((numclients > 0) && mh->clients_normal / numclients > (1024*200)) {
            big_client_buf = 1;
            num_reports++;
        }

        /* Slaves using more than 10 MB each? */
        if (numslaves > 0 && mh->clients_slaves / numslaves > (1024*1024*10)) {
            big_slave_buf = 1;
            num_reports++;
        }

        /* Too many scripts are cached? */
        if (dictSize(g_pserver->lua_scripts) > 1000) {
            many_scripts = 1;
            num_reports++;
        }
    }

    sds s;
    if (num_reports == 0) {
        s = sdsnew(
        "Hi Sam, I can't find any memory issue in your instance. "
        "I can only account for what occurs on this base.\n");
    } else if (empty == 1) {
        s = sdsnew(
        "Hi Sam, this instance is empty or is using very little memory, "
        "my issues detector can't be used in these conditions. "
        "Please, leave for your mission on Earth and fill it with some data. "
        "The new Sam and I will be back to our programming as soon as I "
        "finished rebooting.\n");
    } else {
        s = sdsnew("Sam, I detected a few issues in this KeyDB instance memory implants:\n\n");
        if (big_peak) {
            s = sdscat(s," * Peak memory: In the past this instance used more than 150% the memory that is currently using. The allocator is normally not able to release memory after a peak, so you can expect to see a big fragmentation ratio, however this is actually harmless and is only due to the memory peak, and if the KeyDB instance Resident Set Size (RSS) is currently bigger than expected, the memory will be used as soon as you fill the KeyDB instance with more data. If the memory peak was only occasional and you want to try to reclaim memory, please try the MEMORY PURGE command, otherwise the only other option is to shutdown and restart the instance.\n\n");
        }
        if (high_frag) {
            s = sdscatprintf(s," * High total RSS: This instance has a memory fragmentation and RSS overhead greater than 1.4 (this means that the Resident Set Size of the KeyDB process is much larger than the sum of the logical allocations KeyDB performed). This problem is usually due either to a large peak memory (check if there is a peak memory entry above in the report) or may result from a workload that causes the allocator to fragment memory a lot. If the problem is a large peak memory, then there is no issue. Otherwise, make sure you are using the Jemalloc allocator and not the default libc malloc. Note: The currently used allocator is \"%s\".\n\n", ZMALLOC_LIB);
        }
        if (high_alloc_frag) {
            s = sdscatprintf(s," * High allocator fragmentation: This instance has an allocator external fragmentation greater than 1.1. This problem is usually due either to a large peak memory (check if there is a peak memory entry above in the report) or may result from a workload that causes the allocator to fragment memory a lot. You can try enabling 'activedefrag' config option.\n\n");
        }
        if (high_alloc_rss) {
            s = sdscatprintf(s," * High allocator RSS overhead: This instance has an RSS memory overhead is greater than 1.1 (this means that the Resident Set Size of the allocator is much larger than the sum what the allocator actually holds). This problem is usually due to a large peak memory (check if there is a peak memory entry above in the report), you can try the MEMORY PURGE command to reclaim it.\n\n");
        }
        if (high_proc_rss) {
            s = sdscatprintf(s," * High process RSS overhead: This instance has non-allocator RSS memory overhead is greater than 1.1 (this means that the Resident Set Size of the KeyDB process is much larger than the RSS the allocator holds). This problem may be due to Lua scripts or Modules.\n\n");
        }
        if (big_slave_buf) {
            s = sdscat(s," * Big replica buffers: The replica output buffers in this instance are greater than 10MB for each replica (on average). This likely means that there is some replica instance that is struggling receiving data, either because it is too slow or because of networking issues. As a result, data piles on the master output buffers. Please try to identify what replica is not receiving data correctly and why. You can use the INFO output in order to check the replicas delays and the CLIENT LIST command to check the output buffers of each replica.\n\n");
        }
        if (big_client_buf) {
            s = sdscat(s," * Big client buffers: The clients output buffers in this instance are greater than 200K per client (on average). This may result from different causes, like Pub/Sub clients subscribed to channels bot not receiving data fast enough, so that data piles on the KeyDB instance output buffer, or clients sending commands with large replies or very large sequences of commands in the same pipeline. Please use the CLIENT LIST command in order to investigate the issue if it causes problems in your instance, or to understand better why certain clients are using a big amount of memory.\n\n");
        }
        if (many_scripts) {
            s = sdscat(s," * Many scripts: There seem to be many cached scripts in this instance (more than 1000). This may be because scripts are generated and `EVAL`ed, instead of being parameterized (with KEYS and ARGV), `SCRIPT LOAD`ed and `EVALSHA`ed. Unless `SCRIPT FLUSH` is called periodically, the scripts' caches may end up consuming most of your memory.\n\n");
        }
        s = sdscat(s,"I'm here to keep you safe, Sam. I want to help you.\n");
    }
    freeMemoryOverheadData(mh);
    return s;
}

/* Set the object LRU/LFU depending on g_pserver->maxmemory_policy.
 * The lfu_freq arg is only relevant if policy is MAXMEMORY_FLAG_LFU.
 * The lru_idle and lru_clock args are only relevant if policy
 * is MAXMEMORY_FLAG_LRU.
 * Either or both of them may be <0, in that case, nothing is set. */
int objectSetLRUOrLFU(robj *val, long long lfu_freq, long long lru_idle,
                       long long lru_clock, int lru_multiplier) {
    if (g_pserver->maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        if (lfu_freq >= 0) {
            serverAssert(lfu_freq <= 255);
            val->lru = (LFUGetTimeInMinutes()<<8) | lfu_freq;
            return 1;
        }
    } else if (lru_idle >= 0) {
        /* Provided LRU idle time is in seconds. Scale
         * according to the LRU clock resolution this Redis
         * instance was compiled with (normally 1000 ms, so the
         * below statement will expand to lru_idle*1000/1000. */
        lru_idle = lru_idle*lru_multiplier/LRU_CLOCK_RESOLUTION;
        long lru_abs = lru_clock - lru_idle; /* Absolute access time. */
        /* If the LRU field underflows (since LRU it is a wrapping
         * clock), the best we can do is to provide a large enough LRU
         * that is half-way in the circlular LRU clock we use: this way
         * the computed idle time for this object will stay high for quite
         * some time. */
        if (lru_abs < 0)
            lru_abs = (lru_clock+(LRU_CLOCK_MAX/2)) % LRU_CLOCK_MAX;
        val->lru = lru_abs;
        return 1;
    }
    return 0;
}

/* ======================= The OBJECT and MEMORY commands =================== */

/* This is a helper function for the OBJECT command. We need to lookup keys
 * without any modification of LRU or other parameters. */
robj_roptr objectCommandLookup(client *c, robj *key) {
    return c->db->find(key);
}

robj_roptr objectCommandLookupOrReply(client *c, robj *key, robj *reply) {
    robj_roptr o = objectCommandLookup(c,key);
    if (!o) SentReplyOnKeyMiss(c, reply);
    return o;
}

/* Object command allows to inspect the internals of a Redis Object.
 * Usage: OBJECT <refcount|encoding|idletime|freq> <key> */
void objectCommand(client *c) {
    robj_roptr o;

    if (c->argc == 2 && !strcasecmp(szFromObj(c->argv[1]),"help")) {
        const char *help[] = {
"ENCODING <key>",
"    Return the kind of internal representation used in order to store the value",
"    associated with a <key>.",
"FREQ <key>",
"    Return the access frequency index of the <key>. The returned integer is",
"    proportional to the logarithm of the recent access frequency of the key.",
"IDLETIME <key>",
"    Return the idle time of the <key>, that is the approximated number of",
"    seconds elapsed since the last access to the key.",
"REFCOUNT <key>",
"    Return the number of references of the value associated with the specified",
"    <key>.",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(szFromObj(c->argv[1]),"refcount") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == nullptr) return;
        addReplyLongLong(c,o->getrefcount(std::memory_order_relaxed));
    } else if (!strcasecmp(szFromObj(c->argv[1]),"encoding") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == nullptr) return;
        addReplyBulkCString(c,strEncoding(o->encoding));
    } else if (!strcasecmp(szFromObj(c->argv[1]),"idletime") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == nullptr) return;
        if (g_pserver->maxmemory_policy & MAXMEMORY_FLAG_LFU) {
            addReplyError(c,"An LFU maxmemory policy is selected, idle time not tracked. Please note that when switching between policies at runtime LRU and LFU data will take some time to adjust.");
            return;
        }
        addReplyLongLong(c,estimateObjectIdleTime(o)/1000);
    } else if (!strcasecmp(szFromObj(c->argv[1]),"freq") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == nullptr) return;
        if (!(g_pserver->maxmemory_policy & MAXMEMORY_FLAG_LFU)) {
            addReplyError(c,"An LFU maxmemory policy is not selected, access frequency not tracked. Please note that when switching between policies at runtime LRU and LFU data will take some time to adjust.");
            return;
        }
        /* LFUDecrAndReturn should be called
         * in case of the key has not been accessed for a long time,
         * because we update the access time only
         * when the key is read or overwritten. */
        addReplyLongLong(c,LFUDecrAndReturn(o.unsafe_robjcast()));
    } else if (!strcasecmp(szFromObj(c->argv[1]), "lastmodified") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == nullptr) return;
        uint64_t mvcc = mvccFromObj(o);
        addReplyLongLong(c, (g_pserver->mstime - (mvcc >> MVCC_MS_SHIFT)) / 1000);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

/* The memory command will eventually be a complete interface for the
 * memory introspection capabilities of Redis.
 *
 * Usage: MEMORY usage <key> */
void memoryCommand(client *c) {
    if (!strcasecmp(szFromObj(c->argv[1]),"help") && c->argc == 2) {
        const char *help[] = {
            "DOCTOR",
            "    Return memory problems reports.",
            "MALLOC-STATS"
            "    Return internal statistics report from the memory allocator.",
            "PURGE",
            "    Attempt to purge dirty pages for reclamation by the allocator.",
            "STATS",
            "    Return information about the memory usage of the server.",
            "USAGE <key> [SAMPLES <count>]",
            "    Return memory in bytes used by <key> and its value. Nested values are",
            "    sampled up to <count> times (default: 5).",
            NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(szFromObj(c->argv[1]),"usage") && c->argc >= 3) {
        long long samples = OBJ_COMPUTE_SIZE_DEF_SAMPLES;
        for (int j = 3; j < c->argc; j++) {
            if (!strcasecmp(szFromObj(c->argv[j]),"samples") &&
                j+1 < c->argc)
            {
                if (getLongLongFromObjectOrReply(c,c->argv[j+1],&samples,NULL)
                     == C_ERR) return;
                if (samples < 0) {
                    addReplyErrorObject(c,shared.syntaxerr);
                    return;
                }
                if (samples == 0) samples = LLONG_MAX;
                j++; /* skip option argument. */
            } else {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
        }

        auto itr = c->db->find(c->argv[2]);
        if (itr == nullptr) {
            addReplyNull(c);
            return;
        }
        size_t usage = objectComputeSize(itr.val(),samples);
        usage += sdsZmallocSize(itr.key());
        usage += sizeof(dictEntry);
        addReplyLongLong(c,usage);
    } else if (!strcasecmp(szFromObj(c->argv[1]),"stats") && c->argc == 2) {
        struct redisMemOverhead *mh = getMemoryOverheadData();

        addReplyMapLen(c,25+mh->num_dbs);

        addReplyBulkCString(c,"peak.allocated");
        addReplyLongLong(c,mh->peak_allocated);

        addReplyBulkCString(c,"total.allocated");
        addReplyLongLong(c,mh->total_allocated);

        addReplyBulkCString(c,"startup.allocated");
        addReplyLongLong(c,mh->startup_allocated);

        addReplyBulkCString(c,"replication.backlog");
        addReplyLongLong(c,mh->repl_backlog);

        addReplyBulkCString(c,"clients.slaves");
        addReplyLongLong(c,mh->clients_slaves);

        addReplyBulkCString(c,"clients.normal");
        addReplyLongLong(c,mh->clients_normal);

        addReplyBulkCString(c,"aof.buffer");
        addReplyLongLong(c,mh->aof_buffer);

        addReplyBulkCString(c,"lua.caches");
        addReplyLongLong(c,mh->lua_caches);

        for (size_t j = 0; j < mh->num_dbs; j++) {
            char dbname[32];
            snprintf(dbname,sizeof(dbname),"db.%zd",mh->db[j].dbid);
            addReplyBulkCString(c,dbname);
            addReplyMapLen(c,2);

            addReplyBulkCString(c,"overhead.hashtable.main");
            addReplyLongLong(c,mh->db[j].overhead_ht_main);

            addReplyBulkCString(c,"overhead.hashtable.expires");
            addReplyLongLong(c,mh->db[j].overhead_ht_expires);
        }

        addReplyBulkCString(c,"overhead.total");
        addReplyLongLong(c,mh->overhead_total);

        addReplyBulkCString(c,"keys.count");
        addReplyLongLong(c,mh->total_keys);

        addReplyBulkCString(c,"keys.bytes-per-key");
        addReplyLongLong(c,mh->bytes_per_key);

        addReplyBulkCString(c,"dataset.bytes");
        addReplyLongLong(c,mh->dataset);

        addReplyBulkCString(c,"dataset.percentage");
        addReplyDouble(c,mh->dataset_perc);

        addReplyBulkCString(c,"peak.percentage");
        addReplyDouble(c,mh->peak_perc);

        addReplyBulkCString(c,"allocator.allocated");
        addReplyLongLong(c,g_pserver->cron_malloc_stats.allocator_allocated);

        addReplyBulkCString(c,"allocator.active");
        addReplyLongLong(c,g_pserver->cron_malloc_stats.allocator_active);

        addReplyBulkCString(c,"allocator.resident");
        addReplyLongLong(c,g_pserver->cron_malloc_stats.allocator_resident);

        addReplyBulkCString(c,"allocator-fragmentation.ratio");
        addReplyDouble(c,mh->allocator_frag);

        addReplyBulkCString(c,"allocator-fragmentation.bytes");
        addReplyLongLong(c,mh->allocator_frag_bytes);

        addReplyBulkCString(c,"allocator-rss.ratio");
        addReplyDouble(c,mh->allocator_rss);

        addReplyBulkCString(c,"allocator-rss.bytes");
        addReplyLongLong(c,mh->allocator_rss_bytes);

        addReplyBulkCString(c,"rss-overhead.ratio");
        addReplyDouble(c,mh->rss_extra);

        addReplyBulkCString(c,"rss-overhead.bytes");
        addReplyLongLong(c,mh->rss_extra_bytes);

        addReplyBulkCString(c,"fragmentation"); /* this is the total RSS overhead, including fragmentation */
        addReplyDouble(c,mh->total_frag); /* it is kept here for backwards compatibility */

        addReplyBulkCString(c,"fragmentation.bytes");
        addReplyLongLong(c,mh->total_frag_bytes);

        freeMemoryOverheadData(mh);
    } else if (!strcasecmp(szFromObj(c->argv[1]),"malloc-stats") && c->argc == 2) {
#if defined(USE_JEMALLOC)
        sds info = sdsempty();
        malloc_stats_print(inputCatSds, &info, NULL);
        addReplyVerbatim(c,info,sdslen(info),"txt");
        sdsfree(info);
#else
        addReplyBulkCString(c,"Stats not supported for the current allocator");
#endif
    } else if (!strcasecmp(szFromObj(c->argv[1]),"doctor") && c->argc == 2) {
        sds report = getMemoryDoctorReport();
        addReplyVerbatim(c,report,sdslen(report),"txt");
        sdsfree(report);
    } else if (!strcasecmp(szFromObj(c->argv[1]),"purge") && c->argc == 2) {
        if (jemalloc_purge() == 0)
            addReply(c, shared.ok);
        else
            addReplyError(c, "Error purging dirty pages");
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

void redisObject::SetFExpires(bool fExpire)
{
    serverAssert(this->refcount != OBJ_SHARED_REFCOUNT || fExpire == FExpires());
    if (fExpire)
        this->refcount.fetch_or(1U << 31, std::memory_order_relaxed);
    else
        this->refcount.fetch_and(~(1U << 31), std::memory_order_relaxed);
}

void redisObject::setrefcount(unsigned ref)
{ 
    serverAssert(!FExpires());
    refcount.store(ref, std::memory_order_relaxed); 
}

sds serializeStoredStringObject(sds str, robj_roptr o)
{
    uint64_t mvcc;
    mvcc = mvccFromObj(o);
    str = sdscatlen(str, &mvcc, sizeof(mvcc));
    str = sdscatlen(str, &(*o), sizeof(robj));
    static_assert((sizeof(robj) + sizeof(mvcc)) == sizeof(redisObjectStack), "");
    switch (o->encoding)
    {
    case OBJ_ENCODING_EMBSTR:
    case OBJ_ENCODING_RAW:
        str = sdscatsds(str, (sds)szFromObj(o));
        break;

    case OBJ_ENCODING_INT:
        break;  //nop
    }
        
    return str;
}

robj *deserializeStoredStringObject(const char *data, size_t cb)
{
    uint64_t mvcc = *reinterpret_cast<const uint64_t*>(data);
    const robj *oT = (const robj*)(data+sizeof(uint64_t));
    robj *newObject = nullptr;
    switch (oT->encoding)
    {
    case OBJ_ENCODING_INT:
        newObject = createObject(OBJ_STRING, nullptr);
        newObject->encoding = oT->encoding;
        newObject->m_ptr = oT->m_ptr;
        break;

    case OBJ_ENCODING_EMBSTR:
        newObject = createEmbeddedStringObject(data+sizeof(robj)+sizeof(mvcc), cb-sizeof(robj)-sizeof(uint64_t));
        break;

    case OBJ_ENCODING_RAW:
        newObject = createObject(OBJ_STRING, sdsnewlen(SDS_NOINIT,cb-sizeof(robj)-sizeof(uint64_t)));
        newObject->lru = oT->lru;
        memcpy(newObject->m_ptr, data+sizeof(robj)+sizeof(mvcc), cb-sizeof(robj)-sizeof(mvcc));
        break;

    default:
        serverPanic("Unknown string object encoding from storage");
    }
    setMvccTstamp(newObject, mvcc);

    return newObject;
}

robj *deserializeStoredObject(const void *data, size_t cb)
{
    switch (((char*)data)[0])
    {
        case RDB_TYPE_STRING:
            return deserializeStoredStringObject(((char*)data)+1, cb-1);

        default:
            rio payload;
            int type;
            robj *obj;
            rioInitWithConstBuffer(&payload,data,cb);
            if (((type = rdbLoadObjectType(&payload)) == -1) ||
                ((obj = rdbLoadObject(type,&payload,nullptr,NULL,OBJ_MVCC_INVALID)) == nullptr))
            {
                serverPanic("Bad data format");
            }
            if (rdbLoadType(&payload) == RDB_OPCODE_AUX)
            {
                robj *auxkey, *auxval;
                if ((auxkey = rdbLoadStringObject(&payload)) == NULL) goto eoferr;
                if ((auxval = rdbLoadStringObject(&payload)) == NULL) {
                    decrRefCount(auxkey);
                    goto eoferr;
                }
                if (strcasecmp(szFromObj(auxkey), "mvcc-tstamp") == 0) {
                    setMvccTstamp(obj, strtoull(szFromObj(auxval), nullptr, 10));
                }
                decrRefCount(auxkey);
                decrRefCount(auxval);
            }
        eoferr:
            return obj;
    }
    serverPanic("Unknown object type loading from storage");
}

sds serializeStoredObject(robj_roptr o, sds sdsPrefix)
{
    switch (o->type)
    {
        case OBJ_STRING:
        {
            sds sdsT = nullptr;
            if (sdsPrefix)
                sdsT = sdsgrowzero(sdsPrefix, sdslen(sdsPrefix)+1);
            else
                sdsT = sdsnewlen(nullptr, 1);
            sdsT[sdslen(sdsT)-1] = RDB_TYPE_STRING;
            return serializeStoredStringObject(sdsT, o);
        }
            
        default:
            rio rdb;
            createDumpPayload(&rdb,o,nullptr);
            if (sdsPrefix)
            {
                sds rval = sdscatsds(sdsPrefix, (sds)rdb.io.buffer.ptr);
                sdsfree((sds)rdb.io.buffer.ptr);
                return rval;
            }
            return (sds)rdb.io.buffer.ptr;
    }
    serverPanic("Attempting to store unknown object type");
}

redisObjectStack::redisObjectStack()
{
    // We need to ensure the Extended Object is first in the class layout
    serverAssert(reinterpret_cast<ptrdiff_t>(static_cast<redisObject*>(this)) != reinterpret_cast<ptrdiff_t>(this));
}

void *allocPtrFromObj(robj_roptr o) {
    if (g_pserver->fActiveReplica)
        return reinterpret_cast<redisObjectExtended*>(o.unsafe_robjcast()) - 1;
    return o.unsafe_robjcast();
}

robj *objFromAllocPtr(void *pv) {
    if (pv != nullptr && g_pserver->fActiveReplica) {
        return reinterpret_cast<robj*>(reinterpret_cast<redisObjectExtended*>(pv)+1);
    } 
    return reinterpret_cast<robj*>(pv);
}

uint64_t mvccFromObj(robj_roptr o)
{
    if (g_pserver->fActiveReplica) {
        redisObjectExtended *oe = reinterpret_cast<redisObjectExtended*>(o.unsafe_robjcast()) - 1;
        return oe->mvcc_tstamp;
    }
    return OBJ_MVCC_INVALID;
}


/**
 * 设置指定Redis对象的MVCC时间戳。
 *
 * 参数:
 * o    - 指向robj对象的指针，该对象必须与其扩展结构体redisObjectExtended连续存储
 * mvcc - 要设置的MVCC时间戳值（64位无符号整数）
 *
 * 返回值: 无
 *
 * 注意: 该函数仅在当前实例为活跃副本时生效
 */
void setMvccTstamp(robj *o, uint64_t mvcc)
{
    /**
     * 检查当前实例是否为活跃副本节点
     * 非活跃副本直接返回，不执行任何操作
     */
    if (!g_pserver->fActiveReplica)
        return;

    /**
     * 定位关联的扩展结构体
     * 通过指针运算获取robj对象前序存储的redisObjectExtended结构体
     * 这种内存布局要求两个结构体必须连续存储
     */
    redisObjectExtended *oe = reinterpret_cast<redisObjectExtended*>(o) - 1;

    /**
     * 更新扩展结构体中的MVCC时间戳字段
     * 这个时间戳用于实现多版本并发控制（MVCC）机制
     */
    oe->mvcc_tstamp = mvcc;
}

