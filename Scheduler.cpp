//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#include "Scheduler.hpp"
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <limits>

//maybe delete
static unsigned active_machines = 16;

using namespace std;
namespace{
    const MachineId_t NO_VALID_MACHINE = numeric_limits<MachineId_t>::max();
    const VMId_t NO_VALID_VM = numeric_limits<VMId_t>::max();
    const unsigned MEM_BUFFER = 16;

    struct VMRecord{
        static bool migrating = false;
        //starting  w no vms until we get task
        VMId_t vm_id = NO_VALID_VM;
        VMType_t vm_type = LINUX;
        CPUType_t cpu_type = X86;
        MachineId_t machine_id = NO_VALID_MACHINE;
        unordered_set<TaskId_t> cur_tasks;
    };

    unordered_map<VMId_t, VMRecord> map_vm_records;
    unordered_map<TaskId_t, VMId_t> map_task_to_vm;

    Priority_t findPriority(SLAType_t sla){
        switch(sla){
            case SLA0:
            case SLA1:
                return HIGH_PRIORITY;
            case SLA2:
                return MID_PRIORITY;
            case SLA3:
            default:
                return LOW_PRIORITY;
        }
    }

    bool checkMachineReady(MachineId_t machine_id){
        MachineInfo_t machine_info = Machine_GetInfo(machine_id);
        if(machine_info.s_state == S0){
            return true;
        } else{
            return false;
        }
    }

    bool machineCheckMemory(MachineId_t machine_id, TaskId_t task_id, bool create_new_vm){
        MachineInfo_t machine_info = Machine_GetInfo(machine_id);
        unsigned mem_needed = GetTaskMemory(task_id) + MEM_BUFFER;
        if(create_new_vm){
            mem_needed += VM_MEMORY_OVERHEAD;
        }
        if(machine_info.memory_used > machine_info.memory_size){
            return false;
        }
         return (machine_info.memory_size - machine_info.memory_used) >= mem_needed;
    }

    bool canVMTakeTask(const VMRecord& vmr, TaskId_t task_id){
        if(vmr.migrating){
            return false;
        }
        if(vmr.cpu_type != RequiredCPUType(task_id)){
            return false;
        }
        if(vmr.vm_type != RequiredVMType(task_id)){
            return false;
        }
        if(vmr.machine_id == NO_VALID_MACHINE){
            return false;
        }
        if(!checkMachineReady(vmr.machine_id)){
            return false;
        }
        return machineCheckMemory(vmr.machine_id, task_id, false);
    }

    int rankVMForTask(const VMRecord& vmr, TaskId_t task_id){
        MachineInfo_t machine_info = Machine_GetInfo(vmr.machine_id);
        int rank = 0;
        //if machine has lots of active tasks, add penalty
        rank += static_cast<int>(machine_info.active_tasks) * 20;
        //if machine has active VMs this is good, bc we want to avoid creating new VMs
        rank -= static_cast<int>(machine_info.active_vms) * 5;

        if (IsTaskGPUCapable(task_id) && machine_info.gpus) {
            rank -= 20;
        } else if(IsTaskGPUCapable(task_id) && !machine_info.gpus){
            rank += 15;
        }

        unsigned free_memory = (machine_info.memory_size >= machine_info.memory_used) ? (machine_info.memory_size - machine_info.memory_used) : 0;
        if(free_memory > GetTaskMemory(task_id)){
            rank+= static_cast<int>((free_memory - GetTaskMemory(task_id)) / 32);
        }

        return rank;

    }

    VMId_t findBestVMForTask(TaskId_t task_id){
         VMId_t best_vm = NO_VALID_VM;
         int best_rank = numeric_limits<int>::max();

         for(auto& key : map_vm_records){
            const VMRecord& vmr = key.second;
            if(!canVMTakeTask(vmr, task_id)) continue;

            int rank = rankVMForTask(vmr, task_id);
            if(rank < best_rank){
                best_rank = rank;
                best_vm = vmr.vm_id;
            }
         }
        return best_vm;
    }

    int rankMachineForNewVM(MachineId_t machine_id, TaskId_t task_id) {
        MachineInfo_t machine_info = Machine_GetInfo(machine_id);

        if (machine_info.cpu != RequiredCPUType(task_id)) {
            return numeric_limits<int>::max();
        }

        if (machine_info.s_state != S0) {
            return numeric_limits<int>::max();
        }

        if (!machineCheckMemory(machine_id, task_id, true)) {
            return numeric_limits<int>::max();
        }

        int rank = 0;
        rank += static_cast<int>(machine_info.active_tasks) * 20;
        rank += static_cast<int>(machine_info.active_vms) * 8;

        if (IsTaskGPUCapable(task_id) && machine_info.gpus) {
            rank -= 25;
        } else if (IsTaskGPUCapable(task_id) && !machine_info.gpus) {
            rank += 10;
        }

        return rank;
    }

    MachineId_t findBestMachineForNewVM(TaskId_t task_id) {
        MachineId_t best_machine = NO_VALID_MACHINE;
        int best_rank = numeric_limits<int>::max();

        unsigned total = Machine_GetTotal();
        for (unsigned i = 0; i < total; i++) {
            MachineId_t machine_id = static_cast<MachineId_t>(i);
            int rank = rankMachineForNewVM(machine_id, task_id);
            if (rank < best_rank) {
                best_rank = rank;
                best_machine = machine_id;
            }
        }

        return best_machine;
    }

    void setPStateOfAllCoresOnMachine(MachineId_t machine_id, CPUPerformance_t pstate) {
        MachineInfo_t machine_info = Machine_GetInfo(machine_id);
        for (unsigned core = 0; core < machine_info.num_cpus; core++) {
            Machine_SetCorePerformance(machine_id, core, pstate);
        }
    }

    void tryPlacingTaskOnVM(VMId_t vm_id, TaskId_t task_id) {
    auto vmr_iterator = map_vm_records.find(vm_id);
    if (vmr_iterator == g_vm_records.end()) {
        ThrowException("TryPlaceTaskOnVM(): can't find VM");
        return;
    }

    Priority_t sla_priority = findPriority(RequiredSLA(task_id));
    SetTaskPriority(task_id, sla_priority);
    VM_AddTask(vm_id, task_id, sla_priority);

    vmr_iterator->second.tasks.insert(task_id);
    map_vm_records[task_id] = vm_id;
    }

    VMId_t attachVMAndPlaceTask(TaskId_t task_id, MachineId_t machine_id) {
        VMType_t vm_type = RequiredVMType(task_id);
        CPUType_t cpu_type = RequiredCPUType(task_id);
        //create new vm and then attach to specific machine (which is ranked and chosen)
        VMId_t vm_id = VM_Create(vm_type, cpu_type);
        VM_Attach(vm_id, machine_id);

        VMRecord rec;
        rec.vm_id = vm_id;
        rec.vm_type = vm_type;
        rec.cpu = cpu_type;
        rec.machine_id = machine_id;
        rec.migrating = false;

        map_vm_records[vm_id] = rec;
        tryPlacingTaskOnVM(vm_id, task_id);
        return vm_id;
    }

    bool trySchedulingTaskImmediately(TaskId_t task_id) {
        VMId_t existing_vm = findBestVMForTask(task_id);
        if (existing_vm != NO_VALID_VM) {
            tryPlacingTaskOnVM(existing_vm, task_id);
            return true;
        }

        MachineId_t awake_machine = findBestMachineForNewVM(task_id);
        if (awake_machine != NO_VALID_MACHINE) {
            attachVMAndPlaceTask(task_id, awake_machine);
            return true;
        }

        return false;

    }

}



void Scheduler::Init() {
    // Find the parameters of the clusters
    // Get the total number of machines
    // For each machine:
    //      Get the type of the machine
    //      Get the memory of the machine
    //      Get the number of CPUs
    //      Get if there is a GPU or not
    // 
    SimOutput("Scheduler::Init(): Total number of machines is " + to_string(Machine_GetTotal()), 3);
    SimOutput("Scheduler::Init(): no startup VMs, creating on-demand", 1);

    machines.clear();
    vms.clear();
    // looping through machines to set ID to index 
    for(unsigned i = 0; i < Machine_GetTotal; i++) {
        MachineId_t machine_id = static_cast<MachineId_t>(i);
        machines.push_back(machine_id);

        MachineInfo_t machine_info = Machine_GetInfo(machine_id);
        if (machine_info.s_state == S0) {
            setPStateOfAllCoresOnMachine(machine_id, P1);
        }
    }

   
}
void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    // Update your data structure. The VM now can receive new tasks
    SimOutput("Scheduler::MigrationComplete(): VM " + to_string(vm_id) +
              " completed migration at time " + to_string(time), 2);
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    // Get the task parameters
    //  IsGPUCapable(task_id);
    //  GetMemory(task_id);
    //  RequiredVMType(task_id);
    //  RequiredSLA(task_id);
    //  RequiredCPUType(task_id);
    // Decide to attach the task to an existing VM, 
    //      vm.AddTask(taskid, Priority_T priority); or
    // Create a new VM, attach the VM to a machine
    //      VM vm(type of the VM)
    //      vm.Attach(machine_id);
    //      vm.AddTask(taskid, Priority_t priority) or
    // Turn on a machine, create a new VM, attach it to the VM, then add the task
    //
    // Turn on a machine, migrate an existing VM from a loaded machine....
    //
    // Other possibilities as desired

    SimOutput("Scheduler::NewTask(): task " + to_string(task_id) +
              " arrived at " + to_string(now), 3);
    if (trySchedulingTaskImmediately(task_id)) {
        return;
    }
    ThrowException("Scheduler::NewTask(): no valid placement found for task ", task_id);
}
   

void Scheduler::PeriodicCheck(Time_t now) {
    // This method should be called from SchedulerCheck()
    // SchedulerCheck is called periodically by the simulator to allow you to monitor, make decisions, adjustments, etc.
    // Unlike the other invocations of the scheduler, this one doesn't report any specific event
    // Recommendation: Take advantage of this function to do some monitoring and adjustments as necessary
    SimOutput("Scheduler::PeriodicCheck(): time " + to_string(now), 4);
}

void Scheduler::Shutdown(Time_t time) {
    // Do your final reporting and bookkeeping here.
    // Report about the total energy consumed
    // Report about the SLA compliance
    // Shutdown everything to be tidy :-)
    SimOutput("SimulationComplete(): Finished!", 4);
    SimOutput("SimulationComplete(): Time is " + to_string(time), 4);
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    // Do any bookkeeping necessary for the data structures
    // Decide if a machine is to be turned off, slowed down, or VMs to be migrated according to your policy
    // This is an opportunity to make any adjustments to optimize performance/energy
    SimOutput("Scheduler::TaskComplete(): Task " + to_string(task_id) + " is complete at " + to_string(now), 4);
}

// Public interface below

static Scheduler SchedulerInstance;

void InitScheduler() {
    SimOutput("InitScheduler(): Initializing scheduler", 4);
    SchedulerInstance.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id) {
    SimOutput("HandleNewTask(): Received new task " + to_string(task_id) + " at time " + to_string(time), 4);
    SchedulerInstance.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id) {
    SimOutput("HandleTaskCompletion(): Task " + to_string(task_id) + " completed at time " + to_string(time), 4);
    SchedulerInstance.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    // The simulator is alerting you that machine identified by machine_id is overcommitted
    SimOutput("MemoryWarning(): Overflow at " + to_string(machine_id) + " was detected at time " + to_string(time), 0);
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    // The function is called on to alert you that migration is complete
    SimOutput("MigrationDone(): Migration of VM " + to_string(vm_id) + " was completed at time " + to_string(time), 4);
    SchedulerInstance.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    // This function is called periodically by the simulator, no specific event
    SimOutput("SchedulerCheck(): SchedulerCheck() called at " + to_string(time), 4);
    SchedulerInstance.PeriodicCheck(time);

}

void SimulationComplete(Time_t time) {
    // This function is called before the simulation terminates Add whatever you feel like.
    cout << "SLA violation report" << endl;
    cout << "SLA0: " << GetSLAReport(SLA0) << "%" << endl;
    cout << "SLA1: " << GetSLAReport(SLA1) << "%" << endl;
    cout << "SLA2: " << GetSLAReport(SLA2) << "%" << endl;     // SLA3 do not have SLA violation issues
    cout << "Total Energy " << Machine_GetClusterEnergy() << "KW-Hour" << endl;
    cout << "Simulation run finished in " << double(time)/1000000 << " seconds" << endl;
    SimOutput("SimulationComplete(): Simulation finished at time " + to_string(time), 4);
    
    Scheduler.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    // Called in response to an earlier request to change the state of a machine
}

