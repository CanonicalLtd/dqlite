#include <sqlite3.h>

#include "../../src/tuple.h"

#include "../lib/runner.h"

TEST_MODULE(tuple);

/******************************************************************************
 *
 * Helper macros.
 *
 ******************************************************************************/

#define DECODER_INIT(N)                                       \
	{                                                     \
		int rc2;                                      \
		rc2 = tupleDecoderInit(&decoder, N, &cursor); \
		munit_assert_int(rc2, ==, 0);                 \
	}

#define DECODER_NEXT                                      \
	{                                                 \
		int rc2;                                  \
		rc2 = tupleDecoderNext(&decoder, &value); \
		munit_assert_int(rc2, ==, 0);             \
	}

#define ENCODER_INIT(N, FORMAT)                                             \
	{                                                                   \
		int rc2;                                                    \
		rc2 = tupleEncoderInit(&f->encoder, N, FORMAT, &f->buffer); \
		munit_assert_int(rc2, ==, 0);                               \
	}

#define ENCODER_NEXT                                         \
	{                                                    \
		int rc2;                                     \
		rc2 = tupleEncoderNext(&f->encoder, &value); \
		munit_assert_int(rc2, ==, 0);                \
	}

/******************************************************************************
 *
 * Assertions.
 *
 ******************************************************************************/

#define ASSERT_VALUE_TYPE(TYPE) munit_assert_int(value.type, ==, TYPE)

/******************************************************************************
 *
 * Decoder.
 *
 ******************************************************************************/

TEST_SUITE(decoder);

TEST_GROUP(decoder, init);

/* If n is 0, then the parameters format is used to dermine the number of
 * elements of the tuple. */
TEST_CASE(decoder, init, param, NULL)
{
	struct tupleDecoder decoder;
	char buf[] = {2, 0, 0, 0, 0, 0, 0, 0, 0};
	struct cursor cursor = {buf, sizeof buf};
	(void)data;
	(void)params;
	DECODER_INIT(0);
	munit_assert_int(decoder.n, ==, 2);
	munit_assert_int(tupleDecoderN(&decoder), ==, 2);
	return MUNIT_OK;
}

/* If n is not 0, then it is the number of elements. */
TEST_CASE(decoder, init, row, NULL)
{
	struct tupleDecoder decoder;
	char buf[] = {2, 0, 0, 0, 0, 0, 0, 0, 0};
	struct cursor cursor = {buf, sizeof buf};
	(void)data;
	(void)params;
	DECODER_INIT(3);
	munit_assert_int(decoder.n, ==, 3);
	munit_assert_int(tupleDecoderN(&decoder), ==, 3);
	return MUNIT_OK;
}

TEST_GROUP(decoder, row);

/* Decode a tuple with row format and only one value. */
TEST_CASE(decoder, row, oneValue, NULL)
{
	struct tupleDecoder decoder;
	uint8_t buf[][8] = {
	    {SQLITE_INTEGER, 0, 0, 0, 0, 0, 0, 0},
	    {7, 0, 0, 0, 0, 0, 0, 0},
	};
	struct cursor cursor = {buf, sizeof buf};
	struct value value;

	(void)data;
	(void)params;

	DECODER_INIT(1);
	DECODER_NEXT;

	ASSERT_VALUE_TYPE(SQLITE_INTEGER);
	munit_assert_int(value.integer, ==, 7);

	return MUNIT_OK;
}

/* Decode a tuple with row format and two values. */
TEST_CASE(decoder, row, twoValues, NULL)
{
	struct tupleDecoder decoder;
	uint8_t buf[][8] = {
	    {SQLITE_INTEGER | SQLITE_TEXT << 4, 0, 0, 0, 0, 0, 0, 0},
	    {7, 0, 0, 0, 0, 0, 0, 0},
	    {'h', 'e', 'l', 'l', 'o', 0, 0, 0},
	};
	struct cursor cursor = {buf, sizeof buf};
	struct value value;

	(void)data;
	(void)params;

	DECODER_INIT(2);
	DECODER_NEXT;

	ASSERT_VALUE_TYPE(SQLITE_INTEGER);
	munit_assert_int(value.integer, ==, 7);

	DECODER_NEXT;

	ASSERT_VALUE_TYPE(SQLITE_TEXT);
	munit_assert_string_equal(value.text, "hello");

	return MUNIT_OK;
}

TEST_GROUP(decoder, params);

/* Decode a tuple with params format and only one value. */
TEST_CASE(decoder, params, oneValue, NULL)
{
	struct tupleDecoder decoder;
	uint8_t buf[][8] = {
	    {1, SQLITE_INTEGER, 0, 0, 0, 0, 0, 0},
	    {7, 0, 0, 0, 0, 0, 0, 0},
	};
	struct cursor cursor = {buf, sizeof buf};
	struct value value;

	(void)data;
	(void)params;

	DECODER_INIT(0);
	DECODER_NEXT;

	ASSERT_VALUE_TYPE(SQLITE_INTEGER);
	munit_assert_int(value.integer, ==, 7);

	return MUNIT_OK;
}

/* Decode a tuple with params format and two values. */
TEST_CASE(decoder, params, twoValues, NULL)
{
	struct tupleDecoder decoder;
	uint8_t buf[][8] = {
	    {2, SQLITE_INTEGER, SQLITE_TEXT, 0, 0, 0, 0, 0},
	    {7, 0, 0, 0, 0, 0, 0, 0},
	    {'h', 'e', 'l', 'l', 'o', 0, 0, 0},
	};
	struct cursor cursor = {buf, sizeof buf};
	struct value value;

	(void)data;
	(void)params;

	DECODER_INIT(0);
	DECODER_NEXT;

	ASSERT_VALUE_TYPE(SQLITE_INTEGER);
	munit_assert_int(value.integer, ==, 7);

	DECODER_NEXT;

	ASSERT_VALUE_TYPE(SQLITE_TEXT);
	munit_assert_string_equal(value.text, "hello");

	return MUNIT_OK;
}

TEST_GROUP(decoder, type);

/* Decode a floating point number. */
TEST_CASE(decoder, type, float, NULL)
{
	struct tupleDecoder decoder;
	uint8_t buf[][8] = {
	    {SQLITE_FLOAT, 0, 0, 0, 0, 0, 0, 0},
	    {0, 0, 0, 0, 0, 0, 0, 0},
	};
	struct cursor cursor = {buf, sizeof buf};
	struct value value;
	double pi = 3.1415;

	(void)data;
	(void)params;

	memcpy(buf[1], &pi, sizeof pi);
	*(uint64_t *)buf[1] = byteFlip64(*(uint64_t *)buf[1]);

	DECODER_INIT(1);
	DECODER_NEXT;

	ASSERT_VALUE_TYPE(SQLITE_FLOAT);
	munit_assert_double(value.float_, ==, 3.1415);

	return MUNIT_OK;
}

/* Decode a null value. */
TEST_CASE(decoder, type, null, NULL)
{
	struct tupleDecoder decoder;
	uint8_t buf[][8] = {
	    {SQLITE_NULL, 0, 0, 0, 0, 0, 0, 0},
	    {0, 0, 0, 0, 0, 0, 0, 0},
	};
	struct cursor cursor = {buf, sizeof buf};
	struct value value;

	(void)data;
	(void)params;

	DECODER_INIT(1);
	DECODER_NEXT;

	ASSERT_VALUE_TYPE(SQLITE_NULL);

	return MUNIT_OK;
}

/* Decode a date string in ISO8601 format. */
TEST_CASE(decoder, type, iso8601, NULL)
{
	struct tupleDecoder decoder;
	uint8_t buf[5][8] = {
	    {DQLITE_ISO8601, 0, 0, 0, 0, 0, 0, 0},
	};
	struct cursor cursor = {buf, sizeof buf};
	struct value value;

	(void)data;
	(void)params;

	strcpy((char *)buf[1], "2018-07-20 09:49:05+00:00");

	DECODER_INIT(1);
	DECODER_NEXT;

	ASSERT_VALUE_TYPE(DQLITE_ISO8601);
	munit_assert_string_equal(value.iso8601, "2018-07-20 09:49:05+00:00");

	return MUNIT_OK;
}

/* Decode a boolean. */
TEST_CASE(decoder, type, boolean, NULL)
{
	struct tupleDecoder decoder;
	uint8_t buf[][8] = {
	    {DQLITE_BOOLEAN, 0, 0, 0, 0, 0, 0, 0},
	    {1, 0, 0, 0, 0, 0, 0, 0},
	};
	struct cursor cursor = {buf, sizeof buf};
	struct value value;

	(void)data;
	(void)params;

	DECODER_INIT(1);
	DECODER_NEXT;

	ASSERT_VALUE_TYPE(DQLITE_BOOLEAN);
	munit_assert_int(value.boolean, ==, 1);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * Encoder.
 *
 ******************************************************************************/

struct encoderFixture
{
	struct buffer buffer;
	struct tupleEncoder encoder;
};

TEST_SUITE(encoder);
TEST_SETUP(encoder)
{
	struct encoderFixture *f = munit_malloc(sizeof *f);
	int rc;
	(void)params;
	(void)userData;
	rc = bufferInit(&f->buffer);
	munit_assert_int(rc, ==, 0);
	return f;
}
TEST_TEAR_DOWN(encoder)
{
	struct encoderFixture *f = data;
	bufferClose(&f->buffer);
	free(data);
}

TEST_GROUP(encoder, row);

/* Encode a tuple with row format and only one value. */
TEST_CASE(encoder, row, oneValue, NULL)
{
	struct encoderFixture *f = data;
	struct value value;
	uint8_t(*buf)[8] = f->buffer.data;
	(void)params;

	ENCODER_INIT(1, TUPLE_ROW);

	value.type = SQLITE_INTEGER;
	value.integer = 7;
	ENCODER_NEXT;

	munit_assert_int(buf[0][0], ==, SQLITE_INTEGER);
	munit_assert_int(*(uint64_t *)buf[1], ==, byteFlip64(7));

	return MUNIT_OK;
}

/* Encode a tuple with row format and two values. */
TEST_CASE(encoder, row, twoValues, NULL)
{
	struct encoderFixture *f = data;
	struct value value;
	uint8_t(*buf)[8] = f->buffer.data;
	(void)params;

	ENCODER_INIT(2, TUPLE_ROW);

	value.type = SQLITE_INTEGER;
	value.integer = 7;
	ENCODER_NEXT;

	value.type = SQLITE_TEXT;
	value.text = "hello";
	ENCODER_NEXT;

	munit_assert_int(buf[0][0], ==, SQLITE_INTEGER | SQLITE_TEXT << 4);
	munit_assert_int(*(uint64_t *)buf[1], ==, byteFlip64(7));
	munit_assert_string_equal((const char *)buf[2], "hello");

	return MUNIT_OK;
}

TEST_GROUP(encoder, params);

/* Encode a tuple with params format and only one value. */
TEST_CASE(encoder, params, oneValue, NULL)
{
	struct encoderFixture *f = data;
	struct value value;
	uint8_t(*buf)[8] = f->buffer.data;
	(void)params;

	ENCODER_INIT(1, TUPLE_PARAMS);

	value.type = SQLITE_INTEGER;
	value.integer = 7;
	ENCODER_NEXT;

	munit_assert_int(buf[0][0], ==, 1);
	munit_assert_int(buf[0][1], ==, SQLITE_INTEGER);
	munit_assert_int(*(uint64_t *)buf[1], ==, byteFlip64(7));

	return MUNIT_OK;
}

/* Encode a tuple with params format and two values. */
TEST_CASE(encoder, params, twoValues, NULL)
{
	struct encoderFixture *f = data;
	struct value value;
	uint8_t(*buf)[8] = f->buffer.data;
	(void)params;

	ENCODER_INIT(2, TUPLE_PARAMS);

	value.type = SQLITE_INTEGER;
	value.integer = 7;
	ENCODER_NEXT;

	value.type = SQLITE_TEXT;
	value.text = "hello";
	ENCODER_NEXT;

	munit_assert_int(buf[0][0], ==, 2);
	munit_assert_int(buf[0][1], ==, SQLITE_INTEGER);
	munit_assert_int(buf[0][2], ==, SQLITE_TEXT);
	munit_assert_int(*(uint64_t *)buf[1], ==, byteFlip64(7));
	munit_assert_string_equal((const char *)buf[2], "hello");

	return MUNIT_OK;
}

TEST_GROUP(encoder, type);

/* Encode a float parameter. */
TEST_CASE(encoder, type, float, NULL)
{
	struct encoderFixture *f = data;
	struct value value;
	uint8_t(*buf)[8] = f->buffer.data;
	(void)params;

	ENCODER_INIT(1, TUPLE_ROW);

	value.type = SQLITE_FLOAT;
	value.float_ = 3.1415;
	ENCODER_NEXT;

	munit_assert_int(buf[0][0], ==, SQLITE_FLOAT);
	munit_assert_int(*(uint64_t *)buf[1], ==,
			 byteFlip64(*(uint64_t *)&value.float_));

	return MUNIT_OK;
}

/* Encode a unix time parameter. */
TEST_CASE(encoder, type, unixtime, NULL)
{
	struct encoderFixture *f = data;
	struct value value;
	uint8_t(*buf)[8] = f->buffer.data;
	(void)params;

	ENCODER_INIT(1, TUPLE_ROW);

	value.type = DQLITE_UNIXTIME;
	value.unixtime = 12345;
	ENCODER_NEXT;

	munit_assert_int(buf[0][0], ==, DQLITE_UNIXTIME);
	munit_assert_int(*(int64_t *)buf[1], ==, byteFlip64(value.unixtime));

	return MUNIT_OK;
}

/* Encode an ISO8601 date string time parameter. */
TEST_CASE(encoder, type, iso8601, NULL)
{
	struct encoderFixture *f = data;
	struct value value;
	uint8_t(*buf)[8] = f->buffer.data;
	(void)params;

	ENCODER_INIT(1, TUPLE_ROW);

	value.type = DQLITE_ISO8601;
	value.iso8601 = "2018-07-20 09:49:05+00:00";
	ENCODER_NEXT;

	munit_assert_int(buf[0][0], ==, DQLITE_ISO8601);
	munit_assert_string_equal((char *)buf[1], "2018-07-20 09:49:05+00:00");

	return MUNIT_OK;
}

/* Encode a boolean parameter. */
TEST_CASE(encoder, type, boolean, NULL)
{
	struct encoderFixture *f = data;
	struct value value;
	uint8_t(*buf)[8] = f->buffer.data;
	(void)params;

	ENCODER_INIT(1, TUPLE_ROW);

	value.type = DQLITE_BOOLEAN;
	value.boolean = 1;
	ENCODER_NEXT;

	munit_assert_int(buf[0][0], ==, DQLITE_BOOLEAN);
	munit_assert_int(*(uint64_t *)buf[1], ==, byteFlip64(value.boolean));

	return MUNIT_OK;
}
