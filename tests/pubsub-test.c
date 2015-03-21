#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "greatest.h"
#include "util.h"
#include "../src/rlite.h"
#include "../src/pubsub.h"

#define CHANNEL "mychannel"
#define CHANNEL2 "other channel"

struct buf {
	const char *data;
	size_t datalen;
	const char *channel;
	size_t channellen;
	int read;
};

static void do_publish(rlite *db, struct buf *buffer) {
	if (rl_publish(db, UNSIGN(buffer->channel), buffer->channellen, buffer->data, buffer->datalen)) {
		fprintf(stderr, "Failed to publish\n");
		return;
	}
	if (rl_commit(db)) {
		fprintf(stderr, "Failed to commit\n");
		return;
	}
}

static void* publish(void* _buffer) {
	sleep(1);
	struct buf *buffer = _buffer;

	rlite *db = NULL;
	if (setup_db(&db, 1, 0)) {
		fprintf(stderr, "Failed to open database\n");
		return NULL;
	}
	do_publish(db, buffer);
	rl_close(db);
	return NULL;
}

static void poll(rlite *db, struct buf *buffer) {
	int elementc;
	unsigned char **elements;
	long *elementslen;
	char *testdata = NULL;
	size_t testdatalen = 0;
	while (rl_poll(db, &elementc, &elements, &elementslen) == RL_NOT_FOUND) {
		// need to discard to release lock on file
		rl_discard(db);
		sleep(1);
	}
	testdata = (char *)elements[2];
	testdatalen = (size_t)elementslen[2];

	if (buffer->datalen == testdatalen && memcmp(buffer->data, testdata, testdatalen) == 0) {
		buffer->read = 1;
	} else {
		fprintf(stderr, "Data mismatch on secondary subscriber");
	}
	rl_free(elements[0]);
	rl_free(elements[1]);
	rl_free(elements[2]);
	rl_free(elements);
	rl_free(elementslen);
}

static void* subscribe(void* _buffer) {
	struct buf *buffer = _buffer;

	rlite *db = NULL;
	if (setup_db(&db, 1, 0)) {
		fprintf(stderr, "Failed to open database\n");
		return NULL;
	}
	char *channel = CHANNEL;
	long channellen = strlen(CHANNEL);
	if (rl_subscribe(db, 1, (unsigned char **)&channel, &channellen)) {
		fprintf(stderr, "Failed to subscribe\n");
		return NULL;
	}

	poll(db, buffer);
	rl_close(db);

	return NULL;
}

TEST basic_subscribe_publish()
{
	int retval;
	static const char *data = "hello world!";
	size_t datalen = strlen(data);
	char *testdata = NULL;
	size_t testdatalen = 0;
	char *channel = CHANNEL;
	long channellen = strlen(CHANNEL);

	struct buf buffer;
	buffer.data = data;
	buffer.datalen = datalen;
	buffer.channel = channel;
	buffer.channellen = channellen;

	rlite *db = NULL;
	RL_CALL_VERBOSE(setup_db, RL_OK, &db, 1, 1);
	if (rl_subscribe(db, 1, (unsigned char **)&channel, &channellen)) {
		fprintf(stderr, "Failed to subscribe\n");
		FAIL();
	}
	do_publish(db, &buffer);
	rl_discard(db);
	poll(db, &buffer);
	rl_close(db);

	ASSERT_EQ(buffer.read, 1);
	rl_free(testdata);
	PASS();
}

TEST basic_subscribe_publish_newdb()
{
	int retval;
	static const char *data = "hello world!";
	size_t datalen = strlen(data);
	char *testdata = NULL;
	size_t testdatalen = 0;
	char *channel = CHANNEL;
	long channellen = strlen(CHANNEL);

	struct buf buffer;
	buffer.data = data;
	buffer.datalen = datalen;
	buffer.channel = channel;
	buffer.channellen = channellen;

	rlite *db = NULL;
	RL_CALL_VERBOSE(setup_db, RL_OK, &db, 1, 1);
	if (rl_subscribe(db, 1, (unsigned char **)&channel, &channellen)) {
		fprintf(stderr, "Failed to subscribe\n");
		FAIL();
	}
	rl_discard(db);
	publish(&buffer);
	poll(db, &buffer);
	rl_close(db);

	ASSERT_EQ(buffer.read, 1);
	rl_free(testdata);
	PASS();
}

/*
TEST basic_publish_two_subscribers()
{
	int retval;
	static const char *data = "hello world!";
	size_t datalen = strlen(data);
	char *testdata = NULL;
	size_t testdatalen = 0;
	pthread_t thread_w;
	pthread_t thread_r;
	char *channel = CHANNEL;
	long channellen = strlen(CHANNEL);

	struct buf buffer;
	buffer.data = data;
	buffer.datalen = datalen;
	buffer.read = 0;
	buffer.channel = channel;
	buffer.channellen = channellen;

	rlite *db = NULL;
	RL_CALL_VERBOSE(setup_db, RL_OK, &db, 1, 1);
	pthread_create(&thread_w, NULL, publish, &buffer);
	pthread_create(&thread_r, NULL, subscribe, &buffer);
	RL_CALL_VERBOSE(rl_subscribe, RL_OK, db, 1, (unsigned char **)&channel, &channellen, &testdata, &testdatalen);
	rl_close(db);

	ASSERT_EQ(buffer.read, 1);
	ASSERT_EQ(datalen, testdatalen);
	ASSERT_EQ(memcmp(data, testdata, datalen), 0);
	rl_free(testdata);
	PASS();
}

TEST basic_publish_no_subscriber()
{
	int retval;
	static const char *data = "hello world!";
	size_t datalen = strlen(data);

	struct buf buffer;
	buffer.data = data;
	buffer.datalen = datalen;

	rlite *db = NULL;
	RL_CALL_VERBOSE(setup_db, RL_OK, &db, 1, 1);
	rl_close(db);
	publish(&buffer);

	PASS();
}

TEST basic_subscribe2_publish(int publish_channel)
{
	int retval;
	static const char *data = "hello world!";
	size_t datalen = strlen(data);
	char *testdata = NULL;
	size_t testdatalen = 0;
	pthread_t thread;
	unsigned char *channels[2] = {UNSIGN(CHANNEL), UNSIGN(CHANNEL2)};
	long channelslen[2] = {strlen(CHANNEL), strlen(CHANNEL2)};

	struct buf buffer;
	buffer.data = data;
	buffer.datalen = datalen;
	buffer.channel = (const char *)channels[publish_channel];
	buffer.channellen = channelslen[publish_channel];

	rlite *db = NULL;
	RL_CALL_VERBOSE(setup_db, RL_OK, &db, 1, 1);
	pthread_create(&thread, NULL, publish, &buffer);
	RL_CALL_VERBOSE(rl_subscribe, RL_OK, db, 2, channels, channelslen, &testdata, &testdatalen);
	rl_close(db);

	ASSERT_EQ(datalen, testdatalen);
	ASSERT_EQ(memcmp(data, testdata, datalen), 0);
	rl_free(testdata);
	PASS();
}
*/

SUITE(pubsub_test)
{
	RUN_TEST(basic_subscribe_publish);
	RUN_TEST(basic_subscribe_publish_newdb);
	// RUN_TEST(basic_publish_two_subscribers);
	// RUN_TEST(basic_publish_no_subscriber);
	// RUN_TEST1(basic_subscribe2_publish, 0);
	// RUN_TEST1(basic_subscribe2_publish, 1);
}