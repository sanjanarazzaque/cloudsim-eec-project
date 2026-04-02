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
    // track when to decide to move machine to deep sleep 
    const unsigned IDLE_TO_S0I1_CHECKS = 1;
    const unsigned IDLE_TO_S1_CHECKS   = 3;
    const unsigned IDLE_TO_S3_CHECKS   = 6;
    const unsigned IDLE_TO_S5_CHECKS   = 10;


    struct VMRecord{
        bool migrating = false;
        //starting  w no vms until we get task
        VMId_t vm_id = NO_VALID_VM;
        VMType_t vm_type = LINUX;
        CPUType_t cpu = X86;
        MachineId_t machine_id = NO_VALID_MACHINE;
        unordered_set<TaskId_t> tasks;
    };

    unordered_map<VMId_t, VMRecord> map_vm_records;
    unordered_map<TaskId_t, VMId_t> map_task_to_vm;
    unordered_map<MachineId_t, vector<TaskId_t>> tasks_waiting_for_machine;
    unordered_set<MachineId_t> machines_waking;
    unordered_map<MachineId_t, unsigned> idle_checks_map;
    bool control_migration = false;


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
        if(vmr.cpu != RequiredCPUType(task_id)){
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

    bool checkVMForSLAs(const VMRecord& vmr){
         for (TaskId_t t : vmr.tasks) {
        SLAType_t sla = RequiredSLA(t);
        if (sla == SLA0 || sla == SLA1) {
            return true;
        }
    }
    return false;
    }

    bool checkVMForGPUTasks(const VMRecord& vmr){
        for(TaskId_t t: vmr.tasks){
            if(IsTaskGPUCapable(t)) return true;
        }
        return false;
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

    int machineSleepRank(MachineState_t s){
        switch(s){
            case S0: return 0;
            case S0i1: return 1;
            case S1: return 2;
            case S2: return 3;
            case S3: return 4;
            case S4: return 5;
            case S5: return 6;
            default: return 10;
        }
    }

    int rankMachineForNewVM(MachineId_t machine_id, TaskId_t task_id, bool awake_only) {
        MachineInfo_t machine_info = Machine_GetInfo(machine_id);

        if (machine_info.cpu != RequiredCPUType(task_id)) {
            return numeric_limits<int>::max();
        }

        if (awake_only && machine_info.s_state != S0) {
            return numeric_limits<int>::max();
        }

        if (!machineCheckMemory(machine_id, task_id, true)) {
            return numeric_limits<int>::max();
        }

        int rank = 0;
        rank += static_cast<int>(machine_info.active_tasks) * 20;
        rank += static_cast<int>(machine_info.active_vms) * 8;

        if(machine_info.s_state != S0){
            rank += 100 + machineSleepRank(machine_info.s_state) * 40;
        }

        if (IsTaskGPUCapable(task_id) && machine_info.gpus) {
            rank -= 25;
        } else if (IsTaskGPUCapable(task_id) && !machine_info.gpus) {
            rank += 10;
        }

        return rank;
    }

    MachineId_t findBestMachineForNewVM(TaskId_t task_id, bool awake_only) {
        MachineId_t best_machine = NO_VALID_MACHINE;
        int best_rank = numeric_limits<int>::max();

        unsigned total = Machine_GetTotal();
        for (unsigned i = 0; i < total; i++) {
            MachineId_t machine_id = static_cast<MachineId_t>(i);
            int rank = rankMachineForNewVM(machine_id, task_id, awake_only);
            if (rank < best_rank) {
                best_rank = rank;
                best_machine = machine_id;
            }
        }

        return best_machine;
    }

    bool checkIfCPUsAlign(MachineId_t machine_id, TaskId_t task_id){
        MachineInfo_t machine_info = Machine_GetInfo(machine_id);
        return machine_info.cpu == RequiredCPUType(task_id);
    }

    void setPStateOfAllCoresOnMachine(MachineId_t machine_id, CPUPerformance_t pstate) {
        MachineInfo_t machine_info = Machine_GetInfo(machine_id);
        for (unsigned core = 0; core < machine_info.num_cpus; core++) {
            Machine_SetCorePerformance(machine_id, core, pstate);
        }
    }

    void tryPlacingTaskOnVM(VMId_t vm_id, TaskId_t task_id) {
        auto vmr_iterator = map_vm_records.find(vm_id);
        if (vmr_iterator == map_vm_records.end()) {
            ThrowException("TryPlaceTaskOnVM(): can't find VM");
            return;
        }

        Priority_t sla_priority = findPriority(RequiredSLA(task_id));
        SetTaskPriority(task_id, sla_priority);
        VM_AddTask(vm_id, task_id, sla_priority);

        vmr_iterator->second.tasks.insert(task_id);
        map_task_to_vm[task_id] = vm_id;
        // machine isn't idle since we added a task 
        MachineId_t machine_id = vmr_iterator->second.machine_id;
        idle_checks_map[machine_id] = 0;
    }

    VMId_t attachVMAndPlaceTask(TaskId_t task_id, MachineId_t machine_id) {
        VMType_t vm_type = RequiredVMType(task_id);
        CPUType_t cpu = RequiredCPUType(task_id);
        //create new vm and then attach to specific machine (which is ranked and chosen)
        VMId_t vm_id = VM_Create(vm_type, cpu);
        VM_Attach(vm_id, machine_id);

        VMRecord rec;
        rec.vm_id = vm_id;
        rec.vm_type = vm_type;
        rec.cpu = cpu;
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

        MachineId_t awake_machine = findBestMachineForNewVM(task_id, true);
        if (awake_machine != NO_VALID_MACHINE) {
            attachVMAndPlaceTask(task_id, awake_machine);
            return true;
        }

        return false;

    }

    MachineId_t chooseMachineToWakeUp(TaskId_t task_id){
        MachineId_t best_machine = NO_VALID_MACHINE;
        int best_rank = numeric_limits<int>::max();

        unsigned total_mach = Machine_GetTotal();
        for(unsigned i = 0; i < total_mach; i++){
            MachineId_t machine_id = static_cast<MachineId_t>(i);
            //first check if task aligns w that machine
            if (!checkIfCPUsAlign(machine_id, task_id)) continue;
            //check if machine has space for that task
            if(!machineCheckMemory(machine_id, task_id, true)) continue;
            //skip machine if it is already waking up
            if(machines_waking.count(machine_id)) continue;
            //if its awake move on
            MachineInfo_t machine_info = Machine_GetInfo(machine_id);
            if(machine_info.s_state == S0) continue;
            int rank = rankMachineForNewVM(machine_id, task_id, false);
            if(rank < best_rank){
                best_rank = rank;
                best_machine = machine_id;
            }
        }

        return best_machine;
    }

    void storeTaskInMachineWaiting(TaskId_t task_id, MachineId_t machine_id){
        tasks_waiting_for_machine[machine_id].push_back(task_id);
        if(!machines_waking.count(machine_id)){
            machines_waking.insert(machine_id);
            Machine_SetState(machine_id, S0);
            SimOutput("storeTaskInMachineWaiting(): waking machine " + to_string(machine_id) +
                  " for task " + to_string(task_id), 2);

        }
        
    }

    bool canMigratingVMFitOnMachine(const VMRecord& vmr, MachineId_t dest){
        MachineInfo_t dst_mach = Machine_GetInfo(dest);
        unsigned mem_needed = VM_MEMORY_OVERHEAD + MEM_BUFFER;
        for (TaskId_t t : vmr.tasks) {
            mem_needed += GetTaskMemory(t);
        }

        if (dst_mach.memory_used > dst_mach.memory_size) return false;
        return (dst_mach.memory_size - dst_mach.memory_used) >= mem_needed;

    }

    bool canVMMigrate(const VMRecord& vmr, MachineId_t dest){
        MachineInfo_t dst_mach = Machine_GetInfo(dest);
        if (dst_mach.s_state != S0) return false;
        if (dst_mach.cpu != vmr.cpu) return false;
        if (dst_mach.machine_id == vmr.machine_id) return false;
        if (!canMigratingVMFitOnMachine(vmr, dest)) return false;
        return true;

    }

    void consolidateVMs(){
        if (control_migration) return;

        VMId_t best_vm = NO_VALID_VM;
        MachineId_t best_dst = NO_VALID_MACHINE;

        for (auto& kv : map_vm_records) {
            VMRecord& vmr = kv.second;
            if (vmr.tasks.empty()) continue;
            if (vmr.migrating) continue;

            MachineInfo_t src = Machine_GetInfo(vmr.machine_id);

            if (src.active_tasks > 1) continue;
            if (src.active_vms != 1) continue;
            if (checkVMForSLAs(vmr)) continue;

            for (unsigned i = 0; i < Machine_GetTotal(); i++) {
                MachineId_t dst_machine = static_cast<MachineId_t>(i);
                if (!canVMMigrate(vmr, dst_machine)) continue;

                MachineInfo_t dst = Machine_GetInfo(dst_machine);

                if (dst.active_tasks == 0) continue;

                if (checkVMForGPUTasks(vmr) && !dst.gpus) continue;

                best_vm = vmr.vm_id;
                best_dst = dst_machine;
                break;
            }

            if (best_vm != NO_VALID_VM) break;
        }

        if (best_vm != NO_VALID_VM && best_dst != NO_VALID_MACHINE) {
            map_vm_records[best_vm].migrating = true;
            control_migration = true;
            SimOutput("consolidateVMs(): migrating VM " + to_string(best_vm) +
                    " to machine " + to_string(best_dst), 2);
            VM_Migrate(best_vm, best_dst);
        }

    }


    void decideMachinePerformance(MachineId_t machine_id) {
        MachineInfo_t machine_info = Machine_GetInfo(machine_id);

        if (machine_info.s_state != S0 || machine_info.active_tasks == 0) {
            return;
        }

        bool needs_p0= false;
        for (auto& kv : map_vm_records) {
            const VMRecord& vmr = kv.second;
            if (vmr.machine_id != machine_id) 
                continue;
            if (checkVMForSLAs(vmr)) {
                needs_p0 = true;
                break;
            }
        }

        if (needs_p0 || machine_info.active_tasks >= machine_info.num_cpus) {
            setPStateOfAllCoresOnMachine(machine_id, P0);
        } else if (machine_info.active_tasks > 0) {
            setPStateOfAllCoresOnMachine(machine_id, P1);
        }
    }

    void removeFinishedTaskFromMap(TaskId_t task_id) {
        auto iter = map_task_to_vm.find(task_id);
        if (iter == map_task_to_vm.end()) return;

        VMId_t vm_id = iter->second;
        auto vm_iterator = map_vm_records.find(vm_id);
        if (vm_iterator != map_vm_records.end()) {
            vm_iterator->second.tasks.erase(task_id);
        }
        map_task_to_vm.erase(iter);
    }

    void shutdownEmptyVM(VMId_t vm_id) {
        auto iter = map_vm_records.find(vm_id);
        if (iter == map_vm_records.end()) 
            return;
        if (iter->second.migrating) 
            return;
        if (!iter->second.tasks.empty()) 
            return;
        VM_Shutdown(vm_id);
        MachineId_t machine_id = iter->second.machine_id;
        map_vm_records.erase(iter);
        idle_checks_map[machine_id] = 0;
    }

    void lowerIdleMachineState(MachineId_t machine_id) {
        if (machines_waking.count(machine_id)) 
            return;

        MachineInfo_t machine_info = Machine_GetInfo(machine_id);

        if (machine_info.active_vms > 0 || machine_info.active_tasks > 0) {
            idle_checks_map[machine_id] = 0;
            return;
        }

        unsigned idle_num = ++idle_checks_map[machine_id];

        if (idle_num >= IDLE_TO_S5_CHECKS && machine_info.s_state != S5) {
            Machine_SetState(machine_id, S5);
        } else if (idle_num >= IDLE_TO_S3_CHECKS && machine_info.s_state != S3 && machine_info.s_state != S4 && machine_info.s_state != S5) {
            Machine_SetState(machine_id, S3);
        } else if (idle_num >= IDLE_TO_S1_CHECKS && machine_info.s_state == S0i1) {
            Machine_SetState(machine_id, S1);
        } else if (idle_num >= IDLE_TO_S0I1_CHECKS && machine_info.s_state == S0) {
            Machine_SetState(machine_id, S0i1);
        }

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
    for(unsigned i = 0; i < Machine_GetTotal(); i++) {
        MachineId_t machine_id = static_cast<MachineId_t>(i);
        machines.push_back(machine_id);
        idle_checks_map[machine_id] = 0;

        MachineInfo_t machine_info = Machine_GetInfo(machine_id);
        if (machine_info.s_state == S0) {
            setPStateOfAllCoresOnMachine(machine_id, P1);
        }
    }

   
}
void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
     auto iter = map_vm_records.find(vm_id);
     if (iter != map_vm_records.end()) {
        iter->second.migrating = false;
        VMInfo_t machine_info = VM_GetInfo(vm_id);
        iter->second.machine_id = machine_info.machine_id;
    }

    control_migration = false;

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
    MachineId_t sleeping_machine = chooseMachineToWakeUp(task_id);
    if (sleeping_machine != NO_VALID_MACHINE) {
        storeTaskInMachineWaiting(task_id, sleeping_machine);
        return;
    }

    for (auto& kv : map_vm_records) {
        const VMRecord& vmr = kv.second;
        if (vmr.migrating) continue;
        if (vmr.vm_type != RequiredVMType(task_id)) continue;
        if (vmr.cpu != RequiredCPUType(task_id)) continue;
        if (!checkMachineReady(vmr.machine_id)) continue;

        tryPlacingTaskOnVM(vmr.vm_id, task_id);
        SimOutput("Scheduler::NewTask(): used last-resort placement for task " +
                  to_string(task_id), 1);
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
    for (unsigned i = 0; i < Machine_GetTotal(); i++) {
        MachineId_t machine_id = static_cast<MachineId_t>(i);
        decideMachinePerformance(machine_id);
        lowerIdleMachineState(machine_id);
    }

    consolidateVMs();

}

void Scheduler::Shutdown(Time_t time) {
    // Do your final reporting and bookkeeping here.
    // Report about the total energy consumed
    // Report about the SLA compliance
    // Shutdown everything to be tidy :-)
    vector<VMId_t> vm_ids;
    vm_ids.reserve(map_vm_records.size());
    for (auto& kv : map_vm_records) {
        vm_ids.push_back(kv.first);
    }

    for (VMId_t vm_id : vm_ids) {
        try {
            VM_Shutdown(vm_id);
        } catch (...) {
        }
    }

    SimOutput("SimulationComplete(): Finished!", 4);
    SimOutput("SimulationComplete(): Time is " + to_string(time), 4);
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    // Do any bookkeeping necessary for the data structures
    // Decide if a machine is to be turned off, slowed down, or VMs to be migrated according to your policy
    // This is an opportunity to make any adjustments to optimize performance/energy
    SimOutput("Scheduler::TaskComplete(): Task " + to_string(task_id) + " is complete at " + to_string(now), 4);

    auto iter = map_task_to_vm.find(task_id);
    if (iter == map_task_to_vm.end()) {
        return;
    }

    VMId_t vm_id = iter->second;

    try {
        VM_RemoveTask(vm_id, task_id);
    } catch (...) {
    }

    removeFinishedTaskFromMap(task_id);
    shutdownEmptyVM(vm_id);

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
    MachineInfo_t machine_info = Machine_GetInfo(machine_id);

    if (machine_info.s_state == S0) {
        setPStateOfAllCoresOnMachine(machine_id, P0);
    }

    for (unsigned i = 0; i < Machine_GetTotal(); i++) {
        MachineId_t candidate = static_cast<MachineId_t>(i);
        if (candidate == machine_id) continue;

        MachineInfo_t cinfo = Machine_GetInfo(candidate);
        if (cinfo.cpu != machine_info.cpu) continue;
        if (cinfo.s_state == S0) continue;
        if (machines_waking.count(candidate)) continue;

        machines_waking.insert(candidate);
        Machine_SetState(candidate, S0);
        SimOutput("MemoryWarning(): proactively waking machine " +
                  to_string(candidate), 2);
        break;
    }
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
    
    SchedulerInstance.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    SimOutput("SLAWarning(): the task " + to_string(task_id) +
    " warned at time: " + to_string(time), 1);
    SetTaskPriority(task_id, HIGH_PRIORITY);
    auto iter = map_task_to_vm.find(task_id);
    if (iter != map_task_to_vm.end()) {
        VMId_t vm_id = iter->second;
        auto vm_it = map_vm_records.find(vm_id);
        if (vm_it != map_vm_records.end()) {
            MachineId_t machine_id = vm_it->second.machine_id;
            if (machine_id != NO_VALID_MACHINE) {
                MachineInfo_t machine_info = Machine_GetInfo(machine_id);
                if (machine_info.s_state == S0) {
                    setPStateOfAllCoresOnMachine(machine_id, P0);
                }
            }
        }
    }

    CPUType_t cpu = RequiredCPUType(task_id);
    for (unsigned i = 0; i < Machine_GetTotal(); i++) {
        MachineId_t machine_id = static_cast<MachineId_t>(i);
        MachineInfo_t machine_info = Machine_GetInfo(machine_id);

        if (machine_info.cpu != cpu) continue;
        if (machine_info.s_state == S0) continue;
        if (machines_waking.count(machine_id)) continue;

        machines_waking.insert(machine_id);
        Machine_SetState(machine_id, S0);
        SimOutput("SLAWarning(): waking machine " + to_string(machine_id) +
                  " for SLA protection", 2);
        break;
    }


}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    // Called in response to an earlier request to change the state of a machine
    SimOutput("StateChangeComplete(): the machine " + to_string(machine_id) +
    " changed state at time: " + to_string(time), 3);

    machines_waking.erase(machine_id);
    idle_checks_map[machine_id] = 0;

    MachineInfo_t machine_info = Machine_GetInfo(machine_id);
    if (machine_info.s_state != S0) {
        return;
    }

    auto wait_iter = tasks_waiting_for_machine.find(machine_id);
    if (wait_iter == tasks_waiting_for_machine.end()) {
        return;
    }

    vector<TaskId_t> tasks_waiting = wait_iter->second;
    tasks_waiting_for_machine.erase(wait_iter);

    for (TaskId_t task_id : tasks_waiting) {
        SchedulerInstance.NewTask(time, task_id);
    }

}

