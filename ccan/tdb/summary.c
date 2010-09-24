 /* 
   Trivial Database: human-readable summary code
   Copyright (C) Rusty Russell 2010
   
   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#include "tdb_private.h"
#include <ccan/tally/tally.h>

#define SUMMARY_FORMAT \
	"Size of file: %zu\n" \
	"Number of records: %zu\n" \
	"Smallest/average/largest records: %zu/%zu/%zu\n%s" \
	"Smallest/average/largest padding: %zu/%zu/%zu\n%s" \
	"Number of dead records: %zu\n" \
	"Smallest/average/largest dead records: %zu/%zu/%zu\n%s" \
	"Number of free records: %zu\n" \
	"Smallest/average/largest free records: %zu/%zu/%zu\n%s" \
	"Number of hash chains: %zu\n" \
	"Smallest/average/largest hash chains: %zu/%zu/%zu\n%s" \
	"Total data = %zu (%.0f%%)\n"

#define HISTO_WIDTH 70
#define HISTO_HEIGHT 20

/* Slow, but should be very rare. */
static size_t dead_space(struct tdb_context *tdb, tdb_off_t off)
{
	size_t len;

	for (len = 0; off + len < tdb->map_size; len++) {
		char c;
		if (tdb->methods->tdb_read(tdb, off, &c, 1, 0))
			return 0;
		if (c != 0 && c != 0x42)
			break;
	}
	return len;
}

static size_t get_hash_length(struct tdb_context *tdb, unsigned int i)
{
	tdb_off_t rec_ptr;
	size_t count = 0;

	if (tdb_ofs_read(tdb, TDB_HASH_TOP(i), &rec_ptr) == -1)
		return 0;

	/* keep looking until we find the right record */
	while (rec_ptr) {
		struct tdb_record r;
		++count;
		if (tdb_rec_read(tdb, rec_ptr, &r) == -1)
			return 0;
		rec_ptr = r.next;
	}
	return count;
}

char *tdb_summary(struct tdb_context *tdb, enum tdb_summary_flags flags)
{
	tdb_off_t off;
	struct tally *freet, *used, *dead, *extra, *hash;
	char *freeg, *usedg, *deadg, *extrag, *hashg;
	struct tdb_record rec;
	char *ret = NULL;
	bool locked;
	size_t len;

	/* Read-only databases use no locking at all: it's best-effort.
	 * We may have a write lock already, so skip that case too. */
	if (tdb->read_only || tdb->allrecord_lock.count != 0) {
		locked = false;
	} else {
		if (tdb_lockall_read(tdb) == -1)
			return NULL;
		locked = true;
	}

	freet = tally_new(100);
	used = tally_new(100);
	dead = tally_new(100);
	extra = tally_new(100);
	hash = tally_new(100);
	if (!freet || !used || !dead || !extra || !hash) {
		tdb->ecode = TDB_ERR_OOM;
		goto unlock;
	}

	for (off = TDB_DATA_START(tdb->header.hash_size);
	     off < tdb->map_size - 1;
	     off += sizeof(rec) + rec.rec_len) {
		if (tdb->methods->tdb_read(tdb, off, &rec, sizeof(rec),
					   DOCONV()) == -1)
			goto unlock;
		switch (rec.magic) {
		case TDB_MAGIC:
			tally_add(used, rec.key_len + rec.data_len);
			tally_add(extra, rec.rec_len - (rec.key_len
							+ rec.data_len));
			break;
		case TDB_FREE_MAGIC:
			tally_add(freet, sizeof(rec) + rec.rec_len);
			break;
		/* If we crash after ftruncate, we can get zeroes or fill. */
		case TDB_RECOVERY_INVALID_MAGIC:
		case 0x42424242:
			rec.rec_len = dead_space(tdb, off) - sizeof(rec);
			/* Fall through */
		case TDB_DEAD_MAGIC:
			tally_add(dead, sizeof(rec) + rec.rec_len);
			break;
		default:
			TDB_LOG((tdb, TDB_DEBUG_ERROR,
				 "Unexpected record magic 0x%x at offset %d\n",
				 rec.magic, off));
			goto unlock;
		}
	}

	for (off = 0; off < tdb->header.hash_size; off++)
		tally_add(hash, get_hash_length(tdb, off));

	if (flags & TDB_SUMMARY_HISTOGRAMS) {
		freeg = tally_histogram(freet, HISTO_WIDTH, HISTO_HEIGHT);
		usedg = tally_histogram(used, HISTO_WIDTH, HISTO_HEIGHT);
		deadg = tally_histogram(dead, HISTO_WIDTH, HISTO_HEIGHT);
		extrag = tally_histogram(extra, HISTO_WIDTH, HISTO_HEIGHT);
		hashg = tally_histogram(hash, HISTO_WIDTH, HISTO_HEIGHT);
	} else {
		freeg = usedg = deadg = extrag = hashg = NULL;
	}

	/* 20 is max length of a %zu. */
	len = strlen(SUMMARY_FORMAT) + 22*20 + 1
		+ (freeg ? strlen(freeg) : 0)
		+ (usedg ? strlen(usedg) : 0)
		+ (deadg ? strlen(deadg) : 0)
		+ (extrag ? strlen(extrag) : 0)
		+ (hashg ? strlen(hashg) : 0);
	ret = malloc(len);
	if (!ret)
		goto unlock;

	sprintf(ret, SUMMARY_FORMAT,
		tdb->map_size,
		tally_num(used),
		tally_min(used), tally_mean(used), tally_max(used),
		usedg ? usedg : "",
		tally_min(extra), tally_mean(extra), tally_max(extra),
		extrag ? extrag : "",
		tally_num(dead),
		tally_min(dead), tally_mean(dead), tally_max(dead),
		deadg ? deadg : "",
		tally_num(freet),
		tally_min(freet), tally_mean(freet), tally_max(freet),
		freeg ? freeg : "",
		tally_num(hash),
		tally_min(hash), tally_mean(hash), tally_max(hash),
		hashg ? hashg : "",
		tally_total(used, NULL),
		tally_total(used, NULL) * 100.0 / tdb->map_size);

unlock:
	free(freeg);
	free(usedg);
	free(deadg);
	free(extrag);
	free(hashg);
	free(freet);
	free(used);
	free(dead);
	free(extra);
	free(hash);
	if (locked) {
		tdb_unlockall_read(tdb);
	}
	return ret;
}