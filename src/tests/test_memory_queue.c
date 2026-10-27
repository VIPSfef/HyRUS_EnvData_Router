#include "memory_queue.h"

#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(test) do { ++checks; if (!(test)) { \
    fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#test); return 0; \
} } while (0)

static int exact_budget_and_prefix_release(void)
{
    mq_queue queue;
    mq_record *second;
    mq_metadata first_meta = {1234, 2, 7}, second_meta = {5678, 3, 9};
    char source[] = "{\"id\":1}";
    const char replacement[] = "{\"id\":2}";
    size_t length = sizeof(source) - 1;
    size_t record_bytes = sizeof(mq_record) + length + 1;
    mq_init(&queue, record_bytes * 2);
    CHECK(queue.head == NULL && queue.tail == NULL && !queue.bytes && !queue.count && !queue.peak);
    CHECK(mq_push(&queue, first_meta, source, length) == MQ_OK);
    CHECK(queue.bytes == record_bytes && queue.peak == record_bytes && queue.count == 1);
    CHECK(queue.head == queue.tail && queue.head->next == NULL);
    CHECK(queue.head->length == length && !strcmp(queue.head->json, source));
    CHECK(queue.head->meta.queued_ms == 1234 && queue.head->meta.kind == 2 && queue.head->meta.samples == 7);
    source[6] = '9';
    CHECK(!strcmp(queue.head->json, "{\"id\":1}"));
    CHECK(mq_push(&queue, second_meta, replacement, length) == MQ_OK);
    second = queue.tail;
    CHECK(queue.bytes == queue.limit && queue.count == 2 && queue.peak == queue.limit);
    CHECK(queue.head->next == second && second->next == NULL);
    CHECK(mq_push(&queue, first_meta, source, length) == MQ_FULL);
    CHECK(queue.count == 2 && queue.tail == second && queue.bytes == queue.limit);
    CHECK(!strcmp(queue.head->json, "{\"id\":1}") && !strcmp(second->json, replacement));
    mq_release(&queue, 0);
    CHECK(queue.count == 2 && queue.head->next == second);
    mq_release(&queue, 1);
    CHECK(queue.head == second && queue.tail == second && queue.count == 1);
    CHECK(queue.bytes == record_bytes && queue.peak == queue.limit);
    CHECK(second->meta.queued_ms == 5678 && second->meta.kind == 3 && second->meta.samples == 9);
    CHECK(mq_push(&queue, first_meta, source, length) == MQ_OK);
    CHECK(queue.head == second && !strcmp(queue.tail->json, source) && queue.count == 2);
    mq_release(&queue, 3);
    CHECK(queue.head == NULL && queue.tail == NULL && !queue.bytes && !queue.count);
    CHECK(queue.limit == record_bytes * 2 && queue.peak == queue.limit);
    CHECK(mq_push(&queue, first_meta, source, length) == MQ_OK);
    CHECK(queue.head == queue.tail && queue.count == 1);
    mq_clear(&queue);
    CHECK(queue.head == NULL && queue.tail == NULL && !queue.bytes && !queue.count);
    CHECK(queue.limit == record_bytes * 2 && queue.peak == queue.limit);
    mq_clear(&queue);
    mq_release(&queue, 1);
    CHECK(queue.bytes == 0 && queue.count == 0);
    return 1;
}

static int boundaries_and_invalid_input(void)
{
    mq_queue queue;
    mq_metadata meta = {0};
    const char json[] = "{}";
    size_t exact = sizeof(mq_record) + sizeof(json);
    mq_init(NULL, 0);
    mq_clear(NULL);
    mq_release(NULL, 1);
    CHECK(mq_push(NULL, meta, json, 2) == MQ_INVALID);
    mq_init(&queue, exact - 1);
    CHECK(mq_push(&queue, meta, json, 2) == MQ_FULL);
    CHECK(!queue.bytes && !queue.count && !queue.head && !queue.tail && !queue.peak);
    mq_init(&queue, 0);
    CHECK(mq_push(&queue, meta, json, 2) == MQ_FULL);
    mq_init(&queue, SIZE_MAX);
    CHECK(mq_push(&queue, meta, NULL, 2) == MQ_INVALID);
    CHECK(mq_push(&queue, meta, json, 0) == MQ_INVALID);
    CHECK(mq_push(&queue, meta, json, SIZE_MAX) == MQ_INVALID);
    CHECK(mq_push(&queue, meta, json, SIZE_MAX - sizeof(mq_record)) == MQ_INVALID);
    CHECK(!queue.bytes && !queue.count && !queue.head && !queue.tail && !queue.peak);
    CHECK(mq_push(&queue, meta, json, 2) == MQ_OK);
    CHECK(queue.bytes == exact && queue.limit == SIZE_MAX);
    mq_clear(&queue);
    CHECK(!queue.bytes && !queue.count && queue.peak == exact);
    return 1;
}

static int many_records_and_fifo_order(void)
{
    enum { RECORDS = 512 };
    mq_queue queue;
    mq_metadata meta = {0};
    unsigned i;
    const mq_record *record;
    const char json[] = "{}";
    mq_init(&queue, RECORDS * (sizeof(mq_record) + sizeof(json)));
    for (i = 0; i < RECORDS; ++i) {
        meta.queued_ms = i;
        CHECK(mq_push(&queue, meta, json, sizeof(json)-1) == MQ_OK);
    }
    CHECK(queue.count == RECORDS && queue.bytes == queue.limit);
    for (i = 0, record = queue.head; record; record = record->next, ++i)
        CHECK(record->meta.queued_ms == i && !strcmp(record->json, json));
    CHECK(i == RECORDS);
    mq_release(&queue, RECORDS / 2);
    CHECK(queue.count == RECORDS / 2 && queue.head->meta.queued_ms == RECORDS / 2);
    CHECK(queue.bytes == queue.limit / 2);
    mq_clear(&queue);
    CHECK(!queue.count && !queue.bytes && !queue.head && !queue.tail);
    return 1;
}

int main(void)
{
    if (!exact_budget_and_prefix_release() || !boundaries_and_invalid_input() ||
        !many_records_and_fifo_order()) return 1;
    printf("PASS %u memory queue checks\n", checks);
    return 0;
}
