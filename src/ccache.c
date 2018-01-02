/*
 * ccache.c
 *
 * Columnar cache implementation of PG-Strom
 * ----
 * Copyright 2011-2018 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2018 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "pg_strom.h"

#define CCACHE_MAX_NUM_DATABASES	100
#define CCBUILDER_STATE__SHUTDOWN	0
#define CCBUILDER_STATE__STARTUP	1
#define CCBUILDER_STATE__LOADING	3
#define CCBUILDER_STATE__SLEEP		4
typedef struct
{
	char		dbname[NAMEDATALEN];
	bool		invalid_database;
	pg_atomic_uint64 curr_scan_pos;
} ccacheDatabase;

typedef struct
{
	int			builder_id;
	Oid			database_oid;
	Oid			table_oid;
	BlockNumber	block_nr;
	int			state;		/* one of CCBUILDER_STATE__* */
	Latch	   *latch;
} ccacheBuilder;

typedef struct
{
	pg_atomic_uint32 builder_log_output;
	/* management of ccache chunks */
	size_t			ccache_usage;
	slock_t			chunks_lock;
	dlist_head		lru_misshit_list;
	dlist_head		lru_active_list;
	dlist_head		free_chunks_list;
	dlist_head	   *active_slots;
	/* management of ccache builder workers */
	pg_atomic_uint32 generation;
	slock_t			lock;
	int				rr_count;
	int				num_databases;
	ccacheDatabase	databases[CCACHE_MAX_NUM_DATABASES];
	ccacheBuilder	builders[FLEXIBLE_ARRAY_MEMBER];
} ccacheState;

#define BUILDER_LOG \
	(ccache_state && pg_atomic_read_u32(&ccache_state->builder_log_output) > 0 ? LOG : DEBUG2)

/*
 * ccacheChunk
 */
struct ccacheChunk
{
	dlist_node	lru_chain;		/* link to LRU list */
	dlist_node	hash_chain;		/* link to Hash list */
	pg_crc32	hash;			/* hash value */
	Oid			database_oid;	/* OID of the cached database */
	Oid			table_oid;		/* OID of the cached table */
	BlockNumber	block_nr;		/* block number where is head of the chunk */
	size_t		length;			/* length of the ccache file */
	cl_uint		nitems;			/* number of valid rows cached */
	cl_int		nattrs;			/* number of regular columns. KDS on ccache
								 * file has more rows for system columns */
	cl_int		refcnt;			/* reference counter */
	TimestampTz	ctime;			/* timestamp of the cache creation.
								 * may be zero, if not constructed yet. */
	TimestampTz	atime;			/* time of the latest access */
};
typedef struct ccacheChunk		ccacheChunk;

#define CCACHE_CTIME_NOT_BUILD		(0)
#define CCACHE_CTIME_IN_PROGRESS	(DT_NOEND)
#define CCACHE_CTIME_IS_READY(ctime)			\
	((ctime) != CCACHE_CTIME_NOT_BUILD && (ctime) != CCACHE_CTIME_IN_PROGRESS)

/* static variables */
static shmem_startup_hook_type shmem_startup_next = NULL;
static char		   *ccache_startup_databases;	/* GUC */
static int			ccache_num_builders;		/* GUC */
static bool			__ccache_log_output;		/* GUC */
static size_t		ccache_total_size;			/* GUC */
static char		   *ccache_base_dir_name;		/* GUC */
static DIR		   *ccache_base_dir = NULL;
static ccacheState *ccache_state = NULL;		/* shmem */
static cl_int		ccache_num_chunks;
static cl_int		ccache_num_slots;
static ccacheDatabase *ccache_database = NULL;	/* only builder */
static ccacheBuilder  *ccache_builder = NULL;	/* only builder */
static uint32		ccache_builder_generation = UINT_MAX;	/* only builder */
static bool			ccache_builder_got_sigterm = false;
static HTAB		   *ccache_relations_htab = NULL;
static oidvector   *ccache_relations_oid = NULL;
static Oid			ccache_invalidator_func_oid = InvalidOid;

extern void ccache_builder_main(Datum arg);

/*
 * ccache_compute_hashvalue
 */
static inline pg_crc32
ccache_compute_hashvalue(Oid database_oid, Oid table_oid, BlockNumber block_nr)
{
	pg_crc32	hash;

	Assert((block_nr & (CCACHE_CHUNK_NBLOCKS - 1)) == 0);
	INIT_LEGACY_CRC32(hash);
	COMP_LEGACY_CRC32(hash, &database_oid, sizeof(Oid));
	COMP_LEGACY_CRC32(hash, &table_oid, sizeof(Oid));
	COMP_LEGACY_CRC32(hash, &block_nr, sizeof(BlockNumber));
	FIN_LEGACY_CRC32(hash);

	return hash;
}

/*
 * ccache_chunk_filename
 */
static inline void
ccache_chunk_filename(char *fname,
					  Oid database_oid, Oid table_oid, BlockNumber block_nr)
{
	Assert((block_nr & (CCACHE_CHUNK_NBLOCKS - 1)) == 0);

	snprintf(fname, MAXPGPATH, "CC%u_%u:%ld.dat",
			 database_oid, table_oid,
			 block_nr / CCACHE_CHUNK_NBLOCKS);
}

/*
 * ccache_check_filename
 */
static bool
ccache_check_filename(const char *fname,
					  Oid *p_database_oid, Oid *p_table_oid,
					  BlockNumber *p_block_nr)
{
	const char *pos = fname;
	cl_long		database_oid = -1;
	cl_long		table_oid = -1;
	cl_long		block_nr = -1;

	if (fname[0] != 'C' || fname[1] != 'C')
		return false;
	pos = fname + 2;
	while (isdigit(*pos))
	{
		if (database_oid < 0)
			database_oid = (*pos - '0');
		else
			database_oid = 10 * database_oid + (*pos - '0');
		if (database_oid > OID_MAX)
			return false;
		pos++;
	}
	if (database_oid < 0 || *pos++ != '_')
		return false;
	while (isdigit(*pos))
	{
		if (table_oid < 0)
			table_oid = (*pos - '0');
		else
			table_oid = 10 * table_oid + (*pos - '0');
		if (table_oid > OID_MAX)
			return false;
		pos++;
	}
	if (table_oid < 0 || *pos++ != ':')
		return false;
	while (isdigit(*pos))
	{
		if (block_nr < 0)
			block_nr = (*pos - '0');
		else
			block_nr = 10 * block_nr + (*pos - '0');
		if (block_nr > MaxBlockNumber)
			return false;
		pos++;
	}
	if (block_nr < 0 || strcmp(pos, ".dat") != 0)
		return false;

	*p_database_oid	= database_oid;
	*p_table_oid	= table_oid;
	*p_block_nr		= block_nr;

	return true;
}

/*
 * ccache_put_chunk
 */
static void
ccache_put_chunk_nolock(ccacheChunk *cc_chunk)
{
	Assert(cc_chunk->refcnt > 0);
	if (--cc_chunk->refcnt == 0)
	{
		char		fname[MAXPGPATH];

		Assert(cc_chunk->hash_chain.prev == NULL &&
			   cc_chunk->hash_chain.next == NULL);
		if (cc_chunk->lru_chain.prev != NULL ||
			cc_chunk->lru_chain.next != NULL)
			dlist_delete(&cc_chunk->lru_chain);
		if (cc_chunk->ctime == CCACHE_CTIME_NOT_BUILD ||
			cc_chunk->ctime == CCACHE_CTIME_IN_PROGRESS)
			Assert(cc_chunk->length == 0);
		else
		{
			Assert(cc_chunk->length > 0);
			ccache_chunk_filename(fname,
								  cc_chunk->database_oid,
								  cc_chunk->table_oid,
								  cc_chunk->block_nr);
			if (unlinkat(dirfd(ccache_base_dir), fname, 0) != 0)
				elog(WARNING, "failed on unlinkat \"%s\": %m", fname);
			Assert(ccache_state->ccache_usage >= TYPEALIGN(BLCKSZ,
														   cc_chunk->length));
			ccache_state->ccache_usage -= TYPEALIGN(BLCKSZ, cc_chunk->length);
		}
		/* back to the free list */
		memset(cc_chunk, 0, sizeof(ccacheChunk));
		dlist_push_head(&ccache_state->free_chunks_list,
						&cc_chunk->hash_chain);
	}
}

void
pgstrom_ccache_put_chunk(ccacheChunk *cc_chunk)
{
	SpinLockAcquire(&ccache_state->chunks_lock);
	ccache_put_chunk_nolock(cc_chunk);
	SpinLockRelease(&ccache_state->chunks_lock);
}

bool
pgstrom_ccache_is_empty(ccacheChunk *cc_chunk)
{
	return (cc_chunk->nitems == 0);
}

/*
 * pgstrom_ccache_get_chunk
 */
ccacheChunk *
pgstrom_ccache_get_chunk(Relation relation, BlockNumber block_nr)
{
	Oid			table_oid = RelationGetRelid(relation);
	pg_crc32	hash;
	cl_int		index;
	dlist_iter	iter;
	dlist_node *dnode;
	ccacheChunk *cc_chunk = NULL;
	ccacheChunk *cc_temp;

	hash = ccache_compute_hashvalue(MyDatabaseId, table_oid, block_nr);
	index = hash % ccache_num_slots;

	SpinLockAcquire(&ccache_state->chunks_lock);
	dlist_foreach (iter, &ccache_state->active_slots[index])
	{
		cc_temp = dlist_container(ccacheChunk, hash_chain, iter.cur);
		if (cc_temp->hash == hash &&
			cc_temp->database_oid == MyDatabaseId &&
			cc_temp->table_oid == table_oid &&
			cc_temp->block_nr == block_nr)
		{
			cc_temp->atime = GetCurrentTimestamp();
			if (cc_temp->ctime == CCACHE_CTIME_NOT_BUILD)
			{
				Assert(cc_temp->length == 0 &&
					   cc_temp->lru_chain.next != NULL &&
					   cc_temp->lru_chain.prev != NULL);
				dlist_move_head(&ccache_state->lru_misshit_list,
								&cc_temp->lru_chain);
				cc_temp->atime = GetCurrentTimestamp();
			}
			else if (cc_temp->ctime == CCACHE_CTIME_IN_PROGRESS)
			{
				Assert(cc_temp->length == 0 &&
					   cc_temp->lru_chain.next == NULL &&
					   cc_temp->lru_chain.prev == NULL);
				cc_temp->atime = GetCurrentTimestamp();
			}
			else
			{
				Assert(cc_temp->length > 0);
				dlist_move_head(&ccache_state->lru_active_list,
								&cc_temp->lru_chain);
				cc_chunk = cc_temp;
				cc_chunk->refcnt++;
				cc_chunk->atime = GetCurrentTimestamp();
			}
			goto found;
		}
	}
	/* no chunks are tracked, add it as misshit entry */
	if (!dlist_is_empty(&ccache_state->free_chunks_list))
	{
		dnode = dlist_pop_head_node(&ccache_state->free_chunks_list);
		cc_temp = dlist_container(ccacheChunk, hash_chain, dnode);
		Assert(cc_temp->lru_chain.prev == NULL &&
			   cc_temp->lru_chain.next == NULL &&
			   cc_temp->refcnt == 0 &&
			   cc_temp->ctime == CCACHE_CTIME_NOT_BUILD);
	}
	else if (!dlist_is_empty(&ccache_state->lru_misshit_list))
	{
		dnode = dlist_tail_node(&ccache_state->lru_misshit_list);
		cc_temp = dlist_container(ccacheChunk, lru_chain, dnode);
		Assert(cc_temp->hash_chain.prev != NULL &&
			   cc_temp->hash_chain.next != NULL &&
			   cc_temp->refcnt == 1 &&
			   cc_temp->ctime == CCACHE_CTIME_NOT_BUILD);
		dlist_delete(&cc_temp->hash_chain);
		dlist_delete(&cc_temp->lru_chain);
	}
	memset(cc_temp, 0, sizeof(ccacheChunk));
	cc_temp->hash = hash;
	cc_temp->database_oid = MyDatabaseId;
	cc_temp->table_oid = table_oid;
	cc_temp->block_nr = block_nr;
	cc_temp->refcnt = 1;
	cc_temp->atime = GetCurrentTimestamp();
	dlist_push_tail(&ccache_state->active_slots[index],
					&cc_temp->hash_chain);
	dlist_push_head(&ccache_state->lru_misshit_list,
					&cc_temp->lru_chain);
found:
	SpinLockRelease(&ccache_state->chunks_lock);

	return cc_chunk;
}

/*
 * pgstrom_ccache_load_chunk
 */
pgstrom_data_store *
pgstrom_ccache_load_chunk(ccacheChunk *cc_chunk,
						  GpuContext *gcontext,
						  Relation relation,
						  Relids ccache_refs)
{
	TupleDesc	tupdesc = RelationGetDescr(relation);
	int			i, ncols;
	int			fdesc = -1;
	ssize_t		nitems;
	ssize_t		length;
	ssize_t		offset;
	CUdeviceptr	m_deviceptr;
	CUresult	rc;
	char		buffer[MAXPGPATH];
	kern_data_store *kds_head = (kern_data_store *)buffer;
	pgstrom_data_store *pds;

	/* open the ccache file */
	Assert(CCACHE_CTIME_IS_READY(cc_chunk->ctime));
	ccache_chunk_filename(buffer,
						  MyDatabaseId,
						  RelationGetRelid(relation),
						  cc_chunk->block_nr);
	fdesc = openat(dirfd(ccache_base_dir), buffer, O_RDONLY);
	if (fdesc < 0)
		elog(ERROR, "failed on open('%s'): %m", buffer);

	PG_TRY();
	{
		/* load the header portion of kds-column */
		Assert(cc_chunk->nattrs == tupdesc->natts);
		ncols = cc_chunk->nattrs - (1 + FirstLowInvalidHeapAttributeNumber);
		length = STROMALIGN(offsetof(kern_data_store, colmeta[ncols]));
		if (length > sizeof(buffer))
			kds_head = palloc(length);
		if (pread(fdesc, kds_head, length, 0) != length)
			elog(ERROR, "failed on pread(2): %m");
		nitems = kds_head->nitems;
		/* count length of the PDS_column */
		for (i = bms_next_member(ccache_refs, -1);
			 i >= 0;
			 i = bms_next_member(ccache_refs, i))
		{
			kern_colmeta   *cmeta = &kds_head->colmeta[i];

			length += cmeta->extra_sz * MAXIMUM_ALIGNOF;
			if (cmeta->attlen > 0)
				length += MAXALIGN(TYPEALIGN(cmeta->attalign,
											 cmeta->attlen) * nitems);
			else
				length += MAXALIGN(sizeof(cl_uint) * nitems);
		}
		/* allocation of pds_column buffer */
		rc = gpuMemAllocManaged(gcontext,
								&m_deviceptr,
								offsetof(pgstrom_data_store,
										 kds) + length,
								CU_MEM_ATTACH_GLOBAL);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "out of managed memory");
		pds = (pgstrom_data_store *)m_deviceptr;
		memset(&pds->chain, 0, sizeof(dlist_node));
		pds->gcontext = gcontext;
		pg_atomic_init_u32(&pds->refcnt, 1);
		init_kernel_data_store(&pds->kds, tupdesc, length,
							   KDS_FORMAT_COLUMN, nitems);
		/* load from the ccache file */
		offset = STROMALIGN(offsetof(kern_data_store, colmeta[ncols]));
		for (i = bms_next_member(ccache_refs, -1);
			 i >= 0;
			 i = bms_next_member(ccache_refs, i))
		{
			kern_colmeta   *cmeta = &kds_head->colmeta[i];
			size_t			nbytes;

			Assert(pds->kds.colmeta[i].attbyval  == cmeta->attbyval &&
				   pds->kds.colmeta[i].attalign  == cmeta->attalign &&
				   pds->kds.colmeta[i].attlen    == cmeta->attlen &&
				   pds->kds.colmeta[i].attnum    == cmeta->attnum &&
				   pds->kds.colmeta[i].atttypid  == cmeta->atttypid &&
				   pds->kds.colmeta[i].atttypmod == cmeta->atttypmod);
			Assert(offset == MAXALIGN(offset));
			pds->kds.colmeta[i].va_offset = offset / MAXIMUM_ALIGNOF;
			pds->kds.colmeta[i].extra_sz = cmeta->extra_sz;

			nbytes = cmeta->extra_sz * MAXIMUM_ALIGNOF;
			if (cmeta->attlen > 0)
				nbytes += MAXALIGN(TYPEALIGN(cmeta->attalign,
											 cmeta->attlen) * nitems);
			else
				nbytes += MAXALIGN(sizeof(cl_uint) * nitems);

			if (pread(fdesc,
					  (char *)&pds->kds + offset,
					  nbytes,
					  cmeta->va_offset * MAXIMUM_ALIGNOF) != nbytes)
				elog(ERROR, "failed on pread(2): %m");
			offset += nbytes;
		}
		pds->kds.nitems = nitems;
		Assert(offset == length);
	}
	PG_CATCH();
	{
		close(fdesc);
		PG_RE_THROW();
	}
	PG_END_TRY();
	close(fdesc);
	if ((char *)kds_head != buffer)
		pfree(kds_head);

	return pds;
}

/*
 * ccache_invalidator_oid - returns OID of invalidator trigger function
 */
static Oid
ccache_invalidator_oid(bool missing_ok)
{
	Oid			pgstrom_namespace_oid;
	oidvector	proc_args;
	Form_pg_proc proc_form;
	HeapTuple	tup;
	PGFunction	invalidator_fn;
	Oid			invalidator_oid;
	Datum		datum;
	bool		isnull;
	char	   *probin;
	char	   *prosrc;

	if (OidIsValid(ccache_invalidator_func_oid))
		return ccache_invalidator_func_oid;

	pgstrom_namespace_oid = get_namespace_oid("pgstrom", missing_ok);
	if (!OidIsValid(pgstrom_namespace_oid))
		return InvalidOid;

	SET_VARSIZE(&proc_args, offsetof(oidvector, values));
	proc_args.ndim = 1;
	proc_args.dataoffset = 0;
	proc_args.elemtype = OIDOID;
	proc_args.dim1 = 0;
	proc_args.lbound1 = 1;

	tup = SearchSysCache3(PROCNAMEARGSNSP,
						  CStringGetDatum("ccache_invalidator"),
						  PointerGetDatum(&proc_args),
						  ObjectIdGetDatum(pgstrom_namespace_oid));
	if (!HeapTupleIsValid(tup))
	{
		if (!missing_ok)
			elog(ERROR, "cache lookup failed for function pgstrom.ccache_invalidator");
		return InvalidOid;
	}
	invalidator_oid = HeapTupleGetOid(tup);
	proc_form = (Form_pg_proc) GETSTRUCT(tup);

	if (proc_form->prolang != ClanguageId)
		elog(ERROR, "pgstrom.ccache_invalidator is not C function");

	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc for pgstrom.ccache_invalidator function");
	prosrc = TextDatumGetCString(datum);

	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_probin, &isnull);
	if (isnull)
		elog(ERROR, "null probin for pgstrom.ccache_invalidator function");
	probin = TextDatumGetCString(datum);
	ReleaseSysCache(tup);

	invalidator_fn = load_external_function(probin, prosrc,
											!missing_ok, NULL);
	if (invalidator_fn != pgstrom_ccache_invalidator)
		return InvalidOid;

	ccache_invalidator_func_oid = invalidator_oid;
	return ccache_invalidator_func_oid;
}

/*
 * refresh_ccache_source_relations
 */
static void
refresh_ccache_source_relations(void)
{
	Oid			invalidator_oid;
	Relation	hrel;
	Relation	irel;
	SysScanDesc	sscan;
	HeapTuple	tup;
	HTAB	   *htab = NULL;
	oidvector  *vec = NULL;

	/* already built? */
	if (ccache_relations_htab)
		return;
	/* invalidator function installed? */
	invalidator_oid = ccache_invalidator_oid(true);
	if (!OidIsValid(invalidator_oid))
		return;
	/* make a ccache_relations_hash */
	PG_TRY();
	{
		HASHCTL	hctl;
		Oid		curr_relid = InvalidOid;
		bool	has_row_insert = false;
		bool	has_row_update = false;
		bool	has_row_delete = false;
		bool	has_stmt_truncate = false;

		memset(&hctl, 0, sizeof(HASHCTL));
		hctl.keysize = sizeof(Oid);
		hctl.entrysize = sizeof(void *);

		htab = hash_create("ccache relations hash-table",
						   1024,
						   &hctl,
						   HASH_ELEM | HASH_BLOBS);

		/* walk on the pg_trigger catalog */
		hrel = heap_open(TriggerRelationId, AccessShareLock);
		irel = index_open(TriggerRelidNameIndexId, AccessShareLock);
		sscan = systable_beginscan_ordered(hrel, irel, NULL, 0, NULL);

		for (;;)
		{
			Oid		trig_tgrelid;
			int		trig_tgtype;
			bool	found;

			tup = systable_getnext_ordered(sscan, ForwardScanDirection);
			if (HeapTupleIsValid(tup))
			{
				Form_pg_trigger trig_form = (Form_pg_trigger) GETSTRUCT(tup);

				if (trig_form->tgfoid != invalidator_oid)
					continue;
				if (trig_form->tgenabled != TRIGGER_FIRES_ALWAYS)
					continue;

				trig_tgrelid = trig_form->tgrelid;
				trig_tgtype  = trig_form->tgtype;
			}
			else
			{
				trig_tgrelid = InvalidOid;
				trig_tgtype = 0;
			}

			/* switch current focus if any */
			if (curr_relid != trig_tgrelid)
			{
				if (OidIsValid(curr_relid) &&
					has_row_insert &&
					has_row_update &&
					has_row_delete &&
					has_stmt_truncate)
				{
					elog(BUILDER_LOG, "ccache-builder%d added relation \"%s\"",
						 ccache_builder->builder_id, get_rel_name(curr_relid));
					hash_search(htab,
								&curr_relid,
								HASH_ENTER,
								&found);
					Assert(!found);
				}
				curr_relid = trig_tgrelid;
				has_row_insert = false;
				has_row_update = false;
				has_row_delete = false;
				has_stmt_truncate = false;
			}
			if (!HeapTupleIsValid(tup))
				break;

			/* is invalidator configured correctly? */
			if (TRIGGER_FOR_AFTER(trig_tgtype))
			{
				if (TRIGGER_FOR_ROW(trig_tgtype))
				{
					if (TRIGGER_FOR_INSERT(trig_tgtype))
						has_row_insert = true;
					if (TRIGGER_FOR_UPDATE(trig_tgtype))
						has_row_update = true;
					if (TRIGGER_FOR_DELETE(trig_tgtype))
						has_row_delete = true;
				}
				else
				{
					if (TRIGGER_FOR_TRUNCATE(trig_tgtype))
						has_stmt_truncate = true;
				}
			}
		}
		ccache_relations_htab = htab;
		systable_endscan_ordered(sscan);
		index_close(irel, AccessShareLock);
		heap_close(hrel, AccessShareLock);

		/* also setup oidvector for fast lookup on cache-build */
		if (ccache_builder)
		{
			long	nitems = hash_get_num_entries(ccache_relations_htab);
			size_t	len = offsetof(oidvector, values[nitems]);
			Oid	   *keyptr;
			int		i = 0;
			HASH_SEQ_STATUS	seq;

			vec = MemoryContextAlloc(CacheMemoryContext, len);
			SET_VARSIZE(vec, len);
			vec->ndim = 1;
			vec->dataoffset = 0;
			vec->elemtype = OIDOID;
			vec->dim1 = nitems;
			vec->lbound1 = 0;

			hash_seq_init(&seq, ccache_relations_htab);
			while ((keyptr = hash_seq_search(&seq)) != NULL)
				vec->values[i++] = *keyptr;
		}
	}
	PG_CATCH();
	{
		if (vec)
			pfree(vec);
		hash_destroy(htab);
		PG_RE_THROW();
	}
	PG_END_TRY();

	ccache_relations_htab = htab;
	if (ccache_relations_oid)
		pfree(ccache_relations_oid);
	ccache_relations_oid = vec;
}

/*
 * ccache_callback_on_reloid - catcache callback on RELOID
 */
static void
ccache_callback_on_reloid(Datum arg, int cacheid, uint32 hashvalue)
{
	dlist_mutable_iter iter;
	int			i;
	Datum		reloid;
	uint32		relhash;

	Assert(cacheid == RELOID);
	if (!ccache_relations_htab)
	{
		if (ccache_state)
		{
			SpinLockAcquire(&ccache_state->lock);
			for (i=0; i < ccache_num_builders; i++)
			{
				if (ccache_state->builders[i].latch)
					SetLatch(ccache_state->builders[i].latch);
			}
			SpinLockRelease(&ccache_state->lock);
		}
		return;
	}

	/* invalidation of related cache */
	SpinLockAcquire(&ccache_state->chunks_lock);
	PG_TRY();
	{
		for (i=0; i < ccache_num_slots; i++)
		{
			dlist_foreach_modify(iter, &ccache_state->active_slots[i])
			{
				ccacheChunk	   *cc_temp = dlist_container(ccacheChunk,
														  hash_chain,
														  iter.cur);
				if (cc_temp->database_oid != MyDatabaseId)
					continue;
				if (hashvalue != 0)
				{
					reloid = ObjectIdGetDatum(cc_temp->table_oid);
					relhash = GetSysCacheHashValue(RELOID, reloid, 0, 0, 0);
					if (relhash != hashvalue)
						continue;
				}
				elog(BUILDER_LOG,
					 "ccache: relation oid:%u block_nr %u invalidation",
					 cc_temp->table_oid, cc_temp->block_nr);
				/* ccache invalidation */
				dlist_delete(&cc_temp->hash_chain);
				if (cc_temp->lru_chain.prev &&
					cc_temp->lru_chain.next)
					dlist_delete(&cc_temp->lru_chain);
				memset(&cc_temp->hash_chain, 0, sizeof(dlist_node));
				memset(&cc_temp->lru_chain, 0, sizeof(dlist_node));
				ccache_put_chunk_nolock(cc_temp);
			}
		}
	}
	PG_CATCH();
	{
		SpinLockRelease(&ccache_state->chunks_lock);
		PG_RE_THROW();
	}
	PG_END_TRY();
	SpinLockRelease(&ccache_state->chunks_lock);
	hash_destroy(ccache_relations_htab);
	ccache_relations_htab = NULL;
}

/*
 * ccache_callback_on_procoid - catcache callback on PROCOID
 */
static void
ccache_callback_on_procoid(Datum arg, int cacheid, uint32 hashvalue)
{
	Assert(cacheid == PROCOID);
	if (OidIsValid(ccache_invalidator_func_oid))
	{
		Datum	fnoid = ObjectIdGetDatum(ccache_invalidator_func_oid);
		uint32	inval_hash = GetSysCacheHashValue(PROCOID, fnoid, 0, 0, 0);

		if (inval_hash == hashvalue)
		{
			ccache_invalidator_func_oid = InvalidOid;
			ccache_callback_on_reloid(0, RELOID, 0);
		}
	}
}

/*
 * RelationCanUseColumnarCache
 */
bool
RelationCanUseColumnarCache(Relation relation)
{
	TriggerDesc *trigdesc = relation->trigdesc;
	bool	has_row_insert = false;
	bool	has_row_update = false;
	bool	has_row_delete = false;
	bool	has_stmt_truncate = false;
	int		i;

	if (!trigdesc ||
		!trigdesc->trig_insert_after_row ||
		!trigdesc->trig_update_after_row ||
		!trigdesc->trig_delete_after_row ||
		!trigdesc->trig_truncate_after_statement)
		return false;

	for (i=0; i < trigdesc->numtriggers; i++)
	{
		Trigger	   *trigger = &trigdesc->triggers[i];

		if (trigger->tgfoid != ccache_invalidator_oid(true))
			continue;
		if (trigger->tgenabled != TRIGGER_FIRES_ALWAYS)
			continue;
		if (TRIGGER_FOR_AFTER(trigger->tgtype))
		{
			if (TRIGGER_FOR_ROW(trigger->tgtype))
			{
				if (TRIGGER_FOR_INSERT(trigger->tgtype))
					has_row_insert = true;
				if (TRIGGER_FOR_UPDATE(trigger->tgtype))
					has_row_update = true;
				if (TRIGGER_FOR_DELETE(trigger->tgtype))
					has_row_delete = true;
			}
			else
			{
				if (TRIGGER_FOR_TRUNCATE(trigger->tgtype))
					has_stmt_truncate = true;
			}
		}
	}
	return (has_row_insert &&
			has_row_update &&
			has_row_delete &&
			has_stmt_truncate);
}

/*
 * pgstrom_ccache_invalidator
 */
Datum
pgstrom_ccache_invalidator(PG_FUNCTION_ARGS)
{
	FmgrInfo	   *flinfo = fcinfo->flinfo;
	TriggerData	   *trigdata = (TriggerData *) fcinfo->context;

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "%s: must be called as trigger", __FUNCTION__);
	if (!TRIGGER_FIRED_AFTER(trigdata->tg_event))
		elog(ERROR, "%s: must be configured as AFTER trigger", __FUNCTION__);
	if (TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
	{
		Relation	rel = trigdata->tg_relation;
		HeapTuple	tuple = trigdata->tg_trigtuple;
		BlockNumber	block_nr;
		BlockNumber	block_nr_last;
		pg_crc32	hash;
		int			index;
		dlist_iter	iter;

		if (!TRIGGER_FIRED_BY_INSERT(trigdata->tg_event) &&
			!TRIGGER_FIRED_BY_DELETE(trigdata->tg_event) &&
			!TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
			elog(ERROR, "%s: triggered by unknown event", __FUNCTION__);

		block_nr_last = DatumGetInt32(flinfo->fn_extra);
		block_nr = BlockIdGetBlockNumber(&tuple->t_self.ip_blkid);
		block_nr &= ~(CCACHE_CHUNK_NBLOCKS - 1);
		if ((block_nr_last & ~(CCACHE_CHUNK_NBLOCKS - 1)) != 0)
		{
			block_nr_last &= ~(CCACHE_CHUNK_NBLOCKS - 1);
			if (block_nr == block_nr_last)
				PG_RETURN_VOID();
		}
		hash = ccache_compute_hashvalue(MyDatabaseId,
										RelationGetRelid(rel),
										block_nr);
		index = hash % ccache_num_slots;
		SpinLockAcquire(&ccache_state->chunks_lock);
		dlist_foreach(iter, &ccache_state->active_slots[index])
		{
			ccacheChunk *cc_temp = dlist_container(ccacheChunk,
												   hash_chain,
												   iter.cur);
			if (cc_temp->hash == hash &&
				cc_temp->database_oid == MyDatabaseId &&
				cc_temp->table_oid == RelationGetRelid(rel) &&
				cc_temp->block_nr == block_nr)
			{
				dlist_delete(&cc_temp->hash_chain);
				memset(&cc_temp->hash_chain, 0, sizeof(dlist_node));
				ccache_put_chunk_nolock(cc_temp);
				elog(BUILDER_LOG,
					 "ccache: relation %s, block %u invalidation",
					 RelationGetRelationName(rel), block_nr);
				break;
			}
		}
		SpinLockRelease(&ccache_state->chunks_lock);

		/*
		 * Several least bits of @block_nr should be always zero.
		 * So, we use the least bit as a mark of valid @block_nr_last.
		 */
		flinfo->fn_extra = DatumGetPointer((Datum)(block_nr + 1));
	}
	else
	{
		Relation	rel = trigdata->tg_relation;
		int			index;
		BlockNumber	block_nr;
		dlist_mutable_iter iter;

		if (!TRIGGER_FIRED_BY_TRUNCATE(trigdata->tg_event))
			elog(ERROR, "%s: triggered by unknown event", __FUNCTION__);

		for (index=0; index < ccache_num_slots; index++)
		{
			SpinLockAcquire(&ccache_state->chunks_lock);
			dlist_foreach_modify(iter, &ccache_state->active_slots[index])
			{
				ccacheChunk *cc_temp = dlist_container(ccacheChunk,
													   hash_chain,
													   iter.cur);
				if (cc_temp->database_oid == MyDatabaseId &&
					cc_temp->table_oid == RelationGetRelid(rel))
				{
					dlist_delete(&cc_temp->hash_chain);
					memset(&cc_temp->hash_chain, 0, sizeof(dlist_node));
					block_nr = cc_temp->block_nr;
					ccache_put_chunk_nolock(cc_temp);
					elog(BUILDER_LOG,
						 "ccache: relation %s, block %u invalidation",
						 RelationGetRelationName(rel), block_nr);
				}
			}
			SpinLockRelease(&ccache_state->chunks_lock);
		}
	}
	PG_RETURN_VOID();
}
PG_FUNCTION_INFO_V1(pgstrom_ccache_invalidator);

/*
 * pgstrom_ccache_info
 */
Datum
pgstrom_ccache_info(PG_FUNCTION_ARGS)
{
	FuncCallContext *fncxt;
	ccacheChunk	   *cc_chunk;
	List		   *cc_chunks_list = NIL;
	HeapTuple		tuple;
	bool			isnull[6];
	Datum			values[6];

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc		tupdesc;
		MemoryContext	oldcxt;
		dlist_iter		iter;
		int				i;

		fncxt = SRF_FIRSTCALL_INIT();
		oldcxt = MemoryContextSwitchTo(fncxt->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(6, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "database_id",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "table_id",
						   REGCLASSOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "block_nr",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "length",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "ctime",
						   TIMESTAMPTZOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "atime",
						   TIMESTAMPTZOID, -1, 0);
		fncxt->tuple_desc = BlessTupleDesc(tupdesc);
		/* collect current cache state */
		SpinLockAcquire(&ccache_state->chunks_lock);
		PG_TRY();
		{
			for (i=0; i < ccache_num_slots; i++)
			{
				dlist_foreach(iter, &ccache_state->active_slots[i])
				{
					ccacheChunk	   *cc_temp = dlist_container(ccacheChunk,
															  hash_chain,
															  iter.cur);
					if (!CCACHE_CTIME_IS_READY(cc_temp->ctime))
						continue;
					cc_chunk = palloc(sizeof(ccacheChunk));
					memcpy(cc_chunk, cc_temp, sizeof(ccacheChunk));
					cc_chunks_list = lappend(cc_chunks_list, cc_chunk);
				}
			}
		}
		PG_CATCH();
		{
			SpinLockRelease(&ccache_state->chunks_lock);
			PG_RE_THROW();
		}
		PG_END_TRY();
		SpinLockRelease(&ccache_state->chunks_lock);
		fncxt->user_fctx = cc_chunks_list;
		MemoryContextSwitchTo(oldcxt);
	}
	fncxt = SRF_PERCALL_SETUP();
	cc_chunks_list = fncxt->user_fctx;

	if (cc_chunks_list == NIL)
		SRF_RETURN_DONE(fncxt);
	cc_chunk = linitial(cc_chunks_list);
	fncxt->user_fctx = list_delete_ptr(cc_chunks_list, cc_chunk);

	memset(isnull, 0, sizeof(isnull));
	values[0] = ObjectIdGetDatum(cc_chunk->database_oid);
	values[1] = ObjectIdGetDatum(cc_chunk->table_oid);
	values[2] = Int32GetDatum(cc_chunk->block_nr);
	values[3] = Int64GetDatum(cc_chunk->length);
	//nitems?
	values[4] = TimestampTzGetDatum(cc_chunk->ctime);
	values[5] = TimestampTzGetDatum(cc_chunk->atime);

	tuple = heap_form_tuple(fncxt->tuple_desc, values, isnull);

	SRF_RETURN_NEXT(fncxt, HeapTupleGetDatum(tuple));
}
PG_FUNCTION_INFO_V1(pgstrom_ccache_info);

/*
 * pgstrom_ccache_builder_info
 */
Datum
pgstrom_ccache_builder_info(PG_FUNCTION_ARGS)
{
	FuncCallContext *fncxt;
	ccacheBuilder *builder;
	List	   *builders_list = NIL;
	Datum		values[5];
	bool		isnull[5];
	HeapTuple	tuple;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc		tupdesc;
		MemoryContext	oldcxt;
		int				i;

		fncxt = SRF_FIRSTCALL_INIT();
		oldcxt = MemoryContextSwitchTo(fncxt->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(5, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "builder_id",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "state",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "database_id",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "table_id",
						   REGCLASSOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "block_nr",
						   INT4OID, -1, 0);
		fncxt->tuple_desc = BlessTupleDesc(tupdesc);
		/* collect current builder state */
		SpinLockAcquire(&ccache_state->lock);
		PG_TRY();
		{
			for (i=0; i < ccache_num_builders; i++)
			{
				builder = palloc(sizeof(ccacheBuilder));
				memcpy(builder, &ccache_state->builders[i],
					   sizeof(ccacheBuilder));
				builders_list = lappend(builders_list, builder);
			}
		}
		PG_CATCH();
		{
			SpinLockRelease(&ccache_state->lock);
			PG_RE_THROW();
		}
		PG_END_TRY();
		SpinLockRelease(&ccache_state->lock);

		fncxt->user_fctx = builders_list;
		MemoryContextSwitchTo(oldcxt);
	}
	fncxt = SRF_PERCALL_SETUP();
	builders_list = fncxt->user_fctx;

	if (builders_list == NIL)
		SRF_RETURN_DONE(fncxt);
	builder = linitial(builders_list);
	fncxt->user_fctx = list_delete_ptr(builders_list, builder);

	memset(isnull, 0, sizeof(isnull));
	values[0] = Int32GetDatum(builder->builder_id);
	if (builder->state == CCBUILDER_STATE__SHUTDOWN)
		values[1] = CStringGetTextDatum("shutdown");
	else if (builder->state == CCBUILDER_STATE__STARTUP)
		values[1] = CStringGetTextDatum("startup");
	else if (builder->state == CCBUILDER_STATE__LOADING)
		values[1] = CStringGetTextDatum("loading");
	else if (builder->state == CCBUILDER_STATE__SLEEP)
		values[1] = CStringGetTextDatum("sleep");
	else
		values[1] = CStringGetTextDatum("unknown");

	if (OidIsValid(builder->database_oid))
		values[2] = ObjectIdGetDatum(builder->database_oid);
	else
		isnull[2] = true;
	if (OidIsValid(builder->table_oid))
		values[3] = ObjectIdGetDatum(builder->table_oid);
	else
		isnull[3] = true;

	if (builder->block_nr != InvalidBlockNumber)
		values[4] = Int32GetDatum(builder->block_nr);
	else
		isnull[4] = true;

	tuple = heap_form_tuple(fncxt->tuple_desc, values, isnull);

	SRF_RETURN_NEXT(fncxt, HeapTupleGetDatum(tuple));
}
PG_FUNCTION_INFO_V1(pgstrom_ccache_builder_info);

/*
 * ccache_builder_sigterm
 */
static void
ccache_builder_sigterm(SIGNAL_ARGS)
{
	int		saved_errno = errno;

	ccache_builder_got_sigterm = true;

	pg_memory_barrier();

    SetLatch(MyLatch);

	errno = saved_errno;
}

/*
 * ccache_builder_sighup
 */
static void
ccache_builder_sighup(SIGNAL_ARGS)
{
	SetLatch(MyLatch);
}

/*
 * ccache_builder_fail_on_connectdb
 */
static void
ccache_builder_fail_on_connectdb(int code, Datum arg)
{
	cl_int		generation;

	/* remove database entry from pg_strom.ccache_databases */
	SpinLockAcquire(&ccache_state->lock);
	generation = pg_atomic_read_u32(&ccache_state->generation);
	if (generation == ccache_builder_generation)
		ccache_database->invalid_database = true;
	SpinLockRelease(&ccache_state->lock);
}

/*
 * ccache_builder_connectdb
 */
static void
ccache_builder_connectdb(void)
{
	int		i, j, ev;
	uint32	generation;
	char	dbname[NAMEDATALEN];
	bool	startup_log = false;

	/*
	 * Pick up a database to connect
	 */
	for (;;)
	{
		ResetLatch(MyLatch);

		if (ccache_builder_got_sigterm)
			elog(ERROR, "terminating ccache-builder%d",
				 ccache_builder->builder_id);

		SpinLockAcquire(&ccache_state->lock);
		for (i=0; i < ccache_state->num_databases; i++)
		{
			if (!ccache_state->databases[i].invalid_database)
				break;
		}

		if (i < ccache_state->num_databases)
		{
			/* any valid databases are configured */
			j = (ccache_state->rr_count++ %
				 ccache_state->num_databases);
			if (ccache_state->databases[j].invalid_database)
			{
				SpinLockRelease(&ccache_state->lock);
				continue;
			}
			generation = pg_atomic_read_u32(&ccache_state->generation);
			ccache_builder_generation = generation;
			ccache_database = &ccache_state->databases[j];
			strncpy(dbname, ccache_database->dbname, NAMEDATALEN);
			SpinLockRelease(&ccache_state->lock);
			break;
		}
		else
		{
			/* no valid databases are configured right now */
			SpinLockRelease(&ccache_state->lock);
			if (!startup_log)
			{
				elog(BUILDER_LOG,
					 "ccache-builder%d launched with no database assignment",
					 ccache_builder->builder_id);
				startup_log = true;
			}
			ev = WaitLatch(MyLatch,
						   WL_LATCH_SET |
						   WL_TIMEOUT |
						   WL_POSTMASTER_DEATH,
						   60000L
#if PG_VERSION_NUM >= 100000
						   ,PG_WAIT_EXTENSION
#endif
);
			if (ev & WL_POSTMASTER_DEATH)
				elog(FATAL, "Unexpected postmaster dead");
		}
	}

	/*
	 * Try to connect database
	 */
	PG_ENSURE_ERROR_CLEANUP(ccache_builder_fail_on_connectdb, 0L);
	{
		BackgroundWorkerInitializeConnection(dbname, NULL);
	}
	PG_END_ENSURE_ERROR_CLEANUP(ccache_builder_fail_on_connectdb, 0L);
	elog(BUILDER_LOG,
		 "ccache-builder%d (gen=%u) now ready on database \"%s\"",
		 ccache_builder->builder_id, generation, dbname);
	SpinLockAcquire(&ccache_state->lock);
	ccache_builder->database_oid = MyDatabaseId;
	ccache_builder->state = CCBUILDER_STATE__LOADING;
	SpinLockRelease(&ccache_state->lock);
}

/*
 * vl_dict_hash_value - hash value of varlena dictionary
 */
static uint32
vl_dict_hash_value(const void *__key, Size keysize)
{
	const vl_dict_key *key = __key;
	pg_crc32	crc;

	if (VARATT_IS_COMPRESSED(key->vl_datum))
		elog(ERROR, "unexpected compressed varlena datum");
	if (VARATT_IS_EXTERNAL(key->vl_datum))
		elog(ERROR, "unexpected external toast datum");

	INIT_LEGACY_CRC32(crc);
	COMP_LEGACY_CRC32(crc, key->vl_datum, VARSIZE_ANY(key->vl_datum));
	FIN_LEGACY_CRC32(crc);

	return (uint32) crc;
}

/*
 * vl_dict_compare - comparison of varlena dictionary
 */
static int
vl_dict_compare(const void *__key1, const void *__key2, Size keysize)
{
	const vl_dict_key *key1 = __key1;
	const vl_dict_key *key2 = __key2;

	if (VARATT_IS_COMPRESSED(key1->vl_datum) ||
		VARATT_IS_COMPRESSED(key2->vl_datum))
		elog(ERROR, "unexpected compressed varlena datum");
	if (VARATT_IS_EXTERNAL(key1->vl_datum) ||
		VARATT_IS_EXTERNAL(key2->vl_datum))
		elog(ERROR, "unexpected external toast datum");

	if (VARSIZE_ANY_EXHDR(key1->vl_datum) == VARSIZE_ANY_EXHDR(key2->vl_datum))
		return memcmp(VARDATA_ANY(key1->vl_datum),
					  VARDATA_ANY(key2->vl_datum),
					  VARSIZE_ANY_EXHDR(key1->vl_datum));
	return 1;
}

/*
 * create_varlena_dictionary
 */
HTAB *
create_varlena_dictionary(size_t nrooms)
{
	HASHCTL		hctl;

	memset(&hctl, 0, sizeof(HASHCTL));
	hctl.hash = vl_dict_hash_value;
	hctl.match = vl_dict_compare;
	hctl.keysize = sizeof(vl_dict_key);

	return hash_create("varlena dictionary hash-table",
					   Max(nrooms / 5, 4096),
					   &hctl,
					   HASH_FUNCTION |
					   HASH_COMPARE);
}

/*
 * pgstrom_ccache_extract_row
 */
void
pgstrom_ccache_extract_row(TupleDesc tupdesc,
						   size_t nitems,
						   size_t nrooms,
						   bool *tup_isnull,
						   Datum *tup_values,
						   bits8 **cs_nullmap,
						   bool *cs_hasnull,
						   void **cs_values,
						   HTAB **cs_vl_dict,
						   size_t *cs_extra_sz)
{
	int		j;

	for (j=0; j < tupdesc->natts; j++)
	{
		Form_pg_attribute attr = tupdesc->attrs[j];
		bool	isnull = tup_isnull[j];
		Datum	datum = tup_values[j];

		if (attr->attlen < 0)
		{
			vl_dict_key *entry = NULL;

			if (!isnull)
			{
				struct varlena *vl = PG_DETOAST_DATUM_PACKED(datum);
				vl_dict_key key;
				bool		found;

				key.offset = 0;
				key.vl_datum = vl;
				entry = hash_search(cs_vl_dict[j],
									&key,
									HASH_ENTER,
									&found);
				if (!found)
				{
					entry->offset = 0;
					if (PointerGetDatum(vl) != datum)
						entry->vl_datum = vl;
					else
						entry->vl_datum = PG_DETOAST_DATUM_COPY(datum);
					cs_extra_sz[j] += MAXALIGN(VARSIZE_ANY(vl));

					if (MAXALIGN(sizeof(cl_uint) * nrooms) +
						cs_extra_sz[j] >= ((size_t)UINT_MAX * MAXIMUM_ALIGNOF))
						elog(ERROR, "attribute \"%s\" consumed too much",
							 NameStr(attr->attname));
				}
			}
			((vl_dict_key **)cs_values[j])[nitems] = entry;
		}
		else
		{
			bits8  *nullmap = cs_nullmap[j];
			char   *base = cs_values[j];

			if (isnull)
			{
				cs_hasnull[j] = true;
				nullmap[nitems >> 3] &= ~(1 << (nitems & 7));
			}
			else if (!attr->attbyval)
			{
				nullmap[nitems >> 3] |= (1 << (nitems & 7));
				base += att_align_nominal(attr->attlen,
										  attr->attalign) * nitems;
				memcpy(base, DatumGetPointer(datum), attr->attlen);
			}
			else
			{
				nullmap[nitems >> 3] |= (1 << (nitems & 7));
				base += att_align_nominal(attr->attlen,
										  attr->attalign) * nitems;
				memcpy(base, &datum, attr->attlen);
			}
		}
	}
}

/*
 * pgstrom_ccache_writeout_chunk
 */
void
pgstrom_ccache_writeout_chunk(kern_data_store *kds,
							  bits8 **cs_nullmap,
							  bool *cs_hasnull,
							  void **cs_values,
							  HTAB **cs_vl_dict,
							  size_t *cs_extra_sz)
{
	size_t	nitems = kds->nitems;
	char   *pos;
	int		i, j;

	pos = (char *)kds + STROMALIGN(offsetof(kern_data_store,
											colmeta[kds->ncols]));
	for (j=0; j < kds->ncols; j++)
	{
		kern_colmeta   *cmeta = &kds->colmeta[j];
		size_t			offset;
		size_t			nbytes;

		if (!cs_values[j])
			continue;

		offset = ((char *)pos - (char *)kds);
		Assert((offset & (MAXIMUM_ALIGNOF - 1)) == 0);
		cmeta->va_offset = offset / MAXIMUM_ALIGNOF;
		if (cmeta->attlen < 0)
		{
			/* variable-length column */
			HASH_SEQ_STATUS hseq;
			vl_dict_key *entry;
			cl_uint	   *base = (cl_uint *)pos;
			size_t		base_len = MAXALIGN(sizeof(cl_uint) * nitems);
			char	   *extra = pos + base_len;

			hash_seq_init(&hseq, cs_vl_dict[j]);
			while ((entry = hash_seq_search(&hseq)) != NULL)
			{
				offset = (size_t)(extra - (char *)base);
				Assert((offset & (MAXIMUM_ALIGNOF - 1)) == 0);

				entry->offset = offset / MAXIMUM_ALIGNOF;
				nbytes = VARSIZE_ANY(entry->vl_datum);
				memcpy(extra, entry->vl_datum, nbytes);
				cmeta->extra_sz += MAXALIGN(nbytes) / MAXIMUM_ALIGNOF;
				extra += MAXALIGN(nbytes);
			}

			for (i=0; i < nitems; i++)
			{
				entry = ((vl_dict_key **)cs_values[j])[i];
				if (!entry)
					base[i] = 0;
				else
				{
					Assert(entry->offset < cmeta->extra_sz + base_len);
					base[i] = entry->offset;
				}
			}
			pos += (char *)extra - (char *)base;
		}
		else
		{
			/* fixed-length column */
			offset = pos - (char *)kds;
			Assert((offset & (MAXIMUM_ALIGNOF - 1)) == 0);

			cmeta->va_offset = offset / MAXIMUM_ALIGNOF;
			nbytes = MAXALIGN(TYPEALIGN(cmeta->attalign,
										cmeta->attlen) * nitems);
			memcpy(pos, cs_values[j], nbytes);
			pos += nbytes;
			/* null bitmap, if any */
			if (cs_hasnull[j])
			{
				cmeta->extra_sz = nbytes = MAXALIGN(BITMAPLEN(nitems));
				memcpy(pos, cs_nullmap[j], nbytes);
				pos += nbytes;
			}
		}
	}
	Assert(kds->length == (char *)pos - (char *)kds);
}

/*
 * long-life resources for preload/tryload
 */
static MemoryContext	PerChunkMemoryContext;
static char			   *PerChunkLoadBuffer;
static Buffer			VisibilityMapBuffer = InvalidBuffer;

/*
 * ccache_preload_chunk - preload a chunk
 */
static bool
__ccache_preload_chunk(ccacheChunk *cc_chunk,
					   Relation relation,
					   BlockNumber block_nr)
{
	TupleDesc	tupdesc = RelationGetDescr(relation);
	size_t		nrooms = 0;
	size_t		nitems = 0;
	Datum	   *tup_values;
	bool	   *tup_isnull;
	void	  **cs_values;
	bool	   *cs_hasnull;
	bits8	  **cs_nullmap;
	HTAB	  **cs_vl_dict;
	size_t	   *cs_extra_sz;
	int			ncols;
	int			i, j, fdesc;
	size_t		length;
	char		fname[MAXPGPATH];
	kern_data_store *kds;
	BufferAccessStrategy strategy;

	/* check visibility map first */
	Assert((block_nr & (CCACHE_CHUNK_NBLOCKS-1)) == 0);
	if (block_nr + CCACHE_CHUNK_NBLOCKS >= RelationGetNumberOfBlocks(relation))
		return false;
	for (i=0; i < CCACHE_CHUNK_NBLOCKS; i++)
	{
		if (!VM_ALL_VISIBLE(relation, block_nr+i, &VisibilityMapBuffer))
		{
			elog(BUILDER_LOG,
				 "relation %s, block_nr %u - %lu not all visible",
				 RelationGetRelationName(relation),
				 block_nr, block_nr + CCACHE_CHUNK_NBLOCKS - 1);
			return false;
		}
	}

	/* load and lock buffers */
	strategy = GetAccessStrategy(BAS_BULKREAD);
	for (i=0; i < CCACHE_CHUNK_NBLOCKS; i++)
	{
		Buffer	buffer;
		Page	page;

		CHECK_FOR_INTERRUPTS();
		buffer = ReadBufferExtended(relation, MAIN_FORKNUM, block_nr+i,
									RBM_NORMAL, strategy);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = (Page) BufferGetPage(buffer);
		if (!PageIsAllVisible(page))
		{
			UnlockReleaseBuffer(buffer);
			pfree(strategy);
			elog(BUILDER_LOG,
				 "relation %s, block_nr %u failed on read",
				 RelationGetRelationName(relation),
				 block_nr + i);
			return false;
		}
		nrooms += PageGetMaxOffsetNumber(page);
		memcpy(PerChunkLoadBuffer + i * BLCKSZ, page, BLCKSZ);
		UnlockReleaseBuffer(buffer);
	}
	pfree(strategy);
	/* ok, all the buffers are all-visible */

	/*
	 * read buffers and convert to the columnar format
	 */
	ncols = tupdesc->natts - (1 + FirstLowInvalidHeapAttributeNumber);
	cs_values = palloc0(sizeof(void *) * ncols);
	cs_hasnull = palloc0(sizeof(bool) * ncols);
	cs_nullmap = palloc0(sizeof(void *) * ncols);
	cs_vl_dict = palloc0(sizeof(HTAB *) * ncols);
	cs_extra_sz = palloc0(sizeof(size_t) * ncols);
	tup_values = palloc(sizeof(Datum) * tupdesc->natts);
	tup_isnull = palloc(sizeof(bool) * tupdesc->natts);
	for (j=0; j < tupdesc->natts; j++)
	{
		Form_pg_attribute attr = tupdesc->attrs[j];

		if (attr->attlen < 0)
		{
			cs_vl_dict[j] = create_varlena_dictionary(nrooms);
			cs_values[j] = palloc0(sizeof(vl_dict_key) * nrooms);
		}
		else
		{
			cs_values[j] = palloc(att_align_nominal(attr->attlen,
													attr->attalign) * nrooms);
			cs_nullmap[j] = palloc0(BITMAPLEN(nrooms));
		}
	}
	/* system columns (except for 'tableoid') */
	for (j=FirstLowInvalidHeapAttributeNumber+1; j < 0; j++)
	{
		Form_pg_attribute attr = SystemAttributeDefinition(j, true);

		if (j == TableOidAttributeNumber ||
			(j == ObjectIdAttributeNumber && !tupdesc->tdhasoid))
			continue;
		cs_values[ncols + j] =
			palloc0(att_align_nominal(attr->attlen,
									  attr->attalign) * nrooms);
	}

	/*
	 * extract rows
	 */
	for (i=0; i < CCACHE_CHUNK_NBLOCKS; i++)
	{
		Page	page = (Page)(PerChunkLoadBuffer + BLCKSZ * i);
		int		lines = PageGetMaxOffsetNumber(page);
		OffsetNumber lineoff;
		ItemId	lpp;

		for (lineoff = FirstOffsetNumber, lpp = PageGetItemId(page, lineoff);
			 lineoff <= lines;
			 lineoff++, lpp++)
		{
			HeapTupleData	tup;

			if (!ItemIdIsNormal(lpp))
				continue;
			tup.t_tableOid = RelationGetRelid(relation);
			tup.t_data = (HeapTupleHeader) PageGetItem(page, lpp);
			tup.t_len = ItemIdGetLength(lpp);
			ItemPointerSet(&tup.t_self, block_nr+i, lineoff);

			Assert(nitems < nrooms);
			heap_deform_tuple(&tup, tupdesc, tup_values, tup_isnull);
			pgstrom_ccache_extract_row(tupdesc,
									   nitems, nrooms,
									   tup_isnull,
									   tup_values,
									   cs_nullmap,
									   cs_hasnull,
									   cs_values,
									   cs_vl_dict,
									   cs_extra_sz);
			/* extract system columns */
			for (j=FirstLowInvalidHeapAttributeNumber+1; j < 0; j++)
			{
				Form_pg_attribute attr = SystemAttributeDefinition(j, true);
				char   *base = cs_values[ncols + j];
				Datum	datum;
				bool	isnull;

				if (j == TableOidAttributeNumber ||
					(j == ObjectIdAttributeNumber && !tupdesc->tdhasoid))
					continue;
				datum = heap_getsysattr(&tup, j, tupdesc, &isnull);
				Assert(!isnull);

				base += att_align_nominal(attr->attlen,
										  attr->attalign) * nitems;
				if (!attr->attbyval)
					memcpy(base, DatumGetPointer(datum), attr->attlen);
				else
					memcpy(base, &datum, attr->attlen);
			}
			nitems++;
		}
	}

	/*
	 * write out to the ccache file
	 */
	length = STROMALIGN(offsetof(kern_data_store, colmeta[ncols]));
	for (j=FirstLowInvalidHeapAttributeNumber+1; j < tupdesc->natts; j++)
	{
		Form_pg_attribute attr;

		if (j < 0)
		{
			if (j == TableOidAttributeNumber)
				continue;
			if (j == ObjectIdAttributeNumber && !tupdesc->tdhasoid)
				continue;
			attr = SystemAttributeDefinition(j, true);
		}
		else
			attr = tupdesc->attrs[j];

		if (attr->attlen < 0)
		{
			length += (MAXALIGN(sizeof(cl_uint) * nitems) +
					   MAXALIGN(cs_extra_sz[j]));
		}
		else
		{
			length += MAXALIGN(att_align_nominal(attr->attlen,
												 attr->attalign) * nitems);
			if (cs_hasnull[j])
				length += MAXALIGN(BITMAPLEN(nitems));
		}
	}
	ccache_chunk_filename(fname,
						  MyDatabaseId,
						  RelationGetRelid(relation),
						  block_nr);
	fdesc = openat(dirfd(ccache_base_dir), fname,
				   O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fdesc < 0)
		elog(ERROR, "failed on openat('%s'): %m", fname);
	if (fallocate(fdesc, 0, 0, length) < 0)
	{
		close(fdesc);
		elog(ERROR, "failed on fallocate: %m");
	}
	kds = mmap(NULL, length,
			   PROT_READ | PROT_WRITE,
			   MAP_SHARED,
			   fdesc, 0);
	if (kds == MAP_FAILED)
	{
		close(fdesc);
		elog(ERROR, "failed on mmap: %m");
	}
	init_kernel_data_store(kds, tupdesc, length, KDS_FORMAT_COLUMN, nitems);
	kds->nitems = nitems;
	pgstrom_ccache_writeout_chunk(kds,
								  cs_nullmap,
								  cs_hasnull,
								  cs_values,
								  cs_vl_dict,
								  cs_extra_sz);

	if (munmap(kds, length) != 0)
		elog(WARNING, "failed on munmap: %m");
	if (close(fdesc) != 0)
		elog(WARNING, "failed on munmap: %m");

	/* ensure all-visible flags under the lock */
	i = cc_chunk->hash % ccache_num_slots;
	SpinLockAcquire(&ccache_state->chunks_lock);
	Assert(cc_chunk->lru_chain.next == NULL &&
		   cc_chunk->lru_chain.prev == NULL &&
		   cc_chunk->ctime == CCACHE_CTIME_IN_PROGRESS);
	PG_TRY();
	{
		for (j=0; j < CCACHE_CHUNK_NBLOCKS; j++)
		{
			if (!VM_ALL_VISIBLE(relation, block_nr+j, &VisibilityMapBuffer))
			{
				if (unlinkat(dirfd(ccache_base_dir), fname, 0) != 0)
					elog(WARNING, "failed on unlinkat('%s'): %m", fname);
				length = 0;
				elog(BUILDER_LOG,
					 "relation %s, block_nr %u - %lu not all visible",
					 RelationGetRelationName(relation),
					 block_nr, block_nr + CCACHE_CHUNK_NBLOCKS - 1);
				break;
			}
		}
	}
	PG_CATCH();
	{
		SpinLockRelease(&ccache_state->chunks_lock);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (length > 0)
	{
		dlist_iter		iter;
		ccacheChunk	   *cc_temp;

		ccache_state->ccache_usage += TYPEALIGN(BLCKSZ, length);

		cc_chunk->length = length;
		cc_chunk->nitems = nitems;
		cc_chunk->nattrs = RelationGetNumberOfAttributes(relation);
		cc_chunk->ctime = GetCurrentTimestamp();
		/* move to the LRU active list */
		dlist_foreach(iter, &ccache_state->lru_active_list)
		{
			cc_temp = dlist_container(ccacheChunk, lru_chain, iter.cur);
            if (cc_chunk->atime > cc_temp->atime)
			{
				dlist_insert_before(&cc_temp->lru_chain,
									&cc_chunk->lru_chain);
				ccache_put_chunk_nolock(cc_chunk);
				cc_chunk = NULL;
				break;
			}
		}
		if (cc_chunk)
		{
			dlist_push_tail(&ccache_state->lru_active_list,
							&cc_chunk->lru_chain);
			ccache_put_chunk_nolock(cc_chunk);
		}
	}
	SpinLockRelease(&ccache_state->chunks_lock);
	return (length > 0);
}

static bool
ccache_preload_chunk(ccacheChunk *cc_chunk,
					 Relation relation,
					 BlockNumber block_nr)
{
	bool	retval;

	PG_TRY();
	{
		retval = __ccache_preload_chunk(cc_chunk, relation, block_nr);
		if (retval)
		{
			elog(BUILDER_LOG,
				 "ccache-builder%d: relation \"%s\" block_nr %u loaded",
				 ccache_builder->builder_id,
				 RelationGetRelationName(relation), block_nr);
		}
		else
		{
			/*
			 * Fail of ccache_preload_chunk() is likely due to partial
			 * all-visible blocks. It needs to be set by auto or manual
			 * vacuum analyze, thus it is hopeless to cache this segment
			 * very soon.
			 * So, we purge this ccache misshit at this moment.
			 */
			SpinLockAcquire(&ccache_state->chunks_lock);
			Assert(cc_chunk->lru_chain.next == NULL &&
				   cc_chunk->lru_chain.prev == NULL &&
				   cc_chunk->ctime == CCACHE_CTIME_IN_PROGRESS);
			dlist_delete(&cc_chunk->hash_chain);
			memset(&cc_chunk->hash_chain, 0, sizeof(dlist_node));
			ccache_put_chunk_nolock(cc_chunk);
			ccache_put_chunk_nolock(cc_chunk);
			SpinLockRelease(&ccache_state->chunks_lock);
		}
	}
	PG_CATCH();
	{
		/* see comment above */
		SpinLockAcquire(&ccache_state->chunks_lock);
		Assert(cc_chunk->lru_chain.next == NULL &&
			   cc_chunk->lru_chain.prev == NULL &&
			   cc_chunk->ctime == CCACHE_CTIME_IN_PROGRESS);
		dlist_delete(&cc_chunk->hash_chain);
		memset(&cc_chunk->hash_chain, 0, sizeof(dlist_node));
		ccache_put_chunk_nolock(cc_chunk);
		ccache_put_chunk_nolock(cc_chunk);
		SpinLockRelease(&ccache_state->chunks_lock);

		PG_RE_THROW();
	}
	PG_END_TRY();

	return retval;
}

/*
 * ccache_tryload_misshit_chunk - called by tryload
 */
static bool
ccache_tryload_misshit_chunk(void)
{
	dlist_node	   *dnode;
	ccacheChunk	   *cc_chunk;
	ccacheChunk	   *cc_temp;
	bool			retval;

	/* pick up a target chunk to be loaded, which is recently referenced */
	SpinLockAcquire(&ccache_state->chunks_lock);
	if (dlist_is_empty(&ccache_state->lru_misshit_list))
	{
		SpinLockRelease(&ccache_state->chunks_lock);
		return false;
	}
	dnode = dlist_head_node(&ccache_state->lru_misshit_list);
	cc_chunk = dlist_container(ccacheChunk, lru_chain, dnode);
	Assert(cc_chunk->hash_chain.prev != NULL &&
		   cc_chunk->hash_chain.next != NULL &&
		   cc_chunk->ctime == CCACHE_CTIME_NOT_BUILD);

	/* release existing chunks if ccache usage is nearby the limitation */
	while (ccache_state->ccache_usage +
		   CCACHE_CHUNK_SIZE > ccache_total_size)
	{
		if (dlist_is_empty(&ccache_state->lru_active_list))
		{
			SpinLockRelease(&ccache_state->chunks_lock);
			return false;
		}
		dnode = dlist_tail_node(&ccache_state->lru_active_list);
		cc_temp = dlist_container(ccacheChunk, lru_chain, dnode);
		Assert(cc_temp->ctime != CCACHE_CTIME_IN_PROGRESS);
		dlist_delete(&cc_temp->hash_chain);
		memset(&cc_temp->hash_chain, 0, sizeof(dlist_node));
		ccache_put_chunk_nolock(cc_temp);
	}
	/* detach from the LRU list and mark it 'in-progress' */
	dlist_delete(&cc_chunk->lru_chain);
	memset(&cc_chunk->lru_chain, 0, sizeof(dlist_node));
	cc_chunk->ctime = CCACHE_CTIME_IN_PROGRESS;
	cc_chunk->refcnt++;
	SpinLockRelease(&ccache_state->chunks_lock);
	/* update builder's state */
	SpinLockAcquire(&ccache_state->lock);
	ccache_builder->table_oid = cc_chunk->table_oid;
	ccache_builder->block_nr = cc_chunk->block_nr;
	SpinLockRelease(&ccache_state->lock);

	PG_TRY();
	{
		Relation	relation;

		relation = heap_open(cc_chunk->table_oid, AccessShareLock);
		retval = ccache_preload_chunk(cc_chunk, relation, cc_chunk->block_nr);
		heap_close(relation, NoLock);
	}
	PG_CATCH();
	{
		/* update builder's state */
		SpinLockAcquire(&ccache_state->lock);
		ccache_builder->table_oid = InvalidOid;
		ccache_builder->block_nr = InvalidBlockNumber;
		SpinLockRelease(&ccache_state->lock);
		PG_RE_THROW();
	}
	PG_END_TRY();
	/* update builder's state */
	SpinLockAcquire(&ccache_state->lock);
	ccache_builder->table_oid = InvalidOid;
	ccache_builder->block_nr = InvalidBlockNumber;
	SpinLockRelease(&ccache_state->lock);

	return retval;
}

/*
 * ccache_tryload_chilly_chunk
 */
static int
ccache_tryload_chilly_chunk(int nchunks_atonce)
{
	Relation	   *open_relations = NULL;
	cl_long		   *open_nchunks;
	cl_long		   *start_count;
	Relation		relation;
	cl_int			num_rels;
	cl_int			num_open_rels;
	cl_ulong		i, j, k;
	cl_ulong		count;
	BlockNumber		nchunks;
	BlockNumber		block_nr;
	pg_crc32		hashvalue;
	dlist_node	   *dnode;
	dlist_iter		iter;
	ccacheChunk	   *cc_chunk;
	ccacheChunk	   *cc_temp;

	/* any relations to be loaded? */
	if (!ccache_relations_oid)
		return nchunks_atonce;
	num_rels = ccache_relations_oid->dim1;
	if (num_rels == 0)
		return nchunks_atonce;
	num_open_rels = num_rels;

	/* check available space for quick exit */
	SpinLockAcquire(&ccache_state->chunks_lock);
	if (ccache_state->ccache_usage +
		CCACHE_CHUNK_SIZE > ccache_total_size)
	{
		SpinLockRelease(&ccache_state->chunks_lock);
		return nchunks_atonce;
	}
	SpinLockRelease(&ccache_state->chunks_lock);

	while (num_open_rels > 0 && nchunks_atonce > 0)
	{
		/* allocation of relations array on first try */
		if (!open_relations)
		{
			open_relations = palloc0(sizeof(Relation) * num_rels);
			open_nchunks = palloc0(sizeof(cl_long) * num_rels);
			start_count = palloc0(sizeof(cl_long) * num_rels);
		}
		count = pg_atomic_fetch_add_u64(&ccache_database->curr_scan_pos, 1);
		j = count % num_rels;	/* relation selector */
		i = count / num_rels;	/* block selector */

		if (!open_relations[j])
		{
			relation = heap_open(ccache_relations_oid->values[j],
								 AccessShareLock);
			nchunks = (RelationGetNumberOfBlocks(relation)
					   / CCACHE_CHUNK_NBLOCKS);
			open_relations[j] = relation;
			open_nchunks[j] = nchunks;
			start_count[j] = i;
		}
		relation = open_relations[j];
		nchunks = open_nchunks[j];
		/* already done? */
		if (relation == (void *)(~0L))
			continue;
		/* end of the scan? */
		i -= start_count[j];
		if (i > nchunks)
		{
			heap_close(relation, NoLock);
			open_relations[j] = (void *)(~0UL);
			num_open_rels--;
			continue;
		}

		/* try to lookup ccache already build */
		cc_chunk = NULL;
		block_nr = i * CCACHE_CHUNK_NBLOCKS;
		hashvalue = ccache_compute_hashvalue(MyDatabaseId,
											 RelationGetRelid(relation),
											 block_nr);
		SpinLockAcquire(&ccache_state->chunks_lock);
		if (ccache_state->ccache_usage +
			CCACHE_CHUNK_SIZE > ccache_total_size)
		{
			SpinLockRelease(&ccache_state->chunks_lock);
			break;
		}
		k = hashvalue % ccache_num_slots;
		dlist_foreach(iter, &ccache_state->active_slots[k])
		{
			cc_temp = dlist_container(ccacheChunk, hash_chain, iter.cur);
			if (cc_temp->hash == hashvalue &&
				cc_temp->database_oid == MyDatabaseId &&
				cc_temp->table_oid == RelationGetRelid(relation) &&
				cc_temp->block_nr == block_nr)
			{
				if (cc_temp->ctime == CCACHE_CTIME_NOT_BUILD)
					cc_chunk = cc_temp;
				else
					cc_chunk = (void *)(~0L);
				break;
			}
		}
		/* this chunk is already loaded, or in-progress */
		if (cc_chunk == (void *)(~0L))
		{
			SpinLockRelease(&ccache_state->chunks_lock);
			continue;
		}
		else if (cc_chunk)
		{
			/* once detach cc_chunk from the LRU misshit list */
			dlist_delete(&cc_chunk->lru_chain);
			memset(&cc_chunk->lru_chain, 0, sizeof(dlist_node));
			cc_chunk->refcnt++;
		}
		else if (!dlist_is_empty(&ccache_state->free_chunks_list))
		{
			/* try to fetch a free ccacheChunk object */
			dnode = dlist_pop_head_node(&ccache_state->free_chunks_list);
			cc_chunk = dlist_container(ccacheChunk, hash_chain, dnode);
			Assert(cc_chunk->lru_chain.prev == NULL &&
				   cc_chunk->lru_chain.next == NULL &&
				   cc_chunk->refcnt == 0);
			memset(cc_chunk, 0, sizeof(ccacheChunk));
			cc_chunk->hash = hashvalue;
			cc_chunk->database_oid = MyDatabaseId;
			cc_chunk->table_oid = RelationGetRelid(relation);
			cc_chunk->block_nr = block_nr;
			cc_chunk->refcnt = 2;
			cc_chunk->ctime = CCACHE_CTIME_IN_PROGRESS;
			cc_chunk->atime = GetCurrentTimestamp();
			dlist_push_head(&ccache_state->active_slots[k],
							&cc_chunk->hash_chain);
		}
		else if (!dlist_is_empty(&ccache_state->lru_misshit_list))
		{
			/* purge oldest misshit entry, if any */
			dnode = dlist_tail_node(&ccache_state->lru_misshit_list);
			cc_chunk = dlist_container(ccacheChunk, lru_chain, dnode);
			dlist_delete(&cc_chunk->hash_chain);
			dlist_delete(&cc_chunk->lru_chain);
			Assert(cc_chunk->length == 0 &&
				   cc_chunk->refcnt == 1 &&
				   cc_chunk->ctime == CCACHE_CTIME_NOT_BUILD);
			memset(cc_chunk, 0, sizeof(ccacheChunk));
			cc_chunk->hash = hashvalue;
			cc_chunk->database_oid = MyDatabaseId;
			cc_chunk->table_oid = RelationGetRelid(relation);
			cc_chunk->block_nr = block_nr;
			cc_chunk->refcnt = 2;
			cc_chunk->ctime = CCACHE_CTIME_IN_PROGRESS;
			cc_chunk->atime = GetCurrentTimestamp();
			dlist_push_head(&ccache_state->active_slots[k],
							&cc_chunk->hash_chain);
		}
		else
		{
			SpinLockRelease(&ccache_state->chunks_lock);
			break;
		}
		SpinLockRelease(&ccache_state->chunks_lock);

		/* try to load this chunk */
		PG_TRY();
		{
			/* update builder's state */
			SpinLockAcquire(&ccache_state->lock);
			ccache_builder->table_oid = cc_chunk->table_oid;
			ccache_builder->block_nr = cc_chunk->block_nr;
			SpinLockRelease(&ccache_state->lock);

			ccache_preload_chunk(cc_chunk, relation, cc_chunk->block_nr);

			/* update builder's state */
			SpinLockAcquire(&ccache_state->lock);
			ccache_builder->table_oid = InvalidOid;
			ccache_builder->block_nr = InvalidBlockNumber;
			SpinLockRelease(&ccache_state->lock);
		}
		PG_CATCH();
		{
			/* update builder's state */
			SpinLockAcquire(&ccache_state->lock);
			ccache_builder->table_oid = InvalidOid;
			ccache_builder->block_nr = InvalidBlockNumber;
			SpinLockRelease(&ccache_state->lock);

			PG_RE_THROW();
		}
		PG_END_TRY();
		/* load a chunk */
		nchunks_atonce--;
	}

	if (open_relations)
	{
		for (i=0; i < num_rels; i++)
		{
			if (open_relations[i] && open_relations[i] != (void *)(~0L))
				heap_close(open_relations[i], NoLock);
		}
	}
	return nchunks_atonce;
}

/*
 * ccache_builder_main_on_shutdown
 */
static void
ccache_builder_main_on_shutdown(int code, Datum arg)
{
	SpinLockAcquire(&ccache_state->lock);
    ccache_builder->database_oid = InvalidOid;
	ccache_builder->table_oid = InvalidOid;
	ccache_builder->block_nr = InvalidBlockNumber;
    ccache_builder->state = CCBUILDER_STATE__SHUTDOWN;
    ccache_builder->latch = NULL;
    SpinLockRelease(&ccache_state->lock);
}

/*
 * ccache_builder_main 
 */
void
ccache_builder_main(Datum arg)
{
	cl_int		builder_id = DatumGetInt32(arg);
	int			ev;

	pqsignal(SIGTERM, ccache_builder_sigterm);
	pqsignal(SIGHUP, ccache_builder_sighup);
	BackgroundWorkerUnblockSignals();

	CurrentResourceOwner = ResourceOwnerCreate(NULL, "ccache builder");
	CurrentMemoryContext = AllocSetContextCreate(TopMemoryContext,
												 "ccache builder context",
												 ALLOCSET_DEFAULT_SIZES);
	PerChunkMemoryContext = AllocSetContextCreate(TopMemoryContext,
												  "per-chunk memory context",
												  ALLOCSET_DEFAULT_SIZES);
	PerChunkLoadBuffer = MemoryContextAlloc(TopMemoryContext,
											CCACHE_CHUNK_SIZE);
	ccache_builder = &ccache_state->builders[builder_id];
	SpinLockAcquire(&ccache_state->lock);
	ccache_builder->database_oid = InvalidOid;
	ccache_builder->state = CCBUILDER_STATE__STARTUP;
	ccache_builder->latch = MyLatch;
	SpinLockRelease(&ccache_state->lock);

	PG_ENSURE_ERROR_CLEANUP(ccache_builder_main_on_shutdown, 0);
	{
		/* connect to one of the databases */
		ccache_builder_connectdb();
		for (;;)
		{
			MemoryContext	oldcxt;
			long			timeout = 0L;
			int				nchunks_atonce = 12;

			if (ccache_builder_got_sigterm)
				elog(ERROR, "terminating ccache-builder%d", builder_id);
			ResetLatch(MyLatch);

			/* pg_strom.ccache_databases updated? */
			if (ccache_builder_generation !=
				pg_atomic_read_u32(&ccache_state->generation))
				elog(ERROR,"restarting ccache-builder%d", builder_id);

			CHECK_FOR_INTERRUPTS();

			/*
			 * -------------------------
			 *   BEGIN Transaction
			 * -------------------------
			 */
			SetCurrentStatementStartTimestamp();
			StartTransactionCommand();
			PushActiveSnapshot(GetTransactionSnapshot());

			/* refresh ccache relations, if needed */
			refresh_ccache_source_relations();
			/* try to load several chunks at once */
			oldcxt = MemoryContextSwitchTo(PerChunkMemoryContext);
			VisibilityMapBuffer = InvalidBuffer;
			while (nchunks_atonce > 0)
			{
				if (ccache_tryload_misshit_chunk())
					nchunks_atonce--;
				else
					break;
			}
			if (nchunks_atonce > 0)
				nchunks_atonce = ccache_tryload_chilly_chunk(nchunks_atonce);
			timeout = (nchunks_atonce == 0 ? 0 : 4000);

			if (VisibilityMapBuffer != InvalidBuffer)
				ReleaseBuffer(VisibilityMapBuffer);
			MemoryContextSwitchTo(oldcxt);
			MemoryContextReset(PerChunkMemoryContext);
			/*
			 * -----------------------
			 *   END Transaction
			 * -----------------------
			 */
			PopActiveSnapshot();
			CommitTransactionCommand();

			if (timeout > 0)
			{
				SpinLockAcquire(&ccache_state->lock);
				ccache_builder->state = CCBUILDER_STATE__SLEEP;
				SpinLockRelease(&ccache_state->lock);
			}
			ev = WaitLatch(MyLatch,
						   WL_LATCH_SET |
						   WL_TIMEOUT |
						   WL_POSTMASTER_DEATH,
						   timeout
#if PG_VERSION_NUM >= 100000
						   ,PG_WAIT_EXTENSION
#endif
				);
			if (ev & WL_POSTMASTER_DEATH)
				elog(FATAL, "Unexpected postmaster dead");

			SpinLockAcquire(&ccache_state->lock);
			ccache_builder->state = CCBUILDER_STATE__LOADING;
			SpinLockRelease(&ccache_state->lock);
		}
	}
	PG_END_ENSURE_ERROR_CLEANUP(ccache_builder_main_on_shutdown, 0);
	elog(FATAL, "Bug? ccache-builder%d should not exit normaly", builder_id);
}

/*
 * GUC callbacks for pg_strom.ccache_databases
 */
static bool
guc_check_ccache_databases(char **newval, void **extra, GucSource source)
{
	char	   *rawnames = pstrdup(*newval);
	List	   *options;
	ListCell   *lc1, *lc2;
	ccacheDatabase *my_extra;
	int			i;

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawnames, ',', &options))
	{
		/* syntax error in name list */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawnames);
		list_free(options);
		return false;
	}

	foreach (lc1, options)
	{
		const char   *dbname = lfirst(lc1);

		if (strlen(dbname) >= NAMEDATALEN)
			elog(ERROR, "too long database name: \"%s\"", dbname);
		/* check existence if under transaction */
		if (IsTransactionState())
			get_database_oid(dbname, false);
		/* duplication check */
		foreach (lc2, options)
		{
			if (lc1 == lc2)
				break;
			if (strcmp(dbname, lfirst(lc2)) == 0)
				elog(ERROR, "database \"%s\" appeared twice", dbname);
		}
	}

	if (list_length(options) > CCACHE_MAX_NUM_DATABASES)
		elog(ERROR, "pg_strom.ccache_databases configured too much databases");
	if (list_length(options) > ccache_num_builders)
		elog(ERROR, "number of the configured databases by pg_strom.ccache_databases is larger than number of the builder processes by pg_strom.ccache_num_builders, so columnar cache will be never built on some databases");

	my_extra = calloc(list_length(options) + 1, sizeof(ccacheDatabase));
	if (!my_extra)
		elog(ERROR, "out of memory");
	i = 0;
	foreach (lc1, options)
	{
		strncpy(my_extra[i].dbname, lfirst(lc1), NAMEDATALEN);
		i++;
	}
	my_extra[i].invalid_database = true;

	*extra = my_extra;

	return true;
}

static void
guc_assign_ccache_databases(const char *newval, void *extra)
{
	ccacheDatabase *my_extra = extra;

	if (ccache_state)
	{
		int		i = 0;

		SpinLockAcquire(&ccache_state->lock);
		for (i=0; !my_extra[i].invalid_database; i++)
		{
			Assert(i < CCACHE_MAX_NUM_DATABASES);
			strncpy(ccache_state->databases[i].dbname,
					my_extra[i].dbname,
					NAMEDATALEN);
			ccache_state->databases[i].invalid_database = false;
		}
		ccache_state->num_databases = i;

		/* force to restart ccache builder */
		pg_atomic_fetch_add_u32(&ccache_state->generation, 1);
		for (i=0; i < ccache_num_builders; i++)
		{
			if (ccache_state->builders[i].latch)
				SetLatch(ccache_state->builders[i].latch);
		}
		SpinLockRelease(&ccache_state->lock);
	}
}

static const char *
guc_show_ccache_databases(void)
{
	StringInfoData str;
	int		i;

	initStringInfo(&str);
	SpinLockAcquire(&ccache_state->lock);
	PG_TRY();
	{
		for (i=0; i < ccache_state->num_databases; i++)
		{
			const char *dbname = ccache_state->databases[i].dbname;

			if (!ccache_state->databases[i].invalid_database)
				appendStringInfo(&str, "%s%s",
								 str.len > 0 ? "," : "",
								 quote_identifier(dbname));
		}
	}
	PG_CATCH();
	{
		SpinLockRelease(&ccache_state->lock);
	}
	PG_END_TRY();
	SpinLockRelease(&ccache_state->lock);

	return str.data;
}

static void
guc_assign_ccache_log_output(bool newval, void *extra)
{
	if (ccache_state)
		pg_atomic_write_u32(&ccache_state->builder_log_output,
							(uint32)newval);
}

static const char *
guc_show_ccache_log_output(void)
{
	if (pg_atomic_read_u32(&ccache_state->builder_log_output) == 0)
		return "off";
	return "on";
}

/*
 * pgstrom_startup_ccache
 */
static void
pgstrom_startup_ccache(void)
{
	ccacheChunk *cc_chunk;
	size_t		required;
	bool		found;
	int			i, num_databases;
	void	   *extra = NULL;

	if (shmem_startup_next)
		(*shmem_startup_next)();

	required = MAXALIGN(offsetof(ccacheState,
								 builders[ccache_num_builders])) +
		MAXALIGN(sizeof(dlist_head) * ccache_num_slots) +
		MAXALIGN(sizeof(ccacheChunk) * ccache_num_chunks);
	ccache_state = ShmemInitStruct("Columnar Cache Shared Segment",
								   required, &found);
	if (found)
		elog(ERROR, "Bug? Columnar Cache Shared Segment is already built");
	memset(ccache_state, 0, required);
	ccache_state->active_slots = (dlist_head *)
		((char *)ccache_state +
		 MAXALIGN(offsetof(ccacheState, builders[ccache_num_builders])));
	/* hash slot of ccache chunks */
	SpinLockInit(&ccache_state->chunks_lock);
	dlist_init(&ccache_state->lru_misshit_list);
	dlist_init(&ccache_state->lru_active_list);
	dlist_init(&ccache_state->free_chunks_list);
	for (i=0; i < ccache_num_slots; i++)
		dlist_init(&ccache_state->active_slots[i]);
	/* ccache-chunks */
	cc_chunk = (ccacheChunk *)
		((char *)ccache_state->active_slots +
		 MAXALIGN(sizeof(dlist_head) * ccache_num_slots));
	for (i=0; i < ccache_num_chunks; i++)
	{
		dlist_push_tail(&ccache_state->free_chunks_list,
						&cc_chunk->hash_chain);
		cc_chunk++;
	}
	/* fields for management of builder processes */
	SpinLockInit(&ccache_state->lock);

	/* setup GUC again */
	if (!guc_check_ccache_databases(&ccache_startup_databases,
									&extra, PGC_S_DEFAULT))
		elog(ERROR, "Bug? failed on parse pg_strom.ccache_databases");
	guc_assign_ccache_databases(ccache_startup_databases, extra);

	SpinLockAcquire(&ccache_state->lock);
	for (i=0; i < ccache_num_builders; i++)
	{
		ccache_state->builders[i].builder_id = i;
		ccache_state->builders[i].state = CCBUILDER_STATE__SHUTDOWN;
		ccache_state->builders[i].database_oid = InvalidOid;
		ccache_state->builders[i].table_oid = InvalidOid;
		ccache_state->builders[i].block_nr = InvalidBlockNumber;
	}
	num_databases = ccache_state->num_databases;
	SpinLockRelease(&ccache_state->lock);
	/* cleanup ccache files if no database is configured */
	if (num_databases == 0)
	{
		struct dirent *dent;

		rewinddir(ccache_base_dir);
		while ((dent = readdir(ccache_base_dir)) != NULL)
		{
			Oid			database_oid;
			Oid			table_oid;
			BlockNumber	block_nr;

			if (ccache_check_filename(dent->d_name,
									  &database_oid,
									  &table_oid,
									  &block_nr))
			{
				if (unlinkat(dirfd(ccache_base_dir), dent->d_name, 0) != 0)
					elog(WARNING, "failed on unlinkat('%s','%s'): %m",
						 ccache_base_dir_name, dent->d_name);
			}
		}
	}
}

/*
 * pgstrom_init_ccache
 */
void
pgstrom_init_ccache(void)
{
	static int	ccache_total_size_kb;
	int			ccache_total_size_default;
	long		sc_pagesize = sysconf(_SC_PAGESIZE);
	long		sc_phys_pages = sysconf(_SC_PHYS_PAGES);
	struct statfs statbuf;
	size_t		required = 0;
	BackgroundWorker worker;
	char		pathname[MAXPGPATH];
	int			i;

	DefineCustomStringVariable("pg_strom.ccache_base_dir",
							   "directory name used by ccache",
							   NULL,
							   &ccache_base_dir_name,
							   "/dev/shm",
							   PGC_POSTMASTER,
							   GUC_NOT_IN_SAMPLE,
							   NULL, NULL, NULL);
	snprintf(pathname, sizeof(pathname), "%s/.pg_strom.ccache.%u",
			 ccache_base_dir_name, PostPortNumber);
	ccache_base_dir = opendir(pathname);
	if (!ccache_base_dir)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open ccache directory \"%s\": %m",
							pathname)));
		else if (ccache_num_builders > 0)
		{
			/*
			 * Even if ccache directory is not found, we try to make
			 * an empty directory if ccache builder process will run.
			 */
			if (mkdir(pathname, 0700) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not make a ccache directory \"%s\": %m",
								pathname)));
			ccache_base_dir = opendir(pathname);
			if (!ccache_base_dir)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open ccache directory \"%s\": %m",
								pathname)));
		}
	}
	if (fstatfs(dirfd(ccache_base_dir), &statbuf) != 0)
		elog(ERROR, "failed on fstatfs('%s'): %m", pathname);

	DefineCustomStringVariable("pg_strom.ccache_databases",
							   "databases where ccache builder works on",
							   NULL,
							   &ccache_startup_databases,
							   "",
							   PGC_SUSET,
							   GUC_NOT_IN_SAMPLE,
							   guc_check_ccache_databases,
							   guc_assign_ccache_databases,
							   guc_show_ccache_databases);
	DefineCustomIntVariable("pg_strom.ccache_num_builders",
							"number of ccache builder worker processes",
							NULL,
							&ccache_num_builders,
							2,
							0,
							INT_MAX,
							PGC_POSTMASTER,
							GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("pg_strom.ccache_log_output",
							 "turn on/off log output by ccache builder",
							 NULL,
							 &__ccache_log_output,
							 false,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 guc_assign_ccache_log_output,
							 guc_show_ccache_log_output);

	/* calculation of the default 'pg_strom.ccache_total_size' */
	ccache_total_size_default =
		Min((((3 * statbuf.f_blocks) / 4) * statbuf.f_bsize) >> 10,
			(((2 * sc_phys_pages) / 3) * (sc_pagesize >> 10)));
	DefineCustomIntVariable("pg_strom.ccache_total_size",
							"possible maximum allocation of ccache",
							NULL,
							&ccache_total_size_kb,
							ccache_total_size_default,
							0,
							INT_MAX,
							PGC_POSTMASTER,
							GUC_UNIT_KB,
							NULL, NULL, NULL);

	ccache_total_size = (size_t)ccache_total_size_kb << 10;
	ccache_num_slots = Max(ccache_total_size / CCACHE_CHUNK_SIZE, 300);
	ccache_num_chunks = 5 * ccache_num_slots;

	/* bgworker registration */
	for (i=0; i < ccache_num_builders; i++)
	{
		memset(&worker, 0, sizeof(BackgroundWorker));
		snprintf(worker.bgw_name, sizeof(worker.bgw_name),
				 "PG-Strom ccache-builder%d", i+1);
		worker.bgw_flags = BGWORKER_SHMEM_ACCESS
			| BGWORKER_BACKEND_DATABASE_CONNECTION;
		worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
		worker.bgw_restart_time = 2;
		snprintf(worker.bgw_library_name, BGW_MAXLEN, "pg_strom");
		snprintf(worker.bgw_function_name, BGW_MAXLEN, "ccache_builder_main");
		worker.bgw_main_arg = i;
		RegisterBackgroundWorker(&worker);
	}

	/* request for static shared memory */
	required = MAXALIGN(offsetof(ccacheState,
								 builders[ccache_num_builders])) +
		MAXALIGN(sizeof(slock_t) * ccache_num_slots) +
		MAXALIGN(sizeof(dlist_head) * ccache_num_slots) +
		MAXALIGN(sizeof(ccacheChunk) * ccache_num_chunks);
	RequestAddinShmemSpace(required);

	shmem_startup_next = shmem_startup_hook;
	shmem_startup_hook = pgstrom_startup_ccache;

	CacheRegisterSyscacheCallback(RELOID, ccache_callback_on_reloid, 0);
	CacheRegisterSyscacheCallback(PROCOID, ccache_callback_on_procoid, 0);
}
