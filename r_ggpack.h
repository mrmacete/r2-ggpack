/* MIT - Copyright 2017-2022 - mrmacete */

#ifndef R_GGPACK
#define R_GGPACK

#define R_GGPACK_TYPE_TWP 0
#define R_GGPACK_TYPE_RTMI 1

#define R_GGPACK_TYPE_MAX 1

typedef struct {
	char *file_name;
	ut32 offset;
	ut32 size;
	bool is_obfuscated;
	bool has_pass2;
	bool has_printable_content;
} RGGPackIndexEntry;

typedef struct {
	RGGPackIndexEntry **entries;
	ut32 length;
} RGGPackIndex;

typedef struct {
	RGGPackIndex *index;
	char *file_name;
	FILE *file;
	ut32 version;
	ut32 index_offset;
	ut32 index_size;
	ut32 offset;
	bool wait_for_shift_and_rebuild_index;
	bool shifting_index;
    int type;
} RIOGGPack;

#endif
