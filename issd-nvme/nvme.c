#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>

#include <syslog.h>

#include <sys/stat.h>        /* For mode constants */
#include <mqueue.h>

#include "nvme.h"
#include "common.h"
#include "reset_reg.h"
#include "i2c_dimm.h"

// gunjae: strings for print
const char *NvmeIoCommandsName[] = {
	"FLUSH",	// 0
	"WRITE",	// 1
	"READ",		// 2
	"XXX",		// 3
	"WRITE_UNCOR",	// 4
	"COMPARE",	// 5
	"XXX",	// 6
	"XXX",	// 7
	"WRITE_ZERO",	// 8
	"DSM"	// 9
};

//Kiran:
#include <inttypes.h>
#include <unistd.h>
#include <stdbool.h>

//#define DEBUG
extern void *io_completer (void *arg);
extern void *df_io_completer (void *arg);
#if ((GK_TEST_RD_NAND==3) || (GK_TEST_RD_NAND==4))
extern void *df_io_rd_completer(void *arg);
#endif
#if (GK_TEST_RD_NAND==4)
extern void *df_inproc_completer(void *arg);
#endif
extern struct bdbm_drv_info* _bdi;

extern uint8_t nvmeKilled;
NvmeCtrl g_NvmeCtrl;

static void nvme_process_reg (NvmeCtrl *n, uint64_t offset, uint64_t data);
static void nvme_process_db (NvmeCtrl *n, uint64_t offset, uint64_t data);
static uint16_t nvme_init_cq (NvmeCQ *cq, NvmeCtrl *n, uint64_t dma_addr, uint16_t cqid, uint16_t vector, uint16_t size, uint16_t irq_enabled, int contig);
static int nvme_start_ctrl (NvmeCtrl *n);
static uint16_t nvme_init_sq (NvmeSQ *sq, NvmeCtrl *n, uint64_t dma_addr, uint16_t sqid, uint16_t cqid, uint16_t size, enum NvmeQFlags prio, int contig);

static inline void nvme_free_sq (NvmeSQ *sq, NvmeCtrl *n)
{
	n->sq[sq->sqid] = NULL;
	FREE_VALID (sq->io_req);
	FREE_VALID (sq->req_list);
	FREE_VALID (sq->out_req_list);
	FREE_VALID (sq->prp_list);

	if (sq->dma_addr) {
#if (CUR_SETUP != TARGET_SETUP && CUR_SETUP != TARGET_RD_SETUP)
		int cnt = (!sq->sqid)?2:16;
		munmap ((void *)sq->dma_addr, cnt * getpagesize ());
#endif
		sq->dma_addr = 0;
	}
	SAFE_CLOSE (sq->fd_qmem);
	if (sq->sqid) {
		FREE_VALID (sq);
	}
}

static inline void nvme_free_cq (NvmeCQ *cq, NvmeCtrl *n)
{
	n->cq[cq->cqid] = NULL;
	if (cq->prp_list) {
		FREE_VALID (cq->prp_list);
	}
	if (cq->dma_addr) {
#if (CUR_SETUP != TARGET_SETUP && CUR_SETUP != TARGET_RD_SETUP)
		int cnt = (!cq->cqid)?2:16;
		munmap ((void *)cq->dma_addr, cnt*getpagesize ());
#endif
		cq->dma_addr = 0;
	}
	SAFE_CLOSE (cq->fd_qmem);
	if (cq->cqid) {
		FREE_VALID (cq);
	}
}

static inline void nvme_addr_read (NvmeCtrl *n, uint64_t addr, void *buf, int size)
{
	if (n->cmb && addr >= n->ctrl_mem.addr && \
			addr < (n->ctrl_mem.addr + n->ctrl_mem.size)) {
		memcpy (buf, (void *)&n->cmbuf[addr - n->ctrl_mem.addr], size);
	} else {
		/*pci_dma_read (&n->parent_obj, addr, buf, size);*/
		memcpy (buf, (void *)addr, size);
	}
}

static inline void nvme_addr_write (NvmeCtrl *n, uint64_t addr, void *buf, int size)
{
	if (n->cmb && addr >= n->ctrl_mem.addr && \
			addr < (n->ctrl_mem.addr + n->ctrl_mem.size)) {
		memcpy ((void *)&n->cmbuf[addr - n->ctrl_mem.addr], buf, size);
		return;
	} else {
		/*pci_dma_write (&n->parent_obj, addr, buf, size);*/
		memcpy ((void *)addr , buf, size);
	}
}

static inline int nvme_check_sqid (NvmeCtrl *n, uint16_t sqid)
{
	return sqid < n->num_queues && n->sq[sqid] != NULL ? 0 : -1;
}

static inline int nvme_check_cqid (NvmeCtrl *n, uint16_t cqid)
{
	return cqid < n->num_queues && n->cq[cqid] != NULL ? 0 : -1;
}

static inline void nvme_inc_cq_tail (NvmeCQ *cq)
{
	cq->tail++;
	if (cq->tail >= cq->size) {
		cq->tail = 0;
		cq->phase = !cq->phase;
	}
}

static inline int nvme_cqes_pending (NvmeCQ *cq)
{
	return cq->tail > cq->head ?
		cq->head + (cq->size - cq->tail) :
		cq->head - cq->tail;
}

static inline void nvme_inc_sq_head (NvmeSQ *sq)
{
	sq->head = (sq->head + 1) % sq->size;
}

static inline void nvme_update_cq_head (NvmeCQ *cq)
{
	if (cq->db_addr) {
		nvme_addr_read (cq->ctrl, cq->db_addr, &cq->head, sizeof (cq->head));
	}
}

static inline uint8_t nvme_cq_full (NvmeCQ *cq)
{
	nvme_update_cq_head (cq);
	return (cq->tail + 1) % cq->size == cq->head;
}

static inline uint8_t nvme_sq_empty (NvmeSQ *sq)
{
	return sq->head == sq->tail;
}

#if (CUR_SETUP != TARGET_SETUP && CUR_SETUP != TARGET_RD_SETUP)
uint32_t *irq_ctrl;
#endif
static void nvme_isr_notify (void *opaque)
{
	NvmeCQ *cq = opaque;
	NvmeCtrl *n = cq->ctrl;
#if 1
#if (CUR_SETUP == STANDALONE_SETUP)	
	static int nvme_irq_fd = 0;
	char buf[4];
	if(nvme_irq_fd <= 0){
		nvme_irq_fd = open("/dev/nvme0", O_RDWR);
		if(nvme_irq_fd < 0){
			perror("open nvme irq");
		}
	}
#endif
#endif

	if (cq->irq_enabled) {
#if (CUR_SETUP == TARGET_SETUP || CUR_SETUP == TARGET_RD_SETUP)
		uint32_t vector_addr;
		uint16_t *data_addr;
		if ((*(uint16_t *)(n->host.msix.msix_en_ptr)) & 0x8000) {
			*(uint32_t *)(n->host.msix.addr_ptr) = cq->vector;
		} else if ((*(uint16_t *)(n->host.msi.msi_en_ptr)) & 0x1) {
			/*Multiple MSIs to be tested yet -TODO*/
			//data_addr = n->host.msix.data_ptr + cq->vector;
			data_addr = n->host.msi.data_ptr;
			vector_addr = *(uint32_t *)(n->host.msi.addr_ptr);
			*(vector_addr + (char *)n->host.io_mem.addr) = *data_addr;
		} else {
			/*Raise INTX?? -TODO*/
			/*pci_irq_pulse (&n->parent_obj);*/
			return;
		}
		//*(vector_addr + (char *)n->host.io_mem.addr) = *data_addr;
#elif (CUR_SETUP == STANDALONE_SETUP)
#if 1
		sprintf(buf, "%d", cq->cqid);
		if(nvme_irq_fd > 0){
			write(nvme_irq_fd, buf, strlen(buf)+1);	
		}else{
			printf("/dev/nvmen0 node is not opened to write irq number: %d\n", cq->vector);	
		}
#else
		*(uint16_t *)(irq_ctrl + 0x04) = 0x0000;
		*(uint16_t *)(irq_ctrl + 0x04) = 0x0002;	
#endif
#else
		*(uint16_t *)(irq_ctrl + 0x04) = 0x0000;
		*(uint16_t *)(irq_ctrl + 0x04) = 0x0002;
		*(uint16_t *)(irq_ctrl + 0x04) = 0x0000;
#endif
	}
}

#if(KM_TEST_RD_NAND==1)
bool gc_phy_addr_freePages[1024];//Declare it as global
extern unsigned long long int gc_phy_addr[70];
extern uint8_t *ls2_virt_addr[5];

/*uint8_t gk_virt_map()//Call this only once
{
	uint32_t i;
	int fd = open("/dev/mem",O_RDWR);
	if (fd < 0) {
		perror("open:");
		return -1;
	}
	for (i = 0; i < 70; i++) {
		gk_virt_addr[i] = (uint8_t*)mmap(0, 1024*getpagesize(), PROT_READ|PROT_WRITE, MAP_SHARED, fd, gc_phy_addr[i]);
		if (gk_virt_addr[i] == (uint8_t *)MAP_FAILED) {
			perror("gk_virt_map:");
			close(fd);
			return -1;
		}
		GK_PRINT("GK: gk_virt_addr[%u] 0x%llX is mapped to gc_phy_addr:0x%llX\n", i, gk_virt_addr[i], gc_phy_addr[i]);
	}
	return 0;
}*/
#endif

static uint16_t nvme_map_prp (NvmeRequest *req, uint64_t prp1, uint64_t prp2, uint32_t len, NvmeCtrl *n)
{
#if(KM_TEST_RD_NAND==1)
        //Kiran
        uint32_t temp_originalLen = len;
#endif

#ifdef STATIC_BIO
	nvme_bio *bio = &req->bio;
#else
	nvme_bio *bio = NULL;
#endif
	uint64_t trans_len = n->page_size - (prp1 % n->page_size);
	/*uint64_t nprps = (len >> n->page_bits) + 1;*/

	if (!prp1) {
		return NVME_INVALID_FIELD | NVME_DNR;
	}

#ifdef STATIC_BIO
	bio->nprps = bio->size = bio->nlb =0;
#else
	bio = nvme_bio_create (nprps);
	if (!bio) {
		goto unmap;
	}
#endif
	trans_len = MIN(len, trans_len);
	if (nvme_add_prp (bio, prp1, trans_len)) {
		goto unmap;
	}

	len -= trans_len;
	if (len) {
		if (!prp2) {
			goto unmap;
		}
		if (len > n->page_size) {
			uint64_t prp[n->max_prp_ents];
			uint32_t nents, prp_trans;
			int i = 0;

//			memset(&prp[0], 0, sizeof(prp));
			nents = (len + n->page_size - 1) >> n->page_bits;
			prp_trans = MIN (n->max_prp_ents, nents) * sizeof(uint64_t);
			prp_read_write (n->host.io_mem.addr, prp2, &prp[0], prp_trans);

			while (len != 0) {
				uint64_t prp_ent = prp[i];

				if (!prp_ent || prp_ent & (n->page_size - 1)) {
					goto unmap;
				}

				if (i == n->max_prp_ents - 1 && len > n->page_size) {

					i = 0;
	//				memset(&prp[0], 0, sizeof(prp));
					nents = (len + n->page_size - 1) >> n->page_bits;
					prp_trans = MIN(n->max_prp_ents, nents) * sizeof(uint64_t);
					prp_read_write (n->host.io_mem.addr, prp_ent, &prp[0], prp_trans);
					prp_ent = prp[i];
					if (!prp_ent || prp_ent & (n->page_size - 1)) {
						goto unmap;
					}
				}

				trans_len = MIN(len, n->page_size);
				if (nvme_add_prp (bio, prp_ent, trans_len)) {
					goto unmap;
				}
				len -= trans_len;
				i++;
			}
		} else {
			if (prp2 & (n->page_size - 1)) {
				goto unmap;
			}
			if (nvme_add_prp (bio, prp2, len)) {
				goto unmap;
			}
		}
		//GK_REQ_PRINT("prp[0x%lX]->", bio->prp[0]);
	}
#if (CUR_SETUP != STANDALONE_SETUP)
#if (CUR_SETUP != TARGET_SETUP && CUR_SETUP != TARGET_RD_SETUP)
	bio->fd_prp_mmap = nvme_bio_mmap_prps (bio);
	if (bio->fd_prp_mmap <= 0) {
		syslog(LOG_ERR,"nvme_bio_mmap_prps fail: %x\n", bio->fd_prp_mmap);
		goto unmap;
	}
#else
	// gunjae: this will be called
	nvme_bio_mmap_prps (bio);
	GK_REQ_PRINT("->prp[0x%lX]", bio->prp[0]);
#endif
#endif

//Kiran:
#if (KM_TEST_RD_NAND==1)
#ifdef STATIC_BIO
        if(req->is_write == 0)
        {       
		syslog(LOG_INFO, "ModifyingForCommand id is %d\n", req->cqe.cid);
                syslog(LOG_INFO, "Kiran: n->page_size = %llu bio->nlb = %llu bio->nprps = %u getpagesize() = %d KERNEL_PAGE_SIZE = %d\n",n->page_size, bio->nlb, bio->nprps, getpagesize(), KERNEL_PAGE_SIZE );

		syslog(LOG_INFO, "Kiran: No. of prps is %u Length of transfer is %u\n", bio->nprps, temp_originalLen);

		int i=0;
		for(i=0; i < bio->nprps; i++)
		{
			syslog(LOG_INFO, "Kiran: Original prp address is %llx\n", bio->prp[i]);
			bio->prp_temp[i] = bio->prp[i];

			int j=0;
			int gc_phy_addr_index = -1;
			for(j=0; j < 1024; j++)
			{
				if(gc_phy_addr_freePages[j] == 0)
				{
					gc_phy_addr_index = j;
					break;
				}
			}
			if(gc_phy_addr_index == -1)
			{
				syslog(LOG_INFO, "Out of pages in gc_phy_addr space\n");
			}
			gc_phy_addr_freePages[gc_phy_addr_index] = 1;
			syslog(LOG_INFO, "gc_phy_addr free index = %d\n", gc_phy_addr_index);

			uint8_t *lmem_phy_base;
			uint8_t *lmem_phy;
			lmem_phy_base = (uint8_t *)gc_phy_addr[2];
			lmem_phy = lmem_phy_base + getpagesize() * gc_phy_addr_index;
			bio->prp[i] = (uint64_t)lmem_phy;

			syslog(LOG_INFO, "LS2 gc_phy_addr is %llx\n", bio->prp[i]);

			uint8_t *lmem_virt_base;
			lmem_virt_base = (uint8_t *)ls2_virt_addr[2];
			bio->prp_ls2_virtual[i] = (uint64_t)(lmem_virt_base + getpagesize() * gc_phy_addr_index);

			syslog(LOG_INFO, "LS2 lmem_virt_addr is %llx\n", bio->prp_ls2_virtual[i]);

			bio->prp_phy_addr_index[i] = gc_phy_addr_index;

//			syslog(LOG_INFO, "Kiran: New prp address is %llu size of malloc is %d page size is %d\n", bio->prp[i], getpagesize(), n->page_size);
//			syslog(LOG_INFO, "Kiran: New prp address after adjustment is %llu\n", bio->prp[i]);
		}
		bio->temp_size = bio->size;
	}
/*      if(req->is_write == 0)
        {
                //      realloc(bio->prp_temp, bio->nprps * sizeof(uint64_t));
                int i=0;
                syslog(LOG_INFO, "Kiran: No. of prps is %u Length of transfer is %u\n", bio->nprps, temp_originalLen);
                for(i=0; i < bio->nprps; i++)
                {
                        syslog(LOG_INFO, "Kiran: Original prp address is %llu\n", bio->prp[i]);
                        bio->prp_temp[i] = bio->prp[i];
                        bio->prp[i] = (uint64_t)malloc(getpagesize());
                        syslog(LOG_INFO, "Kiran: New prp address is %llu size of malloc is %d page size is %d\n", bio->prp[i], getpagesize(), n->page_size);
                        bio->prp[i] = bio->prp[i] + (bio->prp_temp[i] % getpagesize());
                        syslog(LOG_INFO, "Kiran: New prp address after adjustment is %llu\n", bio->prp[i]);
                }
                bio->temp_size = bio->size;
        }
*/
#endif
#endif


#ifndef STATIC_BIO
	req->bio = bio;
#endif
	return NVME_SUCCESS;

unmap:
#ifndef STATIC_BIO
	nvme_bio_destroy (&bio);
#endif
	return NVME_INVALID_FIELD | NVME_DNR;
}

#if(KM_TEST_RD_NAND == 1)
/*uint8_t gk_virt_unmap()//Call this only once
{
	uint32_t i;
	for (i = 0; i < 70; i++) {
		munmap(gk_virt_addr[i], 1024*getpagesize());
	}
	return 0;
}*/
#endif

#if 0
static uint16_t nvme_dma_write_prp (NvmeCtrl *n, uint8_t *ptr, uint32_t len, uint64_t prp1, uint64_t prp2)
{
	SGList qsg;

	if (nvme_map_prp (&qsg, prp1, prp2, len, n)) {
		return NVME_INVALID_FIELD | NVME_DNR;
	}
	if (dma_buf_write (ptr, len, &qsg)) {
		qemu_sglist_destroy(&qsg);
		return NVME_INVALID_FIELD | NVME_DNR;
	}
	qemu_sglist_destroy(&qsg);
	return NVME_SUCCESS;
}
#endif

static uint16_t nvme_dma_read_prp (NvmeCtrl *n, uint8_t *ptr, uint32_t len,
		uint64_t prp1, uint64_t prp2)
{
#if 0
	SGList qsg;

	if (nvme_map_prp (&qsg, prp1, prp2, len, n)) {
		return NVME_INVALID_FIELD | NVME_DNR;
	}
	if (dma_buf_read (ptr, len, &qsg)) {
		qemu_sglist_destroy(&qsg);
		return NVME_INVALID_FIELD | NVME_DNR;
	}
#endif
	return NVME_SUCCESS;
}

static void nvme_post_cqe (NvmeCQ *cq, NvmeRequest *req)
{
	NvmeCtrl *n = cq->ctrl;
	NvmeSQ *sq = req->sq;
	NvmeCqe *cqe = &req->cqe;
	uint8_t phase = cq->phase;
	uint64_t addr;

	if (cq->phys_contig) {
		addr = cq->dma_addr + cq->tail * n->cqe_size;
	} else {
		addr = 0;
		/*addr = nvme_discontig (cq->prp_list, cq->tail, n->page_size, \*/
		/*n->cqe_size);*/
	}

	cqe->status = htole16((req->status << 1) | phase);
	cqe->sq_id = sq->sqid;
	cqe->sq_head = htole16(sq->head);
	nvme_addr_write (n, addr, (void *)cqe, sizeof (*cqe));
	nvme_inc_cq_tail (cq);

	QTAILQ_INSERT_TAIL (sq->req_list, req);
	if (cq->hold_sqs) cq->hold_sqs = 0;
}

static void nvme_post_cqes (void *opaque)
{
	NvmeCtrl *n = &g_NvmeCtrl;
	NvmeCQ *cq = opaque;
	NvmeRequest *req;

	mutex_lock(&n->req_mutex);
	QTAILQ_FOREACH (req, cq->req_list) {
		if (nvme_cq_full (cq)) {
			break;
		}
		QTAILQ_REMOVE (cq->req_list, req);
		nvme_post_cqe (cq, req);
	}
	mutex_unlock(&n->req_mutex);
	nvme_isr_notify (cq);
}

static void nvme_clear_ctrl (NvmeCtrl *n)
{
	NvmeAsyncEvent *event;
	int i;

	if (!n->running) {
		/*nvme not in action.. so nothing to stop*/
		return;
	}
	n->running = 0;
	if (n->sq) {
		for (i = 0; i < n->num_queues; i++) {
			if (n->sq[i] != NULL) {
				nvme_free_sq (n->sq[i], n);
			}
		}
	}

	if (n->cq) {
		for (i = 0; i < n->num_queues; i++) {
			if (n->cq[i] != NULL) {
				nvme_free_cq (n->cq[i], n);
			}
		}
	}

#ifdef SPINLOCK
	spin_lock(&n->aer_req_spin);
	while((event = (NvmeAsyncEvent *)QTAILQ_FIRST(n->aer_queue)) != NULL) {
		QTAILQ_REMOVE(n->aer_queue, event);
		FREE_VALID(event);
	}
	spin_unlock(&n->aer_req_spin);
#else
	mutex_lock(&n->req_mutex);
	while((event = (NvmeAsyncEvent *)QTAILQ_FIRST(n->aer_queue)) != NULL) {
		QTAILQ_REMOVE(n->aer_queue, event);
		FREE_VALID(event);
	}
	mutex_unlock(&n->req_mutex);
#endif
	delete_nvme_timer(n->aer_timer);
	n->nvme_regs.vBar.cc = 0;
	n->nvme_regs.vBar.csts = 0;
	if (n->fpga.ep[0].bar[2].addr) {
		n->fpga.nvme_regs->vBar.cc = 0;
	}
	n->features.temp_thresh = 0x14d;
	n->temp_warn_issued = 0;
	n->outstanding_aers = 0;
	reset_iosqdb_bits(&n->fpga);
	reset_nvmeregs(&n->fpga);
	reset_fifo(&n->fpga);
}

static void nvme_enqueue_event (NvmeCtrl *n, uint8_t event_type, \
		uint8_t event_info, uint8_t log_page)
{
	NvmeAsyncEvent *event;
	if (!(n->nvme_regs.vBar.csts & NVME_CSTS_READY))
		return;
	event = (NvmeAsyncEvent *)calloc (1, sizeof (*event));
	event->result.event_type = event_type;
	event->result.event_info = event_info;
	event->result.log_page   = log_page;

#ifdef SPINLOCK
	spin_lock(&n->aer_req_spin);
	QTAILQ_INSERT_TAIL (n->aer_queue, event);
	spin_unlock(&n->aer_req_spin);
#else
	mutex_lock(&n->req_mutex);
	QTAILQ_INSERT_TAIL (n->aer_queue, event);
	mutex_unlock(&n->req_mutex);
#endif
	start_nvme_timer (n->aer_timer);
}

static void nvme_enqueue_req_completion (NvmeCQ *cq, NvmeRequest *req)
{
	NvmeCtrl *n = cq->ctrl;
	uint64_t time_ns = NVME_INTC_TIME(n->features.int_coalescing) * 100000;
	uint8_t thresh = NVME_INTC_THR(n->features.int_coalescing) + 1;
	uint8_t coalesce_disabled =
		(n->features.int_vector_config[cq->vector] >> 16) & 1;
	uint8_t notify;

	assert (cq->cqid == req->sq->cqid);
	mutex_lock(&n->req_mutex);
	QTAILQ_REMOVE (req->sq->out_req_list, req);
	mutex_unlock(&n->req_mutex);

	if (nvme_cq_full (cq) || !QTAILQ_EMPTY (cq->req_list)) {
		mutex_lock(&n->req_mutex);
		QTAILQ_INSERT_TAIL (cq->req_list, req);
		mutex_unlock(&n->req_mutex);
		return;
	}
	mutex_lock(&n->req_mutex);
	nvme_post_cqe (cq, req);
	mutex_unlock(&n->req_mutex);
	// gunjae: monitoring
	//printf("GK: request is enqueued in CQ, QID : %d head : %d tail : %d \n", cq->cqid, cq->head, cq->tail);
#ifdef DEBUG
	syslog(LOG_INFO,"QID : %d head : %d tail : %d \n", cq->cqid, cq->head, cq->tail);
#endif
	notify = coalesce_disabled || !req->sq->sqid || !time_ns ||
		req->status != NVME_SUCCESS || nvme_cqes_pending(cq) >= thresh;
	if (!notify) {
		if (!nvme_timer_pending (cq->timer)) {
			start_nvme_timer (cq->timer);
		}
	} else {
		// gunjae: what is ISR notify??? let's check
		nvme_isr_notify (cq);
		if (nvme_timer_pending (cq->timer)) {
			stop_nvme_timer (cq->timer);
		}
	}
}

static void nvme_aer_process_cb(int sig,siginfo_t * si,void *param)
{
	NvmeCtrl *n = si->si_value.sival_ptr;
	NvmeRequest *req;
	NvmeAerResult *result;
	NvmeAsyncEvent *event;

	QTAILQ_FOREACH(event, n->aer_queue) {
		if (n->outstanding_aers <= 0) {
			break;
		}
		if (n->aer_mask & (1 << event->result.event_type)) {
			continue;
		}
#ifdef SPINLOCK
		spin_lock(&n->aer_req_spin);
		QTAILQ_REMOVE(n->aer_queue, event);
		spin_unlock(&n->aer_req_spin);
#else
		mutex_lock(&n->req_mutex);
		QTAILQ_REMOVE(n->aer_queue, event);
		mutex_unlock(&n->req_mutex);
#endif
		n->aer_mask |= 1 << event->result.event_type;
		n->outstanding_aers--;

		req = n->aer_reqs[n->outstanding_aers];
		result = (NvmeAerResult *)&req->cqe.result;
		result->event_type = event->result.event_type;
		result->event_info = event->result.event_info;
		result->log_page = event->result.log_page;
		FREE_VALID (event);

		req->status = NVME_SUCCESS;
		nvme_enqueue_req_completion(n->cq[0], req);
	}

}

static inline void nvme_discard_cb (void *opaque, int ret)
{
	NvmeRequest *req = opaque;

	if (!ret) {
		req->status = NVME_SUCCESS;
	} else {
		req->status = NVME_INTERNAL_DEV_ERROR;
	}
}

static inline void nvme_misc_cb (void *opaque, int ret)
{
	NvmeRequest *req = opaque;
	NvmeSQ *sq = req->sq;
	NvmeCtrl *n = sq->ctrl;
	NvmeCQ *cq = n->cq[sq->cqid];

	/*block_acct_done (blk_get_stats (n->conf.blk), &req->acct);*/
	if (!ret) {
		req->status = NVME_SUCCESS;
	} else {
		req->status = NVME_INTERNAL_DEV_ERROR;
	}
	nvme_enqueue_req_completion (cq, req);
}

static inline void nvme_set_error_page (NvmeCtrl *n, uint16_t sqid, uint16_t cid,
		uint16_t status, uint16_t location, uint64_t lba, uint32_t nsid)
{
	NvmeErrorLog *elp;

	elp = &n->elpes[n->elp_index];
	elp->error_count = n->num_errors;
	elp->sqid = sqid;
	elp->cid = cid;
	elp->status_field = status;
	elp->param_error_location = location;
	elp->lba = lba;
	elp->nsid = nsid;
	n->elp_index = (n->elp_index + 1) % n->elpe;
	++n->num_errors;
}

#if(KM_TPCH_QUERY6_CMP_ALL)
extern uint8_t * KM_tpch_query6_cmp_all_buffer;
extern unsigned int KM_tpch_query6_cmp_all_buffer_pageNum, KM_tpch_query6_cmp_all_buffer_page_position;
extern unsigned int KM_tpch_query6_cmp_all_numRecords;
#endif //KM_TPCH_QUERY6_CMP_ALL

#if(KM_TPCH_QUERY14_CMP_ALL)
extern uint8_t * KM_tpch_query14_cmp_all_buffer;
extern unsigned int KM_tpch_query14_cmp_all_buffer_pageNum, KM_tpch_query14_cmp_all_buffer_page_position;
extern unsigned int KM_tpch_query14_cmp_all_numRecords;
#endif //KM_TPCH_QUERY14_CMP_ALL

#if(KM_TPCH_QUERY6_CMP_ALL || KM_TPCH_QUERY6)
extern char KM_tpch_query6_date[20];
extern unsigned int KM_tpch_query6_quantity;
extern double KM_tpch_query6_discount;
extern unsigned int KM_tpch_query6_current_query;
#endif

#if(KM_TPCH_QUERY1_CMP_ALL)
extern uint8_t * KM_tpch_query1_cmp_all_buffer;
extern unsigned int KM_tpch_query1_cmp_all_buffer_pageNum, KM_tpch_query1_cmp_all_buffer_page_position;
extern unsigned int KM_tpch_query1_cmp_all_numRecords;
#endif //KM_TPCH_QUERY1_CMP_ALL

void nvme_rw_cb (void *opaque, int ret)
{
	NvmeRequest *req = opaque;
	NvmeSQ *sq = req->sq;
	NvmeCtrl *n = sq->ctrl;
	NvmeCQ *cq = n->cq[sq->cqid];
	NvmeNamespace *ns = req->ns;

	if (!ret) {
		req->status = NVME_SUCCESS;
		if(req->is_write) {
			n->stat.nr_bytes_written[0] += (req->nlb * (2 << ((9 + req->lba_index)-1))) >> 9;
		}
		else {
			n->stat.nr_bytes_read[0] += (req->nlb * (2 << ((9 + req->lba_index)-1))) >> 9;
		}
	} else {
		req->status = NVME_INTERNAL_DEV_ERROR;
		nvme_set_error_page (n, sq->sqid, req->cqe.cid, req->status,
				offsetof(NvmeRwCmd, slba), req->slba, ns->id);
		if (req->is_write) {
			//bitmap_clear(ns->util, req->slba, req->nlb);
		}
	}

#if(KM_TPCH_QUERY6_CMP_ALL)
	if(req->bio.req_type == REQTYPE_TPCH_QUERY6_CMP_ALL)
	{
		if(KM_tpch_query6_cmp_all_numRecords != 0)
		{
			int i=0;
			memcpy((void *)KM_tpch_query6_cmp_all_buffer, (void *)(&KM_tpch_query6_cmp_all_numRecords), sizeof(unsigned int));
			for(i=0; i <= KM_tpch_query6_cmp_all_buffer_pageNum; i++)
			{
				int lengthOfTransfer = getpagesize();
//				syslog(LOG_INFO, "Kiran: dest. address is %llx src address is %llx size of transfer is %d\n", (req->bio.prp[i] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr), (KM_tpch_query6_cmp_all_buffer + i*getpagesize()), lengthOfTransfer);
				memcpy((void *)(req->bio.prp[i] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr), (void *) (KM_tpch_query6_cmp_all_buffer + i*getpagesize()), (size_t)lengthOfTransfer);
//				syslog(LOG_INFO, "After memcpy\n");
			}
		}
		KM_tpch_query6_cmp_all_buffer_pageNum = 0;
		KM_tpch_query6_cmp_all_buffer_page_position = 0;
		KM_tpch_query6_cmp_all_numRecords = 0;
	}
#endif //KM_TPCH_QUERY6_CMP_ALL
#if(KM_TPCH_QUERY14_CMP_ALL)
	if(req->bio.req_type == REQTYPE_TPCH_QUERY14_CMP_ALL_READ1 || req->bio.req_type == REQTYPE_TPCH_QUERY14_CMP_ALL_READ2)
	{
		if(KM_tpch_query14_cmp_all_numRecords != 0)
		{
			int i=0;
			memcpy((void *)KM_tpch_query14_cmp_all_buffer, (void *)(&KM_tpch_query14_cmp_all_numRecords), sizeof(unsigned int));
			for(i=0; i <= KM_tpch_query14_cmp_all_buffer_pageNum; i++)
			{
				int lengthOfTransfer = getpagesize();
//				syslog(LOG_INFO, "Kiran: dest. address is %llx src address is %llx size of transfer is %d\n", (req->bio.prp[i] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr), (KM_tpch_query6_cmp_all_buffer + i*getpagesize()), lengthOfTransfer);
				memcpy((void *)(req->bio.prp[i] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr), (void *) (KM_tpch_query14_cmp_all_buffer + i*getpagesize()), (size_t)lengthOfTransfer);
//				syslog(LOG_INFO, "After memcpy\n");
			}
		}
		KM_tpch_query14_cmp_all_buffer_pageNum = 0;
		KM_tpch_query14_cmp_all_buffer_page_position = 0;
		KM_tpch_query14_cmp_all_numRecords = 0;
	}
#endif //KM_TPCH_QUERY14_CMP_ALL
#if(KM_TPCH_QUERY1_CMP_ALL)
	if(req->bio.req_type == REQTYPE_TPCH_QUERY1_CMP_ALL)
	{
		//MAKE SURE APPLICATION INITIALIZES TO ZERO
		if(KM_tpch_query1_cmp_all_numRecords != 0)
		{
			int i=0;
			memcpy((void *)KM_tpch_query1_cmp_all_buffer, (void *)(&KM_tpch_query1_cmp_all_numRecords), sizeof(unsigned int));
			for(i=0; i <= KM_tpch_query1_cmp_all_buffer_pageNum; i++)
			{
				int lengthOfTransfer = getpagesize();
				memcpy((void *)(req->bio.prp[i] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr), (void *) (KM_tpch_query1_cmp_all_buffer + i*getpagesize()), (size_t)lengthOfTransfer);
			}
		}
		KM_tpch_query1_cmp_all_buffer_pageNum = 0;
		KM_tpch_query1_cmp_all_buffer_page_position = 0;
		KM_tpch_query1_cmp_all_numRecords = 0;
	}
#endif //KM_TPCH_QUERY1_CMP_ALL

#if (KM_TEST_RD_NAND==1)
#ifdef STATIC_BIO
                if(req->is_write == 0)
                {
			syslog(LOG_INFO, "ModifiedCommand id is %d\n", req->cqe.cid);
                        int i;
                        syslog(LOG_INFO, "Kiran: After dma finished, No. of prps is %u\n", req->bio.nprps);
                        for(i=0; i<req->bio.nprps; i++)
                        {
                                int lengthOfTransfer = getpagesize();
                                syslog(LOG_INFO, "Kiran: dest. address is %llx src address is %llx size of transfer is %d %llu\n", (req->bio.prp_temp[i] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr), (req->bio.prp_ls2_virtual[i]), lengthOfTransfer, req->bio.temp_size );
                                int j;
                                syslog(LOG_INFO, "\n");
                                memcpy((void *)(req->bio.prp_temp[i] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr), (void *) (req->bio.prp_ls2_virtual[i]), (size_t)lengthOfTransfer);
                                syslog(LOG_INFO, "Amount of memory transferred %d\n",lengthOfTransfer);
                                req->bio.temp_size = req->bio.temp_size - lengthOfTransfer;
				syslog(LOG_INFO, "Address of gc_phy_addr free space index is %d\n", req->bio.prp_phy_addr_index[i]);
                                gc_phy_addr_freePages[req->bio.prp_phy_addr_index[i]] = 0;
                        }
//                        syslog(LOG_INFO, "Kiran: dest. address is %llu src address is %llu size of transfer is %d\n", req->bio.prp, req->bio.prp_temp, req->bio.nprps );
                        memcpy((void *) req->bio.prp, (void *) req->bio.prp_temp, sizeof(uint64_t) * req->bio.nprps);
                        //        free(req->bio.prp_temp);
                }
/*
        if(req->is_write == 0)
        {
                int i;  
                syslog(LOG_INFO, "Kiran: After dma finished, No. of prps is %u\n", req->bio.nprps);
                for(i=0; i<req->bio.nprps; i++)
                {
                        int lengthOfTransfer = MIN(getpagesize() - (req->bio.prp_temp[i] % getpagesize()), req->bio.temp_size);
//                      int lengthOfTransfer = getpagesize();
                        syslog(LOG_INFO, "Kiran: dest. address is %llu src address is %llu size of transfer is %d %llu\n", req->bio.prp_temp[i], req->bio.prp[i], lengthOfTransfer, req->bio.temp_size );
                        int j;
                        //              for(j=0; j < lengthOfTransfer; j++)
                        //                      syslog(LOG_INFO, "%c ",((char *)req->bio.prp[i])[j]);
                        syslog(LOG_INFO, "\n");
                        memcpy((void *)(req->bio.prp_temp[i] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr), (void *) (req->bio.prp[i]), (size_t)lengthOfTransfer);
                        syslog(LOG_INFO, "Amount of memory transferred %d\n",lengthOfTransfer);
                        req->bio.temp_size = req->bio.temp_size - lengthOfTransfer;
                        syslog(LOG_INFO, "Freeing pointer %llu\n",(req->bio.prp[i] - (req->bio.prp_temp[i] % getpagesize())));
                        free((void *)(req->bio.prp[i] - (req->bio.prp_temp[i] % getpagesize())));
                }
                syslog(LOG_INFO, "Kiran: dest. address is %llu src address is %llu size of transfer is %d\n", req->bio.prp, req->bio.prp_temp, req->bio.nprps );
                memcpy((void *) req->bio.prp, (void *) req->bio.prp_temp, sizeof(uint64_t) * req->bio.nprps);
                //        free(req->bio.prp_temp);
        }*/
#endif
#endif

#ifndef STATIC_BIO
	nvme_bio_destroy (&req->bio);
#endif
	nvme_enqueue_req_completion (cq, req);
}

#if(KM_TPCH_QUERY6)
extern double KM_tpch_query6_revenue;
#endif //KM_TPCH_QUERY6

#if (KM_TPCH_QUERY14 || KM_TPCH_QUERY14_CMP_ALL)
extern char KM_tpch_query14_date[20];
#endif // (KM_TPCH_QUERY14 || KM_TPCH_QUERY14_CMP_ALL)
#if(KM_TPCH_QUERY14)
extern double KM_tpch_query14_revenue;
extern double KM_tpch_query14_promo_revenue;
extern void KM_tpch_query14_free_hash_table();
#endif //KM_TPCH_QUERY14

#if (KM_TPCH_QUERY1 || KM_TPCH_QUERY1_CMP_ALL)
extern char KM_tpch_query1_date[20];
#endif // (KM_TPCH_QUERY1 || KM_TPCH_QUERY1_CMP_ALL)

#if(TE_AFC_STICKY)
extern float TE_sampling_rate;
extern int TE_minCount;
extern int TE_results[100];
void TE_afc_update_entries();
void TE_afc_get_results();
void TE_afc_free_hash_table();
#endif // TE_AFC_STICKY
#if (HV_PREFIX_SIMILARITY)
extern int KM_prefix_similarity_number_of_matched_query_records;
#endif //HV_PREFIX_SIMILARITY

// gunjae: read/write process function?
static uint16_t nvme_rw (NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
	NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
	int ret = 0;
	int off_count;
	uint16_t ctrl = rw->control;
	uint32_t nlb  = rw->nlb + 1;
	uint64_t slba = rw->slba;
	uint64_t prp1 = rw->prp1;
	uint64_t prp2 = rw->prp2;
	uint64_t addr = 0;

	const uint64_t elba = slba + nlb;
	const uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
	const uint8_t data_shift = ns->id_ns.lbaf[lba_index].ds;
	const uint16_t ms = ns->id_ns.lbaf[lba_index].ms;
	uint64_t data_size = nlb << data_shift;
	uint64_t meta_size = nlb * ms;
	uint64_t aio_slba =
		ns->start_block + (slba << (data_shift - BDRV_SECTOR_BITS));

	req->is_write = (rw->opcode == NVME_CMD_WRITE);

//	GK_REQ_PRINT("REQ(NVME_RW): opcode[%s] flags[%d] prp1[0x%lX] slba[0x%llX]->[0x%llX] cid[%d] nsid[%d] ", 
//			NvmeIoCommandsName[rw->opcode], rw->flags, rw->prp1, rw->slba, aio_slba, rw->cid, rw->nsid);

	if (elba > (ns->id_ns.nsze)) {
		nvme_set_error_page (n, req->sq->sqid, cmd->cid, NVME_LBA_RANGE,
				offsetof(NvmeRwCmd, nlb), elba, ns->id);
		return NVME_LBA_RANGE | NVME_DNR;
	}
	if (n->id_ctrl.mdts && data_size > n->page_size * (1 << n->id_ctrl.mdts)) {
		nvme_set_error_page (n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
				offsetof(NvmeRwCmd, nlb), nlb, ns->id);
		return NVME_INVALID_FIELD | NVME_DNR;
	}
	if (meta_size) {
		nvme_set_error_page (n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
				offsetof(NvmeRwCmd, control), ctrl, ns->id);
		return NVME_INVALID_FIELD | NVME_DNR;
	}
	if ((ctrl & NVME_RW_PRINFO_PRACT) && !(ns->id_ns.dps & DPS_TYPE_MASK)) {
		nvme_set_error_page (n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
				offsetof(NvmeRwCmd, control), ctrl, ns->id);
		return NVME_INVALID_FIELD | NVME_DNR;
	}
#if 0
	if (!req->is_write && find_next_bit (ns->uncorrectable, elba, slba) < elba) {
		nvme_set_error_page (n, req->sq->sqid, cmd->cid, NVME_UNRECOVERED_READ,
				offsetof(NvmeRwCmd, slba), elba, ns->id);
		return NVME_UNRECOVERED_READ;
	}
#endif
	if (nvme_map_prp (req, prp1, prp2, data_size, n)) {
#if 0
		nvme_set_error_page (n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
				offsetof(NvmeRwCmd, prp1), 0, ns->id);
#endif
		return NVME_INVALID_FIELD | NVME_DNR;
	}
	req->slba = aio_slba;
	req->meta_size = 0;
	req->status = NVME_SUCCESS;
	req->nlb = nlb;
	req->ns = ns;
	req->lba_index = lba_index;
	off_count = (aio_slba % (PAGE_SIZE/(1<<data_shift)));
	
	syslog(LOG_INFO, "Before Initialization %d %d\n", KM_TEST_TRANSFER_TIME, rw->flags);
	// gunjae: define req_type (0: write, 1: read)
#if (GK_REQ_TYPE || KM_TPCH_QUERY6 || KM_TPCH_QUERY6_CMP_ALL || KM_TPCH_QUERY14 || KM_TPCH_QUERY14_CMP_ALL || KM_TPCH_QUERY1 || KM_TPCH_QUERY1_CMP_ALL || TE_AFC_STICKY || HV_PREFIX_SIMILARITY || HV_PREFIX_SIMILARITY_CMP_ALL || KM_TEST_TRANSFER_TIME)
	if (req->is_write)
		req->bio.req_type = NVME_REQ_WRITE;
	else {
		switch (rw->flags) {
			case 17:
				req->bio.req_type = REQTYPE_AFC_QUERY_FINALIZE;
				break;
			case 16:
				req->bio.req_type = REQTYPE_AFC_STICKY;
				break;
			case 15:
				req->bio.req_type = REQTYPE_AFC_QUERY_UPDATE;
				break;
			case 20:
				req->bio.req_type = REQTYPE_PREFIX_SIMILARITY_FINALIZE; 
				break;
			case 19:
				req->bio.req_type = REQTYPE_PREFIX_SIMILARITY_CMP_ALL; 
				break;
			case 18:
				req->bio.req_type = REQTYPE_PREFIX_SIMILARITY_READ; 
				break;
			case 14:
				req->bio.req_type = REQTYPE_TPCH_QUERY1_CMP_ALL; 
				break;
			case 13:
				req->bio.req_type = REQTYPE_TPCH_QUERY1_FINALIZE; 
				break;
			case 12:
				req->bio.req_type = REQTYPE_TPCH_QUERY1_READ; 
				break;
			case 11:
                                req->bio.req_type = REQTYPE_TPCH_QUERY14_CMP_ALL_READ2; 
                                break;
			case 10:
                                req->bio.req_type = REQTYPE_TPCH_QUERY14_CMP_ALL_READ1;
                                break;
			case 9:
                                req->bio.req_type = REQTYPE_TPCH_QUERY14_FINALIZE; 
                                break;
			case 8:
                                req->bio.req_type = REQTYPE_TPCH_QUERY14_READ2; 
                                break;
			case 7:
                                req->bio.req_type = REQTYPE_TPCH_QUERY14_READ1;
                                break;
			case 6:
		#if(KM_TPCH_QUERY6_CMP_ALL)
				KM_tpch_query6_cmp_all_buffer_pageNum = 0;
				KM_tpch_query6_cmp_all_buffer_page_position = 0;
				KM_tpch_query6_cmp_all_numRecords = 0;
				req->bio.req_type = REQTYPE_TPCH_QUERY6_CMP_ALL;
		#endif
				break;
			case 5:
				req->bio.req_type = REQTYPE_TPCH_QUERY6_FINALIZE;
				break;
			case 4:
				syslog(LOG_INFO, "At Initialization\n");
//				syslog(LOG_INFO, "At Initialization %u\n", KM_tpch_query6_current_query);
/*			#if(KM_TPCH_QUERY6 || KM_TPCH_QUERY6_CMP_ALL)
				if(KM_tpch_query6_current_query == 0) {memcpy((void *)(KM_tpch_query6_date), "19930101", 9);KM_tpch_query6_discount = 0.05;KM_tpch_query6_quantity=24;}
				else if(KM_tpch_query6_current_query == 1) {memcpy((void *)(KM_tpch_query6_date), "19970101", 9);KM_tpch_query6_discount = 0.06;KM_tpch_query6_quantity=24;}
				else if(KM_tpch_query6_current_query == 2) {memcpy((void *)(KM_tpch_query6_date), "19940101", 9);KM_tpch_query6_discount = 0.05;KM_tpch_query6_quantity=25;}
				else if(KM_tpch_query6_current_query == 3) {memcpy((void *)(KM_tpch_query6_date), "19970101", 9);KM_tpch_query6_discount = 0.03;KM_tpch_query6_quantity=24;}
				else if(KM_tpch_query6_current_query == 4) {memcpy((void *)(KM_tpch_query6_date), "19960101", 9);KM_tpch_query6_discount = 0.04;KM_tpch_query6_quantity=24;}
				KM_tpch_query6_current_query += 1;
			#endif
*/				req->bio.req_type = REQTYPE_TPCH_QUERY_INITIALIZATION;
				break;
			case 3:
				req->bio.req_type = REQTYPE_TPCH_QUERY6_READ; // = 11
				break;
			case 2:
				req->bio.req_type = REQTYPE_READ_CALC0;	// = 10
				break;
			default:
				req->bio.req_type = NVME_REQ_READ;	// = 1
				break;
		}
	}
#else
	req->bio.req_type = req->is_write ? NVME_REQ_WRITE : NVME_REQ_READ;
#endif	// (GK_REQ_TYPE || KM_TPCH_QUERY6 || KM_TPCH_QUERY6_CMP_ALL || KM_TPCH_QUERY14 || KM_TPCH_QUERY14_CMP_ALL || KM_TPCH_QUERY1 || TE_AFC_STICKY) 

#ifdef STATIC_BIO
	req->bio.req_info = (void *)req;
	//req->bio.req_type = req->is_write ? NVME_REQ_WRITE : NVME_REQ_READ;
	req->bio.n_sectors = (nlb * (1 << data_shift)) >> BDRV_SECTOR_BITS;
	req->bio.nlb = nlb;
	req->bio.slba[0] = aio_slba;
	req->bio.ns_num = ns->id - 1;
	req->bio.offset = (off_count * (1<<data_shift));
	req->bio.is_seq = 1;
#else
	req->bio->req_info = (void *)req;
	//req->bio->req_type = req->is_write ? NVME_REQ_WRITE : NVME_REQ_READ;
	req->bio->n_sectors = (nlb * (1 << data_shift)) >> BDRV_SECTOR_BITS;
	req->bio->nlb = nlb;
	req->bio->slba = aio_slba;
	req->bio->ns_num = ns->id - 1;
	req->bio->offset = (off_count * (1<<data_shift));
#endif

#if (TE_AFC_STICKY)
	if(req->bio.req_type == REQTYPE_AFC_QUERY_UPDATE)
	{
	// Reading sampling rate and support
	memcpy(&TE_sampling_rate,(void *)(req->bio.prp[0] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr), sizeof(float));
	memcpy(&TE_minCount, (void *)(req->bio.prp[0] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr + sizeof(float)), sizeof(int));
	syslog(LOG_INFO, "STICKY_QUERY %f %d", TE_sampling_rate, TE_minCount);
	// Update hash table entries
	TE_afc_update_entries();
	}
	else if(req->bio.req_type == REQTYPE_AFC_QUERY_FINALIZE) 
	{
	syslog(LOG_INFO, "Entering int sticky sampling finalize");
	// Put the results
	TE_afc_get_results();
	// Getting results
	memcpy((void *)((req->bio.prp[0] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr)), (void *)(TE_results), 100*sizeof(int));
	// Free hash table and result array
	TE_afc_free_hash_table();
	}
#endif

#if (KM_TPCH_QUERY6 || KM_TPCH_QUERY6_CMP_ALL || KM_TPCH_QUERY14 || KM_TPCH_QUERY14_CMP_ALL || KM_TPCH_QUERY1 || KM_TPCH_QUERY1_CMP_ALL || HV_PREFIX_SIMILARITY || HV_PREFIX_SIMILARITY_CMP_ALL || KM_TEST_TRANSFER_TIME)
	if(req->bio.req_type == REQTYPE_TPCH_QUERY_INITIALIZATION)
	{
		syslog(LOG_INFO, "Entering into TPCH query initialization\n");
#if (KM_TPCH_QUERY1 || KM_TPCH_QUERY1_CMP_ALL)
	memcpy(KM_tpch_query1_date,(void *)(req->bio.prp[0] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr), 9);
	syslog(LOG_INFO, "TPCH_QUERY1 %s", KM_tpch_query1_date);
#if (KM_TPCH_QUERY1)
	KM_tpch_query1_free_map();
#endif // KM_TPCH_QUERY1
#endif // (KM_TPCH_QUERY1 || KM_TPCH_QUERY1_CMP_ALL)

#if (KM_TPCH_QUERY14 || KM_TPCH_QUERY14_CMP_ALL)
	memcpy(KM_tpch_query14_date,(void *)(req->bio.prp[0] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr), 9);
	syslog(LOG_INFO, "TPCH_QUERY14 %s", KM_tpch_query14_date);
#if (KM_TPCH_QUERY14)
	KM_tpch_query14_revenue = 0;
	KM_tpch_query14_promo_revenue = 0;
#endif // KM_TPCH_QUERY14
	syslog(LOG_INFO, "%u %u", KM_TPCH_QUERY6, KM_TPCH_QUERY6_CMP_ALL);
#endif // (KM_TPCH_QUERY14 || KM_TPCH_QUERY14_CMP_ALL)
#if (KM_TPCH_QUERY6 || KM_TPCH_QUERY6_CMP_ALL)
	char *temp_transfer_buffer = (char *)malloc(21);

/*	syslog(LOG_INFO, "TPCH_QUERY6 %s", temp_transfer_buffer);
	memcpy(temp_transfer_buffer,(void *)(req->bio.prp[0] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr), 21);
	syslog(LOG_INFO, "TPCH_QUERY6 %s", temp_transfer_buffer);
	memcpy(KM_tpch_query6_date,(void *)(temp_transfer_buffer), 9);
	syslog(LOG_INFO, "TPCH_QUERY6 %s %lf %u", KM_tpch_query6_date, KM_tpch_query6_discount, KM_tpch_query6_quantity);
	memcpy(&KM_tpch_query6_discount,(void *)(temp_transfer_buffer+9), 8);
	syslog(LOG_INFO, "TPCH_QUERY6 %s %lf %u", KM_tpch_query6_date, KM_tpch_query6_discount, KM_tpch_query6_quantity);
	memcpy(&KM_tpch_query6_quantity,(void *)(temp_transfer_buffer+17), 4);
	syslog(LOG_INFO, "TPCH_QUERY6 %s %lf %u", KM_tpch_query6_date, KM_tpch_query6_discount, KM_tpch_query6_quantity);
*/				if(KM_tpch_query6_current_query == 0) {memcpy((void *)(KM_tpch_query6_date), "19940101", 9);KM_tpch_query6_discount = 0.06;KM_tpch_query6_quantity=24;}
				else if(KM_tpch_query6_current_query == 1) {memcpy((void *)(KM_tpch_query6_date), "19970101", 9);KM_tpch_query6_discount = 0.06;KM_tpch_query6_quantity=24;}
				else if(KM_tpch_query6_current_query == 2) {memcpy((void *)(KM_tpch_query6_date), "19940101", 9);KM_tpch_query6_discount = 0.05;KM_tpch_query6_quantity=25;}
				else if(KM_tpch_query6_current_query == 3) {memcpy((void *)(KM_tpch_query6_date), "19970101", 9);KM_tpch_query6_discount = 0.03;KM_tpch_query6_quantity=24;}
				else if(KM_tpch_query6_current_query == 4) {memcpy((void *)(KM_tpch_query6_date), "19960101", 9);KM_tpch_query6_discount = 0.04;KM_tpch_query6_quantity=24;}
				KM_tpch_query6_current_query = (KM_tpch_query6_current_query + 1) % 5;
	
	free(temp_transfer_buffer);
#if (KM_TPCH_QUERY6)
	KM_tpch_query6_revenue = 0;		
#endif //KM_TPCH_QUERY6
#endif // KM_TPCH_QUERY6 || KM_TPCH_QUERY6_CMP_ALL
#if (KM_TEST_TRANSFER_TIME)
	//Transfer to the allocated memory
	struct timespec time_start;
	struct timespec time_end;
	double time_elapsed;
	char *temp_test_transfer_buffer;
	int test_transfer_numOfPages = 16;
	posix_memalign((void **)(&temp_test_transfer_buffer), getpagesize(), test_transfer_numOfPages*getpagesize());
	int temp_test_transfer_i;
	int transfer_length;
	memcpy((void *)&transfer_length, (void *)(req->bio.prp[0] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr), 4);
	syslog(LOG_INFO, "transfer_length = %d", transfer_length);

	if(transfer_length % 2 == 0)
	{
		for(temp_test_transfer_i = 0; temp_test_transfer_i < test_transfer_numOfPages*4096; temp_test_transfer_i++)
		{
			temp_test_transfer_buffer[temp_test_transfer_i] = 3;
		}

		clock_gettime(CLOCK_MONOTONIC, &time_start);
		for(temp_test_transfer_i = 0; temp_test_transfer_i < test_transfer_numOfPages; temp_test_transfer_i++)
		{
			int i;
			for(i = 0; (i + transfer_length) < getpagesize()/4; i += transfer_length)
			{
				memcpy((void *)(req->bio.prp[temp_test_transfer_i] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr + i), (void *)(temp_test_transfer_buffer+temp_test_transfer_i * getpagesize() + i), transfer_length);//if prp_temp contains the host address then use it
			}
		}
		clock_gettime(CLOCK_MONOTONIC, &time_end);

		time_elapsed = ((double)time_end.tv_nsec - (double)time_start.tv_nsec) / 1000000000.0;
		time_elapsed += ((double)time_end.tv_sec - (double)time_start.tv_sec);
		GK_PRINT("KM_TRANSFER_TIME_TEST1: %lf, bw= %lf MB/s\n", time_elapsed, ((double)getpagesize()*test_transfer_numOfPages)/(time_elapsed*1.0));
	}
	else {
		for(temp_test_transfer_i = 0; temp_test_transfer_i < test_transfer_numOfPages*4096; temp_test_transfer_i++)
		{
			temp_test_transfer_buffer[temp_test_transfer_i] = 4;
		}

		clock_gettime(CLOCK_MONOTONIC, &time_start);
		for(temp_test_transfer_i = 0; temp_test_transfer_i < test_transfer_numOfPages; temp_test_transfer_i++)
			memcpy((void *)((req->bio.prp[temp_test_transfer_i] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr)), (void *)(temp_test_transfer_buffer + temp_test_transfer_i*getpagesize()), getpagesize());//if prp_temp contains the host address then use it
		clock_gettime(CLOCK_MONOTONIC, &time_end);

		time_elapsed = ((double)time_end.tv_nsec - (double)time_start.tv_nsec) / 1000000000.0;
		time_elapsed += ((double)time_end.tv_sec - (double)time_start.tv_sec);
		GK_PRINT("KM_TRANSFER_TIME_TEST2: %lf, bw= %lf MB/s\n", time_elapsed, ((double)getpagesize()*test_transfer_numOfPages)/(time_elapsed*1.0));
	}
	free(temp_test_transfer_buffer);
#endif // KM_TEST_TRANSFER_TIME
	}
	else if(req->bio.req_type == REQTYPE_TPCH_QUERY6_FINALIZE)
	{
		syslog(LOG_INFO, "Entering into TPCH query finalization\n");
		//Write the query to the page and exit here
		//req->bio will have the prp, actually no need to call nvme_map_prp..but its okay...use that nvme_map_prp to transfer the data to something...
#if (KM_TPCH_QUERY6)
		memcpy((void *)((req->bio.prp[0] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr)), (void *)(&KM_tpch_query6_revenue), sizeof(double));//if prp_temp contains the host address then use it
		KM_tpch_query6_revenue = 0;
#endif //KM_TPCH_QUERY6
	}
	else if(req->bio.req_type == REQTYPE_TPCH_QUERY14_FINALIZE)
	{
		syslog(LOG_INFO, "Entering into TPCH query finalization\n");
		//Write the query to the page and exit here
		//req->bio will have the prp, actually no need to call nvme_map_prp..but its okay...use that nvme_map_prp to transfer the data to something...
#if (KM_TPCH_QUERY14)
		memcpy((void *)((req->bio.prp[0] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr)), (void *)(&KM_tpch_query14_revenue), sizeof(double));//if prp_temp contains the host address then use it
		memcpy((void *)((req->bio.prp[0] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr+sizeof(double))), (void *)(&KM_tpch_query14_promo_revenue), sizeof(double));//if prp_temp contains the host address then use it
		KM_tpch_query14_revenue = 0;
		KM_tpch_query14_promo_revenue = 0;
		KM_tpch_query14_free_hash_table();
#endif //KM_TPCH_QUERY14
#if (KM_TPCH_QUERY14_CMP_ALL)
#endif //KM_TPCH_QUERY14_CMP_ALL
	}
	else if(req->bio.req_type == REQTYPE_TPCH_QUERY1_FINALIZE)
	{
#if (KM_TPCH_QUERY1)
		uint8_t *Result_transfer_buffer;
		if (posix_memalign((void **)(&Result_transfer_buffer), getpagesize(), 32*getpagesize())) {
			syslog(LOG_INFO, "can not allocate io payload\n");
		}
		unsigned int buffer_page_number = 0;
		KM_tpch_query1_prepare_transfer(Result_transfer_buffer, &buffer_page_number);
		//MAKE SURE YOU ARE ZEROING IN THE APPLICATION
		if(((unsigned int *)Result_transfer_buffer)[0] != 0)
		{
			int i=0;
			for(i=0; i <= buffer_page_number; i++)
			{
				int lengthOfTransfer = getpagesize();
				memcpy((void *)(req->bio.prp[i] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr), (void *) (Result_transfer_buffer + i*getpagesize()), (size_t)lengthOfTransfer);
			}
		}
		free(Result_transfer_buffer);
		KM_tpch_query1_free_map();
#endif // (KM_TPCH_QUERY1)
	}
#endif // (KM_TPCH_QUERY6 || KM_TPCH_QUERY6_CMP_ALL || KM_TPCH_QUERY14 || KM_TPCH_QUERY14_CMP_ALL || KM_TPCH_QUERY1 || KM_TPCH_QUERY1_CMP_ALL)

	if (req->bio.req_type == REQTYPE_TPCH_QUERY_INITIALIZATION || 
			req->bio.req_type == REQTYPE_TPCH_QUERY6_FINALIZE || 
			req->bio.req_type == REQTYPE_TPCH_QUERY14_FINALIZE || 
			req->bio.req_type == REQTYPE_TPCH_QUERY1_FINALIZE || 
			req->bio.req_type == REQTYPE_AFC_QUERY_UPDATE || 
			req->bio.req_type == REQTYPE_AFC_QUERY_FINALIZE ||
			req->bio.req_type == REQTYPE_PREFIX_SIMILARITY_FINALIZE)
	{	
		syslog(LOG_INFO, "Entering into Prefix query finalization\n");
#if (HV_PREFIX_SIMILARITY)
		memcpy((void *)((req->bio.prp[0] - 0x1400000000 + g_NvmeCtrl.host.io_mem.addr)), (void *)(&KM_prefix_similarity_number_of_matched_query_records), sizeof(int));//if prp_temp contains the host address then use it
		dm_df_read_prefix_similarity_finalize();
#endif //HV_PREFIX_SIMILARITY
	}

	if(req->bio.req_type == REQTYPE_TPCH_QUERY_INITIALIZATION || req->bio.req_type == REQTYPE_TPCH_QUERY6_FINALIZE || req->bio.req_type == REQTYPE_TPCH_QUERY14_FINALIZE || req->bio.req_type == REQTYPE_TPCH_QUERY1_FINALIZE || req->bio.req_type == REQTYPE_PREFIX_SIMILARITY_FINALIZE)
	{
#ifndef STATIC_BIO
		nvme_bio_destroy (&req->bio);
#endif
		return NVME_SUCCESS;
	}

	addr = (uint64_t)req;
	do {
		if ((ret = mq_send(n->mq_txid, (char *)&addr, sizeof (uint64_t), 1)) < 0) {
			perror("mq_send");
		}
	} while (ret < 0);

	return NVME_NO_COMPLETE;
}

static uint16_t nvme_flush(NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
		NvmeRequest *req)
{
#if 0
	/*Flush support yet to be added*/
	block_acct_start (blk_get_stats (n->conf.blk), &req->acct, \
			0, BLOCK_ACCT_FLUSH);
	req->aiocb = blk_aio_flush(n->conf.blk, nvme_misc_cb, req);
	return NVME_NO_COMPLETE;
#else
	return NVME_INVALID_OPCODE;
#endif
}

static uint16_t nvme_dsm (NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
	uint32_t dw10 = cmd->cdw10;
	uint32_t dw11 = cmd->cdw11;
	uint64_t prp1 = cmd->prp1;
	uint64_t prp2 = cmd->prp2;

	if (dw11 & NVME_DSMGMT_AD) {
		uint16_t nr = (dw10 & 0xff) + 1;
		uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
		uint8_t data_shift = ns->id_ns.lbaf[lba_index].ds ;

		int i,ret;
		uint64_t aio_slba,prp_base;
		uint32_t nlb,nprps =1;
		NvmeDsmRange range[nr];
		prp_base = n->host.io_mem.addr + prp1;

		memcpy((void *)range,(void *)prp_base,sizeof(NvmeDsmRange));

		aio_slba = ns->start_block + (range[0].slba <<(data_shift - BDRV_SECTOR_BITS));
		req->slba = aio_slba;
		req->meta_size = 0;
		req->status = NVME_SUCCESS;
		req->nlb = range[0].nlb;
		req->ns = ns;

#ifdef STATIC_BIO
		req->bio.req_info = NULL;
		req->bio.req_type =	REQTYPE_TRIM;
		req->bio.n_sectors = range[0].nlb * (1 << data_shift) / (1 << BDRV_SECTOR_BITS);
		req->bio.nlb = range[0].nlb;
		req->bio.slba[0] = aio_slba;
		req->bio.ns_num = ns->id - 1;
		req->bio.is_seq = 1;
#else
		req->bio->req_info = NULL;
		req->bio->req_type =	REQTYPE_TRIM;
		req->bio->n_sectors = range[0].nlb * (1 << data_shift) / (1 << BDRV_SECTOR_BITS);
		req->bio->nlb = range[0].nlb;
		req->bio->slba = aio_slba;
		req->bio->ns_num = ns->id - 1;
#endif

		ret = nvme_bio_request (&req->bio);
		if (ret) { /*Add proper errer return in the abv function -FIXME*/
			syslog(LOG_ERR,"failed ftl bio req: %x\n", ret);
			goto FAILED_BIO;
		}
	}
#if (CUR_SETUP != TARGET_SETUP)
	return NVME_SUCCESS;
#else
	return NVME_SUCCESS;
	//	printf("return status is NVME_NO_cOMPLETE\n");
	//	return NVME_NO_COMPLETE;
#endif

FAILED_BIO:
	return NVME_DATA_TRAS_ERROR;


#if 0
	if (nvme_dma_write_prp (n, (uint8_t *)range, \
				sizeof (range), prp1, prp2)) {
		nvme_set_error_page (n, req->sq->sqid, cmd->cid, \
				NVME_INVALID_FIELD, offsetof(NvmeCmd, prp1), 0, ns->id);
		return NVME_INVALID_FIELD | NVME_DNR;
	}

	req->status = NVME_SUCCESS;
	for (i = 0; i < nr; i++) {
		slba = range[i].slba;
		nlb = range[i].nlb;
		if (slba + nlb > (ns->id_ns.nsze)) {
			nvme_set_error_page (n, req->sq->sqid, cmd->cid, \
					NVME_LBA_RANGE, offsetof(NvmeCmd, cdw10), \
					slba + nlb, ns->id);
			return NVME_LBA_RANGE | NVME_DNR;
		}

		req->aiocb = blk_aio_discard (n->conf.blk, \
				ns->start_block + (slba << data_shift), \
				nlb << data_shift, nvme_discard_cb, req);
		aio_processor_tid (blk_get_aio_context (n->conf.blk), true);
		if (req->status != NVME_SUCCESS)
			return req->status;
		/*bitmap_clear(ns->util, slba, nlb);*/
	}
#endif
}

#if 0
static uint16_t nvme_compare (NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
		NvmeRequest *req)
{
	NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
	uint32_t nlb  = rw->nlb + 1;
	uint64_t slba = rw->slba;
	uint64_t prp1 = rw->prp1;
	uint64_t prp2 = rw->prp2;

	uint64_t elba = slba + nlb;
	uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
	uint8_t data_shift = ns->id_ns.lbaf[lba_index].ds;
	uint64_t data_size = nlb << data_shift;
	uint64_t offset  = ns->start_block + (slba << data_shift);
	int i;

	if ((slba + nlb) > ns->id_ns.nsze) {
		nvme_set_error_page (n, req->sq->sqid, cmd->cid, NVME_LBA_RANGE,
				offsetof(NvmeRwCmd, nlb), elba, ns->id);
		return NVME_LBA_RANGE | NVME_DNR;
	}
	if (n->id_ctrl.mdts && \
			data_size > n->page_size * (1 << n->id_ctrl.mdts)) {
		nvme_set_error_page (n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
				offsetof(NvmeRwCmd, nlb), nlb, ns->id);
		return NVME_INVALID_FIELD | NVME_DNR;
	}
#if 0
	if (nvme_map_prp (&req->qsg, prp1, prp2, data_size, n)) {
		nvme_set_error_page (n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
				offsetof(NvmeRwCmd, prp1), 0, ns->id);
		return NVME_INVALID_FIELD | NVME_DNR;
	}
	if (find_next_bit (ns->uncorrectable, elba, slba) < elba) {
		return NVME_UNRECOVERED_READ;
	}

	for (i = 0; i < req->qsg.nsg; i++) {
		uint32_t len = 0; //TODO req->qsg.sg[i].len;
		uint8_t tmp[2][len];

		if (blk_pread (n->conf.blk, offset, tmp[0], len) != len) {
			qemu_sglist_destroy(&req->qsg);
			nvme_set_error_page (n, req->sq->sqid, req->cqe.cid,
					NVME_INTERNAL_DEV_ERROR, offsetof(NvmeRwCmd, slba),
					req->slba, ns->id);
			return NVME_INTERNAL_DEV_ERROR;
		}

		nvme_addr_read (n, req->qsg.sg[i].base, tmp[1], len);
		if (memcmp (tmp[0], tmp[1], len)) {
			qemu_sglist_destroy(&req->qsg);
			return NVME_CMP_FAILURE;
		}
		offset += len;
	}

	qemu_sglist_destroy(&req->qsg);
#endif
	return NVME_SUCCESS;
}

static uint16_t nvme_write_zeros (NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
		NvmeRequest *req)
{
	NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
	const uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
	const uint8_t data_shift = ns->id_ns.lbaf[lba_index].ds;
	uint64_t slba = rw->slba;
	uint32_t nlb  = rw->nlb + 1;
	uint64_t aio_slba = ns->start_block + \
			    (slba << (data_shift - BDRV_SECTOR_BITS));
	uint32_t aio_nlb = nlb << (data_shift - BDRV_SECTOR_BITS);

	if ((slba + nlb) > ns->id_ns.nsze) {
		nvme_set_error_page (n, req->sq->sqid, cmd->cid, NVME_LBA_RANGE,
				offsetof(NvmeRwCmd, nlb), slba + nlb, ns->id);
		return NVME_LBA_RANGE | NVME_DNR;
	}
#if 0
	block_acct_start (blk_get_stats (n->conf.blk), \
			&req->acct, 0, BLOCK_ACCT_WRITE);
	req->aiocb = blk_aio_write_zeroes (n->conf.blk, \
			aio_slba, aio_nlb, 0, nvme_misc_cb, req);
#endif
	return NVME_NO_COMPLETE;
}
#endif
static inline uint16_t nvme_write_uncor(NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
		NvmeRequest *req)
{
	NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
	uint64_t slba = rw->slba;
	uint32_t nlb  = rw->nlb + 1;

	if ((slba + nlb) > ns->id_ns.nsze) {
		nvme_set_error_page (n, req->sq->sqid, cmd->cid, NVME_LBA_RANGE,
				offsetof(NvmeRwCmd, nlb), slba + nlb, ns->id);
		return NVME_LBA_RANGE | NVME_DNR;
	}

	/*bitmap_set (ns->uncorrectable, slba, nlb);*/
	return NVME_SUCCESS;
}

// gunjae: is this a command parser? maybe correct
// - it receives NvmeCmd *cmd from SQ and parses command opcode
static uint16_t nvme_io_cmd (NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
	NvmeNamespace *ns;
	uint32_t nsid = cmd->nsid;

	if (nsid == 0 || nsid > n->num_namespaces) {
		syslog(LOG_ERR,"Bad nsid %d\n", nsid);
		return NVME_INVALID_NSID | NVME_DNR;
	}

	ns = &n->namespaces[nsid - 1];
	n->stat.tot_num_IOCmd += 1;
	switch (cmd->opcode) {
		case NVME_CMD_WRITE:
			n->stat.nr_write_ops[0] += 1;
			return nvme_rw(n, ns, cmd, req);

		case NVME_CMD_READ:
			n->stat.nr_read_ops[0] += 1;
			return nvme_rw(n, ns, cmd, req);

		case NVME_CMD_FLUSH:
			if (!n->id_ctrl.vwc || !n->features.volatile_wc) {
				return NVME_SUCCESS;
			}
			return nvme_flush(n, ns, cmd, req);

		case NVME_CMD_DSM:
			if (NVME_ONCS_DSM & n->oncs) {
				if (n->target.mem_type[nsid - 1] & FPGA_NAND_NS) {
					return nvme_dsm(n, ns, cmd, req);
				} else {
					return NVME_SUCCESS;
				}
			}
			/*Commands not supported yet*/
#if 0
		case NVME_CMD_COMPARE:
			if (NVME_ONCS_COMPARE & n->oncs) {
				return nvme_compare (n, ns, cmd, req);
			}
			return NVME_INVALID_OPCODE | NVME_DNR;

		case NVME_CMD_WRITE_ZEROS:
			if (NVME_ONCS_WRITE_ZEROS & n->oncs) {
				return nvme_write_zeros (n, ns, cmd, req);
			}
			return NVME_INVALID_OPCODE | NVME_DNR;

		case NVME_CMD_WRITE_UNCOR:
			if (NVME_ONCS_WRITE_UNCORR & n->oncs) {
				return nvme_write_uncor(n, ns, cmd, req);
			}
			return NVME_INVALID_OPCODE | NVME_DNR;
#endif
		default:
			n->stat.tot_num_IOCmd -= 1;
			return NVME_INVALID_OPCODE | NVME_DNR;
	}
}

static uint16_t nvme_del_sq (NvmeCtrl *n, NvmeCmd *cmd)
{
	NvmeDeleteQ *c = (NvmeDeleteQ *)cmd;
	NvmeRequest *req;
	NvmeSQ *sq;
	NvmeCQ *cq;
	uint16_t qid = c->qid;

	if (!qid || nvme_check_sqid (n, qid)) {
		return NVME_INVALID_QID | NVME_DNR;
	}

	sq = n->sq[qid];
	QTAILQ_FOREACH (req, sq->out_req_list) {
		if (req->aiocb) {
			/*blk_aio_cancel(req->aiocb); //TODO*/
		}
	}
	if (!nvme_check_cqid (n, sq->cqid)) {
		cq = n->cq[sq->cqid];
		mutex_lock(&n->req_mutex);
		QTAILQ_REMOVE (cq->sq_list, sq);
		mutex_unlock(&n->req_mutex);

		nvme_post_cqes (cq);
		mutex_lock(&n->req_mutex);
		QTAILQ_FOREACH (req, cq->req_list) {
			if (req->sq == sq) {
				QTAILQ_REMOVE (cq->req_list, req);
				QTAILQ_INSERT_TAIL (sq->req_list, req);
				if (cq->hold_sqs) cq->hold_sqs = 0;
			}
		}
		mutex_unlock(&n->req_mutex);
	}

	n->qsched.SQID[(qid - 1) >> 5] &= (~(1UL << ((qid - 1) & 31)));
	n->qsched.prio_avail[sq->prio] = n->qsched.prio_avail[sq->prio] - 1;
	n->qsched.mask_regs[sq->prio][(qid - 1) >> 6] &= (~(1UL << ((qid - 1) & (SHADOW_REG_SZ - 1))));
	n->qsched.shadow_regs[sq->prio][(qid -1) >> 6] &= (~(1UL << ((qid - 1) & (SHADOW_REG_SZ - 1))));
	n->qsched.n_active_iosqs--;

	nvme_free_sq (sq, n);
	return NVME_SUCCESS;
}

static uint16_t nvme_create_sq (NvmeCtrl *n, NvmeCmd *cmd)
{
	NvmeSQ *sq;
	NvmeCreateSq *c = (NvmeCreateSq *)cmd;	// gunjae: receive commands?

	uint16_t cqid = c->cqid;
	uint16_t sqid = c->sqid;
	uint16_t qsize = c->qsize;
	uint16_t qflags = c->sq_flags;
	uint64_t prp1 = c->prp1;

	if (!cqid || nvme_check_cqid (n, cqid)) {
		return NVME_INVALID_CQID | NVME_DNR;
	}
	if (!sqid || (sqid && !nvme_check_sqid (n, sqid))) {
		return NVME_INVALID_QID | NVME_DNR;
	}
	if (!qsize || qsize > NVME_CAP_MQES(n->nvme_regs.vBar.cap)) {
		return NVME_MAX_QSIZE_EXCEEDED | NVME_DNR;
	}
	if (!prp1 || prp1 & (n->page_size - 1)) {
		return NVME_INVALID_FIELD | NVME_DNR;
	}
	if (!(NVME_SQ_FLAGS_PC(qflags)) && NVME_CAP_CQR(n->nvme_regs.vBar.cap)) {
		return NVME_INVALID_FIELD | NVME_DNR;
	}

	sq = calloc (1, sizeof (NvmeSQ));

	if (nvme_init_sq (sq, n, prp1, sqid, cqid, qsize + 1, \
				NVME_SQ_FLAGS_QPRIO(qflags), \
				NVME_SQ_FLAGS_PC(qflags))) {
		FREE_VALID (sq);
		return NVME_INVALID_FIELD | NVME_DNR;
	}

	sq->prio = NVME_SQ_FLAGS_QPRIO(qflags);
	n->qsched.SQID[(sqid - 1) >> 5] |= (1UL << ((sqid - 1) & 31));
	n->qsched.mask_regs[sq->prio][(sqid - 1) >> 6] |= (1UL << ((sqid - 1) & (SHADOW_REG_SZ - 1)));
	n->qsched.n_active_iosqs++;
	n->qsched.prio_avail[sq->prio] = n->qsched.prio_avail[sq->prio] + 1;
	n->stat.num_active_queues += 1;

	return NVME_SUCCESS;
}

static uint16_t nvme_del_cq (NvmeCtrl *n, NvmeCmd *cmd)
{
	NvmeDeleteQ *c = (NvmeDeleteQ *)cmd;
	NvmeCQ *cq;
	uint16_t qid = c->qid;

	if (!qid || nvme_check_cqid (n, qid)) {
		return NVME_INVALID_CQID | NVME_DNR;
	}

	cq = n->cq[qid];
	if (!QTAILQ_EMPTY (cq->sq_list)) {
		return NVME_INVALID_QUEUE_DEL;
	}
	nvme_free_cq (cq, n);
	return NVME_SUCCESS;
}

static uint16_t nvme_create_cq (NvmeCtrl *n, NvmeCmd *cmd)
{
	NvmeCQ *cq;
	NvmeCreateCq *c = (NvmeCreateCq *)cmd;

	uint16_t cqid = c->cqid;
	uint16_t vector = c->irq_vector;
	uint16_t qsize = c->qsize;
	uint16_t qflags = c->cq_flags;
	uint64_t prp1 = c->prp1;

	if (!cqid || (cqid && !nvme_check_cqid (n, cqid))) {
		return NVME_INVALID_CQID | NVME_DNR;
	}
	if (!qsize || qsize > NVME_CAP_MQES(n->nvme_regs.vBar.cap)) {
		return NVME_MAX_QSIZE_EXCEEDED | NVME_DNR;
	}
	if (!prp1) {
		return NVME_INVALID_FIELD | NVME_DNR;
	}
	if (vector > n->num_queues) {
		return NVME_INVALID_IRQ_VECTOR | NVME_DNR;
	}
	if (!(NVME_CQ_FLAGS_PC(qflags)) && NVME_CAP_CQR(n->nvme_regs.vBar.cap)) {
		return NVME_INVALID_FIELD | NVME_DNR;
	}

	cq = calloc (1, sizeof (*cq));

	if (nvme_init_cq (cq, n, prp1, cqid, vector, qsize + 1,
				NVME_CQ_FLAGS_IEN(qflags), NVME_CQ_FLAGS_PC(qflags))) {
		FREE_VALID (cq);
		return NVME_INVALID_FIELD | NVME_DNR;
	}
	return NVME_SUCCESS;
}

static uint16_t nvme_identify (NvmeCtrl *n, NvmeCmd *cmd)
{
	NvmeNamespace *ns;
	NvmeIdentify *c = (NvmeIdentify *)cmd;
	uint32_t cns  = c->cns;
	uint32_t nsid = c->nsid;
	uint64_t prp1 = c->prp1;
	/*uint64_t prp2 = c->prp2;*/
	void *identifyInfoPtr = NULL;

	if (cns) {
#if 0
		return nvme_dma_read_prp (n, (uint8_t *)&n->id_ctrl, \
				sizeof (n->id_ctrl), prp1, prp2);
#endif
		NvmeIdCtrl *id = &n->id_ctrl;
		if (prp1) {
			int fd = 0;
#if (CUR_SETUP == TARGET_SETUP || CUR_SETUP == TARGET_RD_SETUP)
			identifyInfoPtr =(void *) (n->host.io_mem.addr + prp1); 
#else
			identifyInfoPtr = (void *)mmap_prp_addr (n->host.io_mem.addr, \
					prp1, 1, &fd);
#endif
			if (identifyInfoPtr) {
				GK_PRINT("GK: AdmIdentify(cns), dst=0x%llX, src=0x%llX, prp=0x%llX, size=%d\n", 
					identifyInfoPtr, (void*)id, prp1, sizeof(NvmeIdCtrl));
				memcpy (identifyInfoPtr, id, sizeof (NvmeIdCtrl));
				munmap_prp_addr (identifyInfoPtr, getpagesize (), &fd);
				return NVME_SUCCESS;
			} else {
				syslog(LOG_ERR,"%s map PRP failed\n", __func__);
				return NVME_INVALID_FIELD;
			}
		}
	}
	if (nsid == 0 || nsid > n->num_namespaces) {
		return NVME_INVALID_NSID | NVME_DNR;
	}

	ns = &n->namespaces[nsid - 1];
	if (prp1) {
		int fd = 0;
#if (CUR_SETUP == TARGET_SETUP || CUR_SETUP == TARGET_RD_SETUP)
		identifyInfoPtr =(void *)(n->host.io_mem.addr + prp1); 
#else
		identifyInfoPtr = (void *)mmap_prp_addr (n->host.io_mem.addr, \
				prp1, 1, &fd);
#endif
		if (identifyInfoPtr) {
			GK_PRINT("GK: AdmIdentify(ns), dst=0x%llX, src=0x%llX, prp=0x%llX, size=%d\n", 
					identifyInfoPtr, (void*)(&ns->id_ns), prp1, sizeof(NvmeIdCtrl));
			memcpy (identifyInfoPtr, &ns->id_ns, sizeof (NvmeIdNs));
			munmap_prp_addr (identifyInfoPtr, getpagesize (), &fd);
			return NVME_SUCCESS;
		} else {
			syslog(LOG_ERR,"%s map PRP failed\n", __func__);
			return NVME_INVALID_FIELD;
		}
	}
#if 0
	return nvme_dma_read_prp (n, (uint8_t *)&ns->id_ns, sizeof (ns->id_ns), \
			prp1, prp2);
#endif
	return NVME_SUCCESS;
}

static uint16_t nvme_set_feature (NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
	/*NvmeRangeType *rt;*/
	uint32_t dw10 = cmd->cdw10;
	uint32_t dw11 = cmd->cdw11;
	uint32_t nsid = cmd->nsid;
	/*uint64_t prp1 = cmd->prp1;
	  uint64_t prp2 = cmd->prp2;*/

	switch (dw10) {
		case NVME_ARBITRATION:
			req->cqe.result = htole32(n->features.arbitration);
			n->features.arbitration = dw11;
			break;
		case NVME_POWER_MANAGEMENT:
			n->features.power_mgmt = dw11;
			break;
		case NVME_LBA_RANGE_TYPE:
			if (nsid == 0 || nsid > n->num_namespaces) {
				return NVME_INVALID_NSID | NVME_DNR;
			}
			return NVME_SUCCESS;
#if 0
			rt = n->namespaces[nsid - 1].lba_range;
			return nvme_dma_write_prp (n, (uint8_t *)rt, \
					MIN(sizeof (*rt), (dw11 & 0x3f) * sizeof (*rt)), \
					prp1, prp2);
#endif
		case NVME_NUMBER_OF_QUEUES:
			if ((dw11 < 1) || (dw11 > NVME_MAX_QUEUE_ENTRIES)) {
				req->cqe.result = htole32 (n->num_queues | \
						(n->num_queues << 16));
			} else {
				req->cqe.result = htole32(dw11 | (dw11 << 16));
				n->num_queues = dw11;
			}
			break;
		case NVME_TEMPERATURE_THRESHOLD:
			n->features.temp_thresh = dw11;
			if (n->features.temp_thresh <= n->temperature && \
					!n->temp_warn_issued) {
				n->temp_warn_issued = 1;
				nvme_enqueue_event (n, NVME_AER_TYPE_SMART,
						NVME_AER_INFO_SMART_TEMP_THRESH,
						NVME_LOG_SMART_INFO);
			} else if (n->features.temp_thresh > n->temperature &&
					!(n->aer_mask & 1 << NVME_AER_TYPE_SMART)) {
				n->temp_warn_issued = 0;
			}
			break;
		case NVME_ERROR_RECOVERY:
			n->features.err_rec = dw11;
			break;
		case NVME_VOLATILE_WRITE_CACHE:
			n->features.volatile_wc = dw11;
			break;
		case NVME_INTERRUPT_COALESCING:
			n->features.int_coalescing = dw11;
			break;
		case NVME_INTERRUPT_VECTOR_CONF:
			if ((dw11 & 0xffff) > n->num_queues) {
				return NVME_INVALID_FIELD | NVME_DNR;
			}
			n->features.int_vector_config[dw11 & 0xffff] = dw11 & 0x1ffff;
			break;
		case NVME_WRITE_ATOMICITY:
			n->features.write_atomicity = dw11;
			break;
		case NVME_ASYNCHRONOUS_EVENT_CONF:
			n->features.async_config = dw11;
			break;
		case NVME_SOFTWARE_PROGRESS_MARKER:
			n->features.sw_prog_marker = dw11;
			break;
		default:
			return NVME_INVALID_FIELD | NVME_DNR;
	}
	return NVME_SUCCESS;
}

static uint16_t nvme_get_feature (NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
	/*NvmeRangeType *rt;*/
	uint32_t dw10 = cmd->cdw10;
	uint32_t dw11 = cmd->cdw11;
	uint32_t nsid = cmd->nsid;
	/*uint64_t prp1 = cmd->prp1;
	  uint64_t prp2 = cmd->prp2;*/

	switch (dw10) {
		case NVME_ARBITRATION:
			req->cqe.result = htole32(n->features.arbitration);
			break;
		case NVME_POWER_MANAGEMENT:
			req->cqe.result = htole32(n->features.power_mgmt);
			break;
		case NVME_LBA_RANGE_TYPE:
			if (nsid == 0 || nsid > n->num_namespaces) {
				return NVME_INVALID_NSID | NVME_DNR;
			}
			return NVME_SUCCESS;
#if 0
			rt = n->namespaces[nsid - 1].lba_range;
			return nvme_dma_read_prp (n, (uint8_t *)rt, \
					MIN(sizeof (*rt), (dw11 & 0x3f) * sizeof (*rt)), \
					prp1, prp2);
#endif
		case NVME_NUMBER_OF_QUEUES:
			req->cqe.result = htole32(n->num_queues | (n->num_queues << 16));
			break;
		case NVME_TEMPERATURE_THRESHOLD:
			req->cqe.result = htole32(n->features.temp_thresh);
			break;
		case NVME_ERROR_RECOVERY:
			req->cqe.result = htole32(n->features.err_rec);
			break;
		case NVME_VOLATILE_WRITE_CACHE:
			req->cqe.result = htole32(n->features.volatile_wc);
			break;
		case NVME_INTERRUPT_COALESCING:
			req->cqe.result = htole32(n->features.int_coalescing);
			break;
		case NVME_INTERRUPT_VECTOR_CONF:
			if ((dw11 & 0xffff) > n->num_queues) {
				return NVME_INVALID_FIELD | NVME_DNR;
			}
			req->cqe.result = htole32(
					n->features.int_vector_config[dw11 & 0xffff]);
			break;
		case NVME_WRITE_ATOMICITY:
			req->cqe.result = htole32(n->features.write_atomicity);
			break;
		case NVME_ASYNCHRONOUS_EVENT_CONF:
			req->cqe.result = htole32(n->features.async_config);
			break;
		case NVME_SOFTWARE_PROGRESS_MARKER:
			req->cqe.result = htole32(n->features.sw_prog_marker);
			break;
		default:
			return NVME_INVALID_FIELD | NVME_DNR;
	}
	return NVME_SUCCESS;
}

static uint16_t nvme_error_log_info (NvmeCtrl *n, NvmeCmd *cmd, \
		uint32_t buf_len)
{
	uint64_t prp1 = cmd->prp1;
	/*uint64_t prp2 = cmd->prp2;
	  uint32_t trans_len;*/
	void *errorlogInfoPtr = NULL;

	/*trans_len = MIN(sizeof (*n->elpes) * n->elpe, buf_len);*/
	n->aer_mask &= ~(1 << NVME_AER_TYPE_ERROR);

	if (!QTAILQ_EMPTY(n->aer_queue)) {
		start_nvme_timer (n->aer_timer);
	}

	if(prp1) {
		int fd = 0;
		errorlogInfoPtr = (void*)mmap_prp_addr(n->host.io_mem.addr, \
				prp1,1,&fd);
		if(errorlogInfoPtr) {
			memcpy (errorlogInfoPtr, n->elpes, sizeof(NvmeErrorLog));
			munmap_prp_addr (errorlogInfoPtr, getpagesize (), &fd);
			return NVME_SUCCESS;
		} else {
			syslog(LOG_ERR,"%s mam PRP Failed\n",__func__);
			return NVME_INVALID_FIELD;
		}
	}
	//return nvme_dma_read_prp (n, (uint8_t *)n->elpes, trans_len, prp1, prp2);
	return NVME_SUCCESS;
}

static inline uint16_t nvme_fw_log_info (NvmeCtrl *n, NvmeCmd *cmd, uint32_t buf_len)
{
	uint32_t trans_len;
	uint64_t prp1 = cmd->prp1;
	uint64_t prp2 = cmd->prp2;
	NvmeFwSlotInfoLog fw_log;

	trans_len = MIN(sizeof (fw_log), buf_len);
	return nvme_dma_read_prp (n, (uint8_t *)&fw_log, trans_len, prp1, prp2);
}

static uint16_t nvme_smart_info (NvmeCtrl *n, NvmeCmd *cmd, uint32_t buf_len)
{
	uint64_t prp1 = cmd->prp1;
	/*uint64_t prp2 = cmd->prp2;
	  uint32_t trans_len;*/

	time_t current_seconds;
	int Rtmp = 0,Wtmp = 0;
	int read = 0,write = 0;
	NvmeSmartLog smart;
	void *smartInfoPtr = NULL;

	/*trans_len = MIN(sizeof (smart), buf_len);*/
	memset (&smart, 0x0, sizeof (smart));
	Rtmp = n->stat.nr_bytes_read[0]/1000;
	Wtmp = n->stat.nr_bytes_written[0]/1000;

	read = n->stat.nr_bytes_read[0]%1000;
	write = n->stat.nr_bytes_written[0]%1000;

	read = (read >= 500)?1:0;
	write = (write >=500)?1:0;

	n->stat.nr_bytes_read[0] = Rtmp + read;
	n->stat.nr_bytes_written[0] = Wtmp + write;

	smart.data_units_read[0] = htole64(n->stat.nr_bytes_read[0]);
	smart.data_units_written[0] = htole64(n->stat.nr_bytes_written[0]);
	smart.host_read_commands[0] = htole64(n->stat.nr_read_ops[0]); 
	smart.host_write_commands[0] = htole64(n->stat.nr_write_ops[0]);

	smart.number_of_error_log_entries[0] = htole64(n->num_errors);
	smart.temperature[0] = n->temperature & 0xff;
	smart.temperature[1] = (n->temperature >> 8) & 0xff;

	current_seconds = time (NULL);
	smart.power_on_hours[0] = htole64(
			((current_seconds - n->start_time) / 60) / 60);

	smart.available_spare_threshold = NVME_SPARE_THRESHOLD;
	if (smart.available_spare <= NVME_SPARE_THRESHOLD) {
		smart.critical_warning |= NVME_SMART_SPARE;
	}
	if (n->features.temp_thresh <= n->temperature) {
		smart.critical_warning |= NVME_SMART_TEMPERATURE;
	}

	n->aer_mask &= ~(1 << NVME_AER_TYPE_SMART);
	if (!QTAILQ_EMPTY(n->aer_queue)) {
		start_nvme_timer (n->aer_timer);
	}
	NvmeSmartLog *smrt = &smart;
	if(prp1) {
		int fd = 0;
#if (CUR_SETUP == TARGET_SETUP || CUR_SETUP == TARGET_RD_SETUP)
		smartInfoPtr = (void *)(n->host.io_mem.addr + prp1); 
#else
		smartInfoPtr = (void *)mmap_prp_addr (n->host.io_mem.addr, \
				prp1,1, &fd);
#endif
		if(smartInfoPtr) {
			memcpy (smartInfoPtr,smrt,sizeof(NvmeSmartLog));
			munmap_prp_addr (smartInfoPtr, getpagesize(), &fd);
			return NVME_SUCCESS;
		} else {
			syslog(LOG_ERR,"%s map PRP failed\n", __func__);
			return NVME_INVALID_FIELD;

		}
	}
	//return nvme_dma_read_prp (n, (uint8_t *)&smart, trans_len, prp1, prp2);
	return NVME_SUCCESS;
}

static inline uint16_t nvme_get_log(NvmeCtrl *n, NvmeCmd *cmd)
{
	uint32_t dw10 = cmd->cdw10;
	uint16_t lid = dw10 & 0xffff;
	uint32_t len = ((dw10 >> 16) & 0xff) << 2;

	switch (lid) {
		case NVME_LOG_ERROR_INFO:
			return nvme_error_log_info (n, cmd, len);
		case NVME_LOG_SMART_INFO:
			return nvme_smart_info (n, cmd, len);
		case NVME_LOG_FW_SLOT_INFO:
			return nvme_fw_log_info (n, cmd, len);
		default:
			return NVME_INVALID_LOG_ID | NVME_DNR;
	}
}

static inline uint16_t nvme_async_req (NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
	if (n->outstanding_aers > n->aerl + 1) {
		return NVME_AER_LIMIT_EXCEEDED;
	}
	n->aer_reqs[n->outstanding_aers] = req;
	n->outstanding_aers++;
	start_nvme_timer (n->aer_timer);

	return NVME_NO_COMPLETE;
}


static uint16_t nvme_abort_req (NvmeCtrl *n, NvmeCmd *cmd, uint32_t *result)
{
	uint32_t index = 0;
	uint16_t sqid = cmd->cdw10 & 0xffff;
	uint16_t cid = (cmd->cdw10 >> 16) & 0xffff;
	NvmeSQ *sq;
	NvmeRequest *req;

	*result = 1;
	if (nvme_check_sqid (n, sqid)) {
		return NVME_SUCCESS;
	}

	sq = n->sq[sqid];
	QTAILQ_FOREACH (req, sq->out_req_list) {
		if (sq->sqid) {
			if (req->aiocb && req->cqe.cid == cid) {
				/*bdrv_aio_cancel(req->aiocb);*/
				*result = 0;
				return NVME_SUCCESS;
			}
		}
	}

	while ((sq->head + index) % sq->size != sq->tail) {
		NvmeCmd abort_cmd;
		uint64_t addr;

		if (sq->phys_contig) {
			addr = sq->dma_addr + ((sq->head + index) % sq->size) *
				n->sqe_size;
		} else {
#if 0
			addr = nvme_discontig (sq->prp_list, (sq->head + index) % sq->size, \
					n->page_size, n->sqe_size);
#endif
		}
		nvme_addr_read (n, addr, (void *)&abort_cmd, sizeof (abort_cmd));
		if (abort_cmd.cid == cid) {
			*result = 0;
			mutex_lock(&n->req_mutex);
			req = (NvmeRequest *)QTAILQ_FIRST (sq->req_list);
			QTAILQ_REMOVE (sq->req_list, req);
			QTAILQ_INSERT_TAIL (sq->out_req_list, req);
			mutex_unlock(&n->req_mutex);

			memset (&req->cqe, 0, sizeof (req->cqe));
			req->cqe.cid = cid;
			req->status = NVME_CMD_ABORT_REQ;

			abort_cmd.opcode = NVME_OP_ABORTED;
			nvme_addr_write (n, addr, (void *)&abort_cmd,
					sizeof (abort_cmd));

			nvme_enqueue_req_completion (n->cq[sq->cqid], req);
			return NVME_SUCCESS;
		}

		++index;
	}
	return NVME_SUCCESS;
}

static uint16_t nvme_format_namespace (NvmeNamespace *ns, uint8_t lba_idx,
		uint8_t meta_loc, uint8_t pil, uint8_t pi, uint8_t sec_erase)
{
	uint64_t blks;
	uint16_t ms = ns->id_ns.lbaf[lba_idx].ms;

	if (lba_idx > ns->id_ns.nlbaf) {
		return NVME_INVALID_FORMAT | NVME_DNR;
	}
	if (pi) {
		if (pil && !NVME_ID_NS_DPC_LAST_EIGHT(ns->id_ns.dpc)) {
			return NVME_INVALID_FORMAT | NVME_DNR;
		}
		if (!pil && !NVME_ID_NS_DPC_FIRST_EIGHT(ns->id_ns.dpc)) {
			return NVME_INVALID_FORMAT | NVME_DNR;
		}
		if (!((ns->id_ns.dpc & 0x7) & (1 << (pi - 1)))) {
			return NVME_INVALID_FORMAT | NVME_DNR;
		}
	}
	if (meta_loc && ms && !NVME_ID_NS_MC_EXTENDED(ns->id_ns.mc)) {
		return NVME_INVALID_FORMAT | NVME_DNR;
	}
	if (!meta_loc && ms && !NVME_ID_NS_MC_SEPARATE(ns->id_ns.mc)) {
		return NVME_INVALID_FORMAT | NVME_DNR;
	}

	FREE_VALID (ns->util);
	FREE_VALID (ns->uncorrectable);
	blks = ns->ctrl->ns_size[ns->id - 1] / ((1 << ns->id_ns.lbaf[lba_idx].ds) + ns->ctrl->meta);
	ns->id_ns.flbas = lba_idx | meta_loc;
	ns->id_ns.nsze = htole64(blks);
	ns->id_ns.ncap = ns->id_ns.nsze;
	ns->id_ns.nuse = ns->id_ns.nsze;
	ns->id_ns.dps = pil | pi;
#if 0
	ns->util = bitmap_new(blks);
	ns->uncorrectable = bitmap_new(blks);
#endif

	if (sec_erase) {
		/* TODO: write zeros, complete asynchronously */;
	}

	return NVME_SUCCESS;
}

static uint16_t nvme_format (NvmeCtrl *n, NvmeCmd *cmd)
{
	NvmeNamespace *ns;
	uint32_t dw10 = cmd->cdw10;
	uint32_t nsid = cmd->nsid;

	uint8_t lba_idx = dw10 & 0xf;
	uint8_t meta_loc = dw10 & 0x10;
	uint8_t pil = (dw10 >> 5) & 0x8;
	uint8_t pi = (dw10 >> 5) & 0x7;
	uint8_t sec_erase = (dw10 >> 8) & 0x7;

	if (nsid == 0xffffffff) {
		uint32_t i;
		uint16_t ret = NVME_SUCCESS;

		for (i = 0; i < n->num_namespaces; ++i) {
			ns = &n->namespaces[i];
			ret = nvme_format_namespace (ns, lba_idx, meta_loc, pil, pi, \
					sec_erase);
			if (ret != NVME_SUCCESS) {
				return ret;
			}
		}
		return ret;
	}

	if (nsid == 0 || nsid > n->num_namespaces) {
		return NVME_INVALID_NSID | NVME_DNR;
	}

	ns = &n->namespaces[nsid - 1];
	return nvme_format_namespace (ns, lba_idx, meta_loc, pil, pi,
			sec_erase);
}

static uint16_t nvme_admin_cmd (NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
	n->stat.tot_num_AdminCmd += 1;
	syslog(LOG_INFO,"AdminCmd: %x\n", cmd->opcode);
    // gunjae:
	char admin_cmd[256];
	switch (cmd->opcode) {
		case NVME_ADM_CMD_DELETE_SQ: strcpy(admin_cmd,"NVME_ADM_CMD_DELETE_SQ"); break;
		case NVME_ADM_CMD_CREATE_SQ: strcpy(admin_cmd,"NVME_ADM_CMD_CREATE_SQ"); break;
		case NVME_ADM_CMD_DELETE_CQ: strcpy(admin_cmd,"NVME_ADM_CMD_DELETE_CQ"); break;
		case NVME_ADM_CMD_CREATE_CQ: strcpy(admin_cmd,"NVME_ADM_CMD_CREATE_CQ"); break;
		case NVME_ADM_CMD_IDENTIFY: strcpy(admin_cmd,"NVME_ADM_CMD_IDENTIFY"); break;
		case NVME_ADM_CMD_SET_FEATURES: strcpy(admin_cmd,"NVME_ADM_CMD_SET_FEATURES"); break;
		case NVME_ADM_CMD_GET_FEATURES: strcpy(admin_cmd,"NVME_ADM_CMD_GET_FEATURES"); break;
		case NVME_ADM_CMD_GET_LOG_PAGE: strcpy(admin_cmd,"NVME_ADM_CMD_GET_LOG_PAGE"); break;
		case NVME_ADM_CMD_ASYNC_EV_REQ: strcpy(admin_cmd,"NVME_ADM_CMD_ASYNC_EV_REQ"); break;
		case NVME_ADM_CMD_ABORT: strcpy(admin_cmd,"NVME_ADM_CMD_ABORT"); break;
		case NVME_ADM_CMD_FORMAT_NVM: strcpy(admin_cmd,"NVME_ADM_CMD_FORMAT_NVM"); break;
		case NVME_ADM_CMD_ACTIVATE_FW:
		case NVME_ADM_CMD_DOWNLOAD_FW:
		case NVME_ADM_CMD_SECURITY_SEND:
		case NVME_ADM_CMD_SECURITY_RECV: 
		default: strcpy(admin_cmd,"????"); break;
	}
	GK_PRINT("GK: AdminCmd: %s\n", admin_cmd);
    //GK_PRINT("GK: AdminCmd: %x\n", cmd->opcode);
    //
	switch (cmd->opcode) {
		case NVME_ADM_CMD_DELETE_SQ:
			return nvme_del_sq (n, cmd);
		case NVME_ADM_CMD_CREATE_SQ:
			return nvme_create_sq (n, cmd);
		case NVME_ADM_CMD_DELETE_CQ:
			return nvme_del_cq (n, cmd);
		case NVME_ADM_CMD_CREATE_CQ:
			return nvme_create_cq (n, cmd);
		case NVME_ADM_CMD_IDENTIFY:
			return nvme_identify (n, cmd);
		case NVME_ADM_CMD_SET_FEATURES:
			return nvme_set_feature (n, cmd, req);
		case NVME_ADM_CMD_GET_FEATURES:
			return nvme_get_feature (n, cmd, req);
		case NVME_ADM_CMD_GET_LOG_PAGE:
			return nvme_get_log(n, cmd);
		case NVME_ADM_CMD_ASYNC_EV_REQ:
			return nvme_async_req (n, cmd, req);
		case NVME_ADM_CMD_ABORT:
			return nvme_abort_req (n, cmd, &req->cqe.result);
		case NVME_ADM_CMD_FORMAT_NVM:
			if (NVME_OACS_FORMAT & n->oacs) {
				return nvme_format (n, cmd);
			}
			return NVME_INVALID_OPCODE | NVME_DNR;
		case NVME_ADM_CMD_ACTIVATE_FW:
		case NVME_ADM_CMD_DOWNLOAD_FW:
		case NVME_ADM_CMD_SECURITY_SEND:
		case NVME_ADM_CMD_SECURITY_RECV:
		default:
			n->stat.tot_num_AdminCmd -= 1;
			return NVME_INVALID_OPCODE | NVME_DNR;
	}
}

static inline void nvme_update_sq_tail (NvmeSQ *sq)
{
	if (sq->db_addr) {
		nvme_addr_read (sq->ctrl, sq->db_addr, &sq->tail, sizeof (sq->tail));
	}
}

static inline void nvme_update_sq_eventidx (const NvmeSQ *sq)
{
	if (sq->eventidx_addr) {
		nvme_addr_write (sq->ctrl, sq->eventidx_addr, (void *)&sq->tail,
				sizeof (sq->tail));
	}
}

static void nvme_process_sq (NvmeSQ *sq)
{
	NvmeCtrl *n = sq->ctrl;
	NvmeCQ *cq = n->cq[sq->cqid];

	if (cq->hold_sqs || QTAILQ_EMPTY (&sq->req_list)) {
		cq->hold_sqs = 1;
		syslog(LOG_INFO,"Process-SQ %d with CQ %d delayed\n", sq->sqid, sq->cqid);
		return;
	}

	uint16_t status;
	uint64_t addr;
	NvmeCmd cmd;
	NvmeRequest *req;
	int processed = 0, reg_sqid = 0;

	nvme_update_sq_tail (sq);
	while (!(nvme_sq_empty(sq) || QTAILQ_EMPTY (&sq->req_list))
			&&	processed < sq->arb_burst) {
		++sq->posted;
		if (sq->phys_contig) {
			addr = sq->dma_addr + sq->head * n->sqe_size;
		} else {
#if 0
			addr = nvme_discontig (sq->prp_list, sq->head, \
					n->page_size, n->sqe_size);
#endif
		}
		nvme_addr_read (n, addr, (void *)&cmd, sizeof (cmd));
		nvme_inc_sq_head (sq);

#ifdef DEBUG
		syslog(LOG_INFO,"SQ:%d h:%d t:%d\n", sq->sqid, sq->head, sq->tail);
#endif
		if (cmd.opcode == NVME_OP_ABORTED) {
			continue;
		}
		mutex_lock(&n->req_mutex);
		req = (NvmeRequest *)QTAILQ_FIRST (sq->req_list);
		QTAILQ_REMOVE (sq->req_list, req);
		QTAILQ_INSERT_TAIL (sq->out_req_list, req);
		mutex_unlock(&n->req_mutex);
		memset (&req->cqe, 0, sizeof (req->cqe));
		req->cqe.cid = cmd.cid;
		req->aiocb = NULL;
		/*hexDump_32Bit ((uint32_t *)&cmd, 16);*/
		// gunjae: nvme_io_cmd - is this command?
		status = sq->sqid ? nvme_io_cmd (n, &cmd, req) :
			nvme_admin_cmd (n, &cmd, req);
		if (status != NVME_NO_COMPLETE) {
			// gunjae: if status is not NO_COMPLETE, then enqueue into req_completion
			req->status = status;
			nvme_enqueue_req_completion (cq, req);
		}
		processed++;
	}
	/*nvme_update_sq_eventidx (sq);*/

	// gunjae: if SQ is empty?
	if (nvme_sq_empty(sq) & (sq->sqid > 0)) {
		nvme_update_sq_tail (sq);
		if (nvme_sq_empty(sq)) {
			reg_sqid = sq->sqid - 1;
#ifdef SPINLOCK
			spin_lock(&n->qs_req_spin);
			if(n->qsched.WRR) {
				n->qsched.shadow_regs[sq->prio][reg_sqid >> 6] &= (~(1UL << reg_sqid));
			} else {
				n->qsched.round_robin_status_regs[reg_sqid >> 5] &= (~(1UL << reg_sqid));
			}
			spin_unlock(&n->qs_req_spin);
#else
			mutex_lock(&n->qsch_mutex);
			if(n->qsched.WRR) {
				n->qsched.shadow_regs[sq->prio][reg_sqid >> 6] &= (~(1UL << reg_sqid));
			} else {
				n->qsched.round_robin_status_regs[reg_sqid >> 5] &= (~(1UL << reg_sqid));
			}
			mutex_unlock(&n->qsch_mutex);
#endif    
		}
	}
	sq->completed += processed;
}

static uint16_t nvme_init_sq (NvmeSQ *sq, NvmeCtrl *n, uint64_t dma_addr, uint16_t sqid, uint16_t cqid, uint16_t size, enum NvmeQFlags prio, int contig)
{
	int i;
	NvmeCQ *cq;
	uint64_t sq_base = 0;
	int fd_qmem = 0;

	if (dma_addr) {
		sq_base = mmap_queue_addr (n->host.io_mem.addr, dma_addr, sqid, &fd_qmem);
		if (!sq_base) {
			syslog(LOG_ERR,"%s Qmap ERR: 0x%p\n", __func__, (void *)dma_addr);
			return NVME_INVALID_QID;
		}
	}

	sq->ctrl = n;
	sq->sqid = sqid;
	sq->size = size;
	sq->cqid = cqid;
	sq->head = sq->tail = 0;
	sq->phys_contig = contig;

	if (sq->phys_contig) {
		sq->dma_addr = sq_base;
	} else {
#if 1
		sq->prp_list = NULL; // TODO
#else
		sq->prp_list = nvme_setup_discontig (n, dma_addr, size, n->sqe_size);
#endif
		if (!sq->prp_list) {
			return NVME_INVALID_FIELD | NVME_DNR;
		}
	}

	sq->io_req = calloc (1, sq->size * sizeof (NvmeRequest));
	sq->req_list = calloc (1, sizeof (NvmeRequest)); /*just for a dlcl head*/
	sq->out_req_list = calloc (1, sizeof (NvmeRequest)); /*just for a dlcl head*/
	QTAILQ_INIT (sq->req_list);
	QTAILQ_INIT (sq->out_req_list);
	for (i = 0; i < sq->size; i++) {
		sq->io_req[i].sq = sq;
		QTAILQ_INSERT_TAIL (sq->req_list, &sq->io_req[i]);
	}

	switch (prio) {
		case NVME_Q_PRIO_URGENT:
			sq->arb_burst = (1 << NVME_ARB_AB(n->features.arbitration));
			break;
		case NVME_Q_PRIO_HIGH:
			sq->arb_burst = NVME_ARB_HPW(n->features.arbitration) + 1;
			break;
		case NVME_Q_PRIO_NORMAL:
			sq->arb_burst = NVME_ARB_MPW(n->features.arbitration) + 1;
			break;
		case NVME_Q_PRIO_LOW:
		default:
			sq->arb_burst = NVME_ARB_LPW(n->features.arbitration) + 1;
			break;
	}

	sq->db_addr = (uint64_t)((char *)n->fpga.nvme_regs + 0x1000 + (sqid << 1) * (4 << n->db_stride));
	sq->eventidx_addr = 0;

	assert (n->cq[cqid]);
	cq = n->cq[cqid];
	QTAILQ_INSERT_TAIL (cq->sq_list, sq);
	n->sq[sqid] = sq;

	sq->fd_qmem = fd_qmem;
	GK_PRINT("GK: SQ init, sqid[%d], cqid[%d], size[%d], dma_addr[0x%X]\n", sq->sqid, sq->cqid, sq->size, sq->dma_addr);
	syslog(LOG_DEBUG,"init SQ\n");

	return NVME_SUCCESS;
}

static uint16_t nvme_init_cq (NvmeCQ *cq, NvmeCtrl *n, uint64_t dma_addr, uint16_t cqid, uint16_t vector, uint16_t size, uint16_t irq_enabled, int contig)
{
	uint64_t cq_base = NULL;
	int fd_qmem = 0;

	if (dma_addr) {
		cq_base = mmap_queue_addr (n->host.io_mem.addr, dma_addr, cqid, &fd_qmem);
		if (!cq_base) {
			syslog(LOG_ERR,"%s Qmap ERR: 0x%p\n", __func__, (void *)dma_addr);
			return NVME_INVALID_QID;
		}
	}

	cq->ctrl = n;
	cq->cqid = cqid;
	cq->size = size;
	cq->phase = 1;
	cq->irq_enabled = irq_enabled;
	cq->vector = vector;
	cq->head = cq->tail = 0;
	cq->phys_contig = contig;

	if (cq->phys_contig) {
		cq->dma_addr = cq_base;
	} else {
#if 1
		cq->prp_list = NULL; //TODO
#else
		cq->prp_list = nvme_setup_discontig (n, dma_addr, size, n->cqe_size);
#endif
		if (!cq->prp_list) {
			return NVME_INVALID_FIELD | NVME_DNR;
		}
	}

	cq->db_addr = (uint64_t)((char *)n->fpga.nvme_regs + 0x1000 + ((cqid << 1) + 1) * (4 << n->db_stride));
	cq->eventidx_addr = 0;
	n->cq[cqid] = cq;
	cq->timer = create_nvme_timer (nvme_isr_notify, cq, \
			NVME_INTC_TIME(n->features.int_coalescing) * 100, SIGUSR1);
	cq->sq_list = calloc (1, sizeof (NvmeSQ));
	QTAILQ_INIT (cq->sq_list);
	cq->req_list = calloc (1, sizeof (NvmeRequest));
	QTAILQ_INIT (cq->req_list);

	cq->fd_qmem = fd_qmem;

	GK_PRINT("GK: CQ init, cqid[%d], size[%d], dma_addr[0x%X], vector[%d]\n", cq->cqid, cq->size, cq->dma_addr, cq->vector);
	syslog(LOG_INFO,"\n init CQ qid: %d irq_vector: %d\n", cqid, vector);
	return NVME_SUCCESS;
}

static int nvme_start_ctrl (NvmeCtrl *n)
{
	uint32_t page_bits = NVME_CC_MPS(n->nvme_regs.vBar.cc) + 12;
	uint32_t page_size = 1 << page_bits;

	syslog(LOG_DEBUG,"nvme start ctrl\n");

	if (n->cq[0] || n->sq[0] || !n->nvme_regs.vBar.asq || !n->nvme_regs.vBar.acq ||
			n->nvme_regs.vBar.asq & (page_size - 1) || n->nvme_regs.vBar.acq & (page_size - 1) ||
			NVME_CC_MPS(n->nvme_regs.vBar.cc) < NVME_CAP_MPSMIN(n->nvme_regs.vBar.cap) ||
			NVME_CC_MPS(n->nvme_regs.vBar.cc) > NVME_CAP_MPSMAX(n->nvme_regs.vBar.cap) ||
			NVME_CC_IOCQES(n->nvme_regs.vBar.cc) < NVME_CTRL_CQES_MIN(n->id_ctrl.cqes) ||
			NVME_CC_IOCQES(n->nvme_regs.vBar.cc) > NVME_CTRL_CQES_MAX(n->id_ctrl.cqes) ||
			NVME_CC_IOSQES(n->nvme_regs.vBar.cc) < NVME_CTRL_SQES_MIN(n->id_ctrl.sqes) ||
			NVME_CC_IOSQES(n->nvme_regs.vBar.cc) > NVME_CTRL_SQES_MAX(n->id_ctrl.sqes) ||
			!NVME_AQA_ASQS(n->nvme_regs.vBar.aqa) || NVME_AQA_ASQS(n->nvme_regs.vBar.aqa) > 4095 ||
			!NVME_AQA_ACQS(n->nvme_regs.vBar.aqa) || NVME_AQA_ACQS(n->nvme_regs.vBar.aqa) > 4095) {
		syslog (LOG_ERR,"nvme init values went bad\n");
		return -1;
	}

	n->page_bits = page_bits;
	n->page_size = 1 << n->page_bits;
	n->max_prp_ents = n->page_size / sizeof (uint64_t);
	n->cqe_size = 1 << NVME_CC_IOCQES(n->nvme_regs.vBar.cc);
	n->sqe_size = 1 << NVME_CC_IOSQES(n->nvme_regs.vBar.cc);
	n->running = 1;

	nvme_init_cq (&n->admin_cq, n, n->nvme_regs.vBar.acq, 0, 0, \
			NVME_AQA_ACQS(n->nvme_regs.vBar.aqa) + 1, 1, 1);
	nvme_init_sq (&n->admin_sq, n, n->nvme_regs.vBar.asq, 0, 0, \
			NVME_AQA_ASQS(n->nvme_regs.vBar.aqa) + 1, NVME_Q_PRIO_HIGH, 1);

	n->aer_queue = calloc (1,sizeof (*n->aer_queue));
	n->aer_timer = create_nvme_timer (nvme_aer_process_cb,n, \
			NVME_INTC_TIME(n->features.int_coalescing) * 100,SIGUSR2);
	QTAILQ_INIT (n->aer_queue);
#if (CUR_SETUP != TARGET_SETUP && CUR_SETUP != TARGET_RD_SETUP)
	irq_ctrl = (uint32_t *)g_NvmeCtrl.fpga.ep[0].bar[4].addr;
#endif
	return 0;
}

static void nvme_process_reg (NvmeCtrl *n, uint64_t offset, uint64_t data)
{
	switch (offset) {
		case  0x0c:
			syslog(LOG_INFO,"INTMS: %lx\r\n", data);
			n->nvme_regs.vBar.intms |= data & 0xffffffff;
			n->nvme_regs.vBar.intmc = n->nvme_regs.vBar.intms;
			break;
		case  0x10:
			n->nvme_regs.vBar.intms &= ~(data & 0xffffffff);
			n->nvme_regs.vBar.intmc = n->nvme_regs.vBar.intms;
			syslog(LOG_INFO,"INTMC: %lx\r\n", data);
		case  0x14:
			syslog(LOG_INFO,"CC: %lx\r\n", data);
			if (NVME_CC_EN(data) && !NVME_CC_EN(n->nvme_regs.vBar.cc)) {
				n->nvme_regs.vBar.cc = data;
				syslog(LOG_DEBUG,"Nvme EN!\r\n");
				if (nvme_start_ctrl(n)) {
					n->nvme_regs.vBar.csts = NVME_CSTS_FAILED;
				} else {
					n->nvme_regs.vBar.csts = NVME_CSTS_READY;
				}
				n->qsched.WRR = n->nvme_regs.vBar.cc & 0x3800;
				if((n->ns_check == DDR_NAND) || (n->ns_check == NAND_ALONE)){
					if (bdbm_drv_init (NULL) != 0) {
						printf("bdbm_drv_init fails ...\n");
					}
				}
			} else if (!NVME_CC_EN(data) && \
					NVME_CC_EN(n->nvme_regs.vBar.cc)) {
				syslog(LOG_DEBUG,"Nvme !EN\r\n");
				nvme_clear_ctrl(n);
				n->nvme_regs.vBar.cc = 0;
				n->nvme_regs.vBar.csts &= ~NVME_CSTS_READY;
			}
			if (NVME_CC_SHN(data) && \
					!(NVME_CC_SHN(n->nvme_regs.vBar.cc))) {
				printf("GK: received closing signal...\n");
				syslog(LOG_DEBUG,"Nvme SHN!\r\n");
				n->nvme_regs.vBar.cc = data;
				nvme_clear_ctrl(n);
				n->nvme_regs.vBar.csts |= NVME_CSTS_SHST_COMPLETE;
				n->nvme_regs.vBar.csts &= ~NVME_CSTS_READY;
				n->nvme_regs.vBar.cc = 0;
				if((n->ns_check == DDR_NAND) || (n->ns_check == NAND_ALONE)){
					printf("\n\tStoring Flash Management Info. MUST WAIT UNTIL STORING PROCESS IS DONE!!!...\t\n");
					//printf("\n\tStoring Flash Management Info. Please wait...\t\n");
					bdbm_drv_deinit();
				}
			} else if (!NVME_CC_SHN(data) && \
					NVME_CC_SHN(n->nvme_regs.vBar.cc)) {
				printf("GK: received closing signal...\n");
				syslog(LOG_DEBUG,"Nvme !SHN\r\n");
				n->nvme_regs.vBar.csts &= ~NVME_CSTS_SHST_COMPLETE;
				n->nvme_regs.vBar.cc = data;
			}
			n->fpga.nvme_regs->vBar.csts = n->nvme_regs.vBar.csts;
			break;
		case 0x20:
			n->nvme_regs.vBar.nssrc = data & 0xffffffff;
			break;
		case  0x24:
			syslog(LOG_INFO,"AQA: %lx\r\n", data);
			n->nvme_regs.vBar.aqa = data;
			break;
		case  0x28:
			syslog(LOG_INFO,"ASQ: %lx\r\n", data);
			n->nvme_regs.vBar.asq = data;
			break;
		case 0x2c:
			n->nvme_regs.vBar.asq |= data << 32;
			break;
		case 0x30:
			syslog(LOG_INFO,"ACQ: %lx\r\n", data);
			n->nvme_regs.vBar.acq = data;
			break;
		case 0x34:
			n->nvme_regs.vBar.acq |= data << 32;
			break;
		default:
			syslog(LOG_INFO,"%x?\r\n", (uint32_t)offset); 
			break;
	}
}

static void nvme_process_db (NvmeCtrl *n, uint64_t offset, uint64_t val)
{
	uint32_t qid;
	uint16_t new_val = val & 0xffff;
	NvmeSQ *sq;

	if (offset & ((1 << (2 + n->db_stride)) - 1)) {
		nvme_enqueue_event (n, NVME_AER_TYPE_ERROR, \
				NVME_AER_INFO_ERR_INVALID_DB, NVME_LOG_ERROR_INFO);
		syslog(LOG_ERR,"Bad DB offset!: %lx\n", offset);
		return;
	}

	if (((offset - 0x1000) >> (2 + n->db_stride)) & 1) {
		/*Door Bell's for CQ!*/
        // gunjae: what is CQ or SQ? - CQ=completion queue / SQ=submission queue
		NvmeCQ *cq;

		qid = (offset - (0x1000 + (1 << (2 + n->db_stride)))) >>
			(3 + n->db_stride);
		if (nvme_check_cqid (n, qid)) {
			nvme_enqueue_event (n, NVME_AER_TYPE_ERROR, \
					NVME_AER_INFO_ERR_INVALID_DB, NVME_LOG_ERROR_INFO);
			return;
		}

		cq = n->cq[qid];
		if (new_val >= cq->size) {
			nvme_enqueue_event (n, NVME_AER_TYPE_ERROR,
					NVME_AER_INFO_ERR_INVALID_DB, NVME_LOG_ERROR_INFO);
			return;
		}

		cq->hold_sqs = nvme_cq_full (cq) ? 1 : 0;

		if (cq->hold_sqs) {
			nvme_post_cqes (cq);
		} else if (cq->tail != cq->head) {
			nvme_isr_notify (cq);
		}
	} else {
		qid = (offset - 0x1000) >> (3 + n->db_stride);
		if (nvme_check_sqid (n, qid)) {
			nvme_enqueue_event (n, NVME_AER_TYPE_ERROR, \
					NVME_AER_INFO_ERR_INVALID_SQ, NVME_LOG_ERROR_INFO);
			return;
		}
		sq = n->sq[qid];
		if (new_val >= sq->size) {
			nvme_enqueue_event (n, NVME_AER_TYPE_ERROR,
					NVME_AER_INFO_ERR_INVALID_DB, NVME_LOG_ERROR_INFO);
			return;
		}

		/*set events to SQ_threads with this cqid for SQ processor -TODO*/
        // gunjae: it can be SQ related process
		nvme_process_sq (sq);
	}
}

static void nvme_set_default (NvmeCtrl *n)
{
	GK_PRINT("GK: nvme_set_default()::");
	/*default values to be updated -TODO*/
#if 0
	n->conf;
	n->serial;
#endif
	if( n->ns_check == DDR_NAND) {
		n->num_namespaces = 2;
	} else {
		n->num_namespaces = 1;
	}
	n->num_queues = 64;
	n->max_q_ents = 0x7ff;
	n->max_cqes = 0x4;
	n->max_sqes = 0x6;
	n->db_stride = 0;
	n->aerl = 3;
	n->acl = 3;
	n->elpe = 3;
	n->mdts = 5;
	n->cqr = 1;
	n->vwc = 0;
	n->intc = 0;
	n->intc_thresh = 0;
	n->intc_time = 0;
	n->mpsmin = 0;
	n->mpsmax = 0;
	n->nlbaf = 4;                 /*For LBA size 512B:1 4KB: 4*/
	n->lba_index = 3;             /*For LBA size 512B:0 4KB: 3*/
	n->extended = 0;
	n->dpc = 0;
	n->dps = 0;
	n->mc = 0;
	n->meta = 0;
	n->cmb = 0;
	n->oacs = NVME_OACS_FORMAT;
	n->oncs = NVME_ONCS_DSM;
	n->vid = PCI_VENDOR_ID_INTEL;
	n->did = 0x5845;
	GK_PRINT("done\n");
}

inline void nvme_regs_setup (NvmeCtrl *n)
{
#if 1
	/*If only we have FPGA to have default register values */
	memcpy (&n->nvme_regs, (uint64_t *)(n->fpga.ep[0].bar[2].addr + 0x2000), sizeof (NvmeRegs));
#else
	memcpy (&n->nvme_regs, &NvmeRegsReset, sizeof (NvmeRegs));
#endif
}

static void nvme_init_ctrl (NvmeCtrl *n)
{
	int i;
	NvmeIdCtrl *id = &n->id_ctrl;
	id->rab = 6;
	id->ieee[0] = 0x00;
	id->ieee[1] = 0x02;
	id->ieee[2] = 0xb3;
	id->cmic = 0;
	id->mdts = n->mdts;
	id->oacs = htole16(n->oacs);
	id->acl = n->acl;
	id->aerl = n->aerl;
	id->frmw = 7 << 1 | 1;
	id->lpa = 0 << 0;
	id->elpe = n->elpe;
	id->npss = 0;
	id->sqes = (n->max_sqes << 4) | 0x6;
	id->cqes = (n->max_cqes << 4) | 0x4;
	id->nn = htole32(n->num_namespaces);
	id->oncs = htole16(n->oncs);
	id->fuses = htole16(0);
	id->fna = 0;
	id->vwc = n->vwc;
	id->awun = htole16(0);
	id->awupf = htole16(0);
	id->psd[0].mp = htole16(0x9c4);
	id->psd[0].enlat = htole32(0x10);
	id->psd[0].exlat = htole32(0x4);

	n->features.arbitration     = 0x1f0f0706;
	n->features.power_mgmt      = 0;
	n->features.temp_thresh     = 0x14d;
	n->features.err_rec         = 0;
	n->features.volatile_wc     = n->vwc;
	n->features.num_queues      = (n->num_queues - 1) |
		((n->num_queues - 1) << 16);
	n->features.int_coalescing  = n->intc_thresh | (n->intc_time << 8);
	n->features.write_atomicity = 0;
	n->features.async_config    = 0x0;
	n->features.sw_prog_marker  = 0;

	n->features.int_vector_config = calloc (1, n->num_queues *
			sizeof (*n->features.int_vector_config));
	for (i = 0; i < n->num_queues; i++) {
		n->features.int_vector_config[i] = i | (n->intc << 16);
	}

#if 1
	nvme_regs_setup (n);
#else
	n->nvme_regs.vBar.cap = 0;
	NVME_CAP_SET_MQES(n->nvme_regs.vBar.cap, n->max_q_ents);
	NVME_CAP_SET_CQR(n->nvme_regs.vBar.cap, n->cqr);
	NVME_CAP_SET_AMS(n->nvme_regs.vBar.cap, 1);
	NVME_CAP_SET_TO(n->nvme_regs.vBar.cap, 0xf);
	NVME_CAP_SET_DSTRD(n->nvme_regs.vBar.cap, n->db_stride);
	NVME_CAP_SET_NSSRS(n->nvme_regs.vBar.cap, 0);
	NVME_CAP_SET_CSS(n->nvme_regs.vBar.cap, 1);
	NVME_CAP_SET_MPSMIN(n->nvme_regs.vBar.cap, n->mpsmin);
	NVME_CAP_SET_MPSMAX(n->nvme_regs.vBar.cap, n->mpsmax);
	if (n->cmb) {
		n->nvme_regs.vBar.vs = 0x00010200;
	} else {
		n->nvme_regs.vBar.vs = 0x00010100;
	}
	n->nvme_regs.vBar.intmc = n->nvme_regs.vBar.intms = 0;
#endif

	n->temperature = NVME_TEMPERATURE;

	n->sq = calloc (1, sizeof (NvmeCQ)*((n->features.num_queues & 0xfffe) + 1));
	n->cq = calloc (1, sizeof (NvmeSQ)*((n->features.num_queues >> 16) + 1));

	n->elpes = calloc (1, (n->elpe + 1) * sizeof (*n->elpes));
	n->aer_reqs = calloc (1, (n->aerl + 1) * sizeof (*n->aer_reqs));
}

static void nvme_init_namespaces (NvmeCtrl *n)
{
	int i, j;

	n->namespaces = calloc (1, sizeof (NvmeNamespace) * n->num_namespaces);

	for (i = 0; i < n->num_namespaces; i++) {
		uint64_t blks;
		int lba_index;
		NvmeNamespace *ns = &n->namespaces[i];
		NvmeIdNs *id_ns = &ns->id_ns;

		id_ns->nsfeat = 0;
		id_ns->nlbaf = n->nlbaf - 1;
		id_ns->flbas = n->lba_index | (n->extended << 4);
		id_ns->mc = n->mc;
		id_ns->dpc = n->dpc;
		id_ns->dps = n->dps;

		for (j = 0; j < n->nlbaf; j++) {
			id_ns->lbaf[j].ds = BDRV_SECTOR_BITS + j;
		}

		lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
		blks = n->ns_size[i] / ((1 << id_ns->lbaf[lba_index].ds));
		id_ns->nuse = id_ns->ncap = id_ns->nsze = htole64(blks);

		ns->id = i + 1;
		ns->ctrl = n;
		//ns->start_block = i * n->ns_size[i] >> BDRV_SECTOR_BITS;
		ns->start_block = 0;
#if 0
		ns->util = bitmap_new(blks);
		ns->uncorrectable = bitmap_new(blks);
#endif
		syslog(LOG_INFO,"%s nsize: 0x%llx blk: 0x%llx ds: %d start_block: %llx\n", __func__, \
				(long long unsigned int)n->ns_size[i], \
				(long long unsigned int)id_ns->nsze, \
				(int)id_ns->lbaf[0].ds, (long long unsigned int)ns->start_block);

	}
}

static int nvme_init_pci (NvmeCtrl *n)
{
	GK_PRINT("GK: nvme_init_pci()::");
	int ret = 0;
#if 0
	/*pci init is needed right here? -TODO*/
	uint8_t *pci_conf = n->parent_obj.config;

	pci_conf[PCI_INTERRUPT_PIN] = 1;
	pci_config_set_prog_interface (pci_conf, 0x2);
	pci_config_set_vendor_id (pci_conf, n->vid);
	pci_config_set_device_id (pci_conf, n->did);
	pci_config_set_class (pci_conf, PCI_CLASS_STORAGE_EXPRESS);
	pcie_endpoint_cap_init (&n->parent_obj, 0x80);

	memory_region_init_io (&n->iomem, OBJECT(n), \
			&nvme_mmio_ops, n, "nvme", n->reg_size);
	pci_register_bar(&n->parent_obj, 0, \
			PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64,
			&n->iomem);
	msix_init_exclusive_bar(&n->parent_obj, n->num_queues, 4);
	msi_init (&n->parent_obj, 0x50, 32, true, false);

	if (n->cmb) {

		/* If CMB is requested it is setup here. Currently n->cmb
		 * determines size in MB. All other properties are hard-coded
		 * (for now) to:
		 * CMBSZ  = SQS=1, CQS=1, LISTS=1, RDS=0, WDS=0, SZU=1MB
		 * CMBLOC = BIR=2, OFST=0
		 */

		n->nvme_regs.cmbloc = 0;
		NVME_CMBLOC_SET_BIR(n->nvme_regs.cmbloc, 2);
		NVME_CMBLOC_SET_OFST(n->nvme_regs.cmbloc, 0);

		n->nvme_regs.cmbsz = 0;
		NVME_CMBSZ_SET_SQS(n->nvme_regs.cmbsz,   1);
		NVME_CMBSZ_SET_CQS(n->nvme_regs.cmbsz,   1);
		NVME_CMBSZ_SET_LISTS(n->nvme_regs.cmbsz, 1);
		NVME_CMBSZ_SET_RDS(n->nvme_regs.cmbsz,   0);
		NVME_CMBSZ_SET_WDS(n->nvme_regs.cmbsz,   0);
		NVME_CMBSZ_SET_SZU(n->nvme_regs.cmbsz,   2);
		NVME_CMBSZ_SET_SZ(n->nvme_regs.cmbsz,    n->cmb);

		n->cmbuf = calloc (1, NVME_CMBSZ_GETSIZE(n->nvme_regs.cmbsz));
		memory_region_init_io (&n->ctrl_mem, OBJECT(n), &nvme_cmb_ops, \
				n, "nvme-cmb", NVME_CMBSZ_GETSIZE(n->nvme_regs.cmbsz));
		pci_register_bar(&n->parent_obj, NVME_CMBLOC_BIR(n->nvme_regs.cmbloc), \
				PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64, \
				&n->ctrl_mem);
	}
#endif
	GK_PRINT("mmap_fpga_bars(), ");
	RETURN_ON_ERROR (mmap_fpga_bars (&n->fpga, n->fd_uio), ret, " ");
	GK_PRINT("setupFpgaCtrlRegs(), ");
	RETURN_ON_ERROR (setupFpgaCtrlRegs (&n->fpga), ret, " ");
	GK_PRINT("mmap_host_mem(), ");
	RETURN_ON_ERROR (mmap_host_mem (&n->host), ret, " ");

	// gunjae: monitoring
	GK_PRINT("GK: FPGA.fifo_reg_ptr = 0x%X\n", n->fpga.fifo_reg);
	GK_PRINT("GK: FPGA.fifo_count_ptr = 0x%X\n", n->fpga.fifo_count);
	GK_PRINT("GK: FPGA.fifo_msi_ptr = 0x%X\n", n->fpga.fifo_msi);
	GK_PRINT("GK: FPGA.gpio_csr_ptr = 0x%X\n", n->fpga.gpio_csr);

	GK_PRINT("GK: HOST.io_mem a[0x%X] sz[%d] v[%d]\n", n->host.io_mem.addr, n->host.io_mem.size, n->host.io_mem.is_valid);
	GK_PRINT("GK: HOST.msix_mem a[0x%X] sz[%d] v[%d]\n", n->host.msix_mem.addr, n->host.msix_mem.size, n->host.msix_mem.is_valid);

	GK_PRINT("done\n");

	return 0;
}

static inline void nvme_deinit_pci (NvmeCtrl *n)
{
	munmap_host_mem (&n->host);
	munmap_fpga_bars (&n->fpga);
	SAFE_CLOSE (n->fd_uio[0]);
	SAFE_CLOSE (n->fd_uio[1]);
}

static int nvme_check_constraints (NvmeCtrl *n)
{
	if ( /*(!(n->conf.blk)) || !(n->serial) ||*/
			(n->num_namespaces == 0 || n->num_namespaces > NVME_MAX_NUM_NAMESPACES) ||
			(n->num_queues < 1 || n->num_queues > NVME_MAX_QS) ||
			(n->db_stride > NVME_MAX_STRIDE) ||
			(n->max_q_ents < 1) ||
			(n->max_sqes > NVME_MAX_QUEUE_ES || n->max_cqes > NVME_MAX_QUEUE_ES ||
			 n->max_sqes < NVME_MIN_SQUEUE_ES || n->max_cqes < NVME_MIN_CQUEUE_ES) ||
			(n->vwc > 1 || n->intc > 1 || n->cqr > 1 || n->extended > 1) ||
			(n->nlbaf > 16) ||
			(n->lba_index >= n->nlbaf) ||
			(n->meta && !n->mc) ||
			(n->extended && !(NVME_ID_NS_MC_EXTENDED(n->mc))) ||
			(!n->extended && n->meta && !(NVME_ID_NS_MC_SEPARATE(n->mc))) ||
			(n->dps && n->meta < 8) ||
			(n->dps && ((n->dps & DPS_FIRST_EIGHT) &&
				    !NVME_ID_NS_DPC_FIRST_EIGHT(n->dpc))) ||
			(n->dps && !(n->dps & DPS_FIRST_EIGHT) &&
			 !NVME_ID_NS_DPC_LAST_EIGHT(n->dpc)) ||
			(n->dps & DPS_TYPE_MASK && !((n->dpc & NVME_ID_NS_DPC_TYPE_MASK) &
						     (1 << ((n->dps & DPS_TYPE_MASK) - 1)))) ||
			(n->mpsmax > 0xf || n->mpsmax < n->mpsmin) ||
			(n->oacs & ~(NVME_OACS_FORMAT)) ||
			(n->oncs & ~(NVME_ONCS_COMPARE | NVME_ONCS_WRITE_UNCORR |
				     NVME_ONCS_DSM | NVME_ONCS_WRITE_ZEROS))) {
					     return -1;
				     }
	return 0;
}

static inline int nvme_init_q_scheduler (NvmeCtrl *n)
{
	NvmeQSched *qs = &n->qsched;
	int i;

#if 0
#if USING_MSGQ_FROM_SYSV
	struct msqid_ds buf;

	do {
		qs->mq_txid = msgget (1234, IPC_CREAT | 0666 );
		rc = msgctl(qs->mq_txid, IPC_STAT, &buf);
		if (buf.msg_qnum) {
			msgctl(qs->mq_txid, IPC_RMID, 0);
		}
	} while(buf.msg_qnum);
#else
	mq_unlink (NVME_MQ_NAME);
	struct mq_attr mqAttr = {0, NVME_MQ_MAXMSG, NVME_MQ_MSGSIZE, 0, 0};
	qs->mq_txid = mq_open (NVME_MQ_NAME, O_RDWR|O_CREAT, S_IWUSR|S_IRUSR, &mqAttr);
	if (qs->mq_txid < 0) {
		perror ("qs->mq_txid");
		return -1;
	}
	qs->time.tv_sec = 1;
	qs->time.tv_nsec = 0;
#endif
#endif
#ifdef SPINLOCK
	if(spin_init(&n->qs_req_spin,0)) {
		perror("qs spin init()");
	}
#else
	mutex_init(&n->qsch_mutex, NULL);
#endif
	qs->iodbst_reg = (uint32_t *)n->fpga.ep[0].bar[4].addr + 0x05;

	for(i = 0;  i < NVME_MAX_PRIORITY; i++) {
		qs->shadow_regs[i][0] = qs->shadow_regs[i][1] = 0x0;
		qs->mask_regs[i][0] = qs->mask_regs[i][1] = 0x0;
	}
	return 0;
}

static int nvme_init (NvmeCtrl *n)
{
	int ret = 0;
	nvme_drv_params *drv_params;

	//GK_PRINT("GK: nvme_init_pci() will be called...\n");
	RETURN_ON_ERROR (nvme_init_pci (n), ret, " ");

	GK_PRINT("GK: nvme_set_default() - nvme will be set with default values...\n");
	nvme_set_default (n);
	n->start_time = time (NULL);

	//GK_PRINT("GK: nvme_bio_init() will be called...\n");
	RETURN_ON_ERROR (nvme_bio_init (n), ret, "Failed Memory Drive Setup!\n");
	if((n->ns_check == DDR_NAND) || (n->ns_check == NAND_ALONE)){
		GK_PRINT("GK: bdbm_drv_init() will be called...\n");
		if (bdbm_drv_init (&drv_params) != 0) {
			printf("bdbm_init failed \n");
			FREE_VALID (drv_params);
			return -1;
		}
		n->drv_params = drv_params;
	}
#if (CUR_SETUP == TARGET_SETUP || CUR_SETUP == STANDALONE_SETUP)
	/*n->ns_size = 64GB;*/
	/*FTL Part might bring changes here*/
	/*Get Namespace details here -TODO*/
//	n->ns_size = (uint64_t)0x200000000/(uint64_t)n->num_namespaces;
	if( n->ns_check == DDR_NAND ) {
		n->ns_size[0] = (uint64_t)n->namespace_size[0];
		n->ns_size[1] = (n->drv_params->npages * n->drv_params->page_size);
	} else if( n->ns_check == DDR_ALONE) {
		n->ns_size[0] = (uint64_t)n->namespace_size[0];
	} else {
		n->ns_size[0] = (n->drv_params->npages * n->drv_params->page_size);
	}
	syslog(LOG_INFO,"ns0 size:%lx\n",n->ns_size[0]);
	syslog(LOG_INFO,"ns1 size:%lx\n",n->ns_size[1]);
#else
	n->ns_size[0] = RAMDISK_SZ_MB/(uint64_t)n->num_namespaces;
#endif

	nvme_init_ctrl (n);
	RETURN_ON_ERROR (nvme_check_constraints (n), ret, "Constricted Constraints!\n");
	nvme_init_namespaces (n);
	RETURN_ON_ERROR (nvme_init_q_scheduler (n), ret, "Q scheduler setup failed!\n");

	mq_unlink (NVME_MQ_NAME);
	struct mq_attr mqAttr = {0, NVME_MQ_MAXMSG, NVME_MQ_MSGSIZE, 0};
	n->mq_txid = mq_open (NVME_MQ_NAME, O_RDWR|O_CREAT, S_IWUSR|S_IRUSR, &mqAttr);
	if (n->mq_txid < 0) {
		perror ("n->mq_txid");
		return -1;
	}

	return 0;
}

#if 0
typedef struct MsgBuf {
	long type;
	int msg;
} MsgBuf;

static inline int nvme_schedule_sq (int sqid, int mq_txid)
{
	int ret = 0;
#if USING_MSGQ_FROM_SYSV
	MsgBuf sBuf = {1, sqid};
	do {
		if ((ret = msgsnd(mq_txid, &sBuf, (sizeof(MsgBuf)), 0)) < 0) {
			perror("msgsnd");
		}
	} while (ret < 0);
#else
	do {
		if ((ret = mq_send(mq_txid, (char *)&sqid, sizeof (int), 1)) < 0) {
			perror("mq_send");
		}
	} while (ret < 0);
#endif
	return 0;
}

static inline int nvme_fetch_qid (uint32_t msg_qid)
{
#if USING_MSGQ_FROM_SYSV
	MsgBuf rBuf;
	/*memset(&rBuf, 0, sizeof(rBuf));*/
	msgrcv (msg_qid, &rBuf, sizeof(MsgBuf), 1, 0);
	return rBuf.msg;
#else
	int n = 0;
	int ret = mq_timedreceive (msg_qid, (char *)&n, sizeof (int), NULL, &g_NvmeCtrl.qsched.time);
	return (ret == -1) ? -1: n;
#endif

}
#endif

static void nvme_update_shadowregs(NvmeQSched *qs)
{
	NvmeCtrl *n = &g_NvmeCtrl;
	NvmeSQ *sq;
	int i, j, n_regs = 0, limit = 0, base = 0;
	uint32_t status_regs[4] = {0,0,0,0};
	uint32_t current_sq = 0;

	n_regs = ((qs->n_active_iosqs - 1) >> 5) + 1;
	for(i = 0; i < n_regs; i++){
		status_regs[i] = *(qs->iodbst_reg + i);
		current_sq = (~status_regs[i]) & qs->SQID[i];

		if(current_sq){
			base = i << 5;
			limit = (qs->n_active_iosqs < (base + 32)) ? (qs->n_active_iosqs - base) : 32;
			for(j = 0; j < limit; j++) {
				if(current_sq & (1UL << j)) {
					sq = n->sq[(j + 1) + base];
					if(sq) {
						nvme_update_sq_tail (sq);
						if(!nvme_sq_empty(sq)) {
							status_regs[i] |= (1UL << j);
						}
					}
				}
			}
		}
	} 
#ifdef SPINLOCK
	spin_lock(&n->qs_req_spin);
	for(i = 0; i < NVME_MAX_PRIORITY; i++) {
		if(qs->prio_avail[i]) {
			qs->shadow_regs[i][0] |= (((((uint64_t)status_regs[1]) << 32) + status_regs[0]) & qs->mask_regs[i][0]);
			qs->shadow_regs[i][1] |= (((((uint64_t)status_regs[3]) << 32) + status_regs[2]) & qs->mask_regs[i][1]);
		}
	}
	spin_unlock(&n->qs_req_spin);
#else
	mutex_lock(&n->qsch_mutex);
	for(i = 0; i < NVME_MAX_PRIORITY; i++) {
		if(qs->prio_avail[i]) {
			qs->shadow_regs[i][0] |= (((((uint64_t)status_regs[1]) << 32) + status_regs[0]) & qs->mask_regs[i][0]);
			qs->shadow_regs[i][1] |= (((((uint64_t)status_regs[3]) << 32) + status_regs[2]) & qs->mask_regs[i][1]);
		}
	}
	mutex_unlock(&n->qsch_mutex);
#endif
}

static int nvme_get_best_sqid(NvmeQSched *qs)
{
	unsigned int prio = 0, base = 0, i = 0, bit = 0, dw_idx = 0;
	unsigned int end_ptr = 0, reg_limit_bit = 0, n_regs = 0, q_range = 0;
	uint64_t tmp = 0x0;

	nvme_update_shadowregs(qs);

	q_range = qs->n_active_iosqs - 1;
	n_regs = (q_range >> 6) + 1;
	for(prio = 0; prio < NVME_MAX_PRIORITY; prio++) {
		if(qs->prio_avail[prio]) {
			dw_idx = qs->prio_lvl_next_q[prio] >> 6;
			end_ptr = (!qs->prio_lvl_next_q[prio]) ? 0 : ((qs->prio_lvl_next_q[prio] - 1) & (SHADOW_REG_SZ - 1));

			for(i = 0; i <= n_regs; i++) {
				tmp = qs->shadow_regs[prio][dw_idx];
				if (tmp) {
					base = dw_idx << 6;
					reg_limit_bit = (i < n_regs) ? (SHADOW_REG_SZ - 1) : end_ptr;
					reg_limit_bit = ((reg_limit_bit + base) < qs->n_active_iosqs) ? reg_limit_bit : (q_range - base);
					for(bit = qs->prio_lvl_next_q[prio] - base; bit <= reg_limit_bit; bit++) {
						qs->prio_lvl_next_q[prio] = ((bit + 1) & (SHADOW_REG_SZ - 1)) + base;
						qs->prio_lvl_next_q[prio] = (qs->prio_lvl_next_q[prio] > q_range) ? 0 : qs->prio_lvl_next_q[prio];
						if(tmp & (1UL << bit)) {
							return (bit + base + 1);
						}
					}
				} else {
					qs->prio_lvl_next_q[prio] = ((dw_idx + 1) & (~n_regs)) << 6;
				}
				dw_idx = ((dw_idx + 1) & (~n_regs));
			}
		}
	}
	return 0;
}

static void weighted_round_robin (NvmeCtrl *n)
{
	/*FpgaIrqs *in_irqs = &g_NvmeCtrl.in_irqs;*/
	NvmeQSched *qs = &n->qsched;
	int sqid = 0;

	do {
		sqid = nvme_get_best_sqid(qs);
		if(sqid){
			nvme_process_sq (n->sq[sqid]);
			/*nvme_schedule_sq (sqid, qs->mq_txid);*/
		} else {
			break;
		}
	} while (!(*n->fpga.fifo_count));
}

static void round_robin (NvmeCtrl *n)
{
	uint32_t status_reg = 0;
	int n_regs = 0, i = 0, j = 0, limit = 0, base = 0;
	NvmeQSched *qs = &n->qsched;
	NvmeSQ *sq = NULL;

	n_regs = ((qs->n_active_iosqs - 1) >> 5) + 1;
	for(i = 0; i < n_regs; i++) {
		status_reg = *(qs->iodbst_reg + i);
#ifdef SPINLOCK
		spin_lock(&n->qs_req_spin);
		qs->round_robin_status_regs[i] |= status_reg;
		spin_unlock(&n->qs_req_spin);
#else
		mutex_lock(&n->qsch_mutex);
		qs->round_robin_status_regs[i] |= status_reg;
		mutex_unlock(&n->qsch_mutex);
#endif

		base = i << 5;
		limit = (qs->n_active_iosqs < (base + 32)) ? (qs->n_active_iosqs - base) : 32;
		for(j = 0; j < limit; j++) {
			if((sq = n->sq[(j + 1) + base])) {
				if(qs->round_robin_status_regs[i] & (1UL << j)) {
					nvme_process_sq(sq);
				} else {
					nvme_update_sq_tail (sq);
					if(!nvme_sq_empty(sq)) {
						nvme_process_sq(sq);
					}
				}
			}
		}
	}
}

static inline void nvme_q_scheduler (NvmeCtrl *n)
{
	if(n->qsched.WRR) {
		weighted_round_robin(n);
	} else {
		round_robin(n);
	}
}

// gunjae: generate block io commands to FTL
void *io_processor (void *arg)
{
	NvmeCtrl *n = &g_NvmeCtrl;
	int ret = 0;
	NvmeRequest *req = NULL;
	NvmeCQ *cq;
	uint64_t rev_addr;

	set_thread_affinity (2, pthread_self ());
	mutex_init(&n->req_mutex, NULL);
#ifdef SPINLOCK
	if(spin_init(&n->aer_req_spin,0)) {
		perror("aer spin_init()");
	}
#endif
	int mq_rxid = mq_open (NVME_MQ_NAME, O_RDWR);

	while(1) {
		rev_addr = 0;
		// gunjae: receive something from host
		if((mq_receive (mq_rxid, (char *)&rev_addr, sizeof (uint64_t), NULL)) < 0) {
			perror("mq_receive");
		}
		req = (NvmeRequest *)rev_addr;
		if(unlikely(req == NULL)) {
			printf("req:NULL\n");	
			continue;
		}
		// gunjae: generate requests here? -> make_dma_desc
#ifdef STATIC_BIO
		ret = nvme_bio_request(&req->bio);
#else
		ret = nvme_bio_request(req->bio);
#endif
		if(ret) {
			syslog(LOG_ERR,"failed ftl bio req: %x\n", ret);
#ifndef STATIC_BIO
			nvme_bio_destroy (&req->bio);
#endif
			req->status = NVME_DATA_TRAS_ERROR;
			cq = n->cq[req->sq->cqid];
			nvme_enqueue_req_completion (cq, req);
		}

#if (CUR_SETUP != TARGET_SETUP && CUR_SETUP != STANDALONE_SETUP)
		req->status = NVME_SUCCESS;
#ifndef STATIC_BIO
		nvme_bio_destroy (&req->bio);
#endif
		if(req->is_write) {
			n->stat.nr_bytes_written[0] += (req->nlb * (2 << ((9 + req->lba_index)-1))) >> 9;
		}
		else {
			n->stat.nr_bytes_read[0] += (req->nlb * (2 << (9 + req->lba_index)-1)) >> 9;
		}
		cq = n->cq[req->sq->cqid];
		nvme_enqueue_req_completion (cq, req);
#endif
	}
}

static inline void nvme_await_update_irqs (FpgaIrqs *in_irqs)
{
	/*Get all pending IRQ info (number and count for each irq) -TODO*/
}

// gunjae: main function is here
int main (int argc, char **argv)
{
	int ret = 0;
	fifo_data entry;
	NvmeCtrl *n = &g_NvmeCtrl;
	uint16_t fifo_count = 0;
	uint32_t *fifo_count_ptr;
	/*FpgaIrqs *in_irqs;*/
	pthread_t io_processor_tid;
	pthread_t io_completer_tid_ddr;
	pthread_t io_completer_tid_nand;
#if ((GK_TEST_RD_NAND==3) || (GK_TEST_RD_NAND==4))
	pthread_t io_completer_tid_nand_rd;
#endif
#if (GK_TEST_RD_NAND==4)
	pthread_t proc_completer_tid_nand;
#endif

#if (GK_MEASURE_TIME)
	char et_name[64];

	n->nand_acc_cnt = 0;
	n->nand_acc_clks = 0;
	n->nand_acc_time = 0.0;
	//n->nand_bw_init_clks = 0;
	clock_gettime(CLOCK_MONOTONIC, &(n->nand_bw_init_time));
	n->dram_acc_cnt = 0;
	n->dram_acc_clks = 0;
	n->dram_bw_init_clks = 0;
	//n->ls2_acc_cnt = 0;
	//n->ls2_acc_clks = 0;
	//n->ls2_acc_time = 0.0;
	////n->ls2_bw_init_clks = 0;
	//clock_gettime(CLOCK_MONOTONIC, &(n->ls2_bw_init_time));

	strcpy(et_name, "LS2_DATA");
	init_exec_time( &(n->et_ls2_data), et_name );
	strcpy(et_name, "LS2_CTRL");
	init_exec_time( &(n->et_ls2_ctrl), et_name );
	strcpy(et_name, "PROC");
	init_exec_time( &(n->et_proc), et_name );
	//n->func_acc_cnt = 0;
	//n->func_acc_clks = 0;
	//n->func_acc_time = 0.0;
	//n->func_bw_init_clks = 0;
	GK_PRINT("GK: GK_MEASURE_TIME is enabled, CLOCKS_PER_SEC = %lld\n", CLOCKS_PER_SEC);
#endif

	set_thread_affinity (0, pthread_self ());
	openlog("NVME",LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL0);
	syslog(LOG_INFO,"iSSD Software running on: %s\n", \
			(CUR_SETUP == TARGET_SETUP) ? "TARGET_SETUP" : \
			(CUR_SETUP == STANDALONE_SETUP) ? "STANDALONE_SETUP" : \
			(CUR_SETUP == LS2_TEST_SETUP)? "LS2_TEST_SETUP" : \
			(CUR_SETUP == X86_TEST_SETUP)? \
			"X86_TEST_SETUP" : "TARGET_RD_SETUP");

	// gunjae: SIGINT (kill -2) handler is set here
	GK_PRINT("GK: SIGINT (kill -2) handler is set up...\n");
	GK_PRINT("GK: GK_TEST_RD_NAND = %d\n", GK_TEST_RD_NAND);
	GK_PRINT("GK: IN_PROC_RATE = %d\n", IN_PROC_RATE);
	GK_PRINT("GK: GK_DMA_DELAY = %d\n", GK_DMA_DELAY);
	GK_PRINT("GK: GK_PROC_DELAY_QUERY6 = %d\n", GK_PROC_DELAY_QUERY6);
	GK_PRINT("GK: GK_PROC_DELAY_QUERY1 = %d\n", GK_PROC_DELAY_QUERY1);
	GK_PRINT("GK: GK_PROC_DELAY_QUERY14 = %d\n", GK_PROC_DELAY_QUERY14);
//	GK_PRINT("GK: GK_PROC_DELAY_PREFIX_SIMILARITY = %d\n", GK_PROC_DELAY_PREFIX_SIMILARITY);
	printf("GK: GK_TEST_RD_NAND = %d\n", GK_TEST_RD_NAND);
	printf("GK: IN_PROC_RATE = %d\n", IN_PROC_RATE);
	printf("GK: GK_DMA_DELAY = %d\n", GK_DMA_DELAY);
	printf("GK: GK_PROC_DELAY_QUERY6 = %d\n", GK_PROC_DELAY_QUERY6);
//	printf("GK: GK_PROC_DELAY_PREFIX_SIMILARITY = %d\n", GK_PROC_DELAY_PREFIX_SIMILARITY);
	printf("GK: GK_PROC_DELAY_QUERY1 = %d\n", GK_PROC_DELAY_QUERY1);
	printf("GK: GK_PROC_DELAY_QUERY14 = %d\n", GK_PROC_DELAY_QUERY14);
	RETURN_ON_ERROR (setup_ctrl_c_handler (), ret, " ");

#if (CUR_SETUP == TARGET_SETUP || CUR_SETUP == STANDALONE_SETUP)
	RETURN_ON_ERROR (get_dimm_info(n), ret, " ");

	if(n->ns_check != DDR_ALONE) {
		// gunjae: uio_rc is mapped (read_mem_addr??)
		GK_PRINT("GK: uio_rc is mapped by read_mem_addr()\n");
		ret = read_mem_addr();
		if(ret) {
			perror("read_mem_addr:");
			return -1;
		}
#ifndef MGMT_DATA_ON_NAND
	    // gunjae: some prints
	    GK_PRINT("GK: MGMT_DATA_ON_NAND is not defined. FTL table will be stored in MicroSD\n");
		GK_PRINT("GK: /dev/mmcblk0p1 will be mounted on /run/ftl_pmt\n");
	    //
		if(system("mkdir -p /run/ftl_pmt &> /dev/null")) {
			perror("mkdir -p /run/ftl_pmt ERROR");
		}
		if(system(" ls /dev/mmcblk0p1 &> /dev/null")){
			printf("SD card not available. Storing Page Mapping Table is not Possible. Excluding NAND namespace\n");
			if( (n->ns_check == DDR_NAND) ) {
				n->ns_check = DDR_ALONE;
			} else if (n->ns_check == NAND_ALONE) {
				n->ns_check == NO_NAMESPACE;
				return -1;
			}
		} else  {
			GK_PRINT("GK: FTL table will be mounted on /dev/mmcblk0p1\n");
			GK_PRINT("GK: /run/ftl_pmt/metaData.tar.gz will be checked.. This is a page mapping table\n");
	
			system ("umount /dev/mmcblk0p1 &> /dev/null");
			if( system ("mount /dev/mmcblk0p1 /run/ftl_pmt &>/dev/null")) {
				perror("mount ftl_pmt ERROR");
			}
			// gunjae: it looks like FTL data is managed in metaData.tar.gz, What will happen if it doesn't exist?
			if (system (" ls /run/ftl_pmt/metaData.tar.gz &> /dev/null")){
				printf(" Page Mapping Files Not found. It can be Due to Either New DIMM or Missing of Page Mapping Table in SD card\n" \
						"**********Proceeding With Erase***********\n");
				if(system("erase_nand")){
					printf("Erase nand failed\n");
				} else { printf ("\tNAND Erase Completed\n\n");}
			}
		}
#endif
    }   // if(n->ns_check != DDR_ALONE)
#endif
#if (CUR_SETUP == STANDALONE_SETUP)
	n->ns_check = DDR_ALONE ;
#endif
	if (n->ns_check == DDR_NAND) {
		// gunjae: default mode
		printf("\n===============================================================================\n");
		printf("\tNamespace 1 -> DDR(nvme0n1) ,  Namespace 2 -> NAND(nvme0n2)\n");
		printf("===============================================================================\n");
	} else if (n->ns_check == DDR_ALONE) {
		printf("\n===============================================================================\n");
		printf("\t\t\tNamespace 1 -> DDR (nvme0n1)\n");
		printf("===============================================================================\n");
	} else if (n->ns_check == NAND_ALONE){
		printf("\n===============================================================================\n");
		printf("\t\t\tNamespace 1 -> NAND (nvme0n1)\n");
		printf("===============================================================================\n");
	}

#if (CUR_SETUP == TARGET_SETUP || CUR_SETUP == STANDALONE_SETUP)
	if(n->ns_check == NAND_ALONE || n->ns_check == DDR_NAND) {
#ifdef COMT3
		GK_PRINT("GK: io_completer is generated (COMT3 is defined)\n");
		JUMP_ON_ERROR (pthread_create (&io_completer_tid_nand, NULL, df_io_completer, (void *)n), \
				ret, "io_completer: %s\n", strerror(errno));
	#if ((GK_TEST_RD_NAND==3) || (GK_TEST_RD_NAND==4))
		GK_PRINT("GK: io_rd_completer is generated (COMT3 is defined)\n");
		JUMP_ON_ERROR (pthread_create (&io_completer_tid_nand_rd, NULL, df_io_rd_completer, (void *)n), \
				ret, "io_rd_completer: %s\n", strerror(errno));
	#endif	// GK_TEST_RD_NAND==3
	#if ((GK_TEST_RD_NAND==4))
		GK_PRINT("GK: proc_completer is generated (COMT3 is defined)\n");
		JUMP_ON_ERROR (pthread_create (&proc_completer_tid_nand, NULL, df_inproc_completer, (void *)n), \
				ret, "proc_completer: %s\n", strerror(errno));
	#endif	// GK_TEST_RD_NAND==4
	
#endif
	}
#endif

	GK_PRINT("GK: nvme_init() will be called...\n");
	JUMP_ON_ERROR (nvme_init (n), ret, " ");
	printf("\n\tINIT SEQUENCE COMPLETED.. PROCEEDING FOR NVME OPERATIONS\t\n");

#if (CUR_SETUP == TARGET_SETUP || CUR_SETUP == STANDALONE_SETUP)
	if (n->ns_check == DDR_ALONE || n->ns_check == DDR_NAND) {
#ifdef COMT3
		JUMP_ON_ERROR (pthread_create (&io_completer_tid_ddr, NULL, io_completer, (void *)n), \
				ret, "io_completer: %s\n", strerror(errno));
#endif
	}
#endif
	JUMP_ON_ERROR (pthread_create (&io_processor_tid, NULL, io_processor, (void *)n), \
			ret, "io_processor: %s\n", strerror(errno));
            
    // gunjae: initial environment check is done? init_cli performs pthread_create()
	init_cli();

	/*in_irqs = &n->in_irqs;*/

    // gunjae: FPGA has fifo counter?
	fifo_count_ptr = n->fpga.fifo_count;

	uint32_t arr[4];
	int j = 0;
#ifdef ADMIN_MSI
    // gunjae: what is ADMIN_MSI?
    GK_PRINT("GK: ADMIN_MSI is defined\n");
    //
	int uiofd, uiofd1;
	int rc;
	ssize_t nb;
	uint32_t int_no;
	uint32_t intr_enable = 1;
	static struct timeval def_time = {0, 10000};
	static struct timeval timeout = {0, 10000};

	uiofd = open("/dev/uio2", O_RDWR);
	PERROR_ON_ERROR ((uiofd < 0), ret, "/dev/uio2" ": %s\n", strerror (errno));

	uiofd1 = open("/dev/uio3", O_RDWR);
	PERROR_ON_ERROR ((uiofd1 < 0), ret, "/dev/uio3" ": %s\n", strerror (errno));

	int max = (uiofd > uiofd1) ? uiofd : uiofd1;
	fd_set readfds;

	do {
		nb = write(uiofd, &intr_enable, sizeof(intr_enable));
		if (nb < sizeof(intr_enable)) {
			perror("config write:");
			continue;
		}
	} while (nb < 0);
	do {
		nb = write(uiofd1, &intr_enable, sizeof(intr_enable));
		if (nb < sizeof(intr_enable)) {
			perror("config write:");
			continue;
		}
	} while (nb < 0);

	FD_ZERO(&readfds);
#endif

	while (!nvmeKilled) {
#if 0
		nvme_await_update_irqs (&n->in_irqs);
		if (in_irqs->irq_bits & NIRQ_FIFO) {
#endif
#ifdef ADMIN_MSI
			FD_SET(uiofd, &readfds);
			FD_SET(uiofd1, &readfds);

			rc = select(max+1, &readfds, NULL, NULL, &timeout);
			if(rc == -1) {
				timeout = def_time;
				continue;
			} else if (rc == 0) {
				fifo_count = *fifo_count_ptr;
				while (fifo_count) {
					for (j=0; j<4; j++){
						arr[j] = *n->fpga.fifo_reg;
					}
					entry.new_val = *(uint64_t *)arr;
					entry.offset =  (arr[2] & 0xffffffff);
					if((entry.offset == 0) || (entry.offset > 0x200)) {
						syslog(LOG_INFO,"Wrong offset: 0x%lx clearing FIFO\n", (long unsigned)entry.offset);    
						reset_fifo(&n->fpga);
						reset_iosqdb_bits(&n->fpga);
						fifo_count = *fifo_count_ptr;
						continue;
					}
					/*memcpy (&entry, n->fpga.fifo_reg, sizeof (fifo_data));*/
					fifo_count -= 4;
                    // gunjae: what is nvme_process_reg and nvme_process_db?
					if ((entry.offset << 3) < 0x1000) {
						if(arr[3] == 0xc0000000) {
							nvme_process_reg (n, entry.offset << 3, entry.new_val);
						} else if (arr[3] == 0x80000000) {
							nvme_process_reg (n, (entry.offset << 3) + 0x4 , (entry.new_val >> 32));
						} else {
							nvme_process_reg (n, entry.offset << 3, (entry.new_val & 0xffffffff));
						}
					} else {
						nvme_process_db (n, entry.offset << 3, entry.new_val);
					}
				}

				nvme_q_scheduler(n);
				timeout = def_time;
			} else {
				if(FD_ISSET(uiofd, &readfds)) {
					nb = read(uiofd, &int_no, sizeof(int_no));
					if(nb != sizeof(int_no)){
						perror("uio Read:");
						continue;
					}
#endif
					fifo_count = *fifo_count_ptr;
					while (fifo_count) {    // gunjae: fifo_count > 0 (what is this fifo?)
						for (j=0; j<4; j++){
							arr[j] = *n->fpga.fifo_reg;
						}
						entry.new_val = *(uint64_t *)arr;
						entry.offset =  (arr[2] & 0xffffffff);
						if((entry.offset == 0) || (entry.offset > 0x200)) {
							syslog(LOG_INFO,"Wrong offset: 0x%lx clearing FIFO\n", (long unsigned)entry.offset);	
							reset_fifo(&n->fpga);
							reset_iosqdb_bits(&n->fpga);
							fifo_count = *fifo_count_ptr;
							continue;
						}
						/*memcpy (&entry, n->fpga.fifo_reg, sizeof (fifo_data));*/
						fifo_count -= 4;
                        // gunjae: what is nvme_process_reg and nvme_process_db?
						if ((entry.offset << 3) < 0x1000) {
							if(arr[3] == 0xc0000000) {
								nvme_process_reg (n, entry.offset << 3, entry.new_val);
							} else if (arr[3] == 0x80000000) {
								nvme_process_reg (n, (entry.offset << 3) + 0x4 , (entry.new_val >> 32));
							} else {	
								nvme_process_reg (n, entry.offset << 3, (entry.new_val & 0xffffffff));
							}
						} else {
							nvme_process_db (n, entry.offset << 3, entry.new_val);
						}
					}
#ifdef ADMIN_MSI
					nb = write(uiofd, &intr_enable, sizeof(intr_enable));
					if (nb < sizeof(intr_enable)) {
						perror("config write:");
						continue;
					}
				} 
				if(FD_ISSET(uiofd1, &readfds)) {
					nb = read(uiofd1, &int_no, sizeof(int_no));
					if(nb != sizeof(int_no)){
						perror("uio Read:");
						continue;
					}
#endif
					nvme_q_scheduler(n);
#ifdef ADMIN_MSI
					nb = write(uiofd1, &intr_enable, sizeof(intr_enable));
					if (nb < sizeof(intr_enable)) {
						perror("config write:");
						continue;
					}
				}
			}
#else
			usleep(10);
#endif
#if 0
		}
		if (in_irqs->irq_bits & NIRQ_IODB) {
		}
		usleep (10);
#endif
	}   // while (!nvmeKilled)

CLEAN_EXIT:
    // gunjae: what is ADMIN_MSI?
    printf("GK: CLEAN_EXIT process starting...\n");
	pthread_cancel (io_processor_tid);
	pthread_join (io_processor_tid, NULL);
#if 0
#if USING_MSGQ_FROM_SYSV
	msgctl(n->qsched.mq_txid, IPC_RMID, 0);
#else
	mq_close (n->qsched.mq_txid);
	mq_close (n->qsched.mq_rxid);
	mq_unlink (NVME_MQ_NAME);
#endif
#endif
	syslog(LOG_DEBUG,"Cleaning up...\n");
	if((n->ns_check == DDR_NAND) || (n->ns_check == NAND_ALONE)) {
		FREE_VALID (n->drv_params);
		bdbm_drv_deinit();
	}

#if (CUR_SETUP == TARGET_SETUP || CUR_SETUP == STANDALONE_SETUP)
#ifdef COMT3

	if (n->ns_check == DDR_ALONE) {
		pthread_cancel (io_completer_tid_ddr);
		pthread_join (io_completer_tid_ddr, NULL);
	}
	if(n->ns_check == NAND_ALONE) {
		pthread_cancel (io_completer_tid_nand);
		pthread_join (io_completer_tid_nand, NULL);
	#if ((GK_TEST_RD_NAND==3) || (GK_TEST_RD_NAND==4))
		pthread_cancel (io_completer_tid_nand_rd);
		pthread_join (io_completer_tid_nand_rd, NULL);
	#endif	// GK_TEST_RD_NAND==3
	#if ((GK_TEST_RD_NAND==4))
		pthread_cancel (proc_completer_tid_nand);
		pthread_join (proc_completer_tid_nand, NULL);
	#endif	// GK_TEST_RD_NAND==4
	}
	if(n->ns_check == DDR_NAND) {
		pthread_cancel (io_completer_tid_ddr);
		pthread_join (io_completer_tid_ddr, NULL);
		pthread_cancel (io_completer_tid_nand);
		pthread_join (io_completer_tid_nand, NULL);
	#if ((GK_TEST_RD_NAND==3) || (GK_TEST_RD_NAND==4))
		pthread_cancel (io_completer_tid_nand_rd);
		pthread_join (io_completer_tid_nand_rd, NULL);
	#endif	// GK_TEST_RD_NAND==3
	#if ((GK_TEST_RD_NAND==4))
		pthread_cancel (proc_completer_tid_nand);
		pthread_join (proc_completer_tid_nand, NULL);
	#endif	// GK_TEST_RD_NAND==4
	}
#endif
#endif

	mq_close (n->mq_txid);
	mq_close (n->mq_rxid);
	mq_unlink (NVME_MQ_NAME);

	syslog(LOG_DEBUG,"Cleaning up...\n");
	nvme_bio_deinit (n);
	nvme_clear_ctrl (n);
	nvme_deinit_pci (n);
	FREE_VALID (n->sq);
	FREE_VALID (n->cq);
	FREE_VALID (n->aer_reqs);
	FREE_VALID (n->elpes);
	FREE_VALID (n->aer_queue);
	FREE_VALID (n->features.int_vector_config);
	FREE_VALID (n->namespaces);
	syslog(LOG_INFO,"***************NVMe EXIT STATUS: %d****************\r\n", ret);
	mutex_destroy(&n->req_mutex);
#ifdef SPINLOCK
	spin_destroy(&n->qs_req_spin);
	spin_destroy(&n->aer_req_spin);
#else
	mutex_destroy(&n->qsch_mutex);
#endif

#ifdef ADMIN_MSI
	close(uiofd);
	close(uiofd1);
#endif
	return ret;
}

