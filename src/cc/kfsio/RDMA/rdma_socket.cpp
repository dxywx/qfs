#include "rdma_socket.h"

#include <sys/socket.h> 
#include <netinet/in.h>
#include <arpa/inet.h>

#include "logging.h"

using namespace dmlc;

void GetAvaliableDeviceAndPort(ibv_device*& dev,int& ib_port) {
    ibv_context *ctx;
    ibv_device** dev_list;
    ibv_device_attr device_attr;
    ibv_device_attr_ex attrx;
    ibv_port_attr port_attr;
	CHECK_NOTNULL(dev_list = ibv_get_device_list(NULL));
	while(dev_list) {
		CHECK_NOTNULL(ctx = ibv_open_device(*dev_list));
		CHECK_EQ(ibv_query_device(ctx, &device_attr),0)
			<< "Failed to query device props";
		for(int port = 1; port <= device_attr.phys_port_cnt; port++) {
			CHECK_EQ(ibv_query_port(ctx, port, &port_attr),0)
				<< "Failed to query port props";
			if(port_attr.state == IBV_PORT_ACTIVE) {
				dev = *dev_list;
				ib_port = port;
				ibv_close_device(ctx);
				return;
			}
		}
		ibv_close_device(ctx);
		dev_list++;
	}
	CHECK_NOTNULL(dev);
	return;	
}


RDMASocket::RDMASocket(const RDMAConfig& config,int buf_size) {
	config_ = config;
	buf_size_ = buf_size;
	buf_ = new char[buf_size];
	server_side_ = (config.tcp_host == NULL);
	// Bind();
}

RDMASocket::RDMASocket(const char* tcp_host,int tcp_port,
	int num_comp_queue_entries,int buf_size) {
	config_.tcp_host = tcp_host;
	config_.tcp_port = tcp_port;
	config_.num_comp_queue_entries = num_comp_queue_entries;
	buf_size_ = buf_size;
	buf_ = new char[buf_size];
	server_side_ = (tcp_host == NULL);
	// Bind();
}

RDMASocket::RDMASocket(int tcp_port,
	int num_comp_queue_entries,int buf_size) {
	config_.tcp_port = tcp_port;
	config_.num_comp_queue_entries = num_comp_queue_entries;
	buf_size_ = buf_size;
	buf_ = new char[buf_size];
	server_side_ = true;
	// Bind();
}

void RDMASocket::Close() {
	if(buf_)
		delete[] buf_;
	if(context_) {
		ibv_close_device(context_->context);
		delete context_;
	}
}

RDMASocket::~RDMASocket() {
	Close();
}

bool RDMASocket::Bind() {
	bool allThrough = false;
	do {
		if (!InitContext()) break;
		if (!GenerateLocalAddr()) break;
		if (!ExchangeAddrThroughTCP()) break;
		if (!InitQueuePair()) break;
		if (!SetQueuePairRTR()) break;
		if (!SetQueuePairRTS()) break;
		allThrough = true;
	} while(0);
	return allThrough;
}

bool RDMASocket::InitContext() {
	context_ = new RDMAContext;
	memset(context_,0,sizeof(*context_));
	context_->num_comp_queue_entries = config_.num_comp_queue_entries;
	GetAvaliableDeviceAndPort(config_.dev,config_.ib_port);
	//CHECK_NOTNULL(dev_list = ibv_get_device_list(NULL));
	CHECK_NOTNULL(context_->context = ibv_open_device(config_.dev));
	CHECK_NOTNULL(context_->pd = ibv_alloc_pd(context_->context));
	CHECK_NOTNULL(context_->mr = ibv_reg_mr(context_->pd,buf_,buf_size_,
		IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE));
	CHECK_NOTNULL(context_->ch = ibv_create_comp_channel(context_->context));
	CHECK_NOTNULL(context_->recv_cq = ibv_create_cq(context_->context,
		context_->num_comp_queue_entries,NULL,NULL, 0));
	CHECK_NOTNULL(context_->send_cq = ibv_create_cq(context_->context,
		context_->num_comp_queue_entries,context_,context_->ch,0));
	ibv_qp_init_attr qp_init_attr;
	memset(&qp_init_attr,0,sizeof(qp_init_attr));
	qp_init_attr.send_cq = context_->send_cq;
	qp_init_attr.recv_cq = context_->recv_cq;
	qp_init_attr.qp_type = IBV_QPT_RC;
	qp_init_attr.cap.max_send_wr = context_->num_comp_queue_entries;
	qp_init_attr.cap.max_recv_wr = context_->num_comp_queue_entries;
	qp_init_attr.cap.max_send_sge = 1;
	qp_init_attr.cap.max_recv_sge = 1;
	qp_init_attr.cap.max_inline_data = 0;
	context_->qp = ibv_create_qp(context_->pd,&qp_init_attr);
	CHECK_NOTNULL(context_->qp);
	return true;
}

bool RDMASocket::InitQueuePair() {
    ibv_qp_attr *attr = new ibv_qp_attr;
    memset(attr, 0, sizeof(*attr));

    attr->qp_state        	= IBV_QPS_INIT;
    attr->pkey_index      	= 0;
    attr->port_num        	= config_.ib_port;
    attr->qp_access_flags	= IBV_ACCESS_REMOTE_WRITE|IBV_ACCESS_REMOTE_READ;

    CHECK_EQ(ibv_modify_qp(context_->qp, attr,
       IBV_QP_STATE|IBV_QP_PKEY_INDEX|IBV_QP_PORT|IBV_QP_ACCESS_FLAGS),0) 
    		<< "Could not modify QP to INIT, ibv_modify_qp";
    delete attr;
	return true;
}

bool RDMASocket::SetQueuePairRTR() {
	ibv_qp_attr *attr = new ibv_qp_attr;

    memset(attr, 0, sizeof(*attr));

    attr->qp_state              = IBV_QPS_RTR;
    attr->path_mtu              = IBV_MTU_2048;
    attr->dest_qp_num           = peer_addr_.qpn;
    attr->rq_psn                = peer_addr_.psn;
    attr->max_dest_rd_atomic    = 1;
    attr->min_rnr_timer         = 12;
    attr->ah_attr.is_global     = 0;
    attr->ah_attr.dlid          = peer_addr_.lid;
    //attr->ah_attr.sl            = sl;
    attr->ah_attr.src_path_bits = 0;
    attr->ah_attr.port_num      = config_.ib_port;

    CHECK_EQ(ibv_modify_qp(context_->qp, attr,
                IBV_QP_STATE|IBV_QP_AV|IBV_QP_PATH_MTU|IBV_QP_DEST_QPN|
		IBV_QP_RQ_PSN|IBV_QP_MAX_DEST_RD_ATOMIC|IBV_QP_MIN_RNR_TIMER),0)
    		<< "Could not modify QP to RTR state";

	delete attr;
	return true;
}

bool RDMASocket::SetQueuePairRTS() {
    ibv_qp_attr *attr = new ibv_qp_attr;
    memset(attr, 0, sizeof *attr);

    attr->qp_state              = IBV_QPS_RTS;
    attr->timeout               = 14;
    attr->retry_cnt             = 7;
    attr->rnr_retry             = 7;    /* infinite retry */
    attr->sq_psn                = self_addr_.psn;
    attr->max_rd_atomic         = 1;

    CHECK_EQ(ibv_modify_qp(context_->qp, attr,
                IBV_QP_STATE|IBV_QP_TIMEOUT|IBV_QP_RETRY_CNT|
                IBV_QP_RNR_RETRY|IBV_QP_SQ_PSN|IBV_QP_MAX_QP_RD_ATOMIC),0)
        << "Could not modify QP to RTS state";

	return true;
}

bool RDMASocket::GenerateLocalAddr() {
	ibv_port_attr attr;
	ibv_query_port(context_->context,config_.ib_port,&attr);
	self_addr_.lid = attr.lid;
	self_addr_.qpn = context_->qp->qp_num;
	srand(time(0));
	self_addr_.psn = rand() & 0xffffff;
	return true;
}

bool RDMASocket::ExchangeAddrThroughTCP() {
	int sockfd = -1;
	int serverfd = -1;
	//create a tcp server socket which listens for incoming connections
	if(server_side_) {
		sockaddr_in server_addr;
        memset(&server_addr,0,sizeof(server_addr));
		server_addr.sin_family = AF_INET;
    	server_addr.sin_port = htons(config_.tcp_port);
        server_addr.sin_addr.s_addr = htons(INADDR_ANY);
        serverfd = socket(AF_INET, SOCK_STREAM, 0);
        CHECK_GE(serverfd,0);
        int opt =1;
        setsockopt(serverfd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    	CHECK_EQ(bind(serverfd, reinterpret_cast<const sockaddr*>(&server_addr),
    		sizeof(server_addr)),0);
    	CHECK_EQ(listen(serverfd,12),0);
        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
    	sockfd = accept(serverfd,
            reinterpret_cast<sockaddr*>(&client_addr),
            &addr_len);
    	CHECK_GE(sockfd,0);
	}
	else {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        CHECK_GE(sockfd,0);

        sockaddr_in server_addr;
        memset(&server_addr,0,sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(config_.tcp_port);
    	inet_aton(config_.tcp_host, &server_addr.sin_addr);
    	connect(sockfd,
    		reinterpret_cast<const sockaddr*>(&server_addr),
    		sizeof(server_addr));
	}
    const char* addr_pattern = "0000:000000:000000";
	char addr_str[32];
	sprintf(addr_str,"%04x:%06x:%06x",
            self_addr_.lid,
            self_addr_.qpn,
            self_addr_.psn);
    
    if(send(sockfd,addr_str,sizeof(addr_str),0) != sizeof(addr_str)) {
		LOG(ERROR) << "Could not send local address to peer";
	}
	if(recv(sockfd,addr_str,sizeof(addr_str),0) != sizeof(addr_str)) {
		LOG(ERROR) << "Could not receive local address to peer";
	}
	int parsed = sscanf(addr_str,"%x:%x:%x",&peer_addr_.lid,
		&peer_addr_.qpn,
		&peer_addr_.psn);
	CHECK_EQ(parsed,3) << "Coluld not parse message from peer";
	close(sockfd);
    if(server_side_)
        close(serverfd);

	return true;
}

void RDMASocket::SendMsg(char* msg,size_t size) {
	memcpy(buf_,msg,size);
	context_->sge_list.addr      = (uint64_t)buf_;
   	context_->sge_list.length    = size;
   	context_->sge_list.lkey      = context_->mr->lkey;

    context_->send_wr.wr_id       = (uint64_t)context_;
    context_->send_wr.sg_list     = &context_->sge_list;
    context_->send_wr.num_sge     = 1;
    context_->send_wr.opcode      = IBV_WR_SEND;
    context_->send_wr.send_flags  = IBV_SEND_SIGNALED;
    context_->send_wr.next        = NULL;

    ibv_send_wr *bad_wr;
    CHECK_EQ(ibv_post_send(context_->qp,&context_->send_wr,&bad_wr),0)
   		<< "ibv_post_send failed.This is bad mkey";
   	int num_succeeded = 0;
   	ibv_wc wc;
   	do {
   		num_succeeded = ibv_poll_cq(context_->send_cq,1,&wc);
   	} while(num_succeeded == 0);
   	CHECK_GE(num_succeeded,0) << "poll CQ failed";
   	CHECK_EQ(wc.status,IBV_WC_SUCCESS)<< ibv_wc_status_str(wc.status);
}  	

void RDMASocket::RecvMsg(char*& msg,size_t& size) {
	context_->sge_list.addr      = (uint64_t)buf_;
   	context_->sge_list.length    = buf_size_;
   	context_->sge_list.lkey      = context_->mr->lkey;

    context_->recv_wr.wr_id       = (uint64_t)context_;
    context_->recv_wr.sg_list     = &context_->sge_list;
    context_->recv_wr.num_sge     = 1;
    context_->recv_wr.next        = NULL;

    ibv_recv_wr *bad_wr;
    CHECK_EQ(ibv_post_recv(context_->qp,&context_->recv_wr,&bad_wr),0)
   		<< "ibv_post_send failed.This is bad mkey";
   	int num_succeeded = 0;
   	ibv_wc wc;
   	do {
   		num_succeeded = ibv_poll_cq(context_->recv_cq,1,&wc);
   	} while(num_succeeded == 0);

   	CHECK_GE(num_succeeded,0) << "poll CQ failed";

   	CHECK_EQ(wc.status,IBV_WC_SUCCESS);
   	size = wc.byte_len;
    msg = buf_;
    //msg = new char[size];
    //memcpy(msg,buf_,size);
    //msg = buf_;
}  
