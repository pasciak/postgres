#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/copyrel.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/planner.h"
#include "parser/parse_relation.h"
#include "nodes/makefuncs.h"
#include "rewrite/rewriteHandler.h"
#include "storage/fd.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"



/*

Ideas:

AIM: To use and to merge CopyTo and CopyFrom to COPY data between relations. 
We want to communicate with the foreign data wrapper and 
COPY to and from it. FDW that is the external relation 
that allows us to access non-Postgre data from within postgres environment. 

WHAT DO WE HAVE IN POSTGRES SOURCECODE:

( The initial NOTE: The file in /doc/copyreplan.c  (lets call it WORKPLAN from now on
) keeps the functions which were taken
out of the main copy.c file and that might serve useful for the implementation of functionality at this moment. Most functions are truncated , I usually remove 
binary, encoding etc checks as I find them not necessary at this stage. We are working 
on the basic functionality at this stage.



-- CopyTo takes the data from relation (call it relFrom) and moves it to the
file (see the top half of WORKPLAN for essential funcions associated with it)

-- CopyFrom takes the data from file and stores is in
the relation (call it relIN)  (see the bottom half of WORKPLAN for essential funcions associated with it)


Ideally, one would want Relation  --> CopyRel --> Relation,  where CopyRel is a COPY function
that moves data directly between relations. However, this can be tough ( some implementation
I have made up to the 12.07.2016 aimed at this, but after catchup it is time to reconsider
the approach )

-- Hence, consider the CopyTo --> File --> Copyfrom first. In this scenario, we take the
relation, copy it to file, then the file is accessed by the CopyFrom statement that
takes data and stores it to a new relation,

This is slow and has too many steps. It should be possible to do something like this

CopyTo --> Temporary file in memory --> Copyfrom

Hence we dont save the file to disc but create the temporary 'file'/relation in ram memory
that can be accessed from both ways (read/write)



 */


typedef enum CopyDest
{
	COPY_RELATION,					/* to/from file (or a piped program) */
	COPY_FILE,
	COPY_NEW_FE	

} CopyDest;

/*
Not needed for now
*/

typedef enum EolType
{
	EOL_UNKNOWN,
	EOL_NL,
	EOL_CR,
	EOL_CRNL
} EolType;


/* 

DestReceiver for COPY (query) TO , because the COPY __ TO  needs to go somewhere

NOTE: lookup dest.c and DestReceiver() function cases , each of them depending
on the tyoe of destination container. We want relation, dont we?

*/

typedef struct
{
    DestReceiver pub;           /* publicly-known function pointers */
    CopyState   cstate;         /* CopyStateData for the command */
    uint64      processed;      /* # of tuples processed */

} DR_copy;
  


/*

copystate from adopted from copy.c , minor additions here

Note that I have added a new structure in the parsenodes.h called CopyRelStmt, which
is similar ot CopyStmt but has additional fields called relation_from and relation_in

*/

typedef struct CopyStateData
{


	Relation	relFrom;			/* source relation */
	Relation	relIn;			/* destination relation, this could FDW */

		/* low-level state data */

	CopyDest	copy_dest;		/* type of copy source/destination */
	FILE	   *copy_file;		/* used if copy_dest == COPY_FILE */
	StringInfo	fe_msgbuf;		/* used for all dests during COPY TO, only for
								 * dest == COPY_NEW_FE in COPY FROM */
	bool		fe_eof;			/* true if detected end of copy data */
	EolType		eol_type;		/* EOL type of input */
	int			file_encoding;	/* file or remote side's character encoding */
	bool		need_transcoding;		/* file encoding diff from server? */
	bool		encoding_embeds_ascii;	/* ASCII can be non-first byte? */

	/* parameters from the COPY command */
	Relation	rel;			/* relation to copy to or from */
	QueryDesc  *queryDesc;		/* executable query to copy from */
	List	   *attnumlist;		/* integer list of attnums to copy */
	char	   *filename;		/* filename, or NULL for STDIN/STDOUT */
	bool		is_program;		/* is 'filename' a program to popen? */
	bool		binary;			/* binary format? */
	bool		oids;			/* include OIDs? */
	bool		freeze;			/* freeze rows on loading? */
	bool		csv_mode;		/* Comma Separated Value format? */
	bool		header_line;	/* CSV header line? */
	char	   *null_print;		/* NULL marker string (server encoding!) */
	int			null_print_len; /* length of same */
	char	   *null_print_client;		/* same converted to file encoding */
	char	   *delim;			/* column delimiter (must be 1 byte) */
	char	   *quote;			/* CSV quote char (must be 1 byte) */
	char	   *escape;			/* CSV escape char (must be 1 byte) */
	List	   *force_quote;	/* list of column names */
	bool		force_quote_all;	/* FORCE QUOTE *? */
	bool	   *force_quote_flags;		/* per-column CSV FQ flags */
	List	   *force_notnull;	/* list of column names */
	bool	   *force_notnull_flags;	/* per-column CSV FNN flags */
	List	   *force_null;		/* list of column names */
	bool	   *force_null_flags;		/* per-column CSV FN flags */
	bool		convert_selectively;	/* do selective binary conversion? */
	List	   *convert_select; /* list of column names (can be NIL) */
	bool	   *convert_select_flags;	/* per-column CSV/TEXT CS flags */

	/* these are just for error messages, see CopyFromErrorCallback */
	const char *cur_relname;	/* table name for error messages */
	int			cur_lineno;		/* line number for error messages */
	const char *cur_attname;	/* current att for error messages */
	const char *cur_attval;		/* current att value for error messages */

	/*
	 * Working state for COPY TO/FROM
	 */
	MemoryContext copycontext;	/* per-copy execution context */

	/*
	 * Working state for COPY TO
	 */
	FmgrInfo   *out_functions;	/* lookup info for output functions */
	MemoryContext rowcontext;	/* per-row evaluation context */

	/*
	 * Working state for COPY FROM
	 */
	AttrNumber	num_defaults;
	bool		file_has_oids;
	FmgrInfo	oid_in_function;
	Oid			oid_typioparam;
	FmgrInfo   *in_functions;	/* array of input functions for each attrs */
	Oid		   *typioparams;	/* array of element types for in_functions */
	int		   *defmap;			/* array of default att numbers */
	ExprState **defexprs;		/* array of default att expressions */
	bool		volatile_defexprs;		/* is any of defexprs volatile? */
	List	   *range_table;

	/*
	 * These variables are used to reduce overhead in textual COPY FROM.
	 *
	 * attribute_buf holds the separated, de-escaped text for each field of
	 * the current line.  The CopyReadAttributes functions return arrays of
	 * pointers into this buffer.  We avoid palloc/pfree overhead by re-using
	 * the buffer on each cycle.
	 */
	StringInfoData attribute_buf;

	/* field raw data pointers found by COPY FROM */

	int			max_fields;
	char	  **raw_fields;

	/*
	 * Similarly, line_buf holds the whole input line being processed. The
	 * input cycle is first to read the whole line into line_buf, convert it
	 * to server encoding there, and then extract the individual attribute
	 * fields into attribute_buf.  line_buf is preserved unmodified so that we
	 * can display it in error messages if appropriate.
	 */
	StringInfoData line_buf;
	bool		line_buf_converted;		/* converted to server encoding? */
	bool		line_buf_valid; /* contains the row being processed? */

	/*
	 * Finally, raw_buf holds raw data read from the data source (file or
	 * client connection).  CopyReadLine parses this data sufficiently to
	 * locate line boundaries, then transfers the data to line_buf and
	 * converts it.  Note: we guarantee that there is a \0 at
	 * raw_buf[raw_buf_len].
	 */
#define RAW_BUF_SIZE 65536		/* we palloc RAW_BUF_SIZE+1 bytes */
	char	   *raw_buf;
	int			raw_buf_index;	/* next byte to process */
	int			raw_buf_len;	/* total # of bytes stored */


} CopyStateData;

// Function prototypes

static List *CopyGetAttnums(TupleDesc tupDesc, Relation rel,
			   List *attnamelist);

static CopyState PrepareCopyRel(Relation relFrom, Relation relIn, Node *raw_query,
		  const char *queryString, const Oid queryRelId, const Oid queryRelId2 ,List *attnamelist, List *options);

static CopyState BeginCopyRel(Relation relFrom, Relation relIn, Node *query,
		  const char *queryString, const Oid queryRelId, const Oid queryRelId2 ,List *attnamelist, List *options);


/********************************************************************************/

/*

main function that is called from utility.c and performs
the copy between relations

CopyRelStmt -> see parsenodes.h for definition 

Load contents of one table into other table, and exectute
the arbitrary statement query

*/

Oid DoCopyRel(const CopyRelStmt *stmt, const char *queryString, uint64 *processed){ 

	CopyState	cstate;

// we consider two relations in this case;

	Relation	relFrom;
	Relation 	relIn;	
	Oid			relid;
	Oid			relid2;

	Node	   *query = NULL;
	List	   *range_table = NIL;

	Assert((stmt->relation_in) && (!stmt->query || !stmt->relation_from));

/* 

the standard procedure for the permission check, only the superuser can use the copy function

*/
	if (!superuser())
			{
			ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to use the COPY function"),
				errhint("")));
	}

	if (!stmt->query)

	{

		Assert(stmt->relation_from);

/* open and lock tables, the lock type depending on
whether we read or write to table */

		relFrom = heap_openrv(stmt->relation_from,
						 AccessShareLock);
		relid=RelationGetRelid(relFrom);

		relIn = heap_openrv(stmt->relation_in,
						  RowExclusiveLock);
		relid2=RelationGetRelid(relIn);

	}

	else if (stmt->query)
	{

		Assert(!stmt->relation_from);		
		query = stmt->query;
		relid = InvalidOid;
		relFrom = NULL;

		Assert(stmt->relation_in);
	/*
		we are using SELECT statement to load data
		into relation_in table, hence we need to 
		lock this table for concurrent use

	*/	
		relIn = heap_openrv(stmt->relation_in,
						  RowExclusiveLock);
		relid2=RelationGetRelid(relIn);
	}
		
	cstate = PrepareCopyRel(relFrom, relIn, query, queryString, relid, relid2, stmt->attlist, stmt->options);




//	*processed = ProcessCopyRel(cstate);	/* copy from database to file */
//	EndCopyRel(cstate);

 	if (relFrom != NULL)
		heap_close(relFrom, AccessShareLock);
	if (relIn != NULL)
		heap_close(relIn, NoLock);

	elog(LOG, "123");

	return relid;

}


/*

This function prepares the copystate 

*/


static CopyState PrepareCopyRel(Relation relFrom, Relation relIn, Node *raw_query,
		  const char *queryString, const Oid queryRelId, const Oid queryRelId2 ,List *attnamelist,
		  List *options)
{


/*

All _from are source, all _in are target objects

*/	

	CopyState	cstate;
	TupleDesc	tupDescFrom;
	TupleDesc	tupDescIn;

	int			num_phys_attrs_from;
	int			num_phys_attrs_in;

	MemoryContext oldcontext;

	Query	   *query;


	/* Allocate workspace and zero all fields */
	cstate = (CopyStateData *) palloc0(sizeof(CopyStateData));

	/*
	 * We allocate everything used by a cstate in a new memory context. This
	 * avoids memory leaks during repeated use of COPY in a query.
	 */
	cstate->copycontext = AllocSetContextCreate(CurrentMemoryContext,
												"COPYREL",
												ALLOCSET_DEFAULT_MINSIZE,
												ALLOCSET_DEFAULT_INITSIZE,
												ALLOCSET_DEFAULT_MAXSIZE);

	oldcontext = MemoryContextSwitchTo(cstate->copycontext);

	/* Process ooptions, do it later!  */

	//ProcessCopyOptions(cstate, is_from, options);
/*

If no SELECT passed, directly get info about both relations under consideration

*/
	if (relFrom!=NULL){

		Assert(!raw_query);

		cstate->relFrom=relFrom;
		cstate->relIn=relIn;
		tupDescFrom = RelationGetDescr(cstate->relFrom);
		tupDescIn = RelationGetDescr(cstate->relIn);

		query=NULL;

// make the OID check here ?

	}

	else if (relFrom==NULL){

/*

	set up the querry-modified relFrom and the relIn 

*/

		List	     *rewritten;
		PlannedStmt  *plan;
		DestReceiver *dest;

		Assert(raw_query);

		cstate->relFrom = NULL;

		if (cstate->oids)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY (SELECT) WITH OIDS is not supported")));


		cstate->relIn=relIn;
		tupDescIn = RelationGetDescr(cstate->relIn);


		rewritten = pg_analyze_and_rewrite((Node *) copyObject(raw_query),
										   queryString, NULL, 0);

		if (list_length(rewritten) != 1)
			elog(ERROR, "unexpected rewrite result");

		query = (Query *) linitial(rewritten);

		Assert(query->commandType == CMD_SELECT);
		Assert(query->utilityStmt == NULL);


		plan = planner(query, 0, NULL);

/*

We have the querry planned, and now we wanr to convert the COPY relation TO relation
to a query based COPY statement

Note that at the beginning of DoCopy the relations are locked. The planner will
search for the locked relation again, and so we need to make sure that
it now finds the same relation 

*/

		if (queryRelId != InvalidOid)
		{
			
			if (!list_member_oid(plan->relationOids, queryRelId))
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("relation referenced by COPY statement has changed")));
		}


/*

Here we prepare the snapshot. In other words, the SELECT statement derives 
a modified source that consist of only the rows under consideration.


*/
		PushCopiedSnapshot(GetActiveSnapshot());
		UpdateActiveSnapshotCommandId();

		/* 

		Create dest receiver for COPY OUT 

		Need to figure this out: in the original copy.c, COPY query TO
		sends data to file. In here, we send data to another relation.
		Should we change the argument that the CreateDestReveiver gets, so 
		that the

		   case DestIntoRel:
               return CreateIntoRelDestReceiver(NULL);

        case from the dest.c is called?      
   

		*/

		dest = CreateDestReceiver(DestCopyOut);
		((DR_copy *) dest)->cstate = cstate;

		/* Create a QueryDesc requesting no output */
		cstate->queryDesc = CreateQueryDesc(plan, queryString,
											GetActiveSnapshot(),
											InvalidSnapshot,
											dest, NULL, 0);

		/*
		 * Call ExecutorStart to prepare the plan for execution.
		 *
		 * ExecutorStart computes a result tupdesc for us
		 */
		ExecutorStart(cstate->queryDesc, 0);

/*

Hence we get the query-modfiied set of tuples 

*/

		tupDescFrom = cstate->queryDesc->tupDesc;

	}


	cstate->attnumlist = CopyGetAttnums(tupDescFrom, cstate->relFrom, attnamelist);

	num_phys_attrs_from = tupDescFrom->natts;


	MemoryContextSwitchTo(oldcontext);

	return cstate;
}

/*
 * CopyGetAttnums - build an integer list of attnums to be copied
 *
 * The input attnamelist is either the user-specified column list,
 * or NIL if there was none (in which case we want all the non-dropped
 * columns).
 *
 * rel can be NULL ... it's only used for error reports.
 * 
 *	Note that we want this only for the relFrom case, as this the 
 *	relation that . Hence for now this function will be left unmodified, as in copy.c
 *
 */

static List *
CopyGetAttnums(TupleDesc tupDesc, Relation rel, List *attnamelist)
{
	List	   *attnums = NIL;

	if (attnamelist == NIL)
	{
		/* Generate default column list */

		Form_pg_attribute *attr = tupDesc->attrs;
		int			attr_count = tupDesc->natts;
		int			i;

		for (i = 0; i < attr_count; i++)
		{
			if (attr[i]->attisdropped)
				continue;
			attnums = lappend_int(attnums, i + 1);
		}
	}
	else
	{
		/* Validate the user-supplied list and extract attnums */
		ListCell   *l;

		foreach(l, attnamelist)
		{
			char	   *name = strVal(lfirst(l));
			int			attnum;
			int			i;

			/* Lookup column name */
			attnum = InvalidAttrNumber;
			for (i = 0; i < tupDesc->natts; i++)
			{
				if (tupDesc->attrs[i]->attisdropped)
					continue;
				if (namestrcmp(&(tupDesc->attrs[i]->attname), name) == 0)
				{
					attnum = tupDesc->attrs[i]->attnum;
					break;
				}
			}
			if (attnum == InvalidAttrNumber)
			{
				if (rel != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
					errmsg("column \"%s\" of relation \"%s\" does not exist",
						   name, RelationGetRelationName(rel))));
				else
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" does not exist",
									name)));
			}
			/* Check for duplicates */
			if (list_member_int(attnums, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column \"%s\" specified more than once",
								name)));
			attnums = lappend_int(attnums, attnum);
		}
	}

	return attnums;
}