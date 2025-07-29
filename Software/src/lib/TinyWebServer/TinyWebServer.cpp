#include "TinyWebServer.h"

#include <algorithm>
#include <fcntl.h>
#include <memory>

#ifndef LOCAL
#include <src/devboard/utils/logging.h>
#define tws_log_printf logging.printf
#endif



extern "C" {
    #ifdef LOCAL
    #include <stdio.h>
    #include <sys/socket.h>
    #include <errno.h>
    #include <unistd.h>
    #include <string.h>
    #include <netinet/in.h>
    #include <time.h>

    #define tws_log_printf printf

    unsigned long millis(void) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
    }

    TwsRequestHandlerEntry default_handlers[] = {
        TwsRequestHandlerEntry("/", TWS_HTTP_GET, [](TwsRequest& request) {
            request.set_writer_callback([](TwsRequest &req, int alreadyWritten) {
                //tws_log_printf("TWS request writer callback: %d bytes already written\n", alreadyWritten);
                if(alreadyWritten==0) {
                    req.write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/html\r\n\r\n<h3>Hello World!</h3>\n");
                } else if (alreadyWritten > 1000) {
                    req.write("end!");
                    req.finish();
                } else {
                    //req.write("Hello World!\n");
                    // while(req.free() > 27 && alreadyWritten < 1000) {
                    //     alreadyWritten += req.write((const uint8_t *)"<p>banana banana banana</p>", 27);
                    // }
                    while(alreadyWritten < 1000) {
                        const int written = req.write_direct("<p>banana banana banana</p>", 27);
                        if(written < 27) {
                            break;
                        }
                        alreadyWritten += written;
                    }
                }
            });
        }),
        TwsRequestHandlerEntry("/moo", TWS_HTTP_GET, [](TwsRequest& request) {
            request.write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/html\r\n\r\nmoo\n");
            request.finish();
        }),
        TwsRequestHandlerEntry(nullptr, 0, nullptr) // Sentinel entry to mark the end of the array
    };

    int main() {
        TinyWebServer tws(12345, default_handlers);
        tws.openPort();

        // tws.on("/", TWS_HTTP_GET, [](TwsRequest& request) {
        //     //if (WEBSERVER_AUTH_REQUIRED && !request->authenticate(http_username, http_password))
        //         //return request->requestAuthentication();
        //     //request->send(200, "text/html", index_html, processor);
        //     // request.write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/html\r\nContent-Length: 22\r\n\r\n<h3>Hello World!</h3>\n");
        //     // request.finish();
        //     request.set_writer_callback([](TwsRequest &req, int alreadyWritten) {
        //         //tws_log_printf("TWS request writer callback: %d bytes already written\n", alreadyWritten);
        //         if(alreadyWritten==0) {
        //             req.write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/html\r\n\r\n<h3>Hello World!</h3>\n");
        //         } else if (alreadyWritten > 1000) {
        //             req.write("end!");
        //             req.finish();
        //         } else {
        //             //req.write("Hello World!\n");
        //             // while(req.free() > 27 && alreadyWritten < 1000) {
        //             //     alreadyWritten += req.write((const uint8_t *)"<p>banana banana banana</p>", 27);
        //             // }
        //             while(alreadyWritten < 1000) {
        //                 const int written = req.write_direct("<p>banana banana banana</p>", 27);
        //                 if(written < 27) {
        //                     break;
        //                 }
        //                 alreadyWritten += written;
        //             }
        //         }
        //     });
        // });
        

        while (true) {
            tws.poll();
            tws.tick();
            //usleep(50000); // Sleep for 50ms
        }

        return 0;
    }

    #else
    #include "lwip/sockets.h"
    #endif
    extern unsigned long millis(void);
}



TinyWebServer::TinyWebServer(uint16_t port, TwsRequestHandlerEntry *handlers) {
    _port = port;
    _handlers = handlers;
}

TinyWebServer::~TinyWebServer() {
    if (_listen_socket >= 0) {
        close(_listen_socket);
        _listen_socket = -1;
    }
}

void TinyWebServer::openPort() {
    int listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket < 0) return;
    _listen_socket = listen_socket;

    const int enable = 1;
    if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
        tws_log_printf("setsockopt(SO_REUSEADDR) failed\n");

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = 0;//(uint32_t) _addr;
    server.sin_port = htons(_port);
    if (bind(_listen_socket, (struct sockaddr *)&server, sizeof(server)) < 0) {
        tws_log_printf("TWS bind error\n");
        return;
    }

    static uint8_t backlog = 5;
    if (listen(_listen_socket , backlog) < 0) {
        close(_listen_socket);

        //log_e("listen error: %d - %s", errno, strerror(errno));
        tws_log_printf("TWS listen error\n");
        return;
    }
    int flags = fcntl(_listen_socket, F_GETFL);
    fcntl(_listen_socket, F_SETFL, flags | O_NONBLOCK);
}

void TinyWebServer::tick() {
    // Accept new connections
    //acceptNewConnections();

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].socket >= 0) {
            clients[i].tick();
            handleClient(clients[i]);

            
        }
    }
}

void TinyWebServer::handleClient(TinyWebServerClient &client) {
    while(true) {
        int len = 0;
        switch(client.parse_state) {
            case TWS_AWAITING_HEADER:
                len = client.scan('\n', '\n');
                if(len==0 && client.recv_buffer_full()) {
                    // We can't fit the whole header into the buffer, so junk it
                    client.read_flush(client.available());
                }
                break;
            default:
                len = client.scan(' ', '\n');
                break;
        }
        if(len==0) {
            if(client.recv_buffer_full()) {
                tws_log_printf("TWS client buffer full, closing connection\n");
                client.reset();
            }
            break;
        }

        bool found = false;
        switch(client.parse_state) {
        case TWS_AWAITING_METHOD:
            if(client.match("POST ", 5)) {
                client.method = TWS_HTTP_POST;
                client.parse_state = TWS_AWAITING_PATH;
            } else if(client.match("GET ", 4)) {
                client.method = TWS_HTTP_GET;
                client.parse_state = TWS_AWAITING_PATH;
            } else {
                // char mbuf[len+1];
                // clients[i].read((uint8_t*)mbuf, len);
                // mbuf[len] = '\0'; // Null-terminate the string
                tws_log_printf("TWS client invalid method! %d %d [%s]\n", client.recv_buffer_read_ptr, client.recv_buffer_scan_ptr, client.recv_buffer);
                client.reset();
            }
            break;
        case TWS_AWAITING_PATH:
            for(int j=0;_handlers[j].uri!=nullptr;j++) {
                int uri_len = strlen(_handlers[j].uri);
                if(len == (uri_len + 1) && client.match(_handlers[j].uri, uri_len)) {
                    client.read(); // consume the delimiter
                    client.handler = &_handlers[j];
                    found = true;
                    break;
                }
            }
            if(!found) {
                client.read_flush(len);
            }
            // for(auto &handler : _handlers) {
            //     if(clients[i].match(handler.uri, strlen(handler.uri))) {
            //         clients[i].handler = &handler;
            //         break;
            //     }
            // }
            client.parse_state = TWS_AWAITING_VERSION;
            // if(clients[i].match("/ ", 2)) {
            //     clients[i].parse_state = TWS_AWAITING_VERSION;
            // } else {
            //     tws_log_printf("TWS client %d invalid path!\n", i);
            //     clients[i].reset();
            // }
            break;
        case TWS_AWAITING_VERSION:
            if(client.match("HTTP/1.1\r\n", 10) || client.match("HTTP/1.1\n", 9) ||
                client.match("HTTP/1.0\r\n", 10) || client.match("HTTP/1.0\n", 9)) {
                //tws_log_printf("TWS client %d HTTP version OK\n", i);
            //if(clients[i].match("HTTP/1.1", 8) || clients[i].match("HTTP/1.0", 8)) {
                client.parse_state = TWS_AWAITING_HEADER;
            } else {
                tws_log_printf("TWS client invalid HTTP version!\n");
                client.reset();
            }
            break;
        case TWS_AWAITING_HEADER:
            if(client.match("\r\n", 2) || client.match("\n", 1)) {
                // End of headers, process the request
                //tws_log_printf("TWS client %d headers complete\n", i);
                //clients[i].parse_state = TWS_AWAITING_METHOD; // Reset for next request
                // Here you would typically call the registered handler for the request
                // For now, we just log it
                
                //clients[i].write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/html\r\nContent-Length: 22\r\n\r\n<h3>Hello World!</h3>\n");
                if(client.handler) {
                    // Call the handler function
                    client.handler->onRequest(client);
                } else {
                    client.write("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\n404 Not Found\n");
                    client.finish();
                }

                // for(auto &handler : _handlers) {
                //     if(handler.method == clients[i].method) {
                //         // Call the handler function
                //         handler.onRequest(clients[i]);
                //         break;
                //     }
                // }
                
                //clients[i].parse_state = TWS_AWAITING_METHOD; // Reset for next request

                //clients[i].reset();
            // } else if(clients[i].match("Connection: keep-alive\r\n", 24) ||
            //           clients[i].match("Connection: keep-alive\n", 23)) {
            //     // Keep-alive connection, continue reading headers
            //     //tws_log_printf("TWS client %d keep-alive header\n", i);
            //     clients[i].keep_alive = true;
            
            } else {
                //char buf[len+1];
                client.read_flush(len);
                //clients[i].read((uint8_t*)buf, len);
                //buf[len] = '\0'; // Null-terminate the string
                //tws_log_printf("TWS client %d got header: %d %s\n", i, len, buf);
            }
        }
    }
}

void TinyWebServer::acceptNewConnections() {
    // Find a free client slot
    int client_index = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].socket == -1) {
            client_index = i;
            break;
        }
    }
    if (client_index < 0) {
        // No free client slot, cannot accept new connections
        return;
    }

    int socket = accept(_listen_socket, NULL, NULL);

    if (socket < 0) {
        if (errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK) {
            // Not a problem, ignore
            return;
        }
        // Another error occurred
        tws_log_printf("TWS accept error\n");
        return;
    }

    int flags = fcntl(socket, F_GETFL);
    fcntl(socket, F_SETFL, flags | O_NONBLOCK);

    clients[client_index].reset();
    clients[client_index].socket = socket;
    clients[client_index].last_activity = millis();

    //clients[client_index].write((const uint8_t*)"Hello woorld!\n", 14);
}

void TinyWebServer::closeOldConnections() {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].socket >= 0) {
            if(close(clients[i].socket)<0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Failed to close, try again later.
                    continue;
                }
            }
            clients[i].socket = -1; // Mark as available
        }
    }
}

void TinyWebServer::poll() {
    fd_set read_sockets, write_sockets;
    FD_ZERO(&read_sockets);
    FD_ZERO(&write_sockets);
    FD_SET(_listen_socket, &read_sockets);
    int max_fds = _listen_socket + 1;
    // for (int i = 0; i < MAX_CLIENTS; i++) {
    //     if (clients[i].socket >= 0) {
    //         FD_SET(clients[i].socket, &read_sockets);
    //         max_fds = std::max(max_fds, clients[i].socket + 1);
    //         if( clients[i].send_buffer_len > 0 || clients[i].pending_direct_write ) {
    //             FD_SET(clients[i].socket, &write_sockets);
    //         }
    //     }
    // }
    for (auto &client : clients) {
        if (client.socket >= 0) {
            FD_SET(client.socket, &read_sockets);
            max_fds = std::max(max_fds, client.socket + 1);
            if (client.send_buffer_len > 0 || client.pending_direct_write) {
                FD_SET(client.socket, &write_sockets);
            }
        }
    }

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 50000;

    int activity = select(max_fds, &read_sockets, &write_sockets, NULL, &timeout);
    if(activity>0) {
        //tws_log_printf("TWS select activity: %d\n", activity);
        // Check if the socket has closed
        if (FD_ISSET(_listen_socket, &read_sockets)) {
            //tws_log_printf("TWS new connection available\n");
            // New connection available
            acceptNewConnections();
        }
        // Check each client socket for activity
        //for (int i = 0; i < MAX_CLIENTS; i++) {
        for (auto &client : clients) {
            if (client.socket >= 0 && FD_ISSET(client.socket, &read_sockets)) {
                
                //|| FD_ISSET(clients[i].socket, &write_sockets)) {
                // if(FD_ISSET(clients[i].socket, &write_sockets) && clients[i].pending_direct_write) {
                //     // There's now space so our direct write must have finished
                //     clients[i].pending_direct_write = false;
                // }

                // Client socket has activity, process it
                client.tick();
            } else if(client.socket >= 0 && (client.send_buffer_len > 0 || client.pending_direct_write) && FD_ISSET(client.socket, &write_sockets)) {
                // any pending direct write must have finished
                client.pending_direct_write = false;
                // Client socket is ready for writing
                client.tick();
            }
        }

    }
}

void TinyWebServer::on(const char *uri, int method, TwsRequestHandlerFunction onRequest) {
    // This function is a placeholder for handling requests.
    // In a real implementation, you would store the handler and call it when a request matches the URI.
    //tws_log_printf("TWS on() called for URI: %s with method: %d\n", uri, method);
    //_handlers.push_back({uri, method, onRequest});

}

void TinyWebServerClient::reset() {
    if(socket>-1) {
        close(socket);
    }
    socket = -1;
    send_buffer_len = 0;
    send_buffer_write_ptr = 0;
    send_buffer_read_ptr = 0;
    recv_buffer_len = 0;
    recv_buffer_write_ptr = 0;
    recv_buffer_read_ptr = 0;
    recv_buffer_scan_len = 0;
    recv_buffer_scan_ptr = 0;
    parse_state = TWS_AWAITING_METHOD;
    //keep_alive = false;
    done = false;
    total_written = 0;
    pending_direct_write = false;
    writer_callback = nullptr;
    handler = nullptr;
}

void TinyWebServerClient::tick() {
    auto now = millis();

    // uint8_t buf[100];
    // int bytes_read = read(buf, sizeof(buf));
    // if(bytes_read>0) {
    //     write(buf, bytes_read); // Echo back the received data
    //     logging.printf("TWS %d %d %d %d\n", bytes_read, send_buffer_len, send_buffer_read_ptr, send_buffer_write_ptr);
    // }

    // send any data awaiting sending
    while(send_buffer_len>0) {
        const int contiguous_block_size = BUFFER_SIZE - send_buffer_read_ptr;
        const int bytes_to_send = std::min(send_buffer_len, contiguous_block_size);
        const int bytes_sent = ::send(socket, &send_buffer[send_buffer_read_ptr], bytes_to_send, 0);
        if (bytes_sent > 0) {
            // Advance the read pointer, wrapping around the buffer if necessary.
            send_buffer_read_ptr = (send_buffer_read_ptr + bytes_sent) % BUFFER_SIZE;
            send_buffer_len -= bytes_sent;
            last_activity = now;
        } else if (bytes_sent <= 0 && errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
            // An error occurred, close the connection
            tws_log_printf("TWS send error: %d - %s\n", errno, strerror(errno));
            reset();
            return;
        } else {
            // No more space available to write, break out of the loop
            break;
        }
    }

    // Read any incoming data
    while(recv_buffer_len < BUFFER_SIZE) {
        const int contiguous_block_size = BUFFER_SIZE - recv_buffer_write_ptr;
        const int bytes_to_read = std::min(BUFFER_SIZE - recv_buffer_len, contiguous_block_size);
        int bytes_received = ::recv(socket, &recv_buffer[recv_buffer_write_ptr], bytes_to_read, 0);
        //printf("TWS recv %d %d\n", bytes_received, errno);
        if (bytes_received > 0) {
            // Advance the write pointer, wrapping around the buffer if necessary.
            recv_buffer_write_ptr = (recv_buffer_write_ptr + bytes_received) % BUFFER_SIZE;
            recv_buffer_len += bytes_received;
            last_activity = now;
        } else if (bytes_received == 0) {
            // Connection closed by the client
            //tws_log_printf("TWS client closed connection\n");
            reset();
            return;
        } else if (bytes_received < 0 && errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
            // An error occurred, close the connection
            tws_log_printf("TWS recv error: %d - %s\n", errno, strerror(errno));
            reset();
            return;
        } else {
            // No more data available to read
            break;
        }
    }

    if(send_buffer_len==0 && done && !pending_direct_write) {
        // If the send buffer is empty and the request is done, close the connection
        reset();
        return;
    }

    if(send_buffer_len==0 && !done && writer_callback) {
        // If the send buffer is empty and a callback is set, call it
        writer_callback(*this, this->total_written);
    }

    if((now - last_activity) > 10000) { // 10 seconds timeout
        // No activity for 10 seconds, close the connection
        tws_log_printf("TWS client timeout, closing connection\n");
        reset();
        return;
    }
}

int TinyWebServerClient::write(const char *buf) {
    if (send_buffer_len >= BUFFER_SIZE) {
        return 0; // Buffer full
    }
    int i;
    for(i = 0; buf[i] != '\0' && send_buffer_len < BUFFER_SIZE; ++i) {
        // Place the byte at the current write position.
        send_buffer[send_buffer_write_ptr] = buf[i];
        send_buffer_write_ptr = (send_buffer_write_ptr + 1) % BUFFER_SIZE;
        send_buffer_len++;
    }

    total_written += i; // Update total written bytes

    // Return the number of bytes written
    return i;
}

int TinyWebServerClient::write(const char* buf, int len) {
    int bytes_sent = 0;
    if(send_buffer_len==0) {
        // try to shortcut
        bytes_sent = ::send(socket, buf, len, 0);
        if(bytes_sent>0) {
            len -= bytes_sent;
            buf += bytes_sent;
            total_written += bytes_sent;
        } else {
            bytes_sent = 0;
        }
        if(len==0) {
            return bytes_sent;
        }
    }
    
    const int available_space = BUFFER_SIZE - send_buffer_len;
    if (available_space <= 0) {
        return 0;
    }
    const int bytes_to_copy = std::min(len, available_space);

    for (int i = 0; i < bytes_to_copy; ++i) {
        // Place the byte at the current write position.
        send_buffer[send_buffer_write_ptr] = buf[i];
        send_buffer_write_ptr = (send_buffer_write_ptr + 1) % BUFFER_SIZE;
    }

    // Increase the stored data length by the number of bytes copied.
    send_buffer_len += bytes_to_copy;
    total_written += bytes_to_copy; // Update total written bytes
    return bytes_sent + bytes_to_copy;
}

int TinyWebServerClient::write(const char byte) {
    if (send_buffer_len >= BUFFER_SIZE) {
        return -1; // Buffer full
    }
    send_buffer[send_buffer_write_ptr] = byte;
    send_buffer_write_ptr = (send_buffer_write_ptr + 1) % BUFFER_SIZE;
    send_buffer_len++;
    total_written++;
    return 1; // Success
}

int TinyWebServerClient::write_direct(const char *buf, int len) {
    if(send_buffer_len > 0) {
        // Can't bypass the send buffer if there is already data in it
        return 0;
    }
    if(socket==-1) return 0;
    const int bytes_written = ::send(socket, buf, len, 0);
    if(bytes_written < 0 && errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
        // An error occurred, close the connection
        tws_log_printf("TWS write_direct error: %d - %s\n", errno, strerror(errno));
        reset();
        return 0;
    } else if(bytes_written < 0) {
        // No data was written, return 0
        return 0;
    }
    total_written += bytes_written; // Update total written bytes
    pending_direct_write = true;
    return bytes_written;
}

int16_t TinyWebServerClient::read() {
    if (recv_buffer_len <= 0) {
        return -1; // No data available
    }
    int16_t byte = recv_buffer[recv_buffer_read_ptr];
    recv_buffer_read_ptr = (recv_buffer_read_ptr + 1) % BUFFER_SIZE;
    recv_buffer_len--;
    return byte;
}
int TinyWebServerClient::read(char *buf, int len) {
    if (recv_buffer_len <= 0) {
        return 0; // No data available
    }
    int bytes_to_read = std::min(len, recv_buffer_len);
    for (int i = 0; i < bytes_to_read; ++i) {
        buf[i] = recv_buffer[recv_buffer_read_ptr];
        recv_buffer_read_ptr = (recv_buffer_read_ptr + 1) % BUFFER_SIZE;
    }
    recv_buffer_len -= bytes_to_read;
    recv_buffer_scan_ptr = recv_buffer_read_ptr; // Reset scan pointer to the read position
    recv_buffer_scan_len = 0; // Reset scan length
    return bytes_to_read;
}
int TinyWebServerClient::available() {
    return recv_buffer_len; // Return the number of bytes available to read
}
int TinyWebServerClient::free() {
    return BUFFER_SIZE - send_buffer_len; // Return the number of free bytes in the send buffer
}
bool TinyWebServerClient::recv_buffer_full() {
    return recv_buffer_len >= BUFFER_SIZE; // Return true if the receive buffer is full
}
int TinyWebServerClient::read_flush(int len) {
    const int bytes_to_flush = std::min(len, recv_buffer_len);
    recv_buffer_read_ptr = (recv_buffer_read_ptr + bytes_to_flush) % BUFFER_SIZE;
    recv_buffer_len -= bytes_to_flush;
    
    const int scan_bytes_to_flush = std::min(len, recv_buffer_scan_len);
    recv_buffer_scan_ptr = (recv_buffer_scan_ptr + scan_bytes_to_flush) % BUFFER_SIZE;
    recv_buffer_scan_len -= scan_bytes_to_flush;

    return bytes_to_flush;
}

int TinyWebServerClient::scan(const char delim1, char delim2) {
    // Keep looping until we catch up with the write pointer
    while(recv_buffer_scan_len<recv_buffer_len) {//recv_buffer_scan_ptr != recv_buffer_write_ptr) {
        //printf("TWS scan %d %d %d\n", recv_buffer_scan_ptr, recv_buffer_write_ptr, recv_buffer_scan_len);
        if (recv_buffer[recv_buffer_scan_ptr] == delim1 || recv_buffer[recv_buffer_scan_ptr] == delim2) {
            // Skip over the delimiter
            recv_buffer_scan_ptr = (recv_buffer_scan_ptr + 1) % BUFFER_SIZE;
            int count = recv_buffer_scan_len+1;
            recv_buffer_scan_len = 0;
            return count;
        }
        recv_buffer_scan_ptr = (recv_buffer_scan_ptr + 1) % BUFFER_SIZE;
        recv_buffer_scan_len++;
    }
    return 0; // No delimiter found, return 0
}
bool TinyWebServerClient::match(const char *str, int len) {
    if(recv_buffer_len < len) {
        return false;
    }
    // Check if the next 'len' bytes match the given string
    for (int i = 0; i < len; ++i) {
        if (recv_buffer[(recv_buffer_read_ptr + i) % BUFFER_SIZE] != str[i]) {
            return false;
        }
    }
    // Consume the matched bytes
    recv_buffer_read_ptr = (recv_buffer_read_ptr + len) % BUFFER_SIZE;
    recv_buffer_len -= len;
    return true;
}

void TinyWebServerClient::send(int code, const char *content_type, const char *content) {
    write("HTTP/1.1 200 OK\r\n");
    if(content_type && content_type[0] != '\0') {
        write("Content-Type: ");
        write(content_type);
        write("\r\n");
    }
    write("Content-Length: ");
    if(content && content[0] != '\0') {
        //write(std::to_string(strlen(content)).c_str(), std::to_string(strlen(content)).length());
        write(content);
    } else {
        write("0");
    }
    write("\r\n\r\n");
    if(content && content[0] != '\0') {
        write(content);
    }
}

void TinyWebServerClient::finish() {
    done = true; // Mark the request as done
}

#ifndef LOCAL

const char* HTTP_RESPONSE = "HTTP/1.1 200 OK\r\n"
                        "Connection: close\r\n"
                        "Content-Type: text/html\r\n"
                        "\r\n";
extern const char index_html_header[];
extern const char index_html_footer[];
extern String processor(const String& var);
extern String cellmonitor_processor(const String& var);
extern void debug_logger_processor(String &content);

class MyString : public String {
    // subclass that prints everytime it is copied
public:
    MyString() : String() {}
    MyString(const String &other) : String(other) {
        tws_log_printf("MyString copied!\n");
    }
    MyString(MyString &&other) noexcept : String(std::move(other)) {
        tws_log_printf("MyString moved!\n");
    }
};

TwsRequestWriterCallbackFunction StringWriter(std::shared_ptr<MyString> &response) {
    return [response = std::move(response)](TwsRequest &req, int alreadyWritten) {
        const int remaining = response->length() - alreadyWritten;
        if(remaining <= 0) {
            tws_log_printf("TWS finished %d %d\n", alreadyWritten, response->length());
            req.finish(); // No more data to write, finish the request
            return;
        }
        req.write_direct(response->c_str() + alreadyWritten, remaining);
    };

    // return [response = std::move(response)](TwsRequest &req, int alreadyWritten) {
    //     const int remaining = response.length() - alreadyWritten;
    //     if(remaining <= 0) {
    //         req.finish(); // No more data to write, finish the request
    //         return;
    //     }
    //     req.write_direct(response->c_str() + alreadyWritten, remaining);
    // };
}

TwsRequestHandlerEntry default_handlers[] = {
    TwsRequestHandlerEntry("/", TWS_HTTP_GET, [](TwsRequest& request) {
        auto response = std::make_shared<MyString>();
        response->reserve(5000);
        response->concat(HTTP_RESPONSE);
        response->concat(index_html_header);
        response->concat(processor(String("X")));
        response->concat(index_html_footer);
        request.set_writer_callback(StringWriter(response));
    }),
    TwsRequestHandlerEntry("/cellmonitor", TWS_HTTP_GET, [](TwsRequest& request) {
        auto response = std::make_shared<MyString>();
        //String response = String();
        response->reserve(5000);
        response->concat(HTTP_RESPONSE);
        response->concat(index_html_header);
        response->concat(cellmonitor_processor(String("X")));
        response->concat(index_html_footer);
        request.set_writer_callback(StringWriter(response));
    }),
    TwsRequestHandlerEntry("/log", TWS_HTTP_GET, [](TwsRequest& request) {
        auto response = std::make_shared<MyString>();
        //auto response = std::make_unique<String>();
        response->reserve(17000);
        response->concat(HTTP_RESPONSE);
        //String response = String();
        
        debug_logger_processor(*response);
        if(response->length() < 20) {
            request.write("HTTP/1.1 500 Internal Server Error\r\n"
                          "Connection: close\r\n"
                          "Content-Type: text/plain\r\n"
                          "\r\nNo response?\n");
            request.finish();
        } else {
            request.set_writer_callback(StringWriter(response));
            // request.set_writer_callback([resp = std::move(response)](TwsRequest &req, int alreadyWritten) {
            //     const int remaining = resp->length() - alreadyWritten;
            //     if(remaining <= 0) {
            //         tws_log_printf("TWS finished %d %d\n", alreadyWritten, resp->length());
            //         req.finish(); // No more data to write, finish the request
            //         return;
            //     }
            //     req.write_direct(resp->c_str() + alreadyWritten, remaining);
            // });
        }
    }),
    TwsRequestHandlerEntry(nullptr, 0, nullptr) // Sentinel entry to mark the end of the array
};

TinyWebServer tinyWebServer(12345, default_handlers);

void tiny_web_server_loop(void * pData) {
    TinyWebServer * server = (TinyWebServer *)pData;

    /*
    server->on("/", TWS_HTTP_GET, [](TwsRequest& request) {
        String response = String();
        response.reserve(5000);
        response += HTTP_RESPONSE;
        response += index_html_header;
        response += processor(String("X"));
        response += index_html_footer;
        //if (WEBSERVER_AUTH_REQUIRED && !request->authenticate(http_username, http_password))
            //return request->requestAuthentication();
        //request->send(200, "text/html", index_html, processor);
        //request.write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/html\r\nContent-Length: 22\r\n\r\n<h3>Hello World!</h3>\n");
        //request.finish();
        request.set_writer_callback(StringWriter(response));
        // request.set_writer_callback([response](TwsRequest &req, int alreadyWritten) {
        //     const int remaining = response.length() - alreadyWritten;
        //     if(remaining <= 0) {
        //         req.finish(); // No more data to write, finish the request
        //         return;
        //     }
        //     req.write_direct(response.c_str() + alreadyWritten, response.length() - alreadyWritten);
        //     //tws_log_printf("TWS request writer callback: %d bytes already written\n", alreadyWritten);
        //     // if(alreadyWritten==0) {
        //     //     req.write(HTTP_RESPONSE, strlen(HTTP_RESPONSE));
        //     //     return;
        //     // } else if (alreadyWritten > strlen(HTTP_RESPONSE)) {
        //     //     alreadyWritten -= strlen(HTTP_RESPONSE);
        //     // }



        //     //     req.write(index_html_header, strlen(index_html_header));
        //     //     //alreadyWritten -= 

        //     // }

        //     // if(
        //     // index_html_header
        //     // if(alreadyWritten==0) {
        //     //     req.write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/html\r\n\r\n<h3>Hello World!</h3>\n");
        //     // } else if (alreadyWritten > 1000) {
        //     //     req.write("end!");
        //     //     req.finish();
        //     // } else {
        //     //     //req.write("Hello World!\n");
        //     //     while(req.free() > 27 && alreadyWritten < 1000) {
        //     //         alreadyWritten += req.write("<p>banana banana banana</p>", 27);
        //     //     }
        //     // }
        // });

    });*/

    server->openPort();


    while (true) {
        server->poll();
        server->tick();
        //vTaskDelay(pdMS_TO_TICKS(50)); // Adjust the delay as needed
    }
}
#endif