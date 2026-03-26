machine class:
{
 #general x86 servers to handle normal work
 Number of machines: 10
 CPU type: X86
 Number of cores: 8
 Memory: 16384
 S-States: [125, 105, 95, 75, 45, 12, 0]
 P-States: [14, 10, 7, 5]
 C-States: [14, 4, 1, 0]
 MIPS: [1100, 850, 650, 450]
 GPUs: no
}

machine class:
{
 #GPU x86 servers for AI tasks
 Number of machines: 3
 CPU type: X86
 Number of cores: 16
 Memory: 49152
 S-States: [180, 150, 130, 95, 55, 15, 0]
 P-States: [20, 15, 11, 8]
 C-States: [20, 5, 2, 0]
 MIPS: [1800, 1400, 1000, 700]
 GPUs: yes
}

machine class:
{
 #ARM servers for low-priority background work
 Number of machines: 6
 CPU type: ARM
 Number of cores: 8
 Memory: 16384
 S-States: [90, 72, 60, 45, 25, 8, 0]
 P-States: [9, 7, 5, 3]
 C-States: [9, 3, 1, 0]
 MIPS: [800, 620, 450, 300]
 GPUs: no
}

task class:
{
 #normal background web traffic
 Start time: 0
 End time: 1200000
 Inter arrival: 6000
 Expected runtime: 400000
 Memory: 192
 VM type: LINUX
 GPU enabled: no
 SLA type: SLA0
 CPU type: X86
 Task type: WEB
 Seed: 100001
}

task class:
{
 #interactive traffic spike
 Start time: 250000
 End time: 850000
 Inter arrival: 3500
 Expected runtime: 500000
 Memory: 384
 VM type: LINUX_RT
 GPU enabled: no
 SLA type: SLA1
 CPU type: X86
 Task type: STREAM
 Seed: 100002
}

task class:
{
 #AI/ML jobs for GPU servers
 Start time: 300000
 End time: 1000000
 Inter arrival: 18000
 Expected runtime: 2200000
 Memory: 1024
 VM type: LINUX
 GPU enabled: yes
 SLA type: SLA2
 CPU type: X86
 Task type: AI
 Seed: 100003
}

task class:
{
 #long low-priority background jobs
 Start time: 100000
 End time: 1100000
 Inter arrival: 45000
 Expected runtime: 5000000
 Memory: 2048
 VM type: LINUX
 GPU enabled: no
 SLA type: SLA3
 CPU type: ARM
 Task type: HPC
 Seed: 100004
}
