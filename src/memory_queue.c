#include "memory_queue.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

void mq_init(mq_queue *queue, size_t limit)
{
    if (!queue) return;
    memset(queue, 0, sizeof(*queue));
    queue->limit = limit;
}

mq_result mq_push(mq_queue *queue, mq_metadata meta,
                  const char *json, size_t length)
{
    mq_record *record;
    size_t bytes;
    if (!queue || !json || !length || length > SIZE_MAX - sizeof(*record) - 1)
        return MQ_INVALID;
    bytes = sizeof(*record) + length + 1;
    if (queue->count == UINT_MAX || queue->bytes > queue->limit ||
        bytes > queue->limit - queue->bytes)
        return MQ_FULL;
    record = malloc(bytes);
    if (!record) return MQ_ALLOC_FAILED;
    record->next = NULL;
    record->meta = meta;
    record->length = length;
    memcpy(record->json, json, length);
    record->json[length] = '\0';
    if (queue->tail) queue->tail->next = record;
    else queue->head = record;
    queue->tail = record;
    queue->bytes += bytes;
    if (queue->bytes > queue->peak) queue->peak = queue->bytes;
    ++queue->count;
    return MQ_OK;
}

void mq_release(mq_queue *queue, unsigned count)
{
    if (!queue) return;
    while (count-- && queue->head) {
        mq_record *record = queue->head;
        queue->head = record->next;
        queue->bytes -= sizeof(*record) + record->length + 1;
        --queue->count;
        free(record);
    }
    if (!queue->head) queue->tail = NULL;
}

void mq_clear(mq_queue *queue)
{
    if (queue) mq_release(queue, queue->count);
}
