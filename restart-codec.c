/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Alexandre Fiori <fiorix@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"
#include "restart-codec-private.h"

/* Set the error, keeping the first one. */
void
restart_set_cause(char **cause, const char *fmt, ...)
{
	va_list ap;

	if (cause == NULL || *cause != NULL)
		return;
	va_start(ap, fmt);
	xvasprintf(cause, fmt, ap);
	va_end(ap);
}

/* Add sizes, failing on overflow. */
int
restart_size_add(size_t a, size_t b, size_t *out)
{
	if (b > SIZE_MAX - a)
		return (-1);
	*out = a + b;
	return (0);
}

/* Multiply sizes, failing on overflow. */
int
restart_size_mul(size_t a, size_t b, size_t *out)
{
	if (a != 0 && b > SIZE_MAX / a)
		return (-1);
	*out = a * b;
	return (0);
}

/*
 * Charge the decode budget. Everything the codec allocates while decoding
 * comes from a byte string it did not write, so the total is bounded here
 * rather than trusted to the sizes that string declares.
 */
int
restart_budget_charge(struct restart_budget *budget, size_t size, char **cause)
{
	size_t	total;

	if (restart_size_add(budget->allocation, size, &total) != 0 ||
	    total > RESTART_MAX_SIZE) {
		restart_set_cause(cause, "restart allocation limit exceeded");
		return (-1);
	}
	budget->allocation = total;
	return (0);
}

/* Allocate against the budget. */
void *
restart_alloc(struct restart_budget *budget, size_t size, char **cause)
{
	if (size == 0)
		size = 1;
	if (restart_budget_charge(budget, size, cause) != 0)
		return (NULL);
	return (xmalloc(size));
}

/* Allocate an array against the budget. */
void *
restart_calloc(struct restart_budget *budget, size_t nmemb, size_t size,
    char **cause)
{
	size_t	total;

	if (restart_size_mul(nmemb, size, &total) != 0) {
		restart_set_cause(cause, "restart allocation overflow");
		return (NULL);
	}
	if (restart_budget_charge(budget, total, cause) != 0)
		return (NULL);
	return (xcalloc(nmemb == 0 ? 1 : nmemb, size == 0 ? 1 : size));
}

/* Grow an array against the budget, charging only the delta. */
void *
restart_grow(struct restart_budget *budget, void *ptr, size_t oldnmemb,
    size_t nmemb, size_t size, char **cause)
{
	size_t	old, new;

	if (restart_size_mul(oldnmemb, size, &old) != 0 ||
	    restart_size_mul(nmemb, size, &new) != 0) {
		restart_set_cause(cause, "restart allocation overflow");
		return (NULL);
	}
	if (new > old && restart_budget_charge(budget, new - old, cause) != 0)
		return (NULL);
	return (xreallocarray(ptr, nmemb == 0 ? 1 : nmemb, size));
}

/* Get the features this build can encode. */
static uint64_t
restart_codec_features(void)
{
#ifdef ENABLE_SIXEL
	return (RESTART_FEATURE_SIXEL);
#else
	return (0);
#endif
}

/* Create a writer. */
struct restart_writer *
restart_writer_create(char **cause)
{
	struct restart_writer *rw;

	rw = calloc(1, sizeof *rw);
	if (rw == NULL) {
		restart_set_cause(cause, "out of memory");
		return (NULL);
	}
	rw->buf = ibuf_dynamic(0, RESTART_MAX_SIZE);
	if (rw->buf == NULL) {
		free(rw);
		restart_set_cause(cause, "out of memory");
		return (NULL);
	}
	return (rw);
}

/* Free a writer and anything it has written. */
void
restart_writer_discard(struct restart_writer *rw)
{
	if (rw == NULL)
		return;
	ibuf_free(rw->buf);
	free(rw);
}

/* Take the finished buffer from a writer. */
int
restart_writer_detach(struct restart_writer *rw, struct ibuf **out,
    char **cause)
{
	if (rw == NULL || rw->buf == NULL) {
		restart_set_cause(cause, "invalid restart writer");
		return (-1);
	}
	*out = rw->buf;
	rw->buf = NULL;
	free(rw);
	return (0);
}

/* Report a write failure. */
static int
restart_write_failed(char **cause)
{
	restart_set_cause(cause, "restart output exceeds format limit");
	return (-1);
}

/* Start the outer envelope. */
int
restart_write_envelope_begin(struct restart_writer *rw, uint16_t kind,
    char **cause)
{
	const char *producer = getversion();
	size_t producer_size = strlen(producer), header_size, offset;

	if (producer_size == 0 || producer_size > RESTART_MAX_PRODUCER ||
	    restart_size_add(RESTART_HEADER_SIZE, producer_size,
	    &header_size) != 0)
		return (restart_write_failed(cause));
	offset = ibuf_size(rw->buf);
	if (ibuf_add(rw->buf, RESTART_MAGIC, RESTART_MAGIC_SIZE) != 0 ||
	    ibuf_add_n16(rw->buf, RESTART_MAJOR) != 0 ||
	    ibuf_add_n16(rw->buf, RESTART_MINOR) != 0 ||
	    ibuf_add_n16(rw->buf, kind) != 0 ||
	    ibuf_add_n16(rw->buf, 0) != 0 ||
	    ibuf_add_n32(rw->buf, header_size) != 0 ||
	    ibuf_add_n32(rw->buf, 0) != 0 ||
	    ibuf_add_n64(rw->buf, 0) != 0 ||
	    ibuf_add_n64(rw->buf, 0) != 0 ||
	    ibuf_add_n32(rw->buf, producer_size) != 0 ||
	    ibuf_add_n32(rw->buf, 0) != 0 ||
	    ibuf_add(rw->buf, producer, producer_size) != 0)
		return (restart_write_failed(cause));
	rw->top.count_offset = offset + 20;
	rw->top.features_offset = offset + 24;
	rw->top.length_offset = offset + 32;
	rw->top.payload_offset = offset + header_size;
	rw->top.count = 0;
	return (0);
}

/* Finish the outer envelope and fix up its header. */
int
restart_write_envelope_end(struct restart_writer *rw, char **cause)
{
	size_t size = ibuf_size(rw->buf), payload_size;

	if (size < rw->top.payload_offset)
		return (restart_write_failed(cause));
	payload_size = size - rw->top.payload_offset;
	if (ibuf_set_n32(rw->buf, rw->top.count_offset, rw->top.count) != 0 ||
	    ibuf_set_n64(rw->buf, rw->top.features_offset, rw->features) != 0 ||
	    ibuf_set_n64(rw->buf, rw->top.length_offset, payload_size) != 0)
		return (restart_write_failed(cause));
	return (0);
}

/* Count a record against the writer budget. */
static int
restart_writer_charge_record(struct restart_writer *rw, char **cause)
{
	if (rw->budget.records == RESTART_MAX_RECORDS) {
		restart_set_cause(cause, "restart record limit exceeded");
		return (-1);
	}
	rw->budget.records++;
	return (0);
}

/* Write a record with a payload. */
int
restart_write_record(struct restart_writer *rw,
    struct restart_write_scope *scope, uint16_t type, uint16_t flags,
    const void *data, size_t size, char **cause)
{
	if (size > UINT32_MAX || scope->count == UINT32_MAX ||
	    restart_writer_charge_record(rw, cause) != 0 ||
	    ibuf_add_n16(rw->buf, type) != 0 ||
	    ibuf_add_n16(rw->buf, flags) != 0 ||
	    ibuf_add_n32(rw->buf, size) != 0 ||
	    ibuf_add(rw->buf, data, size) != 0)
		return (restart_write_failed(cause));
	scope->count++;
	return (0);
}

/* Start a record with a nested payload. */
int
restart_write_record_begin(struct restart_writer *rw,
    struct restart_write_scope *parent, uint16_t type, uint16_t flags,
    struct restart_write_scope *scope, char **cause)
{
	size_t offset = ibuf_size(rw->buf);

	if (parent->count == UINT32_MAX ||
	    restart_writer_charge_record(rw, cause) != 0 ||
	    ibuf_add_n16(rw->buf, type) != 0 ||
	    ibuf_add_n16(rw->buf, flags) != 0 ||
	    ibuf_add_n32(rw->buf, 0) != 0)
		return (restart_write_failed(cause));
	parent->count++;
	memset(scope, 0, sizeof *scope);
	scope->length_offset = offset + 4;
	scope->payload_offset = offset + 8;
	return (0);
}

/* Finish a record and fix up its length. */
int
restart_write_record_end(struct restart_writer *rw,
    struct restart_write_scope *scope, char **cause)
{
	size_t size = ibuf_size(rw->buf), payload_size;

	if (size < scope->payload_offset)
		return (restart_write_failed(cause));
	payload_size = size - scope->payload_offset;
	if (payload_size > UINT32_MAX || ibuf_set_n32(rw->buf,
	    scope->length_offset, payload_size) != 0)
		return (restart_write_failed(cause));
	return (0);
}

/* Start a record holding other records. */
int
restart_write_container_begin(struct restart_writer *rw,
    struct restart_write_scope *parent, uint16_t type, uint16_t flags,
    struct restart_write_scope *scope, char **cause)
{
	size_t offset = ibuf_size(rw->buf);

	if (parent->count == UINT32_MAX ||
	    restart_writer_charge_record(rw, cause) != 0 ||
	    ibuf_add_n16(rw->buf, type) != 0 ||
	    ibuf_add_n16(rw->buf, flags) != 0 ||
	    ibuf_add_n32(rw->buf, 0) != 0 ||
	    ibuf_add_n32(rw->buf, 0) != 0)
		return (restart_write_failed(cause));
	parent->count++;
	scope->length_offset = offset + 4;
	scope->payload_offset = offset + 8;
	scope->count_offset = offset + 8;
	scope->count = 0;
	return (0);
}

/* Finish a container and fix up its count. */
int
restart_write_container_end(struct restart_writer *rw,
    struct restart_write_scope *scope, char **cause)
{
	size_t size = ibuf_size(rw->buf), payload_size;

	if (size < scope->payload_offset)
		return (restart_write_failed(cause));
	payload_size = size - scope->payload_offset;
	if (payload_size > UINT32_MAX ||
	    ibuf_set_n32(rw->buf, scope->count_offset, scope->count) != 0 ||
	    ibuf_set_n32(rw->buf, scope->length_offset, payload_size) != 0)
		return (restart_write_failed(cause));
	return (0);
}

/* Write an integer record of the given width. */
static int
restart_write_integer(struct restart_writer *rw,
    struct restart_write_scope *scope, uint16_t type, uint64_t value,
    size_t size, char **cause)
{
	u_char data[8];
	size_t i;

	for (i = 0; i < size; i++) {
		data[size - i - 1] = value & 0xff;
		value >>= 8;
	}
	return (restart_write_record(rw, scope, type, RESTART_RECORD_REQUIRED,
	    data, size, cause));
}

/* Write an unsigned 8 bit record. */
int
restart_write_u8(struct restart_writer *rw, struct restart_write_scope *scope,
    uint16_t type, uint8_t value, char **cause)
{
	return (restart_write_integer(rw, scope, type, value, 1, cause));
}

/* Write an unsigned 16 bit record. */
int
restart_write_u16(struct restart_writer *rw,
    struct restart_write_scope *scope, uint16_t type, uint16_t value,
    char **cause)
{
	return (restart_write_integer(rw, scope, type, value, 2, cause));
}

/* Write an unsigned 32 bit record. */
int
restart_write_u32(struct restart_writer *rw,
    struct restart_write_scope *scope, uint16_t type, uint32_t value,
    char **cause)
{
	return (restart_write_integer(rw, scope, type, value, 4, cause));
}

/* Write an unsigned 64 bit record. */
int
restart_write_u64(struct restart_writer *rw,
    struct restart_write_scope *scope, uint16_t type, uint64_t value,
    char **cause)
{
	return (restart_write_integer(rw, scope, type, value, 8, cause));
}

/* Write a signed 32 bit record. */
int
restart_write_s32(struct restart_writer *rw,
    struct restart_write_scope *scope, uint16_t type, int32_t value,
    char **cause)
{
	uint32_t encoded;

	if (value < 0)
		encoded = ((uint32_t)(-(value + 1)) << 1) | 1;
	else
		encoded = (uint32_t)value << 1;
	return (restart_write_u32(rw, scope, type, encoded, cause));
}

/* Write a signed 64 bit record. */
int
restart_write_s64(struct restart_writer *rw,
    struct restart_write_scope *scope, uint16_t type, int64_t value,
    char **cause)
{
	uint64_t encoded;

	if (value < 0)
		encoded = ((uint64_t)(-(value + 1)) << 1) | 1;
	else
		encoded = (uint64_t)value << 1;
	return (restart_write_u64(rw, scope, type, encoded, cause));
}

/*
 * Write a string record. The caller names what it is writing so that an
 * over-size failure says which object was too large, which is the only part
 * of it a user can act on.
 */
int
restart_write_string(struct restart_writer *rw,
    struct restart_write_scope *scope, uint16_t type, const char *value,
    const char *what, char **cause)
{
	size_t size = strlen(value);

	if (size > RESTART_MAX_FIELD) {
		restart_set_cause(cause, "restart %s is too large: %zu bytes, "
		    "limit %zu", what, size, (size_t)RESTART_MAX_FIELD);
		return (-1);
	}
	return (restart_write_record(rw, scope, type, RESTART_RECORD_REQUIRED,
	    value, size, cause));
}

/* Write a byte record, named the same way as a string record. */
int
restart_write_blob(struct restart_writer *rw,
    struct restart_write_scope *scope, uint16_t type, const void *data,
    size_t size, const char *what, char **cause)
{
	if (size > RESTART_MAX_FIELD) {
		restart_set_cause(cause, "restart %s is too large: %zu bytes, "
		    "limit %zu", what, size, (size_t)RESTART_MAX_FIELD);
		return (-1);
	}
	return (restart_write_record(rw, scope, type, RESTART_RECORD_REQUIRED,
	    data, size, cause));
}

/* Write bytes as a container of chunks. */
int
restart_write_stream(struct restart_writer *rw,
    struct restart_write_scope *parent, uint16_t type, const void *data,
    size_t size, const char *what, char **cause)
{
	struct restart_write_scope scope;
	const u_char *cp = data;
	size_t left = size, chunk;

	if (restart_write_container_begin(rw, parent, type,
	    RESTART_RECORD_REQUIRED, &scope, cause) != 0 ||
	    restart_write_u64(rw, &scope, 1, size, cause) != 0)
		return (-1);
	while (left != 0) {
		chunk = left > RESTART_MAX_FIELD ? RESTART_MAX_FIELD : left;
		if (restart_write_blob(rw, &scope, 2, cp, chunk, what,
		    cause) != 0)
			return (-1);
		cp += chunk;
		left -= chunk;
	}
	return (restart_write_container_end(rw, &scope, cause));
}

/* Count a record against the reader budget. */
static int
restart_reader_charge_record(struct restart_reader *rr, char **cause)
{
	if (rr->budget->records == RESTART_MAX_RECORDS) {
		restart_set_cause(cause, "restart record limit exceeded");
		return (-1);
	}
	rr->budget->records++;
	return (0);
}

/* Open the outer envelope and check its header. */
int
restart_reader_open_envelope(struct restart_reader *rr, const void *data,
    size_t size, uint16_t kind, struct restart_budget *budget, char **cause)
{
	struct ibuf input, payload;
	u_char magic[RESTART_MAGIC_SIZE];
	uint16_t major, flags;
	uint32_t header_size, producer_size, reserved;
	uint64_t payload_size;
	const u_char *producer;
	size_t total;

	memset(rr, 0, sizeof *rr);
	if (budget == NULL)
		rr->budget = &rr->own_budget;
	else
		rr->budget = budget;
	if (size < RESTART_HEADER_SIZE || size > RESTART_MAX_SIZE) {
		restart_set_cause(cause, "invalid restart envelope size");
		return (-1);
	}
	ibuf_from_buffer(&input, (void *)data, size);
	if (ibuf_get(&input, magic, sizeof magic) != 0) {
		restart_set_cause(cause, "truncated restart envelope");
		return (-1);
	}
	if (memcmp(magic, RESTART_MAGIC, sizeof magic) != 0) {
		restart_set_cause(cause, "not a restart envelope");
		return (-1);
	}
	if (ibuf_get_n16(&input, &major) != 0 ||
	    ibuf_get_n16(&input, &rr->minor) != 0 ||
	    ibuf_get_n16(&input, &rr->kind) != 0 ||
	    ibuf_get_n16(&input, &flags) != 0 ||
	    ibuf_get_n32(&input, &header_size) != 0 ||
	    ibuf_get_n32(&input, &rr->records_left) != 0 ||
	    ibuf_get_n64(&input, &rr->required_features) != 0 ||
	    ibuf_get_n64(&input, &payload_size) != 0 ||
	    ibuf_get_n32(&input, &producer_size) != 0 ||
	    ibuf_get_n32(&input, &reserved) != 0) {
		restart_set_cause(cause, "truncated restart envelope");
		return (-1);
	}
	if (major != RESTART_MAJOR || rr->kind != kind || flags != 0 ||
	    reserved != 0 || producer_size == 0 ||
	    producer_size > RESTART_MAX_PRODUCER ||
	    header_size != RESTART_HEADER_SIZE + producer_size ||
	    rr->records_left > RESTART_MAX_RECORDS ||
	    (rr->required_features & ~restart_codec_features()) != 0 ||
	    payload_size > RESTART_MAX_SIZE ||
	    restart_size_add(header_size, payload_size, &total) != 0 ||
	    total != size || ibuf_size(&input) < producer_size) {
		restart_set_cause(cause, "invalid restart envelope");
		return (-1);
	}
	producer = ibuf_data(&input);
	if (memchr(producer, '\0', producer_size) != NULL ||
	    ibuf_skip(&input, producer_size) != 0 ||
	    ibuf_get_ibuf(&input, payload_size, &payload) != 0 ||
	    ibuf_size(&input) != 0) {
		restart_set_cause(cause, "invalid restart producer or payload");
		return (-1);
	}
	rr->buf = payload;
	return (0);
}

/* Get the next record. */
int
restart_reader_next(struct restart_reader *rr, struct restart_record *record,
    char **cause)
{
	uint32_t size;

	if (rr->records_left == 0)
		return (0);
	if (restart_reader_charge_record(rr, cause) != 0 ||
	    ibuf_get_n16(&rr->buf, &record->type) != 0 ||
	    ibuf_get_n16(&rr->buf, &record->flags) != 0 ||
	    ibuf_get_n32(&rr->buf, &size) != 0 ||
	    ibuf_get_ibuf(&rr->buf, size, &record->payload) != 0) {
		restart_set_cause(cause, "truncated restart record");
		return (-1);
	}
	if ((record->flags & ~RESTART_RECORD_REQUIRED) != 0) {
		restart_set_cause(cause, "unknown restart record flags");
		return (-1);
	}
	rr->records_left--;
	return (1);
}

/* Check the envelope has no records left. */
int
restart_reader_finish_envelope(struct restart_reader *rr, char **cause)
{
	if (rr->records_left != 0 || ibuf_size(&rr->buf) != 0) {
		restart_set_cause(cause, "restart envelope count mismatch");
		return (-1);
	}
	return (0);
}

/* Open a record holding other records. */
int
restart_reader_open_container(struct restart_record *record,
    struct restart_reader *rr, struct restart_budget *budget, char **cause)
{
	memset(rr, 0, sizeof *rr);
	rr->budget = budget;
	if (ibuf_get_n32(&record->payload, &rr->records_left) != 0 ||
	    rr->records_left > RESTART_MAX_RECORDS) {
		restart_set_cause(cause, "invalid restart container count");
		return (-1);
	}
	rr->buf = record->payload;
	return (0);
}

/* Check a container has no records left. */
int
restart_reader_finish_container(struct restart_reader *rr, char **cause)
{
	return (restart_reader_finish_envelope(rr, cause));
}

/* Read an integer record of the given width. */
static int
restart_read_integer(struct restart_record *record, uint64_t *value,
    size_t size, char **cause)
{
	int error = -1;

	if (ibuf_size(&record->payload) != size)
		goto fail;
	if (size == 1) {
		uint8_t v;
		error = ibuf_get_n8(&record->payload, &v);
		*value = v;
	} else if (size == 2) {
		uint16_t v;
		error = ibuf_get_n16(&record->payload, &v);
		*value = v;
	} else if (size == 4) {
		uint32_t v;
		error = ibuf_get_n32(&record->payload, &v);
		*value = v;
	} else if (size == 8)
		error = ibuf_get_n64(&record->payload, value);
	if (error == 0 && ibuf_size(&record->payload) == 0)
		return (0);
fail:
	restart_set_cause(cause, "invalid restart integer");
	return (-1);
}

/* Read an unsigned 8 bit record. */
int
restart_read_u8(struct restart_record *record, uint8_t *value, char **cause)
{
	uint64_t v;

	if (restart_read_integer(record, &v, 1, cause) != 0)
		return (-1);
	*value = v;
	return (0);
}

/* Read an unsigned 16 bit record. */
int
restart_read_u16(struct restart_record *record, uint16_t *value, char **cause)
{
	uint64_t v;

	if (restart_read_integer(record, &v, 2, cause) != 0)
		return (-1);
	*value = v;
	return (0);
}

/* Read an unsigned 32 bit record. */
int
restart_read_u32(struct restart_record *record, uint32_t *value, char **cause)
{
	uint64_t v;

	if (restart_read_integer(record, &v, 4, cause) != 0)
		return (-1);
	*value = v;
	return (0);
}

/* Read an unsigned 64 bit record. */
int
restart_read_u64(struct restart_record *record, uint64_t *value, char **cause)
{
	return (restart_read_integer(record, value, 8, cause));
}

/* Read a signed 32 bit record. */
int
restart_read_s32(struct restart_record *record, int32_t *value, char **cause)
{
	uint32_t encoded, magnitude;

	if (restart_read_u32(record, &encoded, cause) != 0)
		return (-1);
	magnitude = encoded >> 1;
	if (encoded & 1) {
		if (magnitude > INT32_MAX) {
			restart_set_cause(cause,
			    "restart signed integer overflow");
			return (-1);
		}
		*value = -(int32_t)magnitude - 1;
	} else {
		if (magnitude > INT32_MAX) {
			restart_set_cause(cause,
			    "restart signed integer overflow");
			return (-1);
		}
		*value = magnitude;
	}
	return (0);
}

/* Read a signed 64 bit record. */
int
restart_read_s64(struct restart_record *record, int64_t *value, char **cause)
{
	uint64_t encoded, magnitude;

	if (restart_read_u64(record, &encoded, cause) != 0)
		return (-1);
	magnitude = encoded >> 1;
	if (encoded & 1)
		*value = -(int64_t)magnitude - 1;
	else {
		if (magnitude > INT64_MAX) {
			restart_set_cause(cause,
			    "restart signed integer overflow");
			return (-1);
		}
		*value = magnitude;
	}
	return (0);
}

/* Read a string record. */
int
restart_read_string(struct restart_record *record, char **out,
    struct restart_budget *budget, char **cause)
{
	size_t size = ibuf_size(&record->payload);
	char *value;

	*out = NULL;
	if (size > RESTART_MAX_FIELD ||
	    memchr(ibuf_data(&record->payload), '\0', size) != NULL) {
		restart_set_cause(cause, "invalid restart string");
		return (-1);
	}
	value = restart_alloc(budget, size + 1, cause);
	if (value == NULL)
		return (-1);
	memcpy(value, ibuf_data(&record->payload), size);
	value[size] = '\0';
	if (ibuf_skip(&record->payload, size) != 0) {
		free(value);
		return (-1);
	}
	*out = value;
	return (0);
}

/* Read a byte record. */
int
restart_read_blob(struct restart_record *record, struct restart_bytes *out,
    size_t maximum, struct restart_budget *budget, char **cause)
{
	size_t size = ibuf_size(&record->payload);

	memset(out, 0, sizeof *out);
	if (size > maximum) {
		restart_set_cause(cause, "restart blob is too large");
		return (-1);
	}
	if (size != 0) {
		out->data = restart_alloc(budget, size, cause);
		if (out->data == NULL)
			return (-1);
		memcpy(out->data, ibuf_data(&record->payload), size);
	}
	out->size = size;
	if (ibuf_skip(&record->payload, size) != 0)
		return (-1);
	return (0);
}

/* Read a container of chunks as bytes. */
int
restart_read_stream(struct restart_record *record, struct restart_bytes *out,
    struct restart_budget *budget, char **cause)
{
	struct restart_reader rr;
	struct restart_record child;
	uint64_t total = 0;
	size_t used = 0, old, chunk, previous = 0;
	uint32_t seen = 0;
	void *new;
	int found;

	memset(out, 0, sizeof *out);
	if (restart_reader_open_container(record, &rr, budget, cause) != 0)
		return (-1);
	while ((found = restart_reader_next(&rr, &child, cause)) == 1) {
		if (child.type != 1 && child.type != 2) {
			if (child.flags & RESTART_RECORD_REQUIRED)
				goto unknown;
			continue;
		}
		if (child.flags != RESTART_RECORD_REQUIRED)
			goto invalid;
		if (child.type == 1) {
			if (seen & 1 ||
			    restart_read_u64(&child, &total, cause) != 0)
				goto fail;
			seen |= 1;
		} else {
			chunk = ibuf_size(&child.payload);
			if (chunk == 0 || chunk > RESTART_MAX_FIELD ||
			    (previous != 0 && previous != RESTART_MAX_FIELD) ||
			    restart_size_add(used, chunk, &old) != 0 ||
			    old > RESTART_MAX_SIZE)
				goto invalid;
			new = restart_grow(budget, out->data, used, old, 1,
			    cause);
			if (new == NULL)
				goto fail;
			out->data = new;
			memcpy(out->data + used, ibuf_data(&child.payload),
			    chunk);
			used = old;
			previous = chunk;
			if (ibuf_skip(&child.payload, chunk) != 0)
				goto invalid;
		}
	}
	if (found == -1)
		goto fail;
	if (restart_reader_finish_container(&rr, cause) != 0)
		goto fail;
	if (seen != 1 || total != used || (total == 0 && previous != 0))
		goto invalid;
	out->size = used;
	return (0);

unknown:
	restart_set_cause(cause, "unknown required restart stream record");
	goto fail;
invalid:
	restart_set_cause(cause, "invalid restart stream");
fail:
	free(out->data);
	memset(out, 0, sizeof *out);
	return (-1);
}

/* Encode the state of a pane's terminal. */
int
restart_terminal_encode(const struct window_pane *wp, struct ibuf **out,
    char **cause)
{
	struct restart_writer *rw;

	*out = NULL;
	if (cause != NULL)
		*cause = NULL;
	rw = restart_writer_create(cause);
	if (rw == NULL)
		return (-1);
	if (restart_terminal_write_nested(rw, wp, cause) != 0 ||
	    restart_writer_detach(rw, out, cause) != 0) {
		restart_writer_discard(rw);
		return (-1);
	}
	return (0);
}

/* Decode the state of a pane's terminal. */
int
restart_terminal_decode(const void *data, size_t size,
    struct restart_terminal **out, char **cause)
{
	struct restart_reader rr;

	*out = NULL;
	if (cause != NULL)
		*cause = NULL;
	if (restart_reader_open_envelope(&rr, data, size,
	    RESTART_KIND_TERMINAL, NULL, cause) != 0)
		return (-1);
	if (restart_terminal_read_nested(&rr, out, cause) != 0 ||
	    restart_reader_finish_envelope(&rr, cause) != 0) {
		restart_terminal_free(*out);
		*out = NULL;
		return (-1);
	}
	if ((*out)->features != rr.required_features) {
		restart_set_cause(cause, "restart terminal feature mismatch");
		restart_terminal_free(*out);
		*out = NULL;
		return (-1);
	}
	if (restart_terminal_validate_nested(*out, input_get_buffer_size(),
	    cause) != 0) {
		restart_terminal_free(*out);
		*out = NULL;
		return (-1);
	}
	return (0);
}
