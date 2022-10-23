/* MIT - Copyright 2017 - mrmacete */

#include <r_core.h>
#include <r_bin.h>
#include <r_lib.h>
#include <r_io.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include "r_ggpack.h"

static bool __check_buffer(RBinFile *bf, RBuffer *b) {
	ut8 tmp[4];
	if (r_buf_read_at (b, 0, tmp, 4) != 4) {
		return false;
	}
	ut32 index_offset = r_read_le32 (tmp);
	if (index_offset >= r_buf_size (b)) {
		return false;
	}
	if (r_buf_read_at (b, index_offset, tmp, 4) != 4) {
		return false;
	}
	ut32 index_magic = r_read_be32 (tmp);
	if (index_magic != 0x01020304) {
		return false;
	}
	if (r_buf_read_at (b, index_offset + 8, tmp, 4) != 4) {
		return false;
	}
	ut32 plo = r_read_le32 (tmp);
	if (r_buf_read_at (b, index_offset + plo, tmp, 1) != 1) {
		return false;
	}
	if (tmp[0] != 7) {
		return false;
	}

	return true;
}

static bool __load_buffer(RBinFile *bf, void **bin_obj, RBuffer *buf, ut64 loadaddr, Sdb *sdb) {
	return true;
}

static RBinInfo *__info(RBinFile *arch) {
	RBinInfo *ret = R_NEW0 (RBinInfo);
	if (!ret || !arch || !arch->buf) {
		free (ret);
		return NULL;
	}
	ret->file = strdup (arch->file);
	ret->type = strdup ("ggpack");
	ret->has_va = 0;
	return ret;
}

static RList *__symbols(RBinFile *arch) {
	RList *result = r_list_newf ((RListFree)free);
	if (!result) {
		return NULL;
	}

	int i;
	RIO * io = arch->rbin->iob.io;
	RIOGGPack *rg = io->desc->data;

	for (i = 0; i < rg->index->length; i++) {
		RGGPackIndexEntry * entry = rg->index->entries[i];

		RBinSymbol *sym = R_NEW0 (RBinSymbol);
		if (!sym) {
			r_list_free (result);
			return NULL;
		}

		sym->name = strdup (entry->file_name);
		sym->paddr = sym->vaddr = entry->offset;
		sym->size = entry->size;
		sym->ordinal = 0;

		r_list_append (result, sym);
	}

	return result;
}

static RList *__strings(RBinFile *arch) {
	return NULL;
}

RBinPlugin r_bin_plugin_ggpack = {
	.name = "ggpack",
	.desc = "ggpack bin goodies",
	.license = "MIT",
    .load_buffer = &__load_buffer,
	.symbols = &__symbols,
	.strings = &__strings,
	.check_buffer = &__check_buffer,
	.info = &__info,
};

#ifndef CORELIB
RLibStruct radare_plugin = {
	.type = R_LIB_TYPE_BIN,
	.data = &r_bin_plugin_ggpack,
	.version = R2_VERSION
};
#endif

