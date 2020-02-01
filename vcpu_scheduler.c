/*
William Emmanuel
CS-6210 Spring 2020
wemmanuel3@gatech.edu
*/

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdio.h>
#include <libvirt/libvirt.h>
#include <signal.h>
#include <unistd.h>

static int STABLE_PERCENT_TOLERANCE = 15;
static int DEBUG_MODE = 1;

/*
Helper struct which wraps a domain and tracks
information between intervals
*/
typedef struct DomainStat {
    // Pointer to the domain
    virDomainPtr ptr;

    // CPU utilization stats, stored as an integer 1-100
    int lastPercentUsed;
    int currentPercentUsed;
    unsigned long long int lastCpuTime;
    unsigned long long int cpuTimeDiff;
    // CPU information
    int pCpu;
    int vCpu;

} DomainStat;

/*
Print the current stats for a given VM domain
*/
void printDomainStat(DomainStat *domain) {
    unsigned int currDomainId = virDomainGetID(domain->ptr);
    const char *currDomainName = virDomainGetName(domain->ptr);
    printf("====Info====\n");
    printf("Domain ID: %i\n", currDomainId);
    printf("Domain Name: %s\n", currDomainName);
    printf("====CPU====\n");
    printf("cpu_time: %llu \n", domain->lastCpuTime);
    printf("lastPercentUsed: %d \n", domain->lastPercentUsed);
    printf("currentPercentUsed: %d \n", domain->currentPercentUsed);
    printf("====Pin====\n");
    printf("pCpu: %d\n", domain->pCpu);
    printf("vCpu: %d\n", domain->vCpu);
    printf("==========\n");
}

/*
Print the current pinnings
*/
void printCpuPinnings(DomainStat *stats, int numDomains, int numCpus) {
    for (int i = 0; i < numCpus; i++) {
        fprintf(stdout, "CPU: %d ", i);
        for (int j = 0; j < numDomains; j++) {
            if (stats[j].pCpu == i) {
                const char *name = virDomainGetName(stats[j].ptr);
                fprintf(stdout, "%s ", name);
            }
        }
        fprintf(stdout, "\n");
    }
    fprintf(stdout, "=====\n");
}

void populateDomainPinInfo(DomainStat *domain, int numPCpu) {
    virVcpuInfoPtr infoPtr = (virVcpuInfoPtr)malloc(sizeof(virVcpuInfo));
    /*
    Arguments:
     - domain pointer
     - virVcpuInfoPtr output pointer
     - number of output structs (always 1 vCpu, so 1)
     - CPU map; not used
     - maplen set to 0 when CPU map not provided
    */
	virDomainGetVcpus(domain->ptr, infoPtr, 1, NULL, 0);
    domain->pCpu = infoPtr[0].cpu;
    domain->vCpu = infoPtr[0].number;
    free(infoPtr);
}

void populateDomainStats(DomainStat *domain) {
    /*
    Arguments:
     - domain pointer
     - params output pointer; set to null to receive number of params
     - nparams; set to 0 if fetching nparams initially
     - start_cpu; -1 for summary
     - ncpus to query; always 1 vCpu
     - flags set to 0
    */
    int nParams = virDomainGetCPUStats(domain->ptr, NULL, 0, -1, 1, 0);
    domain->lastPercentUsed = domain->currentPercentUsed;
    virTypedParameterPtr cpuStats = calloc(nParams, sizeof(virTypedParameter));
    virDomainGetCPUStats(domain->ptr, cpuStats, nParams, -1, 1, 0);
    unsigned long long int cpuTime = virTypedParamsGet(cpuStats, nParams, "cpu_time")->value.ul;
    unsigned long long int cpuTimeDiff = cpuTime - domain->lastCpuTime;
    domain->cpuTimeDiff = cpuTimeDiff;
    free(cpuStats);
}

/*
Pin given domain to pCpu
*/
void pinDomainToPcpu(DomainStat *domain, int pCpu) {
    unsigned char *cpuMap = malloc (sizeof(unsigned char));
    *cpuMap = 1 << pCpu;
    /*
    Arguments:
    - domain pointer
    - virtual CPU number; always 0 since they are single-core
    - pointer to a bit map of real CPUs; each 1 means that pCPU is usable
    - cpuMap length; 1 char byte
    */
    virDomainPinVcpu(domain->ptr, 0, cpuMap, 1);
    free(cpuMap);
}

/*
Validate and parse arguments
*/
int getSchedulingInterval(int argc, char **argv) {
    if (argc < 2) {
        printf("Scheduling interval must be provided\n");
        exit(1);
    }
    int interval = atoi(argv[1]);
    if (interval <= 0) {
        printf("Invalid interval provided\n");
        exit(1);
    }
    return interval;
}

/*
Comparator for DomainStat struct used by qsort
*/
int compareDomain(const void *pa, const void *pb) {
    DomainStat* a = (DomainStat*)pa;
    DomainStat* b = (DomainStat*)pb;
    if (a->currentPercentUsed < b->currentPercentUsed) return -1;
    if (a->currentPercentUsed > b->currentPercentUsed) return 1;
    return 0;
}

/*
Given all domain stats and a pCpu, find total CPU time
*/
unsigned long long int cpuTotalTime(DomainStat *stats, int numDomains, int pCpu) {
    unsigned long long int totalTime = 0;
    for (int i = 0; i < numDomains; i++) {
        if (stats[i].pCpu == pCpu) totalTime += stats[i].cpuTimeDiff;
    }
    return totalTime;
}

/*
Given an int array, find smallest element
*/
int findSmallestIdx(int* array, int size) {
    int smallest = array[0];
    int smallestIdx = 0;
    for (int i = 0; i < size; i++) {
        if (array[i] < smallest) {
            smallest = array[i];
            smallestIdx = i;
        }
    }
    return smallestIdx;
}

/*
If all mappings are stable, return 1
*/
int areProcessorMappingsStable(DomainStat *stats, int numDomains) {
    for (int i = 0; i < numDomains; i++) {
        int diff = stats[i].lastPercentUsed - stats[i].currentPercentUsed;
        if (abs(diff) > STABLE_PERCENT_TOLERANCE) return 0;
    }
    return 1;
}

/*
Handler to catch end of process
*/
static volatile int shouldRun = 1;
void interruptHandler(int dummy) {
    shouldRun = 0;
}

int main(int argc, char **argv) {
    int interval = getSchedulingInterval(argc, argv);
    signal(SIGINT, interruptHandler);
    virDomainPtr *domains = NULL;
    virConnectPtr conn = virConnectOpen("qemu:///system");
    int numCpus = virNodeGetCPUMap(conn, NULL, NULL, 0);
    int numDomains = virConnectListAllDomains(conn, &domains,
                                              VIR_CONNECT_LIST_DOMAINS_ACTIVE);
    DomainStat *stats = calloc(numDomains, sizeof(DomainStat));

    while (shouldRun) {
        // Fetch CPU stats and pinnings for each domain
        for (int domainIdx = 0; domainIdx < numDomains; domainIdx++) {
            stats[domainIdx].ptr = domains[domainIdx];
            populateDomainStats(&stats[domainIdx]);
            populateDomainPinInfo(&stats[domainIdx], numCpus);
        }

        for (int domainIdx = 0; domainIdx < numDomains; domainIdx++) {
            unsigned long long int pCpuTotal = cpuTotalTime(stats, numDomains, stats[domainIdx].pCpu);
            double percentUsed = (double) stats[domainIdx].cpuTimeDiff * 100.0 / (double)pCpuTotal;
            // Calculate percentage each domain used of CPU time
            stats[domainIdx].currentPercentUsed = (int) percentUsed;
            if (DEBUG_MODE) { printDomainStat(&stats[domainIdx]); }
        }

        if (areProcessorMappingsStable(stats, numDomains)) {
            if (DEBUG_MODE) { printf("Processor mapping are stable\n"); };
        } else {
            if (DEBUG_MODE) { printf("Processor mapping are not stable; repinning\n"); }
            int *pCpuCapacity = (int*) malloc(numCpus * sizeof(int));
            for (int pCpu = 0; pCpu < numCpus; pCpu++) { pCpuCapacity[pCpu] = 0; }
            // Sort all domains by work percentage
            qsort((void*)stats, numDomains, sizeof(DomainStat), compareDomain);
            // Greedily assign domains to pCpu with least used capacity so far
            for (int domainIdx = 0; domainIdx < numDomains; domainIdx++) {
                int pCpuToUse = findSmallestIdx(pCpuCapacity, numCpus);
                pCpuCapacity[pCpuToUse] += stats[domainIdx].currentPercentUsed;
                // Call hypervisor to pin CPU
                pinDomainToPcpu(&stats[domainIdx], pCpuToUse);
            }
            free(pCpuCapacity);
        }

        if (DEBUG_MODE) { printCpuPinnings(stats, numDomains, numCpus); }
        if (DEBUG_MODE) { printf("Sleeping for %d\n", interval); }
        sleep(interval);
    }
    free(stats);
    free(domains);
    return 0;
}
