/*-
 * Copyright (c) 1998 Berkeley Software Design, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Berkeley Software Design Inc's name may not be used to endorse or
 *    promote products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY BERKELEY SOFTWARE DESIGN INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL BERKELEY SOFTWARE DESIGN INC BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	from BSDI $Id: mutex_witness.c,v 1.1.2.20 2000/04/27 03:10:27 cp Exp $
 *	and BSDI $Id: synch_machdep.c,v 2.3.2.39 2000/04/27 03:10:25 cp Exp $
 * $FreeBSD$
 */

/*
 * Implementation of the `witness' lock verifier.  Originally implemented for
 * mutexes in BSD/OS.  Extended to handle generic lock objects and lock
 * classes in FreeBSD.
 */

/*
 *	Main Entry: witness
 *	Pronunciation: 'wit-n&s
 *	Function: noun
 *	Etymology: Middle English witnesse, from Old English witnes knowledge,
 *	    testimony, witness, from 2wit
 *	Date: before 12th century
 *	1 : attestation of a fact or event : TESTIMONY
 *	2 : one that gives evidence; specifically : one who testifies in
 *	    a cause or before a judicial tribunal
 *	3 : one asked to be present at a transaction so as to be able to
 *	    testify to its having taken place
 *	4 : one who has personal knowledge of something
 *	5 a : something serving as evidence or proof : SIGN
 *	  b : public affirmation by word or example of usually
 *	      religious faith or conviction <the heroic witness to divine
 *	      life -- Pilot>
 *	6 capitalized : a member of the Jehovah's Witnesses 
 */

#include "opt_ddb.h"
#include "opt_witness.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <ddb/ddb.h>

#define WITNESS_COUNT 200
#define WITNESS_CHILDCOUNT (WITNESS_COUNT * 4)
/*
 * XXX: This is somewhat bogus, as we assume here that at most 1024 processes
 * will hold LOCK_NCHILDREN * 2 locks.  We handle failure ok, and we should
 * probably be safe for the most part, but it's still a SWAG.
 */
#define LOCK_CHILDCOUNT (MAXCPU + 1024) * 2

#define	WITNESS_NCHILDREN 6

struct witness_child_list_entry;

struct witness {
	const	char *w_name;
	struct	lock_class *w_class;
	STAILQ_ENTRY(witness) w_list;		/* List of all witnesses. */
	STAILQ_ENTRY(witness) w_typelist;	/* Witnesses of a type. */
	struct	witness_child_list_entry *w_children;	/* Great evilness... */
	const	char *w_file;
	int	w_line;
	u_int	w_level;
	u_int	w_refcount;
	u_char	w_Giant_squawked:1;
	u_char	w_other_squawked:1;
	u_char	w_same_squawked:1;
};

struct witness_child_list_entry {
	struct	witness_child_list_entry *wcl_next;
	struct	witness *wcl_children[WITNESS_NCHILDREN];
	u_int	wcl_count;
};

STAILQ_HEAD(witness_list, witness);

struct witness_blessed {
	const	char *b_lock1;
	const	char *b_lock2;
};

struct witness_order_list_entry {
	const	char *w_name;
	struct	lock_class *w_class;
};

static struct	witness *enroll(const char *description,
				struct lock_class *lock_class);
static int	itismychild(struct witness *parent, struct witness *child);
static void	removechild(struct witness *parent, struct witness *child);
static int	isitmychild(struct witness *parent, struct witness *child);
static int	isitmydescendant(struct witness *parent, struct witness *child);
static int	dup_ok(struct witness *);
static int	blessed(struct witness *, struct witness *);
static void	witness_display_list(void(*prnt)(const char *fmt, ...),
				     struct witness_list *list);
static void	witness_displaydescendants(void(*)(const char *fmt, ...),
					   struct witness *);
static void	witness_leveldescendents(struct witness *parent, int level);
static void	witness_levelall(void);
static struct	witness *witness_get(void);
static void	witness_free(struct witness *m);
static struct	witness_child_list_entry *witness_child_get(void);
static void	witness_child_free(struct witness_child_list_entry *wcl);
static struct	lock_list_entry *witness_lock_list_get(void);
static void	witness_lock_list_free(struct lock_list_entry *lle);
static void	witness_display(void(*)(const char *fmt, ...));

MALLOC_DEFINE(M_WITNESS, "witness", "witness structure");

static int witness_watch;
TUNABLE_INT_DECL("debug.witness_watch", 1, witness_watch);
SYSCTL_INT(_debug, OID_AUTO, witness_watch, CTLFLAG_RD, &witness_watch, 0, "");

#ifdef DDB
/*
 * When DDB is enabled and witness_ddb is set to 1, it will cause the system to
 * drop into kdebug() when:
 *	- a lock heirarchy violation occurs
 *	- locks are held when going to sleep.
 */
int	witness_ddb;
#ifdef WITNESS_DDB
TUNABLE_INT_DECL("debug.witness_ddb", 1, witness_ddb);
#else
TUNABLE_INT_DECL("debug.witness_ddb", 0, witness_ddb);
#endif
SYSCTL_INT(_debug, OID_AUTO, witness_ddb, CTLFLAG_RW, &witness_ddb, 0, "");
#endif /* DDB */

int	witness_skipspin;
#ifdef WITNESS_SKIPSPIN
TUNABLE_INT_DECL("debug.witness_skipspin", 1, witness_skipspin);
#else
TUNABLE_INT_DECL("debug.witness_skipspin", 0, witness_skipspin);
#endif
SYSCTL_INT(_debug, OID_AUTO, witness_skipspin, CTLFLAG_RD, &witness_skipspin, 0,
    "");

static struct mtx w_mtx;
static struct witness_list w_free = STAILQ_HEAD_INITIALIZER(w_free);
static struct witness_list w_all = STAILQ_HEAD_INITIALIZER(w_all);
static struct witness_list w_spin = STAILQ_HEAD_INITIALIZER(w_spin);
static struct witness_list w_sleep = STAILQ_HEAD_INITIALIZER(w_sleep);
static struct witness_child_list_entry *w_child_free = NULL;
static struct lock_list_entry *w_lock_list_free = NULL;
static int witness_dead;	/* fatal error, probably no memory */

static struct witness w_data[WITNESS_COUNT];
static struct witness_child_list_entry w_childdata[WITNESS_CHILDCOUNT];
static struct lock_list_entry w_locklistdata[LOCK_CHILDCOUNT];

static struct witness_order_list_entry order_lists[] = {
	{ "Giant", &lock_class_mtx_sleep },
	{ "proctree", &lock_class_sx },
	{ "allproc", &lock_class_sx },
	{ "process lock", &lock_class_mtx_sleep },
	{ "uidinfo hash", &lock_class_mtx_sleep },
	{ "uidinfo struct", &lock_class_mtx_sleep },
	{ NULL, NULL },
#if defined(__i386__) && defined (SMP)
	{ "com", &lock_class_mtx_spin },
#endif
	{ "sio", &lock_class_mtx_spin },
#ifdef __i386__
	{ "cy", &lock_class_mtx_spin },
#endif
	{ "ng_node", &lock_class_mtx_spin },
	{ "ng_worklist", &lock_class_mtx_spin },
	{ "ithread table lock", &lock_class_mtx_spin },
	{ "ithread list lock", &lock_class_mtx_spin },
	{ "sched lock", &lock_class_mtx_spin },
#ifdef __i386__
	{ "clk", &lock_class_mtx_spin },
#endif
	{ "callout", &lock_class_mtx_spin },
	/*
	 * leaf locks
	 */
#ifdef SMP
#ifdef __i386__
	{ "ap boot", &lock_class_mtx_spin },
	{ "imen", &lock_class_mtx_spin },
#endif
	{ "smp rendezvous", &lock_class_mtx_spin },
#endif
	{ NULL, NULL },
	{ NULL, NULL }
};

static const char *dup_list[] = {
	"process lock",
	NULL
};

/*
 * Pairs of locks which have been blessed
 * Don't complain about order problems with blessed locks
 */
static struct witness_blessed blessed_list[] = {
};
static int blessed_count =
	sizeof(blessed_list) / sizeof(struct witness_blessed);

/*
 * List of all locks in the system.
 */
STAILQ_HEAD(, lock_object) all_locks = STAILQ_HEAD_INITIALIZER(all_locks);

static struct mtx all_mtx = {
	{ &lock_class_mtx_sleep,	/* mtx_object.lo_class */
	  "All locks list",		/* mtx_object.lo_name */
	  NULL,				/* mtx_object.lo_file */
	  0,				/* mtx_object.lo_line */
	  LO_INITIALIZED,		/* mtx_object.lo_flags */
	  { NULL },			/* mtx_object.lo_list */
	  NULL },			/* mtx_object.lo_witness */
	MTX_UNOWNED, 0,			/* mtx_lock, mtx_recurse */
	0,				/* mtx_savecrit */
	TAILQ_HEAD_INITIALIZER(all_mtx.mtx_blocked),
	{ NULL, NULL }			/* mtx_contested */
};

/*
 * This global is set to 0 once it becomes safe to use the witness code.
 */
static int witness_cold = 1;

/*
 * Global variables for book keeping.
 */
static int lock_cur_cnt;
static int lock_max_cnt;

/*
 * The WITNESS-enabled diagnostic code.
 */
static void
witness_initialize(void *dummy __unused)
{
	struct lock_object *lock;
	struct witness_order_list_entry *order;
	struct witness *w, *w1;
	int i;

	/*
	 * We have to release Giant before initializing its witness
	 * structure so that WITNESS doesn't get confused.
	 */
	mtx_unlock(&Giant);
	mtx_assert(&Giant, MA_NOTOWNED);

	STAILQ_INSERT_HEAD(&all_locks, &all_mtx.mtx_object, lo_list);
	mtx_init(&w_mtx, "witness lock", MTX_SPIN | MTX_QUIET | MTX_NOWITNESS);
	for (i = 0; i < WITNESS_COUNT; i++)
		witness_free(&w_data[i]);
	for (i = 0; i < WITNESS_CHILDCOUNT; i++)
		witness_child_free(&w_childdata[i]);
	for (i = 0; i < LOCK_CHILDCOUNT; i++)
		witness_lock_list_free(&w_locklistdata[i]);

	/* First add in all the specified order lists. */
	for (order = order_lists; order->w_name != NULL; order++) {
		w = enroll(order->w_name, order->w_class);
		w->w_file = "order list";
		for (order++; order->w_name != NULL; order++) {
			w1 = enroll(order->w_name, order->w_class);
			w1->w_file = "order list";
			itismychild(w, w1);
			w = w1;
		}
	}

	/* Iterate through all locks and add them to witness. */
	mtx_lock(&all_mtx);
	STAILQ_FOREACH(lock, &all_locks, lo_list) {
		if (lock->lo_flags & LO_WITNESS)
			lock->lo_witness = enroll(lock->lo_name,
			    lock->lo_class);
		else
			lock->lo_witness = NULL;
	}
	mtx_unlock(&all_mtx);

	/* Mark the witness code as being ready for use. */
	atomic_store_rel_int(&witness_cold, 0);

	mtx_lock(&Giant);
}
SYSINIT(witness_init, SI_SUB_WITNESS, SI_ORDER_FIRST, witness_initialize, NULL)

void
witness_init(struct lock_object *lock)
{
	struct lock_class *class;

	class = lock->lo_class;
	if (lock->lo_flags & LO_INITIALIZED)
		panic("%s: lock (%s) %s is already initialized!\n", __func__,
		    class->lc_name, lock->lo_name);

	if ((lock->lo_flags & LO_RECURSABLE) != 0 &&
	    (class->lc_flags & LC_RECURSABLE) == 0)
		panic("%s: lock (%s) %s can not be recursable!\n", __func__,
		    class->lc_name, lock->lo_name);
	
	if ((lock->lo_flags & LO_SLEEPABLE) != 0 &&
	    (class->lc_flags & LC_SLEEPABLE) == 0)
		panic("%s: lock (%s) %s can not be sleepable!\n", __func__,
		    class->lc_name, lock->lo_name);
	
	mtx_lock(&all_mtx);
	STAILQ_INSERT_TAIL(&all_locks, lock, lo_list);
	lock->lo_flags |= LO_INITIALIZED;
	lock_cur_cnt++;
	if (lock_cur_cnt > lock_max_cnt)
		lock_max_cnt = lock_cur_cnt;
	mtx_unlock(&all_mtx);
	if (!witness_cold && !witness_dead &&
	    (lock->lo_flags & LO_WITNESS) != 0)
		lock->lo_witness = enroll(lock->lo_name, class);
	else
		lock->lo_witness = NULL;
}

void
witness_destroy(struct lock_object *lock)
{
	struct witness *w;

	if (witness_cold)
		panic("lock (%s) %s destroyed while witness_cold",
		    lock->lo_class->lc_name, lock->lo_name);

	if ((lock->lo_flags & LO_INITIALIZED) == 0)
		panic("%s: lock (%s) %s is not initialized!\n", __func__,
		    lock->lo_class->lc_name, lock->lo_name);

	if (lock->lo_flags & LO_LOCKED)
		panic("lock (%s) %s destroyed while held",
		    lock->lo_class->lc_name, lock->lo_name);

	w = lock->lo_witness;
	if (w != NULL) {
		mtx_lock_spin(&w_mtx);
		w->w_refcount--;
		if (w->w_refcount == 0) {
			w->w_name = "(dead)";
			w->w_file = "(dead)";
			w->w_line = 0;
		}
		mtx_unlock_spin(&w_mtx);
	}

	mtx_lock(&all_mtx);
	lock_cur_cnt--;
	STAILQ_REMOVE(&all_locks, lock, lock_object, lo_list);
	lock->lo_flags &= LO_INITIALIZED;
	mtx_unlock(&all_mtx);
}

static void
witness_display_list(void(*prnt)(const char *fmt, ...),
		     struct witness_list *list)
{
	struct witness *w, *w1;
	int found;

	STAILQ_FOREACH(w, list, w_typelist) {
		if (w->w_file == NULL)
			continue;
		found = 0;
		STAILQ_FOREACH(w1, list, w_typelist) {
			if (isitmychild(w1, w)) {
				found++;
				break;
			}
		}
		if (found)
			continue;
		/*
		 * This lock has no anscestors, display its descendants. 
		 */
		witness_displaydescendants(prnt, w);
	}
}
	
static void
witness_display(void(*prnt)(const char *fmt, ...))
{
	struct witness *w;

	KASSERT(!witness_cold, ("%s: witness_cold\n", __func__));
	witness_levelall();

	/*
	 * First, handle sleep locks which have been acquired at least
	 * once.
	 */
	prnt("Sleep locks:\n");
	witness_display_list(prnt, &w_sleep);
	
	/*
	 * Now do spin locks which have been acquired at least once.
	 */
	prnt("\nSpin locks:\n");
	witness_display_list(prnt, &w_spin);
	
	/*
	 * Finally, any locks which have not been acquired yet.
	 */
	prnt("\nLocks which were never acquired:\n");
	STAILQ_FOREACH(w, &w_all, w_list) {
		if (w->w_file != NULL)
			continue;
		prnt("%s\n", w->w_name);
	}
}

void
witness_lock(struct lock_object *lock, int flags, const char *file, int line)
{
	struct lock_list_entry **lock_list, *lle;
	struct lock_object *lock1, *lock2;
	struct lock_class *class;
	struct witness *w, *w1;
	struct proc *p;
	int i, j;
#ifdef DDB
	int go_into_ddb = 0;
#endif /* DDB */

	if (witness_cold || witness_dead || lock->lo_witness == NULL ||
	    panicstr)
		return;
	w = lock->lo_witness;
	class = lock->lo_class;
	p = curproc;

	if ((lock->lo_flags & LO_LOCKED) == 0)
		panic("%s: lock (%s) %s is not locked @ %s:%d", __func__,
		    class->lc_name, lock->lo_name, file, line);

	if ((lock->lo_flags & LO_RECURSED) != 0) {
		if ((lock->lo_flags & LO_RECURSABLE) == 0)
			panic(
			"%s: recursed on non-recursive lock (%s) %s @ %s:%d",
			    __func__, class->lc_name, lock->lo_name, file,
			    line);
		return;
	}
	
	/*
	 * We have to hold a spinlock to keep lock_list valid across the check
	 * in the LC_SLEEPLOCK case.  In the LC_SPINLOCK case, it is already
	 * protected by the spinlock we are currently performing the witness
	 * checks on, so it is ok to release the lock after performing this
	 * check.  All we have to protect is the LC_SLEEPLOCK case when no
	 * spinlocks are held as we may get preempted during this check and
	 * lock_list could end up pointing to some other CPU's spinlock list.
	 */
	mtx_lock_spin(&w_mtx);
	lock_list = PCPU_PTR(spinlocks);
	if (class->lc_flags & LC_SLEEPLOCK) {
		if (*lock_list != NULL) {
			mtx_unlock_spin(&w_mtx);
			panic("blockable sleep lock (%s) %s @ %s:%d",
			    class->lc_name, lock->lo_name, file, line);
		}
		lock_list = &p->p_sleeplocks;
	}
	mtx_unlock_spin(&w_mtx);

	if (flags & LOP_TRYLOCK)
		goto out;

	/*
	 * Is this the first lock acquired?  If so, then no order checking
	 * is needed.
	 */
	if (*lock_list == NULL)
		goto out;

	/*
	 * Check for duplicate locks of the same type.  Note that we only
	 * have to check for this on the last lock we just acquired.  Any
	 * other cases will be caught as lock order violations.
	 */
	lock1 = (*lock_list)->ll_children[(*lock_list)->ll_count - 1];
	w1 = lock1->lo_witness;
	if (w1 == w) {
		if (w->w_same_squawked || dup_ok(w))
			goto out;
		w->w_same_squawked = 1;
		printf("acquring duplicate lock of same type: \"%s\"\n", 
			lock->lo_name);
		printf(" 1st @ %s:%d\n", w->w_file, w->w_line);
		printf(" 2nd @ %s:%d\n", file, line);
#ifdef DDB
		go_into_ddb = 1;
#endif /* DDB */
		goto out;
	}
	MPASS(!mtx_owned(&w_mtx));
	mtx_lock_spin(&w_mtx);
	/*
	 * If we have a known higher number just say ok
	 */
	if (witness_watch > 1 && w->w_level > w1->w_level) {
		mtx_unlock_spin(&w_mtx);
		goto out;
	}
	if (isitmydescendant(w1, w)) {
		mtx_unlock_spin(&w_mtx);
		goto out;
	}
	for (j = 0, lle = *lock_list; lle != NULL; lle = lle->ll_next) {
		for (i = lle->ll_count - 1; i >= 0; i--, j++) {

			MPASS(j < WITNESS_COUNT);
			lock1 = lle->ll_children[i];
			w1 = lock1->lo_witness;

			/*
			 * If this lock doesn't undergo witness checking,
			 * then skip it.
			 */
			if (w1 == NULL) {
				KASSERT((lock1->lo_flags & LO_WITNESS) == 0,
				    ("lock missing witness structure"));
				continue;
			}
			if (!isitmydescendant(w, w1))
				continue;
			/*
			 * We have a lock order violation, check to see if it
			 * is allowed or has already been yelled about.
			 */
			mtx_unlock_spin(&w_mtx);
			if (blessed(w, w1))
				goto out;
			if (lock1 == &Giant.mtx_object) {
				if (w1->w_Giant_squawked)
					goto out;
				else
					w1->w_Giant_squawked = 1;
			} else {
				if (w1->w_other_squawked)
					goto out;
				else
					w1->w_other_squawked = 1;
			}
			/*
			 * Ok, yell about it.
			 */
			printf("lock order reversal\n");
			/*
			 * Try to locate an earlier lock with
			 * witness w in our list.
			 */
			do {
				lock2 = lle->ll_children[i];
				MPASS(lock2 != NULL);
				if (lock2->lo_witness == w)
					break;
				i--;
				if (i == 0 && lle->ll_next != NULL) {
					lle = lle->ll_next;
					i = lle->ll_count - 1;
					MPASS(i != 0);
				}
			} while (i >= 0);
			if (i < 0)
				/*
				 * We are very likely bogus in this case.
				 */
				printf(" 1st %s last acquired @ %s:%d\n",
				    w->w_name, w->w_file, w->w_line);
			else
				printf(" 1st %p %s @ %s:%d\n", lock2,
				    lock2->lo_name, lock2->lo_file,
				    lock2->lo_line);
			printf(" 2nd %p %s @ %s:%d\n",
			    lock1, lock1->lo_name, lock1->lo_file,
			    lock1->lo_line);
			printf(" 3rd %p %s @ %s:%d\n",
			    lock, lock->lo_name, file, line);
#ifdef DDB
			go_into_ddb = 1;
#endif /* DDB */
			goto out;
		}
	}
	lock1 = (*lock_list)->ll_children[(*lock_list)->ll_count - 1];
	if (!itismychild(lock1->lo_witness, w))
		mtx_unlock_spin(&w_mtx);

out:
#ifdef DDB
	if (witness_ddb && go_into_ddb)
		Debugger("witness_enter");
#endif /* DDB */
	w->w_file = file;
	w->w_line = line;
	lock->lo_line = line;
	lock->lo_file = file;
	
	lle = *lock_list;
	if (lle == NULL || lle->ll_count == LOCK_CHILDCOUNT) {
		*lock_list = witness_lock_list_get();
		if (*lock_list == NULL)
			return;
		(*lock_list)->ll_next = lle;
		lle = *lock_list;
	}
	lle->ll_children[lle->ll_count++] = lock;
}

void
witness_unlock(struct lock_object *lock, int flags, const char *file, int line)
{
	struct lock_list_entry **lock_list, *lle;
	struct lock_class *class;
	struct proc *p;
	int i, j;

	if (witness_cold || witness_dead || lock->lo_witness == NULL ||
	    panicstr)
		return;
	p = curproc;
	class = lock->lo_class;

	if (lock->lo_flags & LO_RECURSED) {
		if ((lock->lo_flags & LO_LOCKED) == 0)
			panic("%s: recursed lock (%s) %s is not locked @ %s:%d",
			    __func__, class->lc_name, lock->lo_name, file,
			    line);
		return;
	}

	/*
	 * We don't need to protect this PCPU_GET() here against preemption
	 * because if we hold any spinlocks then we are already protected,
	 * and if we don't we will get NULL if we hold no spinlocks even if
	 * we switch CPU's while reading it.
	 */
	if (class->lc_flags & LC_SLEEPLOCK) {
		if ((flags & LOP_NOSWITCH) == 0 && PCPU_GET(spinlocks) != NULL)
			panic("switchable sleep unlock (%s) %s @ %s:%d",
			    class->lc_name, lock->lo_name, file, line);
		lock_list = &p->p_sleeplocks;
	} else
		lock_list = PCPU_PTR(spinlocks);

	for (; *lock_list != NULL; lock_list = &(*lock_list)->ll_next)
		for (i = 0; i < (*lock_list)->ll_count; i++)
			if ((*lock_list)->ll_children[i] == lock) {
				(*lock_list)->ll_count--;
				for (j = i; j < (*lock_list)->ll_count; j++)
					(*lock_list)->ll_children[j] =
					    (*lock_list)->ll_children[j + 1];
				if ((*lock_list)->ll_count == 0) {
					lle = *lock_list;
					*lock_list = lle->ll_next;
					witness_lock_list_free(lle);
				}
				return;
			}
}

/*
 * Warn if any held locks are not sleepable.  Note that Giant and the lock
 * passed in are both special cases since they are both released during the
 * sleep process and aren't actually held while the process is asleep.
 */
int
witness_sleep(int check_only, struct lock_object *lock, const char *file,
	      int line)
{
	struct lock_list_entry **lock_list, *lle;
	struct lock_object *lock1;
	struct proc *p;
	critical_t savecrit;
	int i, n;

	if (witness_dead || panicstr)
		return (0);
	KASSERT(!witness_cold, ("%s: witness_cold\n", __func__));
	n = 0;
	/*
	 * Preemption bad because we need PCPU_PTR(spinlocks) to not change.
	 */
	savecrit = critical_enter();	
	p = curproc;
	lock_list = &p->p_sleeplocks;
again:
	for (lle = *lock_list; lle != NULL; lle = lle->ll_next)
		for (i = lle->ll_count - 1; i >= 0; i--) {
			lock1 = lle->ll_children[i];
			if (lock1 == lock || lock1 == &Giant.mtx_object ||
			    (lock1->lo_flags & LO_SLEEPABLE))
				continue;
			n++;
			printf("%s:%d: %s with \"%s\" locked from %s:%d\n",
			    file, line, check_only ? "could sleep" : "sleeping",
			    lock1->lo_name, lock1->lo_file, lock1->lo_line);
		}
	if (lock_list == &p->p_sleeplocks) {
		lock_list = PCPU_PTR(spinlocks);
		goto again;
	}
#ifdef DDB
	if (witness_ddb && n)
		Debugger("witness_sleep");
#endif /* DDB */
	critical_exit(savecrit);
	return (n);
}

static struct witness *
enroll(const char *description, struct lock_class *lock_class)
{
	struct witness *w;

	if (!witness_watch)
		return (NULL);

	if ((lock_class->lc_flags & LC_SPINLOCK) && witness_skipspin)
		return (NULL);
	mtx_lock_spin(&w_mtx);
	STAILQ_FOREACH(w, &w_all, w_list) {
		if (strcmp(description, w->w_name) == 0) {
			w->w_refcount++;
			mtx_unlock_spin(&w_mtx);
			if (lock_class != w->w_class)
				panic(
				"lock (%s) %s does not match earlier (%s) lock",
				    description, lock_class->lc_name,
				    w->w_class->lc_name);
			return (w);
		}
	}
	/*
	 * This isn't quite right, as witness_cold is still 0 while we
	 * enroll all the locks initialized before witness_initialize().
	 */
	if ((lock_class->lc_flags & LC_SPINLOCK) && !witness_cold)
		panic("spin lock %s not in order list", description);
	if ((w = witness_get()) == NULL)
		return (NULL);
	w->w_name = description;
	w->w_class = lock_class;
	w->w_refcount = 1;
	STAILQ_INSERT_HEAD(&w_all, w, w_list);
	if (lock_class->lc_flags & LC_SPINLOCK)
		STAILQ_INSERT_HEAD(&w_spin, w, w_typelist);
	else if (lock_class->lc_flags & LC_SLEEPLOCK)
		STAILQ_INSERT_HEAD(&w_sleep, w, w_typelist);
	else
		panic("lock class %s is not sleep or spin",
		    lock_class->lc_name);
	mtx_unlock_spin(&w_mtx);

	return (w);
}

static int
itismychild(struct witness *parent, struct witness *child)
{
	static int recursed;
	struct witness_child_list_entry **wcl;
	struct witness_list *list;

	MPASS(child != NULL && parent != NULL);
	if ((parent->w_class->lc_flags & (LC_SLEEPLOCK | LC_SPINLOCK)) !=
	    (child->w_class->lc_flags & (LC_SLEEPLOCK | LC_SPINLOCK)))
		panic(
		"%s: parent (%s) and child (%s) are not the same lock type",
		    __func__, parent->w_class->lc_name,
		    child->w_class->lc_name);

	/*
	 * Insert "child" after "parent"
	 */
	wcl = &parent->w_children;
	while (*wcl != NULL && (*wcl)->wcl_count == WITNESS_NCHILDREN)
		wcl = &(*wcl)->wcl_next;

	if (*wcl == NULL) {
		*wcl = witness_child_get();
		if (*wcl == NULL)
			return (1);
	}

	(*wcl)->wcl_children[(*wcl)->wcl_count++] = child;

	/*
	 * Now prune whole tree.  We look for cases where a lock is now
	 * both a descendant and a direct child of a given lock.  In that
	 * case, we want to remove the direct child link from the tree.
	 */
	if (recursed)
		return (0);
	recursed = 1;
	if (parent->w_class->lc_flags & LC_SLEEPLOCK)
		list = &w_sleep;
	else
		list = &w_spin;
	STAILQ_FOREACH(child, list, w_typelist) {
		STAILQ_FOREACH(parent, list, w_typelist) {
			if (!isitmychild(parent, child))
				continue;
			removechild(parent, child);
			if (isitmydescendant(parent, child))
				continue;
			itismychild(parent, child);
		}
	}
	recursed = 0;
	witness_levelall();
	return (0);
}

static void
removechild(struct witness *parent, struct witness *child)
{
	struct witness_child_list_entry **wcl, *wcl1;
	int i;

	for (wcl = &parent->w_children; *wcl != NULL; wcl = &(*wcl)->wcl_next)
		for (i = 0; i < (*wcl)->wcl_count; i++)
			if ((*wcl)->wcl_children[i] == child)
				goto found;
	return;
found:
	(*wcl)->wcl_count--;
	if ((*wcl)->wcl_count > i)
		(*wcl)->wcl_children[i] =
		    (*wcl)->wcl_children[(*wcl)->wcl_count];
	MPASS((*wcl)->wcl_children[i] != NULL);

	if ((*wcl)->wcl_count != 0)
		return;

	wcl1 = *wcl;
	*wcl = wcl1->wcl_next;
	witness_child_free(wcl1);
}

static int
isitmychild(struct witness *parent, struct witness *child)
{
	struct witness_child_list_entry *wcl;
	int i;

	for (wcl = parent->w_children; wcl != NULL; wcl = wcl->wcl_next) {
		for (i = 0; i < wcl->wcl_count; i++) {
			if (wcl->wcl_children[i] == child)
				return (1);
		}
	}
	return (0);
}

static int
isitmydescendant(struct witness *parent, struct witness *child)
{
	struct witness_child_list_entry *wcl;
	int i, j;

	if (isitmychild(parent, child))
		return (1);
	j = 0;
	for (wcl = parent->w_children; wcl != NULL; wcl = wcl->wcl_next) {
		MPASS(j < 1000);
		for (i = 0; i < wcl->wcl_count; i++) {
			if (isitmydescendant(wcl->wcl_children[i], child))
				return (1);
		}
		j++;
	}
	return (0);
}

void
witness_levelall (void)
{
	struct witness_list *list;
	struct witness *w, *w1;

	/*
	 * First clear all levels.
	 */
	STAILQ_FOREACH(w, &w_all, w_list) {
		w->w_level = 0;
	}

	/*
	 * Look for locks with no parent and level all their descendants.
	 */
	STAILQ_FOREACH(w, &w_all, w_list) {
		/*
		 * This is just an optimization, technically we could get
		 * away just walking the all list each time.
		 */
		if (w->w_class->lc_flags & LC_SLEEPLOCK)
			list = &w_sleep;
		else
			list = &w_spin;
		STAILQ_FOREACH(w1, list, w_typelist) {
			if (isitmychild(w1, w))
				goto skip;
		}
		witness_leveldescendents(w, 0);
	skip:
	}
}

static void
witness_leveldescendents(struct witness *parent, int level)
{
	struct witness_child_list_entry *wcl;
	int i;

	if (parent->w_level < level)
		parent->w_level = level;
	level++;
	for (wcl = parent->w_children; wcl != NULL; wcl = wcl->wcl_next)
		for (i = 0; i < wcl->wcl_count; i++)
			witness_leveldescendents(wcl->wcl_children[i], level);
}

static void
witness_displaydescendants(void(*prnt)(const char *fmt, ...),
			   struct witness *parent)
{
	struct witness_child_list_entry *wcl;
	int i, level;

	level =  parent->w_level;

	prnt("%-2d", level);
	for (i = 0; i < level; i++)
		prnt(" ");
	prnt("%s", parent->w_name);
	if (parent->w_file != NULL)
		prnt(" -- last acquired @ %s:%d\n", parent->w_file,
		    parent->w_line);

	for (wcl = parent->w_children; wcl != NULL; wcl = wcl->wcl_next)
		for (i = 0; i < wcl->wcl_count; i++)
			    witness_displaydescendants(prnt,
				wcl->wcl_children[i]);
}

static int
dup_ok(struct witness *w)
{
	const char **dup;
	
	for (dup = dup_list; *dup != NULL; dup++)
		if (strcmp(w->w_name, *dup) == 0)
			return (1);
	return (0);
}

static int
blessed(struct witness *w1, struct witness *w2)
{
	int i;
	struct witness_blessed *b;

	for (i = 0; i < blessed_count; i++) {
		b = &blessed_list[i];
		if (strcmp(w1->w_name, b->b_lock1) == 0) {
			if (strcmp(w2->w_name, b->b_lock2) == 0)
				return (1);
			continue;
		}
		if (strcmp(w1->w_name, b->b_lock2) == 0)
			if (strcmp(w2->w_name, b->b_lock1) == 0)
				return (1);
	}
	return (0);
}

static struct witness *
witness_get(void)
{
	struct witness *w;

	if (STAILQ_EMPTY(&w_free)) {
		witness_dead = 1;
		mtx_unlock_spin(&w_mtx);
		printf("%s: witness exhausted\n", __func__);
		return (NULL);
	}
	w = STAILQ_FIRST(&w_free);
	STAILQ_REMOVE_HEAD(&w_free, w_list);
	bzero(w, sizeof(*w));
	return (w);
}

static void
witness_free(struct witness *w)
{

	STAILQ_INSERT_HEAD(&w_free, w, w_list);
}

static struct witness_child_list_entry *
witness_child_get(void)
{
	struct witness_child_list_entry *wcl;

	wcl = w_child_free;
	if (wcl == NULL) {
		witness_dead = 1;
		mtx_unlock_spin(&w_mtx);
		printf("%s: witness exhausted\n", __func__);
		return (NULL);
	}
	w_child_free = wcl->wcl_next;
	bzero(wcl, sizeof(*wcl));
	return (wcl);
}

static void
witness_child_free(struct witness_child_list_entry *wcl)
{

	wcl->wcl_next = w_child_free;
	w_child_free = wcl;
}

static struct lock_list_entry *
witness_lock_list_get(void)
{
	struct lock_list_entry *lle;

	mtx_lock_spin(&w_mtx);
	lle = w_lock_list_free;
	if (lle == NULL) {
		witness_dead = 1;
		mtx_unlock_spin(&w_mtx);
		printf("%s: witness exhausted\n", __func__);
		return (NULL);
	}
	w_lock_list_free = lle->ll_next;
	mtx_unlock_spin(&w_mtx);
	bzero(lle, sizeof(*lle));
	return (lle);
}
		
static void
witness_lock_list_free(struct lock_list_entry *lle)
{

	mtx_lock_spin(&w_mtx);
	lle->ll_next = w_lock_list_free;
	w_lock_list_free = lle;
	mtx_unlock_spin(&w_mtx);
}

int
witness_list_locks(struct lock_list_entry **lock_list)
{
	struct lock_list_entry *lle;
	struct lock_object *lock;
	int i, nheld;

	nheld = 0;
	for (lle = *lock_list; lle != NULL; lle = lle->ll_next)
		for (i = lle->ll_count - 1; i >= 0; i--) {
			lock = lle->ll_children[i];
			printf("\t(%s) %s (%p) locked at %s:%d\n",
			    lock->lo_class->lc_name, lock->lo_name, lock,
			    lock->lo_file, lock->lo_line);
			nheld++;
		}
	return (nheld);
}

/*
 * Calling this on p != curproc is bad unless we are in ddb.
 */
int
witness_list(struct proc *p)
{
	critical_t savecrit;
	int nheld;

	KASSERT(p == curproc || db_active,
	    ("%s: p != curproc and we aren't in the debugger", __func__));
	KASSERT(!witness_cold, ("%s: witness_cold", __func__));

	nheld = witness_list_locks(&p->p_sleeplocks);

	/*
	 * We only handle spinlocks if p == curproc.  This is somewhat broken
	 * if p is currently executing on some other CPU and holds spin locks
	 * as we won't display those locks.  If we had a MI way of getting
	 * the per-cpu data for a given cpu then we could use p->p_oncpu to
	 * get the list of spinlocks for this process and "fix" this.
	 */
	if (p == curproc) {
		/*
		 * Preemption bad because we need PCPU_PTR(spinlocks) to not
		 * change.
		 */
		savecrit = critical_enter();
		nheld += witness_list_locks(PCPU_PTR(spinlocks));
		critical_exit(savecrit);
	}

	return (nheld);
}

void
witness_save(struct lock_object *lock, const char **filep, int *linep)
{

	KASSERT(!witness_cold, ("%s: witness_cold\n", __func__));
	if (lock->lo_witness == NULL)
		return;

	*filep = lock->lo_file;
	*linep = lock->lo_line;
}

void
witness_restore(struct lock_object *lock, const char *file, int line)
{

	KASSERT(!witness_cold, ("%s: witness_cold\n", __func__));
	if (lock->lo_witness == NULL)
		return;

	lock->lo_witness->w_file = file;
	lock->lo_witness->w_line = line;
	lock->lo_file = file;
	lock->lo_line = line;
}

#ifdef DDB

DB_SHOW_COMMAND(locks, db_witness_list)
{
	struct proc *p;
	pid_t pid;

	if (have_addr) {
		pid = (addr % 16) + ((addr >> 4) % 16) * 10 +
		    ((addr >> 8) % 16) * 100 + ((addr >> 12) % 16) * 1000 +
		    ((addr >> 16) % 16) * 10000;

		/* sx_slock(&allproc_lock); */
		LIST_FOREACH(p, &allproc, p_list) {
			if (p->p_pid == pid)
				break;
		}
		/* sx_sunlock(&allproc_lock); */
		if (p == NULL) {
			db_printf("pid %d not found\n", pid);
			return;
		}
	} else
		p = curproc;
		
	witness_list(p);
}

DB_SHOW_COMMAND(witness, db_witness_display)
{

	witness_display(db_printf);
}
#endif
