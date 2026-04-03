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
#include <deque>
#include <limits>
#include <string>

using namespace std;

namespace {

const MachineId_t NO_VALID_MACHINE = numeric_limits<MachineId_t>::max();
const VMId_t NO_VALID_VM = numeric_limits<VMId_t>::max();

const unsigned MEM_BUFFER = 16;

const unsigned IDLE_TO_S0I1_CHECKS = 4;
const unsigned IDLE_TO_S1_CHECKS   = 8;
const unsigned IDLE_TO_S3_CHECKS   = 14;
const unsigned IDLE_TO_S5_CHECKS   = 22;

const double CONSOLIDATE_LOW_UTIL  = 0.25;
const double CONSOLIDATE_HIGH_UTIL = 0.80;

enum AlgorithmKind {
    GREEDY_BEST_FIT = 0,
    SLA_AWARE = 1,
    GPU_FIRST = 2,
    THRESHOLD_CONSOLIDATION = 3
};

// change for the diff algos
const AlgorithmKind ACTIVE_ALGO = SLA_AWARE;

struct VMRecord {
    bool migrating = false;
    VMId_t vm_id = NO_VALID_VM;
    VMType_t vm_type = LINUX;
    CPUType_t cpu = X86;
    MachineId_t machine_id = NO_VALID_MACHINE;
    unordered_set<TaskId_t> tasks;
};

unordered_map<VMId_t, VMRecord> map_vm_records;
unordered_map<TaskId_t, VMId_t> map_task_to_vm;
unordered_map<MachineId_t, deque<TaskId_t>> tasks_waiting_for_machine;

deque<TaskId_t> urgent_waiting_tasks;
deque<TaskId_t> normal_waiting_tasks;

unordered_set<MachineId_t> machines_waking;
unordered_set<MachineId_t> machines_sleeping;
unordered_map<MachineId_t, unsigned> idle_checks_map;

bool control_migration = false;

string algoName() {
    switch (ACTIVE_ALGO) {
        case GREEDY_BEST_FIT: return "GREEDY_BEST_FIT";
        case SLA_AWARE: return "SLA_AWARE";
        case GPU_FIRST: return "GPU_FIRST";
        case THRESHOLD_CONSOLIDATION: return "THRESHOLD_CONSOLIDATION";
        default: return "UNKNOWN";
    }
}

Priority_t findPriority(SLAType_t sla) {
    switch (sla) {
        case SLA0: return HIGH_PRIORITY;
        case SLA1: return HIGH_PRIORITY;
        case SLA2: return MID_PRIORITY;
        case SLA3: default: return LOW_PRIORITY;
    }
}

bool isStrictSLA(SLAType_t sla) {
    return (sla == SLA0 || sla == SLA1);
}

bool isSLA0(TaskId_t task_id) {
    return RequiredSLA(task_id) == SLA0;
}

bool isMachineReadyNow(MachineId_t machine_id) {
    if (machines_waking.count(machine_id)) return false;
    if (machines_sleeping.count(machine_id)) return false;
    MachineInfo_t info = Machine_GetInfo(machine_id);
    return info.s_state == S0;
}

unsigned memoryNeededForTask(TaskId_t task_id, bool creating_new_vm) {
    unsigned need = GetTaskMemory(task_id) + MEM_BUFFER;
    if (creating_new_vm) need += VM_MEMORY_OVERHEAD;
    return need;
}

bool machineHasEnoughMemory(MachineId_t machine_id, TaskId_t task_id, bool creating_new_vm) {
    MachineInfo_t info = Machine_GetInfo(machine_id);
    if (info.memory_used > info.memory_size) return false;
    unsigned free_mem = info.memory_size - info.memory_used;
    return free_mem >= memoryNeededForTask(task_id, creating_new_vm);
}

unsigned vmTaskCount(const VMRecord& vmr) {
    return static_cast<unsigned>(vmr.tasks.size());
}

bool vmHasStrictTasks(const VMRecord& vmr) {
    for (TaskId_t t : vmr.tasks) {
        if (isStrictSLA(RequiredSLA(t))) return true;
    }
    return false;
}

bool vmHasOnlySLA0Tasks(const VMRecord& vmr) {
    if (vmr.tasks.empty()) return false;
    for (TaskId_t t : vmr.tasks) {
        if (RequiredSLA(t) != SLA0) return false;
    }
    return true;
}

bool machineHostsOnlySLA0Work(MachineId_t machine_id) {
    bool saw_task = false;
    for (auto& kv : map_vm_records) {
        const VMRecord& vmr = kv.second;
        if (vmr.machine_id != machine_id) continue;
        for (TaskId_t t : vmr.tasks) {
            saw_task = true;
            if (RequiredSLA(t) != SLA0) return false;
        }
    }
    return saw_task;
}

bool vmHasGpuTasks(const VMRecord& vmr) {
    for (TaskId_t t : vmr.tasks) {
        if (IsTaskGPUCapable(t)) return true;
    }
    return false;
}

void setPStateOfAllCoresOnMachine(MachineId_t machine_id, CPUPerformance_t pstate) {
    MachineInfo_t info = Machine_GetInfo(machine_id);
    for (unsigned core = 0; core < info.num_cpus; core++) {
        Machine_SetCorePerformance(machine_id, core, pstate);
    }
}

double machineUtilizationEstimate(MachineId_t machine_id) {
    MachineInfo_t info = Machine_GetInfo(machine_id);
    if (info.num_cpus == 0) return 0.0;
    return static_cast<double>(info.active_tasks) / static_cast<double>(info.num_cpus);
}

int sleepDepthRank(MachineState_t s) {
    switch (s) {
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

bool isFastMachineForStrict(const MachineInfo_t& m) {
    return m.gpus || m.num_cpus >= 16;
}

int machineSpeedBonus(const MachineInfo_t& m) {
    int bonus = 0;
    if (!m.performance.empty()) {
        bonus += static_cast<int>(m.performance[0] / 20);
    }
    bonus += static_cast<int>(m.num_cpus) * 6;
    if (m.gpus) bonus += 80;
    return bonus;
}

uint64_t p0Mips(MachineId_t machine_id) {
    MachineInfo_t info = Machine_GetInfo(machine_id);
    if (info.performance.empty()) return 0;
    return static_cast<uint64_t>(info.performance[0]);
}

unsigned bestCompatibleP0(TaskId_t task_id) {
    unsigned best = 0;
    CPUType_t cpu = RequiredCPUType(task_id);

    for (unsigned i = 0; i < Machine_GetTotal(); i++) {
        MachineId_t mid = static_cast<MachineId_t>(i);
        MachineInfo_t info = Machine_GetInfo(mid);
        if (info.cpu != cpu) continue;
        if (info.performance.empty()) continue;
        best = max(best, info.performance[0]);
    }
    return best;
}

unsigned bestCompatibleCores(TaskId_t task_id) {
    unsigned best = 0;
    CPUType_t cpu = RequiredCPUType(task_id);

    for (unsigned i = 0; i < Machine_GetTotal(); i++) {
        MachineId_t mid = static_cast<MachineId_t>(i);
        MachineInfo_t info = Machine_GetInfo(mid);
        if (info.cpu != cpu) continue;
        best = max(best, info.num_cpus);
    }
    return best;
}

bool canMeetStrictBestCase(MachineId_t machine_id, TaskId_t task_id) {
    if (!isStrictSLA(RequiredSLA(task_id))) return true;

    TaskInfo_t task_info = GetTaskInfo(task_id);
    MachineInfo_t machine_info = Machine_GetInfo(machine_id);

    if (machine_info.performance.empty()) return false;
    uint64_t mips = static_cast<uint64_t>(machine_info.performance[0]);
    if (mips == 0) return false;

    uint64_t budget_us = 0;
    if (task_info.target_completion > task_info.arrival) {
        budget_us = static_cast<uint64_t>(task_info.target_completion - task_info.arrival);
    }
    if (budget_us == 0) return false;

    uint64_t best_runtime_us =
        (static_cast<uint64_t>(task_info.total_instructions) + mips - 1) / mips;

    return best_runtime_us <= budget_us;
}

bool isCompetitiveStrictMachine(MachineId_t machine_id, TaskId_t task_id) {
    if (!isStrictSLA(RequiredSLA(task_id))) return true;

    MachineInfo_t info = Machine_GetInfo(machine_id);
    if (info.performance.empty()) return false;

    unsigned best_p0 = bestCompatibleP0(task_id);
    unsigned best_cores = bestCompatibleCores(task_id);

    if (best_p0 == 0 || best_cores == 0) return true;

    unsigned perf_pct = static_cast<unsigned>((100ULL * info.performance[0]) / best_p0);
    unsigned core_pct = static_cast<unsigned>((100ULL * info.num_cpus) / best_cores);

    if (isSLA0(task_id)) {
        return perf_pct >= 70 && core_pct >= 75;
    } else {
        return perf_pct >= 60 && core_pct >= 50;
    }
}

bool machineMatchesTaskBasic(MachineId_t machine_id, TaskId_t task_id) {
    MachineInfo_t info = Machine_GetInfo(machine_id);
    if (info.cpu != RequiredCPUType(task_id)) return false;
    if (IsTaskGPUCapable(task_id) && !info.gpus && ACTIVE_ALGO == GPU_FIRST) return false;

    if (ACTIVE_ALGO == SLA_AWARE && isStrictSLA(RequiredSLA(task_id))) {
        if (!canMeetStrictBestCase(machine_id, task_id)) return false;
        if (!isCompetitiveStrictMachine(machine_id, task_id)) return false;
    }

    return true;
}

bool vmCanTakeTask(const VMRecord& vmr, TaskId_t task_id) {
    if (vmr.migrating) return false;
    if (vmr.vm_id == NO_VALID_VM) return false;
    if (vmr.machine_id == NO_VALID_MACHINE) return false;
    if (vmr.cpu != RequiredCPUType(task_id)) return false;
    if (vmr.vm_type != RequiredVMType(task_id)) return false;
    if (!isMachineReadyNow(vmr.machine_id)) return false;
    if (!machineHasEnoughMemory(vmr.machine_id, task_id, false)) return false;

    if (ACTIVE_ALGO == SLA_AWARE && isStrictSLA(RequiredSLA(task_id))) {
        if (!canMeetStrictBestCase(vmr.machine_id, task_id)) return false;
        if (!isCompetitiveStrictMachine(vmr.machine_id, task_id)) return false;
    }

    if (ACTIVE_ALGO == SLA_AWARE) {
        SLAType_t sla = RequiredSLA(task_id);
        unsigned count = vmTaskCount(vmr);

        if (sla == SLA0) {
            if (count >= 2) return false;
            if (!vmr.tasks.empty() && !vmHasOnlySLA0Tasks(vmr)) return false;
        } else if (sla == SLA1) {
            if (count >= 2) return false;
        } else {
            if (vmHasOnlySLA0Tasks(vmr)) return false;
        }
    }

    return true;
}

int scoreExistingVM(const VMRecord& vmr, TaskId_t task_id) {
    MachineInfo_t m = Machine_GetInfo(vmr.machine_id);
    int score = 0;

    score += static_cast<int>(m.active_tasks) * 20;
    score -= static_cast<int>(m.active_vms) * 5;

    unsigned free_mem = (m.memory_size >= m.memory_used) ? (m.memory_size - m.memory_used) : 0;
    if (free_mem >= GetTaskMemory(task_id)) {
        score += static_cast<int>((free_mem - GetTaskMemory(task_id)) / 32);
    }

    if (ACTIVE_ALGO == SLA_AWARE) {
        SLAType_t sla = RequiredSLA(task_id);

        if (sla == SLA0) {
            if (!vmHasOnlySLA0Tasks(vmr)) score += 600;
            score += static_cast<int>(vmTaskCount(vmr)) * 260;
            score += static_cast<int>(m.active_tasks) * 60;
            score -= machineSpeedBonus(m);
            if (isFastMachineForStrict(m)) score -= 140;
        } else if (sla == SLA1) {
            score += static_cast<int>(vmTaskCount(vmr)) * 150;
            score += static_cast<int>(m.active_tasks) * 45;
            score -= machineSpeedBonus(m) / 2;
            if (m.active_tasks >= m.num_cpus / 2) score += 80;
        } else {
            if (vmHasOnlySLA0Tasks(vmr)) score += 500;
            score -= 10;
        }
    }

    if (ACTIVE_ALGO == GPU_FIRST) {
        if (IsTaskGPUCapable(task_id)) {
            if (m.gpus) score -= 40;
            else score += 1000;
        } else {
            if (m.gpus) score += 80;
        }
    }

    if (ACTIVE_ALGO == THRESHOLD_CONSOLIDATION) {
        double util = machineUtilizationEstimate(vmr.machine_id);
        if (util < 0.30) score -= 5;
        if (util >= 0.30 && util <= 0.70) score -= 20;
        if (util > 0.80) score += 50;
    }

    return score;
}

VMId_t findBestExistingVM(TaskId_t task_id) {
    VMId_t best = NO_VALID_VM;
    int best_score = numeric_limits<int>::max();

    for (auto& kv : map_vm_records) {
        const VMRecord& vmr = kv.second;
        if (!vmCanTakeTask(vmr, task_id)) continue;

        int score = scoreExistingVM(vmr, task_id);
        if (score < best_score) {
            best_score = score;
            best = vmr.vm_id;
        }
    }
    return best;
}

int scoreNewMachine(MachineId_t machine_id, TaskId_t task_id, bool ready_only) {
    MachineInfo_t m = Machine_GetInfo(machine_id);

    if (!machineMatchesTaskBasic(machine_id, task_id)) return numeric_limits<int>::max();
    if (ready_only && !isMachineReadyNow(machine_id)) return numeric_limits<int>::max();
    if (!machineHasEnoughMemory(machine_id, task_id, true)) return numeric_limits<int>::max();

    int score = 0;

    score += static_cast<int>(m.active_tasks) * 20;
    score += static_cast<int>(m.active_vms) * 10;

    if (!ready_only && m.s_state != S0) {
        score += 120 + 40 * sleepDepthRank(m.s_state);
    }

    switch (ACTIVE_ALGO) {
        case GREEDY_BEST_FIT: {
            unsigned free_mem = (m.memory_size >= m.memory_used) ? (m.memory_size - m.memory_used) : 0;
            if (free_mem >= memoryNeededForTask(task_id, true)) {
                score += static_cast<int>((free_mem - memoryNeededForTask(task_id, true)) / 16);
            }
            break;
        }
        case SLA_AWARE: {
            SLAType_t sla = RequiredSLA(task_id);
            if (sla == SLA0) {
                if (m.s_state != S0) score += 600;
                score += static_cast<int>(m.active_tasks) * 60;
                score += static_cast<int>(m.active_vms) * 30;
                score -= machineSpeedBonus(m);
                if (isFastMachineForStrict(m)) score -= 180;
                if (m.active_tasks == 0) score -= 140;
                if (m.active_tasks >= m.num_cpus / 2) score += 160;
                if (m.p_state != P0) score += 30;
            } else if (sla == SLA1) {
                if (m.s_state != S0) score += 250;
                score += static_cast<int>(m.active_tasks) * 40;
                score -= machineSpeedBonus(m) / 2;
                if (m.active_tasks == 0) score -= 60;
                if (m.active_tasks >= m.num_cpus / 2) score += 100;
                if (m.p_state != P0) score += 15;
            } else {
                if (machineHostsOnlySLA0Work(machine_id) && m.active_tasks > 0) score += 500;
                score -= 5;
            }
            break;
        }
        case GPU_FIRST: {
            if (IsTaskGPUCapable(task_id)) {
                if (m.gpus) score -= 60;
                else score += 1000;
            } else {
                if (m.gpus) score += 100;
            }
            break;
        }
        case THRESHOLD_CONSOLIDATION: {
            double util = machineUtilizationEstimate(machine_id);
            if (m.s_state != S0) score += 80;
            if (m.active_tasks == 0) score -= 15;
            if (util >= 0.30 && util <= 0.70) score -= 25;
            if (util > 0.80) score += 50;
            break;
        }
    }

    return score;
}

MachineId_t findBestMachineForNewVM(TaskId_t task_id, bool ready_only) {
    MachineId_t best = NO_VALID_MACHINE;
    int best_score = numeric_limits<int>::max();

    for (unsigned i = 0; i < Machine_GetTotal(); i++) {
        MachineId_t mid = static_cast<MachineId_t>(i);
        int score = scoreNewMachine(mid, task_id, ready_only);
        if (score < best_score) {
            best_score = score;
            best = mid;
        }
    }
    return best;
}

bool tryPlaceTaskOnVM(VMId_t vm_id, TaskId_t task_id) {
    auto it = map_vm_records.find(vm_id);
    if (it == map_vm_records.end()) return false;

    VMRecord& vmr = it->second;
    if (!vmCanTakeTask(vmr, task_id)) return false;

    Priority_t pr = findPriority(RequiredSLA(task_id));
    SetTaskPriority(task_id, pr);
    VM_AddTask(vm_id, task_id, pr);

    vmr.tasks.insert(task_id);
    map_task_to_vm[task_id] = vm_id;
    idle_checks_map[vmr.machine_id] = 0;

    if (ACTIVE_ALGO == SLA_AWARE) {
        if (isStrictSLA(RequiredSLA(task_id))) {
            setPStateOfAllCoresOnMachine(vmr.machine_id, P0);
        }
    }

    return true;
}

bool createAttachAndPlace(TaskId_t task_id, MachineId_t machine_id) {
    if (!isMachineReadyNow(machine_id)) return false;
    if (!machineMatchesTaskBasic(machine_id, task_id)) return false;
    if (!machineHasEnoughMemory(machine_id, task_id, true)) return false;

    VMType_t vm_type = RequiredVMType(task_id);
    CPUType_t cpu = RequiredCPUType(task_id);

    if (!isMachineReadyNow(machine_id)) return false;

    VMId_t vm_id = VM_Create(vm_type, cpu);

    if (!isMachineReadyNow(machine_id)) {
        return false;
    }

    try {
        VM_Attach(vm_id, machine_id);
    } catch (const runtime_error&) {
        return false;
    }

    VMRecord rec;
    rec.vm_id = vm_id;
    rec.vm_type = vm_type;
    rec.cpu = cpu;
    rec.machine_id = machine_id;
    rec.migrating = false;

    map_vm_records[vm_id] = rec;
    return tryPlaceTaskOnVM(vm_id, task_id);
}

void queueTaskForMachine(TaskId_t task_id, MachineId_t machine_id) {
    tasks_waiting_for_machine[machine_id].push_back(task_id);
    idle_checks_map[machine_id] = 0;

    if (!machines_waking.count(machine_id)) {
        machines_sleeping.erase(machine_id);
        machines_waking.insert(machine_id);
        Machine_SetState(machine_id, S0);
        SimOutput("queueTaskForMachine(): waking machine " + to_string(machine_id) +
                  " for task " + to_string(task_id), 2);
    }
}

MachineId_t findCompatibleWakingMachine(TaskId_t task_id) {
    for (unsigned i = 0; i < Machine_GetTotal(); i++) {
        MachineId_t mid = static_cast<MachineId_t>(i);
        if (!machines_waking.count(mid)) continue;
        if (!machineMatchesTaskBasic(mid, task_id)) continue;
        if (!machineHasEnoughMemory(mid, task_id, true)) continue;
        return mid;
    }
    return NO_VALID_MACHINE;
}

bool tryScheduleTaskNow(TaskId_t task_id) {
    VMId_t existing = findBestExistingVM(task_id);
    if (existing != NO_VALID_VM) {
        return tryPlaceTaskOnVM(existing, task_id);
    }

    MachineId_t ready_machine = findBestMachineForNewVM(task_id, true);
    if (ready_machine != NO_VALID_MACHINE) {
        if (createAttachAndPlace(task_id, ready_machine)) return true;
    }

    return false;
}

bool anyCompatibleMachineExists(TaskId_t task_id) {
    for (unsigned i = 0; i < Machine_GetTotal(); i++) {
        MachineId_t mid = static_cast<MachineId_t>(i);
        if (machineMatchesTaskBasic(mid, task_id)) return true;
    }
    return false;
}

void tryDrainMachineQueue(Time_t now, MachineId_t machine_id) {
    if (!isMachineReadyNow(machine_id)) return;

    auto it = tasks_waiting_for_machine.find(machine_id);
    if (it == tasks_waiting_for_machine.end()) return;

    auto& q = it->second;
    while (!q.empty()) {
        TaskId_t task_id = q.front();

        if (tryScheduleTaskNow(task_id)) {
            q.pop_front();
            continue;
        }

        if (createAttachAndPlace(task_id, machine_id)) {
            q.pop_front();
            continue;
        }

        break;
    }

    if (q.empty()) tasks_waiting_for_machine.erase(machine_id);
}

void tryDrainGlobalQueue(Time_t now) {
    size_t attempts = urgent_waiting_tasks.size();
    while (attempts-- > 0 && !urgent_waiting_tasks.empty()) {
        TaskId_t task_id = urgent_waiting_tasks.front();
        urgent_waiting_tasks.pop_front();

        if (tryScheduleTaskNow(task_id)) continue;

        MachineId_t sleeping = findBestMachineForNewVM(task_id, false);
        if (sleeping != NO_VALID_MACHINE && !isMachineReadyNow(sleeping)) {
            queueTaskForMachine(task_id, sleeping);
            continue;
        }

        MachineId_t waking = findCompatibleWakingMachine(task_id);
        if (waking != NO_VALID_MACHINE) {
            tasks_waiting_for_machine[waking].push_back(task_id);
            continue;
        }

        urgent_waiting_tasks.push_back(task_id);
    }

    attempts = normal_waiting_tasks.size();
    while (attempts-- > 0 && !normal_waiting_tasks.empty()) {
        TaskId_t task_id = normal_waiting_tasks.front();
        normal_waiting_tasks.pop_front();

        if (tryScheduleTaskNow(task_id)) continue;

        MachineId_t sleeping = findBestMachineForNewVM(task_id, false);
        if (sleeping != NO_VALID_MACHINE && !isMachineReadyNow(sleeping)) {
            queueTaskForMachine(task_id, sleeping);
            continue;
        }

        normal_waiting_tasks.push_back(task_id);
    }
}

void pushToWaitQueue(TaskId_t task_id) {
    if (isStrictSLA(RequiredSLA(task_id))) {
        urgent_waiting_tasks.push_back(task_id);
    } else {
        normal_waiting_tasks.push_back(task_id);
    }
}

void removeFinishedTaskFromMaps(TaskId_t task_id) {
    auto it = map_task_to_vm.find(task_id);
    if (it == map_task_to_vm.end()) return;

    VMId_t vm_id = it->second;
    auto vm_it = map_vm_records.find(vm_id);
    if (vm_it != map_vm_records.end()) {
        vm_it->second.tasks.erase(task_id);
    }
    map_task_to_vm.erase(it);
}

unsigned countReadyMachinesByCPU(CPUType_t cpu) {
    unsigned count = 0;
    for (unsigned i = 0; i < Machine_GetTotal(); i++) {
        MachineId_t mid = static_cast<MachineId_t>(i);
        MachineInfo_t info = Machine_GetInfo(mid);
        if (info.cpu == cpu && isMachineReadyNow(mid)) {
            count++;
        }
    }
    return count;
}

void shutdownEmptyVM(VMId_t vm_id) {
    auto it = map_vm_records.find(vm_id);
    if (it == map_vm_records.end()) return;
    if (it->second.migrating) return;
    if (!it->second.tasks.empty()) return;

    MachineId_t machine_id = it->second.machine_id;
    VM_Shutdown(vm_id);
    map_vm_records.erase(it);
    idle_checks_map[machine_id] = 0;
}

void decideMachinePerformance(MachineId_t machine_id) {
    MachineInfo_t m = Machine_GetInfo(machine_id);
    if (m.s_state != S0 || m.active_tasks == 0) return;

    bool strict_present = false;
    bool sla0_present = false;
    for (auto& kv : map_vm_records) {
        const VMRecord& vmr = kv.second;
        if (vmr.machine_id != machine_id) continue;
        if (vmHasStrictTasks(vmr)) strict_present = true;
        for (TaskId_t t : vmr.tasks) {
            if (isSLA0(t)) { sla0_present = true; break; }
        }
    }

    switch (ACTIVE_ALGO) {
        case GREEDY_BEST_FIT:
            if (m.active_tasks >= m.num_cpus) setPStateOfAllCoresOnMachine(machine_id, P0);
            else setPStateOfAllCoresOnMachine(machine_id, P1);
            break;

        case SLA_AWARE:
            if (strict_present) {
                setPStateOfAllCoresOnMachine(machine_id, P0);
            } else if (m.active_tasks >= m.num_cpus / 2) {
                setPStateOfAllCoresOnMachine(machine_id, P0);
            } else if (m.active_tasks > 0) {
                setPStateOfAllCoresOnMachine(machine_id, P1);
            }
            break;

        case GPU_FIRST:
            if (m.gpus && m.active_tasks > 0) setPStateOfAllCoresOnMachine(machine_id, P0);
            else if (m.active_tasks >= m.num_cpus) setPStateOfAllCoresOnMachine(machine_id, P0);
            else setPStateOfAllCoresOnMachine(machine_id, P1);
            break;

        case THRESHOLD_CONSOLIDATION: {
            double util = machineUtilizationEstimate(machine_id);
            if (strict_present || util >= 0.80) setPStateOfAllCoresOnMachine(machine_id, P0);
            else if (util >= 0.40) setPStateOfAllCoresOnMachine(machine_id, P1);
            else setPStateOfAllCoresOnMachine(machine_id, P2);
            break;
        }
    }
}

void lowerIdleMachineState(MachineId_t machine_id) {
    if (machines_waking.count(machine_id)) return;
    if (machines_sleeping.count(machine_id)) return;

    MachineInfo_t m = Machine_GetInfo(machine_id);

    unsigned min_ready_x86 = 2;
    if (ACTIVE_ALGO == SLA_AWARE)               min_ready_x86 = 16;
    else if (ACTIVE_ALGO == THRESHOLD_CONSOLIDATION) min_ready_x86 = 4;
    else if (ACTIVE_ALGO == GREEDY_BEST_FIT)    min_ready_x86 = 3;
    else if (ACTIVE_ALGO == GPU_FIRST)          min_ready_x86 = 3;

    if (m.cpu == X86 && countReadyMachinesByCPU(X86) <= min_ready_x86) {
        return;
    }

    if (m.cpu == X86 && !urgent_waiting_tasks.empty()) {
        for (TaskId_t t : urgent_waiting_tasks) {
            if (RequiredCPUType(t) == X86) return;
        }
    }

    if (m.active_vms > 0 || m.active_tasks > 0) {
        idle_checks_map[machine_id] = 0;
        return;
    }

    unsigned idle_num = ++idle_checks_map[machine_id];

    MachineState_t target = m.s_state;
    switch (ACTIVE_ALGO) {
        case GREEDY_BEST_FIT:
        case GPU_FIRST:
            if (idle_num >= IDLE_TO_S5_CHECKS && m.s_state != S5) target = S5;
            else if (idle_num >= IDLE_TO_S3_CHECKS && m.s_state != S3 && m.s_state != S4 && m.s_state != S5) target = S3;
            else if (idle_num >= IDLE_TO_S1_CHECKS && m.s_state == S0i1) target = S1;
            else if (idle_num >= IDLE_TO_S0I1_CHECKS && m.s_state == S0) target = S0i1;
            break;

        case SLA_AWARE:
            if (m.cpu == X86) {
                return;
            } else {
                if (idle_num >= IDLE_TO_S3_CHECKS && m.s_state != S3 && m.s_state != S4 && m.s_state != S5) target = S3;
                else if (idle_num >= IDLE_TO_S1_CHECKS && m.s_state == S0i1) target = S1;
                else if (idle_num >= IDLE_TO_S0I1_CHECKS && m.s_state == S0) target = S0i1;
            }
            break;

        case THRESHOLD_CONSOLIDATION:
            if (idle_num >= IDLE_TO_S3_CHECKS && m.s_state != S3 && m.s_state != S4 && m.s_state != S5) target = S3;
            else if (idle_num >= IDLE_TO_S1_CHECKS && m.s_state == S0i1) target = S1;
            else if (idle_num >= IDLE_TO_S0I1_CHECKS && m.s_state == S0) target = S0i1;
            break;
    }

    if (target != m.s_state) {
        machines_waking.erase(machine_id);
        machines_sleeping.insert(machine_id);
        Machine_SetState(machine_id, target);
    }
}

bool canMigrateVMToMachine(const VMRecord& vmr, MachineId_t dst_machine) {
    if (vmr.migrating) return false;
    if (!isMachineReadyNow(dst_machine)) return false;

    MachineInfo_t dst = Machine_GetInfo(dst_machine);
    if (dst.machine_id == vmr.machine_id) return false;
    if (dst.cpu != vmr.cpu) return false;

    unsigned need = VM_MEMORY_OVERHEAD + MEM_BUFFER;
    for (TaskId_t t : vmr.tasks) need += GetTaskMemory(t);

    if (dst.memory_used > dst.memory_size) return false;
    return (dst.memory_size - dst.memory_used) >= need;
}

void tryConsolidateVMs() {
    return;
}

} // namespace

void Scheduler::Init() {
    SimOutput("Scheduler::Init(): algorithm = " + algoName(), 1);
    SimOutput("Scheduler::Init(): Total number of machines is " + to_string(Machine_GetTotal()), 2);

    machines.clear();
    vms.clear();

    for (unsigned i = 0; i < Machine_GetTotal(); i++) {
        MachineId_t machine_id = static_cast<MachineId_t>(i);
        machines.push_back(machine_id);
        idle_checks_map[machine_id] = 0;

        MachineInfo_t info = Machine_GetInfo(machine_id);
        if (info.s_state == S0) {
            if (ACTIVE_ALGO == SLA_AWARE && info.cpu == X86) {
                setPStateOfAllCoresOnMachine(machine_id, P0);
            } else if (ACTIVE_ALGO == THRESHOLD_CONSOLIDATION) {
                setPStateOfAllCoresOnMachine(machine_id, P2);
            } else {
                setPStateOfAllCoresOnMachine(machine_id, P1);
            }
        }
    }
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    auto it = map_vm_records.find(vm_id);
    if (it != map_vm_records.end()) {
        it->second.migrating = false;
        VMInfo_t info = VM_GetInfo(vm_id);
        it->second.machine_id = info.machine_id;
        idle_checks_map[info.machine_id] = 0;
    }

    control_migration = false;
    SimOutput("Scheduler::MigrationComplete(): VM " + to_string(vm_id) +
              " completed migration at time " + to_string(time), 2);
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    SimOutput("Scheduler::NewTask(): task " + to_string(task_id) +
              " arrived at " + to_string(now), 3);

    if (tryScheduleTaskNow(task_id)) return;

    if (isSLA0(task_id)) {
        MachineId_t sleeping_candidate = findBestMachineForNewVM(task_id, false);
        if (sleeping_candidate != NO_VALID_MACHINE && !isMachineReadyNow(sleeping_candidate)) {
            queueTaskForMachine(task_id, sleeping_candidate);
            return;
        }

        MachineId_t waking_machine = findCompatibleWakingMachine(task_id);
        if (waking_machine != NO_VALID_MACHINE) {
            tasks_waiting_for_machine[waking_machine].push_back(task_id);
            return;
        }
        if (anyCompatibleMachineExists(task_id)) {
            urgent_waiting_tasks.push_back(task_id);
            SimOutput("Scheduler::NewTask(): urgent queue fallback for SLA0 task " + to_string(task_id), 2);
            return;
        }
    } else {
        MachineId_t sleeping_candidate = findBestMachineForNewVM(task_id, false);
        if (sleeping_candidate != NO_VALID_MACHINE && !isMachineReadyNow(sleeping_candidate)) {
            queueTaskForMachine(task_id, sleeping_candidate);
            return;
        }

        MachineId_t waking_machine = findCompatibleWakingMachine(task_id);
        if (waking_machine != NO_VALID_MACHINE) {
            tasks_waiting_for_machine[waking_machine].push_back(task_id);
            return;
        }

        if (anyCompatibleMachineExists(task_id)) {
            pushToWaitQueue(task_id);
            SimOutput("Scheduler::NewTask(): global queue fallback for task " + to_string(task_id), 2);
            return;
        }
    }

    ThrowException("Scheduler::NewTask(): no compatible machine type exists for task ", task_id);
}

void Scheduler::PeriodicCheck(Time_t now) {
    SimOutput("Scheduler::PeriodicCheck(): time " + to_string(now), 4);

    for (unsigned i = 0; i < Machine_GetTotal(); i++) {
        MachineId_t machine_id = static_cast<MachineId_t>(i);
        decideMachinePerformance(machine_id);
        lowerIdleMachineState(machine_id);
    }

    tryConsolidateVMs();
    tryDrainGlobalQueue(now);
}

void Scheduler::Shutdown(Time_t time) {
    vector<VMId_t> ids;
    ids.reserve(map_vm_records.size());
    for (auto& kv : map_vm_records) ids.push_back(kv.first);

    for (VMId_t vm_id : ids) {
        try { VM_Shutdown(vm_id); } catch (...) {}
    }

    SimOutput("SimulationComplete(): Finished with algorithm " + algoName(), 1);
    SimOutput("SimulationComplete(): Time is " + to_string(time), 3);
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    SimOutput("Scheduler::TaskComplete(): Task " + to_string(task_id) +
              " complete at " + to_string(now), 4);

    auto it = map_task_to_vm.find(task_id);
    if (it == map_task_to_vm.end()) return;

    VMId_t vm_id = it->second;
    try { VM_RemoveTask(vm_id, task_id); } catch (...) {}

    removeFinishedTaskFromMaps(task_id);
    shutdownEmptyVM(vm_id);

    auto vm_it = map_vm_records.find(vm_id);
    if (vm_it != map_vm_records.end()) {
        idle_checks_map[vm_it->second.machine_id] = 0;
    }

    tryDrainGlobalQueue(now);
}

static Scheduler SchedulerInstance;

void InitScheduler() {
    SimOutput("InitScheduler(): Initializing scheduler", 4);
    SchedulerInstance.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id) {
    SimOutput("HandleNewTask(): Received new task " + to_string(task_id) +
              " at time " + to_string(time), 4);
    SchedulerInstance.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id) {
    SimOutput("HandleTaskCompletion(): Task " + to_string(task_id) +
              " completed at time " + to_string(time), 4);
    SchedulerInstance.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    SimOutput("MemoryWarning(): Overflow at " + to_string(machine_id) +
              " detected at time " + to_string(time), 0);

    MachineInfo_t info = Machine_GetInfo(machine_id);
    if (info.s_state == S0) {
        setPStateOfAllCoresOnMachine(machine_id, P0);
    }

    unsigned woken = 0;
    for (unsigned i = 0; i < Machine_GetTotal(); i++) {
        MachineId_t candidate = static_cast<MachineId_t>(i);
        if (candidate == machine_id) continue;

        MachineInfo_t cinfo = Machine_GetInfo(candidate);
        if (cinfo.cpu != info.cpu) continue;
        if (machines_waking.count(candidate)) continue;
        if (cinfo.s_state == S0) continue;

        machines_sleeping.erase(candidate);
        machines_waking.insert(candidate);
        Machine_SetState(candidate, S0);

        SimOutput("MemoryWarning(): proactively waking machine " +
                  to_string(candidate), 2);

        woken++;
        if (info.cpu == X86) {
            if (woken >= 2) break;
        } else {
            if (woken >= 1) break;
        }
    }
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    SimOutput("MigrationDone(): Migration of VM " + to_string(vm_id) +
              " completed at time " + to_string(time), 4);
    SchedulerInstance.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    SimOutput("SchedulerCheck(): called at " + to_string(time), 4);
    SchedulerInstance.PeriodicCheck(time);
}

void SimulationComplete(Time_t time) {
    cout << "Algorithm: " << algoName() << endl;
    cout << "SLA violation report" << endl;
    cout << "SLA0: " << GetSLAReport(SLA0) << "%" << endl;
    cout << "SLA1: " << GetSLAReport(SLA1) << "%" << endl;
    cout << "SLA2: " << GetSLAReport(SLA2) << "%" << endl;
    cout << "Total Energy " << Machine_GetClusterEnergy() << "KW-Hour" << endl;
    cout << "Simulation run finished in " << double(time) / 1000000 << " seconds" << endl;

    SimOutput("SimulationComplete(): Simulation finished at time " + to_string(time), 4);
    SchedulerInstance.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    SimOutput("SLAWarning(): task " + to_string(task_id) +
              " warned at time " + to_string(time), 1);

    SetTaskPriority(task_id, HIGH_PRIORITY);

    auto it = map_task_to_vm.find(task_id);
    if (it != map_task_to_vm.end()) {
        VMId_t vm_id = it->second;
        auto vm_it = map_vm_records.find(vm_id);
        if (vm_it != map_vm_records.end()) {
            MachineId_t machine_id = vm_it->second.machine_id;
            if (machine_id != NO_VALID_MACHINE) {
                MachineInfo_t info = Machine_GetInfo(machine_id);
                if (info.s_state == S0 && !machines_waking.count(machine_id) && !machines_sleeping.count(machine_id)) {
                    setPStateOfAllCoresOnMachine(machine_id, P0);
                } else if (!machines_waking.count(machine_id)) {
                    machines_sleeping.erase(machine_id);
                    machines_waking.insert(machine_id);
                    Machine_SetState(machine_id, S0);
                }
            }
        }
    }

    if (ACTIVE_ALGO == SLA_AWARE || ACTIVE_ALGO == THRESHOLD_CONSOLIDATION) {
        CPUType_t cpu = RequiredCPUType(task_id);
        unsigned woken = 0;

        for (unsigned i = 0; i < Machine_GetTotal(); i++) {
            MachineId_t machine_id = static_cast<MachineId_t>(i);
            MachineInfo_t info = Machine_GetInfo(machine_id);

            if (info.cpu != cpu) continue;
            if (machines_waking.count(machine_id)) continue;
            if (info.s_state == S0) continue;

            if (ACTIVE_ALGO == SLA_AWARE && isStrictSLA(RequiredSLA(task_id)) &&
                !isCompetitiveStrictMachine(machine_id, task_id)) {
                continue;
            }

            machines_sleeping.erase(machine_id);
            machines_waking.insert(machine_id);
            Machine_SetState(machine_id, S0);

            SimOutput("SLAWarning(): waking machine " + to_string(machine_id) +
                      " for SLA protection", 2);

            woken++;

            if (isStrictSLA(RequiredSLA(task_id))) {
                if (woken >= 3) break;
            } else {
                if (woken >= 1) break;
            }
        }
    }
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    SimOutput("StateChangeComplete(): machine " + to_string(machine_id) +
              " changed state at time " + to_string(time), 3);

    MachineInfo_t info = Machine_GetInfo(machine_id);

    machines_waking.erase(machine_id);
    idle_checks_map[machine_id] = 0;

    if (info.s_state == S0) {
        machines_sleeping.erase(machine_id);
        auto it = tasks_waiting_for_machine.find(machine_id);
        if (it != tasks_waiting_for_machine.end()) {
            for (TaskId_t t : it->second) {
                if (isStrictSLA(RequiredSLA(t))) {
                    setPStateOfAllCoresOnMachine(machine_id, P0);
                    break;
                }
            }
        }
        tryDrainMachineQueue(time, machine_id);
        tryDrainGlobalQueue(time);
    } else {
        machines_sleeping.insert(machine_id);
    }
}