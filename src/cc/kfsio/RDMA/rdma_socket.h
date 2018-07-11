#ifndef RDMA_SOCKET_HPP_
#define RDMA_SOCKET_HPP_

#include <infiniband/verbs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>

struct RDMAAddr {
	uint32_t lid;
	uint32_t qpn;
	uint32_t psn;
};

struct RDMAConfig {
	const char* tcp_host;
	int tcp_port;
	ibv_device* dev;
	int ib_port;
	int num_comp_queue_entries;
};

struct RDMAContext {
	ibv_context* context;
	ibv_cq* send_cq;
	ibv_cq* recv_cq;
	ibv_qp* qp;
	ibv_mr* mr;
	ibv_pd* pd;
	ibv_comp_channel* ch;
	int num_comp_queue_entries;
	ibv_sge sge_list;
	ibv_send_wr send_wr;
	ibv_recv_wr recv_wr;
};


void GetAvaliableDeviceAndPort(ibv_device& dev,int& port);

class RDMASocket {
public:
	RDMASocket() = delete;
	RDMASocket(const char* tcp_host,
			   int tcp_port,
			   int num_comp_queue_entries,
			   int buf_size);
	RDMASocket(int tcp_port,
			   int num_comp_queue_entries,
			   int buf_size);
	RDMASocket(const RDMAConfig& config,
			   int buf_size);
	void Close();
	~RDMASocket();

	void SendMsg(char* msg,size_t size);
	void RecvMsg(char*& msg,size_t& size);
	bool Bind();

protected:
	bool InitContext();
	bool ExchangeAddrThroughTCP();
	bool InitQueuePair();
	bool SetQueuePairRTR();
	bool SetQueuePairRTS();
	bool GenerateLocalAddr();
private:
	RDMAAddr self_addr_;
	RDMAAddr peer_addr_;
	RDMAConfig config_;
	RDMAContext* context_;
	char* buf_;
	size_t buf_size_;
	bool server_side_;
};
#endif
