/*
 * Copyright (c) 2021 HiSilicon Technologies Co., Ltd.
 * Wayca scheduler is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 *
 * See the Mulan PSL v2 for more details.
 */

#define _GNU_SOURCE
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <wayca-scheduler.h>

#include "common.h"

WAYCA_SC_INIT_PRIO(wayca_managed_thread_init, LAST);
struct task_cpu_map wayca_sc_cpu_maps[MAX_MANAGED_MAPS];

static inline void nodemask_to_cpumask(node_set_t *node_mask, cpu_set_t *cpu_mask)
{
	int cr_in_node = wayca_sc_cpus_in_node();
	int nr_in_total = wayca_sc_nodes_in_total();

	CPU_ZERO(cpu_mask);

	for (int i = 0; i < nr_in_total; i++) {
		if (!NODE_ISSET(i, node_mask))
			continue;
		for (int j = 0; j < cr_in_node; j++)
			CPU_SET(i * cr_in_node + j, cpu_mask);
	}
}

int WAYCA_SC_DECLSPEC to_task_cpu_map(char *cpu_list, struct task_cpu_map maps[])
{
	char *p = cpu_list;
	char *q, *r;
	int i = 0;

	/* format is like 1,3@c1$1 2,4@n0-1$2 */
	while (p && i < MAX_MANAGED_MAPS) {
		q = strchr(p, '@');
		if (!q)
			break;
		*q++ = 0;
		list_to_mask(p, sizeof(maps[i].tasks), &maps[i].tasks);

		p = strchr(q, ' ');
		if (p)
			*p = 0;

		r = strchr(q, '$');
		if (r) {
			*r = 0;
			maps[i].cpu_util = strtoul(r + 1, NULL, 10);
		}

		if (*q == 'c') {
			list_to_mask(q + 1, sizeof(maps[i].cpus), &maps[i].cpus);
		} else if (*q == 'n') {
			list_to_mask(q + 1, sizeof(maps[i].nodes), &maps[i].nodes);
			nodemask_to_cpumask(&maps[i].nodes, &maps[i].cpus);
		} else {
			perror("Bad cpu_bind");
			return -1;
		}

		if (!p)
			break;
		p++;
		i++;
	};

	return 0;
}

static void wayca_managed_thread_init(void)
{
	char *p = secure_getenv("MANAGED_THREADS");
	to_task_cpu_map(p, wayca_sc_cpu_maps);
}

int WAYCA_SC_DECLSPEC wayca_managed_thread_cpumask(int id, cpu_set_t *mask)
{
	for (int i = 0; i < MAX_MANAGED_MAPS; i++) {
		if (TASK_ISSET(id, &wayca_sc_cpu_maps[i].tasks)) {
			*mask = wayca_sc_cpu_maps[i].cpus;
			return 0;
		}
	}

	return -1;
}

int WAYCA_SC_DECLSPEC wayca_managed_thread_create(int id, pthread_t *thread, const pthread_attr_t *attr,
		void *(*start_routine) (void *), void *arg)
{
	int ret = pthread_create(thread, attr, start_routine, arg);

	if (ret == 0) {
		cpu_set_t mask;
		int err;

		err = wayca_managed_thread_cpumask(id, &mask);
		if (err) {
			perror("failed to get affinity for managed thread");
			return ret;
		}
		err = pthread_setaffinity_np(*thread, sizeof(mask), &mask);
		if (err)
			perror("failed to set affinity for managed thread");
	}

	return ret;
}

/*
 * On success, it returns the number of threads which have been created; otherwise, returns -1
 */
int WAYCA_SC_DECLSPEC wayca_managed_threadpool_create(int id, int num, pthread_t *thread[], const pthread_attr_t *attr,
		void *(*start_routine) (void *), void *arg)
{
	int ret, i;

	if (num <= 0)
		return -1;

	for (i = 0; i < num; i++) {
		ret = wayca_managed_thread_create(id + i, thread[i], attr, start_routine, arg);
		if (ret) {
			perror("failed to create managed thread in threadpool");
			break;
		}
	}

	return i;
}
