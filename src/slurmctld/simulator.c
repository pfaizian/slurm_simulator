#include "config.h"

#ifdef SLURM_SIMULATOR
#if HAVE_SYS_PRCTL_H
#  include <sys/prctl.h>
#endif

#include <errno.h>
#include <grp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "slurm/slurm_errno.h"

#include "src/common/assoc_mgr.h"
#include "src/common/checkpoint.h"
#include "src/common/daemonize.h"
#include "src/common/fd.h"
#include "src/common/gres.h"
#include "src/common/hostlist.h"
#include "src/common/layouts_mgr.h"
#include "src/common/log.h"
#include "src/common/macros.h"
#include "src/common/node_features.h"
#include "src/common/node_select.h"
#include "src/common/pack.h"
#include "src/common/power.h"
#include "src/common/proc_args.h"
#include "src/common/read_config.h"
#include "src/common/slurm_acct_gather_profile.h"
#include "src/common/slurm_accounting_storage.h"
#include "src/common/slurm_auth.h"
#include "src/common/slurm_ext_sensors.h"
#include "src/common/slurm_jobacct_gather.h"
#include "src/common/slurm_jobcomp.h"
#include "src/common/slurm_mcs.h"
#include "src/common/slurm_priority.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_interface.h"
#include "src/common/slurm_route.h"
#include "src/common/slurm_topology.h"
#include "src/common/switch.h"
#include "src/common/timers.h"
#include "src/common/uid.h"
#include "src/common/xsignal.h"
#include "src/common/xstring.h"

#include "src/slurmctld/acct_policy.h"
#include "src/slurmctld/agent.h"
#include "src/slurmctld/burst_buffer.h"
#include "src/slurmctld/fed_mgr.h"
#include "src/slurmctld/front_end.h"
#include "src/slurmctld/job_scheduler.h"
#include "src/slurmctld/job_submit.h"
#include "src/slurmctld/licenses.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/ping_nodes.h"
#include "src/slurmctld/port_mgr.h"
#include "src/slurmctld/power_save.h"
#include "src/slurmctld/powercapping.h"
#include "src/slurmctld/preempt.h"
#include "src/slurmctld/proc_req.h"
#include "src/slurmctld/read_config.h"
#include "src/slurmctld/reservation.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/slurmctld_plugstack.h"
#include "src/slurmctld/sched_plugin.h"
#include "src/slurmctld/srun_comm.h"
#include "src/slurmctld/state_save.h"
#include "src/slurmctld/trigger_mgr.h"

#include "src/common/sim/sim.h"


//fill slurm_node_registration_status_msg_t from fake slurmd
static void _sim_fill_slurm_node_registration_status_msg(slurm_node_registration_status_msg_t *msg)
{
	char *arch, *os;
	struct utsname buf;
	static bool first_msg = true;
	Buf gres_info;
	//slurmd_conf_t * conf = conf = xmalloc(sizeof(slurmd_conf_t));
	//_init_conf();
	slurm_ctl_conf_t *conf=&slurmctld_conf;

	msg->node_name   = xstrdup (conf->control_addr);//in sim same
	msg->version     = xstrdup (PACKAGE_VERSION);

	//this is slurmd machine not node, so doe not matter
	msg->cpus	 = 4;
	msg->boards	 = 1;
	msg->sockets	 = 1;
	msg->cores	 = 2;
	msg->threads	 = 2;
	//if (res_abs_cpus[0] == '\0')
		msg->cpu_spec_list = NULL;
	//else
	//	msg->cpu_spec_list = xstrdup (res_abs_cpus);
	msg->real_memory = 8192;
	msg->tmp_disk    = 8192;
	msg->hash_val    = slurm_get_hash_val();
	msg->cpu_load=0;
	msg->free_mem=4096;

	gres_info = init_buf(1024);
	if (gres_plugin_node_config_pack(gres_info) != SLURM_SUCCESS)
		error("error packing gres configuration");
	else
		msg->gres_info   = gres_info;

	msg->up_time     = 300;
	msg->slurmd_start_time = time(NULL)-msg->up_time;

	if (first_msg) {
		first_msg = false;
		info("CPUs=%u Boards=%u Sockets=%u Cores=%u Threads=%u "
		     "Memory=%lu TmpDisk=%u Uptime=%u CPUSpecList=%s",
		     msg->cpus, msg->boards, msg->sockets, msg->cores,
		     msg->threads, msg->real_memory, msg->tmp_disk,
		     msg->up_time, msg->cpu_spec_list);
	} else {
		debug3("CPUs=%u Boards=%u Sockets=%u Cores=%u Threads=%u "
		       "Memory=%lu TmpDisk=%u Uptime=%u CPUSpecList=%s",
		       msg->cpus, msg->boards, msg->sockets, msg->cores,
		       msg->threads, msg->real_memory, msg->tmp_disk,
		       msg->up_time, msg->cpu_spec_list);
	}

	uname(&buf);
	if ((arch = getenv("SLURM_ARCH")))
		msg->arch = xstrdup(arch);
	else
		msg->arch = xstrdup(buf.machine);
	if ((os = getenv("SLURM_OS")))
		msg->os   = xstrdup(os);
	else
		msg->os = xstrdup(buf.sysname);

	if (msg->startup) {
		if (switch_g_alloc_node_info(&msg->switch_nodeinfo))
			error("switch_g_alloc_node_info: %m");
		if (switch_g_build_node_info(msg->switch_nodeinfo))
			error("switch_g_build_node_info: %m");
	}

	msg->job_count = 0;
	msg->job_id    = NULL;
	/* Note: Running batch jobs will have step_id == NO_VAL */
	msg->step_id   = NULL;

	if (!msg->energy)
		msg->energy = acct_gather_energy_alloc(1);
	acct_gather_energy_g_get_data(ENERGY_DATA_NODE_ENERGY, msg->energy);

	msg->timestamp = time(NULL);

	return;
}

//fake a call from frontend slurmd and register nodes
static int _sim_register_nodes()
{
	int error_code = SLURM_SUCCESS;

	bool newly_up = false;

	slurm_node_registration_status_msg_t *node_regmsg =
		xmalloc (sizeof (slurm_node_registration_status_msg_t));

	node_regmsg->startup = (uint16_t) 1;
	_sim_fill_slurm_node_registration_status_msg(node_regmsg);
	node_regmsg->status  = SLURM_SUCCESS;

	error_code = validate_nodes_via_front_end(node_regmsg,
			SLURM_PROTOCOL_VERSION, &newly_up);

	slurm_free_node_registration_status_msg(node_regmsg);

	return error_code;
}
/* kill threads which are not impotent for simulation */
static int _sim_kill_not_critical_threads()
{
	int error_code = SLURM_SUCCESS;
	pthread_t tid_self=pthread_self();
	//slurmctld_config.server_thread_count;
	//slurmctld_config.thread_id_rpc;
	//pthread_cancel
	return error_code;
}

extern int sim_slurm_rpc_submit_batch_job(slurm_msg_t * msg);

static void print_job_specs(job_desc_msg_t* dmesg)
{
	debug("\tdmesg->job_id: %d\n", dmesg->job_id);
	debug2("\t\tdmesg->time_limit: %d\n", dmesg->time_limit);
	debug2("\t\tdmesg->name: (%s)\n", dmesg->name);
	debug2("\t\tdmesg->user_id: %d\n", dmesg->user_id);
	debug2("\t\tdmesg->group_id: %d\n", dmesg->group_id);
	debug2("\t\tdmesg->work_dir: (%s)\n", dmesg->work_dir);
	debug2("\t\tdmesg->qos: (%s)\n", dmesg->qos);
	debug2("\t\tdmesg->partition: (%s)\n", dmesg->partition);
	debug2("\t\tdmesg->account: (%s)\n", dmesg->account);
	debug2("\t\tdmesg->reservation: (%s)\n", dmesg->reservation);
	debug2("\t\tdmesg->dependency: (%s)\n", dmesg->dependency);
	debug2("\t\tdmesg->num_tasks: %d\n", dmesg->num_tasks);
	debug2("\t\tdmesg->min_cpus: %d\n", dmesg->min_cpus);
	debug2("\t\tdmesg->cpus_per_task: %d\n", dmesg->cpus_per_task);
	debug2("\t\tdmesg->ntasks_per_node: %d\n", dmesg->ntasks_per_node);
	//debug2("\t\tdmesg->duration: %d\n", dmesg->duration);
	debug2("\t\tdmesg->env_size: %d\n", dmesg->env_size);
	debug2("\t\tdmesg->environment[0]: (%s)\n", dmesg->environment[0]);
	debug2("\t\tdmesg->script: (%s)\n", dmesg->script);
}

/* simulator wrapper around _slurm_rpc_submit_batch_job - process RPC to submit a batch job */
static int _sim_slurm_rpc_submit_batch_job(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;

	//_slurm_rpc_submit_batch_job(msg);

	char *err_msg = NULL;
	struct job_record *job_ptr = NULL;
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred,
						 slurmctld_config.auth_info);

	job_desc_msg_t *job_desc_msg = (job_desc_msg_t *) msg->data;

	error_code = job_allocate(job_desc_msg,
							  job_desc_msg->immediate,
							  false, NULL, 0, uid, &job_ptr,
							  &err_msg,
							  msg->protocol_version);
	if(error_code!=SLURM_SUCCESS){
		info("_slurm_rpc_submit_batch_job: %s\n\t%s",
			     slurm_strerror(error_code),err_msg);
	}
	xfree(err_msg);
	return error_code;
}

static int _sim_submit_job(job_trace_t* jobd)
{
	job_desc_msg_t dmesg;
	submit_response_msg_t * respMsg = NULL;
	int rv = 0;
	char *script, line[1024];
	uid_t uidt;
	gid_t gidt;

	script = xstrdup("#!/bin/bash\n");

	slurm_init_job_desc_msg(&dmesg);

	/* Set up and call Slurm C-API for actual job submission. */
	dmesg.time_limit    = jobd->wclimit;
	dmesg.job_id        = jobd->job_id;
	dmesg.name	    = xstrdup("sim_job");
	sim_user_info_t *user_info=get_sim_user_by_name(jobd->username);
	if(user_info==NULL){
		info("Error:SIM: unknown user for simulator: %s",jobd->username);
		return SLURM_FAILURE;
	}
	dmesg.user_id       = user_info->sim_uid;
	dmesg.group_id      = user_info->sim_gid;
	dmesg.work_dir      = xstrdup("/tmp"); /* hardcoded to /tmp for now */
	dmesg.qos           = xstrdup(jobd->qosname);
	dmesg.partition     = xstrdup(jobd->partition);
	dmesg.account       = xstrdup(jobd->account);
	dmesg.reservation   = xstrdup(jobd->reservation);
	dmesg.dependency    = xstrdup(jobd->dependency);
	dmesg.num_tasks     = jobd->tasks;
	dmesg.min_cpus      = jobd->tasks;
	dmesg.cpus_per_task = jobd->cpus_per_task;
	dmesg.ntasks_per_node = jobd->tasks_per_node;
	//dmesg.duration      = jobd->duration;

	dmesg.pn_min_memory = NO_VAL64;//jobd->pn_min_memory;
	dmesg.features=xstrdup(jobd->features);
	dmesg.gres=xstrdup(jobd->gres);
	dmesg.shared=jobd->shared;

	/* Need something for environment--Should make this more generic! */
	dmesg.environment  = (char**)xmalloc(sizeof(char*)*2);
	dmesg.environment[0] = xstrdup("HOME=/root");
	dmesg.env_size = 1;

	/* Standard dummy script. */
	sprintf(line,"#SBATCH -n %u\n", jobd->tasks);
	xstrcat(script, line);
	xstrcat(script, "\necho \"Generated BATCH Job\"\necho \"La Fine!\"\n");

	dmesg.script        = xstrdup(script);

	print_job_specs(&dmesg);

	/*if ( slurm_submit_batch_job(&dmesg, &respMsg) ) {
		slurm_perror ("slurm_submit_batch_job");
		rv = -1;
	}*/

	job_desc_msg_t *req=&dmesg;
    slurm_msg_t req_msg;
    //slurm_msg_t resp_msg;
	bool host_set = false;
	char host[64];

	slurm_msg_t_init(&req_msg);
	//slurm_msg_t_init(&resp_msg);


	/*
	 * set Node and session id for this request
	 */
	if (req->alloc_sid == NO_VAL)
		req->alloc_sid = getsid(0);

	if ( (req->alloc_node == NULL)
		&& (gethostname_short(host, sizeof(host)) == 0) ) {
		req->alloc_node = host;
		host_set  = true;
	}

	req_msg.msg_type = REQUEST_SUBMIT_BATCH_JOB ;
	req_msg.data     = req;

	req_msg.auth_cred=g_slurm_auth_create(NULL);

	int error_code=_sim_slurm_rpc_submit_batch_job(&req_msg);


	//if (respMsg)
		info("Response from job submission\n\terror_code: %u"
				"\n\tjob_id: %u",
				error_code, dmesg.job_id);

	/* Cleanup */
	if (respMsg) slurm_free_submit_response_response_msg(respMsg);
	if (script) xfree(script);
	if (dmesg.name)        xfree(dmesg.name);
	if (dmesg.work_dir)    xfree(dmesg.work_dir);
	if (dmesg.qos)         xfree(dmesg.qos);
	if (dmesg.partition)   xfree(dmesg.partition);
	if (dmesg.account)     xfree(dmesg.account);
	if (dmesg.reservation) xfree(dmesg.reservation);
	if (dmesg.dependency)  xfree(dmesg.dependency);
	if (dmesg.script)      xfree(dmesg.script);
	xfree(dmesg.environment[0]);
	xfree(dmesg.environment);

	return rv;
}

/*
 * this function is called from job_scheduler.c::launch_job(job_ptr)
 * instead of spin-off agent_queue_request
 */
extern void sim_agent_queue_request_joblaunch(struct job_record *job_ptr, batch_job_launch_msg_t *launch_msg_ptr)
{
	info("SIM: faking sending message type REQUEST_BATCH_JOB_LAUNCH "
						"to %s\n", job_ptr->batch_host);

	sim_add_future_event(launch_msg_ptr);

	slurm_free_job_launch_msg(launch_msg_ptr);
}

static int _sim_send_complete_batch_script_msg(uint32_t jobid, int err, int status)
{
	//_slurm_rpc_complete_batch_script(&req_msg, 0);
	uid_t uid = g_slurm_auth_get_uid(g_slurm_auth_create(NULL), slurmctld_config.auth_info);

	int error_code = job_complete(jobid, uid, false, false, SLURM_SUCCESS);
	if(error_code!=SLURM_SUCCESS){
		info("ERROR:SIM: can not job_complete for job %d",jobid);
	}
	return SLURM_SUCCESS;
}
/*
 * return number of processed jobs
 */
extern int sim_process_finished_jobs()
{
	int jobs_ended=0;
	time_t now;
	bool run_scheduler;

	now = time(NULL);
	while ((head_simulator_event) &&
			(now >= head_simulator_event->when)) {

		volatile simulator_event_t *aux;
		int event_jid;

		event_jid               = head_simulator_event->job_id;
		aux                     = head_simulator_event;
		head_simulator_event    = head_simulator_event->next;
		//aux->next               = head_sim_completed_jobs;
		//head_sim_completed_jobs = aux;
		//_simulator_remove_job_from_nodes(head_sim_completed_jobs);

		//--total_sim_events;

		debug2("SIM: Sending JOB_COMPLETE_BATCH_SCRIPT"
			" for job %d", event_jid);

		(*sim_jobs_done)++;

		//pthread_mutex_unlock(&simulator_mutex);
		_sim_send_complete_batch_script_msg(event_jid,
			SLURM_SUCCESS, 0);
		//pthread_mutex_lock(&simulator_mutex);

		slurm_msg_t            msg;
		epilog_complete_msg_t  req;

		slurm_msg_t_init(&msg);


		req.job_id      = event_jid;
		req.return_code = SLURM_SUCCESS;
		req.node_name   = "localhost";

		msg.msg_type    = MESSAGE_EPILOG_COMPLETE;
		msg.data        = &req;

		//_slurm_rpc_epilog_complete(&msg, (bool *)&run_scheduler, 0);
		if(job_epilog_complete(req.job_id, req.node_name,req.return_code)){

		}else{
			info("ERROR:SIM: can not job_epilog_complete for job %d",event_jid);
		}



		debug2("SIM: JOB_COMPLETE_BATCH_SCRIPT for "
			"job %d SENT", event_jid);

		++jobs_ended;

	}
	return jobs_ended;
}

/* reference to sched_plugin */
int (*sim_sched_plugin_attempt_sched_ref)(void)=NULL;

/* execure scheduler from schedule_plugin */
extern void schedule_plugin_run_once()
{
	if (sim_sched_plugin_attempt_sched_ref!=NULL){
		(*sim_sched_plugin_attempt_sched_ref)();
	} else {
		info("Error: sched_plugin do not support simulator");
	}
}
/* reference to priority multifactor decay */
int (*sim_run_priority_decay)(void)=NULL;


extern int sched_interval;
static int      sim_job_sched_cnt = 0;
static pthread_mutex_t sim_sched_cnt_mutex = PTHREAD_MUTEX_INITIALIZER;
int sim_schedule()
{
	int jobs_scheduled;
	int purge_job_interval=60;
	uint32_t job_limit;

	static time_t last_sched_time = 0;
	static time_t last_full_sched_time = 0;
	static time_t last_checkpoint_time = 0;
	static time_t last_purge_job_time = 0;
	static time_t last_priority_calc_period = 0;

	time_t now;
	now = time(NULL);

	if (last_sched_time == 0){
		last_sched_time = now;
		last_full_sched_time = now;
		last_checkpoint_time = now;
		last_purge_job_time = now;
	}

	/* Locks: Read config, write job, write node, read partition */
	slurmctld_lock_t job_write_lock = {
		READ_LOCK, WRITE_LOCK, WRITE_LOCK, READ_LOCK, READ_LOCK };
	/* Locks: Write job */
	slurmctld_lock_t job_write_lock2 = {
		NO_LOCK, WRITE_LOCK, NO_LOCK, NO_LOCK, NO_LOCK };



	if (difftime(now, last_purge_job_time) >= purge_job_interval) {
		now = time(NULL);
		last_purge_job_time = now;
		debug2("Performing purge of old job records");
		lock_slurmctld(job_write_lock);
		purge_old_job();
		unlock_slurmctld(job_write_lock);
	}


	job_limit = NO_VAL;
	if (difftime(now, last_full_sched_time) >= sched_interval) {
		slurm_mutex_lock(&sim_sched_cnt_mutex);
		/* job_limit = job_sched_cnt;	Ignored */
		job_limit = INFINITE;
		sim_job_sched_cnt = 0;
		slurm_mutex_unlock(&sim_sched_cnt_mutex);
		last_full_sched_time = now;
	} else {
		slurm_mutex_lock(&sim_sched_cnt_mutex);
		if (sim_job_sched_cnt &&
		    (difftime(now, last_sched_time) >=
		     batch_sched_delay)) {
			job_limit = 0;	/* Default depth */
			sim_job_sched_cnt = 0;
		}
		slurm_mutex_unlock(&sim_sched_cnt_mutex);
	}
	if (job_limit != NO_VAL) {
		now = time(NULL);
		last_sched_time = now;
		lock_slurmctld(job_write_lock2);
		bb_g_load_state(false);	/* May alter job nice/prio */
		unlock_slurmctld(job_write_lock2);

		sim_pause_clock();
		if ( (jobs_scheduled=schedule(job_limit))>0 )
			last_checkpoint_time = 0; /* force state save */
		sim_resume_clock();

		set_job_elig_time();
	}

	return jobs_scheduled;
}
extern void sim_controller()
{
	//read conf
	sim_read_sim_conf();
	//set time
	*current_sim = slurm_sim_conf->time_start;
	*current_micro = 0;

	//read job traces
	sim_read_job_trace(slurm_sim_conf->jobs_trace_file);
	if(trace_head!=NULL){
		*current_sim = trace_head->submit-3;
		*current_micro = 0;
	}

	//kill threads which are not impotent for simulation
	_sim_kill_not_critical_threads();

	//register nodes
	_sim_register_nodes();

	sim_resume_clock();

	uint32_t time_to_terminate=0;
	int run_scheduler=0;
	int failed_submissions=0;

	static time_t last_priority_calc_period = 0;
	uint32_t priority_calc_period = slurm_get_priority_calc_period();

	//uint32_t schedule_last_runtime=*current_sim;
	uint32_t schedule_plugin_next_runtime=*current_sim+30;
	int schedule_plugin_short_sleep=false;
	time_t cur_time;
	schedule_plugin_run_once();
	if(sim_run_priority_decay!=NULL){
		(*sim_run_priority_decay)();
		last_priority_calc_period=time(NULL);
	}

	//simulation controller main loop
	while(1)
	{
		int new_job_submitted=0;
		int job_finished=0;
		//int scheduler_ran=0;
		//info("SIM main loop\n");

		/* Do we have to end simulation in this cycle?
		 * sim_end_point=0 run indefinitely
		 * sim_end_point=1 exit when all jobs are done
		 * sim_end_point>1 run till specified time
		 * */
		if (slurm_sim_conf->time_stop==1){
			/*terminate if all jobs are complete*/
			if(trace_head==NULL){
				//no more jobs to submit
				if(head_simulator_event==NULL){
					//all jobs in queue are done
					if(time_to_terminate==0){
						//No more jobs to run, terminating simulation, but will wait for 1 more minute
						time_to_terminate=current_sim[0]+60;
					}
					else if(current_sim[0]>time_to_terminate){
						info("No more jobs to run, terminating simulation");
						exit(0);
					}
				}
				else if(time_to_terminate>0){
					//change my mind no termination
					time_to_terminate=0;
				}
			}
		}
		else if (slurm_sim_conf->time_stop>1){
			/*terminate if reached end time*/
			if(slurm_sim_conf->time_stop <= current_sim[0]) {
				exit(0);
			}
		}


		/* Now checking if a new job needs to be submitted */
		while (trace_head) {
			/*
			 * Uhmm... This is necessary if a large number of jobs
			 * are submitted at the same time but it seems
			 * to have an impact on determinism
			 */

			debug("time_mgr: current %u and next trace %ld",
					*(current_sim), trace_head->submit);

			if (*(current_sim) >= trace_head->submit) {

				/*job_trace_t *temp_ptr;*/

				debug("[%d] time_mgr--current simulated time: "
					   "%u\n", __LINE__, *current_sim);

				insert_in_queue_trace_record(trace_head);


				if (_sim_submit_job (trace_head) < 0)
					++failed_submissions;

				trace_head = trace_head->next;

				run_scheduler=1;
				new_job_submitted=1;
				/*if (temp_ptr) xfree(temp_ptr);*/
			} else {
				/*
				 * The job trace list is ordered in time so
				 * there is nothing else to do.
				 */
				break;
			}
		}

		//check if jobs done
		if(sim_process_finished_jobs()>0){
			run_scheduler=1;
			job_finished=1;
		}
		run_scheduler=1;

		//
		if(sim_run_priority_decay!=NULL){
			cur_time=time(NULL);
			if(last_priority_calc_period+priority_calc_period<cur_time){
				(*sim_run_priority_decay)();
				last_priority_calc_period=time(NULL);
			}
		}

		//run scheduler

		//if(run_scheduler||schedule_last_runtime+60<time(NULL)){
			//also need to make it run every x seconds
			if(run_scheduler){
				sim_job_sched_cnt++;
				run_scheduler=0;
			}
			int jobs_scheduled=sim_schedule();

			//scheduler_ran=1;
			//schedule_last_runtime=time(NULL);

			//i.e. if was not able to schedule anything, need to wait for some
			//resource to get free
			//if(jobs_scheduled==0)
			//	run_scheduler=0;
		//}
		//plugin schedule e.g. backfill
		cur_time=time(NULL);
		if(new_job_submitted+job_finished){
			schedule_plugin_short_sleep=true;
		}
		if(schedule_plugin_next_runtime<cur_time)
		{
			schedule_plugin_run_once();
			if(schedule_plugin_short_sleep)
				schedule_plugin_next_runtime=*current_sim+1;
			else
				schedule_plugin_next_runtime=*current_sim+30;
			schedule_plugin_short_sleep=false;
		}
		//update time
		if(new_job_submitted+job_finished+run_scheduler==0){
			sim_pause_clock();
			sim_incr_clock(slurm_sim_conf->time_step);
			sim_resume_clock();
		}

	}
}

#endif