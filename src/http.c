#include <string.h>
#include "pico/cyw43_arch.h"
#include "tcp.h"
#include "http.h"
#include "snowdon.h"
#include "hardware/pio.h"
#include "nec.pio.h"


static uint32_t http_ir_lookup(char *code) {
    if (!strcmp(code, "status"))        { return 0; }
    if (!strcmp(code, "power"))         { return 0x807F807F; }
    if (!strcmp(code, "mute"))          { return 0x807FCC33; }
    if (!strcmp(code, "volume_up"))     { return 0x807FC03F; }
    if (!strcmp(code, "volume_down"))   { return 0x807F10EF; }
    if (!strcmp(code, "previous"))      { return 0x807FA05F; }
    if (!strcmp(code, "next"))          { return 0x807F609F; }
    if (!strcmp(code, "play_pause"))    { return 0x807FE01F; }
    if (!strcmp(code, "input"))         { return 0x807F40BF; }
    if (!strcmp(code, "treble_up"))     { return 0x807FA45B; }
    if (!strcmp(code, "treble_down"))   { return 0x807FE41B; }
    if (!strcmp(code, "bass_up"))       { return 0x807F20DF; }
    if (!strcmp(code, "bass_down"))     { return 0x807F649B; }
    if (!strcmp(code, "pair"))          { return 0x807F906F; }
    if (!strcmp(code, "flat"))          { return 0x807F48B7; }
    if (!strcmp(code, "music"))         { return 0x807F946B; }
    if (!strcmp(code, "dialog"))        { return 0x807F54AB; }
    if (!strcmp(code, "movie"))         { return 0x807F14EB; }
    return 1;
}


static void http_message_body_parse(void *arg) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    if (state->buffer_recv == NULL) { return; }
    state->message_body->method = HTTP_METHOD_GET;
    state->message_body->url[0] = '\0';
    state->message_body->version = HTTP_VERSION_1;
    state->message_body->code = 2;

    // Process HTTP message body, example: "PUT /?code=power HTTP/1.1"
    char *message_body = (char*)state->buffer_recv;
    char *delim = " ?=&\r";
    char next_delim;
    char current_delim = '\0';
    char *key;
    char *token;
    while(1) {
        if (*message_body == '\0') { break; }
        token = message_body;
        message_body = strpbrk(message_body, delim);
        if (message_body == NULL) { break; }
        next_delim = *message_body;
        *message_body = '\0';

        switch(current_delim) {
            case '\0':
                DEBUG_printf("http_message_body_parse method: %s\n", token);
                if(!strcmp(token, "GET")) { state->message_body->method = HTTP_METHOD_GET; }
                else if(!strcmp(token, "PUT")) { state->message_body->method = HTTP_METHOD_PUT; }
                else if(!strcmp(token, "POST")) { state->message_body->method = HTTP_METHOD_POST; }
                break;
            case ' ':
                if (next_delim != '\r') {
                    DEBUG_printf("http_message_body_parse url: %s\n", token);
                    strncpy(state->message_body->url, token, count_of(state->message_body->url));
                } else {
                    DEBUG_printf("http_message_body_parse http_ver: %s\n", token);
                    if(!strcmp(token, "HTTP/1")) { state->message_body->version = HTTP_VERSION_1; }
                    else if(!strcmp(token, "HTTP/1.1")) { state->message_body->version = HTTP_VERSION_1_1; }
                    else if(!strcmp(token, "HTTP/2")) { state->message_body->version = HTTP_VERSION_2; }
                    else if(!strcmp(token, "HTTP/3")) { state->message_body->version = HTTP_VERSION_3; }
                }
                break;
            case '?':
            case '&':
                    DEBUG_printf("http_message_body_parse key: %s\n", token);
                    key = token;
                break;
            case '=':
                    DEBUG_printf("http_message_body_parse value: %s\n", token);
                    if (!strcmp(key, "code")) {
                        state->message_body->code = http_ir_lookup(token);
                        DEBUG_printf("http_message_body_parse code: %#x\n", state->message_body->code);
                    }
                break; 
        }
        current_delim = next_delim;
        message_body++; 
        // Carriage return indicates we have reached the end of the first line of message body
        if (current_delim == '\r') { break; }
    }                   
    // Short circuit further processing if code variable has been found 
    if (state->message_body->code != 2) { return; }
    // Return if no header present to indicate JSON content
    if (strstr(message_body, "Content-Type: application/json") == NULL) { return; }

    // Seek to last occurrence of newline, which should contain JSON string
    message_body = strrchr(message_body, '\n');
    message_body++;  // message_body = '{"code": "power"}'
    DEBUG_printf("http_message_body_parse message_body: %s\n", message_body);
                        
                        
    // Lazy man's JSON parser, iterate over key-value pairs from JSON string, ignoring non string values. Treats JSON as a flat file.
                        
    // Seek past first occurance of '{'
    message_body = strpbrk(message_body, "{");
    if (message_body == NULL) {
        DEBUG_printf("http_message_body_parse: malformed json");
        return;
    }
    message_body ++;  // message_body = '"code": "power"}'
    key = NULL;         
    while(1) {
        // Seek past first occurance of '\"'
        message_body = strpbrk(message_body, "\"");
        if (message_body == NULL) { break; }
        message_body++; // message_body = 'code": "power"}'

        // Null terminate the next occurance of '\"' and seek past it, store reference to substring in token
        token = message_body;  
        message_body = strpbrk(message_body, "\"");
        if (message_body == NULL) { break; }
        *message_body = '\0';  // token = 'code'
        message_body++; // message_body = ': "power"}'
        DEBUG_printf("http_message_body_parse key: %s\n", token);

        // Store token in key for later validation
        key = token;

        // Null terminate the next occurance of '\"', store reference to substring in token
        token = message_body;
        message_body = strpbrk(message_body, "\"");
        if (message_body == NULL) { break; }
        *message_body = '\0';  // token = ': '

        // If the token does not appear to be a valid key-value seperator, this means the next value is not a string, 
        // to ignore it we must rewind the last strpbrk / null termination and continue on to next key.
        for(int i=0; token[i]; i++) {
            if (token[i] == ' ' || token[i] == ':') { continue; }
            DEBUG_printf("None string value detected, skipping\n"); 
            *message_body = '\"';
            break;
        }
        if (*message_body == '\"') { 
            message_body--;
            continue; 
        }

        // Value looks valid, seek past '\"', null terminate the next occurance of '\"', store reference to value in token
        message_body++;  // message_body = 'power"}'
        token = message_body;
        message_body = strpbrk(message_body, "\"");
        if (message_body == NULL) { break; }
        *message_body = '\0';  // token = 'power'
        DEBUG_printf("http_message_body_parse value: %s\n", token);

        // Validate that the key associated with this value is the one we want. If so, capture the value and return
        if (!strcmp(key, "code")) { 
            state->message_body->code = http_ir_lookup(token); 
            DEBUG_printf("http_message_body_parse code: %llu\n", state->message_body->code);
            return; 
        }
        message_body++;
    }
}
static void http_generate_response(void *arg, const char *json_body, const char *http_status) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    state->payload_len = sprintf((char*)state->buffer_send, 
        "HTTP/1.1 %s\r\nContent-Length: %d\r\n\r\n%s", http_status, strlen(json_body), json_body);
}


void http_process_recv_data(void *arg, struct tcp_pcb *tpcb) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    http_message_body_parse(arg);
    if (state->message_body->version != HTTP_VERSION_1_1) {
        http_generate_response(arg, "{\"message\": \"HTTP version must be 1.1\"}\n", "400 Bad Request");
        return;
    }
    
    if (strcmp(state->message_body->url, "/")) {
        http_generate_response(arg, "{\"message\": \"Endpoint not found\"}\n", "400 Bad Request");
        return;
    }
    if (state->message_body->method == HTTP_METHOD_GET) {
        http_generate_response(arg, "{\"code\": ["
                "\"status\", "
                "\"power\", "
                "\"mute\", "
                "\"volume_up\", "
                "\"volume_down\", "
                "\"previous\", "
                "\"next\", "
                "\"play_pause\", "
                "\"input\", "
                "\"treble_up\", "
                "\"treble_down\", "
                "\"bass_up\", "
                "\"bass_down\", "
                "\"pair\", "
                "\"flat\", "
                "\"music\", "
                "\"dialog\", "
                "\"movie\""
            "]}\n", "200 OK");
        return;
    }

    if (state->message_body->method != HTTP_METHOD_PUT) {
        http_generate_response(arg, "{\"message\": \"HTTP method not supported\"}\n", "400 Bad Request");
        return;
    }
    
    if (state->message_body->code > 2) {
        pio_sm_put_blocking(PIO_INSTANCE, 0, state->message_body->code);
        http_generate_response(arg, "{\"status\": \"ok\"}\n", "200 OK");
        return;
    }

    if (state->message_body->code == 0) {
        uint32_t gpio;
        do {
            gpio = (gpio_get_all() & RGB_MASK) >> RGB_BASE_PIN;
            switch(gpio) {
                case 0b110: // red
                    http_generate_response(arg, "{\"onoff\": \"off\", \"input\": \"off\"}\n", "200 OK");
                    break;
                case 0b100: // yellow
                    http_generate_response(arg, "{\"onoff\": \"on\", \"input\": \"optical\"}\n", "200 OK");
                    break;
                case 0b000: // white
                    http_generate_response(arg, "{\"onoff\": \"on\", \"input\": \"aux\"}\n", "200 OK");
                    break;
                case 0b101: // green
                    http_generate_response(arg, "{\"onoff\": \"on\", \"input\": \"line-in\"}\n", "200 OK");
                    break;
                case 0b011: // blue
                    http_generate_response(arg, "{\"onoff\": \"on\", \"input\": \"bluetooth\"}\n", "200 OK");
                    break;
                case 0b111: // off
                    busy_wait_ms(50);
                    continue;
            }
        } while(gpio == 0b111);
        return;
    }
    
    if (state->message_body->code == 1) {
        http_generate_response(arg, "{\"message\": \"code not recognised\"}\n", "400 Bad Request");
        return;
    }

    if (state->message_body->code == 2) {
        http_generate_response(arg, "{\"message\": \"code variable required\"}\n", "400 Bad Request");
        return;
    }

}
