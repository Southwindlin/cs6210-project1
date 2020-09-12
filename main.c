#include <float.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libvirt/libvirt.h>

#define BIT_SET(a,b) ((a) |= (1ULL<<(b)))

int ONE_MILLION = 1000000;

int numOfDomains;
virDomainPtr* allDomains;
virConnectPtr hypervisor;
int numOfHostCpus;

/***
TODO: 

getting invalid domain stuff. refresh
allDomains on every run, make sure no memory leaks here or problems.

It looks like when there are empty pCPUs it's scheduling to them.

pCPU 0 has many domains binded to it. 
pCPU 1 and 2 have 3 total, and vCPUs keep switching between them. keeps finding
pCP 1 and 2 to be candidates, because 0 is full. but it's not choosing a vCPU
from 0, it's choosing the "smallest" one, which is on 1 or 2, not 0.


**/
int main() {
   
   while(1) {
      hypervisor = virConnectOpen(NULL);
      balanceCPU(hypervisor);
      for(int i = 0; i < numOfDomains; i++) virDomainFree(allDomains[i]);
      virConnectClose(hypervisor);
      sleep(1);
 }
   int isAlive = virConnectIsAlive(hypervisor); 
   return 0;
}

/** Find vCPU with smallest time on busiest pCPU **/
int determineVCpuToMove(virDomainPtr *domainToMove, int vCpuMappings[], double timeMappings[]) {
  // BUG: least utilized vCPU should NOT be moved if it has a dedicated pCPU!
  int candidatepCpu;
  int hostCpus[numOfHostCpus];
  for(int i = 0; i < numOfHostCpus; i++) hostCpus[i] = 0;
  virVcpuInfoPtr vCpu = calloc(1, sizeof(virVcpuInfoPtr));
  /** Can further optimize to choose the best pCPU to make room on. **/
  for(int i = 0; i < numOfDomains; i++) {
       virDomainGetVcpus(allDomains[i], vCpu, 1, NULL, 0);
       hostCpus[vCpu->cpu] = hostCpus[vCpu->cpu] + 1;
       vCpu = calloc(1, sizeof(virVcpuInfoPtr));
  }
  for(int i = 0; i < numOfHostCpus; i++ ) if (hostCpus[i] > 1) { candidatepCpu = i; break; }

  int smallestvCpu;
  virVcpuInfoPtr smallestVCpuInfo = calloc(1, sizeof(virVcpuInfoPtr));
  virDomainPtr smallestDomain;
  unsigned int smallestvCpuTime = INT_MAX;
  int busiestpCPU = -1;
  double busiestpCPUTime = 0;
  for(int i = 0; i < numOfHostCpus; i++) {
     if (timeMappings[i] > busiestpCPUTime) { busiestpCPUTime = timeMappings[i]; busiestpCPU = i; }
  }
  for(int i = 0; i < numOfDomains; i++) {
    virDomainGetVcpus(allDomains[i], smallestVCpuInfo, 1, NULL, 0);
    int time = (smallestVCpuInfo->cpuTime)/ONE_MILLION;
      if ( smallestVCpuInfo->cpu == busiestpCPU && time < smallestvCpuTime && vCpuMappings[smallestVCpuInfo->cpu] > 1) {
        smallestvCpuTime = time;
        smallestvCpu = smallestVCpuInfo->number;
        *domainToMove = allDomains[i];
      }
      smallestVCpuInfo = calloc(1, sizeof(virVcpuInfoPtr));
  }
  printf("\nMoving vCPU from domain %s off of pCPU %i...", virDomainGetName(*domainToMove), busiestpCPU);
  return smallestvCpu;
}


int balanceCPU(virConnectPtr hypervisor) {
   numOfDomains = virConnectNumOfDomains(hypervisor);
   printf("\nNumber of domains: %i", numOfDomains);
   printf("\nGetting pointers to all domains...");
   allDomains = malloc(numOfDomains * sizeof(virDomainPtr));
   int res = virConnectListAllDomains(hypervisor, &allDomains, VIR_CONNECT_LIST_DOMAINS_ACTIVE);
   

   balanceCpuIfNeeded(hypervisor, numOfDomains);
   


	return 0;
}

void pinvCpuTopCPU(int vCpu, int pCpu, virDomainPtr domain) {
    int maxInfo = 1;
    int mapLen = 1;
    unsigned char * cpuMap = calloc(maxInfo, mapLen);
    virVcpuInfoPtr vCpuInfo = calloc(1, sizeof(virVcpuInfoPtr));

    int vCpuRes = virDomainGetVcpus(domain, vCpuInfo, maxInfo, cpuMap, mapLen);
    int n = 8;
    cpuMap[0] = cpuMap[0] & 0;
    BIT_SET(cpuMap[0], pCpu);
    int res = virDomainPinVcpu(domain, vCpu, cpuMap, mapLen);
    printf("\nReturn code from virDomainPinVcpu: %i \n", res);
}

void balance(int vCpuMappings[],
             virVcpuInfoPtr allVCpuInfos[],
             double timeMappings[]) {
   int pCpuToPin = -1;
   printf("First attempting to find an empty pCPU to pin to...\n");
   for(int i = 0; i < numOfHostCpus; i++) {
   	if (vCpuMappings[i] == 0) {
   		pCpuToPin = i;
   		break;
   	}
   }

   if (pCpuToPin == -1) {
    printf("No empty pCPUs were available. Finding the least utilized one...\n");
   	double min = DBL_MAX;
   	int leastUtilizedpCpu = -1;
   	for(int i = 0; i < numOfHostCpus; i++) {
   		if (timeMappings[i] < min) { min = timeMappings[i]; leastUtilizedpCpu = i; }
   	}
    printf("Least utilized pCPU: %i\n", leastUtilizedpCpu);
    pCpuToPin = leastUtilizedpCpu;
   }
   printf("\npCPU to pin a vCPU to: %i", pCpuToPin);
   printf("\nFinding which vCPU to pin...");
  
   virDomainPtr domainToMove; // OUT ARG
   int vCpuToMove = determineVCpuToMove(&domainToMove, vCpuMappings, timeMappings);

   pinvCpuTopCPU(vCpuToMove, pCpuToPin, domainToMove);
}



int balanceCpuIfNeeded(virConnectPtr hypervisor, int numOfDomains) {
   virVcpuInfoPtr vCpuInfo = malloc(sizeof(virVcpuInfo));
   virVcpuInfoPtr allVCpuInfos[numOfDomains]; // assume 1 vCPU per domain.
   numOfHostCpus = virNodeGetCPUMap(hypervisor, NULL, NULL, 0);

   /** Variable timeMappings is a map of total vCPU CPU time for
       each pCPU. The index of this array is the number assigned
       to the pCPU. The contents of the array at each index are
       the cumulative CPU time across all domains for that pCPU.
       **/
   double timeMappings[numOfHostCpus]; // in ms
   memset(timeMappings, 0, numOfHostCpus*sizeof(double));

   /** Variable vCpuMappings is a map of total number of vCPUs
       currently pinned to each pCPU. The index of this array is 
       the number assigned to the pCPU. The contents of the array
       at each index are the number of vCPUs pinned to that pCPU. 
       **/
   int vCpuMappings[numOfHostCpus];
   memset(vCpuMappings, 0, numOfHostCpus*sizeof(int));

   for(int i = 0; i < numOfDomains; i++) {
   		int vCpuRes = virDomainGetVcpus(allDomains[i], vCpuInfo, 3, NULL, 0);
   		printf("\nDomain: %s, vCPU Number: %i, vCpu State: %i, vCpu CPU Time (ms): %llu, pCpu Number: %i\n", virDomainGetName(allDomains[i]), vCpuInfo->number, vCpuInfo->state, vCpuInfo->cpuTime / ONE_MILLION, vCpuInfo->cpu);
	    timeMappings[vCpuInfo->cpu] = timeMappings[vCpuInfo->cpu] + (vCpuInfo->cpuTime/ONE_MILLION);
	    vCpuMappings[vCpuInfo->cpu] = vCpuMappings[vCpuInfo->cpu] + 1;
      allVCpuInfos[i] = vCpuInfo;    
	}


     //If ANY pCPU has more than one vCPU AND there are empty pCPUs, unbalanced. 
  // If there are no empty CPUs, and a pCPU has > 1 vCPU, check time usage. 
    int emptyCpus = 0;
    int balanced = 1;
    int pCpuWithMoreThanOnevCpu = 0;

    for(int i = 0; i < numOfHostCpus; i++) if (vCpuMappings[i] == 0) emptyCpus = 1;
    for(int i = 0; i < numOfHostCpus; i++) {
      if (vCpuMappings[i] > 1) pCpuWithMoreThanOnevCpu = 1;
    	if (vCpuMappings[i] > 1 && emptyCpus == 1) balanced = 0;
	}
  if (balanced && emptyCpus == 0 && pCpuWithMoreThanOnevCpu == 1) {
    printf("\nThere are no empty pCPUs, and some pCPUs have more than one vCPU.\n");
    printf("\nFinding balance based on time usage...\n");
    unsigned int averageTime;
    // this shows that we're getting a pretty good estimate of pCPU usage here.
    for(int i = 0; i < numOfHostCpus; i++) printf("pCPU %i time usage: %f\n", i, timeMappings[i]);
    for(int i = 0; i < numOfHostCpus; i++) averageTime = averageTime + timeMappings[i];
    averageTime = averageTime / numOfHostCpus;
    printf("\nAverage time for pCPUs: %i", averageTime);
    double unbalancedPercent = 0.1;
    double threshold = averageTime + (averageTime*unbalancedPercent);
    for(int i = 0; i < numOfHostCpus; i++) if (timeMappings[i] > threshold) balanced = 0;
    printf("\nThreshold time value over which system is unbalanced: %f\n", threshold);
}
    printf("\nisBalanced: %i\n", balanced);
	if (!balanced) balance(vCpuMappings, allVCpuInfos, timeMappings);
	return 0;
}


void cpuStats() {
  int nparams = 1;
  int pcpunum = 0;
  virNodeCPUStatsPtr params = params = malloc(sizeof(virNodeCPUStats) * nparams);
  int res = virNodeGetCPUStats(hypervisor,0, params, &nparams,0);
  printf("\ncpuStats(): res: %i\nfield: %s\nvalue: %lli\n", res, params->field, params->value);
}


int balanceMemory(virConnectPtr hypervisor) {
    /***
        Create map of all guests. What their memory allocation is, 
        what % of it is being used in them. Be able to compare them
        all and choose how to balance physical memory allocation.
    ***/
/*
   int numOfDomains = virConnectNumOfDomains(hypervisor);
   printf("\nNumber of domains: %i", numOfDomains);
   printf("\nGetting pointers to all domains...");
   virDomainPtr* allDomains = malloc(numOfDomains * sizeof(virDomainPtr));

    unsigned long vMemRes = virDomainGetMaxMemory(allDomains[i]);

   		virDomainMemoryStatStruct stats[15];
   		int memStats = virDomainMemoryStats(allDomains[i], stats, 15, 0);
		printf("\nNumber of stats returned: %i", memStats);
        for(int i = 0; i < memStats; i++) {
        	printf("\nTag: %x\nVal: %lli", stats[i].tag, stats[i].val);
        }

*/
	return 0;
}
