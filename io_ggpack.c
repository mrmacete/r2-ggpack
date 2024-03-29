/* MIT - Copyright 2017 - mrmacete */

#include <r_core.h>
#include <r_io.h>
#include <r_lib.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include "r_ggpack.h"
#include "gglib.h"
#include "r_ggpack_pass2.h"
#include "rtmi.h"

#define ENABLE_DEBUG 0

#if ENABLE_DEBUG
#define dbg_log(...) eprintf (__VA_ARGS__)
#else
#define dbg_log(...)
#endif

extern RIOPlugin r_io_plugin_ggpack;

#define BRUTE_VERSIONS 4

/*
 * I think this changes across platforms, i have only these 2 sample points.
 * If you stumble upon "cannot open file" while trying this on your platform,
 * just open the TWP binary in r2 and try to search your magic bytes with the
 * command: /x 4fd0a0ac4a..b9e5937945a5c1cb3193
 *
 * If there's no result, just try to enlarge the ".." mask.
 *
 * Once found, you can just "pc 16@hit0_0" (or whatever your hit is) and add the
 * result to this list.
 *
 * The above accounts for the first 16 magic bytes. The last one is a multiplier
 * and can be found in this way:
 *
 * - search for references to the magic bytes
 * - get the second reference
 * - the instruction above is an add, looks like this
 *          0x1000aa2c4      4881c3ad9912.  add rbx, 0x421299ad   <--added constant
│*          0x1000aa2cb      4c8d0dce0428.  lea r9, [0x10032a7a0] <--reference
 *
 * - the multiplier is the last byte of the added constant
 */

static const ut8 magic_bytes[BRUTE_VERSIONS][17] = {
	{ 0x4f, 0xd0, 0xa0, 0xac, 0x4a, 0x5b, 0xb9, 0xe5, 0x93, 0x79, 0x45, 0xa5, 0xc1, 0xcb, 0x31, 0x93, /* multiplier: */ 0x6d },
	{ 0x4f, 0xd0, 0xa0, 0xac, 0x4a, 0x56, 0xb9, 0xe5, 0x93, 0x79, 0x45, 0xa5, 0xc1, 0xcb, 0x31, 0x93, /* multiplier: */ 0x6d },
	{ 0x4f, 0xd0, 0xa0, 0xac, 0x4a, 0x5b, 0xb9, 0xe5, 0x93, 0x79, 0x45, 0xa5, 0xc1, 0xcb, 0x31, 0x93, /* multiplier: */ 0xad },
	{ 0x4f, 0xd0, 0xa0, 0xac, 0x4a, 0x56, 0xb9, 0xe5, 0x93, 0x79, 0x45, 0xa5, 0xc1, 0xcb, 0x31, 0x93, /* multiplier: */ 0xad },
};

#define CURRENT_SIZE(rg) (					\
		rg->index->entries[rg->index->length - 1]->offset +\
		rg->index->entries[rg->index->length - 1]->size	\
)

#define MULTIPLIER(v) ((ut32) magic_bytes[v][16])

static int __read_internal(RIOGGPack *rg, ut32 read_start, ut8 *buf, int count, st32 prev_obfuscated);
static int __write_internal(RIOGGPack *rg, ut32 write_start, const ut8 *buf, int count, st32 prev_obfuscated);

static char *__fs_io_open(RIOGGPack *rg, const char *path);
static char *__fs_io_read(RIOGGPack *rg, const char *path);
static char *__fs_io_dir(RIOGGPack *rg, const char *path);

static int r_io_ggpack_read_entry(RIOGGPack *rg, ut32 read_start, ut8 *buf, int count, RGGPackIndexEntry *entry, st32 *prev_obfuscated);
static int r_io_ggpack_write_entry(RIOGGPack *rg, ut32 write_start, const ut8 *buf, int count, RGGPackIndexEntry *entry, st32 *prev_obfuscated);
static bool r_io_ggpack_resize_entry(RIOGGPack *rg, RIO *io, RIODesc *fd, st64 delta, int i);
static void r_io_ggpack_rebuild_flags(RIOGGPack *rg, RIO *io);
static bool r_ggpack_index_resize_entry_at(RIOGGPack *rg, int i, ut64 at, st64 delta);
static bool r_io_ggpack_create_index(RIOGGPack *rg);

static RGGPackIndexEntry *r_ggpack_entry_new(char *name, ut32 offset, ut32 size);
static void r_ggpack_entry_free(RGGPackIndexEntry *e);

static RGGPackIndex *r_ggpack_index_new(RList *entry_list);
static void r_ggpack_index_free(RGGPackIndex *index);
static int r_ggpack_index_search(RGGPackIndex *index, ut32 offset);

static void gg_deobfuscate(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer, ut32 key_offset,
			   ut32 key_size, ut32 buf_offset, ut32 buf_size, bool has_pass2);
static void gg_deobfuscate_twp(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer, ut32 key_offset,
			   ut32 key_size, ut32 buf_offset, ut32 buf_size, bool has_pass2);
static void gg_deobfuscate_rtmi(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer, ut32 key_offset,
			   ut32 key_size, ut32 buf_offset, ut32 buf_size, bool has_pass2);

static void gg_obfuscate(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer, ut32 key_offset,
			 ut32 key_size, ut32 buf_offset, ut32 buf_size, bool has_pass2);
static void gg_obfuscate_twp(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer, ut32 key_offset,
			 ut32 key_size, ut32 buf_offset, ut32 buf_size, bool has_pass2);

static void gg_deobfuscate_pass2_twp(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer, ut32 key_offset,
				 ut32 key_size, ut32 buf_offset, ut32 buf_size, bool skip_first);
static void gg_deobfuscate_pass1_twp(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer,
				 ut32 key_offset, ut32 key_size, ut32 buf_offset, ut32 buf_size);
static void gg_obfuscate_pass1_twp(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer,
			       ut32 key_offset, ut32 key_size, ut32 buf_offset, ut32 buf_size);
static void gg_obfuscate_pass2_twp(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer, ut32 key_offset,
			       ut32 key_size, ut32 buf_offset, ut32 buf_size);

static ut8 gg_sample_buffer_twp(RIOGGPack *rg, ut8 *buffer, ut32 index, ut32 displace);

static void r_io_ggpack_dump_index(RIO *io, RIOGGPack *rg);
static void r_io_ggpack_dump_dictionary_at_offset(RIO *io, RIOGGPack *rg);

static void r_dump_gghash_json(RIO *io, GGHashValue *hash);
static void r_dump_ggarray_json(RIO *io, GGArrayValue *array);

static GGHashValue *r_ggpack_index_to_hash(RGGPackIndex *index);
static void r_ggpack_rebuild_index(RIOGGPack *rg);

static int entries_sort_func(const void *a, const void *b);
static ut32 fread_le32(FILE *f);
static const char *r_ggpack_base_name(const char *path);

static RIOGGPack *r_io_ggpack_new(void) {
	RIOGGPack *rg = R_NEW0 (RIOGGPack);
	if (!rg) {
		return NULL;
	}

	rg->index = NULL;
	rg->file_name = NULL;
	rg->file = NULL;
	rg->type = R_GGPACK_TYPE_TWP;
	rg->version = 0;
	rg->wait_for_shift_and_rebuild_index = false;
	rg->shifting_index = false;

	return rg;
}

static void r_io_ggpack_free(RIOGGPack *rg) {
	if (!rg) {
		return;
	}

	if (rg->file) {
		fclose (rg->file);
	}
	if (rg->file_name) {
		R_FREE (rg->file_name);
	}
	if (rg->index) {
		r_ggpack_index_free (rg->index);
	}

	R_FREE (rg);
}

static bool __check(RIO *io, const char *pathname, bool many) {
	return strncmp (pathname, "ggpack://", 9) == 0;
}

static RIODesc *__open(RIO *io, const char *pathname, int rw, int mode) {
	RIOGGPack *rg;

	rg = r_io_ggpack_new ();
	if (!rg) {
		goto error;
	}

	if (!__check (io, pathname, false)) {
		goto error;
	}

	rg->file_name = strdup (pathname + 9);
	rg->file = fopen (rg->file_name, (rw & R_PERM_RW) == R_PERM_RW? "rb+": "rb");
	if (!rg->file) {
		goto error;
	}

	if (!r_io_ggpack_create_index (rg)) {
		goto error;
	}

	RIODesc *desc = r_io_desc_new (io, &r_io_plugin_ggpack, pathname, rw, mode, rg);
	//desc->obsz = 1024 * 16;

	return desc;

error:
	r_io_ggpack_free (rg);

	return NULL;
}

static bool __close(RIODesc *fd) {
	RIOGGPack *rg;

	if (!fd || !fd->data) {
		return false;
	}

	rg = fd->data;

	r_io_ggpack_free (fd->data);
	fd->data = NULL;

	return true;
}

static int __read(RIO *io, RIODesc *fd, ut8 *buf, int count) {
	RIOGGPack *rg;
	if (!fd || !fd->data) {
		return -1;
	}
	rg = fd->data;

	ut32 read_start = io->off;
	return __read_internal (rg, read_start, buf, count, -1);
}

static int __read_internal(RIOGGPack *rg, ut32 read_start, ut8 *buf, int count, st32 prev_obfuscated) {
	int i = R_MAX (0, r_ggpack_index_search (rg->index, read_start));

	RGGPackIndexEntry *entry;
	do {
		entry = rg->index->entries[i];
		r_io_ggpack_read_entry (rg, read_start, buf, count, entry, &prev_obfuscated);
		i++;
	} while ((i < rg->index->length) && (entry->offset + entry->size) < (read_start + count));

	return count;
}

static int __write(RIO *io, RIODesc *fd, const ut8 *buf, int count) {
	RIOGGPack *rg;
	if (!fd || !fd->data) {
		return -1;
	}
	rg = fd->data;

	ut32 write_start = io->off;
	return __write_internal (rg, write_start, buf, count, -1);
}

static int __write_internal(RIOGGPack *rg, ut32 write_start, const ut8 *buf, int count, st32 prev_obfuscated) {
	int i = R_MAX (0, r_ggpack_index_search (rg->index, write_start));

	RGGPackIndexEntry *entry;
	do {
		entry = rg->index->entries[i];
		r_io_ggpack_write_entry (rg, write_start, buf, count, entry, &prev_obfuscated);
		i++;
	} while ((i < rg->index->length) && (entry->offset + entry->size) < (write_start + count));

	return count;
}

static ut64 __lseek(RIO *io, RIODesc *fd, ut64 offset, int whence) {
	RIOGGPack *rg = NULL;
	ut64 r_offset = offset;

	if (!fd || !fd->data) {
		return r_offset;
	}

	rg = fd->data;
	ut32 current_size = CURRENT_SIZE (rg);

	switch (whence) {
	case SEEK_SET:
		r_offset = offset;
		break;
	case SEEK_CUR:
		r_offset = rg->offset + (ut32) offset;
		break;
	case SEEK_END:
		r_offset = current_size + offset;
		break;
	}

	if (r_offset > current_size) {
		r_offset = current_size;
	}

	io->off = rg->offset = r_offset;

	return r_offset;
}

static bool __resize(RIO *io, RIODesc *fd, ut64 count) {
	RIOGGPack *rg;
	if (!fd || !fd->data) {
		return false;
	}
	rg = fd->data;

	if (rg->type == R_GGPACK_TYPE_RTMI) {
		eprintf ("Resize not supported (yet) for RTMI\n");
		return false;
	}

	if (count > 0xffffffff) {
		dbg_log ("can't resize that much\n");
		return false;
	}

	int i = r_ggpack_index_search (rg->index, rg->offset);
	if (i < 1 || i >= rg->index->length - 1) {
		dbg_log ("can't resize here\n");
		return false;
	}

	ut32 old_size = CURRENT_SIZE (rg);
	st64 delta = count - old_size;

	return r_io_ggpack_resize_entry (rg, io, fd, delta, i);
}

static char *__system(RIO *io, RIODesc *fd, const char *command) {
	RIOGGPack *rg;
	const char *value;

	if (!fd || !fd->data) {
		return NULL;
	}

	if (!strcmp (command, "help") || !strcmp (command, "h") || !strcmp (command, "?")) {
		return strdup ("ggpack commands available via =!\n"
			"?                          Show this help\n"
			"pDj                        dump GGDictionary at current offset as json\n"
			"pij                        dump index GGDictionary as json\n"
			"ri                         rebuild index (for debugging purpose)\n"
		);
	}

	rg = fd->data;

	if (!strcmp (command, "pDj")) {
		r_io_ggpack_dump_dictionary_at_offset (io, rg);
		return NULL;
	}

	if (!strcmp (command, "pij")) {
		r_io_ggpack_dump_index (io, rg);
		return NULL;
	}

	if (!strcmp (command, "ri")) {
		r_ggpack_rebuild_index (rg);
		return NULL;
	}

	// fs.io support:
	if (!strncmp (command, "m ", 2)) {
		return __fs_io_open (rg, command + 2);
	}

	if (!strncmp (command, "mg ", 3)) {
		return __fs_io_read (rg, command + 3);
	}

	if (!strncmp (command, "md ", 3)) {
		return __fs_io_dir (rg, command + 3);
	}

	return NULL;
}

static char *__fs_io_open(RIOGGPack *rg, const char *path) {
	const char *base_name = r_ggpack_base_name (path);
	int i;
	for (i = 0; i < rg->index->length; i++) {
		RGGPackIndexEntry *entry = rg->index->entries[i];
		if (!strcmp (entry->file_name, base_name)) {
			return r_str_newf ("%u", entry->size);
		}
	}
	return NULL;
}

static char *__fs_io_read(RIOGGPack *rg, const char *path) {
	const char *base_name = r_ggpack_base_name (path);
	int i;
	for (i = 0; i < rg->index->length; i++) {
		RGGPackIndexEntry *entry = rg->index->entries[i];
		if (!strcmp (entry->file_name, base_name)) {
			ut8 *buf = malloc (entry->size);
			if (!buf) {
				return NULL;
			}

			int real_count;
			if ((real_count = r_io_ggpack_read_entry (rg, entry->offset, buf, entry->size, entry, NULL)) != entry->size) {
				eprintf ("Error reading %s\n", base_name);
				R_FREE (buf);
				return NULL;
			}

			char *hex_encoded = malloc (entry->size * 2);
			if (!hex_encoded) {
				R_FREE (buf);
				return NULL;
			}

			r_hex_bin2str (buf, entry->size, hex_encoded);
			R_FREE (buf);

			return hex_encoded;
		}
	}
	return NULL;
}

static char *__fs_io_dir(RIOGGPack *rg, const char *path) {
	char *result = NULL;

	if (!strcmp (path, "/")) {
		int i;
		for (i = 1; i < rg->index->length - 1; i++) {
			RGGPackIndexEntry *entry = rg->index->entries[i];
			if (!result) {
				result = r_str_newf ("f %s\n", entry->file_name);
			} else {
				r_str_appendf (result, "f %s\n", entry->file_name);
			}
		}
	}

	return result;
}

static const char *r_ggpack_base_name(const char *path) {
	if (!path || !*path) {
		return NULL;
	}
	const char *sep;
	if ((sep = strrchr (path, '/'))) {
		return sep + 1;
	} else {
		return path;
	}
}

static void r_ggpack_rebuild_index(RIOGGPack *rg) {
	dbg_log ("rebuilding index...\n");
	GGHashValue *index_dir = r_ggpack_index_to_hash (rg->index);
	ut8 *new_index_buf;
	ut32 new_index_size;
	if (!gg_hash_serialize (index_dir, &new_index_buf, &new_index_size)) {
		eprintf ("ERROR: could not rebuild index.");
		gg_hash_free (index_dir);
		return;
	}

	ut8 int_buf[4];
	r_write_le32 (&int_buf[0], new_index_size);
	__write_internal (rg, 4, &int_buf[0], 4, -1);

	RGGPackIndexEntry *index_entry = rg->index->entries[rg->index->length - 1];
	index_entry->size = new_index_size;
	rg->index_size = new_index_size;
	__write_internal (rg, rg->index_offset, new_index_buf, rg->index_size, -1);
	r_io_ggpack_write_entry (rg, index_entry->offset, new_index_buf, index_entry->size, index_entry, NULL);

	gg_hash_free (index_dir);
	R_FREE (new_index_buf);
}

static GGHashValue *r_ggpack_index_to_hash(RGGPackIndex *index) {
	int i;
	GGHashValue *index_dir = gg_hash_new (1);
	GGArrayValue *files = gg_array_new (index->length - 2);
	index_dir->pairs[0] = gg_pair_new ("files", (GGValue *) files);

	for (i = 1; i < index->length - 1; i++) {
		GGHashValue *file = gg_hash_new (3);
		RGGPackIndexEntry *entry = index->entries[i];

		GGStringValue *filename = gg_string_new (entry->file_name);
		file->pairs[0] = gg_pair_new ("filename", (GGValue *) filename);

		GGIntValue *offset = gg_int_new (entry->offset);
		file->pairs[1] = gg_pair_new ("offset", (GGValue *) offset);

		GGIntValue *size = gg_int_new (entry->size);
		file->pairs[2] = gg_pair_new ("size", (GGValue *) size);

		files->entries[i - 1] = (GGValue *) file;
	}

	return index_dir;
}

static int r_io_ggpack_read_entry(RIOGGPack *rg, ut32 read_start, ut8 *buf, int count, RGGPackIndexEntry *entry, st32 *prev_obfuscated) {
	ut32 entry_start = entry->offset;
	ut32 entry_end = entry->offset + entry->size;
	ut32 read_end = read_start + count;

	// TODO remove this "inside" shit
	bool inside = entry_start < read_end && entry_end >= read_start;
	if (!inside) {
		return 0;
	}

	ut32 start_gap = (entry_start > read_start)? entry_start - read_start: 0;
	ut32 real_count = R_MIN (entry_end, read_end) - R_MAX (entry_start, read_start);

	if (rg->type == R_GGPACK_TYPE_TWP) {
		if (entry->is_obfuscated) {
			if (read_start > entry_start) {
				ut8 *dbuf = malloc (real_count + 1);
				if (!dbuf) {
					return 0;
				}

				if (!prev_obfuscated || *prev_obfuscated == -1) {
					fseek (rg->file, read_start - 1 + start_gap, SEEK_SET);
					fread (dbuf, 1, real_count + 1, rg->file);
				} else {
					fseek (rg->file, read_start - 1 + start_gap, SEEK_SET);
					st32 x = 0;
					fread (&x, 1, 1, rg->file);
					fseek (rg->file, read_start + start_gap, SEEK_SET);
					fread (dbuf + 1, 1, real_count, rg->file);
					dbuf[0] = *prev_obfuscated & 0xff;
				}
				gg_deobfuscate (rg, buf + start_gap, dbuf, entry_start, entry->size, read_start + start_gap, real_count + 1, entry->has_pass2);
				R_FREE (dbuf);
			} else {
				fseek (rg->file, read_start + start_gap, SEEK_SET);
				fread (buf + start_gap, 1, real_count, rg->file);
				gg_deobfuscate (rg, NULL, buf + start_gap, entry_start, entry->size, read_start + start_gap, real_count, entry->has_pass2);
			}
		} else {
			fseek (rg->file, read_start + start_gap, SEEK_SET);
			fread (buf + start_gap, 1, real_count, rg->file);
		}
	} else {
		fseek (rg->file, read_start + start_gap, SEEK_SET);
		fread (buf + start_gap, 1, real_count, rg->file);
		if (entry->is_obfuscated) {
			gg_deobfuscate (rg, NULL, buf + start_gap, entry_start, entry->size, read_start, real_count, entry->has_pass2);
		}
	}


	if (prev_obfuscated) {
		*prev_obfuscated = -1;
	}
	return real_count;
}

static int r_io_ggpack_write_entry(RIOGGPack *rg, ut32 write_start, const ut8 *buf, int count, RGGPackIndexEntry *entry, st32 *prev_obfuscated) {
	ut32 entry_start = entry->offset;
	ut32 entry_end = entry->offset + entry->size;
	ut32 write_end = write_start + count;
	ut32 file_size = CURRENT_SIZE (rg);
	write_end = R_MIN (file_size, write_end);
	ut32 rest_size = R_MAX (0, (st64) entry_end - write_end);

	// TODO remove this "inside" shit
	bool inside = entry_start < write_end && entry_end >= write_start;
	if (!inside) {
		return 0;
	}

	ut32 start_gap = (entry_start > write_start)? entry_start - write_start: 0;
	ut32 real_count = R_MIN (entry_end, write_end) - R_MAX (entry_start, write_start);
	if (real_count == 0) {
		return 0;
	}

	ut8 *dbuf = NULL, *wbuf;;

	if (rg->type == R_GGPACK_TYPE_TWP) {
		if (entry->is_obfuscated) {
			if (write_start > entry_start) {
				dbuf = malloc (real_count + 1 + rest_size);
				if (!dbuf) {
					return 0;
				}

				wbuf = dbuf + 1;

				if (!prev_obfuscated || *prev_obfuscated == -1) {
					fseek (rg->file, write_start + start_gap - 1, SEEK_SET);
					fread (dbuf, 1, 1, rg->file);
				} else {
					dbuf[0] = *prev_obfuscated & 0xff;
				}

				memcpy (wbuf, buf + start_gap, real_count);

				if (rest_size > 0) {
					__read_internal (rg, write_end, wbuf + real_count, rest_size, -1);
				}

				gg_obfuscate (rg, NULL, dbuf, entry_start, entry->size, write_start + start_gap, real_count + 1 + rest_size, entry->has_pass2);
			} else {
				wbuf = dbuf = malloc (real_count + rest_size);
				if (!dbuf) {
					return 0;
				}

				memcpy (dbuf, buf + start_gap, real_count);

				if (rest_size > 0) {
					__read_internal (rg, write_end, wbuf + real_count, rest_size, -1);
				}

				gg_obfuscate (rg, NULL, dbuf, entry_start, entry->size, write_start + start_gap, real_count + rest_size, entry->has_pass2);
			}
		} else {
			wbuf = (ut8 *) buf + start_gap;
		}
	} else {
		if (entry->is_obfuscated) {
			wbuf = dbuf = malloc (real_count + rest_size);
			if (!dbuf) {
				return 0;
			}
			memcpy (dbuf, buf + start_gap, real_count);
			if (rest_size > 0) {
				__read_internal (rg, write_end, wbuf + real_count, rest_size, -1);
			}
			gg_obfuscate (rg, NULL, dbuf, entry_start, entry->size, write_start, real_count + rest_size, entry->has_pass2);
			write_start -= start_gap;
		} else {
			wbuf = (ut8 *) buf + start_gap;
		}
	}

	if (prev_obfuscated) {
		*prev_obfuscated = -1;
	}

	fseek (rg->file, write_start + start_gap, SEEK_SET);
	fwrite (wbuf, 1, real_count + rest_size, rg->file);

	if (dbuf) {
		R_FREE (dbuf);
	}

	if (entry == rg->index->entries[rg->index->length - 1]) {
		if (rg->wait_for_shift_and_rebuild_index) {
			rg->shifting_index = true;
		}
	} else if (rg->shifting_index) {
		rg->shifting_index = false;
		rg->wait_for_shift_and_rebuild_index = false;
		r_ggpack_rebuild_index (rg);
	}

	return real_count;
}

static bool r_io_ggpack_resize_entry(RIOGGPack *rg, RIO *io, RIODesc *fd, st64 delta, int i) {
	ut64 at = io->off;
	bool result = r_ggpack_index_resize_entry_at (rg, i, at, delta);
	io->off = at;
	r_io_ggpack_rebuild_flags (rg, io);
	return result;
}

static bool r_ggpack_index_resize_entry_at(RIOGGPack *rg, int i, ut64 at, st64 delta) {
	RGGPackIndexEntry *entry = rg->index->entries[i];

	st64 new_size = entry->size + delta;
	if (new_size <= 0) {
		return false;
	}

	int j;
	if (delta >= 0) {
		eprintf ("resizing and shifting up by %u bytes...\n", (ut32) delta);
		j = i;
		st32 saved_last_byte = -1;
		for (; j < rg->index->length; j++) {
			ut32 size = rg->index->entries[j]->size;
			ut32 offset = rg->index->entries[j]->offset;
			bool empty = size == 0;

			if (j != i) {
				offset += delta;
			} else if (!empty) {
				size += delta;
			}

			if (empty) {
				rg->index->entries[j]->offset = offset;
				continue;
			}

			ut8 *buf = malloc (size);
			if (!buf) {
				return false;
			}

			__read_internal (rg, offset, buf, size, saved_last_byte);

			fseek (rg->file, offset + size - 1, SEEK_SET);
			ut8 x;
			fread (&x, 1, 1, rg->file);

			rg->index->entries[j]->offset = offset;
			rg->index->entries[j]->size = size;

			r_io_ggpack_write_entry (rg, offset, buf, size, rg->index->entries[j], &saved_last_byte);
			R_FREE (buf);

			saved_last_byte = x;
		}
	} else {
		eprintf ("resizing down by %u bytes...\n", (ut32) - delta);
		j = rg->index->length - 1;
		for (; j >= i; j--) {
			ut32 size = rg->index->entries[j]->size;
			ut32 offset = rg->index->entries[j]->offset;
			bool empty = size == 0;

			if (j != i) {
				offset += delta;
			} else if (!empty) {
				size += delta;
			}

			if (empty) {
				rg->index->entries[j]->offset = offset;
				continue;
			}

			ut8 *buf = malloc (size);
			if (!buf) {
				return false;
			}

			__read_internal (rg, offset, buf, size, -1);

			rg->index->entries[j]->offset = offset;
			rg->index->entries[j]->size = size;

			r_io_ggpack_write_entry (rg, offset, buf, size, rg->index->entries[j], NULL);
			R_FREE (buf);
		}
	}

	ut8 int_buf[4];
	if (i != rg->index->length - 1) {
		rg->index_offset += delta;
		r_write_le32 (&int_buf[0], rg->index_offset);
		__write_internal (rg, 0, &int_buf[0], 4, -1);
	}

	if (delta < 0) {
		r_ggpack_rebuild_index (rg);
		ut32 new_size = CURRENT_SIZE (rg);
		dbg_log ("truncating file to %u\n", new_size);
		if (ftruncate (fileno (rg->file), new_size) != 0) {
			return false;
		}
	} else if (delta > 0) {
		dbg_log ("waiting for shift...\n");
		rg->wait_for_shift_and_rebuild_index = true;
	}

	rg->offset = at;
	fseek (rg->file, at, SEEK_SET);
	return true;
}

static void r_io_ggpack_rebuild_flags(RIOGGPack *rg, RIO *io) {
	char command[1024];
	char name[1024];
	command[1023] = 0;

	dbg_log ("rebuilding flags...\n");
	int i;
	for (i = 0; i < rg->index->length; i++) {
		RGGPackIndexEntry *entry = rg->index->entries[i];
		strncpy (name, entry->file_name, 1023);
		name[1023] = 0;
		r_name_filter (name, strlen (name));

		RCore *core = (RCore*) io->coreb.core;
		snprintf (command, 1023, "f-sym.%s", name);
		io->coreb.cmd (core, command);

		snprintf (command, 1023, "f sym.%s %u@%u", name, entry->size, entry->offset);
		io->coreb.cmd (core, command);
	}
}

static bool r_io_ggpack_create_index(RIOGGPack *rg) {
	ut8 *index_buffer = NULL;
	RList *entries = NULL;
	GGHashValue *index_dir = NULL;

	fseek (rg->file, 0, SEEK_SET);

	rg->index_offset = fread_le32 (rg->file);
	rg->index_size = fread_le32 (rg->file);

	if (rg->index_size != 0) {
		index_buffer = malloc (rg->index_size);
		if (!index_buffer) {
			goto bad_error;
		}
	}

read_direcory:
	fseek (rg->file, rg->index_offset, SEEK_SET);
	fread (index_buffer, 1, rg->index_size, rg->file);

	gg_deobfuscate (rg, NULL, index_buffer, rg->index_offset, rg->index_size, rg->index_offset, rg->index_size, false);

	index_dir = gg_hash_unserialize (index_buffer, rg->index_size);
	if (!index_dir) {
		goto nice_error;
	}

	GGArrayValue *files = (GGArrayValue *) gg_hash_value_for_key (index_dir, "files");
	if (!files) {
		dbg_log ("missing \"files\" key\n");
		goto nice_error;
	}

	entries = r_list_new ();
	int i = 0;
	bool is_sorted = true;
	ut32 previous_offset = 0;
	RGGPackIndexEntry *previous_entry = NULL;
	for (; i < files->length; i++) {
		char *name;
		ut32 offset = 0, size = 0;
		int j = 0;
		GGHashValue *a_file = (GGHashValue *) files->entries[i];

		for (; j < a_file->n_pairs; j++) {
			GGHashPair *pair = a_file->pairs[j];
			if (!strcmp (pair->key, "filename")) {
				if (pair->value->type == GG_TYPE_STRING) {
					GGStringValue *sv = (GGStringValue *) pair->value;
					if (sv->value) {
						name = strdup (sv->value);
					}
				}
			} else if (!strcmp (pair->key, "offset")) {
				if (pair->value->type == GG_TYPE_INT) {
					offset = ((GGIntValue *) pair->value)->value;
				}
			} else if (!strcmp (pair->key, "size")) {
				if (pair->value->type == GG_TYPE_INT) {
					size = ((GGIntValue *) pair->value)->value;
				}
			}
		}

		if (name == NULL || offset == 0) {
			dbg_log ("incomplete entry %s 0x%x %u\n", name, offset, size);
			goto nice_error;
		}

		if (offset < previous_offset || size == 0) {
			is_sorted = false;
		}
		previous_offset = offset;

		RGGPackIndexEntry *entry = r_ggpack_entry_new (name, offset, size);
		r_list_append (entries, entry);
		previous_entry = entry;
	}

	if (!is_sorted) {
		dbg_log ("sorting index\n");
		r_list_sort (entries, entries_sort_func);
	}

	RGGPackIndexEntry *index_entry = r_ggpack_entry_new ("index_directory", rg->index_offset, rg->index_size);
	r_list_append (entries, index_entry);

	RGGPackIndexEntry *header_entry = r_ggpack_entry_new ("header", 0, 8);
	header_entry->is_obfuscated = false;
	r_list_prepend (entries, header_entry);

	rg->index = r_ggpack_index_new (entries);
	r_list_free (entries);
	R_FREE (index_buffer);
	gg_hash_free (index_dir);

	if (!rg->index) {
		return false;
	}
	return true;

nice_error:
	if (entries) {
		RListIter *iter;
		RGGPackIndexEntry *entry;
		r_list_foreach (entries, iter, entry) {
			r_ggpack_entry_free (entry);
		}
		r_list_free (entries);
		entries = NULL;
	}
	if (index_dir) {
		gg_hash_free (index_dir);
		index_dir = NULL;
	}
	if ((rg->type == R_GGPACK_TYPE_TWP && rg->version < BRUTE_VERSIONS - 1) ||
			(rg->type == R_GGPACK_TYPE_RTMI && rg->version < BRUTE_RTMI_VERSIONS - 1)) {
		rg->version++;
		dbg_log ("retry with type %d version %d\n", rg->type, rg->version);
		goto read_direcory;
	} else if (rg->type < R_GGPACK_TYPE_MAX) {
		rg->version = 0;
		rg->type++;
		dbg_log ("retry with type %d version %d\n", rg->type, rg->version);
		goto read_direcory;
	}

bad_error:
	if (index_buffer) {
		R_FREE (index_buffer);
	}

	return false;
}

static int entries_sort_func(const void *a, const void *b) {
	RGGPackIndexEntry *A = (RGGPackIndexEntry *) a;
	RGGPackIndexEntry *B = (RGGPackIndexEntry *) b;
	int offset_compare = A->offset - B->offset;
	if (offset_compare == 0) {
		return A->size - B->size;
	}
	return offset_compare;
}

static void r_io_ggpack_dump_index(RIO *io, RIOGGPack *rg) {
	ut8 *index_buf = malloc (rg->index_size);
	if (!index_buf) {
		return;
	}

	__read_internal (rg, rg->index_offset, index_buf, rg->index_size, -1);
	GGHashValue *index_dir = gg_hash_unserialize (index_buf, rg->index_size);
	if (!index_dir) {
		R_FREE (index_buf);
		return;
	}

	r_dump_gghash_json (io, index_dir);

	gg_hash_free (index_dir);
	R_FREE (index_buf);
}

static void r_io_ggpack_dump_dictionary_at_offset(RIO *io, RIOGGPack *rg) {
	int i = r_ggpack_index_search (rg->index, rg->offset);
	if (i == -1) {
		eprintf ("offset out of range\n");
		return;
	}

	RGGPackIndexEntry *entry = rg->index->entries[i];
	ut32 size = entry->offset + entry->size - rg->offset;
	ut8 *buf = malloc (size);
	if (!buf) {
		return;
	}

	__read_internal (rg, rg->offset, buf, size, -1);
	GGHashValue *hash = gg_hash_unserialize (buf, size);
	if (!hash) {
		R_FREE (buf);
		eprintf ("cannot parse dictionary at 0x%x\n", rg->offset);
		return;
	}

	r_dump_gghash_json (io, hash);

	gg_hash_free (hash);
	R_FREE (buf);
}

static void r_dump_gghash_json(RIO *io, GGHashValue *hash) {
	RListIter *iter;
	GGHashPair *pair;
	int i = 0;

	io->cb_printf ("{");
	for (i = 0; i < hash->n_pairs; i++) {
		GGHashPair *pair = hash->pairs[i];
		io->cb_printf ("\"%s\": ", pair->key);
		switch (pair->value->type) {
		case GG_TYPE_NULL:
			io->cb_printf ("null");
			break;
		case GG_TYPE_STRING:
			io->cb_printf ("\"%s\"", ((GGStringValue *) pair->value)->value);
			break;
		case GG_TYPE_INT:
			io->cb_printf ("%u", ((GGIntValue *) pair->value)->value);
			break;
		case GG_TYPE_DOUBLE:
			io->cb_printf ("%lf", ((GGDoubleValue *) pair->value)->value);
			break;
		case GG_TYPE_HASH:
			r_dump_gghash_json (io, (GGHashValue *) pair->value);
			break;
		case GG_TYPE_ARRAY:
			r_dump_ggarray_json (io, (GGArrayValue *) pair->value);
			break;
		default:
			io->cb_printf ("\"UNSUPPORTED TYPE %d\"", pair->value->type);
		}

		if (i < hash->n_pairs - 1) {
			io->cb_printf (", ");
		}
	}
	io->cb_printf ("}\n");
}

static void r_dump_ggarray_json(RIO *io, GGArrayValue *array) {
	RListIter *iter;
	GGHashPair *pair;
	int i = 0;

	io->cb_printf ("[");
	for (i = 0; i < array->length; i++) {
		GGValue *value = array->entries[i];
		switch (value->type) {
		case GG_TYPE_NULL:
			io->cb_printf ("null");
			break;
		case GG_TYPE_STRING:
			io->cb_printf ("\"%s\"", ((GGStringValue *) value)->value);
			break;
		case GG_TYPE_INT:
			io->cb_printf ("%u", ((GGIntValue *) value)->value);
			break;
		case GG_TYPE_DOUBLE:
			io->cb_printf ("%lf", ((GGDoubleValue *) value)->value);
			break;
		case GG_TYPE_HASH:
			r_dump_gghash_json (io, (GGHashValue *) value);
			break;
		case GG_TYPE_ARRAY:
			r_dump_ggarray_json (io, (GGArrayValue *) value);
			break;
		default:
			io->cb_printf ("\"UNSUPPORTED TYPE %d\"", pair->value->type);
		}

		if (i < array->length - 1) {
			io->cb_printf (", ");
		}
	}
	io->cb_printf ("]");
}

static RGGPackIndexEntry *r_ggpack_entry_new(char *name, ut32 offset, ut32 size) {
	RGGPackIndexEntry *e = R_NEW0 (RGGPackIndexEntry);
	if (!e) {
		return NULL;
	}

	e->is_obfuscated = true;
	if (name) {
		e->file_name = strdup (name);
		if (strstr (name, ".bnut")) {
			e->has_pass2 = true;
			e->has_printable_content = true;
		}
		if (strstr (name, ".bank")) {
			e->is_obfuscated = false;
		}
	}
	e->offset = offset;
	e->size = size;

	return e;
}

static void r_ggpack_entry_free(RGGPackIndexEntry *e) {
	if (!e) {
		return;
	}

	if (e->file_name) {
		R_FREE (e->file_name);
	}

	R_FREE (e);
}

static RGGPackIndex *r_ggpack_index_new(RList *entry_list) {
	RGGPackIndex *index = NULL;

	if (!entry_list) {
		goto error;
	}

	index = R_NEW0 (RGGPackIndex);
	if (!index) {
		goto error;
	}
	index->length = 0;
	index->entries = NULL;

	RListIter *iter;
	RGGPackIndexEntry *entry;
	int length = r_list_length (entry_list);

	if (length == 0) {
		goto error;
	}

	index->entries = (RGGPackIndexEntry **) malloc (length * sizeof (void *));
	if (!index->entries) {
		goto error;
	}

	r_list_foreach (entry_list, iter, entry) {
		bool is_valid = !entry->is_obfuscated || (entry->offset != 0 /*&& entry->size != 0*/);
		if (!is_valid) {
			dbg_log ("incomplete entry: '%s' 0x%x %u\n", entry->file_name, entry->offset, entry->size);
			goto error;
		}
		index->entries[index->length++] = entry;
	}

	return index;

error:
	if (index) {
		r_ggpack_index_free (index);
	}

	return NULL;
}

static void r_ggpack_index_free(RGGPackIndex *index) {
	if (!index) {
		return;
	}

	if (index->entries) {
		int i;
		for (i = 0; i < index->length; i++) {
			r_ggpack_entry_free (index->entries[i]);
		}
	}

	R_FREE (index);
}

static int r_ggpack_index_search(RGGPackIndex *index, ut32 offset) {
	int imid;
	int imin = 0;
	int imax = index->length - 1;

	while (imin < imax) {
		imid = (imin + imax) / 2;
		RGGPackIndexEntry *entry = index->entries[imid];
		if ((entry->offset + entry->size) <= offset || (entry->offset == offset && entry->size == 0)) {
			imin = imid + 1;
		} else {
			imax = imid;
		}
	}

	RGGPackIndexEntry *minEntry = index->entries[imin];
	if ((imax == imin) && (minEntry->offset <= offset) && ((minEntry->offset + minEntry->size) > offset)) {
		return imin;
	}
	return -1;
}

static void gg_deobfuscate(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer, ut32 key_offset,
			   ut32 key_size, ut32 buf_offset, ut32 buf_size, bool has_pass2) {
	if (rg->type == R_GGPACK_TYPE_TWP)
		gg_deobfuscate_twp (rg, out_buffer, buffer, key_offset, key_size, buf_offset, buf_size, has_pass2);
	else if (rg->type == R_GGPACK_TYPE_RTMI)
		gg_deobfuscate_rtmi (rg, out_buffer, buffer, key_offset, key_size, buf_offset, buf_size, has_pass2);
	else
		dbg_log ("unsupported ggpack type\n");
}

static void gg_deobfuscate_rtmi(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer, ut32 key_offset,
			   ut32 key_size, ut32 buf_offset, ut32 buf_size, bool has_pass2) {
	st64 displace = (st64) buf_offset - (st64) key_offset;
	if (!out_buffer) {
		out_buffer = buffer;
	}
	ut32 previous = (key_size + 0x78) & 0xffff;

	ut32 i;
	if (displace > 0) {
		for (i = 0; i < displace /* - 1 */; i++) {
			previous += rtmi_magic_bytes_short[rg->version][previous & 0xff];
		}
	}
	for (i = 0; i < buf_size; i++) {
		out_buffer[i] =
			buffer[i] ^
			rtmi_magic_bytes_long[rg->version][previous & 0xffff] ^
			rtmi_magic_bytes_short[rg->version][(previous + 0x78) & 0xff];
		previous += rtmi_magic_bytes_short[rg->version][previous & 0xff];
	}
}

static void gg_deobfuscate_twp(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer, ut32 key_offset,
			   ut32 key_size, ut32 buf_offset, ut32 buf_size, bool has_pass2) {
	gg_deobfuscate_pass1_twp (rg, out_buffer, buffer, key_offset, key_size, buf_offset, buf_size);

	if (has_pass2) {
		ut8 *middle_buffer = out_buffer;
		bool skip_first = buf_offset != key_offset;
		if (!middle_buffer) {
			middle_buffer = buffer;
		} else {
			skip_first = false;
			buf_size --;
		}

		gg_deobfuscate_pass2_twp (rg, NULL, middle_buffer, key_offset, key_size, buf_offset, buf_size, skip_first);
	}
}

static void gg_deobfuscate_pass1_twp(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer,
				 ut32 key_offset, ut32 key_size, ut32 buf_offset, ut32 buf_size) {
	ut8 previous = key_size & 0xff;
	ut32 i = 0;
	ut32 out_offset = 0;
	ut32 displace = buf_offset - key_offset;
	if (!out_buffer) {
		out_buffer = buffer;
	}

	if (displace) {
		previous = gg_sample_buffer_twp (rg, buffer, i++, displace - 1);
		displace--;
		if (out_buffer != buffer) {
			out_offset = 1;
		}
	}

	for (; i < buf_size; i++) {
		ut8 x = gg_sample_buffer_twp (rg, buffer, i, displace);
		out_buffer[i - out_offset] = x ^ previous;
		previous = x;
	}
}

static void gg_deobfuscate_pass2_twp(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer, ut32 key_offset,
				 ut32 key_size, ut32 buf_offset, ut32 buf_size, bool skip_first) {
	int cursor = key_size & 0xff;
	ut32 displace = buf_offset - key_offset;
	int i = 0;
	ut32 out_offset = 0;

	if (!out_buffer) {
		out_buffer = buffer;
	}

	if (skip_first) {
		i++;
		if (out_buffer != buffer) {
			out_offset = 1;
		}
	}

	cursor = (cursor + displace) % R_GGPACK_PASS2_WIDTH;

	for (; i < buf_size; i++) {
		out_buffer[i - out_offset] = buffer[i] ^ r_ggpack_pass2[0][cursor];
		cursor = (cursor + 1) % R_GGPACK_PASS2_WIDTH;
	}
}

static void gg_obfuscate(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer, ut32 key_offset,
			 ut32 key_size, ut32 buf_offset, ut32 buf_size, bool has_pass2) {
	if (rg->type == R_GGPACK_TYPE_TWP)
		gg_obfuscate_twp (rg, out_buffer, buffer, key_offset, key_size, buf_offset, buf_size, has_pass2);
	else if (rg->type == R_GGPACK_TYPE_RTMI)
		gg_deobfuscate_rtmi (rg, out_buffer, buffer, key_offset, key_size, buf_offset, buf_size, has_pass2);
	else
		dbg_log ("unsupported ggpack type\n");
}

static void gg_obfuscate_twp(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer, ut32 key_offset,
			 ut32 key_size, ut32 buf_offset, ut32 buf_size, bool has_pass2) {
	if (has_pass2) {
		gg_obfuscate_pass2_twp (rg, out_buffer, buffer, key_offset, key_size, buf_offset, buf_size);

		ut8 *middle_buffer = out_buffer;
		if (!middle_buffer) {
			middle_buffer = buffer;
		}

		gg_obfuscate_pass1_twp (rg, NULL, middle_buffer, key_offset, key_size, buf_offset, buf_size);
	} else {
		gg_obfuscate_pass1_twp (rg, out_buffer, buffer, key_offset, key_size, buf_offset, buf_size);
	}
}

static void gg_obfuscate_pass1_twp(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer,
			       ut32 key_offset, ut32 key_size, ut32 buf_offset, ut32 buf_size) {
	ut8 previous = key_size & 0xff;
	ut32 i = 0;
	ut32 out_offset = 0;
	ut32 displace = buf_offset - key_offset;
	if (!out_buffer) {
		out_buffer = buffer;
	}

	if (displace) {
		previous = gg_sample_buffer_twp (rg, buffer, i++, displace - 1);
		displace--;
		if (out_buffer != buffer) {
			out_offset = 1;
		}
	}

	for (; i < buf_size; i++) {
		ut8 x = buffer[i] ^ previous;
		out_buffer[i - out_offset] = x ^ magic_bytes[rg->version][(i + displace) & 0xf] ^ ((i + displace) * MULTIPLIER (rg->version));
		previous = x;
	}
}

static ut8 gg_sample_buffer_twp(RIOGGPack *rg, ut8 *buffer, ut32 index, ut32 displace) {
	return buffer[index] ^ magic_bytes[rg->version][(index + displace) & 0xf] ^ ((index + displace) * MULTIPLIER (rg->version));
}

static void gg_obfuscate_pass2_twp(RIOGGPack *rg, ut8 *out_buffer, ut8 *buffer, ut32 key_offset,
			       ut32 key_size, ut32 buf_offset, ut32 buf_size) {
	int cursor = key_size & 0xff;
	ut32 displace = buf_offset - key_offset;
	int i = 0;
	ut32 out_offset = 0;

	if (!out_buffer) {
		out_buffer = buffer;
	}

	if (displace) {
		i++;
		if (out_buffer != buffer) {
			out_offset = 1;
		}
		cursor = (cursor + displace) % R_GGPACK_PASS2_WIDTH;
	}

	for (; i < buf_size; i++) {
		out_buffer[i - out_offset] = buffer[i] ^ r_ggpack_pass2[0][cursor];
		cursor = (cursor + 1) % R_GGPACK_PASS2_WIDTH;
	}
}

static ut32 fread_le32(FILE *f) {
	void *buf[4];
	fread (buf, 1, 4, f);
	return r_read_le32 (buf);
}

RIOPlugin r_io_plugin_ggpack = {
	.name = "ggpack",
	.desc = "ggpack:// io plugin",
	.license = "MIT",
	.open = __open,
	.close = __close,
	.read = __read,
	.check = __check,
	.seek = __lseek,
	.write = __write,
	.resize = __resize,
	.system = __system,
};

#ifndef CORELIB
RLibStruct radare_plugin = {
	.type = R_LIB_TYPE_IO,
	.data = &r_io_plugin_ggpack,
	.version = R2_VERSION
};
#endif
