/**
 * yama_starter.c
 * 
 * Runs HERD server or client on YAMA
 */ 

#include "bctl.h"
#include "param.h"

#include "common.h"

#include <float.h>
#include <signal.h>

struct ibv_device * ib_dev;
volatile int hrd_started = 0;

extern int pacer_enabled;
extern int pacer_pace;
extern struct pacer_control_block * pacer_cb;

static int usage(const char * argv0);

// Create protection domain. Create queue pairs and modify them to INIT.
static struct ctrl_blk *init_ctx(struct ctrl_blk *ctx, 
	struct ibv_device *ib_dev)
{
	ctx->context = ibv_open_device(ib_dev);
	CPE(!ctx->context, "Couldn't get context", 0);

	ctx->pd = ibv_alloc_pd(ctx->context);
	CPE(!ctx->pd, "Couldn't allocate PD", 0);

	create_qp(ctx);
	modify_qp_to_init(ctx);

	return ctx;
}

// process_pipeline is called before we want to insert a new request or when we 
// want to add a dummy request.
void process_pipeline(struct ctrl_blk *cb)
{
	int pipeline_index = cb->tot_pipelined & 1;
	int k = 0;
	
	// Move backwards through the pipeline
	for(k = 1; k <= 2; k++) {
		int ind = (pipeline_index - k) & 1;
		int req_type = cb->pipeline[ind].req_type;

		// printf("pipeline %d has request type %d", k, req_type);
		if(req_type == DUMMY_TYPE || req_type == EMPTY_TYPE) {
			if(k == 2) {		// Output the dummy pipeline item
				cb->pipeline_out = &cb->pipeline[ind];
			}
			continue;
		}
		long long *key = cb->pipeline[ind].kv->key;
		int ras = cb->pipeline[ind].req_area_slot;
		
		// Set the polled value in the request region temporarily. Must zero it 
		// out later.
		key[KEY_SIZE - 1] = cb->pipeline[ind].poll_val;
				
		int key_bkt_num = KEY_TO_BUCKET(key[0]);
		int key_tag = KEY_TO_TAG(key[0]);

		// Access the bucket as an array of 8 longs
		LL *key_bkt = (LL *) &cb->ht_index[key_bkt_num];

		if(k == 1) {
			if(req_type == PUT_TYPE) {	/*PUT*/
				// If there is ANY chance that the written KV will overflow the log, reset
				if((cb->log_head & LOG_SIZE_) >= LOG_SIZE - 4096) {
					cb->log_head = cb->log_head + (LOG_SIZE - (cb->log_head & LOG_SIZE_));
					fprintf(stderr, "Server %d resetting log head to 0\n",
						cb->id);
				}

				int slot;
				LL max_diff = MIN_LL, best_slot = 0;
				for(slot = SLOTS_PER_BKT - 1; slot >= 1; slot--) {
					if(key_bkt[slot] == INVALID_SLOT) {
						best_slot = slot;
						break;
					}
					LL log_offset = SLOT_TO_OFFSET(key_bkt[slot]);
					if(cb->log_head - log_offset > max_diff) {
						max_diff = cb->log_head - log_offset;
						best_slot = slot;
					}
					// While insertion, we remove collisions.
					int slot_tag = SLOT_TO_TAG(key_bkt[slot]);
					if(slot_tag == key_tag) {
						key_bkt[slot] = INVALID_SLOT;
					}
				}

				// Prepare the slot. Assuming that log_head is less than 2^48,
				// the offset stored in the slot is in [0, 2^48). 
				key_bkt[best_slot] = key_tag;
				key_bkt[best_slot] |= (cb->log_head << 16);
			
				// Append to log
				memcpy(&cb->ht_log[cb->log_head & LOG_SIZE_], cb->pipeline[ind].kv, S_KV);
				cb->log_head += S_KV;
			} else {	/*GET*/
				int slot, key_in_index = 0;
				for(slot = SLOTS_PER_BKT - 1; slot >= 0; slot--) {
					int slot_tag = SLOT_TO_TAG(key_bkt[slot]);
					if(slot_tag == key_tag) {
						LL log_offset = SLOT_TO_OFFSET(key_bkt[slot]);
						if(cb->log_head - log_offset > LOG_SIZE) {
							break;
						}
						__builtin_prefetch(&cb->ht_log[log_offset & LOG_SIZE_], 0, 3);

						key_in_index = 1;
						cb->pipeline[ind].get_slot = slot;
						break;
					}
				}
				if(key_in_index == 0) {
					cb->server_resp_area[ras].len = GET_FAIL_LEN_1;
				}
			}
		}	// END 1st pipeline stage
		if(k == 2) {
			if(req_type == GET_TYPE) {		// GET
				int slot = cb->pipeline[ind].get_slot;
				int key_still_in_index = 0, key_in_log = 0;

				int slot_tag = SLOT_TO_TAG(key_bkt[slot]);
				if(slot_tag == key_tag) {
					key_still_in_index = 1;
					LL log_offset = SLOT_TO_OFFSET(key_bkt[slot]);
					LL log_addr = log_offset & LOG_SIZE_;
					
					LL *log_key = (LL *) &cb->ht_log[log_addr + KV_KEY_OFFSET];
					int valid = (log_key[0] == key[0]);
					#if(KEY_SIZE == 2)
						valid &= (log_key[1] == key[1]);
					#endif
					if(valid) {
						key_in_log = 1;
						// Copy the log straight to the response and record
						// that this has been done.
						SET_PL_IT_MEMCPY_DONE(cb->pipeline[ind]);
						memcpy((char *) &cb->server_resp_area[ras], &cb->ht_log[log_addr], S_KV);
					}
				}
				if(key_still_in_index == 0 || key_in_log == 0) {
					cb->server_resp_area[ras].len = GET_FAIL_LEN_2;
				}
			}
			cb->pipeline_out = &cb->pipeline[ind];
		}
		key[KEY_SIZE - 1] = 0;	// Zero out polled value again
	}
}

void run_server(struct ctrl_blk *cb)
{
	if(cb->id == 0) {
		sleep(1000000000);
	}

	struct ibv_send_wr *bad_send_wr;

	int i, ret = 0, num_resp = 0;
	int last_resp = -1;
	struct timespec start, end;		// Timers for throughput
	
	int * req_lo = malloc(cb->entity->num_remote_hosts * sizeof(int));		// Base request index for each client
	int * req_num = malloc(cb->entity->num_remote_hosts * sizeof(int));		// Offset above the base index

	int failed_polls = 0;			// # of failed attempts to find a new request
	init_ht(cb);

	for(i = 0; i < 2; i++) {
		cb->pipeline[i].req_type = EMPTY_TYPE;
	}

	for(i = 0; i < cb->entity->num_remote_hosts; i++) {
		req_lo[i] = (cb->id * (WINDOW_SIZE * MAX_NUM_CLIENTS)) + (i * WINDOW_SIZE);
		req_num[i] = 0;
	}

	clock_gettime(CLOCK_REALTIME, &start);
	while(1) {
		for(i = 0; i < cb->entity->num_remote_hosts; i++) {
			// usleep(200000);
			if((num_resp & M_1_) == M_1_ && num_resp > 0 && num_resp != last_resp) {
				clock_gettime(CLOCK_REALTIME, &end);
				double seconds = (end.tv_sec - start.tv_sec) +
					(double) (end.tv_nsec - start.tv_nsec) / 1000000000;
				fprintf(stderr, "Entity %d Server %d, IOPS: %f, used fraction: %f\n", 
					cb->eid, cb->id, M_1 / seconds, (double) cb->log_head / LOG_SIZE);
				clock_gettime(CLOCK_REALTIME, &start);
				last_resp = num_resp;
			}

			// Poll for a new request
			int req_ind = req_lo[i] + (req_num[i] & WINDOW_SIZE_);
			if((char) cb->server_req_area[req_ind].key[KEY_SIZE - 1] == 0) {
				failed_polls ++;
				if(failed_polls < FAIL_LIM) {
					// printf("Server %d failed to poll!!!\n", cb->id);
					continue;
				}
			} 
			// else {
				// printf("Entity %d Server %d polled request!!!\n", cb->eid, cb->id);
			// }

			// Issue prefetches before computation
			if(failed_polls < FAIL_LIM) {
				int key_bkt_num = KEY_TO_BUCKET(cb->server_req_area[req_ind].key[0]); 	
				
				// We only get here if we find a new valid request. Therefore,
				// it's OK to use the len field to determine request type
				if(cb->server_req_area[req_ind].len > 0) {
					__builtin_prefetch(&cb->ht_index[key_bkt_num], 1, 3);
				} else {
					__builtin_prefetch(&cb->ht_index[key_bkt_num], 0, 3);
				}
			}

			// Move stuff forward in the pipeline
			process_pipeline(cb);
			// printf("Request %d for client %d moved through the pipeline\n", req_ind, i);

			// Process the pipeline's output. pipeline_out is a pointer to a
			// pipeline slot. The new request will get pushed into this slot.
			// Process the output *before* pushing the new request in.

			// Is the output legit?
			// printf("Client %d request %d: pipeline_out->req_type = %d\n", i, req_lo[i] + req_num[i], pipeline_out->req_type);

			if(cb->pipeline_out->req_type != DUMMY_TYPE && cb->pipeline_out->req_type != EMPTY_TYPE) {	
				int cn = cb->pipeline_out->cn & 0xff;
				int ras = cb->pipeline_out->req_area_slot;

				cb->wr.wr.ud.ah = cb->ah[cn];
				cb->wr.wr.ud.remote_qpn = cb->remote_dgram_qp_attrs[cn].qpn;
				cb->wr.wr.ud.remote_qkey = 0x11111111;

				cb->wr.send_flags = (num_resp & WS_SERVER_) == 0 ?
					MY_SEND_INLINE | IBV_SEND_SIGNALED : MY_SEND_INLINE;
				if((num_resp & WS_SERVER_) == WS_SERVER_) {
					poll_dgram_cq(1, cb, 0);
				}

				if(cb->pipeline_out->req_type == PUT_TYPE) {		// PUT response
					cb->sgl.addr = (uint64_t) (unsigned long) &cb->server_resp_area[ras];
					cb->wr.sg_list->length = 1;
				} else if(cb->pipeline_out->req_type == GET_TYPE) {
					cb->sgl.addr = (uint64_t) (unsigned long) &cb->server_resp_area[ras];
					cb->wr.sg_list->length = KV_KEY_OFFSET;
				} else {
					fprintf(stderr, "No type?!\n");
					exit(0);
				}

				ret = ibv_post_send(cb->dgram_qp[0], &cb->wr, &bad_send_wr);
				// printf("Entity %d Server %d posted response %d !!!\n", cb->eid, cb->id, num_resp);
				CPE(ret, "ibv_post_send error", ret);
				
				num_resp++;

				// if (num_resp % K_128 == 0) {
				// 	printf("Completed %d resps\n", num_resp);
				// }
			}

			// Add a new request (legit/dummy) into the pipeline
			// The index in the pipeline where the new item will be pushed
			int pipeline_index = cb->tot_pipelined & 1;

			if(failed_polls < FAIL_LIM) {
				if(cb->server_req_area[req_ind].len == 0) {
					cb->pipeline[pipeline_index].req_type = GET_TYPE;
					// printf("Get request pipelined\n");
				} else {
					cb->pipeline[pipeline_index].req_type = PUT_TYPE;
					// printf("Put request pipelined\n");
				}

				cb->pipeline[pipeline_index].kv = (struct KV *) &cb->server_req_area[req_ind];
				cb->pipeline[pipeline_index].cn = i;
				cb->pipeline[pipeline_index].req_area_slot = req_ind;

				// Store the polled value in the pipeline item and make it zero
				// in the request region
				cb->pipeline[pipeline_index].poll_val = 
					cb->server_req_area[req_ind].key[KEY_SIZE - 1];

				// Zero out the polled key so that this request is not detected again.
				// Make the len field zero. If a new request is detected an len is
				// still 0, it means that the new request is a GET.
				cb->server_req_area[req_ind].key[KEY_SIZE - 1] = 0;
				cb->server_req_area[req_ind].len = 0;

				req_num[i] ++;
			} else {
				// printf("Got dummy type!!!\n");
				cb->pipeline[pipeline_index].req_type = DUMMY_TYPE;
				failed_polls = 0;
			}

			cb->tot_pipelined ++;
		}
	}
	return;
}

// Post a recv() for a send() from server sn
void post_recv(struct ctrl_blk *cb, int iter_, int sn)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) &cb->client_resp_area[(sn * WINDOW_SIZE) + iter_],
		.length = S_UD_KV,
		.lkey	= cb->client_resp_area_mr->lkey
	};

	// This does not use the wr in cb - avoids interference
	// with the WRITE to server
	struct ibv_recv_wr recv_wr = {
		.sg_list    = &list,
		.num_sge    = 1,
	};
	struct ibv_recv_wr *bad_wr;
	int ret = ibv_post_recv(cb->dgram_qp[sn], &recv_wr, &bad_wr);
	if(ret) {
		fprintf(stderr, "Error %d posting recv.\n", ret);
		exit(0);
	}
}

void run_client(struct ctrl_blk *cb)
{
	struct ibv_send_wr *bad_send_wr;
	struct ibv_wc wc[WS_SERVER];
	
	struct timespec start, end;		// Throughput timers
	struct timespec op_start[WINDOW_SIZE], op_end[WINDOW_SIZE];	// Latency timers
	uint64_t fastrand_seed = 0xdeadbeef;
	LL total_nsec = 0;

	fprintf(stderr, "Starting client %d\n", cb->id);
	clock_gettime(CLOCK_REALTIME, &start);

	int ret, iter = 0, sn = -1;
	int num_resp = 0, num_req = 0, wait_cycles = 0, num_fails = 0;

	// Number of pending requests and responses received from each server
	int * num_req_arr = calloc(cb->entity->num_remote_hosts, sizeof(int));	
	// memset(num_req_arr, 0, NUM_SERVERS * sizeof(int));

	int * num_resp_arr = calloc(cb->entity->num_remote_hosts, sizeof(int));	
	// memset(num_resp_arr, 0, NUM_SERVERS * sizeof(int));

	// The server contacted and the key used in a window slot
	int sn_arr[WINDOW_SIZE];		// Required for polling for recv comps
	memset(sn_arr, 0, WINDOW_SIZE * sizeof(int));
	
	LL pndng_keys[WINDOW_SIZE];		// The keys for which a response is pending
	memset(pndng_keys, 0, WINDOW_SIZE * sizeof(LL));

	// Generate the keys to be requested
	int key_i = 0;
	srand48(cb->id);
	LL *key_corpus = gen_key_corpus(cb->id);

	// Pre-post some RECVs in slot order for the servers
	int serv_i;
	for(serv_i = 1; serv_i < cb->entity->num_remote_hosts; serv_i ++) {
		int recv_i;
		for(recv_i = 0; recv_i < CL_BTCH_SZ; recv_i ++) {
			post_recv(cb, recv_i & WINDOW_SIZE_, serv_i);
		}
	}

	for(iter = 0; iter < NUM_ITER; iter++) {
		// usleep(200000);
		int iter_ = iter & WINDOW_SIZE_;
		volatile struct KV *req_kv = &cb->client_req_area[iter_];

		// Performance measurement
		if((iter & M_1_) == M_1_ && iter != 0) {
			fprintf(stderr, "\nEntity %d Client %d completed %d ops\n", cb->eid, cb->id, iter);
			clock_gettime(CLOCK_REALTIME, &end);
			double seconds = (end.tv_sec - start.tv_sec) +
				(double) (end.tv_nsec - start.tv_nsec) / 1000000000;

			fprintf(stderr, "IOPS = %f\n", M_1 / seconds);
			
			double sgl_read_time = (double) total_nsec / M_1;
			fprintf(stderr, "Average op time = %f us\n", sgl_read_time / 1000);
			total_nsec = 0;
			fprintf(stderr, "Avg wait = %f, avg fail = %f\n", 
				(double) wait_cycles / M_1,
				(double) num_fails / M_1_);
			wait_cycles = 0;
			num_fails = 0;

			clock_gettime(CLOCK_REALTIME, &start);
		}

		// First, we PUT all our keys.
		if(rand() % 100 <= PUT_PERCENT || iter < NUM_KEYS) {
			req_kv->key[0] = key_corpus[key_i];
			#if(KEY_SIZE == 2)
				req_kv->key[1] = key_corpus[key_i];
			#endif
			req_kv->len = VALUE_SIZE;
			memset((char *) req_kv->value, (char) key_corpus[key_i], VALUE_SIZE);
			pndng_keys[iter_] = 0;
			key_i = (key_i + 1) & NUM_KEYS_;
		} else {
			key_i = rand() & NUM_KEYS_;
			req_kv->key[0] = key_corpus[key_i];
			#if(KEY_SIZE == 2)
				req_kv->key[1] = key_corpus[key_i];
			#endif
			req_kv->len = 0;
			memset((char *) req_kv->value, 0, VALUE_SIZE);
			pndng_keys[iter_] = req_kv->key[0];
		}
		
		// sn = KEY_TO_SERVER(req_kv->key[0]);
		sn = (int) ((req_kv->key[0] >> 40) % (cb->entity->num_remote_hosts - 1) + 1);
		sn_arr[iter_] = sn;

		int req_offset = (sn * WINDOW_SIZE * MAX_NUM_CLIENTS) + 
			(WINDOW_SIZE * (cb->id + cb->base_id)) + (num_req_arr[sn] & WINDOW_SIZE_);

		clock_gettime(CLOCK_REALTIME, &op_start[iter_]);
		
		cb->wr.send_flags = (num_req & S_DEPTH_) == 0 ?
			MY_SEND_INLINE | IBV_SEND_SIGNALED : MY_SEND_INLINE;
		if((num_req & S_DEPTH_) == S_DEPTH_) {
			poll_conn_cq(1, cb, 0);
		}

		// Real work
		if(req_kv->len == 0) {		// GET
			cb->sgl.addr = (uint64_t) (unsigned long) &req_kv->key;
			cb->wr.sg_list->length = S_KV - KV_KEY_OFFSET;
			cb->wr.wr.rdma.remote_addr = cb->server_req_area_stag[0].buf + 
				(req_offset * S_KV) + KV_KEY_OFFSET;
		} else {
			cb->sgl.addr = (uint64_t) (unsigned long) req_kv;
			cb->wr.sg_list->length = S_KV;
			cb->wr.wr.rdma.remote_addr = cb->server_req_area_stag[0].buf + 
				(req_offset * S_KV);
		}

		cb->wr.wr.rdma.rkey = cb->server_req_area_stag[0].rkey;

		// Although each client has NUM_SERVERS conn_qps, they only issue RDMA
		// WRITEs to the 0th server
		ret = ibv_post_send(cb->conn_qp[0], &cb->wr, &bad_send_wr);
		// printf("Entity %d Client %d posted request %d\n", cb->eid, cb->id, num_req);
		CPE(ret, "ibv_post_send error", ret);

		num_req_arr[sn]++;
		num_req ++;

		if(num_req - num_resp == WINDOW_SIZE) {
			int rws = num_resp & WINDOW_SIZE_;		// Response window slot
			int rsn = sn_arr[rws];					// Response server number
			int ras = (rsn * WINDOW_SIZE) + (num_resp_arr[rsn] & WINDOW_SIZE_);

			// Poll for the recv
			int recv_comps = 0;
			while(recv_comps == 0) {
				wait_cycles ++;
				if(wait_cycles % M_128 == 0) {
					fprintf(stderr, "Wait for iter %d at entity %d client %d GET = %lld\n", 
						num_resp + 1, cb->eid, cb->id, pndng_keys[rws]);
					// not receiving response for too long 
					// poll_conn_cq(1, cb, 0);
				}
				recv_comps = ibv_poll_cq(cb->dgram_recv_cq[rsn], 1, wc);
			}
			if(wc[0].status != 0) {
				fprintf(stderr, "Bad recv wc status %d\n", wc[0].status);
				exit(0);
			}
		
			// If it was a GET, and it succeeded, check it!
			if(pndng_keys[rws] != 0) {
				if(cb->client_resp_area[ras].kv.len < GET_FAIL_LEN_1) {
					if(!valcheck(cb->client_resp_area[ras].kv.value, 
						pndng_keys[rws])) {
						fprintf(stderr, "Entity %d Client %d get() failed in iter %d. ", 
							cb->eid, cb->id, num_resp);
						print_ud_kv(cb->client_resp_area[ras]);
						exit(0);
					}
				}
			}

			if(cb->client_resp_area[ras].kv.len >= GET_FAIL_LEN_1) {
				num_fails ++;
			}
			
			// Batched posting of RECVs
			num_resp_arr[rsn] ++;

			// Recvs depleted: post some more.
			if((num_resp_arr[rsn] & CL_SEMI_BTCH_SZ_) == 0) {
				int recv_i;
				for(recv_i = 0; recv_i < CL_SEMI_BTCH_SZ; recv_i ++) {
					post_recv(cb, recv_i & WINDOW_SIZE_, rsn);
				}
			}

			memset((char *) &cb->client_resp_area[ras], 0, sizeof(struct UD_KV));

			clock_gettime(CLOCK_REALTIME, &op_end[rws]);
			LL new_nsec = (op_end[rws].tv_sec - op_start[rws].tv_sec)* 1000000000 
				+ (op_end[rws].tv_nsec - op_start[rws].tv_nsec);
			total_nsec += new_nsec;

			if(CLIENT_PRINT_LAT == 1) {	// Print latency so that we can compute percentiles
				if((fastrand(&fastrand_seed) & 0xff) == 0) {
					printf("%lld\n", new_nsec);
				}
			}
			num_resp ++;
		}
	}
	return;
}

struct ibv_device * get_ib_dev(char * name) {
	// Get an InfiniBand/RoCE device
	int num_dev;
	struct ibv_device ** dev_list = ibv_get_device_list(&num_dev);
	CPE(!dev_list, "Failed to get IB devices list", 0);

	int dev_id = 0;
	if (name != NULL) {	
		for (int i=0; i<num_dev; i++) {
			if (!strcmp(name, dev_list[i]->name)) {
				dev_id = i;
				printf("Device %i: name %s dev_name %s toggled\n", i, dev_list[i]->name, dev_list[i]->dev_name);
				goto dev_matched;
			}
		} 
		fprintf(stderr, "Supplied IB device %s not found\n", name);
	} else {
		printf("No device specified: Device 0: name %s dev_name %s toggled\n", dev_list[0]->name, dev_list[0]->dev_name);
	}
dev_matched:
	ib_dev = dev_list[dev_id];

	CPE(!ib_dev, "IB device selected but its struct cannot be retrieved", 0);
}

void hrd_init_ib(struct ctrl_blk * ctx) {	// Create queue pairs and modify them to INIT
	int i;

	pacer_cb->bypass = 0;
	pacer_cb->chunk_sz = 0;
	pacer_cb->weight = 1;
	pacer_cb->tx_depth = Q_DEPTH;
	pacer_cb->remote_host_id = 0;

	init_ctx(ctx, ib_dev);
	CPE(!ctx, "Init ctx failed", 0);

	// Create RDMA (request and response) regions 
	setup_buffers(ctx);

	union ibv_gid my_gid= get_gid(ctx->context);

	// Collect local queue pair attributes
	for(i = 0; i < ctx->num_conn_qps; i++) {
		ctx->local_conn_qp_attrs[i].gid_global_interface_id = 
			my_gid.global.interface_id;
		ctx->local_conn_qp_attrs[i].gid_global_subnet_prefix = 
			my_gid.global.subnet_prefix;

		ctx->local_conn_qp_attrs[i].lid = get_local_lid(ctx->context);
		ctx->local_conn_qp_attrs[i].qpn = ctx->conn_qp[i]->qp_num;
		ctx->local_conn_qp_attrs[i].psn = lrand48() & 0xffffff;
		fprintf(stderr, "Entity %d, Process %d: Local address of conn QP %d: ", ctx->eid, ctx->id, i);
		print_qp_attr(ctx->local_conn_qp_attrs[i]);
	}

	for(i = 0; i < ctx->num_local_dgram_qps; i++) {
		ctx->local_dgram_qp_attrs[i].gid_global_interface_id = 
			my_gid.global.interface_id;
		ctx->local_dgram_qp_attrs[i].gid_global_subnet_prefix = 
			my_gid.global.subnet_prefix;

		ctx->local_dgram_qp_attrs[i].lid = get_local_lid(ctx->context);
		ctx->local_dgram_qp_attrs[i].qpn = ctx->dgram_qp[i]->qp_num;
		ctx->local_dgram_qp_attrs[i].psn = lrand48() & 0xffffff;
		fprintf(stderr, "Entity %d, Process %d: Local address of dgram QP: %d", ctx->eid, ctx->id, i);
		print_qp_attr(ctx->local_dgram_qp_attrs[i]);
	}
}

void hrd_setup_ib(struct ctrl_blk * ctx) {
	int i;

	// Exchange queue pair attributes
	if(ctx->is_client) {
		client_exch_dest(ctx);
	} else {
		server_exch_dest(ctx);
	}

	// The server creates address handles for every clients' UD QP
	if(!ctx->is_client) {
		for(i = 0; i < ctx->num_remote_dgram_qps; i++) {
			fprintf(stderr, "Entity %d, Server %d: create_ah for client %d\n", ctx->eid, ctx->id, i);
			print_qp_attr(ctx->remote_dgram_qp_attrs[i]);
			struct ibv_ah_attr ah_attr = {
				.is_global		= 1, // (is_roce() == 1) ? 1 : 0,
				.dlid			= 0, // (is_roce() == 1) ? 0 : ctx->remote_dgram_qp_attrs[i].lid,
				.sl				= 0,
				.src_path_bits	= 0,
				.port_num		= IB_PHYS_PORT
			};

			// if(is_roce()) {
			ah_attr.grh.dgid.global.interface_id = 
				ctx->remote_dgram_qp_attrs[i].gid_global_interface_id;
			ah_attr.grh.dgid.global.subnet_prefix = 
				ctx->remote_dgram_qp_attrs[i].gid_global_subnet_prefix;
		
			ah_attr.grh.sgid_index = 0;
			ah_attr.grh.hop_limit = 1;
			// }

			ctx->ah[i] = ibv_create_ah(ctx->pd, &ah_attr);
			CPE(!ctx->ah[i], "Failed to create ah", i);
		}
	}

	modify_dgram_qp_to_rts(ctx);

	// Move the client's connected QPs through RTR and RTS stages
	if (ctx->is_client) {
		for(i = 0; i < ctx->num_conn_qps; i++) {
			if(connect_ctx(ctx, ctx->local_conn_qp_attrs[i].psn, 
				ctx->remote_conn_qp_attrs[i], i)) {
				exit(1);
			}
		}
	}
}

void * proc_work(void * arg) {
	struct ctrl_blk * ctx = (struct ctrl_blk *) arg;
	hrd_setup_ib(ctx);
	
  cpu_set_t mask;
	if (sched_getaffinity(0, sizeof(mask), &mask) == -1) {
		fprintf(stderr, "FATAL\tflow_work\tsched_getaffinity: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}
	for (int i=0; i<EXCLUDED_CPU_NUM; i++)
		CPU_CLR(i, &mask);
	if (sched_setaffinity(0, sizeof(mask), &mask) == -1) {
		fprintf(stderr, "FATAL\tflow_work\tsched_setaffinity: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	if(ctx->is_client) {
		while (!__atomic_load_n(&hrd_started, __ATOMIC_SEQ_CST));
		printf("Entity %d Client %d starts working\n", ctx->eid, ctx->id);
		run_client(ctx);
		printf("Entity %d Client %d: work done, exitting\n", ctx->eid, ctx->id);
	} else {
		ctx->log_head = 0;
		ctx->tot_pipelined = 0;
		printf("Entity %d Server %d starts working\n", ctx->eid, ctx->id);
		run_server(ctx);
		printf("Entity %d Server %d: work done, exitting\n", ctx->eid, ctx->id);
	}

	return NULL;
}


int main(int argc, char ** argv)
{
  char logging_msg_buf[128];

  int is_dynamic_interval, is_splitting_requests;
  struct comm_rcm * rcm;

  int i, j, k, is_client, base_id, num_entities, num_paced_hosts;
	struct hrd_entity * entities;
	struct ctrl_blk ** ctxs;
	struct remote_host * paced_hosts;

  float min_weight = FLT_MAX;
  
  TEST_NZ(ibv_fork_init());

  pacer_init();
  
  TEST_NZ(set_sched_batch(0));

  if (argc < 6)
    usage(argv[0]);

  is_client = atoi(argv[1]);
	base_id = atoi(argv[2]);

  is_dynamic_interval = atoi(argv[3]);
  is_splitting_requests = atoi(argv[4]);
  TEST_Z(rcm = comm_create_rcm());

  if (!is_dynamic_interval)
    pacer_cb = malloc(sizeof(*pacer_cb));

  __atomic_store_n(&pacer_enabled, is_dynamic_interval, __ATOMIC_SEQ_CST);
  __atomic_store_n(&pacer_pace, 0, __ATOMIC_SEQ_CST);

  TEST_Z(rcm = comm_create_rcm());
  logging_info("main", "rdma client manager (rcm) created");

  num_entities = atoi(argv[5]);
  if (!num_entities)
    return 0;
	entities = calloc(num_entities, sizeof(struct hrd_entity));
  // remote_hosts = calloc(num_remote_host, sizeof(struct remote_host));

  // pthread_mutexattr_init(&mattr);
  // pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);

  k=6; //token counter
  for (i=0; i<num_entities; i++) {
		entities[i].id = i;

    entities[i].num_local_procs = atoi(argv[k++]);
		entities[i].pids = (hrd_pid_t *) malloc(entities[i].num_local_procs * sizeof(hrd_pid_t));

		if (!is_client) {
			entities[i].local_ports = (uint16_t *) malloc(entities[i].num_local_procs * sizeof(uint16_t));
			for (j=0; j < entities[i].num_local_procs; j++) {
				entities[i].local_ports[j] = (uint16_t)atoi(argv[k++]);
			}
		}

		entities[i].num_remote_hosts = atoi(argv[k++]);
		entities[i].remote_hosts = (struct remote_host *) malloc(entities[i].num_remote_hosts * sizeof(struct remote_host));
		for (j=0; j<entities[i].num_remote_hosts; j++) {
			// entities[i].remote_hosts[j].id = j;
			if (sscanf(argv[k++], "%d,%f,%hu,%s", 
				&entities[i].remote_hosts[j].num_flows,
				&entities[i].remote_hosts[j].weight,
				&entities[i].remote_hosts[j].port,
				entities[i].remote_hosts[j].ip) != 4) {

				logging_fatal("main", "parsing remote host configuration");
				usage(argv[0]);
			}
			if (entities[i].remote_hosts[j].weight < min_weight) {
				min_weight = entities[i].remote_hosts[j].weight;
			}
		}
	}
		
	struct ibv_device * ib_dev = get_ib_dev(getenv("IBDEV_NAME"));

	ctxs = calloc(num_entities, sizeof(struct ctrl_blk *));
  for (i=0; i<num_entities; i++) {
		// for each HERD instance, configure each client/server process
		ctxs[i] = calloc(entities[i].num_local_procs, sizeof(struct ctrl_blk));
    for (j=0; j<entities[i].num_local_procs; j++) {
			ctxs[i][j].eid = i;
			ctxs[i][j].entity = &entities[i];
			if (!is_client)
				ctxs[i][j].sock_port = entities[i].local_ports[j];

			ctxs[i][j].id = j;
			ctxs[i][j].base_id = base_id;

			ctxs[i][j].ah = (struct ibv_ah **) malloc(entities[i].num_remote_hosts * sizeof(struct ibv_ah *));

			ctxs[i][j].num_conn_qps = entities[i].num_remote_hosts;
			ctxs[i][j].num_remote_dgram_qps = entities[i].num_remote_hosts;

			ctxs[i][j].local_conn_qp_attrs = (struct qp_attr *) malloc(entities[i].num_remote_hosts * S_QPA);
			ctxs[i][j].remote_conn_qp_attrs = (struct qp_attr *) malloc(entities[i].num_remote_hosts * S_QPA);

			if (is_client) {
				ctxs[i][j].is_client = 1;
				ctxs[i][j].num_local_dgram_qps = entities[i].num_remote_hosts;

				// The clients don't need an address handle for the servers UD QPs
				ctxs[i][j].local_dgram_qp_attrs = (struct qp_attr *) malloc(entities[i].num_remote_hosts * S_QPA);
			} else {
				ctxs[i][j].num_local_dgram_qps = 1;

				ctxs[i][j].local_dgram_qp_attrs = (struct qp_attr *) malloc(S_QPA);
				ctxs[i][j].remote_dgram_qp_attrs = (struct qp_attr *) malloc(entities[i].num_remote_hosts * S_QPA);
			}

			hrd_init_ib(&ctxs[i][j]);
		}
	}

	for (i = 0; i < num_entities; i++) {
		for (j = 0; j < entities[i].num_local_procs; j++) {
			pthread_create(&entities[i].pids[j], NULL, proc_work, (void *)&ctxs[i][j]);
			if (is_client) {
				usleep(100000);
			} else if (j == 0) {
				usleep(2000000); 
			} else {
				usleep(500000);
			}
		}
	}


  // for (i=0; i<num_remote_host; i++) {
  //   for (j=0; j<remote_hosts[i].entities_n; j++) {

  //     TEST_NZ(entity_build_flows(&remote_hosts[i].entities[j],
  //       rcm, remote_hosts[i].ip, remote_hosts[i].port));

  //     sprintf(logging_msg_buf,
  //       "remote host %s:%hu entity %d created; flows connected",
  //       remote_hosts[i].ip,
  //       remote_hosts[i].port,
  //       j);
  //     logging_info("main", logging_msg_buf);
  //   }
  // }
  
  // create control clow
  if (is_dynamic_interval) {
		num_paced_hosts = atoi(argv[k++]);
		paced_hosts = malloc(num_paced_hosts * sizeof(*paced_hosts));
		for (i=0; i < num_paced_hosts; i++) {
			if (sscanf(argv[k++], "%hu,%s",
								&paced_hosts[i].port,
								paced_hosts[i].ip) != 2) {

				logging_fatal("main", "parsing paced host configuration");
				usage(argv[0]);
			}
		}

    for (i=0; i<num_paced_hosts; i++) {
      pacer_cb->bypass = 1;
      pacer_cb->chunk_sz = 0;
      pacer_cb->weight = min_weight;
      pacer_cb->tx_depth = TX_DEPTH;
      pacer_cb->remote_host_id = i;
      TEST_Z(bctl_ctx.cflow[i] = cflow_create(rcm, NULL, (i+1)*MAX_FLOWS_N_PER_ENTITY*MAX_REMTE_HOSTS_N-1, "read", 512, 0, NULL, 0, 0));
      bctl_ctx.cflow[i]->cflow_id = i;
      bctl_ctx.cflow[i]->cflow_weight = pacer_cb->weight;
      TEST_NZ(flow_connect(bctl_ctx.cflow[i], paced_hosts[i].ip, paced_hosts[i].port));
    }
    logging_info("main", "ref flows created"); 
  }

  __atomic_store_n(&pacer_pace, 1, __ATOMIC_SEQ_CST);

  // FILE* flows_ready_fd = NULL;
  // if (!(flows_ready_fd = fopen("/tmp/flows_ready", "w"))) {
  //   logging_fatal("main", "failed to open /tmp/flows_ready"); 
  // }
  // fprintf(flows_ready_fd, "1\n");
  // fflush(flows_ready_fd);
  // fclose(flows_ready_fd);

  if (!is_dynamic_interval) {
		__atomic_store_n(&hrd_started, 1, __ATOMIC_SEQ_CST);
		
		for (i = 0; i < num_entities; i++) {
			for (j = 0; j < entities[i].num_local_procs; j++) {
				pthread_join(entities[i].pids[j], NULL);
			}
		}
    // while (!signaled);

    // logging_info("main", "signalled, waiting for flows to complete"); 

    // // trigger normal flows
    // for (i=0; i<num_remote_host; i++) {
    //   for (j=0; j<remote_hosts[i].entities_n; j++) {
    //     entity_trigger_flows(&remote_hosts[i].entities[j]);
    //   }
    // }  

    // // check if someone is finished
    // is_complete = 0;
    // while (!is_complete) {
    //   for (i=0; i<num_remote_host; i++) {
    //     for (j=0; j<remote_hosts[i].entities_n; j++) {
    //       pthread_mutex_lock(&remote_hosts[i].entities[j].comp_cnt_lock);
    //       if (remote_hosts[i].entities[j].complete_counter) 
    //         is_complete = 1;
    //       pthread_mutex_unlock(&remote_hosts[i].entities[j].comp_cnt_lock);
    //     }
    //   }
    // }

    goto clean_up;
  }

  // trigger control flow
  for (i=0; i<num_paced_hosts; i++) {
    TEST_NZ(cflow_trigger(bctl_ctx.cflow[i]));
  }

  // trigger normal flows
		__atomic_store_n(&hrd_started, 1, __ATOMIC_SEQ_CST);
		
		for (i = 0; i < num_entities; i++) {
			for (j = 0; j < entities[i].num_local_procs; j++) {
				pthread_join(entities[i].pids[j], NULL);
			}
		}

  // logging_info("main", "waiting for flows to complete"); 

  // while (!signaled);

  // logging_info("main", "some flow quits, cleaning up");

clean_up:
    // for (i=0; i<num_remote_host; i++) {
    //   for (j=0; j<remote_hosts[i].entities_n; j++) {
    //     entity_stop_flows(&remote_hosts[i].entities[j]);
    //   }
    // }

  if (is_dynamic_interval)
    for (i=0; i<num_paced_hosts; i++)
      kill(bctl_ctx.cflow[i]->worker, SIGTERM);

	return 0;
}

int usage(const char * argv0) {
  fprintf(stderr, "usage: %s <is_client> <is_dynamic_interval> <is_splitting_requests> <num_entities> [<num_local_procs>\\\n"\
    "  (server only){<local_port_0> [<local_port_1> ...]} <num_remote_hosts> [<num_flow>,<weight>,<port>,<ip> ...]]\\\n"\
    "  <num_paced_hosts> [ <port>,<ip> ...] \\\n"\
    "     [ <flows_n>,<req_size_b>,<iters_n>,<weight>,<verb> ...] ...]\n", argv0);
  exit(EXIT_FAILURE);
}
