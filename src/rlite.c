#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/md5.h>
#include "page_btree.h"
#include "page_list.h"
#include "page_string.h"
#include "rlite.h"
#include "util.h"

#define DEFAULT_READ_PAGES_LEN 16
#define DEFAULT_WRITE_PAGES_LEN 8
#define DEFAULT_PAGE_SIZE 1024
#define HEADER_SIZE 100

int rl_header_serialize(struct rlite *db, void *obj, unsigned char *data);
int rl_header_deserialize(struct rlite *db, void **obj, void *context, unsigned char *data);
int rl_has_flag(rlite *db, int flag);

rl_data_type rl_data_type_btree_hash_md5_long = {
	"btree_hash_md5_long",
	rl_btree_serialize,
	rl_btree_deserialize,
	rl_btree_destroy,
};
rl_data_type rl_data_type_btree_node_hash_md5_long = {
	"btree_node_hash_md5_long",
	rl_btree_node_serialize_hash_md5_long,
	rl_btree_node_deserialize_hash_md5_long,
	rl_btree_node_destroy,
};
rl_data_type rl_data_type_header = {
	"header",
	rl_header_serialize,
	rl_header_deserialize,
	NULL
};
rl_data_type rl_data_type_btree_set_long = {
	"btree_set_long",
	rl_btree_serialize,
	rl_btree_deserialize,
	rl_btree_destroy,
};
rl_data_type rl_data_type_btree_node_set_long = {
	"btree_node_set_long",
	rl_btree_node_serialize_set_long,
	rl_btree_node_deserialize_set_long,
	rl_btree_node_destroy,
};
rl_data_type rl_data_type_btree_hash_long_long = {
	"btree_hash_long_long",
	rl_btree_serialize,
	rl_btree_deserialize,
	rl_btree_destroy,
};
rl_data_type rl_data_type_btree_node_hash_long_long = {
	"btree_node_hash_long_long",
	rl_btree_node_serialize_hash_long_long,
	rl_btree_node_deserialize_hash_long_long,
	rl_btree_node_destroy,
};
rl_data_type rl_data_type_list_long = {
	"list_long",
	rl_list_serialize,
	rl_list_deserialize,
	rl_list_destroy,
};
rl_data_type rl_data_type_list_node_long = {
	"list_node_long",
	rl_list_node_serialize_long,
	rl_list_node_deserialize_long,
	rl_list_node_destroy,
};
rl_data_type rl_data_type_list_key = {
	"list_key",
	rl_list_serialize,
	rl_list_deserialize,
	rl_list_destroy,
};
rl_data_type rl_data_type_list_node_key = {
	"list_node_key",
	rl_list_node_serialize_key,
	rl_list_node_deserialize_key,
	rl_list_node_destroy,
};

rl_data_type rl_data_type_string = {
	"string",
	rl_string_serialize,
	rl_string_deserialize,
	rl_string_destroy,
};

static const unsigned char *identifier = (unsigned char *)"rlite0.0";

static int file_driver_fp(rlite *db)
{
	int retval = RL_OK;
	rl_file_driver *driver = db->driver;
	if (driver->fp == NULL) {
		driver->fp = fopen(driver->filename, (driver->mode & RLITE_OPEN_READWRITE) == 0 ? "r" : "w+");
		if (driver->fp == NULL) {
			fprintf(stderr, "Cannot open file %s, errno %d\n", driver->filename, errno);
			retval = RL_UNEXPECTED;
			goto cleanup;
		}
	}
cleanup:
	return retval;
}

int rl_header_serialize(struct rlite *db, void *obj, unsigned char *data)
{
	obj = obj;
	int retval = RL_OK;
	int identifier_len = strlen((char *)identifier);
	memcpy(data, identifier, identifier_len);
	put_4bytes(&data[identifier_len], db->page_size);
	return retval;
}

int rl_header_deserialize(struct rlite *db, void **obj, void *context, unsigned char *data)
{
	obj = obj;
	context = context;
	int retval = RL_OK;
	int identifier_len = strlen((char *)identifier);
	if (memcmp(data, identifier, identifier_len) != 0) {
		fprintf(stderr, "Unexpected header, expecting %s\n", identifier);
		return RL_INVALID_STATE;
	}
	db->page_size = get_4bytes(&data[identifier_len]);
	return retval;
}

static int rl_ensure_pages(rlite *db)
{
	void *tmp;
	if (rl_has_flag(db, RLITE_OPEN_READWRITE)) {
		if (db->write_pages_len == db->write_pages_alloc) {
			tmp = realloc(db->write_pages, sizeof(rl_page *) * db->write_pages_alloc * 2);
			if (!tmp) {
				return RL_OUT_OF_MEMORY;
			}
			db->write_pages = tmp;
			db->write_pages_alloc *= 2;
		}
	}
	if (db->read_pages_len == db->read_pages_alloc) {
		tmp = realloc(db->read_pages, sizeof(rl_page *) * db->read_pages_alloc * 2);
		if (!tmp) {
			return RL_OUT_OF_MEMORY;
		}
		db->read_pages = tmp;
		db->read_pages_alloc *= 2;
	}
	return RL_OK;
}

int rl_open(const char *filename, rlite **_db, int flags)
{
	rl_btree_init();
	rl_list_init();
	int retval = RL_OK;
	rlite *db = malloc(sizeof(rlite));
	if (db == NULL) {
		retval = RL_OUT_OF_MEMORY;
		goto cleanup;
	}

	db->page_size = DEFAULT_PAGE_SIZE;
	db->read_pages = db->write_pages = NULL;

	db->read_pages = malloc(sizeof(rl_page *) * DEFAULT_READ_PAGES_LEN);
	if (db->read_pages == NULL) {
		retval = RL_OUT_OF_MEMORY;
		goto cleanup;
	}
	db->read_pages_len = 0;
	db->read_pages_alloc = DEFAULT_READ_PAGES_LEN;
	db->write_pages_len = 0;
	if ((flags & RLITE_OPEN_READWRITE) > 0) {
		db->write_pages_alloc = DEFAULT_WRITE_PAGES_LEN;
		db->write_pages = malloc(sizeof(rl_page *) * DEFAULT_WRITE_PAGES_LEN);
	}
	else {
		db->write_pages_alloc = 0;
	}

	if (memcmp(filename, ":memory:", 9) == 0) {
		db->driver_type = RL_MEMORY_DRIVER;
	}
	else {
		if ((flags & RLITE_OPEN_CREATE) == 0) {
			int _access_flags;
			if ((flags & RLITE_OPEN_READWRITE) != 0) {
				_access_flags = W_OK;
			}
			else {
				_access_flags = R_OK | F_OK;
			}
			if (access(filename, _access_flags) != 0) {
				retval = RL_INVALID_PARAMETERS;
				goto cleanup;
			}
		}

		rl_file_driver *driver = malloc(sizeof(rl_file_driver));
		if (driver == NULL) {
			retval = RL_OUT_OF_MEMORY;
			goto cleanup;
		}
		driver->fp = NULL;
		driver->filename = malloc(sizeof(char) * (strlen(filename) + 1));
		if (driver->filename == NULL) {
			free(driver);
			retval = RL_OUT_OF_MEMORY;
			goto cleanup;
		}
		strcpy(driver->filename, filename);
		driver->mode = flags;
		db->driver = driver;
		db->driver_type = RL_FILE_DRIVER;
	}

	rl_read_header(db);

	*_db = db;
cleanup:
	if (retval != RL_OK) {
		free(db->read_pages);
		free(db->write_pages);
		free(db);
	}
	return retval;
}

int rl_close(rlite *db)
{
	if (db->driver_type == RL_FILE_DRIVER) {
		rl_file_driver *driver = db->driver;
		if (driver->fp) {
			fclose(driver->fp);
		}
		free(driver->filename);
		free(db->driver);
	}
	rl_discard(db);
	free(db->read_pages);
	free(db->write_pages);
	free(db);
	return RL_OK;
}

int rl_has_flag(rlite *db, int flag)
{
	if (db->driver_type == RL_FILE_DRIVER) {
		return (((rl_file_driver *)db->driver)->mode & flag) > 0;
	}
	else if (db->driver_type == RL_MEMORY_DRIVER) {
		return 1;
	}
	fprintf(stderr, "Unknown driver_type %d\n", db->driver_type);
	return RL_UNEXPECTED;
}

int rl_create_db(rlite *db)
{
	int retval = rl_ensure_pages(db);
	db->next_empty_page = 2;
	rl_btree *btree;
	long max_node_size = (db->page_size - 8) / 8; // TODO: this should be in the type
	retval = rl_btree_create(db, &btree, &btree_hash_md5_long, max_node_size);
	if (retval == RL_OK) {
		retval = rl_write(db, &rl_data_type_btree_hash_md5_long, 1, btree);
	}
	return retval;
}

int rl_get_key_btree(rlite *db, rl_btree **btree)
{
	void *_btree;
	int retval = rl_read(db, &rl_data_type_btree_hash_md5_long, 1, NULL, &_btree);
	if (retval != RL_FOUND) {
		goto cleanup;
	}
	*btree = _btree;
	retval = RL_OK;
cleanup:
	return RL_OK;
}

int rl_read_header(rlite *db)
{
	db->page_size = HEADER_SIZE;
	void *header;
	int retval;
	if (db->driver_type == RL_MEMORY_DRIVER) {
		db->page_size = DEFAULT_PAGE_SIZE;
		retval = rl_create_db(db);
		if (retval != RL_OK) {
			goto cleanup;
		}
	}
	else {
		retval = rl_read(db, &rl_data_type_header, 0, NULL, &header);
		if (retval == RL_NOT_FOUND && rl_has_flag(db, RLITE_OPEN_CREATE)) {
			db->page_size = DEFAULT_PAGE_SIZE;
			retval = rl_create_db(db);
			if (retval != RL_OK) {
				goto cleanup;
			}
		}
	}
cleanup:
	return retval;
}

#ifdef DEBUG
void print_cache(rlite *db)
{
	printf("Cache read pages:");
	long i;
	rl_page *page;
	for (i = 0; i < db->read_pages_len; i++) {
		page = db->read_pages[i];
		printf("%ld, ", page->page_number);
	}
	printf("\nCache write pages:");
	for (i = 0; i < db->write_pages_len; i++) {
		page = db->write_pages[i];
		printf("%ld, ", page->page_number);
	}
	printf("\n");
}
#endif

static int rl_search_cache(rlite *db, rl_data_type *type, long page_number, void **obj, long *position, rl_page **pages, long page_len)
{
	db = db;

	long pos, min = 0, max = page_len - 1;
	rl_page *page;
	if (max >= 0) {
		do {
			pos = min + (max - min) / 2;
			page = pages[pos];
			if (page->page_number == page_number) {
				if (type != NULL && page->type != type) {
					fprintf(stderr, "Type of page in cache (%s) doesn't match the asked one (%s)\n", page->type->name, type->name);
					return RL_UNEXPECTED;
				}
				if (obj) {
					*obj = page->obj;
				}
				if (position) {
					*position = pos;
				}
				return RL_FOUND;
			}
			else if (page->page_number > page_number) {
				max = pos - 1;
			}
			else {
				min = pos + 1;
			}
		}
		while (max >= min);
		if (position) {
			if (pages[pos]->page_number > page_number && pos > 0) {
				pos--;
			}
			if (pages[pos]->page_number < page_number && pos < page_len) {
				pos++;
			}
#ifdef DEBUG
			long i, prev;
			rl_page *p;
			for (i = 0; i < db->read_pages_len; i++) {
				p = db->read_pages[i];
				if (i > 0) {
					if (prev >= p->page_number) {
						fprintf(stderr, "Reading cache is broken\n");
						print_cache(db);
						return RL_UNEXPECTED;
					}
				}
				prev = p->page_number;
			}
			for (i = 0; i < db->write_pages_len; i++) {
				p = db->write_pages[i];
				if (i > 0) {
					if (prev >= p->page_number) {
						fprintf(stderr, "Writing cache is broken\n");
						print_cache(db);
						return RL_UNEXPECTED;
					}
				}
				prev = p->page_number;
			}
#endif
			*position = pos;
		}
	}
	else {
		if (position) {
			*position = 0;
		}
	}
	return RL_NOT_FOUND;
}

int rl_read_from_cache(rlite *db, rl_data_type *type, long page_number, void **obj)
{
	int retval = rl_search_cache(db, type, page_number, obj, NULL, db->write_pages, db->write_pages_len);
	if (retval == RL_NOT_FOUND) {
		retval = rl_search_cache(db, type, page_number, obj, NULL, db->read_pages, db->read_pages_len);
	}
	return retval;
}

int rl_read(rlite *db, rl_data_type *type, long page, void *context, void **obj)
{
	int retval = rl_read_from_cache(db, type, page, obj);
	if (retval != RL_NOT_FOUND) {
		return retval;
	}
	unsigned char *data = malloc(db->page_size * sizeof(unsigned char));
	if (data == NULL) {
		retval = RL_OUT_OF_MEMORY;
		goto cleanup;
	}
	if (db->driver_type == RL_FILE_DRIVER) {
		retval = file_driver_fp(db);
		if (retval != RL_OK) {
			goto cleanup;
		}
		rl_file_driver *driver = db->driver;
		fseek(driver->fp, page * db->page_size, SEEK_SET);
		size_t read = fread(data, sizeof(unsigned char), db->page_size, driver->fp);
		if (read != (size_t)db->page_size) {
			if (page > 0) {
#ifdef DEBUG
				print_cache(db);
#endif
				fprintf(stderr, "Unable to read page %ld: ", page);
				perror(NULL);
			}
			retval = RL_NOT_FOUND;
			goto cleanup;
		}
		long pos;
		retval = rl_search_cache(db, type, page, NULL, &pos, db->read_pages, db->read_pages_len);
		if (retval != RL_NOT_FOUND) {
			fprintf(stderr, "Unexpectedly found page in cache\n");
			retval = RL_UNEXPECTED;
			goto cleanup;
		}

		retval = type->deserialize(db, obj, context ? context : type, data);

		rl_ensure_pages(db);
		rl_page *page_obj = malloc(sizeof(rl_page));
		if (page_obj == NULL) {
			return RL_OUT_OF_MEMORY;
		}
		page_obj->page_number = page;
		page_obj->type = type;
		page_obj->obj = *obj;
#ifdef DEBUG
		page_obj->serialized_data = data;

		unsigned char *serialize_data = calloc(db->page_size, sizeof(unsigned char));
		retval = type->serialize(db, *obj, serialize_data);
		if (retval != RL_OK) {
			goto cleanup;
		}
		if (memcmp(data, serialize_data, db->page_size) != 0) {
			fprintf(stderr, "serialize unserialized data mismatch\n");
			long i;
			for (i = 0; i < db->page_size; i++) {
				if (serialize_data[i] != data[i]) {
					fprintf(stderr, "at position %ld expected %d, got %d\n", i, serialize_data[i], data[i]);
				}
			}
		}
		free(serialize_data);
#endif
		if (pos < db->read_pages_len) {
			memmove_dbg(&db->read_pages[pos + 1], &db->read_pages[pos], sizeof(rl_page *) * (db->read_pages_len - pos), __LINE__);
		}
		db->read_pages[pos] = page_obj;
		db->read_pages_len++;
	}
	else {
		fprintf(stderr, "Unexpected driver %d when asking for page %ld\n", db->driver_type, page);
		retval = RL_UNEXPECTED;
		goto cleanup;
	}
	if (retval == RL_OK) {
		retval = RL_FOUND;
	}
cleanup:
	free(data);
	return retval;
}

int rl_write(struct rlite *db, rl_data_type *type, long page_number, void *obj)
{
	long pos;
	if (page_number == 0) {
		fprintf(stderr, "Unable to write to page number 0\n");
		return RL_UNEXPECTED;
	}
	int retval = rl_search_cache(db, type, page_number, NULL, &pos, db->write_pages, db->write_pages_len);
	if (retval == RL_FOUND) {
		db->write_pages[pos]->obj = obj;
	}
	else if (retval == RL_NOT_FOUND) {
		rl_ensure_pages(db);
		rl_page *page = malloc(sizeof(rl_page));
		if (page == NULL) {
			return RL_OUT_OF_MEMORY;
		}
		page->page_number = page_number;
		page->type = type;
		page->obj = obj;
		if (pos < db->write_pages_len) {
			memmove_dbg(&db->write_pages[pos + 1], &db->write_pages[pos], sizeof(rl_page *) * (db->write_pages_len - pos), __LINE__);
		}
		db->write_pages[pos] = page;
		db->write_pages_len++;
		if (page_number == db->next_empty_page) {
			db->next_empty_page++;
		}

		retval = rl_search_cache(db, type, page_number, NULL, &pos, db->read_pages, db->read_pages_len);
		if (retval == RL_NOT_FOUND) {
			retval = RL_OK;
		}
		else if (retval == RL_FOUND) {
			free(db->read_pages[pos]);
			memmove_dbg(&db->read_pages[pos], &db->read_pages[pos + 1], sizeof(rl_page *) * (db->read_pages_len - pos), __LINE__);
			db->read_pages_len--;
			retval = RL_OK;
		}
		return retval;
	}
	return RL_OK;
}

int rl_delete(struct rlite *db, long page_number)
{
	long pos;
	int retval = rl_search_cache(db, NULL, page_number, NULL, &pos, db->write_pages, db->write_pages_len);
	if (retval == RL_FOUND) {
		db->write_pages[pos]->obj = NULL;
		retval = RL_OK;
	}
	else if (retval == RL_NOT_FOUND) {
		// TODO: clear page, reuse
		retval = rl_search_cache(db, NULL, page_number, NULL, &pos, db->read_pages, db->read_pages_len);
		if (retval == RL_FOUND) {
			db->read_pages[pos]->obj = NULL;
		}
		retval = RL_OK;
	}
	return retval;
}

int rl_commit(struct rlite *db)
{
	int retval = RL_UNEXPECTED;
	long i, page_number;
	rl_page *page;
	unsigned char *data = malloc(db->page_size * sizeof(unsigned char));
#ifdef DEBUG
	for (i = 0; i < db->read_pages_len; i++) {
		page = db->read_pages[i];
		memset(data, 0, db->page_size);
		retval = page->type->serialize(db, page->obj, data);
		if (retval != RL_OK) {
			goto cleanup;
		}
		if (memcmp(data, page->serialized_data, db->page_size) != 0) {
			fprintf(stderr, "Read page %ld (%s) was changed\n", page->page_number, page->type->name);
			retval = rl_search_cache(db, page->type, page->page_number, NULL, NULL, db->write_pages, db->write_pages_len);
			if (retval == RL_FOUND) {
				fprintf(stderr, "Page found in write_pages\n");
			}
			else {
				fprintf(stderr, "Page not found in write_pages\n");
			}
			exit(1);
		}
	}
#endif
	if (db->driver_type == RL_FILE_DRIVER) {
		if (!data) {
			return RL_OUT_OF_MEMORY;
		}
		retval = file_driver_fp(db);
		if (retval != RL_OK) {
			goto cleanup;
		}
		for (i = 0; i < db->write_pages_len; i++) {
			rl_file_driver *driver = db->driver;
			page = db->write_pages[i];
			page_number = page->page_number;
			memset(data, 0, db->page_size);
			retval = page->type->serialize(db, page->obj, data);
			fseek(driver->fp, page_number * db->page_size, SEEK_SET);
			if ((size_t)db->page_size != fwrite(data, sizeof(unsigned char), db->page_size, driver->fp)) {
#ifdef DEBUG
				print_cache(db);
#endif
				retval = RL_UNEXPECTED;
				goto cleanup;
			}
		}
		free(data);
		rl_discard(db);
	}
	else if (db->driver_type == RL_MEMORY_DRIVER) {
		return RL_OK;
	}
cleanup:
	return retval;
}

int rl_discard(struct rlite *db)
{
	long i;
	void *tmp;
	int retval = RL_OK;

	rl_page *page;
	for (i = 0; i < db->read_pages_len; i++) {
		page = db->read_pages[i];
		if (page->type->destroy && page->obj) {
			page->type->destroy(db, page->obj);
		}
		free(page);
	}
	for (i = 0; i < db->write_pages_len; i++) {
		page = db->write_pages[i];
		if (page->type->destroy && page->obj) {
			page->type->destroy(db, page->obj);
		}
		free(page);
	}
	db->read_pages_len = 0;
	db->write_pages_len = 0;

	if (db->read_pages_alloc != DEFAULT_READ_PAGES_LEN) {
		tmp = realloc(db->read_pages, sizeof(rl_page *) * DEFAULT_READ_PAGES_LEN);
		if (!tmp) {
			retval = RL_OUT_OF_MEMORY;
			goto cleanup;
		}
		db->read_pages = tmp;
		db->read_pages_alloc = DEFAULT_READ_PAGES_LEN;
	}
	if (db->write_pages_alloc > 0) {
		if (db->write_pages_alloc != DEFAULT_WRITE_PAGES_LEN) {
			db->write_pages_alloc = DEFAULT_WRITE_PAGES_LEN;
			tmp = realloc(db->write_pages, sizeof(rl_page *) * DEFAULT_WRITE_PAGES_LEN);
			if (!tmp) {
				retval = RL_OUT_OF_MEMORY;
				goto cleanup;
			}
			db->write_pages = tmp;
		}
	}
cleanup:
	return retval;
}

static int md5(const unsigned char *data, long datalen, unsigned char digest[16])
{
	MD5_CTX md5;
	MD5_Init(&md5);
	MD5_Update(&md5, data, datalen);
	MD5_Final(digest, &md5);
	return RL_OK;
}

int rl_set_key(rlite *db, const unsigned char *key, long keylen, long value)
{
	unsigned char *digest = malloc(sizeof(unsigned char) * 16);
	int retval = md5(key, keylen, digest);
	if (retval != RL_OK) {
		goto cleanup;
	}
	void *_btree;
	retval = rl_read(db, &rl_data_type_btree_hash_md5_long, 1, NULL, &_btree);
	if (retval != RL_FOUND) {
		goto cleanup;
	}
	rl_btree *btree = (rl_btree *)_btree;
	long *val = malloc(sizeof(long));
	if (!val) {
		retval = RL_OUT_OF_MEMORY;
		goto cleanup;
	}
	*val = value;
	retval = rl_btree_add_element(db, btree, digest, val);
cleanup:
	return retval;
}

int rl_get_key(rlite *db, const unsigned char *key, long keylen, long *value)
{
	unsigned char digest[16];
	int retval = md5(key, keylen, digest);
	if (retval != RL_OK) {
		goto cleanup;
	}
	void *_btree;
	retval = rl_read(db, &rl_data_type_btree_hash_md5_long, 1, NULL, &_btree);
	if (retval != RL_FOUND) {
		goto cleanup;
	}
	rl_btree *btree = (rl_btree *)_btree;
	void *val;
	retval = rl_btree_find_score(db, btree, digest, &val, NULL, NULL);
	if (retval == RL_FOUND && value) {
		*value = *(long *)val;
	}
cleanup:
	return retval;
}
