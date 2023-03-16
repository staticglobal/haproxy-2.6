/*
 * functions about threads.
 *
 * Copyright (C) 2017 Christopher Fauet - cfaulet@haproxy.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>

#include <signal.h>
#include <unistd.h>
#ifdef _POSIX_PRIORITY_SCHEDULING
#include <sched.h>
#endif

#ifdef USE_THREAD
#  include <pthread.h>
#endif

#ifdef USE_CPU_AFFINITY
#  include <sched.h>
#  if defined(__FreeBSD__) || defined(__DragonFly__)
#    include <sys/param.h>
#    ifdef __FreeBSD__
#      include <sys/cpuset.h>
#    endif
#    include <pthread_np.h>
#  endif
#  ifdef __APPLE__
#    include <mach/mach_types.h>
#    include <mach/thread_act.h>
#    include <mach/thread_policy.h>
#  endif
#  include <haproxy/cpuset.h>
#endif

#include <haproxy/cfgparse.h>
#include <haproxy/clock.h>
#include <haproxy/fd.h>
#include <haproxy/global.h>
#include <haproxy/log.h>
#include <haproxy/thread.h>
#include <haproxy/tools.h>

struct tgroup_info ha_tgroup_info[MAX_TGROUPS] = { };
THREAD_LOCAL const struct tgroup_info *tg = &ha_tgroup_info[0];

struct thread_info ha_thread_info[MAX_THREADS] = { };
THREAD_LOCAL const struct thread_info *ti = &ha_thread_info[0];

struct thread_ctx ha_thread_ctx[MAX_THREADS] = { };
THREAD_LOCAL struct thread_ctx *th_ctx = &ha_thread_ctx[0];

#ifdef USE_THREAD

volatile unsigned long threads_want_rdv_mask __read_mostly = 0;
volatile unsigned long threads_harmless_mask = 0;
volatile unsigned long threads_idle_mask = 0;
volatile unsigned long threads_sync_mask = 0;
volatile unsigned long all_threads_mask __read_mostly  = 1; // nbthread 1 assumed by default
THREAD_LOCAL unsigned int  tgid          = 1; // thread ID starts at 1
THREAD_LOCAL unsigned int  tid           = 0;
THREAD_LOCAL unsigned long tid_bit       = (1UL << 0);
int thread_cpus_enabled_at_boot          = 1;
static pthread_t ha_pthread[MAX_THREADS] = { };

/* Marks the thread as harmless until the last thread using the rendez-vous
 * point quits, excluding the current one. Thus an isolated thread may be safely
 * marked as harmless. Given that we can wait for a long time, sched_yield() is
 * used when available to offer the CPU resources to competing threads if
 * needed.
 */
void thread_harmless_till_end()
{
	_HA_ATOMIC_OR(&threads_harmless_mask, tid_bit);
	while (threads_want_rdv_mask & all_threads_mask & ~tid_bit) {
		ha_thread_relax();
	}
}

/* Isolates the current thread : request the ability to work while all other
 * threads are harmless, as defined by thread_harmless_now() (i.e. they're not
 * going to touch any visible memory area). Only returns once all of them are
 * harmless, with the current thread's bit in threads_harmless_mask cleared.
 * Needs to be completed using thread_release().
 */
void thread_isolate()
{
	unsigned long old;

	_HA_ATOMIC_OR(&threads_harmless_mask, tid_bit);
	__ha_barrier_atomic_store();
	_HA_ATOMIC_OR(&threads_want_rdv_mask, tid_bit);

	/* wait for all threads to become harmless */
	old = threads_harmless_mask;
	while (1) {
		if (unlikely((old & all_threads_mask) != all_threads_mask))
			old = threads_harmless_mask;
		else if (_HA_ATOMIC_CAS(&threads_harmless_mask, &old, old & ~tid_bit))
			break;

		ha_thread_relax();
	}
	/* one thread gets released at a time here, with its harmess bit off.
	 * The loss of this bit makes the other one continue to spin while the
	 * thread is working alone.
	 */
}

/* Isolates the current thread : request the ability to work while all other
 * threads are idle, as defined by thread_idle_now(). It only returns once
 * all of them are both harmless and idle, with the current thread's bit in
 * threads_harmless_mask and idle_mask cleared. Needs to be completed using
 * thread_release(). By doing so the thread also engages in being safe against
 * any actions that other threads might be about to start under the same
 * conditions. This specifically targets destruction of any internal structure,
 * which implies that the current thread may not hold references to any object.
 *
 * Note that a concurrent thread_isolate() will usually win against
 * thread_isolate_full() as it doesn't consider the idle_mask, allowing it to
 * get back to the poller or any other fully idle location, that will
 * ultimately release this one.
 */
void thread_isolate_full()
{
	unsigned long old;

	_HA_ATOMIC_OR(&threads_idle_mask, tid_bit);
	_HA_ATOMIC_OR(&threads_harmless_mask, tid_bit);
	__ha_barrier_atomic_store();
	_HA_ATOMIC_OR(&threads_want_rdv_mask, tid_bit);

	/* wait for all threads to become harmless */
	old = threads_harmless_mask;
	while (1) {
		unsigned long idle = _HA_ATOMIC_LOAD(&threads_idle_mask);

		if (unlikely((old & all_threads_mask) != all_threads_mask))
			old = _HA_ATOMIC_LOAD(&threads_harmless_mask);
		else if ((idle & all_threads_mask) == all_threads_mask &&
			 _HA_ATOMIC_CAS(&threads_harmless_mask, &old, old & ~tid_bit))
			break;

		ha_thread_relax();
	}

	/* we're not idle anymore at this point. Other threads waiting on this
	 * condition will need to wait until out next pass to the poller, or
	 * our next call to thread_isolate_full().
	 */
	_HA_ATOMIC_AND(&threads_idle_mask, ~tid_bit);
}

/* Cancels the effect of thread_isolate() by releasing the current thread's bit
 * in threads_want_rdv_mask. This immediately allows other threads to expect be
 * executed, though they will first have to wait for this thread to become
 * harmless again (possibly by reaching the poller again).
 */
void thread_release()
{
	_HA_ATOMIC_AND(&threads_want_rdv_mask, ~tid_bit);
}

/* Cancels the effect of thread_isolate() by releasing the current thread's bit
 * in threads_want_rdv_mask and by marking this thread as harmless until the
 * last worker finishes. The difference with thread_release() is that this one
 * will not leave the function before others are notified to do the same, so it
 * guarantees that the current thread will not pass through a subsequent call
 * to thread_isolate() before others finish.
 */
void thread_sync_release()
{
	_HA_ATOMIC_OR(&threads_sync_mask, tid_bit);
	__ha_barrier_atomic_store();
	_HA_ATOMIC_AND(&threads_want_rdv_mask, ~tid_bit);

	while (threads_want_rdv_mask & all_threads_mask) {
		_HA_ATOMIC_OR(&threads_harmless_mask, tid_bit);
		while (threads_want_rdv_mask & all_threads_mask)
			ha_thread_relax();
		HA_ATOMIC_AND(&threads_harmless_mask, ~tid_bit);
	}

	/* the current thread is not harmless anymore, thread_isolate()
	 * is forced to wait till all waiters finish.
	 */
	_HA_ATOMIC_AND(&threads_sync_mask, ~tid_bit);
	while (threads_sync_mask & all_threads_mask)
		ha_thread_relax();
}

/* Sets up threads, signals and masks, and starts threads 2 and above.
 * Does nothing when threads are disabled.
 */
void setup_extra_threads(void *(*handler)(void *))
{
	sigset_t blocked_sig, old_sig;
	int i;

	/* ensure the signals will be blocked in every thread */
	sigfillset(&blocked_sig);
	sigdelset(&blocked_sig, SIGPROF);
	sigdelset(&blocked_sig, SIGBUS);
	sigdelset(&blocked_sig, SIGFPE);
	sigdelset(&blocked_sig, SIGILL);
	sigdelset(&blocked_sig, SIGSEGV);
	pthread_sigmask(SIG_SETMASK, &blocked_sig, &old_sig);

	/* Create nbthread-1 thread. The first thread is the current process */
	ha_pthread[0] = pthread_self();
	for (i = 1; i < global.nbthread; i++)
		pthread_create(&ha_pthread[i], NULL, handler, &ha_thread_info[i]);
}

/* waits for all threads to terminate. Does nothing when threads are
 * disabled.
 */
void wait_for_threads_completion()
{
	int i;

	/* Wait the end of other threads */
	for (i = 1; i < global.nbthread; i++)
		pthread_join(ha_pthread[i], NULL);

#if defined(DEBUG_THREAD) || defined(DEBUG_FULL)
	show_lock_stats();
#endif
}

/* Tries to set the current thread's CPU affinity according to the cpu_map */
void set_thread_cpu_affinity()
{
#if defined(USE_CPU_AFFINITY)
	/* no affinity setting for the master process */
	if (master)
		return;

	/* Now the CPU affinity for all threads */
	if (ha_cpuset_count(&cpu_map.proc))
		ha_cpuset_and(&cpu_map.thread[tid], &cpu_map.proc);

	if (ha_cpuset_count(&cpu_map.thread[tid])) {/* only do this if the thread has a THREAD map */
#  if defined(__APPLE__)
		/* Note: this API is limited to the first 32/64 CPUs */
		unsigned long set = cpu_map.thread[tid].cpuset;
		int j;

		while ((j = ffsl(set)) > 0) {
			thread_affinity_policy_data_t cpu_set = { j - 1 };
			thread_port_t mthread;

			mthread = pthread_mach_thread_np(ha_pthread[tid]);
			thread_policy_set(mthread, THREAD_AFFINITY_POLICY, (thread_policy_t)&cpu_set, 1);
			set &= ~(1UL << (j - 1));
		}
#  else
		struct hap_cpuset *set = &cpu_map.thread[tid];

		pthread_setaffinity_np(ha_pthread[tid], sizeof(set->cpuset), &set->cpuset);
#  endif
	}
#endif /* USE_CPU_AFFINITY */
}

/* Retrieves the opaque pthread_t of thread <thr> cast to an unsigned long long
 * since POSIX took great care of not specifying its representation, making it
 * hard to export for post-mortem analysis. For this reason we copy it into a
 * union and will use the smallest scalar type at least as large as its size,
 * which will keep endianness and alignment for all regular sizes. As a last
 * resort we end up with a long long ligned to the first bytes in memory, which
 * will be endian-dependent if pthread_t is larger than a long long (not seen
 * yet).
 */
unsigned long long ha_get_pthread_id(unsigned int thr)
{
	union {
		pthread_t t;
		unsigned long long ll;
		unsigned int i;
		unsigned short s;
		unsigned char c;
	} u = { 0 };

	u.t = ha_pthread[thr];

	if (sizeof(u.t) <= sizeof(u.c))
		return u.c;
	else if (sizeof(u.t) <= sizeof(u.s))
		return u.s;
	else if (sizeof(u.t) <= sizeof(u.i))
		return u.i;
	return u.ll;
}

/* send signal <sig> to thread <thr> */
void ha_tkill(unsigned int thr, int sig)
{
	pthread_kill(ha_pthread[thr], sig);
}

/* send signal <sig> to all threads. The calling thread is signaled last in
 * order to allow all threads to synchronize in the handler.
 */
void ha_tkillall(int sig)
{
	unsigned int thr;

	for (thr = 0; thr < global.nbthread; thr++) {
		if (!(all_threads_mask & (1UL << thr)))
			continue;
		if (thr == tid)
			continue;
		pthread_kill(ha_pthread[thr], sig);
	}
	raise(sig);
}

void ha_thread_relax(void)
{
#ifdef _POSIX_PRIORITY_SCHEDULING
	sched_yield();
#else
	pl_cpu_relax();
#endif
}

/* these calls are used as callbacks at init time when debugging is on */
void ha_spin_init(HA_SPINLOCK_T *l)
{
	HA_SPIN_INIT(l);
}

/* these calls are used as callbacks at init time when debugging is on */
void ha_rwlock_init(HA_RWLOCK_T *l)
{
	HA_RWLOCK_INIT(l);
}

/* returns the number of CPUs the current process is enabled to run on,
 * regardless of any MAX_THREADS limitation.
 */
static int thread_cpus_enabled()
{
	int ret = 1;

#ifdef USE_CPU_AFFINITY
#if defined(__linux__) && defined(CPU_COUNT)
	cpu_set_t mask;

	if (sched_getaffinity(0, sizeof(mask), &mask) == 0)
		ret = CPU_COUNT(&mask);
#elif defined(__FreeBSD__) && defined(USE_CPU_AFFINITY)
	cpuset_t cpuset;
	if (cpuset_getaffinity(CPU_LEVEL_CPUSET, CPU_WHICH_PID, -1,
	    sizeof(cpuset), &cpuset) == 0)
		ret = CPU_COUNT(&cpuset);
#elif defined(__APPLE__)
	ret = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
#endif
	ret = MAX(ret, 1);
	return ret;
}

/* Returns 1 if the cpu set is currently restricted for the process else 0.
 * Currently only implemented for the Linux platform.
 */
int thread_cpu_mask_forced()
{
#if defined(__linux__)
	const int cpus_avail = sysconf(_SC_NPROCESSORS_ONLN);
	return cpus_avail != thread_cpus_enabled();
#else
	return 0;
#endif
}

/* Below come the lock-debugging functions */

#if defined(DEBUG_THREAD) || defined(DEBUG_FULL)

struct lock_stat lock_stats[LOCK_LABELS];

/* this is only used below */
static const char *lock_label(enum lock_label label)
{
	switch (label) {
	case TASK_RQ_LOCK:         return "TASK_RQ";
	case TASK_WQ_LOCK:         return "TASK_WQ";
	case LISTENER_LOCK:        return "LISTENER";
	case PROXY_LOCK:           return "PROXY";
	case SERVER_LOCK:          return "SERVER";
	case LBPRM_LOCK:           return "LBPRM";
	case SIGNALS_LOCK:         return "SIGNALS";
	case STK_TABLE_LOCK:       return "STK_TABLE";
	case STK_SESS_LOCK:        return "STK_SESS";
	case APPLETS_LOCK:         return "APPLETS";
	case PEER_LOCK:            return "PEER";
	case SHCTX_LOCK:           return "SHCTX";
	case SSL_LOCK:             return "SSL";
	case SSL_GEN_CERTS_LOCK:   return "SSL_GEN_CERTS";
	case PATREF_LOCK:          return "PATREF";
	case PATEXP_LOCK:          return "PATEXP";
	case VARS_LOCK:            return "VARS";
	case COMP_POOL_LOCK:       return "COMP_POOL";
	case LUA_LOCK:             return "LUA";
	case NOTIF_LOCK:           return "NOTIF";
	case SPOE_APPLET_LOCK:     return "SPOE_APPLET";
	case DNS_LOCK:             return "DNS";
	case PID_LIST_LOCK:        return "PID_LIST";
	case EMAIL_ALERTS_LOCK:    return "EMAIL_ALERTS";
	case PIPES_LOCK:           return "PIPES";
	case TLSKEYS_REF_LOCK:     return "TLSKEYS_REF";
	case AUTH_LOCK:            return "AUTH";
	case LOGSRV_LOCK:          return "LOGSRV";
	case DICT_LOCK:            return "DICT";
	case PROTO_LOCK:           return "PROTO";
	case QUEUE_LOCK:           return "QUEUE";
	case CKCH_LOCK:            return "CKCH";
	case SNI_LOCK:             return "SNI";
	case SSL_SERVER_LOCK:      return "SSL_SERVER";
	case SFT_LOCK:             return "SFT";
	case IDLE_CONNS_LOCK:      return "IDLE_CONNS";
	case QUIC_LOCK:            return "QUIC";
	case OTHER_LOCK:           return "OTHER";
	case DEBUG1_LOCK:          return "DEBUG1";
	case DEBUG2_LOCK:          return "DEBUG2";
	case DEBUG3_LOCK:          return "DEBUG3";
	case DEBUG4_LOCK:          return "DEBUG4";
	case DEBUG5_LOCK:          return "DEBUG5";
	case LOCK_LABELS:          break; /* keep compiler happy */
	};
	/* only way to come here is consecutive to an internal bug */
	abort();
}

void show_lock_stats()
{
	int lbl;

	for (lbl = 0; lbl < LOCK_LABELS; lbl++) {
		if (!lock_stats[lbl].num_write_locked &&
		    !lock_stats[lbl].num_seek_locked &&
		    !lock_stats[lbl].num_read_locked) {
			fprintf(stderr,
			        "Stats about Lock %s: not used\n",
			        lock_label(lbl));
			continue;
		}

		fprintf(stderr,
			"Stats about Lock %s: \n",
			lock_label(lbl));

		if (lock_stats[lbl].num_write_locked)
			fprintf(stderr,
			        "\t # write lock  : %llu\n"
			        "\t # write unlock: %llu (%lld)\n"
			        "\t # wait time for write     : %.3f msec\n"
			        "\t # wait time for write/lock: %.3f nsec\n",
			        (ullong)lock_stats[lbl].num_write_locked,
			        (ullong)lock_stats[lbl].num_write_unlocked,
			        (llong)(lock_stats[lbl].num_write_unlocked - lock_stats[lbl].num_write_locked),
			        (double)lock_stats[lbl].nsec_wait_for_write / 1000000.0,
			        lock_stats[lbl].num_write_locked ? ((double)lock_stats[lbl].nsec_wait_for_write / (double)lock_stats[lbl].num_write_locked) : 0);

		if (lock_stats[lbl].num_seek_locked)
			fprintf(stderr,
			        "\t # seek lock   : %llu\n"
			        "\t # seek unlock : %llu (%lld)\n"
			        "\t # wait time for seek      : %.3f msec\n"
			        "\t # wait time for seek/lock : %.3f nsec\n",
			        (ullong)lock_stats[lbl].num_seek_locked,
			        (ullong)lock_stats[lbl].num_seek_unlocked,
			        (llong)(lock_stats[lbl].num_seek_unlocked - lock_stats[lbl].num_seek_locked),
			        (double)lock_stats[lbl].nsec_wait_for_seek / 1000000.0,
			        lock_stats[lbl].num_seek_locked ? ((double)lock_stats[lbl].nsec_wait_for_seek / (double)lock_stats[lbl].num_seek_locked) : 0);

		if (lock_stats[lbl].num_read_locked)
			fprintf(stderr,
			        "\t # read lock   : %llu\n"
			        "\t # read unlock : %llu (%lld)\n"
			        "\t # wait time for read      : %.3f msec\n"
			        "\t # wait time for read/lock : %.3f nsec\n",
			        (ullong)lock_stats[lbl].num_read_locked,
			        (ullong)lock_stats[lbl].num_read_unlocked,
			        (llong)(lock_stats[lbl].num_read_unlocked - lock_stats[lbl].num_read_locked),
			        (double)lock_stats[lbl].nsec_wait_for_read / 1000000.0,
			        lock_stats[lbl].num_read_locked ? ((double)lock_stats[lbl].nsec_wait_for_read / (double)lock_stats[lbl].num_read_locked) : 0);
	}
}

void __ha_rwlock_init(struct ha_rwlock *l)
{
	memset(l, 0, sizeof(struct ha_rwlock));
	__RWLOCK_INIT(&l->lock);
}

void __ha_rwlock_destroy(struct ha_rwlock *l)
{
	__RWLOCK_DESTROY(&l->lock);
	memset(l, 0, sizeof(struct ha_rwlock));
}


void __ha_rwlock_wrlock(enum lock_label lbl, struct ha_rwlock *l,
                        const char *func, const char *file, int line)
{
	uint64_t start_time;

	if ((l->info.cur_readers | l->info.cur_seeker | l->info.cur_writer) & tid_bit)
		abort();

	HA_ATOMIC_OR(&l->info.wait_writers, tid_bit);

	start_time = now_mono_time();
	__RWLOCK_WRLOCK(&l->lock);
	HA_ATOMIC_ADD(&lock_stats[lbl].nsec_wait_for_write, (now_mono_time() - start_time));

	HA_ATOMIC_INC(&lock_stats[lbl].num_write_locked);

	l->info.cur_writer             = tid_bit;
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	HA_ATOMIC_AND(&l->info.wait_writers, ~tid_bit);
}

int __ha_rwlock_trywrlock(enum lock_label lbl, struct ha_rwlock *l,
                          const char *func, const char *file, int line)
{
	uint64_t start_time;
	int r;

	if ((l->info.cur_readers | l->info.cur_seeker | l->info.cur_writer) & tid_bit)
		abort();

	/* We set waiting writer because trywrlock could wait for readers to quit */
	HA_ATOMIC_OR(&l->info.wait_writers, tid_bit);

	start_time = now_mono_time();
	r = __RWLOCK_TRYWRLOCK(&l->lock);
	HA_ATOMIC_ADD(&lock_stats[lbl].nsec_wait_for_write, (now_mono_time() - start_time));
	if (unlikely(r)) {
		HA_ATOMIC_AND(&l->info.wait_writers, ~tid_bit);
		return r;
	}
	HA_ATOMIC_INC(&lock_stats[lbl].num_write_locked);

	l->info.cur_writer             = tid_bit;
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	HA_ATOMIC_AND(&l->info.wait_writers, ~tid_bit);

	return 0;
}

void __ha_rwlock_wrunlock(enum lock_label lbl,struct ha_rwlock *l,
                          const char *func, const char *file, int line)
{
	if (unlikely(!(l->info.cur_writer & tid_bit))) {
		/* the thread is not owning the lock for write */
		abort();
	}

	l->info.cur_writer             = 0;
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	__RWLOCK_WRUNLOCK(&l->lock);

	HA_ATOMIC_INC(&lock_stats[lbl].num_write_unlocked);
}

void __ha_rwlock_rdlock(enum lock_label lbl,struct ha_rwlock *l)
{
	uint64_t start_time;

	if ((l->info.cur_readers | l->info.cur_seeker | l->info.cur_writer) & tid_bit)
		abort();

	HA_ATOMIC_OR(&l->info.wait_readers, tid_bit);

	start_time = now_mono_time();
	__RWLOCK_RDLOCK(&l->lock);
	HA_ATOMIC_ADD(&lock_stats[lbl].nsec_wait_for_read, (now_mono_time() - start_time));
	HA_ATOMIC_INC(&lock_stats[lbl].num_read_locked);

	HA_ATOMIC_OR(&l->info.cur_readers, tid_bit);

	HA_ATOMIC_AND(&l->info.wait_readers, ~tid_bit);
}

int __ha_rwlock_tryrdlock(enum lock_label lbl,struct ha_rwlock *l)
{
	int r;

	if ((l->info.cur_readers | l->info.cur_seeker | l->info.cur_writer) & tid_bit)
		abort();

	/* try read should never wait */
	r = __RWLOCK_TRYRDLOCK(&l->lock);
	if (unlikely(r))
		return r;
	HA_ATOMIC_INC(&lock_stats[lbl].num_read_locked);

	HA_ATOMIC_OR(&l->info.cur_readers, tid_bit);

	return 0;
}

void __ha_rwlock_rdunlock(enum lock_label lbl,struct ha_rwlock *l)
{
	if (unlikely(!(l->info.cur_readers & tid_bit))) {
		/* the thread is not owning the lock for read */
		abort();
	}

	HA_ATOMIC_AND(&l->info.cur_readers, ~tid_bit);

	__RWLOCK_RDUNLOCK(&l->lock);

	HA_ATOMIC_INC(&lock_stats[lbl].num_read_unlocked);
}

void __ha_rwlock_wrtord(enum lock_label lbl, struct ha_rwlock *l,
                        const char *func, const char *file, int line)
{
	uint64_t start_time;

	if ((l->info.cur_readers | l->info.cur_seeker) & tid_bit)
		abort();

	if (!(l->info.cur_writer & tid_bit))
		abort();

	HA_ATOMIC_OR(&l->info.wait_readers, tid_bit);

	start_time = now_mono_time();
	__RWLOCK_WRTORD(&l->lock);
	HA_ATOMIC_ADD(&lock_stats[lbl].nsec_wait_for_read, (now_mono_time() - start_time));

	HA_ATOMIC_INC(&lock_stats[lbl].num_read_locked);

	HA_ATOMIC_OR(&l->info.cur_readers, tid_bit);
	HA_ATOMIC_AND(&l->info.cur_writer, ~tid_bit);
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	HA_ATOMIC_AND(&l->info.wait_readers, ~tid_bit);
}

void __ha_rwlock_wrtosk(enum lock_label lbl, struct ha_rwlock *l,
                        const char *func, const char *file, int line)
{
	uint64_t start_time;

	if ((l->info.cur_readers | l->info.cur_seeker) & tid_bit)
		abort();

	if (!(l->info.cur_writer & tid_bit))
		abort();

	HA_ATOMIC_OR(&l->info.wait_seekers, tid_bit);

	start_time = now_mono_time();
	__RWLOCK_WRTOSK(&l->lock);
	HA_ATOMIC_ADD(&lock_stats[lbl].nsec_wait_for_seek, (now_mono_time() - start_time));

	HA_ATOMIC_INC(&lock_stats[lbl].num_seek_locked);

	HA_ATOMIC_OR(&l->info.cur_seeker, tid_bit);
	HA_ATOMIC_AND(&l->info.cur_writer, ~tid_bit);
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	HA_ATOMIC_AND(&l->info.wait_seekers, ~tid_bit);
}

void __ha_rwlock_sklock(enum lock_label lbl, struct ha_rwlock *l,
                        const char *func, const char *file, int line)
{
	uint64_t start_time;

	if ((l->info.cur_readers | l->info.cur_seeker | l->info.cur_writer) & tid_bit)
		abort();

	HA_ATOMIC_OR(&l->info.wait_seekers, tid_bit);

	start_time = now_mono_time();
	__RWLOCK_SKLOCK(&l->lock);
	HA_ATOMIC_ADD(&lock_stats[lbl].nsec_wait_for_seek, (now_mono_time() - start_time));

	HA_ATOMIC_INC(&lock_stats[lbl].num_seek_locked);

	HA_ATOMIC_OR(&l->info.cur_seeker, tid_bit);
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	HA_ATOMIC_AND(&l->info.wait_seekers, ~tid_bit);
}

void __ha_rwlock_sktowr(enum lock_label lbl, struct ha_rwlock *l,
                        const char *func, const char *file, int line)
{
	uint64_t start_time;

	if ((l->info.cur_readers | l->info.cur_writer) & tid_bit)
		abort();

	if (!(l->info.cur_seeker & tid_bit))
		abort();

	HA_ATOMIC_OR(&l->info.wait_writers, tid_bit);

	start_time = now_mono_time();
	__RWLOCK_SKTOWR(&l->lock);
	HA_ATOMIC_ADD(&lock_stats[lbl].nsec_wait_for_write, (now_mono_time() - start_time));

	HA_ATOMIC_INC(&lock_stats[lbl].num_write_locked);

	HA_ATOMIC_OR(&l->info.cur_writer, tid_bit);
	HA_ATOMIC_AND(&l->info.cur_seeker, ~tid_bit);
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	HA_ATOMIC_AND(&l->info.wait_writers, ~tid_bit);
}

void __ha_rwlock_sktord(enum lock_label lbl, struct ha_rwlock *l,
                        const char *func, const char *file, int line)
{
	uint64_t start_time;

	if ((l->info.cur_readers | l->info.cur_writer) & tid_bit)
		abort();

	if (!(l->info.cur_seeker & tid_bit))
		abort();

	HA_ATOMIC_OR(&l->info.wait_readers, tid_bit);

	start_time = now_mono_time();
	__RWLOCK_SKTORD(&l->lock);
	HA_ATOMIC_ADD(&lock_stats[lbl].nsec_wait_for_read, (now_mono_time() - start_time));

	HA_ATOMIC_INC(&lock_stats[lbl].num_read_locked);

	HA_ATOMIC_OR(&l->info.cur_readers, tid_bit);
	HA_ATOMIC_AND(&l->info.cur_seeker, ~tid_bit);
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	HA_ATOMIC_AND(&l->info.wait_readers, ~tid_bit);
}

void __ha_rwlock_skunlock(enum lock_label lbl,struct ha_rwlock *l,
                          const char *func, const char *file, int line)
{
	if (!(l->info.cur_seeker & tid_bit))
		abort();

	HA_ATOMIC_AND(&l->info.cur_seeker, ~tid_bit);
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	__RWLOCK_SKUNLOCK(&l->lock);

	HA_ATOMIC_INC(&lock_stats[lbl].num_seek_unlocked);
}

int __ha_rwlock_trysklock(enum lock_label lbl, struct ha_rwlock *l,
                          const char *func, const char *file, int line)
{
	uint64_t start_time;
	int r;

	if ((l->info.cur_readers | l->info.cur_seeker | l->info.cur_writer) & tid_bit)
		abort();

	HA_ATOMIC_OR(&l->info.wait_seekers, tid_bit);

	start_time = now_mono_time();
	r = __RWLOCK_TRYSKLOCK(&l->lock);
	HA_ATOMIC_ADD(&lock_stats[lbl].nsec_wait_for_seek, (now_mono_time() - start_time));

	if (likely(!r)) {
		/* got the lock ! */
		HA_ATOMIC_INC(&lock_stats[lbl].num_seek_locked);
		HA_ATOMIC_OR(&l->info.cur_seeker, tid_bit);
		l->info.last_location.function = func;
		l->info.last_location.file     = file;
		l->info.last_location.line     = line;
	}

	HA_ATOMIC_AND(&l->info.wait_seekers, ~tid_bit);
	return r;
}

int __ha_rwlock_tryrdtosk(enum lock_label lbl, struct ha_rwlock *l,
                          const char *func, const char *file, int line)
{
	uint64_t start_time;
	int r;

	if ((l->info.cur_writer | l->info.cur_seeker) & tid_bit)
		abort();

	if (!(l->info.cur_readers & tid_bit))
		abort();

	HA_ATOMIC_OR(&l->info.wait_seekers, tid_bit);

	start_time = now_mono_time();
	r = __RWLOCK_TRYRDTOSK(&l->lock);
	HA_ATOMIC_ADD(&lock_stats[lbl].nsec_wait_for_seek, (now_mono_time() - start_time));

	if (likely(!r)) {
		/* got the lock ! */
		HA_ATOMIC_INC(&lock_stats[lbl].num_seek_locked);
		HA_ATOMIC_OR(&l->info.cur_seeker, tid_bit);
		HA_ATOMIC_AND(&l->info.cur_readers, ~tid_bit);
		l->info.last_location.function = func;
		l->info.last_location.file     = file;
		l->info.last_location.line     = line;
	}

	HA_ATOMIC_AND(&l->info.wait_seekers, ~tid_bit);
	return r;
}

void __spin_init(struct ha_spinlock *l)
{
	memset(l, 0, sizeof(struct ha_spinlock));
	__SPIN_INIT(&l->lock);
}

void __spin_destroy(struct ha_spinlock *l)
{
	__SPIN_DESTROY(&l->lock);
	memset(l, 0, sizeof(struct ha_spinlock));
}

void __spin_lock(enum lock_label lbl, struct ha_spinlock *l,
                 const char *func, const char *file, int line)
{
	uint64_t start_time;

	if (unlikely(l->info.owner & tid_bit)) {
		/* the thread is already owning the lock */
		abort();
	}

	HA_ATOMIC_OR(&l->info.waiters, tid_bit);

	start_time = now_mono_time();
	__SPIN_LOCK(&l->lock);
	HA_ATOMIC_ADD(&lock_stats[lbl].nsec_wait_for_write, (now_mono_time() - start_time));

	HA_ATOMIC_INC(&lock_stats[lbl].num_write_locked);


	l->info.owner                  = tid_bit;
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	HA_ATOMIC_AND(&l->info.waiters, ~tid_bit);
}

int __spin_trylock(enum lock_label lbl, struct ha_spinlock *l,
                   const char *func, const char *file, int line)
{
	int r;

	if (unlikely(l->info.owner & tid_bit)) {
		/* the thread is already owning the lock */
		abort();
	}

	/* try read should never wait */
	r = __SPIN_TRYLOCK(&l->lock);
	if (unlikely(r))
		return r;
	HA_ATOMIC_INC(&lock_stats[lbl].num_write_locked);

	l->info.owner                  = tid_bit;
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	return 0;
}

void __spin_unlock(enum lock_label lbl, struct ha_spinlock *l,
                   const char *func, const char *file, int line)
{
	if (unlikely(!(l->info.owner & tid_bit))) {
		/* the thread is not owning the lock */
		abort();
	}

	l->info.owner                  = 0;
	l->info.last_location.function = func;
	l->info.last_location.file     = file;
	l->info.last_location.line     = line;

	__SPIN_UNLOCK(&l->lock);
	HA_ATOMIC_INC(&lock_stats[lbl].num_write_unlocked);
}

#endif // defined(DEBUG_THREAD) || defined(DEBUG_FULL)

/* Depending on the platform and how libpthread was built, pthread_exit() may
 * involve some code in libgcc_s that would be loaded on exit for the first
 * time, causing aborts if the process is chrooted. It's harmless bit very
 * dirty. There isn't much we can do to make sure libgcc_s is loaded only if
 * needed, so what we do here is that during early boot we create a dummy
 * thread that immediately exits. This will lead to libgcc_s being loaded
 * during boot on the platforms where it's required.
 */
static void *dummy_thread_function(void *data)
{
	pthread_exit(NULL);
	return NULL;
}

static inline void preload_libgcc_s(void)
{
	pthread_t dummy_thread;
	pthread_create(&dummy_thread, NULL, dummy_thread_function, NULL);
	pthread_join(dummy_thread, NULL);
}

static void __thread_init(void)
{
	char *ptr = NULL;

	if (MAX_THREADS < 1 || MAX_THREADS > LONGBITS) {
		ha_alert("MAX_THREADS value must be between 1 and %d inclusive; "
		         "HAProxy was built with value %d, please fix it and rebuild.\n",
			 LONGBITS, MAX_THREADS);
		exit(1);
	}

	preload_libgcc_s();

	thread_cpus_enabled_at_boot = thread_cpus_enabled();
	thread_cpus_enabled_at_boot = MIN(thread_cpus_enabled_at_boot, MAX_THREADS);

	memprintf(&ptr, "Built with multi-threading support (MAX_THREADS=%d, default=%d).",
		  MAX_THREADS, thread_cpus_enabled_at_boot);
	hap_register_build_opts(ptr, 1);

#if defined(DEBUG_THREAD) || defined(DEBUG_FULL)
	memset(lock_stats, 0, sizeof(lock_stats));
#endif
}
INITCALL0(STG_PREPARE, __thread_init);

#else

/* send signal <sig> to thread <thr> (send to process in fact) */
void ha_tkill(unsigned int thr, int sig)
{
	raise(sig);
}

/* send signal <sig> to all threads (send to process in fact) */
void ha_tkillall(int sig)
{
	raise(sig);
}

void ha_thread_relax(void)
{
#ifdef _POSIX_PRIORITY_SCHEDULING
	sched_yield();
#endif
}

REGISTER_BUILD_OPTS("Built without multi-threading support (USE_THREAD not set).");

#endif // USE_THREAD


/* scans the configured thread mapping and establishes the final one. Returns <0
 * on failure, >=0 on success.
 */
int thread_map_to_groups()
{
	int t, g, ut, ug;
	int q, r;

	ut = ug = 0; // unassigned threads & groups

	for (t = 0; t < global.nbthread; t++) {
		if (!ha_thread_info[t].tg)
			ut++;
	}

	for (g = 0; g < global.nbtgroups; g++) {
		if (!ha_tgroup_info[g].count)
			ug++;
		ha_tgroup_info[g].tgid = g + 1;
	}

	if (ug > ut) {
		ha_alert("More unassigned thread-groups (%d) than threads (%d). Please reduce thread-groups\n", ug, ut);
		return -1;
	}

	/* look for first unassigned thread */
	for (t = 0; t < global.nbthread && ha_thread_info[t].tg; t++)
		;

	/* assign threads to empty groups */
	for (g = 0; ug && ut; ) {
		/* due to sparse thread assignment we can end up with more threads
		 * per group on last assigned groups than former ones, so we must
		 * always try to pack the maximum remaining ones together first.
		 */
		q = ut / ug;
		r = ut % ug;
		if ((q + !!r) > MAX_THREADS_PER_GROUP) {
			ha_alert("Too many remaining unassigned threads (%d) for thread groups (%d). Please increase thread-groups or make sure to keep thread numbers contiguous\n", ut, ug);
			return -1;
		}

		/* thread <t> is the next unassigned one. Let's look for next
		 * unassigned group, we know there are some left
		 */
		while (ut >= ug && ha_tgroup_info[g].count)
			g++;

		/* group g is unassigned, try to fill it with consecutive threads */
		while (ut && ut >= ug && ha_tgroup_info[g].count < q + !!r &&
		       (!ha_tgroup_info[g].count || t == ha_tgroup_info[g].base + ha_tgroup_info[g].count)) {

			if (!ha_tgroup_info[g].count) {
				/* assign new group */
				ha_tgroup_info[g].base = t;
				ug--;
			}

			ha_tgroup_info[g].count++;
			ha_thread_info[t].tg = &ha_tgroup_info[g];

			ut--;
			/* switch to next unassigned thread */
			while (++t < global.nbthread && ha_thread_info[t].tg)
				;
		}
	}

	if (ut) {
		ha_alert("Remaining unassigned threads found (%d) because all groups are in use. Please increase 'thread-groups', reduce 'nbthreads' or remove or extend 'thread-group' enumerations.\n", ut);
		return -1;
	}

	for (t = 0; t < global.nbthread; t++) {
		ha_thread_info[t].tid      = t;
		ha_thread_info[t].ltid     = t - ha_thread_info[t].tg->base;

		ha_thread_info[t].tid_bit  = 1UL << ha_thread_info[t].tid;
		ha_thread_info[t].ltid_bit = 1UL << ha_thread_info[t].ltid;
	}

	return 0;
}

/* converts a configuration thread group+mask to a global group+mask depending on
 * the configured thread group id. This is essentially for use with the "thread"
 * directive on "bind" lines, where "thread 2/1-3" might be turned to "4-6" for
 * the global ID. It cannot be used before the thread mapping above was completed
 * and the thread group number configured. Possible options:
 *  - igid == 0: imask represents global IDs. We have to check that all
 *    configured threads in the mask belong to the same group. If imask is zero
 *    it means everything, so for now we only support this with a single group.
 *  - igid > 0, imask = 0: convert local values to global values for this thread
 *  - igid > 0, imask > 0: convert local values to global values
 *
 * Returns <0 on failure, >=0 on success.
 */
int thread_resolve_group_mask(uint igid, ulong imask, uint *ogid, ulong *omask, char **err)
{
	ulong mask;
	uint t;

	if (igid == 0) {
		/* unspecified group, IDs are global */
		if (!imask) {
			/* all threads of all groups */
			if (global.nbtgroups > 1) {
				memprintf(err, "'thread' directive spans multiple groups");
				return -1;
			}
			mask = 0;
			*ogid = 1; // first and only group
			*omask = all_threads_mask;
			return 0;
		} else {
			/* some global threads */
			imask &= all_threads_mask;
			for (t = 0; t < global.nbthread; t++) {
				if (imask & (1UL << t)) {
					if (ha_thread_info[t].tg->tgid != igid) {
						if (!igid)
							igid = ha_thread_info[t].tg->tgid;
						else {
							memprintf(err, "'thread' directive spans multiple groups (at least %u and %u)", igid, ha_thread_info[t].tg->tgid);
							return -1;
						}
					}
				}
			}

			if (!igid) {
				memprintf(err, "'thread' directive contains threads that belong to no group");
				return -1;
			}

			/* we have a valid group, convert this to global thread IDs */
			*ogid = igid;
			*omask = imask << ha_tgroup_info[igid - 1].base;
			return 0;
		}
	} else {
		/* group was specified */
		if (igid > global.nbtgroups) {
			memprintf(err, "'thread' directive references non-existing thread group %u", igid);
			return -1;
		}

		if (!imask) {
			/* all threads of this groups. Let's make a mask from their count and base. */
			*ogid = igid;
			mask = 1UL << (ha_tgroup_info[igid - 1].count - 1);
			mask |= mask - 1;
			*omask = mask << ha_tgroup_info[igid - 1].base;
			return 0;
		} else {
			/* some local threads. Keep only existing ones for this group */

			mask = 1UL << (ha_tgroup_info[igid - 1].count - 1);
			mask |= mask - 1;

			if (!(mask & imask)) {
				/* no intersection between the thread group's
				 * threads and the bind line's.
				 */
#ifdef THREAD_AUTO_ADJUST_GROUPS
				unsigned long new_mask = 0;

				while (imask) {
					new_mask |= imask & mask;
					imask >>= ha_tgroup_info[igid - 1].count;
				}
				imask = new_mask;
#else
				memprintf(err, "'thread' directive only references threads not belonging to the group");
				return -1;
#endif
			}

			mask &= imask;
			*omask = mask << ha_tgroup_info[igid - 1].base;
			*ogid = igid;
			return 0;
		}
	}
}

/* Parse the "nbthread" global directive, which takes an integer argument that
 * contains the desired number of threads.
 */
static int cfg_parse_nbthread(char **args, int section_type, struct proxy *curpx,
                              const struct proxy *defpx, const char *file, int line,
                              char **err)
{
	long nbthread;
	char *errptr;

	if (too_many_args(1, args, err, NULL))
		return -1;

	nbthread = strtol(args[1], &errptr, 10);
	if (!*args[1] || *errptr) {
		memprintf(err, "'%s' passed a missing or unparsable integer value in '%s'", args[0], args[1]);
		return -1;
	}

#ifndef USE_THREAD
	if (nbthread != 1) {
		memprintf(err, "'%s' specified with a value other than 1 while HAProxy is not compiled with threads support. Please check build options for USE_THREAD", args[0]);
		return -1;
	}
#else
	if (nbthread < 1 || nbthread > MAX_THREADS) {
		memprintf(err, "'%s' value must be between 1 and %d (was %ld)", args[0], MAX_THREADS, nbthread);
		return -1;
	}

	all_threads_mask = nbits(nbthread);
#endif

	HA_DIAG_WARNING_COND(global.nbthread,
	                     "parsing [%s:%d] : '%s' is already defined and will be overridden.\n",
	                     file, line, args[0]);

	global.nbthread = nbthread;
	return 0;
}

/* Parse the "thread-group" global directive, which takes an integer argument
 * that designates a thread group, and a list of threads to put into that group.
 */
static int cfg_parse_thread_group(char **args, int section_type, struct proxy *curpx,
                                  const struct proxy *defpx, const char *file, int line,
                                  char **err)
{
	char *errptr;
	long tnum, tend, tgroup;
	int arg, tot;

	tgroup = strtol(args[1], &errptr, 10);
	if (!*args[1] || *errptr) {
		memprintf(err, "'%s' passed a missing or unparsable integer value in '%s'", args[0], args[1]);
		return -1;
	}

	if (tgroup < 1 || tgroup > MAX_TGROUPS) {
		memprintf(err, "'%s' thread-group number must be between 1 and %d (was %ld)", args[0], MAX_TGROUPS, tgroup);
		return -1;
	}

	/* look for a preliminary definition of any thread pointing to this
	 * group, and remove them.
	 */
	if (ha_tgroup_info[tgroup-1].count) {
		ha_warning("parsing [%s:%d] : '%s %ld' was already defined and will be overridden.\n",
		           file, line, args[0], tgroup);

		for (tnum = ha_tgroup_info[tgroup-1].base;
		     tnum < ha_tgroup_info[tgroup-1].base + ha_tgroup_info[tgroup-1].count;
		     tnum++) {
			if (ha_thread_info[tnum-1].tg == &ha_tgroup_info[tgroup-1])
				ha_thread_info[tnum-1].tg = NULL;
		}
		ha_tgroup_info[tgroup-1].count = ha_tgroup_info[tgroup-1].base = 0;
	}

	tot = 0;
	for (arg = 2; args[arg] && *args[arg]; arg++) {
		tend = tnum = strtol(args[arg], &errptr, 10);

		if (*errptr == '-')
			tend = strtol(errptr + 1, &errptr, 10);

		if (*errptr || tnum < 1 || tend < 1 || tnum > MAX_THREADS || tend > MAX_THREADS) {
			memprintf(err, "'%s %ld' passed an unparsable or invalid thread number '%s' (valid range is 1 to %d)", args[0], tgroup, args[arg], MAX_THREADS);
			return -1;
		}

		for(; tnum <= tend; tnum++) {
			if (ha_thread_info[tnum-1].tg == &ha_tgroup_info[tgroup-1]) {
				ha_warning("parsing [%s:%d] : '%s %ld': thread %ld assigned more than once on the same line.\n",
				           file, line, args[0], tgroup, tnum);
			} else if (ha_thread_info[tnum-1].tg) {
				ha_warning("parsing [%s:%d] : '%s %ld': thread %ld was previously assigned to thread group %ld and will be overridden.\n",
				           file, line, args[0], tgroup, tnum,
				           (long)(ha_thread_info[tnum-1].tg - &ha_tgroup_info[0] + 1));
			}

			if (!ha_tgroup_info[tgroup-1].count) {
				ha_tgroup_info[tgroup-1].base = tnum-1;
				ha_tgroup_info[tgroup-1].count = 1;
			}
			else if (tnum >= ha_tgroup_info[tgroup-1].base + ha_tgroup_info[tgroup-1].count) {
				ha_tgroup_info[tgroup-1].count = tnum - ha_tgroup_info[tgroup-1].base;
			}
			else if (tnum < ha_tgroup_info[tgroup-1].base) {
				ha_tgroup_info[tgroup-1].count += ha_tgroup_info[tgroup-1].base - tnum-1;
				ha_tgroup_info[tgroup-1].base = tnum - 1;
			}

			ha_thread_info[tnum-1].tg = &ha_tgroup_info[tgroup-1];
			tot++;
		}
	}

	if (ha_tgroup_info[tgroup-1].count > tot) {
		memprintf(err, "'%s %ld' assigned sparse threads, only contiguous supported", args[0], tgroup);
		return -1;
	}

	if (ha_tgroup_info[tgroup-1].count > MAX_THREADS_PER_GROUP) {
		memprintf(err, "'%s %ld' assigned too many threads (%d, max=%d)", args[0], tgroup, tot, MAX_THREADS_PER_GROUP);
		return -1;
	}

	return 0;
}

/* Parse the "thread-groups" global directive, which takes an integer argument
 * that contains the desired number of thread groups.
 */
static int cfg_parse_thread_groups(char **args, int section_type, struct proxy *curpx,
                                   const struct proxy *defpx, const char *file, int line,
                                   char **err)
{
	long nbtgroups;
	char *errptr;

	if (too_many_args(1, args, err, NULL))
		return -1;

	nbtgroups = strtol(args[1], &errptr, 10);
	if (!*args[1] || *errptr) {
		memprintf(err, "'%s' passed a missing or unparsable integer value in '%s'", args[0], args[1]);
		return -1;
	}

#ifndef USE_THREAD
	if (nbtgroups != 1) {
		memprintf(err, "'%s' specified with a value other than 1 while HAProxy is not compiled with threads support. Please check build options for USE_THREAD", args[0]);
		return -1;
	}
#else
	if (nbtgroups < 1 || nbtgroups > MAX_TGROUPS) {
		memprintf(err, "'%s' value must be between 1 and %d (was %ld)", args[0], MAX_TGROUPS, nbtgroups);
		return -1;
	}
#endif

	HA_DIAG_WARNING_COND(global.nbtgroups,
	                     "parsing [%s:%d] : '%s' is already defined and will be overridden.\n",
	                     file, line, args[0]);

	global.nbtgroups = nbtgroups;
	return 0;
}

/* config keyword parsers */
static struct cfg_kw_list cfg_kws = {ILH, {
	{ CFG_GLOBAL, "nbthread",       cfg_parse_nbthread, 0 },
	{ CFG_GLOBAL, "thread-group",   cfg_parse_thread_group, 0 },
	{ CFG_GLOBAL, "thread-groups",  cfg_parse_thread_groups, 0 },
	{ 0, NULL, NULL }
}};

INITCALL1(STG_REGISTER, cfg_register_keywords, &cfg_kws);
