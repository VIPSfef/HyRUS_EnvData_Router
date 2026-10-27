#ifndef MEMORY_QUEUE_H
#define MEMORY_QUEUE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t queued_ms;
    unsigned kind, samples;
} mq_metadata;

typedef struct mq_record {
    struct mq_record *next;
    mq_metadata meta;
    size_t length;
    char json[];
} mq_record;

typedef struct {
    mq_record *head, *tail;
    size_t bytes, limit, peak;
    unsigned count;
} mq_queue;

typedef enum {
    MQ_OK,
    MQ_FULL,
    MQ_ALLOC_FAILED,
    MQ_INVALID
} mq_result;

/* Caller owns synchronization. Initialize an empty/uninitialized queue only;
 * clear an existing queue before reinitializing it. Initialization allocates
 * nothing. bytes/peak account for each allocation's requested size, including
 * its record header and NUL terminator, but not allocator-private overhead. */
void mq_init(mq_queue *queue, size_t limit);
/* Copy complete, nonempty JSON bytes and metadata. The caller validates JSON.
 * Stored bytes stay immutable until release. A full budget rejects the new
 * record and preserves every queued record; failure never changes the queue. */
mq_result mq_push(mq_queue *queue, mq_metadata meta,
                  const char *json, size_t length);
/* Release an acknowledged prefix, or all records if count exceeds the queue.
 * clear also empties the queue. Both preserve the budget and lifetime peak. */
void mq_release(mq_queue *queue, unsigned count);
void mq_clear(mq_queue *queue);

#endif
