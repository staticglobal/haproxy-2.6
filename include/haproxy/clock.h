/*
 * include/haproxy/clock.h
 * Exported parts for time-keeping
 *
 * Copyright (C) 2000-2021 Willy Tarreau - w@1wt.eu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _HAPROXY_CLOCK_H
#define _HAPROXY_CLOCK_H

#include <sys/time.h>
#include <haproxy/api.h>

extern struct timeval              start_date;    /* the process's start date in wall-clock time */
extern struct timeval              ready_date;    /* date when the process was considered ready */
extern volatile ullong             global_now;    /* common monotonic date between all threads (32:32) */

extern THREAD_LOCAL struct timeval now;           /* internal monotonic date derived from real clock */
extern THREAD_LOCAL struct timeval date;          /* the real current date (wall-clock time) */

uint64_t now_cpu_time_thread(int thr);
uint64_t now_mono_time(void);
uint64_t now_cpu_time(void);
void clock_set_local_source(void);
void clock_update_date(int max_wait, int interrupted);
void clock_init_process_date(void);
void clock_init_thread_date(void);
int clock_setup_signal_timer(void *timer, int sig, int val);
char *timeofday_as_iso_us(int pad);
uint clock_report_idle(void);
void clock_leaving_poll(int timeout, int interrupted);
void clock_entering_poll(void);

#endif
