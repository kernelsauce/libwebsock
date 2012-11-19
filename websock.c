#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <openssl/ssl.h>

#include "websock.h"
#include "sha1.h"
#include "base64.h"


void libwebsock_handle_signal(evutil_socket_t sig, short event, void *ptr) {
	libwebsock_context *ctx = ptr;
	event_base_loopexit(ctx->base, NULL);
}

void libwebsock_handle_accept_ssl(evutil_socket_t listener, short event, void *arg) {
	libwebsock_ssl_event_data *evdata = arg;
	libwebsock_context *ctx = evdata->ctx;
	SSL_CTX *ssl_ctx = evdata->ssl_ctx;
	libwebsock_client_state *client_state;
	struct bufferevent *bev;
	struct sockaddr_storage ss, *sa;
	socklen_t slen = sizeof(ss);
	int fd = accept(listener, (struct sockaddr *)&ss, &slen);
	if(fd < 0) {
		fprintf(stderr, "Error accepting new connection.\n");
	} else {
		client_state = (libwebsock_client_state *)malloc(sizeof(libwebsock_client_state));
		if(!client_state) {
			fprintf(stderr, "Unable to allocate memory for new connection state structure.\n");
			close(fd);
			return;
		}
		memset(client_state, 0, sizeof(libwebsock_client_state));
		client_state->sockfd = fd;
		client_state->flags |= STATE_CONNECTING | STATE_IS_SSL;
		client_state->control_callback = ctx->control_callback;
		client_state->onopen = ctx->onopen;
		client_state->onmessage = ctx->onmessage;
		client_state->onclose = ctx->onclose;
		client_state->sa = (struct sockaddr_storage *)malloc(sizeof(struct sockaddr_storage));
		if(!client_state->sa) {
			fprintf(stderr, "Unable to allocate memory for sockaddr_storage.\n");
			free(client_state);
			close(fd);
			return;
		}
		memcpy(client_state->sa, &ss, sizeof(struct sockaddr_storage));
		client_state->ssl = SSL_new(ssl_ctx);
		SSL_set_fd(client_state->ssl, fd);
		if(SSL_accept(client_state->ssl) <= 0) {
			fprintf(stderr, "error during ssl handshake.\n");
		}
		evutil_make_socket_nonblocking(fd);
		bev = bufferevent_openssl_socket_new(ctx->base, -1, client_state->ssl, BUFFEREVENT_SSL_OPEN, BEV_OPT_CLOSE_ON_FREE);
		client_state->bev = bev;
		bufferevent_setcb(bev, libwebsock_handshake, NULL, libwebsock_do_event, (void *)client_state);
		bufferevent_setwatermark(bev, EV_READ, 0, 16384);
		bufferevent_enable(bev, EV_READ | EV_WRITE);
	}
}

void libwebsock_handle_accept(evutil_socket_t listener, short event, void *arg) {
	libwebsock_context *ctx = arg;
	libwebsock_client_state *client_state;
	struct bufferevent *bev;
	struct sockaddr_storage ss, *sa;
	socklen_t slen = sizeof(ss);
	int fd = accept(listener, (struct sockaddr *)&ss, &slen);
	if(fd < 0) {
		fprintf(stderr, "Error accepting new connection.\n");
	} else {
		client_state = (libwebsock_client_state *)malloc(sizeof(libwebsock_client_state));
		if(!client_state) {
			fprintf(stderr, "Unable to allocate memory for new connection state structure.\n");
			close(fd);
			return;
		}
		memset(client_state, 0, sizeof(libwebsock_client_state));
		client_state->sockfd = fd;
		client_state->flags |= STATE_CONNECTING;
		client_state->control_callback = ctx->control_callback;
		client_state->onopen = ctx->onopen;
		client_state->onmessage = ctx->onmessage;
		client_state->onclose = ctx->onclose;
		client_state->sa = (struct sockaddr_storage *)malloc(sizeof(struct sockaddr_storage));
		if(!client_state->sa) {
			fprintf(stderr, "Unable to allocate memory for sockaddr_storage.\n");
			free(client_state);
			close(fd);
			return;
		}
		memcpy(client_state->sa, &ss, sizeof(struct sockaddr_storage));
		evutil_make_socket_nonblocking(fd);
		bev = bufferevent_socket_new(ctx->base, fd, BEV_OPT_CLOSE_ON_FREE);
		client_state->bev = bev;
		bufferevent_setcb(bev, libwebsock_handshake, NULL, libwebsock_do_event, (void *)client_state);
		bufferevent_setwatermark(bev, EV_READ, 0, 16384);
		bufferevent_enable(bev, EV_READ | EV_WRITE);
	}
}

void libwebsock_do_event(struct bufferevent *bev, short event, void *ptr) {
	libwebsock_client_state *state = ptr;
	libwebsock_string *str;

	if((state->flags & STATE_CONNECTED) && state->onclose) {
		state->onclose(state);
	}
	libwebsock_free_all_frames(state);
	if(state->sa) {
		free(state->sa);
	}
	if(state->flags & STATE_CONNECTING) {
		if(state->data) {
			str = state->data;
			if(str->data) {
				free(str->data);
			}
			free(str);
		}
	}

	bufferevent_free(bev);
}

void libwebsock_handle_recv(struct bufferevent *bev, void *ptr) {
	//alright... while we haven't reached the end of data keep trying to build frames
	//possible states right now:
	// 1.) we're receiving the beginning of a new frame
	// 2.) we're receiving more data from a frame that was created previously and was not complete
	libwebsock_client_state *state = ptr;
	libwebsock_frame *current = NULL, *new = NULL;
	struct evbuffer *input;
	unsigned char payload_len_short;
	int i, complete_frame, datalen;
	char buf[1024];

	input = bufferevent_get_input(bev);
	while(evbuffer_get_length(input)) {
		datalen = evbuffer_remove(input, buf, sizeof(buf));

		for(i=0;i<datalen;i++) {

			if(state->current_frame == NULL) {
				state->current_frame = (libwebsock_frame *)malloc(sizeof(libwebsock_frame));
				memset(state->current_frame, 0, sizeof(libwebsock_frame));
				state->current_frame->payload_len = -1;
				state->current_frame->rawdata_sz = FRAME_CHUNK_LENGTH;
				state->current_frame->rawdata = (char *)malloc(state->current_frame->rawdata_sz);
				memset(state->current_frame->rawdata, 0, state->current_frame->rawdata_sz);
			}
			current = state->current_frame;
			if(current->rawdata_idx >= current->rawdata_sz) {
				current->rawdata_sz += FRAME_CHUNK_LENGTH;
				current->rawdata = (char *)realloc(current->rawdata, current->rawdata_sz);
				memset(current->rawdata + current->rawdata_idx, 0, current->rawdata_sz - current->rawdata_idx);
			}
			*(current->rawdata + current->rawdata_idx++) = buf[i];
			complete_frame = libwebsock_complete_frame(current);
			if(complete_frame == 1) {
				if(current->fin == 1) {
					//is control frame
					if((current->opcode & 0x08) == 0x08) {
						libwebsock_handle_control_frame(state, current);
					} else {
						libwebsock_dispatch_message(state, current);
						state->current_frame = NULL;
					}
				} else {
					new = (libwebsock_frame *)malloc(sizeof(libwebsock_frame));
					memset(new, 0, sizeof(libwebsock_frame));
					new->payload_len = -1;
					new->rawdata_sz = FRAME_CHUNK_LENGTH;
					new->rawdata = (char *)malloc(new->rawdata_sz);

					memset(new->rawdata, 0, FRAME_CHUNK_LENGTH);
					new->prev_frame = current;
					current->next_frame = new;
					state->current_frame = new;
				}
			} else if(complete_frame == -1) {
				//FAIL connection
				libwebsock_fail_connection(state);
				break;
			}
		}
	}
}

void libwebsock_fail_connection(libwebsock_client_state *state) {
	struct evbuffer *output = bufferevent_get_output(state->bev);
	char close_frame[] = { 0x88, 0x00 };
	libwebsock_free_all_frames(state);

	evbuffer_add(output, close_frame, 2);


	if(state->sa) {
		free(state->sa);
	}

	free(state);
}

void libwebsock_dispatch_message(libwebsock_client_state *state, libwebsock_frame *current) {
	unsigned long long message_payload_len, message_offset;
	int message_opcode, i;
	char *message_payload;
	libwebsock_frame *first = NULL;
	libwebsock_message *msg = NULL;
	if(current == NULL) {
		fprintf(stderr, "Somehow, null pointer passed to libwebsock_dispatch_message.\n");
		exit(1);
	}
	message_offset = 0;
	message_payload_len = 0;
	for(;current->prev_frame != NULL;current = current->prev_frame) {
		message_payload_len += current->payload_len;
	}
	message_payload_len += current->payload_len;
	first = current;
	message_opcode = current->opcode;
	message_payload = (char *)malloc(message_payload_len + 1);
	memset(message_payload, 0, message_payload_len + 1);
	for(;current != NULL; current = current->next_frame) {
		for(i = 0; i < current->payload_len; i++) {
			*(current->rawdata + current->payload_offset + i) ^= (current->mask[i % 4] & 0xff);
		}
		memcpy(message_payload + message_offset, current->rawdata + current->payload_offset, current->payload_len);
		message_offset += current->payload_len;
	}


	libwebsock_cleanup_frames(first);

	msg = (libwebsock_message *)malloc(sizeof(libwebsock_message));
	memset(msg, 0, sizeof(libwebsock_message));
	msg->opcode = message_opcode;
	msg->payload_len = message_payload_len;
	msg->payload = message_payload;
	if(state->onmessage != NULL) {
		state->onmessage(state, msg);
	} else {
		fprintf(stderr, "No onmessage call back registered with libwebsock.\n");
	}
	free(msg->payload);
	free(msg);
}

void libwebsock_handshake_finish(struct bufferevent *bev, libwebsock_client_state *state) {
	libwebsock_string *str = state->data;
	struct evbuffer *output;
	char buf[1024];
	char sha1buf[45];
	char concat[1024];
	unsigned char sha1mac[20];
	char *tok = NULL, *headers = NULL, *key = NULL;
	char *base64buf = NULL;
	const char *GID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	struct epoll_event ev;
	int sockfd = state->sockfd;
	SHA1Context shactx;
	SHA1Reset(&shactx);
	int n = 0;
	int x = 0;
	
	output = bufferevent_get_output(bev);

	headers = (char *)malloc(str->data_sz + 1);
	if(!headers) {
		fprintf(stderr, "Unable to allocate memory in libwebsock_handshake..\n");
		close(sockfd);
		return;
	}
	memset(headers, 0, str->data_sz + 1);
	strncpy(headers, str->data, str->idx);
	for(tok = strtok(headers, "\r\n"); tok != NULL; tok = strtok(NULL, "\r\n")) {
		if(strstr(tok, "Sec-WebSocket-Key: ") != NULL) {
			key = (char *)malloc(strlen(tok));
			strncpy(key, tok+strlen("Sec-WebSocket-Key: "), strlen(tok));
			break;
		}
	}
	free(headers);
	free(str->data);
	free(str);
	state->data = NULL;

	
	if(key == NULL) {
		fprintf(stderr, "Unable to find key in request headers.\n");
		close(sockfd);
		return;
	}


	memset(concat, 0, sizeof(concat));
	strncat(concat, key, strlen(key));
	strncat(concat, GID, strlen(GID));
	SHA1Input(&shactx, (unsigned char *)concat, strlen(concat));
	SHA1Result(&shactx);
	free(key);
	key = NULL;
	sprintf(sha1buf, "%08x%08x%08x%08x%08x", shactx.Message_Digest[0], shactx.Message_Digest[1], shactx.Message_Digest[2], shactx.Message_Digest[3], shactx.Message_Digest[4]);
	for(n = 0; n < (strlen(sha1buf)/2);n++)
		sscanf(sha1buf+(n*2), "%02hhx", sha1mac+n);
	base64buf = (char *)malloc(256);
	base64_encode(sha1mac, 20, base64buf, 256);
	memset(buf, 0, 1024);
	snprintf(buf, 1024, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", base64buf);
	free(base64buf);

	evbuffer_add(output, buf, strlen(buf));
	bufferevent_setcb(bev, libwebsock_handle_recv, NULL, libwebsock_do_event, (void *)state);

	state->flags &= ~STATE_CONNECTING;
	state->flags |= STATE_CONNECTED;

	if(state->onopen != NULL) {
		state->onopen(state);
	}
}

void libwebsock_handshake(struct bufferevent *bev, void *ptr) {
	libwebsock_client_state *state = ptr;
	libwebsock_string *str = NULL;
	struct evbuffer *input;
	char buf[1024];
	int datalen;
	input = bufferevent_get_input(bev);
	str = state->data;
	if(!str) {
		state->data = (libwebsock_string *)malloc(sizeof(libwebsock_string));
		if(!state->data) {
			fprintf(stderr, "Unable to allocate memory in libwebsock_handshake.\n");
			close(state->sockfd);
			return;
		}
		str = state->data;
		memset(str, 0, sizeof(libwebsock_string));
		str->data_sz = FRAME_CHUNK_LENGTH;
		str->data = (char *)malloc(str->data_sz);
		if(!str->data) {
			fprintf(stderr, "Unable to allocate memory in libwebsock_handshake.\n");
			close(state->sockfd);
			return;
		}
		memset(str->data, 0, str->data_sz);
	}


	while(evbuffer_get_length(input)) {
		datalen = evbuffer_remove(input, buf, sizeof(buf));

		if(str->idx + datalen + 1 >= str->data_sz) {
			str->data = realloc(str->data, str->data_sz + FRAME_CHUNK_LENGTH);
			if(!str->data) {
				fprintf(stderr, "Failed realloc.\n");
				close(state->sockfd);
				return;
			}
			str->data_sz += FRAME_CHUNK_LENGTH;
			memset(str->data + str->idx, 0, str->data_sz - str->idx);
		}
		memcpy(str->data + str->idx, buf, datalen);
		str->idx += datalen;
		if(strstr(str->data, "\r\n\r\n") != NULL || strstr(str->data, "\n\n") != NULL) {
			libwebsock_handshake_finish(bev, state);
		}
	}
}


