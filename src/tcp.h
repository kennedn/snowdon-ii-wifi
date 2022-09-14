#include "snowdon.h"
#include "http.h"

typedef struct TCP_SERVER_T_ {
    struct tcp_pcb *server_pcb;
    struct tcp_pcb *client_pcb;
    uint8_t buffer_recv[BUF_SIZE];
    uint8_t buffer_send[BUF_SIZE];
    int recv_len;
    int send_len;
    int payload_len;
    int run_count;
    HTTP_MESSAGE_BODY_T *message_body;
} TCP_SERVER_T;

void run_tcp_server(void);