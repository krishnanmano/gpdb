/*
 * pxf_fdw.c
 *		  Foreign-data wrapper for PXF (Platform Extension Framework)
 *
 * IDENTIFICATION
 *		  contrib/pxf_fdw/pxf_fdw.c
 */

#include "postgres.h"

#include "pxf_fdw.h"
#include "pxf_bridge.h"
#include "pxf_filter.h"
#include "pxf_fragment.h"

#include "access/sysattr.h"
#include "access/reloptions.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "catalog/pg_type.h"
#include "cdb/cdbsreh.h"
#include "cdb/cdbvars.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "nodes/pg_list.h"
#include "nodes/makefuncs.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

#define DEFAULT_PXF_FDW_STARTUP_COST   50000

extern Datum pxf_fdw_handler(PG_FUNCTION_ARGS);

/*
 * SQL functions
 */
PG_FUNCTION_INFO_V1(pxf_fdw_handler);

/*
 * FDW functions declarations
 */
static void pxfGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);

static void pxfGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);

#if (PG_VERSION_NUM <= 90500)

static ForeignScan *pxfGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid, ForeignPath *best_path, List *tlist,
									  List *scan_clauses);

#else
static ForeignScan *pxfGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid, ForeignPath *best_path, List *tlist, List *scan_clauses, Plan *outer_plan);
#endif

static void pxfExplainForeignScan(ForeignScanState *node, ExplainState *es);

static void pxfBeginForeignScan(ForeignScanState *node, int eflags);

static TupleTableSlot *pxfIterateForeignScan(ForeignScanState *node);

static void pxfReScanForeignScan(ForeignScanState *node);

static void pxfEndForeignScan(ForeignScanState *node);

/* Foreign updates */
static void pxfBeginForeignModify(ModifyTableState *mtstate, ResultRelInfo *resultRelInfo, List *fdw_private, int subplan_index, int eflags);
static TupleTableSlot *pxfExecForeignInsert(EState *estate, ResultRelInfo *resultRelInfo, TupleTableSlot *slot, TupleTableSlot *planSlot);

static void pxfEndForeignModify(EState *estate, ResultRelInfo *resultRelInfo);

static int	pxfIsForeignRelUpdatable(Relation rel);

/*
 * Helper functions
 */
static void InitCopyState(PxfFdwScanState * pxfsstate);
static void InitCopyStateForModify(PxfFdwModifyState * pxfmstate);
static CopyState BeginCopyTo(Relation forrel, List *options);

/*
 * Foreign-data wrapper handler functions:
 * returns a struct with pointers to the
 * pxf_fdw callback routines.
 */
Datum
pxf_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdw_routine = makeNode(FdwRoutine);

	/*
	 * foreign table scan support
	 */

	/* master - only */
	fdw_routine->GetForeignRelSize = pxfGetForeignRelSize;
	fdw_routine->GetForeignPaths = pxfGetForeignPaths;
	fdw_routine->GetForeignPlan = pxfGetForeignPlan;
	fdw_routine->ExplainForeignScan = pxfExplainForeignScan;

	/* segment - only when mpp_execute = segments */
	fdw_routine->BeginForeignScan = pxfBeginForeignScan;
	fdw_routine->IterateForeignScan = pxfIterateForeignScan;
	fdw_routine->ReScanForeignScan = pxfReScanForeignScan;
	fdw_routine->EndForeignScan = pxfEndForeignScan;

	/*
	 * foreign table insert support
	 */

	/*
	 * AddForeignUpdateTargets set to NULL, no extra target expressions are
	 * added
	 */
	fdw_routine->AddForeignUpdateTargets = NULL;

	/*
	 * PlanForeignModify set to NULL, no additional plan-time actions are
	 * taken
	 */
	fdw_routine->PlanForeignModify = NULL;
	fdw_routine->BeginForeignModify = pxfBeginForeignModify;
	fdw_routine->ExecForeignInsert = pxfExecForeignInsert;

	/*
	 * ExecForeignUpdate and ExecForeignDelete set to NULL since updates and
	 * deletes are not supported
	 */
	fdw_routine->ExecForeignUpdate = NULL;
	fdw_routine->ExecForeignDelete = NULL;
	fdw_routine->EndForeignModify = pxfEndForeignModify;
	fdw_routine->IsForeignRelUpdatable = pxfIsForeignRelUpdatable;

	PG_RETURN_POINTER(fdw_routine);
}

/*
 * GetForeignRelSize
 *		set relation size estimates for a foreign table
 */
static void
pxfGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	elog(DEBUG5, "pxf_fdw: pxfGetForeignRelSize starts");

	/*
	 * Use an artificial number of estimated rows
	 */
	baserel->rows = 1000;

	elog(DEBUG5, "pxf_fdw: pxfGetForeignRelSize ends");
}

/*
 * GetForeignPaths
 *		create access path for a scan on the foreign table
 */
static void
pxfGetForeignPaths(PlannerInfo *root,
				   RelOptInfo *baserel,
				   Oid foreigntableid)
{
	ForeignPath *path = NULL;
	int			total_cost = DEFAULT_PXF_FDW_STARTUP_COST;


	elog(DEBUG5, "pxf_fdw: pxfGetForeignPaths starts");

	path = create_foreignscan_path(root, baserel,
#if PG_VERSION_NUM >= 90600
								   NULL,	/* default pathtarget */
#endif
								   baserel->rows,
								   DEFAULT_PXF_FDW_STARTUP_COST,
								   total_cost,
								   NIL, /* no pathkeys */
								   NULL,	/* no outer rel either */
#if PG_VERSION_NUM >= 90500
								   NULL,	/* no extra plan */
#endif
								   NIL);



	/*
	 * Create a ForeignPath node and add it as only possible path.
	 */
	add_path(baserel, (Path *) path);

	elog(DEBUG5, "pxf_fdw: pxfGetForeignPaths ends");
}

/*
 * GetForeignPlan
 *		create a ForeignScan plan node
 */
#if PG_VERSION_NUM >= 90500
static ForeignScan *
pxfGetForeignPlan(PlannerInfo *root,
				  RelOptInfo *baserel,
				  Oid foreigntableid,
				  ForeignPath *best_path,
				  List *tlist,
				  List *scan_clauses,
				  Plan *outer_plan)
#else

static ForeignScan *
pxfGetForeignPlan(PlannerInfo *root,
				  RelOptInfo *baserel,
				  Oid foreigntableid,
				  ForeignPath *best_path,
				  List *tlist,	/* target list */
				  List *scan_clauses)
#endif
{
	Index		scan_relid = baserel->relid;

	elog(DEBUG5, "pxf_fdw: pxfGetForeignPlan starts");

	/*
	 * We have no native ability to evaluate restriction clauses, so we just
	 * put all the scan_clauses into the plan node's qual list for the
	 * executor to check.  So all we have to do here is strip RestrictInfo
	 * nodes from the clauses and ignore pseudoconstants (which will be
	 * handled elsewhere).
	 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	elog(DEBUG5, "pxf_fdw: pxfGetForeignPlan ends");

	return make_foreignscan(tlist,
							scan_clauses,
							scan_relid,
							NIL,	/* no expressions to evaluate */
							NIL
#if PG_VERSION_NUM >= 90500
							,NIL
							,remote_exprs
							,outer_plan
#endif
		);

}

/*
 * pxfExplainForeignScan
 *		Produce extra output for EXPLAIN of a ForeignScan on a foreign table
 */
static void
pxfExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	elog(DEBUG5, "pxf_fdw: pxfExplainForeignScan starts on segment: %d", PXF_SEGMENT_ID);

	/* TODO: make this a meaningful callback */

	elog(DEBUG5, "pxf_fdw: pxfExplainForeignScan ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * BeginForeignScan
 *   called during executor startup. perform any initialization
 *   needed, but not start the actual scan.
 */
static void
pxfBeginForeignScan(ForeignScanState *node, int eflags)
{
	elog(DEBUG5, "pxf_fdw: pxfBeginForeignScan starts on segment: %d", PXF_SEGMENT_ID);

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	char	   *filterStr = NULL;
	List	   *quals = node->ss.ps.qual;
	Oid			foreigntableid = RelationGetRelid(node->ss.ss_currentRelation);
	ProjectionInfo *proj_info = node->ss.ps.ps_ProjInfo;
	PxfFdwScanState *pxfsstate = NULL;
	PxfOptions *options = NULL;
	Relation	relation = node->ss.ss_currentRelation;

	options = PxfGetOptions(foreigntableid);

	filterStr = SerializePxfFilterQuals(quals);

	/*
	 * Save state in node->fdw_state.  We must save enough information to call
	 * BeginCopyFrom() again.
	 */
	pxfsstate = (PxfFdwScanState *) palloc(sizeof(PxfFdwScanState));
	initStringInfo(&pxfsstate->uri);

	pxfsstate->filterStr = filterStr;
	pxfsstate->options = options;
	pxfsstate->proj_info = proj_info;
	pxfsstate->quals = quals;
	pxfsstate->fragments = GetFragmentList(pxfsstate->options,
										   relation,
										   filterStr,
										   pxfsstate->proj_info,
										   pxfsstate->quals);
	pxfsstate->relation = relation;

	InitCopyState(pxfsstate);
	node->fdw_state = (void *) pxfsstate;

	elog(DEBUG5, "pxf_fdw: pxfBeginForeignScan ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * IterateForeignScan
 *		Retrieve next row from the result set, or clear tuple slot to indicate
 *		EOF.
 *   Fetch one row from the foreign source, returning it in a tuple table slot
 *    (the node's ScanTupleSlot should be used for this purpose).
 *  Return NULL if no more rows are available.
 */
static TupleTableSlot *
pxfIterateForeignScan(ForeignScanState *node)
{
	elog(DEBUG5, "pxf_fdw: pxfIterateForeignScan Executing on segment: %d", PXF_SEGMENT_ID);

	PxfFdwScanState *pxfsstate = (PxfFdwScanState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	ErrorContextCallback errcallback;
	bool		found;


	/* Set up callback to identify error line number. */
	errcallback.callback = CopyFromErrorCallback;
	errcallback.arg = (void *) pxfsstate->cstate;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/*
	 * The protocol for loading a virtual tuple into a slot is first
	 * ExecClearTuple, then fill the values/isnull arrays, then
	 * ExecStoreVirtualTuple.  If we don't find another row in the file, we
	 * just skip the last step, leaving the slot empty as required.
	 *
	 * We can pass ExprContext = NULL because we read all columns from the
	 * file, so no need to evaluate default expressions.
	 *
	 * We can also pass tupleOid = NULL because we don't allow oids for
	 * foreign tables.
	 */
	ExecClearTuple(slot);

	found = NextCopyFrom(pxfsstate->cstate,
						 NULL,
						 slot_get_values(slot),
						 slot_get_isnull(slot),
						 NULL);
	if (found)
	{
		if (pxfsstate->cstate->cdbsreh)
		{
			/*
			 * If NextCopyFrom failed, the processed row count will have
			 * already been updated, but we need to update it in a successful
			 * case.
			 *
			 * GPDB_91_MERGE_FIXME: this is almost certainly not the right
			 * place for this, but row counts are currently scattered all over
			 * the place. Consolidate.
			 */
			pxfsstate->cstate->cdbsreh->processed++;
		}

		ExecStoreVirtualTuple(slot);
	}

	/* Remove error callback. */
	error_context_stack = errcallback.previous;

	return slot;
}

/*
 * ReScanForeignScan
 *		Restart the scan from the beginning
 */
static void
pxfReScanForeignScan(ForeignScanState *node)
{
	elog(DEBUG5, "pxf_fdw: pxfReScanForeignScan starts on segment: %d", PXF_SEGMENT_ID);

	PxfFdwScanState *pxfsstate = (PxfFdwScanState *) node->fdw_state;

	EndCopyFrom(pxfsstate->cstate);
	InitCopyState(pxfsstate);

	elog(DEBUG5, "pxf_fdw: pxfReScanForeignScan ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * EndForeignScan
 *		End the scan and release resources.
 */
static void
pxfEndForeignScan(ForeignScanState *node)
{
	elog(DEBUG5, "pxf_fdw: pxfEndForeignScan starts on segment: %d", PXF_SEGMENT_ID);

	ForeignScan *foreignScan = (ForeignScan *) node->ss.ps.plan;
	PxfFdwScanState *pxfsstate = (PxfFdwScanState *) node->fdw_state;

	/* Release resources */
	if (foreignScan->fdw_private)
	{
		elog(DEBUG5, "Freeing fdw_private");
		pfree(foreignScan->fdw_private);
	}

	/* if pxfsstate is NULL, we are in EXPLAIN; nothing to do */
	if (pxfsstate)
		EndCopyFrom(pxfsstate->cstate);

	elog(DEBUG5, "pxf_fdw: pxfEndForeignScan ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * pxfBeginForeignModify
 *		Begin an insert/update/delete operation on a foreign table
 */
static void
pxfBeginForeignModify(ModifyTableState *mtstate,
					  ResultRelInfo *resultRelInfo,
					  List *fdw_private,
					  int subplan_index,
					  int eflags)
{
	elog(DEBUG5, "pxf_fdw: pxfBeginForeignModify starts on segment: %d", PXF_SEGMENT_ID);

	PxfOptions *options = NULL;
	PxfFdwModifyState *pxfmstate = NULL;
	Relation	relation = resultRelInfo->ri_RelationDesc;
	TupleDesc	tupDesc;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	tupDesc = RelationGetDescr(relation);
	options = PxfGetOptions(RelationGetRelid(relation));
	pxfmstate = palloc(sizeof(PxfFdwModifyState));

	initStringInfo(&pxfmstate->uri);
	pxfmstate->relation = relation;
	pxfmstate->options = options;
	pxfmstate->values = (Datum *) palloc(tupDesc->natts * sizeof(Datum));
	pxfmstate->nulls = (bool *) palloc(tupDesc->natts * sizeof(bool));

	InitCopyStateForModify(pxfmstate);

	resultRelInfo->ri_FdwState = pxfmstate;

	elog(DEBUG5, "pxf_fdw: pxfBeginForeignModify ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * pxfExecForeignInsert
 *		Insert one row into a foreign table
 */
static TupleTableSlot *
pxfExecForeignInsert(EState *estate,
					 ResultRelInfo *resultRelInfo,
					 TupleTableSlot *slot,
					 TupleTableSlot *planSlot)
{
	elog(DEBUG5, "pxf_fdw: pxfExecForeignInsert starts on segment: %d", PXF_SEGMENT_ID);

	PxfFdwModifyState *pxfmstate = (PxfFdwModifyState *) resultRelInfo->ri_FdwState;
	CopyState	cstate = pxfmstate->cstate;
	Relation	relation = resultRelInfo->ri_RelationDesc;
	TupleDesc	tupDesc = RelationGetDescr(relation);
	HeapTuple	tuple = ExecMaterializeSlot(slot);
	Datum	   *values = pxfmstate->values;
	bool	   *nulls = pxfmstate->nulls;

	/* TEXT or CSV */
	heap_deform_tuple(tuple, tupDesc, values, nulls);
	CopyOneRowTo(cstate, HeapTupleGetOid(tuple), values, nulls);
	CopySendEndOfRow(cstate);

	StringInfo	fe_msgbuf = cstate->fe_msgbuf;

	int			bytes_written = PxfBridgeWrite(pxfmstate, fe_msgbuf->data, fe_msgbuf->len);

	if (bytes_written == -1)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to foreign resource: %m")));
	}

	elog(DEBUG3, "pxf_fdw %d bytes written", bytes_written);

	/* Reset our buffer to start clean next round */
	cstate->fe_msgbuf->len = 0;
	cstate->fe_msgbuf->data[0] = '\0';

	elog(DEBUG5, "pxf_fdw: pxfExecForeignInsert ends on segment: %d", PXF_SEGMENT_ID);
	return slot;
}

/*
 * pxfEndForeignModify
 *		Finish an insert/update/delete operation on a foreign table
 */
static void
pxfEndForeignModify(EState *estate,
					ResultRelInfo *resultRelInfo)
{
	elog(DEBUG5, "pxf_fdw: pxfEndForeignModify starts on segment: %d", PXF_SEGMENT_ID);

	PxfFdwModifyState *pxfmstate = (PxfFdwModifyState *) resultRelInfo->ri_FdwState;

	/* If pxfmstate is NULL, we are in EXPLAIN; nothing to do */
	if (pxfmstate == NULL)
		return;

	EndCopyFrom(pxfmstate->cstate);
	PxfBridgeCleanup(pxfmstate);

	elog(DEBUG5, "pxf_fdw: pxfEndForeignModify ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * pxfIsForeignRelUpdatable
 *  Assume table is updatable regardless of settings.
 *		Determine whether a foreign table supports INSERT, UPDATE and/or
 *		DELETE.
 */
static int
pxfIsForeignRelUpdatable(Relation rel)
{
	elog(DEBUG5, "pxf_fdw: pxfIsForeignRelUpdatable starts on segment: %d", PXF_SEGMENT_ID);
	elog(DEBUG5, "pxf_fdw: pxfIsForeignRelUpdatable ends on segment: %d", PXF_SEGMENT_ID);
	/* Only INSERTs are allowed at the moment */
	return (1 << CMD_INSERT) | (0 << CMD_UPDATE) | (0 << CMD_DELETE);
}

/*
 * Initiates a copy state for pxfBeginForeignScan() and pxfReScanForeignScan()
 */
static void
InitCopyState(PxfFdwScanState * pxfsstate)
{
	CopyState	cstate;

	PxfBridgeImportStart(pxfsstate);

	/*
	 * Create CopyState from FDW options.  We always acquire all columns, so
	 * as to match the expected ScanTupleSlot signature.
	 */
	cstate = BeginCopyFrom(pxfsstate->relation,
						   NULL,
						   false,	/* is_program */
						   &PxfBridgeRead,	/* data_source_cb */
						   pxfsstate,	/* data_source_cb_extra */
						   NIL, /* attnamelist */
						   pxfsstate->options->copy_options,	/* copy options */
						   NIL);	/* ao_segnos */


	if (pxfsstate->options->reject_limit == -1)
	{
		/* Default error handling - "all-or-nothing" */
		cstate->cdbsreh = NULL; /* no SREH */
		cstate->errMode = ALL_OR_NOTHING;
	}
	else
	{
		/* no error log by default */
		cstate->errMode = SREH_IGNORE;

		/* select the SREH mode */
		if (pxfsstate->options->log_errors)
			cstate->errMode = SREH_LOG; /* errors into file */

		cstate->cdbsreh = makeCdbSreh(pxfsstate->options->reject_limit,
									  pxfsstate->options->is_reject_limit_rows,
									  pxfsstate->options->resource,
									  (char *) cstate->cur_relname,
									  pxfsstate->options->log_errors);

		cstate->cdbsreh->relid = RelationGetRelid(pxfsstate->relation);
	}

	/* and 'fe_mgbuf' */
	cstate->fe_msgbuf = makeStringInfo();

	/*
	 * Create a temporary memory context that we can reset once per row to
	 * recover palloc'd memory.  This avoids any problems with leaks inside
	 * datatype input or output routines, and should be faster than retail
	 * pfree's anyway.
	 */
	cstate->rowcontext = AllocSetContextCreate(CurrentMemoryContext,
											   "PxfFdwMemCxt",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);

	pxfsstate->cstate = cstate;
}

/*
 * Initiates a copy state for pxfBeginForeignModify()
 */
static void
InitCopyStateForModify(PxfFdwModifyState * pxfmstate)
{
	List	   *copy_options;
	CopyState	cstate;

	copy_options = pxfmstate->options->copy_options;

	PxfBridgeExportStart(pxfmstate);

	/*
	 * Create CopyState from FDW options.  We always acquire all columns, so
	 * as to match the expected ScanTupleSlot signature.
	 */
	cstate = BeginCopyTo(pxfmstate->relation, copy_options);

	/* Initialize 'out_functions', like CopyTo() would. */

	TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
	Form_pg_attribute *attr = tupDesc->attrs;
	int			num_phys_attrs = tupDesc->natts;

	cstate->out_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	ListCell   *cur;

	foreach(cur, cstate->attnumlist)
	{
		int			attnum = lfirst_int(cur);
		Oid			out_func_oid;
		bool		isvarlena;

		getTypeOutputInfo(attr[attnum - 1]->atttypid,
						  &out_func_oid,
						  &isvarlena);
		fmgr_info(out_func_oid, &cstate->out_functions[attnum - 1]);
	}

	/* and 'fe_mgbuf' */
	cstate->fe_msgbuf = makeStringInfo();

	/*
	 * Create a temporary memory context that we can reset once per row to
	 * recover palloc'd memory.  This avoids any problems with leaks inside
	 * datatype input or output routines, and should be faster than retail
	 * pfree's anyway.
	 */
	cstate->rowcontext = AllocSetContextCreate(CurrentMemoryContext,
											   "PxfFdwMemCxt",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);

	pxfmstate->cstate = cstate;
}

/*
 * Set up CopyState for writing to an foreign table.
 */
static CopyState BeginCopyTo(Relation forrel, List *options)
{
	CopyState	cstate;

	Assert(RelationIsForeign(forrel));

	cstate = BeginCopy(false, forrel, NULL, NULL, NIL, options, NULL);
	cstate->dispatch_mode = COPY_DIRECT;

	/*
	 * We use COPY_CALLBACK to mean that the each line should be
	 * left in fe_msgbuf. There is no actual callback!
	 */
	cstate->copy_dest = COPY_CALLBACK;

	/*
	 * Some more initialization, that in the normal COPY TO codepath, is done
	 * in CopyTo() itself.
	 */
	cstate->null_print_client = cstate->null_print;		/* default */
	if (cstate->need_transcoding)
		cstate->null_print_client = pg_server_to_custom(cstate->null_print,
		                                                cstate->null_print_len,
		                                                cstate->file_encoding,
		                                                cstate->enc_conversion_proc);

	return cstate;
}