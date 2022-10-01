#include "pico/cyw43_arch.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "http.h"
#include "tcp.h"


/*!
  * \brief Allocate memory for state structure
  * \return Pointer to TCP server state struct
  */
static TCP_SERVER_T* tcp_server_init(void) {
    TCP_SERVER_T *state = calloc(1, sizeof(TCP_SERVER_T));
    state->message_body = (HTTP_MESSAGE_BODY_T*)calloc(1, sizeof(HTTP_MESSAGE_BODY_T));
    if (!state) {
        DEBUG_printf("failed to allocate state\n");
        return NULL;
    }
    return state;
}


/*!
  * \brief Shut down TCP client connection
  * \param arg TCP server state struct
  */
static err_t tcp_client_close(void *arg) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    err_t err = ERR_OK;
    if (state->client_pcb == NULL) { return err; }
    tcp_arg(state->client_pcb, NULL);
    tcp_poll(state->client_pcb, NULL, 0);
    tcp_sent(state->client_pcb, NULL);
    tcp_recv(state->client_pcb, NULL);
    tcp_err(state->client_pcb, NULL);
    err = tcp_close(state->client_pcb);
    if (err != ERR_OK) {
        DEBUG_printf("close failed %d, calling abort\n", err);
        tcp_abort(state->client_pcb);
        err = ERR_ABRT;
    }
    state->client_pcb = NULL;
    return err;
}

/*!
  * \brief Shut down TCP server
  * \param arg TCP server state struct
  */
static void tcp_server_close(void *arg) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    if (state->server_pcb == NULL) { return; }
    tcp_arg(state->server_pcb, NULL);
    tcp_close(state->server_pcb);
    state->server_pcb = NULL;
}

/*!
  * \brief TCP send callback, called on each data transfer on the back of a tcp_write(). 
  *        Closes client connection when full length of data has been sent
  * \param arg TCP server state struct
  * \param tpcb Client TCP protocol control block
  * \param len Length of data sent
  * \return err_t Success indicator
  */
static err_t tcp_server_send(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    DEBUG_printf("tcp_server_send %u\n", len);
    state->send_len += len;

    if (state->send_len == state->payload_len) {
        state->recv_len = 0;
        state->payload_len = 0;
        DEBUG_printf("tcp_server_send buffer ok\n");
        return tcp_client_close(arg);
    }
    return ERR_OK;
}

/*!
  * \brief Send data to client
  * 
  * \param arg TCP server state struct
  * \param tpcb Client TCP protocol control block
  * \return err_t Success indicator
  */
err_t tcp_server_send_data(void *arg, struct tcp_pcb *tpcb)
{
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;

    state->send_len = 0;
    DEBUG_printf("Writing %ld bytes to client\n", state->payload_len);
    cyw43_arch_lwip_check();
    err_t err = tcp_write(tpcb, state->buffer_send, state->payload_len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        DEBUG_printf("Failed to write data %d\n", err);
        return tcp_client_close(arg);
    }
    return ERR_OK;
}

/*!
  * \brief Process data received from client, call into http module when full buffer is received
  * 
  * \param arg TCP server state struct
  * \param tpcb Client TCP protocol control block
  * \param p Packet buffer
  * \param err Success indicator
  * \return err_t Success indicator
  */
err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    if (!p) {
        return tcp_client_close(arg);
    }
    cyw43_arch_lwip_check();
    if (p->tot_len > 0) {
        DEBUG_printf("tcp_server_recv %d/%d err %d\n", p->tot_len, state->recv_len, err);

        // Receive the buffer
        const uint16_t buffer_left = BUF_SIZE - state->recv_len;
        state->recv_len += pbuf_copy_partial(p, state->buffer_recv + state->recv_len,
                                             p->tot_len > buffer_left ? buffer_left : p->tot_len, 0);
        tcp_recved(tpcb, p->tot_len);
    }

    // Have we have received the whole buffer
    if (state->recv_len == p->tot_len) {
        DEBUG_printf("tcp_server_recv buffer ok: %s\n", state->buffer_recv);
        http_process_recv_data(arg);
        tcp_server_send_data(arg, tpcb);
    }
    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_server_poll(void *arg, struct tcp_pcb *tpcb) {
    DEBUG_printf("tcp_server_poll_fn\n");
    return tcp_client_close(arg);
}

static void tcp_server_err(void *arg, err_t err) {
    if (err != ERR_ABRT) {
        DEBUG_printf("tcp_client_err_fn %d\n", err);
        tcp_client_close(arg);
    }
}


/*!
  * \brief Client connect entrypoint, set up client callbacks or abort if request is in-flight
  * 
  * \param arg TCP server state struct
  * \param client_pcb Client TCP protocol control block
  * \param err Success indicator
  * \return err_t Success indicator
  */
static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;

    // Would need to refactor to be able to handle simultaneous connections or even respond in HTTP,
    // So just abort any additional connections if one is currently in-flight
    if (state->client_pcb != NULL) {
       DEBUG_printf("Connection already in flight, aborting\n");
       tcp_abort(client_pcb); 
       return ERR_ABRT;
    }

    if (err != ERR_OK || client_pcb == NULL) {
        DEBUG_printf("Failure in accept\n");
        tcp_client_close(arg);
        return ERR_VAL;
    }
    DEBUG_printf("----------------\n");
    DEBUG_printf("Client connected\n");

    state->client_pcb = client_pcb;
    tcp_arg(client_pcb, state);
    tcp_sent(client_pcb, tcp_server_send);
    tcp_recv(client_pcb, tcp_server_recv);
    tcp_poll(client_pcb, tcp_server_poll, POLL_TIME_S * 2);
    tcp_err(client_pcb, tcp_server_err);

    return ERR_OK;
}

/*!
  * \brief Attempts to start TCP server, setting up client accept callback if successful
  * 
  * \param arg TCP server state struct
  * \return true TCP server was opened successfully
  * \return false TCP server could not be opened
  */
static bool tcp_server_open(void *arg) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    DEBUG_printf("Starting server at %s on port %u\n", ip4addr_ntoa(netif_ip4_addr(netif_list)), TCP_PORT);

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        DEBUG_printf("failed to create pcb\n");
        return false;
    }

    err_t err = tcp_bind(pcb, NULL, TCP_PORT);
    if (err) {
        DEBUG_printf("failed to bind to port %d\n");
        return false;
    }

    state->server_pcb = tcp_listen_with_backlog(pcb, 1);
    if (!state->server_pcb) {
        DEBUG_printf("failed to listen\n");
        if (pcb) {
            tcp_close(pcb);
        }
        return false;
    }

    tcp_arg(state->server_pcb, state);
    tcp_accept(state->server_pcb, tcp_server_accept);

    return true;
}

/*!
  * \brief TCP entrypoint, initialise tcp server and wifi. Polls wifi connection periodically to retain connectivity
  */
void run_tcp_server(void) {
    if (cyw43_arch_init()) {
        printf("failed to initialise\n");
        return;
    }

    cyw43_arch_enable_sta_mode();

    TCP_SERVER_T *state = tcp_server_init();
    if (!state) {
        return;
    }
    if (!tcp_server_open(state)) {
        tcp_client_close(state);
        tcp_server_close(state);
        free(state->message_body);
        free(state);
        return;
    }

    while(1) {
        // Wifi appears to disconnect after some time, so re-check connection on a timer
        if(!cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
            sleep_ms(10000);
        }
    }

    tcp_client_close(state);
    tcp_server_close(state);
    free(state->message_body);
    free(state);
    cyw43_arch_deinit();
}
