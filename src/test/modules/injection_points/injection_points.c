/*--------------------------------------------------------------------------
 *
 * injection_points.c
 *		Code for testing injection points.
 *
 * Injection points are able to trigger user-defined callbacks in pre-defined
 * code paths.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/injection_points/injection_points.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/condition_variable.h"
#include "storage/dsm_registry.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/injection_point.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC;

/* Maximum number of waits usable in injection points at once */
#define INJ_MAX_WAIT	8
#define INJ_NAME_MAXLEN	64
#define	INJ_MAX_CONDITION	4

/*
 * Conditions related to injection points.  This tracks in shared memory the
 * runtime conditions under which an injection point is allowed to run.
 *
 * If more types of runtime conditions need to be tracked, this structure
 * should be expanded.
 */
typedef struct InjectionPointCondition
{
	/* Name of the injection point related to this condition */
	char		name[INJ_NAME_MAXLEN];

	/* ID of the process where the injection point is allowed to run */
	int			pid;
} InjectionPointCondition;

/* Shared state information for injection points. */
typedef struct InjectionPointSharedState
{
	/* Protects access to other fields */
	slock_t		lock;

	/* Counters advancing when injection_points_wakeup() is called */
	uint32		wait_counts[INJ_MAX_WAIT];

	/* Names of injection points attached to wait counters */
	char		name[INJ_MAX_WAIT][INJ_NAME_MAXLEN];

	/* Condition variable used for waits and wakeups */
	ConditionVariable wait_point;

	/* Conditions to run an injection point */
	InjectionPointCondition conditions[INJ_MAX_CONDITION];
} InjectionPointSharedState;

/* Pointer to shared-memory state. */
static InjectionPointSharedState *inj_state = NULL;

extern PGDLLEXPORT void injection_error(const char *name);
extern PGDLLEXPORT void injection_notice(const char *name);
extern PGDLLEXPORT void injection_wait(const char *name);

/* track if injection points attached in this process are linked to it */
static bool injection_point_local = false;

/*
 * Callback for shared memory area initialization.
 */
static void
injection_point_init_state(void *ptr)
{
	InjectionPointSharedState *state = (InjectionPointSharedState *) ptr;

	SpinLockInit(&state->lock);
	memset(state->wait_counts, 0, sizeof(state->wait_counts));
	memset(state->name, 0, sizeof(state->name));
	memset(state->conditions, 0, sizeof(state->conditions));
	ConditionVariableInit(&state->wait_point);
}

/*
 * Initialize shared memory area for this module.
 */
static void
injection_init_shmem(void)
{
	bool		found;

	if (inj_state != NULL)
		return;

	inj_state = GetNamedDSMSegment("injection_points",
								   sizeof(InjectionPointSharedState),
								   injection_point_init_state,
								   &found);
}

/*
 * Check runtime conditions associated to an injection point.
 *
 * Returns true if the named injection point is allowed to run, and false
 * otherwise.  Multiple conditions can be associated to a single injection
 * point, so check them all.
 */
static bool
injection_point_allowed(const char *name)
{
	bool		result = true;

	if (inj_state == NULL)
		injection_init_shmem();

	SpinLockAcquire(&inj_state->lock);

	for (int i = 0; i < INJ_MAX_CONDITION; i++)
	{
		InjectionPointCondition *condition = &inj_state->conditions[i];

		if (strcmp(condition->name, name) == 0)
		{
			/*
			 * Check if this injection point is allowed to run in this
			 * process.
			 */
			if (MyProcPid != condition->pid)
			{
				result = false;
				break;
			}
		}
	}

	SpinLockRelease(&inj_state->lock);

	return result;
}

/*
 * before_shmem_exit callback to remove injection points linked to a
 * specific process.
 */
static void
injection_points_cleanup(int code, Datum arg)
{
	char		names[INJ_MAX_CONDITION][INJ_NAME_MAXLEN] = {0};
	int			count = 0;

	/* Leave if nothing is tracked locally */
	if (!injection_point_local)
		return;

	/*
	 * This is done in three steps: detect the points to detach, detach them
	 * and release their conditions.
	 */
	SpinLockAcquire(&inj_state->lock);
	for (int i = 0; i < INJ_MAX_CONDITION; i++)
	{
		InjectionPointCondition *condition = &inj_state->conditions[i];

		if (condition->name[0] == '\0')
			continue;

		if (condition->pid != MyProcPid)
			continue;

		/* Extract the point name to detach */
		strlcpy(names[count], condition->name, INJ_NAME_MAXLEN);
		count++;
	}
	SpinLockRelease(&inj_state->lock);

	/* Detach, without holding the spinlock */
	for (int i = 0; i < count; i++)
		InjectionPointDetach(names[i]);

	/* Clear all the conditions */
	SpinLockAcquire(&inj_state->lock);
	for (int i = 0; i < INJ_MAX_CONDITION; i++)
	{
		InjectionPointCondition *condition = &inj_state->conditions[i];

		if (condition->name[0] == '\0')
			continue;

		if (condition->pid != MyProcPid)
			continue;

		condition->name[0] = '\0';
		condition->pid = 0;
	}
	SpinLockRelease(&inj_state->lock);
}

/* Set of callbacks available to be attached to an injection point. */
void
injection_error(const char *name)
{
	if (!injection_point_allowed(name))
		return;

	elog(ERROR, "error triggered for injection point %s", name);
}

void
injection_notice(const char *name)
{
	if (!injection_point_allowed(name))
		return;

	elog(NOTICE, "notice triggered for injection point %s", name);
}

/* Wait on a condition variable, awaken by injection_points_wakeup() */
void
injection_wait(const char *name)
{
	uint32		old_wait_counts = 0;
	int			index = -1;
	uint32		injection_wait_event = 0;

	if (inj_state == NULL)
		injection_init_shmem();

	if (!injection_point_allowed(name))
		return;

	/*
	 * Use the injection point name for this custom wait event.  Note that
	 * this custom wait event name is not released, but we don't care much for
	 * testing as this should be short-lived.
	 */
	injection_wait_event = WaitEventExtensionNew(name);

	/*
	 * Find a free slot to wait for, and register this injection point's name.
	 */
	SpinLockAcquire(&inj_state->lock);
	for (int i = 0; i < INJ_MAX_WAIT; i++)
	{
		if (inj_state->name[i][0] == '\0')
		{
			index = i;
			strlcpy(inj_state->name[i], name, INJ_NAME_MAXLEN);
			old_wait_counts = inj_state->wait_counts[i];
			break;
		}
	}
	SpinLockRelease(&inj_state->lock);

	if (index < 0)
		elog(ERROR, "could not find free slot for wait of injection point %s ",
			 name);

	/* And sleep.. */
	ConditionVariablePrepareToSleep(&inj_state->wait_point);
	for (;;)
	{
		uint32		new_wait_counts;

		SpinLockAcquire(&inj_state->lock);
		new_wait_counts = inj_state->wait_counts[index];
		SpinLockRelease(&inj_state->lock);

		if (old_wait_counts != new_wait_counts)
			break;
		ConditionVariableSleep(&inj_state->wait_point, injection_wait_event);
	}
	ConditionVariableCancelSleep();

	/* Remove this injection point from the waiters. */
	SpinLockAcquire(&inj_state->lock);
	inj_state->name[index][0] = '\0';
	SpinLockRelease(&inj_state->lock);
}

/*
 * SQL function for creating an injection point.
 */
PG_FUNCTION_INFO_V1(injection_points_attach);
Datum
injection_points_attach(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *action = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *function;

	if (strcmp(action, "error") == 0)
		function = "injection_error";
	else if (strcmp(action, "notice") == 0)
		function = "injection_notice";
	else if (strcmp(action, "wait") == 0)
		function = "injection_wait";
	else
		elog(ERROR, "incorrect action \"%s\" for injection point creation", action);

	InjectionPointAttach(name, "injection_points", function);

	if (injection_point_local)
	{
		int			index = -1;

		/*
		 * Register runtime condition to link this injection point to the
		 * current process.
		 */
		SpinLockAcquire(&inj_state->lock);
		for (int i = 0; i < INJ_MAX_CONDITION; i++)
		{
			InjectionPointCondition *condition = &inj_state->conditions[i];

			if (condition->name[0] == '\0')
			{
				index = i;
				strlcpy(condition->name, name, INJ_NAME_MAXLEN);
				condition->pid = MyProcPid;
				break;
			}
		}
		SpinLockRelease(&inj_state->lock);

		if (index < 0)
			elog(FATAL,
				 "could not find free slot for condition of injection point %s",
				 name);
	}

	PG_RETURN_VOID();
}

/*
 * SQL function for triggering an injection point.
 */
PG_FUNCTION_INFO_V1(injection_points_run);
Datum
injection_points_run(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	INJECTION_POINT(name);

	PG_RETURN_VOID();
}

/*
 * SQL function for waking up an injection point waiting in injection_wait().
 */
PG_FUNCTION_INFO_V1(injection_points_wakeup);
Datum
injection_points_wakeup(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			index = -1;

	if (inj_state == NULL)
		injection_init_shmem();

	/* First bump the wait counter for the injection point to wake up */
	SpinLockAcquire(&inj_state->lock);
	for (int i = 0; i < INJ_MAX_WAIT; i++)
	{
		if (strcmp(name, inj_state->name[i]) == 0)
		{
			index = i;
			break;
		}
	}
	if (index < 0)
	{
		SpinLockRelease(&inj_state->lock);
		elog(ERROR, "could not find injection point %s to wake up", name);
	}
	inj_state->wait_counts[index]++;
	SpinLockRelease(&inj_state->lock);

	/* And broadcast the change to the waiters */
	ConditionVariableBroadcast(&inj_state->wait_point);
	PG_RETURN_VOID();
}

/*
 * injection_points_set_local
 *
 * Track if any injection point created in this process ought to run only
 * in this process.  Such injection points are detached automatically when
 * this process exits.  This is useful to make test suites concurrent-safe.
 */
PG_FUNCTION_INFO_V1(injection_points_set_local);
Datum
injection_points_set_local(PG_FUNCTION_ARGS)
{
	/* Enable flag to add a runtime condition based on this process ID */
	injection_point_local = true;

	if (inj_state == NULL)
		injection_init_shmem();

	/*
	 * Register a before_shmem_exit callback to remove any injection points
	 * linked to this process.
	 */
	before_shmem_exit(injection_points_cleanup, (Datum) 0);

	PG_RETURN_VOID();
}

/*
 * SQL function for dropping an injection point.
 */
PG_FUNCTION_INFO_V1(injection_points_detach);
Datum
injection_points_detach(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	InjectionPointDetach(name);

	if (inj_state == NULL)
		injection_init_shmem();

	/* Clean up any conditions associated to this injection point */
	SpinLockAcquire(&inj_state->lock);
	for (int i = 0; i < INJ_MAX_CONDITION; i++)
	{
		InjectionPointCondition *condition = &inj_state->conditions[i];

		if (strcmp(condition->name, name) == 0)
		{
			condition->pid = 0;
			condition->name[0] = '\0';
		}
	}
	SpinLockRelease(&inj_state->lock);

	PG_RETURN_VOID();
}
