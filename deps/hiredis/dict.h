/* Hash table implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __DICT_H
#define __DICT_H

#define DICT_OK 0
#define DICT_ERR 1

/* Unused arguments generate annoying warnings... */
#define DICT_NOTUSED(V) ((void) V)

/*
 * 哈希表节点结构
 * 每个节点存储键值对及指向下一个节点的指针
 */
typedef struct dictEntry {
    void *key;      // 键指针
    void *val;      // 值指针
    struct dictEntry *next;  // 冲突链表指针
} dictEntry;

/*
 * 字典类型操作函数族定义
 * 包含哈希表所需的函数指针集合
 */
typedef struct dictType {
    unsigned int (*hashFunction)(const void *key);  // 哈希计算函数
    void *(*keyDup)(void *privdata, const void *key);  // 键复制函数
    void *(*valDup)(void *privdata, const void *obj);  // 值复制函数
    int (*keyCompare)(void *privdata, const void *key1, const void *key2);  // 键比较函数
    void (*keyDestructor)(void *privdata, void *key);  // 键销毁函数
    void (*valDestructor)(void *privdata, void *obj);  // 值销毁函数
} dictType;

/*
 * 哈希表核心结构
 * 包含表数据及操作函数集合
 */
typedef struct dict {
    dictEntry **table;      // 哈希表存储数组
    dictType *type;         // 类型操作函数集合
    unsigned long size;     // 表容量
    unsigned long sizemask; // 容量掩码（用于快速取模）
    unsigned long used;     // 已使用节点数
    void *privdata;         // 私有数据指针
} dict;

/*
 * 字典迭代器结构
 * 用于安全遍历哈希表元素
 */
typedef struct dictIterator {
    dict *ht;        // 关联的哈希表
    int index;       // 当前桶索引
    dictEntry *entry;   // 当前节点指针
    dictEntry *nextEntry;  // 预取的下一个节点
} dictIterator;


/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_SIZE     4

/* ------------------------------- Macros ------------------------------------*/
#define dictFreeEntryVal(ht, entry) \
    if ((ht)->type->valDestructor) \
        (ht)->type->valDestructor((ht)->privdata, (entry)->val)

#define dictSetHashVal(ht, entry, _val_) do { \
    if ((ht)->type->valDup) \
        entry->val = (ht)->type->valDup((ht)->privdata, _val_); \
    else \
        entry->val = (_val_); \
} while(0)

#define dictFreeEntryKey(ht, entry) \
    if ((ht)->type->keyDestructor) \
        (ht)->type->keyDestructor((ht)->privdata, (entry)->key)

#define dictSetHashKey(ht, entry, _key_) do { \
    if ((ht)->type->keyDup) \
        entry->key = (ht)->type->keyDup((ht)->privdata, _key_); \
    else \
        entry->key = (_key_); \
} while(0)

#define dictCompareHashKeys(ht, key1, key2) \
    (((ht)->type->keyCompare) ? \
        (ht)->type->keyCompare((ht)->privdata, key1, key2) : \
        (key1) == (key2))

#define dictHashKey(ht, key) (ht)->type->hashFunction(key)

#define dictGetEntryKey(he) ((he)->key)
#define dictGetEntryVal(he) ((he)->val)
#define dictSlots(ht) ((ht)->size)
#define dictSize(ht) ((ht)->used)

/* API */
static unsigned int dictGenHashFunction(const unsigned char *buf, int len);
static dict *dictCreate(dictType *type, void *privDataPtr);
static int dictExpand(dict *ht, unsigned long size);
static int dictAdd(dict *ht, void *key, void *val);
static int dictReplace(dict *ht, void *key, void *val);
static int dictDelete(dict *ht, const void *key);
static void dictRelease(dict *ht);
static dictEntry * dictFind(dict *ht, const void *key);
static dictIterator *dictGetIterator(dict *ht);
static dictEntry *dictNext(dictIterator *iter);
static void dictReleaseIterator(dictIterator *iter);

#endif /* __DICT_H */
