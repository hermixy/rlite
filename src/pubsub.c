#include "rlite.h"
#include "util.h"
#include "fifo.h"
#include "pubsub.h"

#define FILENAME_LENGTH 8

static void generate_subscriptor_id(rlite *db)
{
	// TODO: check for collisions and retry?
	// this is probably the worst random hash in the history of programming
	char str[60];
	unsigned char digest[20];
	memset(str, 0, 60);
	snprintf(str, 60, "%llu", rl_mstime());
	snprintf(&str[40], 20, "%d", rand());
	sha1((unsigned char *)str, 60, digest);
	sha1_formatter(digest, &db->subscriptor_id, NULL);
}

static char *get_filename(rlite *db, char *subscriptor_id)
{
	rl_file_driver *driver = db->driver;
	if (subscriptor_id == NULL) {
		if (db->subscriptor_id == NULL) {
			generate_subscriptor_id(db);
			if (db->subscriptor_id == NULL) {
				return NULL;
			}
		}
		subscriptor_id = db->subscriptor_id;
	}
	return rl_get_filename_with_suffix(driver->filename, subscriptor_id);
}


int rl_subscribe(rlite *db, int channelc, unsigned char **channelv, long *channelvlen)
{
	int i, retval;
	long identifierlen[1] = {40};
	if (db->subscriptor_id == NULL) {
		generate_subscriptor_id(db);
		if (db->subscriptor_id == NULL) {
			retval = RL_OUT_OF_MEMORY;
			goto cleanup;
		}
	}
	RL_CALL(rl_select_internal, RL_OK, db, RLITE_INTERNAL_DB_SUBSCRIBER_CHANNELS);
	for (i = 0; i < channelc; i++) {
		RL_CALL(rl_sadd, RL_OK, db, channelv[i], channelvlen[i], 1, (unsigned char **)&db->subscriptor_id, identifierlen, NULL);
	}
	// important! commit to release the exclusive lock
	RL_CALL(rl_commit, RL_OK, db);
cleanup:
	rl_select_internal(db, RLITE_INTERNAL_DB_NO);
	return retval;
}

int rl_unsubscribe(rlite *db, int channelc, unsigned char **channelv, long *channelvlen)
{
	int i, retval;
	long identifierlen[1] = {40};
	if (db->subscriptor_id == NULL) {
		// if there's no subscriptor id, then the connection is not subscribed to anything
		return RL_OK;
	}
	RL_CALL(rl_select_internal, RL_OK, db, RLITE_INTERNAL_DB_SUBSCRIBER_CHANNELS);
	for (i = 0; i < channelc; i++) {
		RL_CALL(rl_srem, RL_OK, db, channelv[i], channelvlen[i], 1, (unsigned char **)&db->subscriptor_id, identifierlen, NULL);
	}
	// important! commit to release the exclusive lock
	RL_CALL(rl_commit, RL_OK, db);
cleanup:
	rl_select_internal(db, RLITE_INTERNAL_DB_NO);
	return retval;
}

int rl_poll_wait(rlite *db, int *elementc, unsigned char ***_elements, long **_elementslen, struct timeval *timeout)
{
	int retval = rl_poll(db, elementc, _elements, _elementslen);
	if (retval == RL_NOT_FOUND) {
		// TODO: possible race condition between poll and fifo read?
		// can we atomically do both without locking the database?
		char *filename = get_filename(db, NULL);
		rl_discard(db);
		rl_create_fifo(filename);
		rl_read_fifo(filename, timeout, NULL, NULL);
		rl_free(filename);
		retval = rl_poll(db, elementc, _elements, _elementslen);
	}
	return retval;
}

int rl_poll(rlite *db, int *elementc, unsigned char ***_elements, long **_elementslen)
{
	int retval;
	unsigned char *len = NULL, i;
	long lenlen;
	unsigned char **elements = NULL;
	long *elementslen = NULL;
	if (db->subscriptor_id == NULL) {
		retval = RL_NOT_FOUND;
		goto cleanup;
	}
	RL_CALL(rl_refresh, RL_OK, db);
	RL_CALL(rl_select_internal, RL_OK, db, RLITE_INTERNAL_DB_SUBSCRIBER_MESSAGES);
	RL_CALL(rl_pop, RL_OK, db, (unsigned char *)db->subscriptor_id, 40, &len, &lenlen, 1);
	if (lenlen != 1) {
		// TODO: delete the key and commit? PANIC
		retval = RL_UNEXPECTED;
		goto cleanup;
	}

	*elementc = len[0];
	RL_MALLOC(elements, sizeof(unsigned char *) * len[0]);
	RL_MALLOC(elementslen, sizeof(long) * len[0]);
	for (i = 0; i < len[0]; i++) {
		RL_CALL(rl_pop, RL_OK, db, (unsigned char *)db->subscriptor_id, 40, &elements[i], &elementslen[i], 1);
	}
	*_elements = elements;
	*_elementslen = elementslen;
cleanup:
	rl_free(len);
	rl_select_internal(db, RLITE_INTERNAL_DB_NO);
	return retval;
}

int rl_publish(rlite *db, unsigned char *channel, size_t channellen, const char *data, size_t datalen, long *_recipients)
{
	unsigned char *value = NULL;
	char *filename = NULL, *subscriptor_id = NULL;
	long valuelen, recipients = 0;
	int retval;
	unsigned char *values[4];
	long valueslen[4];
	rl_set_iterator *iterator;
	RL_CALL(rl_select_internal, RL_OK, db, RLITE_INTERNAL_DB_SUBSCRIBER_CHANNELS);
	RL_CALL(rl_smembers, RL_OK, db, &iterator, channel, channellen);

#define MESSAGE "message"
	unsigned char q = 3;
	values[0] = &q;
	valueslen[0] = 1;
	values[1] = (unsigned char *)MESSAGE;
	valueslen[1] = (long)strlen(MESSAGE);
	values[2] = channel;
	valueslen[2] = channellen;
	values[3] = (unsigned char *)data;
	valueslen[3] = datalen;

	RL_CALL(rl_select_internal, RL_OK, db, RLITE_INTERNAL_DB_SUBSCRIBER_MESSAGES);
	while ((retval = rl_set_iterator_next(iterator, &value, &valuelen)) == RL_OK) {
		recipients++;
		RL_CALL(rl_push, RL_OK, db, value, valuelen, 1, 0, 4, values, valueslen, NULL);

		subscriptor_id = rl_malloc(sizeof(char) * (valuelen + 1));
		memcpy(subscriptor_id, value, valuelen);
		subscriptor_id[valuelen] = 0;

		filename = get_filename(db, subscriptor_id);
		// this only signals the fifo recipient that new data is available
		rl_write_fifo(filename, "1", 1);
		rl_free(filename);
		filename = NULL;
		rl_free(subscriptor_id);
		subscriptor_id = NULL;
		rl_free(value);
		value = NULL;
	}
	if (retval == RL_END) {
		retval = RL_OK;
	}
	if (_recipients) {
		*_recipients = recipients;
	}
cleanup:
	if (retval == RL_NOT_FOUND) {
		// no subscriber? no problem
		if (_recipients) {
			*_recipients = 0;
		}
		retval = RL_OK;
	}
	rl_free(value);
	rl_free(filename);
	rl_free(subscriptor_id);
	rl_select_internal(db, RLITE_INTERNAL_DB_NO);
	return retval;
}
