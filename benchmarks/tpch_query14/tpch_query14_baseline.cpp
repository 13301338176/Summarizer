#include "nvme_tpch_query14.h"

extern exec_time_t acc_et_io, acc_et_proc;
extern unsigned int proc_delay;

void tpch_query14_baseline(char *date, int fd, void *buffer, unsigned int pageCount1, unsigned int pageCount2, double & promo_revenue, double & revenue)
{
	int err;
	struct nvme_user_io io;
	char *l_shipdate = (char *)calloc(9,1);
	double *l_discount = (double *)calloc(1,sizeof(double));
	double *l_quantitiy = (double *)calloc(1,sizeof(double));
	double *l_extendedprice = (double *)calloc(1,sizeof(double));
	unsigned int *l_partkey = (unsigned int *) calloc(1, sizeof(unsigned int));
	unsigned long long int *p_partkey = (unsigned long long int *) calloc(1, sizeof(unsigned long long int));

	char *p_type;
	
	std::unordered_map<unsigned long long int, char *> hashtable;

	int offset2=0;
	
	io.opcode = nvme_cmd_read;
	io.flags = 0;
	io.control = 0;
	io.metadata = (unsigned long) 0;
	io.addr = (unsigned long) buffer;
	io.slba = LBA_BASE;
	io.nblocks = 0;
	io.dsmgmt = 0;
	io.reftag = 0;
	io.apptag = 0;
	io.appmask = 0;

	// measure time
	exec_time_t et_io, et_proc;
	char et_name[64];
	strcpy(et_name, "io");
	init_exec_time(&et_io, et_name);
	strcpy(et_name, "proc");
	init_exec_time(&et_proc, et_name);

	for(unsigned int i2 = pageCount1; i2 < pageCount2; i2+=NUMBER_OF_PAGES_IN_A_COMMAND)
	{
		unsigned int pageInThisIteration2 = std::min((unsigned int)NUMBER_OF_PAGES_IN_A_COMMAND, (pageCount2-i2));
		io.slba = LBA_BASE + i2;
		io.nblocks = pageInThisIteration2-1;

		set_start_time(&et_io);
		err = ioctl(fd, NVME_IOCTL_SUBMIT_IO, &io);
		add_exec_time(&et_io);
		if (err)
			fprintf(stderr, "nvme read status:%x\n", err);

		for(unsigned int j2 = 0; j2 < pageInThisIteration2; j2++)
		{
			set_start_time(&et_proc);
			if (proc_delay > 0)
				usleep(proc_delay);
			while(1)
			{
				memcpy((char *)p_partkey, (char *)(((char *)buffer)+4096*j2+offset2+0), 8);
				p_type = (char *)calloc(1,6);
//				std::string p_type((((char *)buffer)+4096*j2+offset2+98) ,(((char *)buffer)+4096*j2+offset2+103));
				memcpy(p_type, (char *)(((char *)buffer)+4096*j2+offset2+99), 5);
				hashtable.emplace(*p_partkey, p_type);

				offset2 += 168;
				if((offset2 + 168) >= 4096)
				{
					offset2 = 0;
					break;
				}
			}
			add_exec_time(&et_proc);
		}
	}
	int offset = 0;
	for(unsigned int i=0; i < pageCount1; i += NUMBER_OF_PAGES_IN_A_COMMAND)
	{
		unsigned int pageInThisIteration = std::min((unsigned int)NUMBER_OF_PAGES_IN_A_COMMAND, (pageCount1-i));
		io.slba = LBA_BASE + i;
		io.nblocks = pageInThisIteration-1;

		set_start_time(&et_io);
		err = ioctl(fd, NVME_IOCTL_SUBMIT_IO, &io);
		add_exec_time(&et_io);
		if (err)
			fprintf(stderr, "nvme read status:%x\n", err);

		for(unsigned int j = 0; j < pageInThisIteration; j++)
		{
			set_start_time(&et_proc);
			if (proc_delay > 0)
				usleep(proc_delay);
			
			while(1)
			{
				memcpy((char *)l_shipdate, (char *)(((char *)buffer)+4096*j+offset+50), 8);

				if(((compareDates(l_shipdate, date, 0) == 1) or (strcmp(l_shipdate, date) == 0)) and ((compareDates(date, l_shipdate, 1) == 1)))
				{
					memcpy((char *)l_partkey, (char *)(((char *)buffer)+4096*j+offset+4), 4);
					memcpy((char *)l_discount, (char *)(((char *)buffer)+4096*j+offset+32), 8);
					memcpy((char *)l_extendedprice, (char *)(((char *)buffer)+4096*j+offset+24), 8); 
					std::unordered_map<unsigned long long int, char *>::iterator it;
					it = hashtable.find(*l_partkey);
					if(it != hashtable.end())
					{
						if(strlen(it->second) >= 5)
						{
							if(strncmp(it->second, "PROMO",5) == 0)
							{
								promo_revenue += *l_extendedprice * (1 - *l_discount);
							}
						}
						revenue += *l_extendedprice * (1 - *l_discount);
					}
				}

				offset += 153;
				if((offset + 153) >= (4096) )
				{
					offset = 0;
					break;
				}
			}
			add_exec_time(&et_proc);
		}
	}

	for ( auto local_it = hashtable.begin(); local_it != hashtable.end(); ++local_it )
		free(local_it->second);
	hashtable.clear();

	print_exec_time(&et_io);
	print_exec_time(&et_proc);
	accumulate_exec_time(&acc_et_io, &et_io);
	accumulate_exec_time(&acc_et_proc, &et_proc);

	free(l_shipdate);
	free(l_discount);
	free(l_extendedprice);
	free(l_quantitiy);
	free(l_partkey);
	free(p_partkey);
}
