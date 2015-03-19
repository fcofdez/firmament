// The Firmament project
// Copyright (c) 2014 Malte Schwarzkopf <malte.schwarzkopf@cl.cam.ac.uk>
// Copyright (c) 2015 Ionel Gog <ionel.gog@cl.cam.ac.uk>
//
// Google cluster trace simulator tool.

#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

#include <sys/stat.h>
#include <fcntl.h>

#include "sim/trace-extract/google_trace_simulator.h"
#include "scheduling/dimacs_exporter.h"
#include "scheduling/flow_graph.h"
#include "scheduling/flow_graph_arc.h"
#include "scheduling/flow_graph_node.h"
#include "scheduling/quincy_cost_model.h"
#include "scheduling/quincy_dispatcher.h"
#include "misc/utils.h"
#include "misc/pb_utils.h"
#include "misc/string_utils.h"

using boost::lexical_cast;
using boost::algorithm::is_any_of;
using boost::token_compress_off;

namespace firmament {
namespace sim {

#define SUBMIT_EVENT 0
#define SCHEDULE_EVENT 1
#define EVICT_EVENT 2
#define FAIL_EVENT 3
#define FINISH_EVENT 4
#define KILL_EVENT 5
#define LOST_EVENT 6
#define UPDATE_PENDING_EVENT 7
#define UPDATE_RUNNING_EVENT 8


#define MACHINE_ADD 0
#define MACHINE_REMOVE 1
#define MACHINE_UPDATE 2

#define MACHINE_TMPL_FILE "../../../tests/testdata/machine_topo.pbin"
//#define MACHINE_TMPL_FILE "/tmp/mach_test.pbin"

DEFINE_uint64(runtime, 9223372036854775807,
              "Time in microsec to extract data for (from start of trace)");
DEFINE_string(output_dir, "", "Directory for output flow graphs.");
DEFINE_bool(tasks_preemption_bins, false,
            "Compute bins of number of preempted tasks.");
DEFINE_uint64(bin_time_duration, 10, "Bin size in microseconds.");
DEFINE_string(task_bins_output, "bins.out",
              "The file in which the task bins are written.");
DEFINE_bool(run_incremental_scheduler, false,
            "Run the Flowlessly incremental scheduler.");
DEFINE_int32(num_files_to_process, 500, "Number of files to process.");
DEFINE_string(solver, "cs2", "Solver to use: flowlessly | cs2 | custom.");

GoogleTraceSimulator::GoogleTraceSimulator(const string& trace_path) :
  job_map_(new JobMap_t), task_map_(new TaskMap_t),
  resource_map_(new ResourceMap_t), trace_path_(trace_path),
  knowledge_base_(new KnowledgeBaseSimulator),
  flow_graph_(new FlowGraph(
                new QuincyCostModel(resource_map_, job_map_, task_map_,
                                    &task_bindings_, knowledge_base_))),
  quincy_dispatcher_(
    new scheduler::QuincyDispatcher(shared_ptr<FlowGraph>(flow_graph_),
                                    false)) {
}

GoogleTraceSimulator::~GoogleTraceSimulator() {
  for (ResourceMap_t::iterator it = resource_map_->begin();
       it != resource_map_->end(); ++it) {
    delete it->second;
  }
  delete quincy_dispatcher_;
  delete knowledge_base_;
}

void GoogleTraceSimulator::Run() {
  FLAGS_debug_flow_graph = true;
  FLAGS_add_root_task_to_graph = false;
  if (!FLAGS_solver.compare("flowlessly")) {
    FLAGS_incremental_flow = FLAGS_run_incremental_scheduler;
    FLAGS_flow_scheduling_solver = "flowlessly";
    FLAGS_only_read_assignment_changes = true;
    FLAGS_flowlessly_binary =	SOLVER_DIR "/flowlessly-git/run_fast_cost_scaling";
  } else if (!FLAGS_solver.compare("cs2")) {
    FLAGS_incremental_flow = false;
    FLAGS_flow_scheduling_solver = "cs2";
    FLAGS_only_read_assignment_changes = false;
		FLAGS_cs2_binary = SOLVER_DIR "/cs2-git/cs2.exe";
  } else if (!FLAGS_solver.compare("custom")) {
  	FLAGS_flow_scheduling_solver = "custom";
  	if (!FLAGS_custom_binary.compare("")) {
  		LOG(FATAL) << "Must specify path to solver binary with custom type.";
  	}
  } else {
    LOG(FATAL) << "Unknown solver type: " << FLAGS_solver;
  }

  // command line argument sanity checking
  if (trace_path_.empty()) {
    LOG(FATAL) << "Please specify a path to the Google trace!";
  }

  LOG(INFO) << "Starting Google Trace extraction!";
  LOG(INFO) << "Time to extract for: " << FLAGS_runtime << " microseconds.";

  CreateRootResource();

  if (FLAGS_tasks_preemption_bins) {
    ofstream out_file(FLAGS_output_dir + "/" + FLAGS_task_bins_output);
    if (out_file.is_open()) {
      BinTasksByEventType(EVICT_EVENT, out_file);
      out_file.close();
    } else {
      LOG(ERROR) << "Could not open bin output file.";
    }
  } else {
    ReplayTrace();
  }
}

ResourceDescriptor* GoogleTraceSimulator::AddMachine(
    const ResourceTopologyNodeDescriptor& machine_tmpl, uint64_t machine_id) {
  // Create a new machine topology descriptor.
  ResourceTopologyNodeDescriptor* new_machine = rtn_root_.add_children();
  new_machine->CopyFrom(machine_tmpl);
  const string& root_uuid = rtn_root_.resource_desc().uuid();
  char hn[100];
  snprintf(hn, sizeof(hn), "h%ju", machine_id);
  DFSTraverseResourceProtobufTreeReturnRTND(
      new_machine, boost::bind(&GoogleTraceSimulator::ResetUuidAndAddResource,
                               this, _1, string(hn), root_uuid));
  ResourceDescriptor* rd = new_machine->mutable_resource_desc();
  rd->set_friendly_name(hn);
  // Add the node to the flow graph.
  flow_graph_->AddResourceTopology(*new_machine);
  // Add resource to the google machine_id to ResourceDescriptor* map.
  CHECK(InsertIfNotPresent(&machine_id_to_rd_, machine_id, rd));
  CHECK(InsertIfNotPresent(&machine_id_to_rtnd_, machine_id, new_machine));
  return rd;
}

TaskDescriptor* GoogleTraceSimulator::AddNewTask(
    const TaskIdentifier& task_identifier) {
  JobDescriptor** jdpp = FindOrNull(job_id_to_jd_, task_identifier.job_id);
  JobDescriptor* jd_ptr;
  if (!jdpp) {
    // Add new job to the graph
    jd_ptr = PopulateJob(task_identifier.job_id);
    CHECK_NOTNULL(jd_ptr);
  } else {
    jd_ptr = *jdpp;
  }
  // Ignore task if it has already been added.
  // TODO(ionel): We should not ignore it.
  TaskDescriptor* td_ptr = NULL;
  if (FindOrNull(task_id_to_td_, task_identifier) == NULL) {
    td_ptr = AddTaskToJob(jd_ptr);
    if (InsertIfNotPresent(task_map_.get(), td_ptr->uid(), td_ptr)) {
      CHECK(InsertIfNotPresent(&task_id_to_identifier_,
                               td_ptr->uid(), task_identifier));
      // Add task to the google (job_id, task_index) to TaskDescriptor* map.
      CHECK(InsertIfNotPresent(&task_id_to_td_, task_identifier, td_ptr));
      // Update the job in the flow graph. This method also adds the new task to
      // the flow graph.
      flow_graph_->AddOrUpdateJobNodes(jd_ptr);
    } else {
      // TODO(ionel): We should handle duplicate task ids.
      LOG(WARNING) << "Duplicate task id: " << td_ptr->uid() << " for task "
		   << task_identifier.job_id << " "
		   << task_identifier.task_index;
    }
  }
  return td_ptr;
}

void GoogleTraceSimulator::AddTaskEndEvent(
    uint64_t cur_timestamp,
    const TaskID_t& task_id,
    TaskIdentifier task_identifier,
    unordered_map<TaskIdentifier, uint64_t, TaskIdentifierHasher>* task_rnt) {
  uint64_t* runtime_ptr = FindOrNull(*task_rnt, task_identifier);
  EventDescriptor event_desc;
  event_desc.set_job_id(task_identifier.job_id);
  event_desc.set_task_index(task_identifier.task_index);
  event_desc.set_type(EventDescriptor::TASK_END_RUNTIME);
  if (runtime_ptr != NULL) {
    // We can approximate the duration of the task.
    events_.insert(pair<uint64_t, EventDescriptor>(cur_timestamp + *runtime_ptr,
                                                   event_desc));
    CHECK(InsertIfNotPresent(&task_id_to_end_time_, task_id,
                             cur_timestamp + *runtime_ptr));
  } else {
    // The task didn't finish in the trace. Set the task's end event to the
    // last timestamp of the simulation.
    events_.insert(pair<uint64_t, EventDescriptor>(FLAGS_runtime, event_desc));
    CHECK(InsertIfNotPresent(&task_id_to_end_time_, task_id, FLAGS_runtime));
  }
}

TaskDescriptor* GoogleTraceSimulator::AddTaskToJob(JobDescriptor* jd_ptr) {
  CHECK_NOTNULL(jd_ptr);
  TaskDescriptor* root_task = jd_ptr->mutable_root_task();
  TaskDescriptor* new_task = root_task->add_spawned();
  new_task->set_uid(GenerateTaskID(*root_task));
  new_task->set_state(TaskDescriptor::RUNNABLE);
  new_task->set_job_id(jd_ptr->uuid());
  return new_task;
}

void GoogleTraceSimulator::BinTasksByEventType(uint64_t event,
                                               ofstream& out_file) {
  char line[200];
  vector<string> vals;
  FILE* fptr = NULL;
  uint64_t time_interval_bound = FLAGS_bin_time_duration;
  uint64_t num_tasks = 0;
  for (int32_t file_num = 0; file_num < FLAGS_num_files_to_process;
       file_num++) {
    string fname;
    spf(&fname, "%s/task_events/part-%05d-of-00500.csv",
        trace_path_.c_str(), file_num);
    if ((fptr = fopen(fname.c_str(), "r")) == NULL) {
      LOG(ERROR) << "Failed to open trace for reading of task events.";
    }
    while (!feof(fptr)) {
      if (fscanf(fptr, "%[^\n]%*[\n]", &line[0]) > 0) {
        boost::split(vals, line, is_any_of(","), token_compress_off);
        if (vals.size() != 13) {
          LOG(ERROR) << "Unexpected structure of task event row: found "
                     << vals.size() << " columns.";
        } else {
          uint64_t task_time = lexical_cast<uint64_t>(vals[0]);
          uint64_t event_type = lexical_cast<uint64_t>(vals[5]);
          if (event_type == event) {
            if (task_time <= time_interval_bound) {
              num_tasks++;
            } else {
              out_file << "(" << time_interval_bound - FLAGS_bin_time_duration
                       << ", " << time_interval_bound << "]: "
                       << num_tasks << "\n";
              time_interval_bound += FLAGS_bin_time_duration;
              while (time_interval_bound < task_time) {
                out_file << "(" << time_interval_bound - FLAGS_bin_time_duration
                         << ", " << time_interval_bound << "]: 0\n";
                time_interval_bound += FLAGS_bin_time_duration;
              }
              num_tasks = 1;
            }
          }
        }
      }
    }
    fclose(fptr);
  }
  out_file << "(" << time_interval_bound - FLAGS_bin_time_duration << ", " <<
    time_interval_bound << "]: " << num_tasks << "\n";
}

void GoogleTraceSimulator::CreateRootResource() {
  ResourceID_t root_uuid = GenerateRootResourceID("XXXgoogleXXX");
  ResourceDescriptor* rd = rtn_root_.mutable_resource_desc();
  rd->set_uuid(to_string(root_uuid));
  LOG(INFO) << "Root res ID is " << to_string(root_uuid);
  CHECK(InsertIfNotPresent(&uuid_conversion_map_, to_string(root_uuid),
                           to_string(root_uuid)));

  // Add resources and job to flow graph
  flow_graph_->AddResourceTopology(rtn_root_);
  CHECK(InsertIfNotPresent(resource_map_.get(), root_uuid,
                           new ResourceStatus(rd, "endpoint_uri",
                                              GetCurrentTimestamp())));
}

void GoogleTraceSimulator::JobCompleted(uint64_t simulator_job_id,
                                        JobID_t job_id) {
  flow_graph_->DeleteNodesForJob(job_id);
  // This call frees the JobDescriptor* as well.
  job_map_->erase(job_id);
  job_id_to_jd_.erase(simulator_job_id);
  job_num_tasks_.erase(simulator_job_id);
}

void GoogleTraceSimulator::LoadJobsNumTasks() {
  char line[200];
  vector<string> cols;
  FILE* jobs_tasks_file = NULL;
  string jobs_tasks_file_name = trace_path_ +
    "/jobs_num_tasks/jobs_num_tasks.csv";
  if ((jobs_tasks_file = fopen(jobs_tasks_file_name.c_str(), "r")) == NULL) {
    LOG(FATAL) << "Failed to open jobs num tasks file.";
  }
  int64_t num_line = 1;
  while (!feof(jobs_tasks_file)) {
    if (fscanf(jobs_tasks_file, "%[^\n]%*[\n]", &line[0]) > 0) {
      boost::split(cols, line, is_any_of(" "), token_compress_off);
      if (cols.size() != 2) {
        LOG(ERROR) << "Unexpected structure of jobs num tasks row on line: "
                   << num_line;
      } else {
        uint64_t job_id = lexical_cast<uint64_t>(cols[0]);
        uint64_t num_tasks = lexical_cast<uint64_t>(cols[1]);
        CHECK(InsertIfNotPresent(&job_num_tasks_, job_id, num_tasks));
      }
    }
    num_line++;
  }
  fclose(jobs_tasks_file);
}

void GoogleTraceSimulator::LoadMachineEvents() {
  char line[200];
  vector<string> cols;
  FILE* machines_file;
  string machines_file_name = trace_path_ +
    "/machine_events/part-00000-of-00001.csv";
  if ((machines_file = fopen(machines_file_name.c_str(), "r")) == NULL) {
    LOG(ERROR) << "Failed to open trace for reading machine events.";
  }
  int64_t num_line = 1;
  while (!feof(machines_file)) {
    if (fscanf(machines_file, "%[^\n]%*[\n]", &line[0]) > 0) {
      boost::split(cols, line, is_any_of(","), token_compress_off);
      if (cols.size() != 6) {
        LOG(ERROR) << "Unexpected structure of machine events on line "
                   << num_line << ": found " << cols.size() << " columns.";
      } else {
        // schema: (timestamp, machine_id, event_type, platform, CPUs, Memory)
        EventDescriptor event_desc;
        event_desc.set_machine_id(lexical_cast<uint64_t>(cols[1]));
        event_desc.set_type(TranslateMachineEvent(
            lexical_cast<int32_t>(cols[2])));
        uint64_t timestamp = lexical_cast<uint64_t>(cols[0]);
        if (event_desc.type() == EventDescriptor::REMOVE_MACHINE ||
            event_desc.type() == EventDescriptor::ADD_MACHINE) {
          events_.insert(pair<uint64_t, EventDescriptor>(timestamp,
                                                         event_desc));
        } else {
          // TODO(ionel): Handle machine update events.
        }
      }
    }
    num_line++;
  }
  fclose(machines_file);
}

void GoogleTraceSimulator::LoadTaskRuntimeStats() {
  char line[1000];
  vector<string> cols;
  FILE* usage_file = NULL;
  string usage_file_name = trace_path_ +
    "/task_usage_stat/task_usage_stat.csv";
  if ((usage_file = fopen(usage_file_name.c_str(), "r")) == NULL) {
    LOG(FATAL) << "Failed to open trace task runtime stats file.";
  }
  int64_t num_line = 1;
  while (!feof(usage_file)) {
    if (fscanf(usage_file, "%[^\n]%*[\n]", &line[0]) > 0) {
      boost::split(cols, line, is_any_of(" "), token_compress_off);
      if (cols.size() != 38) {
      } else {
        TaskIdentifier task_id;
        task_id.job_id = lexical_cast<uint64_t>(cols[0]);
        task_id.task_index = lexical_cast<uint64_t>(cols[1]);
        // TODO(ionel): Populate the knowledge base. Note a -1 denotes value
        // unknown.
        // double min_mean_cpu_usage = lexical_cast<double>(cols[2]);
        // double max_mean_cpu_usage = lexical_cast<double>(cols[3]);
        // double avg_mean_cpu_usage = lexical_cast<double>(cols[4]);
        // double sd_mean_cpu_usage = lexical_cast<double>(cols[5]);
        // double min_canonical_mem_usage = lexical_cast<double>(cols[6]);
        // double max_canonical_mem_usage = lexical_cast<double>(cols[7]);
        // double avg_canonical_mem_usage = lexical_cast<double>(cols[8]);
        // double sd_canonical_mem_usage = lexical_cast<double>(cols[9]);
        // double min_assigned_mem_usage = lexical_cast<double>(cols[10]);
        // double max_assigned_mem_usage = lexical_cast<double>(cols[11]);
        // double avg_assigned_mem_usage = lexical_cast<double>(cols[12]);
        // double sd_assigned_mem_usage = lexical_cast<double>(cols[13]);
        // double min_unmapped_page_cache = lexical_cast<double>(cols[14]);
        // double max_unmapped_page_cache = lexical_cast<double>(cols[15]);
        // double avg_unmapped_page_cache = lexical_cast<double>(cols[16]);
        // double sd_unmapped_page_cache = lexical_cast<double>(cols[17]);
        // double min_total_page_cache = lexical_cast<double>(cols[18]);
        // double max_total_page_cache = lexical_cast<double>(cols[19]);
        // double avg_total_page_cache = lexical_cast<double>(cols[20]);
        // double sd_total_page_cache = lexical_cast<double>(cols[21]);
        // double min_mean_disk_io_time = lexical_cast<double>(cols[22]);
        // double max_mean_disk_io_time = lexical_cast<double>(cols[23]);
        // double avg_mean_disk_io_time = lexical_cast<double>(cols[24]);
        // double sd_mean_disk_io_time = lexical_cast<double>(cols[25]);
        // double min_mean_local_disk_used = lexical_cast<double>(cols[26]);
        // double max_mean_local_disk_used = lexical_cast<double>(cols[27]);
        // double avg_mean_local_disk_used = lexical_cast<double>(cols[28]);
        // double sd_mean_local_disk_used = lexical_cast<double>(cols[29]);
        // double min_cpi = lexical_cast<double>(cols[30]);
        // double max_cpi = lexical_cast<double>(cols[31]);
        // double avg_cpi = lexical_cast<double>(cols[32]);
        // double sd_cpi = lexical_cast<double>(cols[33]);
        // double min_mai = lexical_cast<double>(cols[34]);
        // double max_mai = lexical_cast<double>(cols[35]);
        // double avg_mai = lexical_cast<double>(cols[36]);
        // double sd_mai = lexical_cast<double>(cols[37]);
      }
    }
    num_line++;
  }
  fclose(usage_file);
}

unordered_map<TaskIdentifier, uint64_t, TaskIdentifierHasher>*
    GoogleTraceSimulator::LoadTasksRunningTime() {
  unordered_map<TaskIdentifier, uint64_t, TaskIdentifierHasher> *task_runtime =
    new unordered_map<TaskIdentifier, uint64_t, TaskIdentifierHasher>();
  char line[200];
  vector<string> cols;
  FILE* tasks_file = NULL;
  string tasks_file_name = trace_path_ +
    "/task_runtime_events/task_runtime_events.csv";
  if ((tasks_file = fopen(tasks_file_name.c_str(), "r")) == NULL) {
    LOG(FATAL) << "Failed to open trace runtime events file.";
  }
  int64_t num_line = 1;
  while (!feof(tasks_file)) {
    if (fscanf(tasks_file, "%[^\n]%*[\n]", &line[0]) > 0) {
      boost::split(cols, line, is_any_of(" "), token_compress_off);
      if (cols.size() != 13) {
        LOG(ERROR) << "Unexpected structure of task runtime row on line: "
                   << num_line;
      } else {
        TaskIdentifier task_id;
        task_id.job_id = lexical_cast<uint64_t>(cols[0]);
        task_id.task_index = lexical_cast<uint64_t>(cols[1]);
        uint64_t runtime = lexical_cast<uint64_t>(cols[4]);
        if (!InsertIfNotPresent(task_runtime, task_id, runtime) &&
            VLOG_IS_ON(1)) {
          LOG(ERROR) << "There should not be more than an entry for job "
                     << task_id.job_id << ", task " << task_id.task_index;
        }
      }
    }
    num_line++;
  }
  fclose(tasks_file);
  return task_runtime;
}

JobDescriptor* GoogleTraceSimulator::PopulateJob(uint64_t job_id) {
  JobDescriptor jd;
  // Generate a hash out of the trace job_id.
  JobID_t new_job_id = GenerateJobID(job_id);

  CHECK(InsertIfNotPresent(job_map_.get(), new_job_id, jd));
  // Get the new value of the pointer because jd has been copied.
  JobDescriptor* jd_ptr = FindOrNull(*job_map_, new_job_id);

  // Maintain a mapping between the trace job_id and the generated job_id.
  jd_ptr->set_uuid(to_string(new_job_id));
  InsertOrUpdate(&job_id_to_jd_, job_id, jd_ptr);
  TaskDescriptor* rt = jd_ptr->mutable_root_task();
  string bin;
  // XXX(malte): hack, should use logical job name
  spf(&bin, "%jd", job_id);
  rt->set_binary(bin);
  rt->set_uid(GenerateRootTaskID(*jd_ptr));
  rt->set_state(TaskDescriptor::RUNNABLE);
  return jd_ptr;
}

void GoogleTraceSimulator::ProcessSimulatorEvents(
    uint64_t cur_time,
    const ResourceTopologyNodeDescriptor& machine_tmpl) {
  multimap<uint64_t, EventDescriptor>::iterator it = events_.begin();
  multimap<uint64_t, EventDescriptor>::iterator it_to =
    events_.upper_bound(cur_time);
  for (; it != it_to; it++) {
    if (it->second.type() == EventDescriptor::ADD_MACHINE) {
      VLOG(2) << "ADD_MACHINE " << it->second.machine_id();
      AddMachine(machine_tmpl, it->second.machine_id());
    } else if (it->second.type() == EventDescriptor::REMOVE_MACHINE) {
      VLOG(2) << "REMOVE_MACHINE " << it->second.machine_id();
      RemoveMachine(it->second.machine_id());
    } else if (it->second.type() == EventDescriptor::UPDATE_MACHINE) {
      // TODO(ionel): Handle machine update event.
    } else if (it->second.type() == EventDescriptor::TASK_END_RUNTIME) {
      VLOG(2) << "TASK_END_RUNTIME " << it->second.job_id() << " "
              << it->second.task_index();
      // Task has finished.
      TaskIdentifier task_identifier;
      task_identifier.task_index = it->second.task_index();
      task_identifier.job_id = it->second.job_id();
      TaskCompleted(task_identifier);
    } else {
      LOG(ERROR) << "Unexpected machine event";
    }
  }
  events_.erase(events_.begin(), it_to);
}

void GoogleTraceSimulator::ProcessTaskEvent(
    uint64_t cur_time, const TaskIdentifier& task_identifier,
    uint64_t event_type) {
  if (event_type == SUBMIT_EVENT) {
    AddNewTask(task_identifier);
  }
}

void GoogleTraceSimulator::RemoveMachine(uint64_t machine_id) {
  ResourceDescriptor** rd_ptr = FindOrNull(machine_id_to_rd_, machine_id);
  CHECK_NOTNULL(rd_ptr);
  machine_id_to_rd_.erase(machine_id);
  ResourceTopologyNodeDescriptor** rtnd_ptr =
    FindOrNull(machine_id_to_rtnd_, machine_id);
  CHECK_NOTNULL(rtnd_ptr);
  DFSTraverseResourceProtobufTreeReturnRTND(
      *rtnd_ptr, boost::bind(&GoogleTraceSimulator::RemoveResource, this, _1));
  if ((*rtnd_ptr)->has_parent_id()) {
    // TODO(ionel): Delete node from the parent's children list.
  } else {
    LOG(WARNING) << "Machine " << machine_id << " doesn't have a parent";
  }
  machine_id_to_rtnd_.erase(machine_id);
  // We can't delete the node because we haven't removed it from it's parent
  // children list.
  // delete *rtnd_ptr;
}

void GoogleTraceSimulator::RemoveResource(
    ResourceTopologyNodeDescriptor* rtnd) {
  ResourceID_t res_id = ResourceIDFromString(rtnd->resource_desc().uuid());
  flow_graph_->DeleteResourceNode(res_id);
  TaskID_t* task_id = FindOrNull(res_id_to_task_id_, res_id);
  if (task_id != NULL) {
    // Evict the task running on the resource.
    TaskEvicted(*task_id, res_id);
  }
  // We don't need to set the state of the resource because it gets removed
  // anyway.
  ResourceStatus* rs_ptr = FindPtrOrNull(*resource_map_, res_id);
  CHECK_NOTNULL(rs_ptr);
  resource_map_->erase(res_id);
  delete rs_ptr;
}

void GoogleTraceSimulator::TaskEvicted(TaskID_t task_id,
                                       const ResourceID_t& res_id) {
  VLOG(2) << "Evict task " << task_id << " from resource " << res_id;
  TaskDescriptor** td_ptr = FindOrNull(*task_map_, task_id);
  CHECK_NOTNULL(td_ptr);
  // Change the state of the task from running to runnable.
  (*td_ptr)->set_state(TaskDescriptor::RUNNABLE);
  flow_graph_->NodeForTaskID(task_id)->type_.set_type(
      FlowNodeType::UNSCHEDULED_TASK);
  // Remove the running arc and add back arcs to EC and UNSCHED.
  // TODO(ionel): Inform the cost model that this task has already failed.
  flow_graph_->UpdateArcsForEvictedTask(task_id, res_id);

  // Get the Google trace identifier of the task.
  TaskIdentifier* ti_ptr = FindOrNull(task_id_to_identifier_, task_id);
  CHECK_NOTNULL(ti_ptr);
  // Get the end time of the task.
  uint64_t* task_end_time = FindOrNull(task_id_to_end_time_, task_id);
  CHECK_NOTNULL(task_end_time);
  // Remove the task end time event from the simulator events_.
  pair<multimap<uint64_t, EventDescriptor>::iterator,
       multimap<uint64_t, EventDescriptor>::iterator> range_it =
    events_.equal_range(*task_end_time);
  for (; range_it.first != range_it.second; range_it.first++) {
    if (range_it.first->second.type() == EventDescriptor::TASK_END_RUNTIME &&
        range_it.first->second.job_id() == ti_ptr->job_id &&
        range_it.first->second.task_index() == ti_ptr->task_index) {
      break;
    }
  }
  ResourceID_t res_id_tmp(res_id);
  task_bindings_.erase(task_id);
  res_id_to_task_id_.erase(res_id_tmp);
  // Remove current task end time.
  task_id_to_end_time_.erase(task_id);
  // We've found the event.
  if (range_it.first != range_it.second) {
    events_.erase(range_it.first);
  }
}

void GoogleTraceSimulator::ResetUuidAndAddResource(
    ResourceTopologyNodeDescriptor* rtnd, const string& hostname,
    const string& root_uuid) {
  string new_uuid;
  if (rtnd->has_parent_id()) {
    // This is an intermediate node, so translate the parent UUID via the
    // lookup table
    const string& old_parent_id = rtnd->parent_id();
    string* new_parent_id = FindOrNull(uuid_conversion_map_, rtnd->parent_id());
    CHECK_NOTNULL(new_parent_id);
    VLOG(2) << "Resetting parent UUID for " << rtnd->resource_desc().uuid()
            << ", parent was " << old_parent_id
            << ", is now " << *new_parent_id;
    rtnd->set_parent_id(*new_parent_id);
    // Grab a new UUID for the node itself
    new_uuid = to_string(GenerateResourceID());
  } else {
    // This is the top of a machine topology, so generate a first UUID for its
    // topology based on its hostname and link it into the root
    rtnd->set_parent_id(root_uuid);
    new_uuid = to_string(GenerateRootResourceID(hostname));
  }
  VLOG(2) << "Resetting UUID for " << rtnd->resource_desc().uuid() << " to "
          << new_uuid;
  InsertOrUpdate(&uuid_conversion_map_, rtnd->resource_desc().uuid(), new_uuid);
  ResourceDescriptor* rd = rtnd->mutable_resource_desc();
  rd->set_uuid(new_uuid);
  // Add the resource node to the map.
  CHECK(InsertIfNotPresent(resource_map_.get(),
                           ResourceIDFromString(rd->uuid()),
                           new ResourceStatus(rd, "endpoint_uri",
                                              GetCurrentTimestamp())));
}

void GoogleTraceSimulator::ReplayTrace() {
  // Load all the machine events.
  LoadMachineEvents();
  // Populate the job_id to number of tasks mapping.
  LoadJobsNumTasks();
  // Load tasks' runtime.
  unordered_map<TaskIdentifier, uint64_t, TaskIdentifierHasher>* task_runtime =
    LoadTasksRunningTime();
  // Populate the knowledge base.
  LoadTaskRuntimeStats();

  // Import a fictional machine resource topology
  ResourceTopologyNodeDescriptor machine_tmpl;
  int fd = open(MACHINE_TMPL_FILE, O_RDONLY);
  machine_tmpl.ParseFromFileDescriptor(fd);
  close(fd);

  char line[200];
  vector<string> vals;
  FILE* f_task_events_ptr = NULL;
  uint64_t time_interval_bound = FLAGS_bin_time_duration;
  for (int32_t file_num = 0; file_num < FLAGS_num_files_to_process;
       file_num++) {
    string fname;
    spf(&fname, "%s/task_events/part-%05d-of-00500.csv", trace_path_.c_str(),
        file_num);
    if ((f_task_events_ptr = fopen(fname.c_str(), "r")) == NULL) {
      LOG(ERROR) << "Failed to open trace for reading of task events.";
    }
    while (!feof(f_task_events_ptr)) {
      if (fscanf(f_task_events_ptr, "%[^\n]%*[\n]", &line[0]) > 0) {
        boost::split(vals, line, is_any_of(","), token_compress_off);
        if (vals.size() != 13) {
          LOG(ERROR) << "Unexpected structure of task event row: found "
                     << vals.size() << " columns.";
        } else {
          TaskIdentifier task_identifier;
          uint64_t task_time = lexical_cast<uint64_t>(vals[0]);
          task_identifier.job_id = lexical_cast<uint64_t>(vals[2]);
          task_identifier.task_index = lexical_cast<uint64_t>(vals[3]);
          uint64_t event_type = lexical_cast<uint64_t>(vals[5]);

          // We only run for the first FLAGS_runtime microseconds.
          if (FLAGS_runtime < task_time) {
            LOG(INFO) << "Terminating at : " << task_time;
            return;
          }

          if (task_time > time_interval_bound) {
            ProcessSimulatorEvents(time_interval_bound, machine_tmpl);
            ProcessTaskEvent(time_interval_bound, task_identifier, event_type);

            VLOG(2) << "Job id size: " << job_id_to_jd_.size();
            VLOG(2) << "Task id size: " << task_id_to_identifier_.size();
            VLOG(2) << "Job num tasks size: " << job_num_tasks_.size();
            VLOG(2) << "Job id to jd size: " << job_map_->size();
            VLOG(2) << "Task id to td size: " << task_map_->size();
            VLOG(2) << "Res id to rd size: " << resource_map_->size();
            VLOG(2) << "Task binding: " << task_bindings_.size();

            LOG(INFO) << "Scheduler run for time: " << time_interval_bound;

            multimap<uint64_t, uint64_t>* task_mappings =
              quincy_dispatcher_->Run();

            UpdateFlowGraph(time_interval_bound, task_runtime, task_mappings);

            delete task_mappings;

            // Update current time.
            while (time_interval_bound < task_time) {
              time_interval_bound += FLAGS_bin_time_duration;
            }
          }

          ProcessSimulatorEvents(task_time, machine_tmpl);
          ProcessTaskEvent(task_time, task_identifier, event_type);
        }
      }
    }
    fclose(f_task_events_ptr);
  }
  delete task_runtime;
}

void GoogleTraceSimulator::TaskCompleted(
    const TaskIdentifier& task_identifier) {
  TaskDescriptor** td_ptr = FindOrNull(task_id_to_td_, task_identifier);
  if (td_ptr == NULL) {
    LOG(ERROR) << "Could not find TaskDescriptor for: " << task_identifier.job_id
	       << " " << task_identifier.task_index;
    // TODO(ionel): This may have to update the state.
    return;
  }
  TaskID_t task_id = (*td_ptr)->uid();
  JobID_t job_id = JobIDFromString((*td_ptr)->job_id());
  // Remove the task node from the flow graph.
  flow_graph_->DeleteTaskNode(task_id);
  // Erase from local state: task_id_to_td_, task_id_to_identifier_, task_map_
  // and task_bindings_, task_id_to_end_time_, res_id_to_task_id_.
  task_id_to_td_.erase(task_identifier);
  ResourceID_t* res_id_ptr = FindOrNull(task_bindings_, task_id);
  CHECK_NOTNULL(res_id_ptr);
  ResourceID_t res_id_tmp = *res_id_ptr;
  res_id_to_task_id_.erase(res_id_tmp);
  task_bindings_.erase(task_id);
  task_map_->erase(task_id);
  task_id_to_identifier_.erase(task_id);
  task_id_to_end_time_.erase(task_id);
  uint64_t* num_tasks = FindOrNull(job_num_tasks_, task_identifier.job_id);
  CHECK_NOTNULL(num_tasks);
  (*num_tasks)--;
  if (*num_tasks == 0) {
    JobCompleted(task_identifier.job_id, job_id);
  }
}

EventDescriptor_EventType GoogleTraceSimulator::TranslateMachineEvent(
    int32_t machine_event) {
  if (machine_event == MACHINE_ADD) {
    return EventDescriptor::ADD_MACHINE;
  } else if (machine_event == MACHINE_REMOVE) {
    return EventDescriptor::REMOVE_MACHINE;
  } else if (machine_event == MACHINE_UPDATE) {
    return EventDescriptor::UPDATE_MACHINE;
  } else {
    LOG(FATAL) << "Unexpected machine event type: " << machine_event;
  }
}

void GoogleTraceSimulator::UpdateFlowGraph(
    uint64_t scheduling_timestamp,
    unordered_map<TaskIdentifier, uint64_t, TaskIdentifierHasher>* task_runtime,
    multimap<uint64_t, uint64_t>* task_mappings) {
  set<ResourceID_t> pus_used;
  for (multimap<uint64_t, uint64_t>::iterator it = task_mappings->begin();
       it != task_mappings->end(); ++it) {
    SchedulingDelta* delta = new SchedulingDelta();
    // Populate the scheduling delta.
    quincy_dispatcher_->NodeBindingToSchedulingDelta(
        *flow_graph_->Node(it->first), *flow_graph_->Node(it->second),
        &task_bindings_, delta);
    if (delta->type() == SchedulingDelta::NOOP) {
      // We don't have to do anything.
      delete delta;
      continue;
    } else if (delta->type() == SchedulingDelta::PLACE) {
      // Apply the scheduling delta.
      TaskID_t task_id = delta->task_id();
      ResourceID_t res_id = ResourceIDFromString(delta->resource_id());
      // Mark the task as scheduled
      flow_graph_->Node(it->first)->type_.set_type(
          FlowNodeType::SCHEDULED_TASK);
      TaskDescriptor** td = FindOrNull(*task_map_, task_id);
      ResourceStatus** rs = FindOrNull(*resource_map_, res_id);
      CHECK_NOTNULL(td);
      CHECK_NOTNULL(rs);
      ResourceDescriptor* rd = (*rs)->mutable_descriptor();
      rd->set_state(ResourceDescriptor::RESOURCE_BUSY);
      pus_used.insert(res_id);
      (*td)->set_state(TaskDescriptor::RUNNING);
      CHECK(InsertIfNotPresent(&task_bindings_, task_id, res_id));
      CHECK(InsertIfNotPresent(&res_id_to_task_id_, res_id, task_id));
      // After the task is bound, we now remove all of its edges into the flow
      // graph apart from the bound resource.
      // N.B.: This disables preemption and migration!
      flow_graph_->UpdateArcsForBoundTask(task_id, res_id);
      TaskIdentifier* task_identifier =
        FindOrNull(task_id_to_identifier_, task_id);
      CHECK_NOTNULL(task_identifier);
      AddTaskEndEvent(scheduling_timestamp, task_id, *task_identifier,
                      task_runtime);
    } else if (delta->type() == SchedulingDelta::MIGRATE) {
      // Apply the scheduling delta.
      TaskID_t task_id = delta->task_id();
      ResourceID_t res_id = ResourceIDFromString(delta->resource_id());
      // Mark the task as scheduled
      flow_graph_->Node(it->first)->type_.set_type(
          FlowNodeType::SCHEDULED_TASK);
      TaskDescriptor** td = FindOrNull(*task_map_, task_id);
      ResourceStatus** rs = FindOrNull(*resource_map_, res_id);
      CHECK_NOTNULL(td);
      CHECK_NOTNULL(rs);
      ResourceDescriptor* rd = (*rs)->mutable_descriptor();
      rd->set_state(ResourceDescriptor::RESOURCE_BUSY);
      pus_used.insert(res_id);
      (*td)->set_state(TaskDescriptor::RUNNING);
      ResourceID_t* old_res_id = FindOrNull(task_bindings_, task_id);
      CHECK_NOTNULL(old_res_id);
      if (pus_used.find(*old_res_id) == pus_used.end()) {
        // The resource is now idle.
        ResourceStatus** old_rs = FindOrNull(*resource_map_, *old_res_id);
        CHECK_NOTNULL(old_rs);
        ResourceDescriptor* old_rd = (*old_rs)->mutable_descriptor();
        old_rd->set_state(ResourceDescriptor::RESOURCE_IDLE);
        res_id_to_task_id_.erase(*old_res_id);
      }
      InsertOrUpdate(&task_bindings_, task_id, res_id);
      CHECK(InsertIfNotPresent(&res_id_to_task_id_, res_id, task_id));
    } else {
      LOG(FATAL) << "Unhandled scheduling delta case.";
    }
    delete delta;
  }
}

} // namespace sim
} // namespace firmament
