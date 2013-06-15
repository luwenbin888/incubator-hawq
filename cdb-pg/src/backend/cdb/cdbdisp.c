/*-------------------------------------------------------------------------
 *
 * cdbdisp.c
 *	  Functions to dispatch commands to QExecutors.
 *
 *
 * Copyright (c) 2005-2008, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include <pthread.h>
#include <limits.h>

#include "access/catquery.h"
#include "executor/execdesc.h"	/* QueryDesc */
#include "storage/ipc.h"		/* For proc_exit_inprogress  */
#include "miscadmin.h"
#include "utils/memutils.h"

#include "utils/tqual.h" 			/*for the snapshot */
#include "storage/proc.h"  			/* MyProc */
#include "storage/procarray.h"      /* updateSharedLocalSnapshot */
#include "access/xact.h"  			/*for GetCurrentTransactionId */


#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/faultinjector.h"
#include "executor/executor.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "tcop/tcopprot.h"
#include "cdb/cdbplan.h"
#include "postmaster/syslogger.h"

#include "cdb/cdbselect.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbdispatchresult.h"
#include "cdb/cdbfts.h"
#include "cdb/cdbgang.h"
#include "cdb/cdblink.h"		/* just for our CdbProcess population hack. */
#include "cdb/cdbsrlz.h"
#include "cdb/cdbsubplan.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbdtxcontextinfo.h"
#include "cdb/cdbllize.h"
#include "cdb/cdbsreh.h"
#include "cdb/cdbrelsize.h"
#include "gp-libpq-fe.h"
#include "libpq/libpq-be.h"
#include "commands/vacuum.h" /* VUpdatedStats */
#include "cdb/cdbanalyze.h"  /* cdbanalyze_get_columnstats */

#include "parser/parsetree.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "utils/builtins.h"
#include "utils/portal.h"

#include "cdb/cdbinmemheapam.h"

extern bool Test_print_direct_dispatch_info;

extern pthread_t main_tid;
#ifndef _WIN32
#define mythread() ((unsigned long) pthread_self())
#else
#define mythread() ((unsigned long) pthread_self().p)
#endif 

/*
 * default directed-dispatch parameters: don't direct anything.
 */
CdbDispatchDirectDesc default_dispatch_direct_desc = {false, 0, {0}};

/*
 * Static Helper functions
 */

static void *thread_DispatchCommand(void *arg);
static bool thread_DispatchOut(DispatchCommandParms		*pParms);
static void thread_DispatchWait(DispatchCommandParms	*pParms);
static void thread_DispatchWaitSingle(DispatchCommandParms		*pParms);

static void
CdbCheckDispatchResultInt(struct CdbDispatcherState *ds,
						  struct SegmentDatabaseDescriptor   ***failedSegDB,
                          int          *numOfFailed,
                          bool          cancelUnfinishedWork);

static bool
shouldStillDispatchCommand(DispatchCommandParms *pParms, CdbDispatchResult * dispatchResult);

static
void dispatchCommandQuery(CdbDispatchResult *dispatchResult,
                     const char        *query_text,
                     int 			  	query_len,
                     const char        *debug_text);
							
static
void dispatchCommandDtxProtocol(CdbDispatchResult *dispatchResult,
                     const char        *query_text,
                     int 			  	query_len,
                     DtxProtocolCommand dtxProtocolCommand);
							
static void
handleSelectError(DispatchCommandParms *pParms,
                  int                   db_count,
                  int                   sock_errno);							

static void
handleSelectTimeout(DispatchCommandParms   *pParms,
                    int                     db_count,
                    int                    *timeoutCounter,
                    bool                    useSampling);

static void
CollectQEWriterTransactionInformation(SegmentDatabaseDescriptor *segdbDesc, CdbDispatchResult * dispatchResult);


static bool                     /* returns true if command complete */
processResults(CdbDispatchResult   *dispatchResult);


static void
addSegDBToDispatchThreadPool(DispatchCommandParms  *ParmsAr,
                             int                    segdbs_in_thread_pool,
							 GpDispatchCommandType	mppDispatchCommandType,
							 void				   *commandTypeParms,
							 int					sliceId,
							 CdbDispatchResult     *dispatchResult);

static void
cdbdisp_dispatchCommandToAllGangs(const char	*strCommand,
						char					*serializedQuerytree,
						int						serializedQuerytreelen,
						char					*serializedPlantree,
						int						serializedPlantreelen,
                        bool					cancelOnError,
                        bool					needTwoPhase,
                        struct CdbDispatcherState *ds);

static void
bindCurrentOfParams(char *cursor_name, 
					Oid target_relid, 
					ItemPointer ctid, 
					int *gp_segment_id, 
					Oid *tableoid);

#define BEGIN_STR "BEGIN "
#define PREPARE_STR "PREPARE "
#define COMMIT_STR "COMMIT "
#define ROLLBACK_STR "ROLLBACK "
//#define PG_RELATION_SIZE_STR "select pg_relation_size("
//#define PG_HIGHEST_OID_STR "select pg_highest_oid("
#define MPPEXEC_CAP_STR "MPPEXEC "
#define MPPEXEC_STR "mppexec "

#define GP_PARTITION_SELECTION_OID 6084
#define GP_PARTITION_EXPANSION_OID 6085
#define GP_PARTITION_INVERSE_OID 6086

 
/*
 * Clear our "active" flags; so that we know that the writer gangs are busy -- and don't stomp on
 * internal dispatcher structures. See MPP-6253 and MPP-6579.
 */
static void
cdbdisp_clearGangActiveFlag(CdbDispatcherState *ds)
{
	if (ds && ds->primaryResults && ds->primaryResults->writer_gang)
	{
		ds->primaryResults->writer_gang->dispatcherActive = false;
	}
}

/*
 * We need an array describing the relationship between a slice and
 * the number of "child" slices which depend on it.
 */
typedef struct {
	int sliceIndex;
	int children;
	Slice *slice;
} sliceVec;

static int fillSliceVector(SliceTable * sliceTable, int sliceIndex, sliceVec *sliceVector, int len);

/* determines which dispatchOptions need to be set. */
/*static int generateTxnOptions(bool needTwoPhase);*/

typedef struct
{
	plan_tree_base_prefix base; /* Required prefix for plan_tree_walker/mutator */
	bool		single_row_insert;
}	pre_dispatch_function_evaluation_context;

static Node *pre_dispatch_function_evaluation_mutator(Node *node,
						 pre_dispatch_function_evaluation_context * context);

static void
CdbDispatchUtilityStatement_Internal(struct Node *stmt,
									 QueryContextInfo *contextdisp,
									 bool needTwoPhase, bool checkSendback, char* debugCaller);

/* 
 * ====================================================
 * STATIC STATE VARIABLES should not be declared!
 * global state will break the ability to run cursors.
 * only globals with a higher granularity than a running
 * command (i.e: transaction, session) are ok.
 * ====================================================
 */

static DtxContextInfo TempQDDtxContextInfo = DtxContextInfo_StaticInit;

static MemoryContext DispatchContext = NULL;

/*
 * We can't use elog to write to the log if we are running in a thread.
 * 
 * So, write some thread-safe routines to write to the log.
 * 
 * Ugly:  This write in a fixed format, and ignore what the log_prefix guc says.
 */
static pthread_mutex_t send_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef WIN32
static void
write_eventlog(int level, const char *line);
/*
 * Write a message line to the windows event log
 */
static void
write_eventlog(int level, const char *line)
{
	int			eventlevel = EVENTLOG_ERROR_TYPE;
	static HANDLE evtHandle = INVALID_HANDLE_VALUE;

	if (evtHandle == INVALID_HANDLE_VALUE)
	{
		evtHandle = RegisterEventSource(NULL, "PostgreSQL");
		if (evtHandle == NULL)
		{
			evtHandle = INVALID_HANDLE_VALUE;
			return;
		}
	}

	ReportEvent(evtHandle,
				eventlevel,
				0,
				0,				/* All events are Id 0 */
				NULL,
				1,
				0,
				&line,
				NULL);
}
#endif   /* WIN32 */

void get_timestamp(char * strfbuf, int length)
{
	pg_time_t		stamp_time;
	char			msbuf[8];
	struct timeval tv;

	gettimeofday(&tv, NULL);
	stamp_time = tv.tv_sec;

	pg_strftime(strfbuf, length,
	/* leave room for microseconds... */
	/* Win32 timezone names are too long so don't print them */
#ifndef WIN32
			 "%Y-%m-%d %H:%M:%S        %Z",
#else
			 "%Y-%m-%d %H:%M:%S        ",
#endif
			 pg_localtime(&stamp_time, log_timezone ? log_timezone : gmt_timezone));

	/* 'paste' milliseconds into place... */
	sprintf(msbuf, ".%06d", (int) (tv.tv_usec));
	strncpy(strfbuf + 19, msbuf, 7);	
}

void
write_log(const char *fmt,...)
{
	char logprefix[1024];
	char tempbuf[25];
	va_list		ap;

	fmt = _(fmt);

	va_start(ap, fmt);

	if (Redirect_stderr && gp_log_format == 1)
	{
		char		errbuf[2048]; /* Arbitrary size? */
		
		vsnprintf(errbuf, sizeof(errbuf), fmt, ap);

		/* Write the message in the CSV format */
		write_message_to_server_log(LOG,
									0,
									errbuf,
									NULL,
									NULL,
									NULL,
									0,
									0,
									NULL,
									NULL,
									NULL,
									false,
									NULL,
									0,
									0,
									true,
									/* This is a real hack... We want to send alerts on these errors, but we aren't using ereport() */
									strstr(errbuf, "Master unable to connect") != NULL ||
									strstr(errbuf, "Found a fault with a segment") != NULL,
									NULL);

		va_end(ap);
		return;
	}
	
	get_timestamp(logprefix, sizeof(logprefix));
	strcat(logprefix,"|");
	if (MyProcPort)
	{
		const char *username = MyProcPort->user_name;
		if (username == NULL || *username == '\0')
			username = "";
		strcat(logprefix,username); /* user */
	}
	
	strcat(logprefix,"|");
	if (MyProcPort)
	{
		const char *dbname = MyProcPort->database_name;

		if (dbname == NULL || *dbname == '\0')
			dbname = "";
		strcat(logprefix, dbname);
	}
	strcat(logprefix,"|");
	sprintf(tempbuf,"%d",MyProcPid);
	strcat(logprefix,tempbuf); /* pid */
	strcat(logprefix,"|");
	sprintf(tempbuf,"con%d cmd%d",gp_session_id,gp_command_count);
	strcat(logprefix,tempbuf);

	strcat(logprefix,"|");
	strcat(logprefix,":-THREAD ");
	if (pthread_equal(main_tid, pthread_self()))
		strcat(logprefix,"MAIN");
	else
	{
		sprintf(tempbuf,"%lu",mythread());
		strcat(logprefix,tempbuf);
	}
	strcat(logprefix,":  ");
	
	strcat(logprefix,fmt);
	
	if (fmt[strlen(fmt)-1]!='\n')
		strcat(logprefix,"\n");
	 
	/*
	 * We don't trust that vfprintf won't get confused if it 
	 * is being run by two threads at the same time, which could
	 * cause interleaved messages.  Let's play it safe, and
	 * make sure only one thread is doing this at a time.
	 */
	pthread_mutex_lock(&send_mutex);
#ifndef WIN32
	/* On Unix, we just fprintf to stderr */
	vfprintf(stderr, logprefix, ap);
    fflush(stderr);
#else

	/*
	 * On Win32, we print to stderr if running on a console, or write to
	 * eventlog if running as a service
	 */
	if (pgwin32_is_service())	/* Running as a service */
	{
		char		errbuf[2048];		/* Arbitrary size? */

		vsnprintf(errbuf, sizeof(errbuf), logprefix, ap);

		write_eventlog(EVENTLOG_ERROR_TYPE, errbuf);
	}
	else
    {
        /* Not running as service, write to stderr */
        vfprintf(stderr, logprefix, ap);
        fflush(stderr);
    }
#endif
	pthread_mutex_unlock(&send_mutex);
	va_end(ap);
}


/*--------------------------------------------------------------------*/


/*
 * cdbdisp_dispatchToGang:
 * Send the strCommand SQL statement to the subset of all segdbs in the cluster
 * specified by the gang parameter.  cancelOnError indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during cdblink_setup.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable gp_connections_per_thread.
 *
 * The caller must provide a CdbDispatchResults object having available
 * resultArray slots sufficient for the number of QEs to be dispatched:
 * i.e., resultCapacity - resultCount >= gp->size.	This function will
 * assign one resultArray slot per QE of the Gang, paralleling the Gang's
 * db_descriptors array.  Success or failure of each QE will be noted in
 * the QE's CdbDispatchResult entry; but before examining the results, the
 * caller must wait for execution to end by calling CdbCheckDispatchResult().
 *
 * The CdbDispatchResults object owns some malloc'ed storage, so the caller
 * must make certain to free it by calling cdbdisp_destroyDispatchResults().
 *
 * When dispatchResults->cancelOnError is false, strCommand is to be
 * dispatched to every connected gang member if possible, despite any
 * cancellation requests, QE errors, connection failures, etc.
 *
 * This function is passing out a pointer to the newly allocated
 * CdbDispatchCmdThreads object. It holds the dispatch thread information
 * including an array of dispatch thread commands. It gets destroyed later
 * on when the command is finished, along with the DispatchResults objects.
 *
 * NB: This function should return normally even if there is an error.
 * It should not longjmp out via elog(ERROR, ...), ereport(ERROR, ...),
 * PG_THROW, CHECK_FOR_INTERRUPTS, etc.
 *
 * Note: the maxSlices argument is used to allocate the parameter
 * blocks for dispatch, it should be set to the maximum number of
 * slices in a plan. For dispatch of single commands (ie most uses of
 * cdbdisp_dispatchToGang()), setting it to 1 is fine.
 */
void
cdbdisp_dispatchToGang(struct CdbDispatcherState *ds,
					   GpDispatchCommandType		mppDispatchCommandType,
					   void						   *commandTypeParms,
                       struct Gang                 *gp,
                       int                          sliceIndex,
                       unsigned int                 maxSlices,
                       CdbDispatchDirectDesc		*disp_direct)
{
	struct CdbDispatchResults	*dispatchResults = ds->primaryResults;
	SegmentDatabaseDescriptor	*segdbDesc;
	int	i,
		max_threads,
		segdbs_in_thread_pool = 0,
		x,
		newThreads = 0;	
	int db_descriptors_size;
	SegmentDatabaseDescriptor *db_descriptors;

	MemoryContext oldContext;

	Assert(Gp_role == GP_ROLE_DISPATCH);
	Assert(gp && gp->size > 0);
	Assert(dispatchResults && dispatchResults->resultArray);

	if (dispatchResults->writer_gang)
	{
		/* Are we dispatching to the writer-gang when it is already busy ? */
		if (gp == dispatchResults->writer_gang)
		{
			if (dispatchResults->writer_gang->dispatcherActive)
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Cannot dispatch multiple queries to the segments."),
						 errhint("Likely caused by a function that reads or modifies data in a distributed table.")));
			}
			dispatchResults->writer_gang->dispatcherActive = true;
		}
	}

	switch (mppDispatchCommandType)
	{
		case GP_DISPATCH_COMMAND_TYPE_QUERY:
		{
			DispatchCommandQueryParms *pQueryParms = (DispatchCommandQueryParms *) commandTypeParms;

			Assert(pQueryParms->strCommand != NULL);

			if (DEBUG2 >= log_min_messages)
			{
				if (sliceIndex >= 0)
					elog(DEBUG2, "dispatchToGang: sliceIndex=%d gangsize=%d  %.100s",
						 sliceIndex, gp->size, pQueryParms->strCommand);
				else
					elog(DEBUG2, "dispatchToGang: gangsize=%d  %.100s",
						 gp->size, pQueryParms->strCommand);
			}
		}
		break;

		case GP_DISPATCH_COMMAND_TYPE_DTX_PROTOCOL:
		{
			DispatchCommandDtxProtocolParms *pDtxProtocolParms = (DispatchCommandDtxProtocolParms *) commandTypeParms;
			
			Assert(pDtxProtocolParms->dtxProtocolCommandLoggingStr != NULL);
			Assert(pDtxProtocolParms->gid != NULL);
			
			if (DEBUG2 >= log_min_messages)
			{
				if (sliceIndex >= 0)
					elog(DEBUG2, "dispatchToGang: sliceIndex=%d gangsize=%d  dtxProtocol = %s, gid = %s",
						 sliceIndex, gp->size, pDtxProtocolParms->dtxProtocolCommandLoggingStr, pDtxProtocolParms->gid);
				else
					elog(DEBUG2, "dispatchToGang: gangsize=%d  dtxProtocol = %s, gid = %s",
						 gp->size, pDtxProtocolParms->dtxProtocolCommandLoggingStr, pDtxProtocolParms->gid);
			}
		}
		break;

		default:
			elog(FATAL, "Unrecognized MPP dispatch command type: %d",
				 (int) mppDispatchCommandType);
	}

	db_descriptors_size = gp->size;
	db_descriptors = gp->db_descriptors;
	
	/*
	 * The most threads we could have is segdb_count / gp_connections_per_thread, rounded up.
	 * This is equivalent to 1 + (segdb_count-1) / gp_connections_per_thread.
	 * We allocate enough memory for this many DispatchCommandParms structures,
	 * even though we may not use them all.
	 *
	 * We can only use gp->size here if we're not dealing with a
	 * singleton gang. It is safer to always use the max number of segments we are
	 * controlling (largestGangsize).
	 */
	Assert(gp_connections_per_thread >= 0);

	segdbs_in_thread_pool = 0;
	
	Assert(db_descriptors_size <= largestGangsize());

	if (gp_connections_per_thread == 0)
		max_threads = 1;	/* one, not zero, because we need to allocate one param block */
	else
		max_threads = 1 + (largestGangsize() - 1) / gp_connections_per_thread;
	
	if (DispatchContext == NULL)
	{
		DispatchContext = AllocSetContextCreate(TopMemoryContext,
												"Dispatch Context",
												ALLOCSET_DEFAULT_MINSIZE,
												ALLOCSET_DEFAULT_INITSIZE,
												ALLOCSET_DEFAULT_MAXSIZE);
	}
	Assert(DispatchContext != NULL);
	
	oldContext = MemoryContextSwitchTo(DispatchContext);

	if (ds->dispatchThreads == NULL)
	{
		/* the maximum number of command parameter blocks we'll possibly need is
		 * one for each slice on the primary gang. Max sure that we
		 * have enough -- once we've created the command block we're stuck with it
		 * for the duration of this statement (including CDB-DTM ). 
		 * 1 * maxthreads * slices for each primary
		 * X 2 for good measure ? */
		int paramCount = max_threads * 4 * Max(maxSlices, 5);

		elog(DEBUG4, "dispatcher: allocating command array with maxslices %d paramCount %d", maxSlices, paramCount);
			
		ds->dispatchThreads = cdbdisp_makeDispatchThreads(paramCount);
	}
	else
	{
		/*
		 * If we attempt to reallocate, there is a race here: we
		 * know that we have threads running using the
		 * dispatchCommandParamsAr! If we reallocate we
		 * potentially yank it out from under them! Don't do
		 * it!
		 */
		if (ds->dispatchThreads->dispatchCommandParmsArSize < (ds->dispatchThreads->threadCount + max_threads))
		{
			elog(ERROR, "Attempted to reallocate dispatchCommandParmsAr while other threads still running size %d new threadcount %d",
				 ds->dispatchThreads->dispatchCommandParmsArSize, ds->dispatchThreads->threadCount + max_threads);
		}
	}

	MemoryContextSwitchTo(oldContext);

	x = 0;
	/*
	 * Create the thread parms structures based targetSet parameter.
	 * This will add the segdbDesc pointers appropriate to the
	 * targetSet into the thread Parms structures, making sure that each thread
	 * handles gp_connections_per_thread segdbs.
	 */
	for (i = 0; i < db_descriptors_size; i++)
	{
		CdbDispatchResult *qeResult;

		segdbDesc = &db_descriptors[i];

		Assert(segdbDesc != NULL);

#ifdef FAULT_INJECTOR
		int faultType = FaultInjector_InjectFaultIfSet(
							DispatchToGangThreadStructureInitialization,
							DDLNotSpecified,
							"",	//databaseName
							""); // tableName

		if(faultType == FaultInjectorTypeUserCancel)
		{
			QueryCancelPending = true;
			InterruptPending = true;
		}
		if(faultType == FaultInjectorTypeProcDie)
		{
			ProcDiePending = true;
			InterruptPending = true;
		}
#endif

		if (disp_direct->directed_dispatch)
		{
			Assert (disp_direct->count == 1); /* currently we allow direct-to-one dispatch, only */

			if (disp_direct->content[0] != segdbDesc->segment_database_info->segindex)
				continue;
		}

/*		if (mppDispatchCommandType == GP_DISPATCH_COMMAND_TYPE_QUERY)
		{
			MarkSendingToSegment(
				 segdbDesc->segment_database_info->segindex,
				 segdbDesc->segment_database_info->isprimary);
		}*/

		/* Initialize the QE's CdbDispatchResult object. */
		qeResult = cdbdisp_makeResult(dispatchResults, segdbDesc, sliceIndex);

		if (qeResult == NULL)
		{
			dispatchResults->writer_gang->dispatcherActive = true;
			insist_log(false, "allocation failed, cannot dispatch");
		}

		/* Transfer any connection errors from segdbDesc. */
		if (segdbDesc->errcode ||
			segdbDesc->error_message.len)
			cdbdisp_mergeConnectionErrors(qeResult, segdbDesc);

		addSegDBToDispatchThreadPool(ds->dispatchThreads->dispatchCommandParmsAr + ds->dispatchThreads->threadCount,
									 x,
					   				 mppDispatchCommandType,
					   				 commandTypeParms,
									 sliceIndex,
                                     qeResult);

		/*
		 * This CdbDispatchResult/SegmentDatabaseDescriptor pair will be
		 * dispatched and monitored by a thread to be started below. Only that
		 * thread should touch them until the thread is finished with them and
		 * resets the stillRunning flag. Caller must CdbCheckDispatchResult()
		 * to wait for completion.
		 */
		qeResult->stillRunning = true;

		x++;
	}
	segdbs_in_thread_pool = x;

	/*
	 * Compute the thread count based on how many segdbs were added into the
	 * thread pool, knowing that each thread handles gp_connections_per_thread
	 * segdbs.
	 */
	if (segdbs_in_thread_pool == 0)
		newThreads += 0;
	else
		if (gp_connections_per_thread == 0)
			newThreads += 1;
		else
			newThreads += 1 + (segdbs_in_thread_pool - 1) / gp_connections_per_thread;

	/*
	 * Create the threads. (which also starts the dispatching).
	 */
	for (i = 0; i < newThreads; i++)
	{
		DispatchCommandParms *pParms = &(ds->dispatchThreads->dispatchCommandParmsAr + ds->dispatchThreads->threadCount)[i];
		
		Assert(pParms != NULL);

	    if (gp_connections_per_thread==0)
	    {
	    	Assert(newThreads <= 1);
	    	thread_DispatchOut(pParms);
	    }
	    else
	    {
	    	int		pthread_err = 0;

			pParms->thread_valid = true;
			pthread_err = gp_pthread_create(&pParms->thread, thread_DispatchCommand, pParms, "dispatchToGang");


			/*
			 * If defined FAULT_INJECTOR, and injected fault is create thread failure, set pthread_err to -1,
			 * to trigger thread join process
			 */
#ifdef FAULT_INJECTOR
			int faultType = FaultInjector_InjectFaultIfSet(
					DispatchThreadCreation,
					DDLNotSpecified,
					"",	//databaseName
					""); // tableName

			if(faultType == FaultInjectorTypeCreateThreadFail)
			{
				pthread_err = -1;
			}
			if(faultType == FaultInjectorTypeUserCancel)
			{
				QueryCancelPending = true;
				InterruptPending = true;
			}
			if(faultType == FaultInjectorTypeProcDie)
			{
				ProcDiePending = true;
				InterruptPending = true;
			}
#endif

			if (pthread_err != 0)
			{
				int j;
	
				pParms->thread_valid = false;
	
				/*
				 * Error during thread create (this should be caused by
				 * resource constraints). If we leave the threads running,
				 * they'll immediately have some problems -- so we need to
				 * join them, and *then* we can issue our FATAL error
				 */
				pParms->pleaseCancel = true; /* mark the failed thread*/
	
				for (j = 0; j < ds->dispatchThreads->threadCount + (i - 1); j++)
				{
					DispatchCommandParms *pParms;
	
					pParms = &ds->dispatchThreads->dispatchCommandParmsAr[j];

					pParms->pleaseCancel = true; /* mark the rest of threads */
					pParms->thread_valid = false;
					pthread_join(pParms->thread, NULL);
				}
	
				ereport(FATAL, (errcode(ERRCODE_INTERNAL_ERROR),
								errmsg("could not create thread %d of %d", i + 1, newThreads),
								errdetail("pthread_create() failed with err %d", pthread_err)));
			}
	    }

	}

	ds->dispatchThreads->threadCount += newThreads;
	elog(DEBUG4, "dispatchToGang: Total threads now %d", ds->dispatchThreads->threadCount);
}	/* cdbdisp_dispatchToGang */


/*
 * addSegDBToDispatchThreadPool
 * Helper function used to add a segdb's segdbDesc to the thread pool to have commands dispatched to.
 * It figures out which thread will handle it, based on the setting of
 * gp_connections_per_thread.
 */
static void
addSegDBToDispatchThreadPool(DispatchCommandParms  *ParmsAr,
                             int                    segdbs_in_thread_pool,
						     GpDispatchCommandType	mppDispatchCommandType,
						     void				   *commandTypeParms,
                             int					sliceId,
                             CdbDispatchResult     *dispatchResult)
{
	DispatchCommandParms *pParms;
	int			ParmsIndex;
	bool 		firsttime = false;

	/*
	 * The proper index into the DispatchCommandParms array is computed, based on
	 * having gp_connections_per_thread segdbDesc's in each thread.
	 * If it's the first access to an array location, determined
	 * by (*segdbCount) % gp_connections_per_thread == 0,
	 * then we initialize the struct members for that array location first.
	 */
	if (gp_connections_per_thread == 0)
		ParmsIndex = 0;
	else
		ParmsIndex = segdbs_in_thread_pool / gp_connections_per_thread;
	pParms = &ParmsAr[ParmsIndex];

	/* 
	 * First time through?
	 */

	if (gp_connections_per_thread==0)
		firsttime = segdbs_in_thread_pool == 0;
	else
		firsttime = segdbs_in_thread_pool % gp_connections_per_thread == 0;
	if (firsttime)
	{
		pParms->mppDispatchCommandType = mppDispatchCommandType;
		
		switch (mppDispatchCommandType)
		{
			case GP_DISPATCH_COMMAND_TYPE_QUERY:
			{
				DispatchCommandQueryParms *pQueryParms = (DispatchCommandQueryParms *) commandTypeParms;

				if (pQueryParms->strCommand == NULL || strlen(pQueryParms->strCommand) == 0)
				{	
					pParms->queryParms.strCommand = NULL;
					pParms->queryParms.strCommandlen = 0;
				}
				else
				{
					pParms->queryParms.strCommand = pQueryParms->strCommand;
					pParms->queryParms.strCommandlen = strlen(pQueryParms->strCommand) + 1;
				}
				
				if (pQueryParms->serializedQuerytree == NULL || pQueryParms->serializedQuerytreelen == 0)
				{	
					pParms->queryParms.serializedQuerytree = NULL;
					pParms->queryParms.serializedQuerytreelen = 0;
				}
				else
				{
					pParms->queryParms.serializedQuerytree = pQueryParms->serializedQuerytree;			
					pParms->queryParms.serializedQuerytreelen = pQueryParms->serializedQuerytreelen;
				}
				
				if (pQueryParms->serializedPlantree == NULL || pQueryParms->serializedPlantreelen == 0)
				{	
					pParms->queryParms.serializedPlantree = NULL;
					pParms->queryParms.serializedPlantreelen = 0;
				}
				else
				{
					pParms->queryParms.serializedPlantree = pQueryParms->serializedPlantree;
					pParms->queryParms.serializedPlantreelen = pQueryParms->serializedPlantreelen;
				}
				
				if (pQueryParms->serializedParams == NULL || pQueryParms->serializedParamslen == 0)
				{	
					pParms->queryParms.serializedParams = NULL;
					pParms->queryParms.serializedParamslen = 0;
				}
				else
				{
					pParms->queryParms.serializedParams = pQueryParms->serializedParams;
					pParms->queryParms.serializedParamslen = pQueryParms->serializedParamslen;
				}
				
				if (pQueryParms->serializedSliceInfo == NULL || pQueryParms->serializedSliceInfolen == 0)
				{	
					pParms->queryParms.serializedSliceInfo = NULL;
					pParms->queryParms.serializedSliceInfolen = 0;
				}
				else
				{
					pParms->queryParms.serializedSliceInfo = pQueryParms->serializedSliceInfo;
					pParms->queryParms.serializedSliceInfolen = pQueryParms->serializedSliceInfolen;
				}
				
				if (pQueryParms->serializedDtxContextInfo == NULL || pQueryParms->serializedDtxContextInfolen == 0)
				{
					pParms->queryParms.serializedDtxContextInfo = NULL;
					pParms->queryParms.serializedDtxContextInfolen = 0;
				}
				else
				{
					pParms->queryParms.serializedDtxContextInfo = pQueryParms->serializedDtxContextInfo;
					pParms->queryParms.serializedDtxContextInfolen = pQueryParms->serializedDtxContextInfolen;
				}

				pParms->queryParms.rootIdx = pQueryParms->rootIdx;

				if (pQueryParms->seqServerHost == NULL || pQueryParms->seqServerHostlen == 0)
				{
					pParms->queryParms.seqServerHost = NULL;
					pParms->queryParms.seqServerHostlen = 0;
					pParms->queryParms.seqServerPort = -1;
				}

				else
				{
					pParms->queryParms.seqServerHost = pQueryParms->seqServerHost;
					pParms->queryParms.seqServerHostlen = pQueryParms->seqServerHostlen;
					pParms->queryParms.seqServerPort = pQueryParms->seqServerPort;
				}
				
				pParms->queryParms.primary_gang_id = pQueryParms->primary_gang_id;
			}
			break;
		
			case GP_DISPATCH_COMMAND_TYPE_DTX_PROTOCOL:
			{
				DispatchCommandDtxProtocolParms *pDtxProtocolParms = (DispatchCommandDtxProtocolParms *) commandTypeParms;

				pParms->dtxProtocolParms.dtxProtocolCommand = pDtxProtocolParms->dtxProtocolCommand;
				pParms->dtxProtocolParms.flags = pDtxProtocolParms->flags;
				pParms->dtxProtocolParms.dtxProtocolCommandLoggingStr = pDtxProtocolParms->dtxProtocolCommandLoggingStr;
				if (strlen(pDtxProtocolParms->gid) >= TMGIDSIZE)
					elog(PANIC, "Distribute transaction identifier too long (%d)",
						 (int)strlen(pDtxProtocolParms->gid));
				memcpy(pParms->dtxProtocolParms.gid, pDtxProtocolParms->gid, TMGIDSIZE);
				pParms->dtxProtocolParms.gxid = pDtxProtocolParms->gxid;
				pParms->dtxProtocolParms.primary_gang_id = pDtxProtocolParms->primary_gang_id;
				pParms->dtxProtocolParms.argument = pDtxProtocolParms->argument;
				pParms->dtxProtocolParms.argumentLength = pDtxProtocolParms->argumentLength;
			}
			break;

			default:
				elog(FATAL, "Unrecognized MPP dispatch command type: %d",
					 (int) mppDispatchCommandType);
		}
		
		/*
		 * GPSQL: in segments, we run everything as the bootstrap user
		 * which is set in InitPostgres.  There are couple of security
		 * concerns if we support full SQL, but this is ok for the
		 * current status, and for those rich features (such like
		 * nested function calls) we'll need bigger mechanism like
		 * catalog server anyway.
		 */
		pParms->sessUserId = BOOTSTRAP_SUPERUSERID;
		pParms->outerUserId = BOOTSTRAP_SUPERUSERID;
		pParms->currUserId = BOOTSTRAP_SUPERUSERID;
		pParms->sessUserId_is_super = true;
		pParms->outerUserId_is_super = true;

		pParms->cmdID = gp_command_count;
		pParms->localSlice = sliceId;
		Assert(DispatchContext != NULL);
		pParms->dispatchResultPtrArray =
			(CdbDispatchResult **) palloc0((gp_connections_per_thread == 0 ? largestGangsize() : gp_connections_per_thread)*
										   sizeof(CdbDispatchResult *));
		MemSet(&pParms->thread, 0, sizeof(pthread_t));
		pParms->db_count = 0;
	}

	/*
	 * Just add to the end of the used portion of the dispatchResultPtrArray
	 * and bump the count of members
	 */
	pParms->dispatchResultPtrArray[pParms->db_count++] = dispatchResult;

}	/* addSegDBToDispatchThreadPool */


/*
 * CdbCheckDispatchResult:
 *
 * Waits for completion of threads launched by cdbdisp_dispatchToGang().
 *
 * If 'cancelUnfinishedWork' is true, QEs that were dispatched with
 * 'cancelOnError' true and are not yet idle will be canceled.
 */
void
CdbCheckDispatchResult(struct CdbDispatcherState *ds,
					   bool cancelUnfinishedWork)
{
	PG_TRY();
	{
		CdbCheckDispatchResultInt(ds, NULL, NULL, cancelUnfinishedWork);
	}
	PG_CATCH();
	{
		cdbdisp_clearGangActiveFlag(ds);
		PG_RE_THROW();
	}
	PG_END_TRY();

	cdbdisp_clearGangActiveFlag(ds);
	
	if (log_dispatch_stats)
		ShowUsage("DISPATCH STATISTICS");
	
	if (DEBUG1 >= log_min_messages)
	{
		char		msec_str[32];

		switch (check_log_duration(msec_str, false))
		{
			case 1:
			case 2:
				ereport(LOG,  (errmsg("duration to dispatch result received from all QEs: %s ms", msec_str)));
				break;
		}					 
	}
}

static void
CdbCheckDispatchResultInt(struct CdbDispatcherState *ds,
						  struct SegmentDatabaseDescriptor *** failedSegDB,
						  int *numOfFailed,
						  bool cancelUnfinishedWork)
{
	int			i;
	int			j;
	int			nFailed = 0;
	DispatchCommandParms *pParms;
	CdbDispatchResult *dispatchResult;
	SegmentDatabaseDescriptor *segdbDesc;

	Assert(ds != NULL);

	if (failedSegDB)
		*failedSegDB = NULL;
	if (numOfFailed)
		*numOfFailed = 0;

	/* No-op if no work was dispatched since the last time we were called.	*/
	if (!ds->dispatchThreads || ds->dispatchThreads->threadCount == 0)
	{
		elog(DEBUG5, "CheckDispatchResult: no threads active");
		return;
	}

	/*
	 * Wait for threads to finish.
	 */
	for (i = 0; i < ds->dispatchThreads->threadCount; i++)
	{							/* loop over threads */		
		pParms = &ds->dispatchThreads->dispatchCommandParmsAr[i];
		Assert(pParms != NULL);

		/* Does caller want to stop short? */
		if (cancelUnfinishedWork)
		{
			pParms->pleaseCancel = true;
		}
		
		if (gp_connections_per_thread==0)
		{
			thread_DispatchWait(pParms);
		}
		else
		{
			elog(DEBUG4, "CheckDispatchResult: Joining to thread %d of %d",
				 i + 1, ds->dispatchThreads->threadCount);
	
			if (pParms->thread_valid)
			{
				int			pthread_err = 0;
				pthread_err = pthread_join(pParms->thread, NULL);
				if (pthread_err != 0)
					elog(FATAL, "CheckDispatchResult: pthread_join failed on thread %d (%lu) of %d (returned %d attempting to join to %lu)",
						 i + 1, 
#ifndef _WIN32
						 (unsigned long) pParms->thread, 
#else
						 (unsigned long) pParms->thread.p,
#endif
						 ds->dispatchThreads->threadCount, pthread_err, (unsigned long)mythread());
			}
		}
		HOLD_INTERRUPTS();
		pParms->thread_valid = false;
		MemSet(&pParms->thread, 0, sizeof(pParms->thread));
		RESUME_INTERRUPTS();

		/*
		 * Examine the CdbDispatchResult objects containing the results
		 * from this thread's QEs.
		 */
		for (j = 0; j < pParms->db_count; j++)
		{						/* loop over QEs managed by one thread */
			dispatchResult = pParms->dispatchResultPtrArray[j];

			if (dispatchResult == NULL)
			{
				elog(LOG, "CheckDispatchResult: result object is NULL ? skipping.");
				continue;
			}

			if (dispatchResult->segdbDesc == NULL)
			{
				elog(LOG, "CheckDispatchResult: result object segment descriptor is NULL ? skipping.");
				continue;
			}

			segdbDesc = dispatchResult->segdbDesc;

			/* segdbDesc error message is unlikely here, but check anyway. */
			if (segdbDesc->errcode ||
				segdbDesc->error_message.len)
				cdbdisp_mergeConnectionErrors(dispatchResult, segdbDesc);

			/* Log the result */
			if (DEBUG2 >= log_min_messages)
				cdbdisp_debugDispatchResult(dispatchResult, DEBUG2, DEBUG3);

			/* Notify FTS to reconnect if connection lost or never connected. */
			if (failedSegDB &&
				PQstatus(segdbDesc->conn) == CONNECTION_BAD)
			{
				/* Allocate storage.  Caller should pfree() it. */
				if (!*failedSegDB)
					*failedSegDB = palloc(sizeof(**failedSegDB) *
										  (2 * getgpsegmentCount() + 1));

				/* Append to broken connection list. */
				(*failedSegDB)[nFailed++] = segdbDesc;
				(*failedSegDB)[nFailed] = NULL;

				if (numOfFailed)
					*numOfFailed = nFailed;
			}

			/*
			 * Zap our SegmentDatabaseDescriptor ptr because it may be
			 * invalidated by the call to FtsHandleNetFailure() below.
			 * Anything we need from there, we should get before this.
			 */
			dispatchResult->segdbDesc = NULL;

		}						/* loop over QEs managed by one thread */
	}							/* loop over threads */

	/* reset thread state (will be destroyed later on in finishCommand) */
    ds->dispatchThreads->threadCount = 0;
			
	/* It looks like everything went fine, make sure we don't miss a
	 * user cancellation ?
	 *
	 * The cancelUnfinishedWork argument is true when we're handling
	 * an error, false when we are doing "normal work" */
	if (!cancelUnfinishedWork)
		CHECK_FOR_INTERRUPTS();
}	/* cdbdisp_checkDispatchResult */


/*--------------------------------------------------------------------*/

/*
 * I refactored this code out of the two routines
 *    cdbdisp_dispatchRMCommand  and  cdbdisp_dispatchDtxProtocolCommand
 * when I thought I might need it in a third place.
 * 
 * Not sure if this makes things cleaner or not
 */
struct pg_result **
cdbdisp_returnResults(CdbDispatchResults *primaryResults,
					  StringInfo errmsgbuf,
					  int *numresults)
{
	CdbDispatchResults *gangResults;
	CdbDispatchResult *dispatchResult;
	PGresult  **resultSets = NULL;
	int			nslots;
	int			nresults = 0;
	int			i;
	int			totalResultCount=0;

	/*
	 * Allocate result set ptr array. Make room for one PGresult ptr per
	 * primary segment db, plus a null terminator slot after the
	 * last entry. The caller must PQclear() each PGresult and free() the
	 * array.
	 */
	nslots = 2 * largestGangsize() + 1;
	resultSets = (struct pg_result **)calloc(nslots, sizeof(*resultSets));

	if (!resultSets)
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
						errmsg("cdbdisp_returnResults failed: out of memory")));

	/* Collect results from primary gang. */
	gangResults = primaryResults;
	if (gangResults)
	{
		totalResultCount = gangResults->resultCount;

		for (i = 0; i < gangResults->resultCount; ++i)
		{
			dispatchResult = &gangResults->resultArray[i];

			/* Append error messages to caller's buffer. */
			cdbdisp_dumpDispatchResult(dispatchResult, false, errmsgbuf);

			/* Take ownership of this QE's PGresult object(s). */
			nresults += cdbdisp_snatchPGresults(dispatchResult,
												resultSets + nresults,
												nslots - nresults - 1);
		}
		cdbdisp_destroyDispatchResults(gangResults);
	}

	/* Put a stopper at the end of the array. */
	Assert(nresults < nslots);
	resultSets[nresults] = NULL;

	/* If our caller is interested, tell them how many sets we're returning. */
	if (numresults != NULL)
		*numresults = totalResultCount;

	return resultSets;
}
/*
 * cdbdisp_dispatchRMCommand:
 * Sends a non-cancelable command to all segment dbs.
 *
 * Returns a malloc'ed array containing the PGresult objects thus
 * produced; the caller must PQclear() them and free() the array.
 * A NULL entry follows the last used entry in the array.
 *
 * Any error messages - whether or not they are associated with
 * PGresult objects - are appended to a StringInfo buffer provided
 * by the caller.
 */
struct pg_result **				/* returns ptr to array of PGresult ptrs */
cdbdisp_dispatchRMCommand(const char *strCommand,
						  bool withSnapshot,
						  StringInfo errmsgbuf,
						  int *numresults)
{
	volatile struct CdbDispatcherState ds = {NULL, NULL};
	
	PGresult  **resultSets = NULL;

	/* never want to start a global transaction for these */
	bool		needTwoPhase = false;

	elog(((Debug_print_full_dtm || Debug_print_snapshot_dtm) ? LOG : DEBUG5),
		 "cdbdisp_dispatchRMCommand for command = '%s', withSnapshot = %s",
	     strCommand, (withSnapshot ? "true" : "false"));

	PG_TRY();
	{
		/* Launch the command.	Don't cancel on error. */
		cdbdisp_dispatchCommand(strCommand, NULL, 0,
								/* cancelOnError */false,
								needTwoPhase, withSnapshot,
								(struct CdbDispatcherState *)&ds);

		/* Wait for all QEs to finish.	Don't cancel. */
		CdbCheckDispatchResult((struct CdbDispatcherState *)&ds, false);
	}
	PG_CATCH();
	{
		/* Something happend, clean up after ourselves */
		CdbCheckDispatchResult((struct CdbDispatcherState *)&ds, false);

		cdbdisp_destroyDispatchResults(ds.primaryResults);
		cdbdisp_destroyDispatchThreads(ds.dispatchThreads);

		PG_RE_THROW();
		/* not reached */
	}
	PG_END_TRY();

	resultSets = cdbdisp_returnResults(ds.primaryResults, errmsgbuf, numresults);
	
	/* free memory allocated for the dispatch threads struct */
	cdbdisp_destroyDispatchThreads(ds.dispatchThreads);

	return resultSets;
}	/* cdbdisp_dispatchRMCommand */

/*
 * cdbdisp_dispatchDtxProtocolCommand:
 * Sends a non-cancelable command to all segment dbs
 *
 * Returns a malloc'ed array containing the PGresult objects thus
 * produced; the caller must PQclear() them and free() the array.
 * A NULL entry follows the last used entry in the array.
 *
 * Any error messages - whether or not they are associated with
 * PGresult objects - are appended to a StringInfo buffer provided
 * by the caller.
 */
struct pg_result **				/* returns ptr to array of PGresult ptrs */
cdbdisp_dispatchDtxProtocolCommand(DtxProtocolCommand dtxProtocolCommand,
								   int	flags,
								   char	*dtxProtocolCommandLoggingStr,
								   char	*gid,
								   DistributedTransactionId	gxid,
								   StringInfo errmsgbuf,
								   int *numresults,
								   bool *badGangs,
								   CdbDispatchDirectDesc *direct,
								   char *argument, int argumentLength)
{
	CdbDispatcherState ds = {NULL, NULL};

	PGresult  **resultSets = NULL;
	
	DispatchCommandDtxProtocolParms dtxProtocolParms;
	Gang	*primaryGang;
	int		nsegdb = getgpsegmentCount();
	
	elog((Debug_print_full_dtm ? LOG : DEBUG5), "cdbdisp_dispatchDtxProtocolCommand: %s for gid = %s, direct content #: %d",
		 dtxProtocolCommandLoggingStr, gid, direct->directed_dispatch ? direct->content[0] : -1);

	*badGangs = false;

	MemSet(&dtxProtocolParms, 0, sizeof(dtxProtocolParms));
	dtxProtocolParms.dtxProtocolCommand = dtxProtocolCommand;
	dtxProtocolParms.flags = flags;
	dtxProtocolParms.dtxProtocolCommandLoggingStr = dtxProtocolCommandLoggingStr;
	if (strlen(gid) >= TMGIDSIZE)
		elog(PANIC, "Distribute transaction identifier too long (%d)",
			 (int)strlen(gid));
	memcpy(dtxProtocolParms.gid, gid, TMGIDSIZE);
	dtxProtocolParms.gxid = gxid;
	dtxProtocolParms.argument = argument;
	dtxProtocolParms.argumentLength = argumentLength;
	
	/*
	 * Allocate a primary QE for every available segDB in the system.
	 */
	primaryGang = allocateWriterGang();

	Assert(primaryGang);

	if (primaryGang->dispatcherActive)
	{
		elog(LOG, "cdbdisp_dispatchDtxProtocolCommand: primary gang marked active re-marking");
		primaryGang->dispatcherActive = false;
	}
	
	dtxProtocolParms.primary_gang_id = primaryGang->gang_id;

	/*
     * Dispatch the command.
     */
    ds.dispatchThreads = NULL;

	ds.primaryResults = cdbdisp_makeDispatchResults(nsegdb, 0, /* cancelOnError */ false);

	ds.primaryResults->writer_gang = primaryGang;

	cdbdisp_dispatchToGang(&ds, GP_DISPATCH_COMMAND_TYPE_DTX_PROTOCOL,
						   &dtxProtocolParms,
						   primaryGang, -1, 1, direct);

	/* Wait for all QEs to finish.	Don't cancel. */
	CdbCheckDispatchResult(&ds, false);

	if (!gangOK(primaryGang))
	{
		*badGangs = true;

		elog((Debug_print_full_dtm ? LOG : DEBUG5),
			 "cdbdisp_dispatchDtxProtocolCommand: Bad gang from dispatch of %s for gid = %s", 
			 dtxProtocolCommandLoggingStr, gid);
	}

	resultSets = cdbdisp_returnResults(ds.primaryResults, errmsgbuf, numresults);
	
	/* free memory allocated for the dispatch threads struct */
	cdbdisp_destroyDispatchThreads(ds.dispatchThreads);

	return resultSets;
}	/* cdbdisp_dispatchDtxProtocolCommand */


/*--------------------------------------------------------------------*/


/*
 * cdbdisp_dispatchCommand:
 * Send the strCommand SQL statement to all segdbs in the cluster
 * cancelOnError indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during gang creation.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable gp_connections_per_thread.
 *
 * The CdbDispatchResults objects allocated for the command
 * are returned in *pPrimaryResults
 * The caller, after calling CdbCheckDispatchResult(), can
 * examine the CdbDispatchResults objects, can keep them as
 * long as needed, and ultimately must free them with
 * cdbdisp_destroyDispatchResults() prior to deallocation
 * of the memory context from which they were allocated.
 *
 * NB: Callers should use PG_TRY()/PG_CATCH() if needed to make
 * certain that the CdbDispatchResults objects are destroyed by
 * cdbdisp_destroyDispatchResults() in case of error.
 * To wait for completion, check for errors, and clean up, it is
 * suggested that the caller use cdbdisp_finishCommand().
 */
void
cdbdisp_dispatchCommand(const char                 *strCommand,
						char			   		   *serializedQuerytree,
						int							serializedQuerytreelen,
                        bool                        cancelOnError,
                        bool						needTwoPhase,
                        bool						withSnapshot,
						CdbDispatcherState		*ds)
{
	DispatchCommandQueryParms queryParms;
	Gang	*primaryGang;
	int		nsegdb = getgpsegmentCount();
	CdbComponentDatabaseInfo *qdinfo;
	
	if (log_dispatch_stats)
		ResetUsage();
	
	if (DEBUG5 >= log_min_messages)
    	elog(DEBUG3, "cdbdisp_dispatchCommand: %s (needTwoPhase = %s)", 
	    	 strCommand, (needTwoPhase ? "true" : "false"));
    else
    	elog((Debug_print_full_dtm ? LOG : DEBUG3), "cdbdisp_dispatchCommand: %.50s (needTwoPhase = %s)", 
    	     strCommand, (needTwoPhase ? "true" : "false"));

    ds->primaryResults = NULL;
	ds->dispatchThreads = NULL;

	MemSet(&queryParms, 0, sizeof(queryParms));
	queryParms.strCommand = strCommand;
	queryParms.serializedQuerytree = serializedQuerytree;
	queryParms.serializedQuerytreelen = serializedQuerytreelen;

	/*
	 * Allocate a primary QE for every available segDB in the system.
	 */
	primaryGang = allocateWriterGang();

	Assert(primaryGang);

	queryParms.primary_gang_id = primaryGang->gang_id;
	
	/* Serialize a version of our DTX Context Info */
	/*queryParms.serializedDtxContextInfo =
		qdSerializeDtxContextInfo(&queryParms.serializedDtxContextInfolen, withSnapshot, false, generateTxnOptions(needTwoPhase), "cdbdisp_dispatchCommand");
	 */

	/*
	 * in hawq, there is no distributed transaction
	 */
	queryParms.serializedDtxContextInfo = NULL;
	queryParms.serializedDtxContextInfolen = 0;

	/* sequence server info */
	qdinfo = &(getComponentDatabases()->entry_db_info[0]);
	Assert(qdinfo != NULL && qdinfo->hostip != NULL);
	queryParms.seqServerHost = pstrdup(qdinfo->hostip);
	queryParms.seqServerHostlen = strlen(qdinfo->hostip) + 1;
	queryParms.seqServerPort = seqServerCtl->seqServerPort;

	/*
	 * Dispatch the command.
	 */
	ds->primaryResults = cdbdisp_makeDispatchResults(nsegdb, 0, cancelOnError);
	ds->primaryResults->writer_gang = primaryGang;

	cdbdisp_dispatchToGang(ds,
						   GP_DISPATCH_COMMAND_TYPE_QUERY,
						   &queryParms,
						   primaryGang, -1, 1, DEFAULT_DISP_DIRECT);

	/*
	 * don't pfree serializedShapshot here, it will be pfree'd when
	 * the first thread is destroyed.
	 */

}	/* cdbdisp_dispatchCommand */


/* Wait for all QEs to finish, then report any errors from the given
 * CdbDispatchResults objects and free them.  If not all QEs in the
 * associated gang(s) executed the command successfully, throws an
 * error and does not return.  No-op if both CdbDispatchResults ptrs are NULL.
 * This is a convenience function; callers with unusual requirements may
 * instead call CdbCheckDispatchResult(), etc., directly.
 */
void
cdbdisp_finishCommand(struct CdbDispatcherState *ds,
					  void (*handle_results_callback)(CdbDispatchResults *primaryResults, void *ctx),
					  void *ctx)
{
	StringInfoData buf;
	int			errorcode = 0;

	/* If cdbdisp_dispatchToGang() wasn't called, don't wait. */
	if (!ds || !ds->primaryResults)
		return;

	/* Wait for all QEs to finish. Don't cancel them. */
	CdbCheckDispatchResult(ds, false);

	/* If no errors, free the CdbDispatchResults objects and return. */
	if (ds->primaryResults)
		errorcode = ds->primaryResults->errcode;

	if (!errorcode)
	{
		/* Call the callback function to handle the results */
		if (handle_results_callback != NULL)
			handle_results_callback(ds->primaryResults, ctx);

		cdbdisp_destroyDispatchResults(ds->primaryResults);
		ds->primaryResults = NULL;
		cdbdisp_destroyDispatchThreads(ds->dispatchThreads);
		ds->dispatchThreads = NULL;
		return;
	}

	/* Format error messages from the primary gang. */
	initStringInfo(&buf);
	cdbdisp_dumpDispatchResults(ds->primaryResults, &buf, false);

	cdbdisp_destroyDispatchResults(ds->primaryResults);
	ds->primaryResults = NULL;

	cdbdisp_destroyDispatchThreads(ds->dispatchThreads);
	ds->dispatchThreads = NULL;

	/* Too bad, our gang got an error. */
	PG_TRY();
	{
		ereport(ERROR, (errcode(errorcode),
                        errOmitLocation(true),
						errmsg("%s", buf.data)));
	}
	PG_CATCH();
	{
		pfree(buf.data);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* not reached */
}	/* cdbdisp_finishCommand */

/*
 * cdbdisp_handleError
 *
 * When caller catches an error, the PG_CATCH handler can use this
 * function instead of cdbdisp_finishCommand to wait for all QEs
 * to finish, clean up, and report QE errors if appropriate.
 * This function should be called only from PG_CATCH handlers.
 *
 * This function destroys and frees the given CdbDispatchResults objects.
 * It is a no-op if both CdbDispatchResults ptrs are NULL.
 *
 * On return, the caller is expected to finish its own cleanup and
 * exit via PG_RE_THROW().
 */
void
cdbdisp_handleError(struct CdbDispatcherState *ds)
{
	int     qderrcode;
	bool    useQeError = false;

	qderrcode = elog_geterrcode();

	/* If cdbdisp_dispatchToGang() wasn't called, don't wait. */
	if (!ds || !ds->primaryResults)
		return;

	/*
	 * Request any remaining commands executing on qExecs to stop.
	 * We need to wait for the threads to finish.  This allows for proper
	 * cleanup of the results from the async command executions.
	 */
	CdbCheckDispatchResult(ds, true);   /* cancel any QEs still running */

    /*
     * When a QE stops executing a command due to an error, as a
     * consequence there can be a cascade of interconnect errors
     * (usually "sender closed connection prematurely") thrown in
     * downstream processes (QEs and QD).  So if we are handling
     * an interconnect error, and a QE hit a more interesting error,
     * we'll let the QE's error report take precedence.
     */
	if (qderrcode == ERRCODE_GP_INTERCONNECTION_ERROR)
	{
		bool qd_lost_flag = false;
		char *qderrtext = elog_message();

		if (qderrtext && strcmp(qderrtext, CDB_MOTION_LOST_CONTACT_STRING) == 0)
			qd_lost_flag = true;

		if (ds->primaryResults && ds->primaryResults->errcode)
		{
			if (qd_lost_flag && ds->primaryResults->errcode == ERRCODE_GP_INTERCONNECTION_ERROR)
				useQeError = true;
			else if (ds->primaryResults->errcode != ERRCODE_GP_INTERCONNECTION_ERROR)
				useQeError = true;
		}
	}

    if (useQeError)
    {
        /*
         * Throw the QE's error, catch it, and fall thru to return
         * normally so caller can finish cleaning up.  Afterwards
         * caller must exit via PG_RE_THROW().
         */
        PG_TRY();
        {
            cdbdisp_finishCommand(ds, NULL, NULL);
        }
        PG_CATCH();
        {}                      /* nop; fall thru */
        PG_END_TRY();
    }
    else
    {
        /*
         * Discard any remaining results from QEs; don't confuse matters by
         * throwing a new error.  Any results of interest presumably should
         * have been examined before raising the error that the caller is
         * currently handling.
         */
        cdbdisp_destroyDispatchResults(ds->primaryResults);
		ds->primaryResults = NULL;
		cdbdisp_destroyDispatchThreads(ds->dispatchThreads);
		ds->dispatchThreads = NULL;
    }
}                               /* cdbdisp_handleError */

bool
cdbdisp_check_estate_for_cancel(struct EState *estate)
{
	struct CdbDispatchResults  *meleeResults;

	Assert(estate);
	Assert(estate->dispatcherState);

	meleeResults = estate->dispatcherState->primaryResults;

	if (meleeResults == NULL) /* cleanup ? */
	{
		return false;
	}

	Assert(meleeResults);

//	if (pleaseCancel || meleeResults->errcode)
	if (meleeResults->errcode)
	{
		return true;
	}

	return false;
}

static void
cdbdisp_dispatchCommandToAllGangs(const char	*strCommand,
								  char			*serializedQuerytree,
								  int			serializedQuerytreelen,
								  char			*serializedPlantree,
								  int			serializedPlantreelen,
								  bool			cancelOnError,
								  bool			needTwoPhase,
								  struct CdbDispatcherState *ds)
{
	DispatchCommandQueryParms queryParms;
	
	Gang		*primaryGang;
    List		*readerGangs;
    ListCell	*le;

    int			nsegdb = getgpsegmentCount();
	int			gangCount;
	
    ds->primaryResults = NULL;	
	ds->dispatchThreads = NULL;

	MemSet(&queryParms, 0, sizeof(queryParms));
	queryParms.strCommand = strCommand;
	queryParms.serializedQuerytree = serializedQuerytree;
	queryParms.serializedQuerytreelen = serializedQuerytreelen;
	queryParms.serializedPlantree = serializedPlantree;
	queryParms.serializedPlantreelen = serializedPlantreelen;
	
	/*
	 * Allocate a primary QE for every available segDB in the system.
	 */
	primaryGang = allocateWriterGang();

	Assert(primaryGang);
	
	queryParms.primary_gang_id = primaryGang->gang_id;

	/* serialized a version of our snapshot */
	/*queryParms.serializedDtxContextInfo =
		qdSerializeDtxContextInfo(&queryParms.serializedDtxContextInfolen, true  withSnapshot , false  cursor,
								  generateTxnOptions(needTwoPhase), "cdbdisp_dispatchCommandToAllGangs");*/
	/*
	 * in hawq, there is no distributed transaction
	 */
	queryParms.serializedDtxContextInfo = NULL;
	queryParms.serializedDtxContextInfolen = 0;

	readerGangs = getAllReaderGangs();
	
	/*
	 * Dispatch the command.
	 */
	gangCount = 1 + list_length(readerGangs);
	ds->primaryResults = cdbdisp_makeDispatchResults(nsegdb * gangCount, 0, cancelOnError);

	ds->primaryResults->writer_gang = primaryGang;
	cdbdisp_dispatchToGang(ds,
						   GP_DISPATCH_COMMAND_TYPE_QUERY,
						   &queryParms,
						   primaryGang, -1, gangCount, DEFAULT_DISP_DIRECT);

	foreach(le, readerGangs)
	{
		Gang  *rg = lfirst(le);
		cdbdisp_dispatchToGang(ds,
							   GP_DISPATCH_COMMAND_TYPE_QUERY,
							   &queryParms,
							   rg, -1, gangCount, DEFAULT_DISP_DIRECT);
	}
}	/* cdbdisp_dispatchCommandToAllGangs */

/*--------------------------------------------------------------------*/


static bool  
thread_DispatchOut(DispatchCommandParms *pParms)
{
	CdbDispatchResult			*dispatchResult;
	int							i, db_count = pParms->db_count;

	switch (pParms->mppDispatchCommandType)
	{
		case GP_DISPATCH_COMMAND_TYPE_QUERY:
		{
			DispatchCommandQueryParms *pQueryParms = &pParms->queryParms;
			
			pParms->query_text = PQbuildGpQueryString(
				pQueryParms->strCommand, pQueryParms->strCommandlen, 
				pQueryParms->serializedQuerytree, pQueryParms->serializedQuerytreelen, 
				pQueryParms->serializedPlantree, pQueryParms->serializedPlantreelen, 
				pQueryParms->serializedParams, pQueryParms->serializedParamslen, 
				pQueryParms->serializedSliceInfo, pQueryParms->serializedSliceInfolen, 
				pQueryParms->serializedDtxContextInfo, pQueryParms->serializedDtxContextInfolen, 
				0 /* unused flags*/, pParms->cmdID, pParms->localSlice, pQueryParms->rootIdx,
				pQueryParms->seqServerHost, pQueryParms->seqServerHostlen, pQueryParms->seqServerPort,
				pQueryParms->primary_gang_id,
				GetCurrentStatementStartTimestamp(),
				pParms->sessUserId, pParms->sessUserId_is_super,
				pParms->outerUserId, pParms->outerUserId_is_super, pParms->currUserId,
				&pParms->query_text_len);
		}
		break;

		case GP_DISPATCH_COMMAND_TYPE_DTX_PROTOCOL:
		{
			pParms->query_text = PQbuildGpDtxProtocolCommand(
				(int)pParms->dtxProtocolParms.dtxProtocolCommand,
				pParms->dtxProtocolParms.flags,
				pParms->dtxProtocolParms.dtxProtocolCommandLoggingStr,
				pParms->dtxProtocolParms.gid,
				pParms->dtxProtocolParms.gxid,
				pParms->dtxProtocolParms.primary_gang_id,
				pParms->dtxProtocolParms.argument,
				pParms->dtxProtocolParms.argumentLength,
				&pParms->query_text_len);
		}
		break;

		default:
			write_log("bad dispatch command type %d", (int) pParms->mppDispatchCommandType);
			pParms->query_text_len = 0;
			return false;
	}

	if (pParms->query_text == NULL)
	{
		write_log("could not build query string, total length %d", pParms->query_text_len);
		pParms->query_text_len = 0;
		return false;
	}

	/*
	 * The pParms contains an array of SegmentDatabaseDescriptors
	 * to send commands through to.
	 */
	for (i = 0; i < db_count; i++)
	{
		dispatchResult = pParms->dispatchResultPtrArray[i];
		
		/* Don't use elog, it's not thread-safe */
		if (DEBUG5 >= log_min_messages)
		{
			if (dispatchResult->segdbDesc->conn)
			{
				write_log("thread_DispatchCommand working on %d of %d commands.  asyncStatus %d",
						  i + 1, db_count, dispatchResult->segdbDesc->conn->asyncStatus);
			}
		}

        dispatchResult->hasDispatched = false;
        dispatchResult->requestedCancel = false;
        dispatchResult->wasCanceled = false;

        if (!shouldStillDispatchCommand(pParms, dispatchResult))
        {
            /* Don't dispatch if cancellation pending or no connection. */
            dispatchResult->stillRunning = false;
            if (PQisBusy(dispatchResult->segdbDesc->conn))
				write_log(" We thought we were done, because !shouldStillDispatchCommand(), but libpq says we are still busy");
			if (PQstatus(dispatchResult->segdbDesc->conn) == CONNECTION_BAD)
				write_log(" We thought we were done, because !shouldStillDispatchCommand(), but libpq says the connection died?");
        }
        else
        {
            /* Kick off the command over the libpq connection.
             * If unsuccessful, proceed anyway, and check for lost connection below.
             */
			if (PQisBusy(dispatchResult->segdbDesc->conn))
			{
				PGresult   *pRes;
				ExecStatusType resultStatus;
				
				write_log("Trying to send to busy connection %s  %d %d asyncStatus %d",
					dispatchResult->segdbDesc->whoami, i, db_count, dispatchResult->segdbDesc->conn->asyncStatus);

				/*
				 * We weren't expecting the session to be in use!, let's see what it says 
				 */
				pRes = PQgetResult(dispatchResult->segdbDesc->conn);
				if (pRes != NULL) /* null is OK ? */
				{
					resultStatus = PQresultStatus(pRes);
					write_log("ResultStatus = %s\n", PQresStatus(resultStatus));
					write_log("%s", PQerrorMessage(dispatchResult->segdbDesc->conn));
				}
				else
				{
					write_log("result was null\n");
					write_log("%s", PQerrorMessage(dispatchResult->segdbDesc->conn));
					/* Assert() is not safe to call from a thread */
					if (PQisBusy(dispatchResult->segdbDesc->conn)) 
						write_log("Bug!... PQisBusy when we didn't expect it");
				}
			}

			if (PQstatus(dispatchResult->segdbDesc->conn) == CONNECTION_BAD)
			{
				char *msg;

				msg = PQerrorMessage(dispatchResult->segdbDesc->conn);

				write_log("Dispatcher noticed a problem before query transmit: %s (%s)", msg ? msg : "unknown error", dispatchResult->segdbDesc->whoami);

				/* Save error info for later. */
				cdbdisp_appendMessage(dispatchResult, LOG,
									  ERRCODE_GP_INTERCONNECTION_ERROR,
									  "Error before transmit from %s: %s",
									  dispatchResult->segdbDesc->whoami,
									  msg ? msg : "unknown error");

				PQfinish(dispatchResult->segdbDesc->conn);
				dispatchResult->segdbDesc->conn = NULL;
				dispatchResult->stillRunning = false;

				continue;
			}
#ifdef USE_NONBLOCKING
			/* 
			 * In 2000, Tom Lane said:
			 * "I believe that the nonblocking-mode code is pretty buggy, and don't
			 *  recommend using it unless you really need it and want to help debug
			 *  it.."
			 * 
			 * Reading through the code, I'm not convinced the situation has
			 * improved in 2007... I still see some very questionable things
			 * about nonblocking mode, so for now, I'm disabling it.
			 */
			PQsetnonblocking(dispatchResult->segdbDesc->conn, TRUE);
#endif 
			
			switch (pParms->mppDispatchCommandType)
			{
				case GP_DISPATCH_COMMAND_TYPE_QUERY:
					dispatchCommandQuery(dispatchResult, pParms->query_text, pParms->query_text_len, pParms->queryParms.strCommand);
					break;
				
				case GP_DISPATCH_COMMAND_TYPE_DTX_PROTOCOL:

					if (Debug_dtm_action == DEBUG_DTM_ACTION_DELAY &&
						Debug_dtm_action_target == DEBUG_DTM_ACTION_TARGET_PROTOCOL &&
						Debug_dtm_action_protocol == pParms->dtxProtocolParms.dtxProtocolCommand &&
						Debug_dtm_action_segment == dispatchResult->segdbDesc->segment_database_info->segindex)
					{
						write_log("Delaying '%s' broadcast for segment %d by %d milliseconds.",
								  DtxProtocolCommandToString(Debug_dtm_action_protocol),
								  Debug_dtm_action_segment,
								  Debug_dtm_action_delay_ms);
						pg_usleep(Debug_dtm_action_delay_ms * 1000);
					}
				
					dispatchCommandDtxProtocol(dispatchResult, pParms->query_text, pParms->query_text_len, 
											   pParms->dtxProtocolParms.dtxProtocolCommand);
					break;
				
				default:
					write_log("bad dispatch command type %d", (int) pParms->mppDispatchCommandType);
					pParms->query_text_len = 0;
					return false;
			}
			
            dispatchResult->hasDispatched = true;
        }
	}
        
#ifdef USE_NONBLOCKING     
        
    /*
     * Is everything sent?  Well, if the network stack was too busy, and we are using
     * nonblocking mode, some of the sends
     * might not have completed.  We can't use SELECT to wait unless they have
     * received their work, or we will wait forever.    Make sure they do.
     */

	{
		bool allsent=true;
		
		/*
		 * debug loop to check to see if this really is needed
		 */
		for (i = 0; i < db_count; i++)
    	{
    		dispatchResult = pParms->dispatchResultPtrArray[i];
    		if (!dispatchResult->stillRunning || !dispatchResult->hasDispatched)
    			continue;
    		if (PQstatus(dispatchResult->segdbDesc->conn) == CONNECTION_BAD)
    			continue;
    		if (dispatchResult->segdbDesc->conn->outCount > 0)
    		{
    			write_log("Yes, extra flushing is necessary %d",i);
    			break;
    		}
    	}
		
		/*
		 * Check to see if any needed extra flushing.
		 */
		for (i = 0; i < db_count; i++)
    	{
        	int			flushResult;

    		dispatchResult = pParms->dispatchResultPtrArray[i];
    		if (!dispatchResult->stillRunning || !dispatchResult->hasDispatched)
    			continue;
    		if (PQstatus(dispatchResult->segdbDesc->conn) == CONNECTION_BAD)
    			continue;
    		/*
			 * If data remains unsent, send it.  Else we might be waiting for the
			 * result of a command the backend hasn't even got yet.
			 */
    		flushResult = PQflush(dispatchResult->segdbDesc->conn);
    		/*
    		 * First time, go through the loop without waiting if we can't 
    		 * flush, in case we are using multiple network adapters, and 
    		 * other connections might be able to flush
    		 */
    		if (flushResult > 0)
    		{
    			allsent=false;
    			write_log("flushing didn't finish the work %d",i);
    		}
    		
    	}

        /*
         * our first attempt at doing more flushes didn't get everything out,
         * so we need to continue to try.
         */

		for (i = 0; i < db_count; i++)
    	{
    		dispatchResult = pParms->dispatchResultPtrArray[i];
    		while (PQisnonblocking(dispatchResult->segdbDesc->conn))
    		{
    			PQflush(dispatchResult->segdbDesc->conn);
    			PQsetnonblocking(dispatchResult->segdbDesc->conn, FALSE);
    		}
		}

	}
#endif 
	
	return true;

}

static void
thread_DispatchWait(DispatchCommandParms		*pParms)
{
	SegmentDatabaseDescriptor	*segdbDesc;
	CdbDispatchResult			*dispatchResult;
	int							i, db_count = pParms->db_count;
	int							timeoutCounter = 0;

	/*
	 * OK, we are finished submitting the command to the segdbs.
	 * Now, we have to wait for them to finish.
	 */
	for (;;)
	{							/* some QEs running */
		struct timeval tv;
		int			max_sock;
		int			sock;
		int			n;

		/*
		 * Which QEs are still running and could send results to us?
		 */
		max_sock = -1;
		MPP_FD_ZERO(&pParms->mask);

		for (i = 0; i < db_count; i++)
		{						/* loop to check connection status */
			dispatchResult = pParms->dispatchResultPtrArray[i];
			segdbDesc = dispatchResult->segdbDesc;

			/* Already finished with this QE? */
			if (!dispatchResult->stillRunning)
				continue;

			/* Add socket to fd_set if still connected. */
			sock = PQsocket(segdbDesc->conn);
			if (sock >= 0 &&
				PQstatus(segdbDesc->conn) != CONNECTION_BAD)
			{
				MPP_FD_SET(sock, &pParms->mask);
				max_sock = Max(max_sock, sock);
			}

			/* Lost the connection. */
			else
			{
				char	   *msg = PQerrorMessage(segdbDesc->conn);

				/* Save error info for later. */
				cdbdisp_appendMessage(dispatchResult, DEBUG1,
									  ERRCODE_GP_INTERCONNECTION_ERROR,
									  "Lost connection to %s.  %s",
									  segdbDesc->whoami,
									  msg ? msg : "");

				/* Free the PGconn object. */
				PQfinish(segdbDesc->conn);
				segdbDesc->conn = NULL;
				dispatchResult->stillRunning = false;	/* he's dead, Jim */
			}
		}						/* loop to check connection status */

		/* Break out when no QEs still running. */
		if (max_sock < 0)
			break;

		/*
		 * Wait for results from QEs.
		 */

		/* Timeout should be reinitialized before each select(). */
		tv.tv_sec = 2;
		tv.tv_usec = 0;

		/* Block here until input is available. */
		n = select(max_sock + 1, (fd_set *)&pParms->mask, NULL, NULL, &tv);

/*
		 Inject fault for select result
#ifdef FAULT_INJECTOR
		int faultType = FaultInjector_InjectFaultIfSet(
							DispatchWait,
							DDLNotSpecified,
							"",	//databaseName
							""); // tableName

		if(faultType == FaultInjectorTypeDispatchError)
		{
			write_log("fault injected on dispatch wait, fault identifier is DispatchWait, and fault "
					"type is FaultInjectorDispatchError");
			n = -1;
		}
		else if(faultType == FaultInjectorTypeTimeOut)
		{
			write_log("fault injected on dispatch wait, fault identifier is DispatchWait, and fault "
								"type is FaultInjectorTypeTimeOut");
			n = 0;
		}
 #else
		 Block here until input is available.
		n = select(max_sock + 1, (fd_set *)&pParms->mask, NULL, NULL, &tv);
#endif
*/

		if (n < 0)
		{
			int			sock_errno = SOCK_ERRNO;

			if (sock_errno == EINTR)
				continue;

			handleSelectError(pParms, db_count, sock_errno);
			continue;
		}

		if (n == 0)
		{
			handleSelectTimeout(pParms, db_count, &timeoutCounter, true);
			continue;
		}

		/*
		 * We have data waiting on one or more of the connections.
		 */
		for (i = 0; i < db_count; i++)
		{						/* input available; receive and process it */
			bool		finished;

			dispatchResult = pParms->dispatchResultPtrArray[i];
			segdbDesc = dispatchResult->segdbDesc;

			/* Skip if already finished or didn't dispatch. */
			if (!dispatchResult->stillRunning)
				continue;

			if (DEBUG4 >= log_min_messages)
				write_log("looking for results from %d of %d",i+1,db_count);

			/* Skip this connection if it has no input available. */
			sock = PQsocket(segdbDesc->conn);
			if (sock >= 0 &&
				!MPP_FD_ISSET(sock, &pParms->mask))
				continue;

			if (DEBUG4 >= log_min_messages)
				write_log("PQsocket says there are results from %d",i+1);
			/* Receive and process results from this QE. */
			finished = processResults(dispatchResult);

			/* Are we through with this QE now? */
			if (finished)
			{
				if (DEBUG4 >= log_min_messages)
					write_log("processResults says we are finished with %d:  %s",i+1,segdbDesc->whoami);
				dispatchResult->stillRunning = false;
				if (DEBUG1 >= log_min_messages)
				{
					char		msec_str[32];
					switch (check_log_duration(msec_str, false))
					{
						case 1:
						case 2:
							write_log("duration to dispatch result received from thread %d (seg %d): %s ms", i+1 ,dispatchResult->segdbDesc->segindex,msec_str);
							break;
					}					 
				}
				if (PQisBusy(dispatchResult->segdbDesc->conn))
					write_log("We thought we were done, because finished==true, but libpq says we are still busy");
				
			}
			else
				if (DEBUG4 >= log_min_messages)
					write_log("processResults says we have more to do with %d: %s",i+1,segdbDesc->whoami);
		}						/* input available; receive and process it */

	}							/* some QEs running */
	
}

static void
thread_DispatchWaitSingle(DispatchCommandParms		*pParms)
{
	SegmentDatabaseDescriptor	*segdbDesc;
	CdbDispatchResult			*dispatchResult;
	char * msg = NULL;
	
	/* Assert() cannot be used in threads */
	if (pParms->db_count != 1)
		write_log("Bug... thread_dispatchWaitSingle called with db_count %d",pParms->db_count);
	
	dispatchResult = pParms->dispatchResultPtrArray[0];
	segdbDesc = dispatchResult->segdbDesc;
	
	if (PQstatus(segdbDesc->conn) == CONNECTION_BAD)
		/* Lost the connection. */
	{
		msg = PQerrorMessage(segdbDesc->conn);

		/* Save error info for later. */
		cdbdisp_appendMessage(dispatchResult, DEBUG1,
							  ERRCODE_GP_INTERCONNECTION_ERROR,
							  "Lost connection to %s.  %s",
							  segdbDesc->whoami,
							  msg ? msg : "");

		/* Free the PGconn object. */
		PQfinish(segdbDesc->conn);
		segdbDesc->conn = NULL;
		dispatchResult->stillRunning = false;	/* he's dead, Jim */
	}
	else
	{

		PQsetnonblocking(segdbDesc->conn,FALSE);  /* Not necessary, I think */
 
		for(;;)
		{							/* loop to call PQgetResult; will block */
			PGresult   *pRes;
			ExecStatusType resultStatus;
			int			resultIndex = cdbdisp_numPGresult(dispatchResult);

			if (DEBUG4 >= log_min_messages)
				write_log("PQgetResult, resultIndex = %d",resultIndex);
			/* Get one message. */
			pRes = PQgetResult(segdbDesc->conn);

			CollectQEWriterTransactionInformation(segdbDesc, dispatchResult);

			/*
			 * Command is complete when PGgetResult() returns NULL. It is critical
			 * that for any connection that had an asynchronous command sent thru
			 * it, we call PQgetResult until it returns NULL. Otherwise, the next
			 * time a command is sent to that connection, it will return an error
			 * that there's a command pending.
			 */
			if (!pRes)
			{						/* end of results */
				if (DEBUG4 >= log_min_messages)
				{
					/* Don't use elog, it's not thread-safe */
					write_log("%s -> idle", segdbDesc->whoami);
				}
				break;		/* this is normal end of command */
			}						/* end of results */

			/* Attach the PGresult object to the CdbDispatchResult object. */
			cdbdisp_appendResult(dispatchResult, pRes);

			/* Did a command complete successfully? */
			resultStatus = PQresultStatus(pRes);
			if (resultStatus == PGRES_COMMAND_OK ||
				resultStatus == PGRES_TUPLES_OK ||
				resultStatus == PGRES_COPY_IN ||
				resultStatus == PGRES_COPY_OUT)
			{						/* QE reported success */

				/*
				 * Save the index of the last successful PGresult. Can be given to
				 * cdbdisp_getPGresult() to get tuple count, etc.
				 */
				dispatchResult->okindex = resultIndex;

				if (DEBUG3 >= log_min_messages)
				{
					/* Don't use elog, it's not thread-safe */
					char	   *cmdStatus = PQcmdStatus(pRes);

					write_log("%s -> ok %s",
							  segdbDesc->whoami,
							  cmdStatus ? cmdStatus : "(no cmdStatus)");
				}
				
				if (resultStatus == PGRES_COPY_IN ||
					resultStatus == PGRES_COPY_OUT)
					return;
			}						/* QE reported success */

			/* Note QE error.  Cancel the whole statement if requested. */
			else
			{						/* QE reported an error */
				char	   *sqlstate = PQresultErrorField(pRes, PG_DIAG_SQLSTATE);
				int			errcode = 0;

				msg = PQresultErrorMessage(pRes);

				if (DEBUG2 >= log_min_messages)
				{
					/* Don't use elog, it's not thread-safe */
					write_log("%s -> %s %s  %s",
							  segdbDesc->whoami,
							  PQresStatus(resultStatus),
							  sqlstate ? sqlstate : "(no SQLSTATE)",
							  msg ? msg : "");
				}

				/*
				 * Convert SQLSTATE to an error code (ERRCODE_xxx). Use a generic
				 * nonzero error code if no SQLSTATE.
				 */
				if (sqlstate &&
					strlen(sqlstate) == 5)
					errcode = cdbdisp_sqlstate_to_errcode(sqlstate);

				/*
				 * Save first error code and the index of its PGresult buffer
				 * entry.
				 */
				cdbdisp_seterrcode(errcode, resultIndex, dispatchResult);
			}						/* QE reported an error */
		}							/* loop to call PQgetResult; won't block */
		 
		
		if (DEBUG4 >= log_min_messages)
			write_log("processResultsSingle says we are finished with :  %s",segdbDesc->whoami);
		dispatchResult->stillRunning = false;
		if (DEBUG1 >= log_min_messages)
		{
			char		msec_str[32];
			switch (check_log_duration(msec_str, false))
			{
				case 1:
				case 2:
					write_log("duration to dispatch result received from thread (seg %d): %s ms", dispatchResult->segdbDesc->segindex,msec_str);
					break;
			}					 
		}
		if (PQisBusy(dispatchResult->segdbDesc->conn))
			write_log("We thought we were done, because finished==true, but libpq says we are still busy");
	}
}

/*
 * thread_DispatchCommand is the thread proc used to dispatch the command to one or more of the qExecs.
 *
 * NOTE: This function MUST NOT contain elog or ereport statements. (or most any other backend code)
 *		 elog is NOT thread-safe.  Developers should instead use something like:
 *
 *	if (DEBUG3 >= log_min_messages)
 *			write_log("my brilliant log statement here.");
 *
 * NOTE: In threads, we cannot use palloc, because it's not thread safe.
 */
void *
thread_DispatchCommand(void *arg)
{
	DispatchCommandParms		*pParms = (DispatchCommandParms *) arg;

	gp_set_thread_sigmasks();

	if (thread_DispatchOut(pParms))
	{
		/*
		 * thread_DispatchWaitSingle might have a problem with interupts
		 */
		if (pParms->db_count == 1 && false)
			thread_DispatchWaitSingle(pParms);
		else
			thread_DispatchWait(pParms);
	}

	return (NULL);
}	/* thread_DispatchCommand */


/* Helper function to thread_DispatchCommand that decides if we should dispatch
 * to this segment database.
 *
 * (1) don't dispatch if there is already a query cancel notice pending.
 * (2) make sure our libpq connection is still good.
 *
 * NOTE: since this is called via a thread, the same rules apply as to
 *		 thread_DispatchCommand absolutely no elog'ing.
 */
bool
shouldStillDispatchCommand(DispatchCommandParms *pParms, CdbDispatchResult * dispatchResult)
{
	SegmentDatabaseDescriptor *segdbDesc = dispatchResult->segdbDesc;
	CdbDispatchResults *gangResults = dispatchResult->meleeResults;

	/* Don't dispatch to a QE that is not connected. Note, that PQstatus() correctly
	 * handles the case where segdbDesc->conn is NULL, and we *definitely* want to
	 * produce an error for that case. */
	if (PQstatus(segdbDesc->conn) == CONNECTION_BAD)
	{
		char	   *msg = PQerrorMessage(segdbDesc->conn);

		/* Save error info for later. */
		cdbdisp_appendMessage(dispatchResult, LOG,
							  ERRCODE_GP_INTERCONNECTION_ERROR,
							  "Lost connection to %s.  %s",
							  segdbDesc->whoami,
							  msg ? msg : "");

		if (DEBUG4 >= log_min_messages)
		{
			/* Don't use elog, it's not thread-safe */
			write_log("Lost connection: %s", segdbDesc->whoami);
		}

		/* Free the PGconn object at once whenever we notice it's gone bad. */
		PQfinish(segdbDesc->conn);
		segdbDesc->conn = NULL;
		dispatchResult->stillRunning = false;

		return false;
	}

	/*
	 * Don't submit if already encountered an error. The error has already
	 * been noted, so just keep quiet.
	 */
	if (pParms->pleaseCancel || gangResults->errcode)
	{
		if (gangResults->cancelOnError)
		{
			dispatchResult->wasCanceled = true;

			if (Debug_cancel_print || DEBUG4 >= log_min_messages)
			{
				/* Don't use elog, it's not thread-safe */
				write_log("Error cleanup in progress; command not sent to %s", 
						  segdbDesc->whoami);
			}
			return false;
		}
	}

	/*
	 * Don't submit if client told us to cancel. The cancellation request has
	 * already been noted, so hush.
	 */
	if (InterruptPending &&
		gangResults->cancelOnError)
	{
		dispatchResult->wasCanceled = true;
		if (Debug_cancel_print || DEBUG4 >= log_min_messages)
			write_log("Cancellation request pending; command not sent to %s",
					  segdbDesc->whoami);
		return false;
	}

	return true;
}	/* shouldStillDispatchCommand */


/* Helper function to thread_DispatchCommand that actually kicks off the
 * command on the libpq connection.
 *
 * NOTE: since this is called via a thread, the same rules apply as to
 *		 thread_DispatchCommand absolutely no elog'ing.
 */
static void
dispatchCommandQuery(CdbDispatchResult	*dispatchResult,
					 const char			*query_text,
					 int				query_text_len,
					 const char			*strCommand)
{
	SegmentDatabaseDescriptor *segdbDesc = dispatchResult->segdbDesc;
	PGconn	   *conn = segdbDesc->conn;
	TimestampTz beforeSend = 0;
	long		secs;
	int			usecs;

	/* Don't use elog, it's not thread-safe */
	if (DEBUG3 >= log_min_messages)
		write_log("%s <- %.120s", segdbDesc->whoami, strCommand);

	if (DEBUG1 >= log_min_messages)
		beforeSend = GetCurrentTimestamp();

	/*
	 * Submit the command asynchronously.
	 */
	if (PQsendGpQuery_shared(conn, (char *)query_text, query_text_len) == 0)
	{
		char	   *msg = PQerrorMessage(segdbDesc->conn);

		if (DEBUG3 >= log_min_messages)
			write_log("PQsendMPPQuery_shared error %s %s",
					  segdbDesc->whoami, msg ? msg : "");

		/* Note the error. */
		cdbdisp_appendMessage(dispatchResult, LOG,
							  ERRCODE_GP_INTERCONNECTION_ERROR,
							  "Command could not be sent to segment db %s;  %s",
							  segdbDesc->whoami, msg ? msg : "");
		PQfinish(conn);
		segdbDesc->conn = NULL;
		dispatchResult->stillRunning = false;
	}
	
	if (DEBUG1 >= log_min_messages)
	{
		TimestampDifference(beforeSend,
							GetCurrentTimestamp(),
							&secs, &usecs);
		
		if (secs != 0 || usecs > 1000) /* Time > 1ms? */
			write_log("time for PQsendGpQuery_shared %ld.%06d", secs, usecs);
	}


	/*
	 * We'll keep monitoring this QE -- whether or not the command
	 * was dispatched -- in order to check for a lost connection
	 * or any other errors that libpq might have in store for us.
	 */
}	/* dispatchCommand */

/* Helper function to thread_DispatchCommand that actually kicks off the
 * command on the libpq connection.
 *
 * NOTE: since this is called via a thread, the same rules apply as to
 *		 thread_DispatchCommand absolutely no elog'ing.
 */
static void
dispatchCommandDtxProtocol(CdbDispatchResult	*dispatchResult,
						   const char			*query_text,
						   int					query_text_len,
						   DtxProtocolCommand	dtxProtocolCommand)
{
	SegmentDatabaseDescriptor *segdbDesc = dispatchResult->segdbDesc;
	PGconn	   *conn = segdbDesc->conn;

	/* Don't use elog, it's not thread-safe */
	if (DEBUG3 >= log_min_messages)
		write_log("%s <- dtx protocol command %d", segdbDesc->whoami, (int)dtxProtocolCommand);

	/*
	 * Submit the command asynchronously.
	 */
	if (PQsendGpQuery_shared(conn, (char *)query_text, query_text_len) == 0)
	{
		char *msg = PQerrorMessage(segdbDesc->conn);

		if (DEBUG3 >= log_min_messages)
			write_log("PQsendMPPQuery_shared error %s %s",segdbDesc->whoami,
						 msg ? msg : "");
		/* Note the error. */
		cdbdisp_appendMessage(dispatchResult, LOG,
							  ERRCODE_GP_INTERCONNECTION_ERROR,
							  "Command could not be sent to segment db %s;  %s",
							  segdbDesc->whoami,
							  msg ? msg : "");
		PQfinish(conn);
		segdbDesc->conn = NULL;
		dispatchResult->stillRunning = false;
	}

	/*
	 * We'll keep monitoring this QE -- whether or not the command
	 * was dispatched -- in order to check for a lost connection
	 * or any other errors that libpq might have in store for us.
	 */
}	/* dispatchCommand */


/* Helper function to thread_DispatchCommand that handles errors that occur
 * during the select() call.
 *
 * NOTE: since this is called via a thread, the same rules apply as to
 *		 thread_DispatchCommand absolutely no elog'ing.
 *
 * NOTE: The cleanup of the connections will be performed by handleSelectTimeout().
 */
void
handleSelectError(DispatchCommandParms *pParms,
				  int db_count,
				  int sock_errno)
{
	int			i;
	int			forceTimeoutCount;

	if (LOG >= log_min_messages)
	{
		/* Don't use elog, it's not thread-safe */
		write_log("handleSelectError select() failed; errno=%d", sock_errno);
	}

	/*
	 * Based on the select man page, we could get here with
	 * errno == EBADF (bad descriptor), EINVAL (highest descriptor negative or negative timeout)
	 * or ENOMEM (out of memory).
	 * This is most likely a programming error or a bad system failure, but we'll try to 
	 * clean up a bit anyhow.
	 *
	 * MPP-3551: We *can* get here as a result of some hardware issues. the timeout code
	 * knows how to clean up if we've lost contact with one of our peers.
	 *
	 * We should check a connection's integrity before calling PQisBusy().
	 */
	for (i = 0; i < db_count; i++)
	{
		CdbDispatchResult *dispatchResult = pParms->dispatchResultPtrArray[i];

		/* Skip if already finished or didn't dispatch. */
		if (!dispatchResult->stillRunning)
			continue;

		/* We're done with this QE, sadly. */
		if (PQstatus(dispatchResult->segdbDesc->conn) == CONNECTION_BAD)
		{
			char *msg;

			msg = PQerrorMessage(dispatchResult->segdbDesc->conn);
			if (msg)
				write_log("Dispatcher encountered connection error on %s: %s",
						  dispatchResult->segdbDesc->whoami, msg);

			write_log("Dispatcher noticed bad connection in handleSelectError()");

			/* Save error info for later. */
			cdbdisp_appendMessage(dispatchResult, LOG,
								  ERRCODE_GP_INTERCONNECTION_ERROR,
								  "Error after dispatch from %s: %s",
								  dispatchResult->segdbDesc->whoami,
								  msg ? msg : "unknown error");

			PQfinish(dispatchResult->segdbDesc->conn);
			dispatchResult->segdbDesc->conn = NULL;
			dispatchResult->stillRunning = false;
		}
	}

	forceTimeoutCount = 60; /* anything bigger than 30 */
	handleSelectTimeout(pParms, db_count, &forceTimeoutCount, false);

	return;

	/* No point in trying to cancel the other QEs with select() broken. */
}	/* handleSelectError */

static void
cdbdisp_issueCancel(SegmentDatabaseDescriptor *segdbDesc, bool *requestFlag)
{
	char errbuf[256];
	PGcancel * cn = PQgetCancel(segdbDesc->conn);

	/* PQcancel uses some strcpy/strcat functions; let's
	 * clear this for safety */
	MemSet(errbuf, 0, sizeof(errbuf));

	if (Debug_cancel_print || DEBUG4 >= log_min_messages)
		write_log("Calling PQcancel for %s", segdbDesc->whoami);

	/*
	 * Block here until segment postmaster replies that it has
	 * carried out the cancellation request, which means signaling
	 * SIGINT to the QE process, something like: pid =
	 * PQbackendPID(segdbDesc->conn); kill(pid, SIGINT);
	 */
	if (requestFlag != NULL)
		*requestFlag = true;
				
	if (PQcancel(cn, errbuf, 256) == 0)
	{	
		if (Debug_cancel_print || LOG >= log_min_messages)
			write_log("Unable to cancel: %s", errbuf);
	}
	PQfreeCancel(cn);
}

/* Helper function to thread_DispatchCommand that handles timeouts that occur
 * during the select() call.
 *
 * NOTE: since this is called via a thread, the same rules apply as to
 *		 thread_DispatchCommand absolutely no elog'ing.
 */
void
handleSelectTimeout(DispatchCommandParms * pParms,
					int db_count,
					int *timeoutCounter, bool useSampling)
{
	CdbDispatchResult *dispatchResult;
	CdbDispatchResults *meleeResults;
	SegmentDatabaseDescriptor *segdbDesc;
	int			i;

	/*
	 * Are there any QEs that should be canceled?
	 *
	 * CDB TODO: PQcancel() is expensive, and we do them
	 *			 serially.	Just do a few each time; save some
	 *			 for the next timeout.
	 */
	for (i = 0; i < db_count; i++)
	{							/* loop to check connection status */
		dispatchResult = pParms->dispatchResultPtrArray[i];
		if (dispatchResult == NULL)
			continue;
		segdbDesc = dispatchResult->segdbDesc;
		meleeResults = dispatchResult->meleeResults;

		/* Already finished with this QE? */
		if (!dispatchResult->stillRunning)
			continue;

		/*
		 * Send cancellation request to this QE if an error has been reported
		 * by another QE in the gang; or if a cancellation request is pending
		 * here on the QD. Don't cancel if requestor said not to, if no longer
		 * connected, or if we have already sent a cancellation request.
		 */
		if (InterruptPending || pParms->pleaseCancel || meleeResults->errcode)
		{
			if (meleeResults->cancelOnError &&
				!dispatchResult->requestedCancel &&
				!dispatchResult->wasCanceled &&
				segdbDesc->conn &&
				PQstatus(segdbDesc->conn) != CONNECTION_BAD)
			{
				cdbdisp_issueCancel(segdbDesc, &dispatchResult->requestedCancel);
			}
		}
	}

	/*
	 * check the connection still valid, set 1 min time interval
	 * this may affect performance, should turn it off if required.
	 */
	if ((*timeoutCounter)++ > 30)
	{
		*timeoutCounter = 0;

		for (i = 0; i < db_count; i++)
		{
			dispatchResult = pParms->dispatchResultPtrArray[i];
			segdbDesc = dispatchResult->segdbDesc;

			if (DEBUG5 >= log_min_messages)
				write_log("checking status %d of %d     %s stillRunning %d",
						  i+1, db_count, segdbDesc->whoami, dispatchResult->stillRunning);

			/* Skip if already finished or didn't dispatch. */
			if (!dispatchResult->stillRunning)
				continue;

			/*
			 * MPP-5012:
			 * There is a potential race between dispatch and
			 * query-cancel for large queries -- and the cancel code
			 * above only will attempt a single cancellation.
			 *
			 * If we hit the timeout, and the query has already been
			 * cancelled we'll try to re-cancel here.
			 *
			 * The race is:
			 *   QD cancels before QE has started work, QE 'cancels'
			 * from idle_state -> idle_state and then starts work.
			 */
			if (dispatchResult->stillRunning &&
				dispatchResult->requestedCancel &&
				segdbDesc->conn &&
				PQstatus(segdbDesc->conn) != CONNECTION_BAD)
			{
				cdbdisp_issueCancel(segdbDesc, &dispatchResult->requestedCancel);
			}

			/* Skip the entry db. */
			if (segdbDesc->segindex < 0)
				continue;

			if (DEBUG5 >= log_min_messages)
				write_log("testing connection %d of %d     %s stillRunning %d",
						  i+1, db_count, segdbDesc->whoami, dispatchResult->stillRunning);

			if (!FtsTestConnection(segdbDesc->segment_database_info, false))
			{
				/* Note the error. */
				cdbdisp_appendMessage(dispatchResult, DEBUG1,
									  ERRCODE_GP_INTERCONNECTION_ERROR,
									  "Lost connection to one or more segments - fault detector checking for segment failures. (%s)",
									  segdbDesc->whoami);

				/*
				 * Not a good idea to store into the PGconn object. Instead,
				 * just close it.
				 */
				PQfinish(segdbDesc->conn);
				segdbDesc->conn = NULL;

				/* This connection is hosed. */
				dispatchResult->stillRunning = false;
			}
		}
	}

}	/* handleSelectTimeout */

void
CollectQEWriterTransactionInformation(SegmentDatabaseDescriptor *segdbDesc, CdbDispatchResult *dispatchResult)
{
	PGconn *conn = segdbDesc->conn;
	
	if (conn && conn->QEWriter_HaveInfo)
	{
		dispatchResult->QEIsPrimary = true;
		dispatchResult->QEWriter_HaveInfo = true;
		dispatchResult->QEWriter_DistributedTransactionId = conn->QEWriter_DistributedTransactionId;
		dispatchResult->QEWriter_CommandId = conn->QEWriter_CommandId;
		if (conn && conn->QEWriter_Dirty)
		{
			dispatchResult->QEWriter_Dirty = true;
		}
	}
}

bool							/* returns true if command complete */
processResults(CdbDispatchResult *dispatchResult)
{
	SegmentDatabaseDescriptor *segdbDesc = dispatchResult->segdbDesc;
	char	   *msg;
	int			rc;

	/* MPP-2518: PQisBusy() has side-effects */
	if (DEBUG5 >= log_min_messages)
	{
		write_log("processResults.  isBusy = %d", PQisBusy(segdbDesc->conn));

		if (PQstatus(segdbDesc->conn) == CONNECTION_BAD)
			goto connection_error;
	}

	/* Receive input from QE. */
	rc = PQconsumeInput(segdbDesc->conn);

	/* If PQconsumeInput fails, we're hosed. */
	if (rc == 0)
	{ /* handle PQconsumeInput error */
		goto connection_error;
	}

	/* MPP-2518: PQisBusy() has side-effects */
	if (DEBUG4 >= log_min_messages && PQisBusy(segdbDesc->conn))
		write_log("PQisBusy");
			
	/* If we have received one or more complete messages, process them. */
	while (!PQisBusy(segdbDesc->conn))
	{							/* loop to call PQgetResult; won't block */
		PGresult   *pRes;
		ExecStatusType resultStatus;
		int			resultIndex;

		/* MPP-2518: PQisBusy() does some error handling, which can
		 * cause the connection to die -- we can't just continue on as
		 * if the connection is happy without checking first. 
		 *
		 * For example, cdbdisp_numPGresult() will return a completely
		 * bogus value! */
		if (PQstatus(segdbDesc->conn) == CONNECTION_BAD || segdbDesc->conn->sock == -1)
		{ /* connection is dead. */
			goto connection_error;
		}

		resultIndex = cdbdisp_numPGresult(dispatchResult);

		if (DEBUG4 >= log_min_messages)
			write_log("PQgetResult");
		/* Get one message. */
		pRes = PQgetResult(segdbDesc->conn);
		
		CollectQEWriterTransactionInformation(segdbDesc, dispatchResult);

		/*
		 * Command is complete when PGgetResult() returns NULL. It is critical
		 * that for any connection that had an asynchronous command sent thru
		 * it, we call PQgetResult until it returns NULL. Otherwise, the next
		 * time a command is sent to that connection, it will return an error
		 * that there's a command pending.
		 */
		if (!pRes)
		{						/* end of results */
			if (DEBUG4 >= log_min_messages)
			{
				/* Don't use elog, it's not thread-safe */
				write_log("%s -> idle", segdbDesc->whoami);
			}
						
			return true;		/* this is normal end of command */
		}						/* end of results */

		
		/* Attach the PGresult object to the CdbDispatchResult object. */
		cdbdisp_appendResult(dispatchResult, pRes);

		/* Did a command complete successfully? */
		resultStatus = PQresultStatus(pRes);
		if (resultStatus == PGRES_COMMAND_OK ||
			resultStatus == PGRES_TUPLES_OK ||
			resultStatus == PGRES_COPY_IN ||
			resultStatus == PGRES_COPY_OUT)
		{						/* QE reported success */

			/*
			 * Save the index of the last successful PGresult. Can be given to
			 * cdbdisp_getPGresult() to get tuple count, etc.
			 */
			dispatchResult->okindex = resultIndex;

			if (DEBUG3 >= log_min_messages)
			{
				/* Don't use elog, it's not thread-safe */
				char	   *cmdStatus = PQcmdStatus(pRes);

				write_log("%s -> ok %s",
						  segdbDesc->whoami,
						  cmdStatus ? cmdStatus : "(no cmdStatus)");
			}
			
			/* SREH - get number of rows rejected from QE if any */
			if(pRes->numRejected > 0)
				dispatchResult->numrowsrejected += pRes->numRejected;

			if (resultStatus == PGRES_COPY_IN ||
				resultStatus == PGRES_COPY_OUT)
				return true;
		}						/* QE reported success */

		/* Note QE error.  Cancel the whole statement if requested. */
		else
		{						/* QE reported an error */
			char	   *sqlstate = PQresultErrorField(pRes, PG_DIAG_SQLSTATE);
			int			errcode = 0;

			msg = PQresultErrorMessage(pRes);

			if (DEBUG2 >= log_min_messages)
			{
				/* Don't use elog, it's not thread-safe */
				write_log("%s -> %s %s  %s",
						  segdbDesc->whoami,
						  PQresStatus(resultStatus),
						  sqlstate ? sqlstate : "(no SQLSTATE)",
						  msg ? msg : "");
			}

			/*
			 * Convert SQLSTATE to an error code (ERRCODE_xxx). Use a generic
			 * nonzero error code if no SQLSTATE.
			 */
			if (sqlstate &&
				strlen(sqlstate) == 5)
				errcode = cdbdisp_sqlstate_to_errcode(sqlstate);

			/*
			 * Save first error code and the index of its PGresult buffer
			 * entry.
			 */
			cdbdisp_seterrcode(errcode, resultIndex, dispatchResult);
		}						/* QE reported an error */
	}							/* loop to call PQgetResult; won't block */
	
	return false;				/* we must keep on monitoring this socket */

connection_error:
	msg = PQerrorMessage(segdbDesc->conn);

	if (msg)
		write_log("Dispatcher encountered connection error on %s: %s", segdbDesc->whoami, msg);

	/* Save error info for later. */
	cdbdisp_appendMessage(dispatchResult, LOG,
						  ERRCODE_GP_INTERCONNECTION_ERROR,
						  "Error on receive from %s: %s",
						  segdbDesc->whoami,
						  msg ? msg : "unknown error");

	/* Can't recover, so drop the connection. */
	PQfinish(segdbDesc->conn);
	segdbDesc->conn = NULL;
	dispatchResult->stillRunning = false;

	return true; /* connection is gone! */	
}	/* processResults */


/*--------------------------------------------------------------------*/


/*
 * CdbDoCommandNoTxn:
 * Combination of cdbdisp_dispatchCommand and cdbdisp_finishCommand.
 * If not all QEs execute the command successfully, throws an error and
 * does not return.
 *
 * needTwoPhase specifies a desire to include global transaction control
 *  before dispatch.
 */
void
CdbDoCommand(const char *strCommand,
			 bool cancelOnError,
			 bool needTwoPhase)
{
	CdbDispatcherState ds = {NULL, NULL};
	const bool withSnapshot = true;

	elog((Debug_print_full_dtm ? LOG : DEBUG5), "CdbDoCommand for command = '%s', needTwoPhase = %s",
		 strCommand, (needTwoPhase ? "true" : "false"));

	dtmPreCommand("CdbDoCommand", strCommand, NULL, needTwoPhase, withSnapshot, false /* inCursor */);

	cdbdisp_dispatchCommand(strCommand, NULL, 0, cancelOnError, needTwoPhase, 
							/* withSnapshot */true, &ds);

	/*
	 * Wait for all QEs to finish. If not all of our QEs were successful,
	 * report the error and throw up.
	 */
	cdbdisp_finishCommand(&ds, NULL, NULL);
}	/* CdbDoCommandNoTxn */


void
CdbDoCommandOnAllGangs(const char *strCommand,
					   bool cancelOnError,
					   bool needTwoPhase)
{
	volatile CdbDispatcherState ds = {NULL, NULL};
	const bool withSnapshot = true;

	elog((Debug_print_full_dtm ? LOG : DEBUG5), "CdbDoCommandOnAllGangs for command = '%s', needTwoPhase = %s",
		 strCommand, (needTwoPhase ? "true" : "false"));

	dtmPreCommand("CdbDoCommandOnAllGangs", strCommand, NULL, needTwoPhase, withSnapshot, false /* inCursor */ );

	PG_TRY();
	{
		cdbdisp_dispatchCommandToAllGangs(strCommand, NULL, 0, NULL, 0, cancelOnError, needTwoPhase, (struct CdbDispatcherState *)&ds);

		/*
		 * Wait for all QEs to finish. If not all of our QEs were successful,
		 * report the error and throw up.
		 */
		cdbdisp_finishCommand((struct CdbDispatcherState *)&ds, NULL, NULL);
	}
	PG_CATCH();
	{
		/* Something happend, clean up after ourselves */
		CdbCheckDispatchResult((struct CdbDispatcherState *)&ds, true);

		cdbdisp_destroyDispatchResults(ds.primaryResults);
		cdbdisp_destroyDispatchThreads(ds.dispatchThreads);

		PG_RE_THROW();
		/* not reached */
	}
	PG_END_TRY();
}	

/*--------------------------------------------------------------------*/



/*
 * Let's evaluate all STABLE functions that have constant args before dispatch, so we get a consistent
 * view across QEs
 *
 * Also, if this is a single_row insert, let's evaluate nextval() and currval() before dispatching
 *
 */

static Node *
pre_dispatch_function_evaluation_mutator(Node *node,
										 pre_dispatch_function_evaluation_context * context)
{
	Node * new_node = 0;
	
	if (node == NULL)
		return NULL;

	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		/* Not replaceable, so just copy the Param (no need to recurse) */
		return (Node *) copyObject(param);
	}
	else if (IsA(node, FuncExpr))
	{
		FuncExpr   *expr = (FuncExpr *) node;
		List	   *args;
		ListCell   *arg;
		Expr	   *simple;
		FuncExpr   *newexpr;
		bool		has_nonconst_input;

		Form_pg_proc funcform;
		EState	   *estate;
		ExprState  *exprstate;
		MemoryContext oldcontext;
		Datum		const_val;
		bool		const_is_null;
		int16		resultTypLen;
		bool		resultTypByVal;

		Oid			funcid;
		HeapTuple	func_tuple;


		/*
		 * Reduce constants in the FuncExpr's arguments.  We know args is
		 * either NIL or a List node, so we can call expression_tree_mutator
		 * directly rather than recursing to self.
		 */
		args = (List *) expression_tree_mutator((Node *) expr->args,
												pre_dispatch_function_evaluation_mutator,
												(void *) context);
										
		funcid = expr->funcid;

		newexpr = makeNode(FuncExpr);
		newexpr->funcid = expr->funcid;
		newexpr->funcresulttype = expr->funcresulttype;
		newexpr->funcretset = expr->funcretset;
		newexpr->funcformat = expr->funcformat;
		newexpr->args = args;

		/*
		 * Check for constant inputs
		 */
		has_nonconst_input = false;
		
		foreach(arg, args)
		{
			if (!IsA(lfirst(arg), Const))
			{
				has_nonconst_input = true;
				break;
			}
		}
		
		if (!has_nonconst_input)
		{
			bool is_seq_func = false;
			bool tup_or_set;
			cqContext	*pcqCtx;

			pcqCtx = caql_beginscan(
					NULL,
					cql("SELECT * FROM pg_proc "
						" WHERE oid = :1 ",
						ObjectIdGetDatum(funcid)));

			func_tuple = caql_getnext(pcqCtx);

			if (!HeapTupleIsValid(func_tuple))
				elog(ERROR, "cache lookup failed for function %u", funcid);

			funcform = (Form_pg_proc) GETSTRUCT(func_tuple);

			/* can't handle set returning or row returning functions */
			tup_or_set = (funcform->proretset || 
						  type_is_rowtype(funcform->prorettype));

			caql_endscan(pcqCtx);
			
			/* can't handle it */
			if (tup_or_set)
			{
				/* 
				 * We haven't mutated this node, but we still return the
				 * mutated arguments.
				 *
				 * If we don't do this, we'll miss out on transforming function
				 * arguments which are themselves functions we need to mutated.
				 * For example, select foo(now()).
				 *
				 * See MPP-3022 for what happened when we didn't do this.
				 */
				return (Node *)newexpr;
			}

			/* 
			 * Ignored evaluation of gp_partition stable functions.
			 * TODO: garcic12 - May 30, 2013, refactor gp_partition stable functions to be truly
			 * stable (JIRA: MPP-19541).
			 */
			if (funcid == GP_PARTITION_SELECTION_OID 
				|| funcid == GP_PARTITION_EXPANSION_OID 
				|| funcid == GP_PARTITION_INVERSE_OID)
			{
				return (Node *)newexpr;
			}

			/* 
			 * Related to MPP-1429.  Here we want to mark any statement that is
			 * going to use a sequence as dirty.  Doing this means that the
			 * QD will flush the xlog which will also flush any xlog writes that
			 * the sequence server might do. 
			 */
			if (funcid == NEXTVAL_FUNC_OID || funcid == CURRVAL_FUNC_OID ||
				funcid == SETVAL_FUNC_OID)
			{
				ExecutorMarkTransactionUsesSequences();
				is_seq_func = true;
			}

			if (funcform->provolatile == PROVOLATILE_IMMUTABLE)
				/* okay */ ;
			else if (funcform->provolatile == PROVOLATILE_STABLE)
				/* okay */ ;
			else if (context->single_row_insert && is_seq_func)
				;				/* Volatile, but special sequence function */
			else
				return (Node *)newexpr;

			/*
			 * Ok, we have a function that is STABLE (or IMMUTABLE), with
			 * constant args. Let's try to evaluate it.
			 */

			/*
			 * To use the executor, we need an EState.
			 */
			estate = CreateExecutorState();

			/* We can use the estate's working context to avoid memory leaks. */
			oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

			/*
			 * Prepare expr for execution.
			 */
			exprstate = ExecPrepareExpr((Expr *) newexpr, estate);

			/*
			 * And evaluate it.
			 *
			 * It is OK to use a default econtext because none of the
			 * ExecEvalExpr() code used in this situation will use econtext.
			 * That might seem fortuitous, but it's not so unreasonable --- a
			 * constant expression does not depend on context, by definition,
			 * n'est-ce pas?
			 */
			const_val =
				ExecEvalExprSwitchContext(exprstate,
										  GetPerTupleExprContext(estate),
										  &const_is_null, NULL);

			/* Get info needed about result datatype */
			get_typlenbyval(expr->funcresulttype, &resultTypLen, &resultTypByVal);

			/* Get back to outer memory context */
			MemoryContextSwitchTo(oldcontext);

			/* Must copy result out of sub-context used by expression eval */
			if (!const_is_null)
				const_val = datumCopy(const_val, resultTypByVal, resultTypLen);

			/* Release all the junk we just created */
			FreeExecutorState(estate);

			/*
			 * Make the constant result node.
			 */
			simple = (Expr *) makeConst(expr->funcresulttype, -1, resultTypLen,
										const_val, const_is_null,
										resultTypByVal);

			if (simple)			/* successfully simplified it */
				return (Node *) simple;
		}

		/*
		 * The expression cannot be simplified any further, so build and
		 * return a replacement FuncExpr node using the possibly-simplified
		 * arguments.
		 */
		return (Node *) newexpr;
	}
	else if (IsA(node, OpExpr))
	{
		OpExpr	   *expr = (OpExpr *) node;
		List	   *args;

		OpExpr	   *newexpr;

		/*
		 * Reduce constants in the OpExpr's arguments.  We know args is either
		 * NIL or a List node, so we can call expression_tree_mutator directly
		 * rather than recursing to self.
		 */
		args = (List *) expression_tree_mutator((Node *) expr->args,
												pre_dispatch_function_evaluation_mutator,
												(void *) context);

		/*
		 * Need to get OID of underlying function.	Okay to scribble on input
		 * to this extent.
		 */
		set_opfuncid(expr);

		newexpr = makeNode(OpExpr);
		newexpr->opno = expr->opno;
		newexpr->opfuncid = expr->opfuncid;
		newexpr->opresulttype = expr->opresulttype;
		newexpr->opretset = expr->opretset;
		newexpr->args = args;

		return (Node *) newexpr;
	}
	else if (IsA(node, CurrentOfExpr))
	{
		/*
		 * updatable cursors 
		 *
		 * During constant folding, the CurrentOfExpr's gp_segment_id, ctid, 
		 * and tableoid fields are filled in with observed values from the 
		 * referenced cursor. For more detail, see bindCurrentOfParams below.
		 */
		CurrentOfExpr *expr = (CurrentOfExpr *) node,
					  *newexpr = copyObject(expr);

		bindCurrentOfParams(newexpr->cursor_name,
							newexpr->target_relid,
			   		   		&newexpr->ctid,
					  	   	&newexpr->gp_segment_id,
					  	   	&newexpr->tableoid);
		return (Node *) newexpr;
	}
	
	/*
	 * For any node type not handled above, we recurse using
	 * plan_tree_mutator, which will copy the node unchanged but try to
	 * simplify its arguments (if any) using this routine.
	 */
	new_node =  plan_tree_mutator(node, pre_dispatch_function_evaluation_mutator,
								  (void *) context);

	return new_node;
}

/*
 * bindCurrentOfParams
 *
 * During constant folding, we evaluate STABLE functions to give QEs a consistent view
 * of the query. At this stage, we will also bind observed values of 
 * gp_segment_id/ctid/tableoid into the CurrentOfExpr.
 * This binding must happen only after planning, otherwise we disrupt prepared statements.
 * Furthermore, this binding must occur before dispatch, because a QE lacks the 
 * the information needed to discern whether it's responsible for the currently 
 * positioned tuple.
 *
 * The design of this parameter binding is very tightly bound to the parse/analyze
 * and subsequent planning of DECLARE CURSOR. We depend on the "is_simply_updatable"
 * calculation of parse/analyze to decide whether CURRENT OF makes sense for the
 * referenced cursor. Moreover, we depend on the ensuing planning of DECLARE CURSOR
 * to provide the junk metadata of gp_segment_id/ctid/tableoid (per tuple).
 *
 * This function will lookup the portal given by "cursor_name". If it's simply updatable,
 * we'll glean gp_segment_id/ctid/tableoid from the portal's most recently fetched 
 * (raw) tuple. We bind this information into the CurrentOfExpr to precisely identify
 * the currently scanned tuple, ultimately for consumption of TidScan/execQual by the QEs.
 */
static void
bindCurrentOfParams(char *cursor_name, Oid target_relid, ItemPointer ctid, int *gp_segment_id, Oid *tableoid)
{
	char 			*table_name;
	Portal			portal;
	QueryDesc		*queryDesc;
	bool			found_attribute, isnull;
	Datum			value;

	portal = GetPortalByName(cursor_name);
	if (!PortalIsValid(portal))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" does not exist", cursor_name)));

	queryDesc = PortalGetQueryDesc(portal);
	if (queryDesc == NULL)
		ereport(ERROR, 
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" is held from a previous transaction", cursor_name)));

	/* obtain table_name for potential error messages */
	table_name = get_rel_name(target_relid);

	/* 
	 * The referenced cursor must be simply updatable. This has already
	 * been discerned by parse/analyze for the DECLARE CURSOR of the given
	 * cursor. This flag assures us that gp_segment_id, ctid, and tableoid (if necessary)
 	 * will be available as junk metadata, courtesy of preprocess_targetlist.
	 */
	if (!portal->is_simply_updatable)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" is not a simply updatable scan of table \"%s\"",
						cursor_name, table_name)));

	/* 
	 * The target relation must directly match the cursor's relation. This throws out
	 * the simple case in which a cursor is declared against table X and the update is
	 * issued against Y. Moreover, this disallows some subtler inheritance cases where
	 * Y inherits from X. While such cases could be implemented, it seems wiser to
	 * simply error out cleanly.
	 */
	Index varno = extractSimplyUpdatableRTEIndex(queryDesc->plannedstmt->rtable);
	Oid cursor_relid = getrelid(varno, queryDesc->plannedstmt->rtable);
	if (target_relid != cursor_relid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" is not a simply updatable scan of table \"%s\"",
						cursor_name, table_name)));
	/* 
	 * The cursor must have a current result row: per the SQL spec, it's 
	 * an error if not.
	 */
	if (portal->atStart || portal->atEnd)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" is not positioned on a row", cursor_name)));

	/*
	 * As mentioned above, if parse/analyze recognized this cursor as simply
	 * updatable during DECLARE CURSOR, then its subsequent planning must have
	 * made gp_segment_id, ctid, and tableoid available as junk for each tuple.
	 *
	 * To retrieve this junk metadeta, we leverage the EState's junkfilter against
	 * the raw tuple yielded by the highest most node in the plan.
	 */
	TupleTableSlot *slot = queryDesc->planstate->ps_ResultTupleSlot;
	Insist(!TupIsNull(slot));
	Assert(queryDesc->estate->es_junkFilter);

	/* extract gp_segment_id metadata */
	found_attribute = ExecGetJunkAttribute(queryDesc->estate->es_junkFilter,
										   slot,
						 				   "gp_segment_id",
						 				   &value,
						 				   &isnull);
	Insist(found_attribute);
	Assert(!isnull);
	*gp_segment_id = DatumGetInt32(value);

	/* extract ctid metadata */
	found_attribute = ExecGetJunkAttribute(queryDesc->estate->es_junkFilter,
						 				   slot,
						 				   "ctid",
						 				   &value,
						 				   &isnull);
	Insist(found_attribute);
	Assert(!isnull);
	ItemPointerCopy(DatumGetItemPointer(value), ctid);

	/* 
	 * extract tableoid metadata
	 *
	 * DECLARE CURSOR planning only includes tableoid metadata when
	 * scrolling a partitioned table, as this is the only case in which
	 * gp_segment_id/ctid alone do not suffice to uniquely identify a tuple.
	 */
	found_attribute = ExecGetJunkAttribute(queryDesc->estate->es_junkFilter,
										   slot,
						 				   "tableoid",
						 				   &value,
						 				   &isnull);
	if (found_attribute)
	{
		Assert(!isnull);	
		*tableoid = DatumGetObjectId(value);
		
		/*
		 * This is our last opportunity to verify that the physical table given
		 * by tableoid is, indeed, simply updatable.
		 */
		if (!isSimplyUpdatableRelation(*tableoid))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("%s is not updatable",
							get_rel_name_partition(*tableoid))));
	} else
		*tableoid = InvalidOid;

	pfree(table_name);
}

/*
 * This code was refactored out of cdbdisp_dispatchPlan.  It's
 * used both for dispatching plans when we are using normal gangs,
 * and for dispatching all statements from Query Dispatch Agents
 * when we are using dispatch agents.
 */
void
cdbdisp_dispatchX(DispatchCommandQueryParms *pQueryParms,
				  bool cancelOnError,
				  struct SliceTable *sliceTbl,
				  struct CdbDispatcherState *ds)
{
	int			oldLocalSlice = 0;
	sliceVec	*sliceVector = NULL;
	int			nSlices = 1;
	int			sliceLim = 1;
	int			iSlice;
	int			rootIdx = pQueryParms->rootIdx;

	if (log_dispatch_stats)
		ResetUsage();

	ds->primaryResults = NULL;
	ds->dispatchThreads = NULL;

	Assert(Gp_role == GP_ROLE_DISPATCH);
	
	if (sliceTbl)
	{
		Assert(rootIdx == 0 ||
			   (rootIdx > sliceTbl->nMotions &&
				rootIdx <= sliceTbl->nMotions + sliceTbl->nInitPlans));
	
		/*
		 * Keep old value so we can restore it.  We use this field as a parameter.
		 */
		oldLocalSlice = sliceTbl->localSlice;

		/*
		 * Traverse the slice tree in sliceTbl rooted at rootIdx and build a
		 * vector of slice indexes specifying the order of [potential] dispatch.
		 */
		sliceLim = list_length(sliceTbl->slices);
		sliceVector = palloc0(sliceLim * sizeof(sliceVec));
	
		nSlices = fillSliceVector(sliceTbl, rootIdx, sliceVector, sliceLim);
	}

	/* Allocate result array with enough slots for QEs of primary gangs. */
	ds->primaryResults = cdbdisp_makeDispatchResults(nSlices * largestGangsize(),
												   sliceLim,
												   cancelOnError);

	cdb_total_plans++;
	cdb_total_slices += nSlices;
	if (nSlices > cdb_max_slices)
		cdb_max_slices = nSlices;
	
	/* must have somebody to dispatch to. */
	Assert(sliceTbl != NULL || pQueryParms->primary_gang_id > 0);
	
	if (DEBUG1 >= log_min_messages)
	{
		char		msec_str[32];
		switch (check_log_duration(msec_str, false))
		{
			case 1:
			case 2:
				ereport(LOG, (errmsg("duration to start of dispatch send (root %d): %s ms", pQueryParms->rootIdx, msec_str)));
				break;
		}
	}

	/*
	 * Now we need to call CDBDispatchCommand once per slice.  Each such
	 * call dispatches a MPPEXEC command to each of the QEs assigned to
	 * the slice.
	 *
	 * The QE information goes in two places: (1) in the argument to the
	 * function CDBDispatchCommand, and (2) in the serialized
	 * command sent to the QEs.
	 *
	 * So, for each slice in the tree...
	 */
	for (iSlice = 0; iSlice < nSlices; iSlice++)
	{
		CdbDispatchDirectDesc direct;
		Gang	   *primaryGang = NULL;
		Slice *slice = NULL;
		int si = -1;

		if (sliceVector)
		{
			/*
			 * Sliced dispatch, and we are either the dispatch agent, or we are
			 * the QD and are not using Dispatch Agents.
			 * 
			 * So, dispatch to each slice.
			 */
			slice = sliceVector[iSlice].slice;
			si = slice->sliceIndex;

			/*
			 * Is this a slice we should dispatch?
			 */
			if (slice && slice->gangType == GANGTYPE_UNALLOCATED)
			{
				/*
				 * Most slices are dispatched, however, in many  cases the
				 * root runs only on the QD and is not dispatched to the QEs.
				 */
				continue;
			}

			primaryGang = slice->primaryGang;

			/*
			 * If we are on the dispatch agent, the gang pointers aren't filled in.
			 * We must look them up by ID
			 */
			if (primaryGang == NULL)
			{
				elog(DEBUG2,"Dispatch %d, Gangs are %d, type=%d",iSlice, slice->primary_gang_id, slice->gangType);
				primaryGang = findGangById(slice->primary_gang_id);

				Assert(primaryGang != NULL);
				if (primaryGang != NULL)
					Assert(primaryGang->type == slice->gangType || primaryGang->type == GANGTYPE_PRIMARY_WRITER);

			}

			if (slice->directDispatch.isDirectDispatch)
			{
				direct.directed_dispatch = true;
				direct.count = list_length(slice->directDispatch.contentIds);
				Assert(direct.count == 1); /* only support to single content right now.  If this changes then we need to change from a list to another structure to avoid n^2 cases */
				direct.content[0] = linitial_int(slice->directDispatch.contentIds);

				if (Test_print_direct_dispatch_info)
				{
					elog(INFO, "Dispatch command to SINGLE content");
				}
			}
			else
			{
				direct.directed_dispatch = false;
				direct.count = 0;

				if (Test_print_direct_dispatch_info)
				{
					elog(INFO, "Dispatch command to ALL contents");
				}
			}
		}
		else
		{
			direct.directed_dispatch = false;
			direct.count = 0;

			if (Test_print_direct_dispatch_info)
			{
				elog(INFO, "Dispatch command to ALL contents");
			}

			/*
			 *  Non-sliced, used specified gangs
			 */
			elog(DEBUG2,"primary %d",pQueryParms->primary_gang_id);
			if (pQueryParms->primary_gang_id > 0)
				primaryGang = findGangById(pQueryParms->primary_gang_id);
		}

		Assert(primaryGang != NULL);	/* Must have a gang to dispatch to */
		if (primaryGang)
		{
			Assert(ds->primaryResults && ds->primaryResults->resultArray);
		}

#ifdef FAULT_INJECTOR
		int faultType = FaultInjector_InjectFaultIfSet(
							BeforeDispatch,
							DDLNotSpecified,
							"",	//databaseName
							""); // tableName

		if(faultType == FaultInjectorTypeUserCancel)
		{
			QueryCancelPending = true;
			InterruptPending = true;
		}
		if(faultType == FaultInjectorTypeProcDie)
		{
			ProcDiePending = true;
			InterruptPending = true;
		}
#endif

		/* Bail out if already got an error or cancellation request. */
		if (cancelOnError)
		{
			if (ds->primaryResults->errcode)
				break;
			if (InterruptPending)
				break;
		}

		/*
		 * Dispatch the plan to our primaryGang.
		 * Doesn't wait for it to finish.
		 */
		if (primaryGang != NULL)
		{
			if (primaryGang->type == GANGTYPE_PRIMARY_WRITER)
				ds->primaryResults->writer_gang = primaryGang;

			cdbdisp_dispatchToGang(ds,
								   GP_DISPATCH_COMMAND_TYPE_QUERY,
								   pQueryParms, primaryGang,
								   si, sliceLim, &direct);
		}
	}

	if (sliceVector)
		pfree(sliceVector);

	if (sliceTbl)
		sliceTbl->localSlice = oldLocalSlice;

	/*
	 * If failed before completely dispatched, stop QEs and throw error.
	 */
	if (iSlice < nSlices)
	{
		elog(Debug_cancel_print ? LOG : DEBUG2, "Plan dispatch canceled; dispatched %d of %d slices", iSlice, nSlices);

		/* Cancel any QEs still running, and wait for them to terminate. */
		CdbCheckDispatchResult(ds, true);

		/*
		 * Check and free the results of all gangs. If any QE had an
		 * error, report it and exit via PG_THROW.
		 */
		cdbdisp_finishCommand(ds, NULL, NULL);

		/* Wasn't an error, must have been an interrupt. */
		CHECK_FOR_INTERRUPTS();

		/* Strange!  Not an interrupt either. */
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg_internal("Unable to dispatch plan.")));
	}

	if (DEBUG1 >= log_min_messages)
	{
		char		msec_str[32];
		switch (check_log_duration(msec_str, false))
		{
			case 1:
			case 2:
				ereport(LOG, (errmsg("duration to dispatch out (root %d): %s ms", pQueryParms->rootIdx,msec_str)));
				break;
		}
	}

}	/* cdbdisp_dispatchX */

/*
 * Evaluate functions to constants.
 */
Node *
exec_make_plan_constant(struct PlannedStmt *stmt, bool is_SRI)
{
	pre_dispatch_function_evaluation_context pcontext;

	Assert(stmt);
	exec_init_plan_tree_base(&pcontext.base, stmt);
	pcontext.single_row_insert = is_SRI;

	return plan_tree_mutator((Node *)stmt->planTree, pre_dispatch_function_evaluation_mutator, &pcontext);
}

Node *
planner_make_plan_constant(struct PlannerInfo *root, Node *n, bool is_SRI)
{
	pre_dispatch_function_evaluation_context pcontext;

	planner_init_plan_tree_base(&pcontext.base, root);
	pcontext.single_row_insert = is_SRI;

	return plan_tree_mutator(n, pre_dispatch_function_evaluation_mutator, &pcontext);
}

/*
 * Compose and dispatch the MPPEXEC commands corresponding to a plan tree
 * within a complete parallel plan.  (A plan tree will correspond either
 * to an initPlan or to the main plan.)
 *
 * If cancelOnError is true, then any dispatching error, a cancellation
 * request from the client, or an error from any of the associated QEs,
 * may cause the unfinished portion of the plan to be abandoned or canceled;
 * and in the event this occurs before all gangs have been dispatched, this
 * function does not return, but waits for all QEs to stop and exits to
 * the caller's error catcher via ereport(ERROR,...).  Otherwise this
 * function returns normally and errors are not reported until later.
 *
 * If cancelOnError is false, the plan is to be dispatched as fully as
 * possible and the QEs allowed to proceed regardless of cancellation
 * requests, errors or connection failures from other QEs, etc.
 *
 * The CdbDispatchResults objects allocated for the plan are returned
 * in *pPrimaryResults.  The caller, after calling
 * CdbCheckDispatchResult(), can examine the CdbDispatchResults
 * objects, can keep them as long as needed, and ultimately must free
 * them with cdbdisp_destroyDispatchResults() prior to deallocation of
 * the caller's memory context.  Callers should use PG_TRY/PG_CATCH to
 * ensure proper cleanup.
 *
 * To wait for completion, check for errors, and clean up, it is
 * suggested that the caller use cdbdisp_finishCommand().
 *
 * Note that the slice tree dispatched is the one specified in the EState
 * of the argument QueryDesc as es_cur__slice.
 *
 * Note that the QueryDesc params must include PARAM_EXEC_REMOTE parameters
 * containing the values of any initplans required by the slice to be run.
 * (This is handled by calls to addRemoteExecParamsToParamList() from the
 * functions preprocess_initplans() and ExecutorRun().)
 *
 * Each QE receives its assignment as a message of type 'M' in PostgresMain().
 * The message is deserialized and processed by exec_mpp_query() in postgres.c.
 */
void
cdbdisp_dispatchPlan(struct QueryDesc *queryDesc,
					 bool planRequiresTxn,
					 bool cancelOnError,
					 struct CdbDispatcherState *ds)
{
	char 	*splan,
		*ssliceinfo,
		*sparams;

	int 	splan_len,
		ssliceinfo_len,
		sparams_len;

	SliceTable *sliceTbl;
	int			rootIdx;
	int			oldLocalSlice;
	PlannedStmt	   *stmt;
	bool		is_SRI;
	
	DispatchCommandQueryParms queryParms;
	CdbComponentDatabaseInfo *qdinfo;
 
	ds->primaryResults = NULL;
	ds->dispatchThreads = NULL;

	Assert(Gp_role == GP_ROLE_DISPATCH);
	Assert(queryDesc != NULL && queryDesc->estate != NULL);

	/*
	 * Later we'll need to operate with the slice table provided via the
	 * EState structure in the argument QueryDesc.	Cache this information
	 * locally and assert our expectations about it.
	 */
	sliceTbl = queryDesc->estate->es_sliceTable;
	rootIdx = RootSliceIndex(queryDesc->estate);

	Assert(sliceTbl != NULL);
	Assert(rootIdx == 0 ||
		   (rootIdx > sliceTbl->nMotions && rootIdx <= sliceTbl->nMotions + sliceTbl->nInitPlans));

	/*
	 * Keep old value so we can restore it.  We use this field as a parameter.
	 */
	oldLocalSlice = sliceTbl->localSlice;

	/*
	 * This function is called only for planned statements.
	 */
	stmt = queryDesc->plannedstmt;
	Assert(stmt);


	/*
	 * Let's evaluate STABLE functions now, so we get consistent values on the QEs
	 *
	 * Also, if this is a single-row INSERT statement, let's evaluate
	 * nextval() and currval() now, so that we get the QD's values, and a
	 * consistent value for everyone
	 *
	 */

	is_SRI = false;

	if (queryDesc->operation == CMD_INSERT)
	{
		Assert(stmt->commandType == CMD_INSERT);

		/* We might look for constant input relation (instead of SRI), but I'm afraid
		 * that wouldn't scale.
		 */
		is_SRI = IsA(stmt->planTree, Result) && stmt->planTree->lefttree == NULL;
	}

	if (!is_SRI)
		clear_relsize_cache();

	if (queryDesc->operation == CMD_INSERT ||
		queryDesc->operation == CMD_SELECT ||
		queryDesc->operation == CMD_UPDATE ||
		queryDesc->operation == CMD_DELETE)
	{

		MemoryContext oldContext;
		
		oldContext = CurrentMemoryContext;
		if ( stmt->qdContext ) /* Temporary! See comment in PlannedStmt. */
		{
			oldContext = MemoryContextSwitchTo(stmt->qdContext);
		}
		else /* MPP-8382: memory context of plan tree should not change */
		{
			MemoryContext mc = GetMemoryChunkContext(stmt->planTree);
			oldContext = MemoryContextSwitchTo(mc);
		}

		stmt->planTree = (Plan *) exec_make_plan_constant(stmt, is_SRI);
		
		MemoryContextSwitchTo(oldContext);
	}

	/*
	 * Cursor queries and bind/execute path queries don't run on the
	 * writer-gang QEs; but they require snapshot-synchronization to
	 * get started.
	 *
	 * initPlans, and other work (see the function pre-evaluation
	 * above) may advance the snapshot "segmateSync" value, so we're
	 * best off setting the shared-snapshot-ready value here. This
	 * will dispatch to the writer gang and force it to set its
	 * snapshot; we'll then be able to serialize the same snapshot
	 * version (see qdSerializeDtxContextInfo() below).
	 *
	 * For details see MPP-6533/MPP-5805. There are a large number of
	 * interesting test cases for segmate-sync.
	 */
	if (queryDesc->extended_query)
	{
		verify_shared_snapshot_ready();
	}

	/*
	 * serialized plan tree. Note that we're called for a single
	 * slice tree (corresponding to an initPlan or the main plan), so the
	 * parameters are fixed and we can include them in the prefix.
	 */
	splan = serializeNode((Node *) queryDesc->plannedstmt, &splan_len);

	Assert(splan != NULL && splan_len > 0);

	/*
	*/
	if (queryDesc->params != NULL && queryDesc->params->numParams > 0)
	{		
        ParamListInfoData  *pli;
        ParamExternData    *pxd;
        StringInfoData      parambuf;
		Size                length;
        int                 plioff;
		int32               iparam;

        /* Allocate buffer for params */
        initStringInfo(&parambuf);

        /* Copy ParamListInfoData header and ParamExternData array */
        pli = queryDesc->params;
        length = (char *)&pli->params[pli->numParams] - (char *)pli;
        plioff = parambuf.len;
        Assert(plioff == MAXALIGN(plioff));
        appendBinaryStringInfo(&parambuf, pli, length);

        /* Copy pass-by-reference param values. */
        for (iparam = 0; iparam < queryDesc->params->numParams; iparam++)
		{
			int16   typlen;
			bool    typbyval;

            /* Recompute pli each time in case parambuf.data is repalloc'ed */
            pli = (ParamListInfoData *)(parambuf.data + plioff);
			pxd = &pli->params[iparam];

            /* Does pxd->value contain the value itself, or a pointer? */
			get_typlenbyval(pxd->ptype, &typlen, &typbyval);
            if (!typbyval)
            {
				char   *s = DatumGetPointer(pxd->value);

				if (pxd->isnull ||
                    !PointerIsValid(s))
                {
                    pxd->isnull = true;
                    pxd->value = 0;
                }
				else
				{
			        length = datumGetSize(pxd->value, typbyval, typlen);

					/* MPP-1637: we *must* set this before we
					 * append. Appending may realloc, which will
					 * invalidate our pxd ptr. (obviously we could
					 * append first if we recalculate pxd from the new
					 * base address) */
                    pxd->value = Int32GetDatum(length);

                    appendBinaryStringInfo(&parambuf, &iparam, sizeof(iparam));
                    appendBinaryStringInfo(&parambuf, s, length);
				}
            }
		}
        sparams = parambuf.data;
        sparams_len = parambuf.len;
	}
	else
	{
		sparams = NULL;
		sparams_len = 0;
	}

	ssliceinfo = serializeNode((Node *) sliceTbl, &ssliceinfo_len);
	
	MemSet(&queryParms, 0, sizeof(queryParms));
	queryParms.strCommand = queryDesc->sourceText;
	queryParms.serializedQuerytree = NULL;
	queryParms.serializedQuerytreelen = 0;
	queryParms.serializedPlantree = splan;
	queryParms.serializedPlantreelen = splan_len;
	queryParms.serializedParams = sparams;
	queryParms.serializedParamslen = sparams_len;
	queryParms.serializedSliceInfo = ssliceinfo;
	queryParms.serializedSliceInfolen= ssliceinfo_len;
	queryParms.rootIdx = rootIdx;

	/* sequence server info */
	qdinfo = &(getComponentDatabases()->entry_db_info[0]);
	Assert(qdinfo != NULL && qdinfo->hostip != NULL);
	queryParms.seqServerHost = pstrdup(qdinfo->hostip);
	queryParms.seqServerHostlen = strlen(qdinfo->hostip) + 1;
	queryParms.seqServerPort = seqServerCtl->seqServerPort;

	queryParms.primary_gang_id = 0;	/* We are relying on the slice table to provide gang ids */

	/* serialized a version of our snapshot */
	/* 
	 * Generate our transction isolations.  We generally want Plan
	 * based dispatch to be in a global transaction. The executor gets
	 * to decide if the special circumstances exist which allow us to
	 * dispatch without starting a global xact.
	 */
	/*queryParms.serializedDtxContextInfo =
		qdSerializeDtxContextInfo(&queryParms.serializedDtxContextInfolen, true  wantSnapshot , queryDesc->extended_query,
								  generateTxnOptions(planRequiresTxn), "cdbdisp_dispatchPlan");*/

	/*
	 * in hawq, there is no distributed transaction
	 */
	queryParms.serializedDtxContextInfo = NULL;
	queryParms.serializedDtxContextInfolen = 0;

	Assert(sliceTbl);
	Assert(sliceTbl->slices != NIL);

	cdbdisp_dispatchX(&queryParms, cancelOnError, sliceTbl, ds);

	sliceTbl->localSlice = oldLocalSlice;
}	/* cdbdisp_dispatchPlan */


/*
 * Three Helper functions for CdbDispatchPlan:
 *
 * Used to figure out the dispatch order for the sliceTable by
 * counting the number of dependent child slices for each slice; and
 * then sorting based on the count (all indepenedent slices get
 * dispatched first, then the slice above them and so on).
 *
 * fillSliceVector: figure out the number of slices we're dispatching,
 * and order them.
 *
 * count_dependent_children(): walk tree counting up children.
 *
 * compare_slice_order(): comparison function for qsort(): order the
 * slices by the number of dependent children. Empty slices are 
 * sorted last (to make this work with initPlans).
 * 
 */
static int
compare_slice_order(const void *aa, const void *bb)
{
	sliceVec *a = (sliceVec *)aa;
	sliceVec *b = (sliceVec *)bb;

	if (a->slice == NULL)
		return 1;
	if (b->slice == NULL)
		return -1;

	/* sort the writer gang slice first, because he sets the shared snapshot */
	if (a->slice->primary_gang_id == 1 && b->slice->primary_gang_id != 1)
		return -1;
	else if (b->slice->primary_gang_id == 1 && a->slice->primary_gang_id != 1)
		return 1;
	
	if (a->children == b->children)
		return 0;
	else if (a->children > b->children)
		return 1;
	else
		return -1;
}

/* Quick and dirty bit mask operations */
static void
mark_bit(char *bits, int nth)
{
	int nthbyte = nth >> 3;
	char nthbit  = 1 << (nth & 7);
	bits[nthbyte] |= nthbit;
}
static void
or_bits(char* dest, char* src, int n)
{
	int i;

	for(i=0; i<n; i++)
		dest[i] |= src[i];
}

static int
count_bits(char* bits, int nbyte)
{
	int i;
	int nbit = 0;
	int bitcount[] = {
		0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4
	};

	for(i=0; i<nbyte; i++)
	{
		nbit += bitcount[bits[i] & 0x0F];
		nbit += bitcount[(bits[i] >> 4) & 0x0F];
	}

	return nbit;
}

/* We use a bitmask to count the dep. childrens.   
 * Because of input sharing, the slices now are DAG.  We cannot simply go down the 
 * tree and add up number of children, which will return too big number.
 */
static int markbit_dep_children(SliceTable *sliceTable, int sliceIdx, sliceVec *sliceVec, int bitmasklen, char* bits)
{
	ListCell *sublist;
	Slice *slice = (Slice *) list_nth(sliceTable->slices, sliceIdx);

	foreach(sublist, slice->children)
	{
		int childIndex = lfirst_int(sublist);
		char *newbits = palloc0(bitmasklen);

		markbit_dep_children(sliceTable, childIndex, sliceVec, bitmasklen, newbits);
		or_bits(bits, newbits, bitmasklen);
		mark_bit(bits, childIndex);
		pfree(newbits);
	}

	sliceVec[sliceIdx].sliceIndex = sliceIdx;
	sliceVec[sliceIdx].children = count_bits(bits, bitmasklen);
	sliceVec[sliceIdx].slice = slice;

	return sliceVec[sliceIdx].children;
}

/* Count how many dependent childrens and fill in the sliceVector of dependent childrens. */
static int
count_dependent_children(SliceTable * sliceTable, int sliceIndex, sliceVec *sliceVector, int len)
{
	int 		ret = 0;
	int			bitmasklen = (len+7) >> 3;
	char 	   *bitmask = palloc0(bitmasklen);

	ret = markbit_dep_children(sliceTable, sliceIndex, sliceVector, bitmasklen, bitmask);
	pfree(bitmask);

	return ret;
}

int
fillSliceVector(SliceTable *sliceTbl, int rootIdx, sliceVec *sliceVector, int sliceLim)
{
	int top_count;

	/* count doesn't include top slice add 1 */
	top_count = 1 + count_dependent_children(sliceTbl, rootIdx, sliceVector, sliceLim);

	qsort(sliceVector, sliceLim, sizeof(sliceVec), compare_slice_order);

	return top_count;
}

/*
 * Dispatch a command - already parsed and in the form of a Node tree
 * - to all primary segdbs.  Does not wait for completion. Does not
 * start a global transaction.
 *
 * NB: Callers should use PG_TRY()/PG_CATCH() if needed to make
 * certain that the CdbDispatchResults objects are destroyed by
 * cdbdisp_destroyDispatchResults() in case of error.
 * To wait for completion, check for errors, and clean up, it is
 * suggested that the caller use cdbdisp_finishCommand().
 */
void
cdbdisp_dispatchUtilityStatement(struct Node *stmt,
								 QueryContextInfo *contextdisp,
								 bool cancelOnError,
								 bool needTwoPhase,
								 bool withSnapshot,
								 struct CdbDispatcherState *ds,
								 char *debugCaller)
{
	char	   *serializedQuerytree;
	int			serializedQuerytree_len;
	Query	   *q = makeNode(Query);
	StringInfoData buffer;

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"cdbdisp_dispatchUtilityStatement debug_query_string = %s (needTwoPhase = %s, debugCaller = %s)",
	     debug_query_string, (needTwoPhase ? "true" : "false"), debugCaller);

	dtmPreCommand("cdbdisp_dispatchUtilityStatement", "(none)", NULL, needTwoPhase,
			withSnapshot, false /* inCursor */ );
	
	initStringInfo(&buffer);

	q->commandType = CMD_UTILITY;

	Assert(stmt != NULL);
	Assert(stmt->type < 1000);
	Assert(stmt->type > 0);

	q->utilityStmt = stmt;
	q->contextdisp = contextdisp;

	q->querySource = QSRC_ORIGINAL;

	/*
	 * We must set q->canSetTag = true.  False would be used to hide a command
	 * introduced by rule expansion which is not allowed to return its
	 * completion status in the command tag (PQcmdStatus/PQcmdTuples). For
	 * example, if the original unexpanded command was SELECT, the status
	 * should come back as "SELECT n" and should not reflect other commands
	 * inserted by rewrite rules.  True means we want the status.
	 */
	q->canSetTag = true;		/* ? */

	/*
	 * serialized the stmt tree, and create the sql statement: mppexec ....
	 */
	serializedQuerytree = serializeNode((Node *) q, &serializedQuerytree_len);

	Assert(serializedQuerytree != NULL);

	cdbdisp_dispatchCommand(debug_query_string, serializedQuerytree, serializedQuerytree_len, cancelOnError, needTwoPhase,
							withSnapshot, ds);

}	/* cdbdisp_dispatchUtilityStatement */


/*
 * Dispatch a command - already parsed and in the form of a Node tree
 * - to all primary segdbs, and wait for completion.  Starts a global
 * transaction first, if not already started.  If not all QEs in the
 * given gang(s) executed the command successfully, throws an error
 * and does not return.
 */
void
CdbDispatchUtilityStatement(struct Node *stmt, char *debugCaller __attribute__((unused)) )
{
	CdbDispatchUtilityStatement_Internal(stmt,
										 NULL,
										 true,
										 FALSE,
										 "CdbDispatchUtilityStatement");
}

void
CdbDispatchUtilityStatement_NoTwoPhase(struct Node *stmt, char *debugCaller __attribute__((unused)) )
{
	CdbDispatchUtilityStatement_Internal(stmt,
										 NULL,
										 false,
										 FALSE,
										 "CdbDispatchUtilityStatement_NoTwoPhase");
}

/*
 * This interface is used to send QueryContextInfo along with the utility
 * statement.  It is dispatched and reconstructed in segments, and the
 * update results will also be reflected into the master catalogs.
 */
void
CdbDispatchUtilityStatementContext(struct Node *stmt, QueryContextInfo *contextdisp, bool checkSendback)
{
	CdbDispatchUtilityStatement_Internal(stmt,
										 contextdisp,
										 true,
										 checkSendback,
										 "CdbDispatchUtilityStatementContext");
}

static void
CdbDispatchUtilityStatement_Internal(struct Node *stmt, QueryContextInfo *contextdisp, bool needTwoPhase, bool checkSendback, char *debugCaller)
{
	volatile struct CdbDispatcherState ds = {NULL, NULL};
	
	elog((Debug_print_full_dtm ? LOG : DEBUG5),
		 "cdbdisp_dispatchUtilityStatement called (needTwoPhase = %s, debugCaller = %s)",
		 (needTwoPhase ? "true" : "false"), debugCaller);

	PG_TRY();
	{
		cdbdisp_dispatchUtilityStatement(stmt,
										 contextdisp,
										 true /* cancelOnError */,
										 needTwoPhase, 
										 true /* withSnapshot */,
										 (struct CdbDispatcherState *)&ds,
										 debugCaller);

		if (checkSendback && Gp_role == GP_ROLE_DISPATCH)
		{
			CdbCheckDispatchResult((struct CdbDispatcherState *)&ds, false);
			if (ds.primaryResults)
				cdbdisp_handleModifiedCatalogOnSegments(ds.primaryResults,
						UpdateCatalogModifiedOnSegments);
		}

		/* Wait for all QEs to finish.	Throw up if error. */
		cdbdisp_finishCommand((struct CdbDispatcherState *) &ds,
							   NULL, NULL);
	}
	PG_CATCH();
	{
		/* Something happend, clean up after ourselves */
		CdbCheckDispatchResult((struct CdbDispatcherState *)&ds, true);

		cdbdisp_destroyDispatchResults(ds.primaryResults);
		cdbdisp_destroyDispatchThreads(ds.dispatchThreads);

		PG_RE_THROW();
		/* not reached */
	}
	PG_END_TRY();

}	/* CdbDispatchUtilityStatement */

char *
qdSerializeDtxContextInfo(int *size, bool wantSnapshot, bool inCursor, int txnOptions, char *debugCaller)
{
	char *serializedDtxContextInfo;

	Snapshot snapshot;
	int serializedLen;
	DtxContextInfo *pDtxContextInfo = NULL;
	
	/* If we already have a LatestSnapshot set then no reason to try
	 * and get a new one.  just use that one.  But... there is one important
	 * reason why this HAS to be here.  ROLLBACK stmts get dispatched to QEs
	 * in the abort transaction code.  This code tears down enough stuff such
	 * that you can't call GetTransactionSnapshot() within that code. So we
	 * need to use the LatestSnapshot since we can't re-gen a new one.
	 *
	 * It is also very possible that for a single user statement which may
	 * only generate a single snapshot that we will dispatch multiple statements
	 * to our qExecs.  Something like:
	 *
	 *							QD				QEs
	 *							|  				|
	 * User SQL Statement ----->|	  BEGIN		|
	 *  						|-------------->|
	 *						    | 	  STMT		|
	 *							|-------------->|
	 *						 	|    PREPARE	|
	 *							|-------------->|
	 *							|    COMMIT		|
	 *							|-------------->|
	 *							|				|
	 *
	 * This may seem like a problem because all four of those will dispatch
	 * the same snapshot with the same curcid.  But... this is OK because
	 * BEGIN, PREPARE, and COMMIT don't need Snapshots on the QEs.
	 *
	 * NOTE: This will be a problem if we ever need to dispatch more than one
	 *  	 statement to the qExecs and more than one needs a snapshot!
	 */
	*size = 0;
	snapshot = NULL;

	if (wantSnapshot)
	{
	
		if (LatestSnapshot == NULL &&
			SerializableSnapshot == NULL &&
			!IsAbortInProgress() )  		
		{
			/* unfortunately, the dtm issues a select for prepared xacts at the
			 * beginning and this is before a snapshot has been set up.  so we need
			 * one for that but not for when we dont have a valid XID.
			 *
			 * but we CANT do this if an ABORT is in progress... instead we'll send
			 * a NONE since the qExecs dont need the information to do a ROLLBACK.
			 *
			 */
			elog((Debug_print_full_dtm ? LOG : DEBUG5),"qdSerializeDtxContextInfo calling GetTransactionSnapshot to make snapshot");
			
			GetTransactionSnapshot();
		}
		
		if (LatestSnapshot != NULL)
		{
			elog((Debug_print_full_dtm ? LOG : DEBUG5),"qdSerializeDtxContextInfo using LatestSnapshot");
			
			snapshot = LatestSnapshot;
			elog((Debug_print_snapshot_dtm ? LOG : DEBUG5),"[Distributed Snapshot #%u] *QD Use Latest* currcid = %d (gxid = %u, '%s')", 
				 LatestSnapshot->distribSnapshotWithLocalMapping.header.distribSnapshotId,
				 LatestSnapshot->curcid,
				 getDistributedTransactionId(),
				 DtxContextToString(DistributedTransactionContext));
		}
		else if (SerializableSnapshot != NULL)
		{
			elog((Debug_print_full_dtm ? LOG : DEBUG5),"qdSerializeDtxContextInfo using SerializableSnapshot");
			
			snapshot = SerializableSnapshot;
			elog((Debug_print_snapshot_dtm ? LOG : DEBUG5),"[Distributed Snapshot #%u] *QD Use Serializable* currcid = %d (gxid = %u, '%s')", 
				 SerializableSnapshot->distribSnapshotWithLocalMapping.header.distribSnapshotId,
				 SerializableSnapshot->curcid,
				 getDistributedTransactionId(),
				 DtxContextToString(DistributedTransactionContext));

		}
	}

	
	switch (DistributedTransactionContext)
	{
		case DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE:
		case DTX_CONTEXT_LOCAL_ONLY:
			if (snapshot != NULL)
			{
				DtxContextInfo_CreateOnMaster(&TempQDDtxContextInfo,
											  &snapshot->distribSnapshotWithLocalMapping,
											  snapshot->curcid, txnOptions);
			}
			else
			{
				DtxContextInfo_CreateOnMaster(&TempQDDtxContextInfo, 
											  NULL, 0, txnOptions);
			}
			
			TempQDDtxContextInfo.cursorContext = inCursor;

			if (DistributedTransactionContext == DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE &&
				snapshot != NULL)
			{
				updateSharedLocalSnapshot(&TempQDDtxContextInfo, snapshot, "qdSerializeDtxContextInfo");
			}

			pDtxContextInfo = &TempQDDtxContextInfo;
			break;

		case DTX_CONTEXT_QD_RETRY_PHASE_2:
		case DTX_CONTEXT_QE_ENTRY_DB_SINGLETON:
		case DTX_CONTEXT_QE_AUTO_COMMIT_IMPLICIT:
		case DTX_CONTEXT_QE_TWO_PHASE_EXPLICIT_WRITER:
		case DTX_CONTEXT_QE_TWO_PHASE_IMPLICIT_WRITER:
		case DTX_CONTEXT_QE_READER:
		case DTX_CONTEXT_QE_PREPARED:
		case DTX_CONTEXT_QE_FINISH_PREPARED:
			elog(FATAL, "Unexpected distribute transaction context: '%s'",
				 DtxContextToString(DistributedTransactionContext));

		default:
			elog(FATAL, "Unrecognized DTX transaction context: %d",
				 (int)DistributedTransactionContext);
	}
	
	serializedLen = DtxContextInfo_SerializeSize(pDtxContextInfo);
	Assert (serializedLen > 0);
	
	*size = serializedLen;
	serializedDtxContextInfo = palloc(*size);

	DtxContextInfo_Serialize(serializedDtxContextInfo, pDtxContextInfo); 

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"qdSerializeDtxContextInfo (called by %s) returning a snapshot of %d bytes (ptr is %s)",
	     debugCaller, *size, (serializedDtxContextInfo != NULL ? "Non-NULL" : "NULL"));
	return serializedDtxContextInfo;
}

/* generateTxnOptions:
 * Generates an int containing the appropriate flags to direct the remote
 * segdb QE process to perform any needed transaction commands before or
 * after the statement.
 *
 * needTwoPhase - specifies whether this statement even wants a transaction to
 *				 be started.  Certain utility statements dont want to be in a
 *				 distributed transaction.
 */
/*static int
generateTxnOptions(bool needTwoPhase)
{
	int options;
	
	options = mppTxnOptions(needTwoPhase);
		
	return options;
	
}*/

/*
 * cdbdisp_makeDispatchThreads:
 * Allocates memory for a CdbDispatchCmdThreads struct that holds
 * the thread count and array of dispatch command parameters (which
 * is being allocated here as well).
 */
CdbDispatchCmdThreads *
cdbdisp_makeDispatchThreads(int paramCount)
{
	CdbDispatchCmdThreads *dThreads = palloc0(sizeof(*dThreads));

	dThreads->dispatchCommandParmsAr =
		(DispatchCommandParms *)palloc0(paramCount * sizeof(DispatchCommandParms));

	dThreads->dispatchCommandParmsArSize = paramCount;

    dThreads->threadCount = 0;

    return dThreads;
}                               /* cdbdisp_makeDispatchThreads */

/*
 * cdbdisp_destroyDispatchThreads:
 * Frees all memory allocated in CdbDispatchCmdThreads struct.
 */
void
cdbdisp_destroyDispatchThreads(CdbDispatchCmdThreads *dThreads)
{

	DispatchCommandParms *pParms;
	int i;

	if (!dThreads)
        return;

	/*
	 * pfree the memory allocated for the dispatchCommandParmsAr
	 */
	elog(DEBUG3, "destroydispatchthreads: threadcount %d array size %d", dThreads->threadCount, dThreads->dispatchCommandParmsArSize);
	for (i = 0; i < dThreads->dispatchCommandParmsArSize; i++)
	{
		pParms = &(dThreads->dispatchCommandParmsAr[i]);
		if (pParms->dispatchResultPtrArray)
		{
			pfree(pParms->dispatchResultPtrArray);
			pParms->dispatchResultPtrArray = NULL;
		}
		if (pParms->query_text)
		{
			/* NOTE: query_text gets malloc()ed by the pqlib code, use
			 * free() not pfree() */
			free(pParms->query_text);
			pParms->query_text = NULL;
		}
		
		switch (pParms->mppDispatchCommandType)
		{
			case GP_DISPATCH_COMMAND_TYPE_QUERY:
			{
				DispatchCommandQueryParms *pQueryParms = &pParms->queryParms;

				if (pQueryParms->strCommand)
				{
					/* Caller frees if desired */
					pQueryParms->strCommand = NULL;
				}
					
				if (pQueryParms->serializedDtxContextInfo)
				{
					if (i==0)
						pfree(pQueryParms->serializedDtxContextInfo);
					pQueryParms->serializedDtxContextInfo = NULL;
				}
					
				if (pQueryParms->serializedSliceInfo)
				{
					if (i==0)
						pfree(pQueryParms->serializedSliceInfo);
					pQueryParms->serializedSliceInfo = NULL;
				}
					
				if (pQueryParms->serializedQuerytree)
				{
					if (i==0)
						pfree(pQueryParms->serializedQuerytree);
					pQueryParms->serializedQuerytree = NULL;
				}
				
				if (pQueryParms->serializedPlantree)
				{
					if (i==0)
						pfree(pQueryParms->serializedPlantree);
					pQueryParms->serializedPlantree = NULL;
				}
					
				if (pQueryParms->serializedParams)
				{
					if (i==0)
						pfree(pQueryParms->serializedParams);
					pQueryParms->serializedParams = NULL;
				}
			}
			break;
		
			case GP_DISPATCH_COMMAND_TYPE_DTX_PROTOCOL:
			{
				DispatchCommandDtxProtocolParms *pDtxProtocolParms = &pParms->dtxProtocolParms;

				pDtxProtocolParms->dtxProtocolCommand = 0;
			}
			break;

			default:
				elog(FATAL, "Unrecognized MPP dispatch command type: %d",
					 (int) pParms->mppDispatchCommandType);
		}
	}
	
	pfree(dThreads->dispatchCommandParmsAr);	
	dThreads->dispatchCommandParmsAr = NULL;

    dThreads->dispatchCommandParmsArSize = 0;
    dThreads->threadCount = 0;
		
	pfree(dThreads);
}                               /* cdbdisp_destroyDispatchThreads */

