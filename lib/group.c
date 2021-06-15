#define _GNU_SOURCE
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <sched.h>

#include "common.h"
#include "wayca_thread.h"

void wayca_thread_update_load(struct wayca_thread *thread, bool add)
{
	int cnt, pos;
	long long load;

	pthread_mutex_lock(&wayca_cpu_loads_mutex);
	cnt = CPU_COUNT(&thread->cur_set);
	load = div_round_up(cores_in_total(), cnt);

	if (!add)
		load = -load;

	pos = cpuset_find_first_set(&thread->cur_set);
	while (pos >= 0) {
		wayca_cpu_loads[pos] += load;
		pos = cpuset_find_next_set(&thread->cur_set, pos);
	}
	pthread_mutex_unlock(&wayca_cpu_loads_mutex);
}

int find_idlest_core(cpu_set_t *cpuset)
{
	int pos, idlest_core;
	long long load;

	pthread_mutex_lock(&wayca_cpu_loads_mutex);
	pos = cpuset_find_first_set(cpuset);
	idlest_core = pos;
	load = wayca_cpu_loads[pos];

	while (pos <= cpuset_find_last_set(cpuset) && pos >= 0) {
		if (load > wayca_cpu_loads[pos]) {
			load = wayca_cpu_loads[pos];
			idlest_core = pos;
		}

		pos = cpuset_find_next_set(cpuset, pos);
	}
	pthread_mutex_unlock(&wayca_cpu_loads_mutex);

	return idlest_core;
}

/**
 * Find the idlest set in the @cpuset, and return the found set
 * by @cpuset. The @cpuset must not be an empty set.
 */
void find_idlest_set(struct wayca_sc_group *group, cpu_set_t *cpuset)
{
	int stride, pos, idlest_pos, i;
	long long load = INT_MAX, tload;

	stride = group->nr_cpus_per_topo;
	pos = cpuset_find_first_set(cpuset);

	/* Make sure the @pos is always the start the topology. */
	pos -= pos % stride;
	idlest_pos = pos;

	pthread_mutex_lock(&wayca_cpu_loads_mutex);
	while (pos <= cpuset_find_last_set(cpuset)) {
		tload = 0;
		for (i = 0; i < stride; i++)
			tload += wayca_cpu_loads[pos + i];

		if (tload < load) {
			idlest_pos = pos;
			load = tload;
		}

		pos += stride;
	}
	pthread_mutex_unlock(&wayca_cpu_loads_mutex);

	CPU_ZERO(cpuset);

	for (i = idlest_pos; i < idlest_pos + stride; i++)
		CPU_SET(i, cpuset);
}

/**
 * Find the first topology set in the @cpuset, which is not set
 * completely. Return the id of the first CPU in the found set.
 */
int find_incomplete_set(struct wayca_sc_group *group, cpu_set_t *cpuset)
{
	int stride, pos;

	stride = group->nr_cpus_per_topo;
	pos = cpuset_find_first_set(&group->total);

	while (pos <= cpuset_find_last_set(&group->total)) {
		cpu_set_t tset;
		int i;

		CPU_ZERO(&tset);
		for (i = 0; i < stride; i++)
			CPU_SET(pos + i, &tset);

		CPU_AND(&tset, &tset, cpuset);

		/* An empty set is not an incomplete set. */
		if (CPU_COUNT(&tset) != stride && CPU_COUNT(&tset) != 0)
			return pos;

		pos += stride;
	}

	return -1;
}

bool is_thread_in_group(struct wayca_sc_group *group, struct wayca_thread *thread)
{
	struct wayca_thread *member = group->threads;

	while (member) {
		if (member == thread)
			return true;

		member = member->siblings;
	}

	return false;
}

bool is_group_in_father(struct wayca_sc_group *group, struct wayca_sc_group *father)
{
	struct wayca_sc_group *member = father->groups;

	while (member) {
		if (member == group)
			return true;

		member = member->siblings;
	}

	return false;
}

void group_thread_add_to_tail(struct wayca_sc_group *group, struct wayca_thread *thread)
{
	struct wayca_thread *tail = group->threads;

	assert(thread->siblings == NULL);

	/* No tail thread found, this is an empty group */
	if (!tail) {
		group->threads = thread;
		return;
	}

	while (tail->siblings)
		tail = tail->siblings;

	tail->siblings = thread;
}

void group_group_add_to_tail(struct wayca_sc_group *group, struct wayca_sc_group *father)
{
	struct wayca_sc_group *tail = father->groups;

	assert(group->siblings == NULL);

	if (!tail) {
		father->groups = group;
		return;
	}

	while (tail->siblings)
		tail = tail->siblings;

	tail->siblings = group;
}

void group_thread_delete_thread(struct wayca_sc_group *group, struct wayca_thread *thread)
{
	struct wayca_thread *member = group->threads, *next;

	/**
	 * If the first element is the target thread.
	 */
	if (member == thread) {
		group->threads = thread->siblings;
		thread->siblings = NULL;
		return;
	}

	while (member) {
		next = member->siblings;

		/* Found the target thread */
		if (next == thread) {
			next = next->siblings;
			member->siblings = next;
			thread->siblings = NULL;
			break;
		}

		member = next;
	}
}

void group_group_delete_group(struct wayca_sc_group *group, struct wayca_sc_group *father)
{
	struct wayca_sc_group *member = father->groups, *next;

	if (member == group) {
		father->groups = group->siblings;
		group->siblings = NULL;
		return;
	}

	while (member) {
		next = member->siblings;

		if (next == group) {
			next = next->siblings;
			member->siblings = next;
			group->siblings = NULL;
			break;
		}

		member = next;
	}
}

/**
 * wayca_group_request_resource_from_father - Get CPU resources from father group
 *
 * @group: the group which requires resource
 * @cpuset: the CPU resources required and will be allocated. Only represent the CPU counts
 *
 * The cpus assigned to the @group will be decided by the attribute of the father
 * group. If the topology level of the @group is higher than the father, the
 * request will fail.
 *
 * Returns 0 if requirement satified. Otherwise an error.
 */
int wayca_group_request_resource_from_father(struct wayca_sc_group *group, cpu_set_t *cpuset)
{
	int cnts = CPU_COUNT(cpuset);
	struct wayca_sc_group *father;
	cpu_set_t available_set;

	assert(group->father != NULL);
	assert(!CPU_EQUAL(&group->used, &group->total));

	father = group->father;

	if (cnts <= 0 || cnts > father->nr_cpus_per_topo)
		return -1;

	/**
	 * Always assign complete topology region of CPU to the child,
	 * according to the father's topology level.
	 */
	cnts = div_round_up(cnts, father->nr_cpus_per_topo);

	CPU_ZERO(cpuset);

	memset(&available_set, -1, sizeof(cpu_set_t));
	CPU_XOR(&available_set, &available_set, &father->used);
	CPU_AND(&available_set, &available_set, &father->total);

	find_idlest_set(father, &available_set);
	CPU_OR(&father->used, &father->used, &available_set);
	CPU_OR(cpuset, cpuset, &available_set);

	if (CPU_EQUAL(&father->used, &father->total)) {
		father->roll_over_cnts++;
		CPU_ZERO(&father->used);
	}

	return 0;
}

/**
 * wayca_group_request_resource - Determine how many cpu this group need
 *
 * Figure out how many cpus this group needs and get the required cpus
 * and set in the group->total member.
 *
 * If the group->father is NULL, which means this is the top level group,
 * then assign all the cpus in the system to the group.
 *
 * If the group->threads is NULL, which means this is an empty group, then
 * assign the minimum cpus to the group according to the group's attribute.
 */
int wayca_group_request_resource(struct wayca_sc_group *group)
{
	int nr_threads = group->nr_threads ? group->nr_threads : 4;
	cpu_set_t required_cpuset;

	if (group->father == NULL) {
		memcpy(&group->total, &total_cpu_set, sizeof(cpu_set_t));
		return 0;
	}

	CPU_ZERO(&required_cpuset);

	/**
	 * Setup the required cpuset numbers. No position information
	 * is included. The father will pass assigned cpuset later
	 * with the position of the start cpu. If the group has no father,
	 * the required set will always start from 0.
	 */
	for (int pos = 0; pos < nr_threads; pos++) {
		int cpu = pos * group->stride;
		CPU_SET(cpu, &required_cpuset);
	}

	/**
	 * TBD:
	 * 	If cpuset required from father failed, we need some fallback
	 * 	process rather than directly return fail.
	 */
	if (group->father != NULL &&
	    wayca_group_request_resource_from_father(group, &required_cpuset))
		return -1;

	memcpy(&group->total, &required_cpuset, sizeof(cpu_set_t));
	return 0;
}

int wayca_group_init(struct wayca_sc_group *group)
{
	/* Init with no members */
	group->threads = NULL;
	group->nr_threads = 0;

	/* Init group in no hierarchy */
	group->siblings = NULL;
	group->father = NULL;
	group->topo_hint = -1;
	group->roll_over_cnts = 0;

	CPU_ZERO(&group->used);
	pthread_mutex_init(&group->mutex, NULL);

	/* Init the group attribute, threads will compact in CCL and bind per-CPU */
	group->attribute = (WT_GF_CCL | WT_GF_COMPACT | WT_GF_PERCPU);

	return wayca_group_arrange(group);
}

int wayca_group_arrange(struct wayca_sc_group *group)
{
	/* Arrange the parameters according to the attribute */
	switch (group->attribute & 0xffff) {
	case WT_GF_CPU:
		group->nr_cpus_per_topo = 1;
		break;
	case WT_GF_CCL:
		group->nr_cpus_per_topo = cores_in_ccl();
		break;
	case WT_GF_NUMA:
		group->nr_cpus_per_topo = cores_in_node();
		break;
	case WT_GF_PACKAGE:
		group->nr_cpus_per_topo = cores_in_package();
		break;
	case WT_GF_ALL:
		group->nr_cpus_per_topo = cores_in_total();
		break;
	}

	/**
	 * Check the relationship of the threads in this group,
	 * and assign the proper stride to emplace each thread.
	 */
	if (group->attribute & WT_GF_COMPACT)
		group->stride = 1;
	else
		group->stride = group->nr_cpus_per_topo;

	return wayca_group_request_resource(group);
}

int wayca_group_assign_thread_resource(struct wayca_sc_group *group, struct wayca_thread *thread)
{
	cpu_set_t available_set;
	size_t target_pos = 0;

	memset(&available_set, -1, sizeof(cpu_set_t));
	CPU_XOR(&available_set, &available_set, &group->used);
	CPU_AND(&available_set, &available_set, &group->total);

	/**
	 * If threads in the group is compact, and the available CPU counts
	 * isn't integer multiple of topology level CPU counts, then find the
	 * incomplete topology set and place the thread.
	 * 
	 * Else find the idlest core in the idlest set and place the thread.
	 */
	if ((CPU_COUNT(&available_set) % group->nr_cpus_per_topo) && group->attribute & WT_GF_COMPACT) {
		int anchor = find_incomplete_set(group, &available_set);
		target_pos = anchor;
		/* iterate the available cpu set and find a proper cpu */
		while(target_pos < anchor + group->nr_cpus_per_topo && !CPU_ISSET(target_pos, &available_set))
			target_pos += 1;
	} else {
		find_idlest_set(group, &available_set);
		target_pos = find_idlest_core(&available_set);
	}

	/* Reset the thread's cpuset infomation first */
	CPU_ZERO(&thread->allowed_set);
	CPU_ZERO(&thread->cur_set);

	/**
	 * If the bind policy is per-CPU, then
	 * we only to assign one single CPU to the thread. So we directly
	 * assign the CPU at the @target_pos to the thread.
	 *
	 * Otherwise, we assign a set of CPUs to the thread, ranged
	 * [target_pos, target_pos + group->nr_cpus_per_topo).
	 */
	if (group->attribute & WT_GF_PERCPU) {
		CPU_SET(target_pos, &thread->cur_set);
		CPU_SET(target_pos, &thread->allowed_set);
	} else {
		/**
		 * If the bind policy is not per-CPU, then each thread will
		 * bind to a set of CPU according to the topology level.
		 * So always make the start CPU at the beginning of the
		 * The topology set to avoid intercrossing between adjacent
		 * sets.
		 */
		int anchor = target_pos - target_pos % group->nr_cpus_per_topo;
		for (int num = anchor;
		     num < anchor + group->nr_cpus_per_topo;
		     num++)
		{
			CPU_SET(num, &thread->cur_set);
			CPU_SET(num, &thread->allowed_set);
		}
	}

	/**
	 * Update the group's resource information.
	 * If the relationship between threads is compact, we only
	 * record the @target_pos of this thread in the group->used,
	 * as next thread may be placed in the same affinity set.
	 * Otherwise the threads are scattered, we have to record
	 * all the CPUs assigned to this thread in the group->used,
	 * as it's exclusive to the next thread.
	 */
	if (group->attribute & WT_GF_COMPACT)
		CPU_SET(target_pos, &group->used);
	else {
		if (group->attribute & WT_GF_PERCPU) {
			int anchor = target_pos - target_pos % group->nr_cpus_per_topo;
			for (int num = anchor;
			     num < anchor + group->nr_cpus_per_topo;
			     num++)
				CPU_SET(num, &group->used);
		} else
			CPU_OR(&group->used, &group->used, &thread->allowed_set);
	}

	/**
	 * When no cores remains in the group, increase the group->roll_over_cnts
	 * and clear the group->used.
	 */
	if (CPU_EQUAL(&group->used, &group->total)) {
		CPU_ZERO(&group->used);
		group->roll_over_cnts++;
	}

	return 0;
}

int wayca_group_add_thread(struct wayca_sc_group *group, struct wayca_thread *thread)
{
	group->nr_threads++;
	group_thread_add_to_tail(group, thread);

	thread->group = group;

	wayca_group_assign_thread_resource(group, thread);

	return 0;
}

int wayca_group_delete_thread(struct wayca_sc_group *group, struct wayca_thread *thread)
{
	if (!is_thread_in_group(group, thread))
		return -1;

	if (CPU_COUNT(&group->used) == 0) {
		assert(group->roll_over_cnts > 0);

		group->roll_over_cnts--;
		CPU_OR(&group->used, &group->used, &group->total);
	}

	CPU_XOR(&group->used, &group->used, &thread->allowed_set);

	group_thread_delete_thread(group, thread);
	thread->group = NULL;
	group->nr_threads--;

	return 0;
}

int wayca_group_rearrange_thread(struct wayca_sc_group *group, struct wayca_thread *thread)
{
	thread_sched_setaffinity(thread->pid, sizeof(cpu_set_t), &thread->cur_set);

	wayca_thread_update_load(thread, true);

	return 0;
}

int wayca_group_rearrange_group(struct wayca_sc_group *group)
{
	struct wayca_thread *thread;

	if (wayca_group_arrange(group))
		return -1;

	CPU_ZERO(&group->used);
	group->roll_over_cnts = 0;

	group_for_each_threads(thread, group) {
		wayca_thread_update_load(thread, false);
		wayca_group_assign_thread_resource(group, thread);
		wayca_group_rearrange_thread(group, thread);
	}

	return 0;
}

int wayca_group_add_group(struct wayca_sc_group *group, struct wayca_sc_group *father)
{
	if (is_group_in_father(group, father))
		return 0;

	if (group->nr_cpus_per_topo >= father->nr_cpus_per_topo)
		return -1;

	father->nr_groups++;
	group_group_add_to_tail(group, father);

	group->father = father;

	if (wayca_group_rearrange_group(group)) {
		group_group_delete_group(group, father);
		father->nr_groups--;
		group->father = NULL;

		return -1;
	}

	return 0;
}

int wayca_group_delete_group(struct wayca_sc_group *group, struct wayca_sc_group *father)
{
	if (!is_group_in_father(group, father))
		return -1;

	if (CPU_COUNT(&father->used) == 0) {
		assert(father->roll_over_cnts > 0);

		father->roll_over_cnts--;
		CPU_OR(&father->used, &father->used, &father->total);
	}

	CPU_XOR(&father->used, &father->used, &group->total);

	group_group_delete_group(group, father);
	father->nr_groups--;
	group->father = NULL;

	return 0;
}