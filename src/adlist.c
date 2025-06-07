/* adlist.c - A generic doubly linked list implementation
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


#include <stdlib.h>
#include "adlist.h"
#include "zmalloc.h"

/* 创建新链表。
 * 生成的链表可通过 listRelease() 释放，
 * 但调用 listRelease() 前需由用户显式释放每个节点的私有值，
 * 或通过 listSetFreeMethod 设置自动释放方法。
 *
 * 错误时返回 NULL，成功时返回指向新链表的指针。*/
list *listCreate(void)
{
    struct list *list;

    if ((list = zmalloc(sizeof(*list), MALLOC_SHARED)) == NULL)
        return NULL;
    list->head = list->tail = NULL;
    list->len = 0;
    list->dup = NULL;
    list->free = NULL;
    list->match = NULL;
    return list;
}

/* 清空链表中的所有元素而不销毁链表本身 */
void listEmpty(list *list)
{
    unsigned long len;
    listNode *current, *next;

    current = list->head;
    len = list->len;
    while(len--) {
        next = current->next;
        if (list->free) list->free(current->value);
        zfree(current);
        current = next;
    }
    list->head = list->tail = NULL;
    list->len = 0;
}

/* 释放整个链表。
 *
 * 此函数不会失败 */
void listRelease(list *list)
{
    listEmpty(list);
    zfree(list);
}

/* 向链表头部添加新节点，该节点将包含指定的 'value' 指针作为值。
 *
 * 若发生错误，返回 NULL 且链表保持原状；
 * 若操作成功，返回传递给函数的 'list' 指针本身。 */
list *listAddNodeHead(list *list, void *value)
{
    listNode *node;

    if ((node = zmalloc(sizeof(*node), MALLOC_SHARED)) == NULL)
        return NULL;
    node->value = value;
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        node->prev = NULL;
        node->next = list->head;
        list->head->prev = node;
        list->head = node;
    }
    list->len++;
    return list;
}

/* 向链表尾部添加新节点，该节点将包含指定的 'value' 指针作为值。
 *
 * 若发生错误，返回 NULL 且链表保持原状；
 * 若操作成功，返回传递给函数的 'list' 指针本身。 */
list *listAddNodeTail(list *list, void *value)
{
    listNode *node;

    if ((node = zmalloc(sizeof(*node), MALLOC_SHARED)) == NULL)
        return NULL;
    node->value = value;
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        node->prev = list->tail;
        node->next = NULL;
        list->tail->next = node;
        list->tail = node;
    }
    list->len++;
    return list;
}

/* 在链表中插入新节点 */
list *listInsertNode(list *list, listNode *old_node, void *value, int after) {
    listNode *node;

    if ((node = zmalloc(sizeof(*node), MALLOC_SHARED)) == NULL)
        return NULL;
    node->value = value;
    if (after) {
        node->prev = old_node;
        node->next = old_node->next;
        if (list->tail == old_node) {
            list->tail = node;
        }
    } else {
        node->next = old_node;
        node->prev = old_node->prev;
        if (list->head == old_node) {
            list->head = node;
        }
    }
    if (node->prev != NULL) {
        node->prev->next = node;
    }
    if (node->next != NULL) {
        node->next->prev = node;
    }
    list->len++;
    return list;
}

/* 从指定链表中移除指定节点。
 * 调用方需负责释放该节点的私有值。
 *
 * 此函数执行不会失败 */
void listDelNode(list *list, listNode *node)
{
    if (node->prev)
        node->prev->next = node->next;
    else
        list->head = node->next;
    if (node->next)
        node->next->prev = node->prev;
    else
        list->tail = node->prev;
    if (list->free) list->free(node->value);
    zfree(node);
    list->len--;
}

/* 返回链表迭代器 'iter'。初始化后，
 * 每次调用 listNext() 将返回链表的下一个元素。
 *
 * 此函数不会失败 */
listIter *listGetIterator(list *list, int direction)
{
    listIter *iter;

    if ((iter = zmalloc(sizeof(*iter), MALLOC_SHARED)) == NULL) return NULL;
    if (direction == AL_START_HEAD)
        iter->next = list->head;
    else
        iter->next = list->tail;
    iter->direction = direction;
    return iter;
}

/* 释放迭代器内存 */
void listReleaseIterator(listIter *iter) {
    zfree(iter);
}

/* 在链表私有迭代器结构中创建迭代器 */
void listRewind(list *list, listIter *li) {
    li->next = list->head;
    li->direction = AL_START_HEAD;
}

void listRewindTail(list *list, listIter *li) {
    li->next = list->tail;
    li->direction = AL_START_TAIL;
}

/* 返回迭代器的下一个元素。
 * 有效移除当前返回的元素使用 listDelNode()，
 * 但不能移除其他元素。
 *
 * 函数返回指向链表下一个元素的指针，
 * 若没有更多元素，则返回 NULL，经典用法模式是：
 *
 * iter = listGetIterator(list,<direction>);
 * while ((node = listNext(iter)) != NULL) {
 *     doSomethingWith(listNodeValue(node));
 * }
 *
 */
listNode *listNext(listIter *iter)
{
    listNode *current = iter->next;

    if (current != NULL) {
        if (iter->direction == AL_START_HEAD)
            iter->next = current->next;
        else
            iter->next = current->prev;
    }
    return current;
}

/* 复制整个链表。若内存不足则返回 NULL；
 * 成功时返回原始链表的副本。
 *
 * 若通过 listSetDupMethod() 设置了 'Dup' 方法，
 * 则使用该方法复制节点值；
 * 否则直接使用原始节点的指针值（浅拷贝）。
 *
 * 无论操作成功与否，原始链表均不会被修改 */
list *listDup(list *orig)
{
    list *copy;
    listIter iter;
    listNode *node;

    if ((copy = listCreate()) == NULL)
        return NULL;
    copy->dup = orig->dup;
    copy->free = orig->free;
    copy->match = orig->match;
    listRewind(orig, &iter);
    while((node = listNext(&iter)) != NULL) {
        void *value;

        if (copy->dup) {
            value = copy->dup(node->value);
            if (value == NULL) {
                listRelease(copy);
                return NULL;
            }
        } else
            value = node->value;
        if (listAddNodeTail(copy, value) == NULL) {
            listRelease(copy);
            return NULL;
        }
    }
    return copy;
}

/* 在链表中查找与给定 key 匹配的节点。
 * 匹配操作通过 listSetMatchMethod() 设置的 'match' 方法执行。
 * 若未设置 'match' 方法，则直接使用各节点的 'value' 指针与 'key' 指针进行比对。
 *
 * 成功时返回第一个匹配的节点指针（从头部开始搜索），
 * 若不存在匹配节点则返回 NULL */
listNode *listSearchKey(list *list, void *key)
{
    listIter iter;
    listNode *node;

    listRewind(list, &iter);
    while((node = listNext(&iter)) != NULL) {
        if (list->match) {
            if (list->match(node->value, key)) {
                return node;
            }
        } else {
            if (key == node->value) {
                return node;
            }
        }
    }
    return NULL;
}

/* 返回从零开始索引处的元素
 * 其中 0 是头部，1 是头部旁边的元素，
 * 依此类推。负整数用于从尾部计数，
 * -1 是最后一个元素，-2 是倒数第二个元素，
 * 依此类推。如果索引超出范围则返回 NULL */
listNode *listIndex(list *list, long index) {
    listNode *n;

    if (index < 0) {
        index = (-index)-1;
        n = list->tail;
        while(index-- && n) n = n->prev;
    } else {
        n = list->head;
        while(index-- && n) n = n->next;
    }
    return n;
}

/* 旋转链表：移除尾部节点并将其插入到头部 */
void listRotateTailToHead(list *list) {
    if (listLength(list) <= 1) return;

    /* 分离当前尾部 */
    listNode *tail = list->tail;
    list->tail = tail->prev;
    list->tail->next = NULL;
    /* 将其作为头部移动 */
    list->head->prev = tail;
    tail->prev = NULL;
    tail->next = list->head;
    list->head = tail;
}

/* 旋转链表：移除头部节点并将其插入到尾部 */
void listRotateHeadToTail(list *list) {
    if (listLength(list) <= 1) return;

    listNode *head = list->head;
    /* 分离当前头部 */
    list->head = head->next;
    list->head->prev = NULL;
    /* 将其作为尾部移动 */
    list->tail->next = head;
    head->next = NULL;
    head->prev = list->tail;
    list->tail = head;
}

/* 将链表 'o' 的所有元素添加到链表 'l' 的末尾。
 * 链表 'other' 变为空但保持有效状态 */
void listJoin(list *l, list *o) {
    if (o->len == 0) return;

    o->head->prev = l->tail;

    if (l->tail)
        l->tail->next = o->head;
    else
        l->head = o->head;

    l->tail = o->tail;
    l->len += o->len;

    /* 将 other 设置为空列表 */
    o->head = o->tail = NULL;
    o->len = 0;
}
