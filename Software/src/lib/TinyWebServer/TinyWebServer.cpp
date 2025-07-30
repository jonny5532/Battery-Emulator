#include "TinyWebServer.h"

#include <algorithm>
#include <fcntl.h>
#include <memory>

#ifndef LOCAL
#include <src/devboard/utils/logging.h>
#include <src/communication/can/comm_can.h>
#include <src/lib/ayushsharma82-ElegantOTA/src/elop.h>

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
    #include <mcheck.h>


    #define tws_log_printf printf

    extern void *__libc_malloc(size_t size);
    extern void *__libc_free(void *ptr);

    char malloc_log[104857600] = {0};
    char* malloc_log_ptr = malloc_log;

    void* malloc (size_t size) {
        // void *caller = __builtin_return_address(0);
        // if (malloc_hook_active)
        //     return my_malloc_hook(size, caller);
        //puts("malloc called\n");
        //printf("malloc called with size %zu\n", size);
        // print to mallog_loc
        if (malloc_log_ptr + size >= malloc_log + sizeof(malloc_log)) {
            tws_log_printf("malloc log full, cannot allocate more memory\n");
            return nullptr;
        }
        // Store the allocation in the log
        void* ptr = __libc_malloc(size);
        malloc_log_ptr += sprintf(malloc_log_ptr, "malloc(%zu) at %p\n", size, ptr);
        return ptr;
    }

    void free(void *ptr) {
        // print to mallog_loc
        malloc_log_ptr += sprintf(malloc_log_ptr, "free(%p)\n", ptr);
        __libc_free(ptr);
    }

    unsigned long millis(void) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
    }

    class String {
    public:
        String() {
        }
        ~String() {
            if (_buf) {
                tws_log_printf("deallocating buf! %p\n", _buf);
                delete[] _buf;
            }
        }
        // copy constructor
        String(const String &other) {
            if (other._buf) {
                _buf = new char[BUFLEN];
                tws_log_printf("allocating buf! %p\n", _buf);
                strncpy(_buf, other._buf, BUFLEN);
            } else {
                _buf = nullptr;
            }
        }
        void concat(const char* str) {
            if (_buf == nullptr) {
                _buf = new char[BUFLEN];
                _buf[0] = '\0'; // Initialize as an empty string
            }
            strncat(_buf, str, BUFLEN - strlen(_buf) - 1);
        }
        int length() const {
            return _buf ? strlen(_buf) : 0;
        }
        void clear() {
            if (_buf) {
                _buf[0] = '\0'; // Clear the string
            }
        }
        const char* c_str() const {
            return _buf ? _buf : "";
        }
    private:
        static const int BUFLEN = 1048576;
        char* _buf = nullptr;
    };
    
    String strings[TinyWebServer::MAX_CLIENTS];

    TwsRequestWriterCallbackFunction StringWriter(std::shared_ptr<String> &response) {
        return [response](TwsRequest &req, int alreadyWritten) {
            const int remaining = response->length() - alreadyWritten;
            if(remaining <= 0) {
                tws_log_printf("TWS finished %d %d\n", alreadyWritten, response->length());
                req.finish(); // No more data to write, finish the request
                return;
            }
            int wrote = req.write_direct(response->c_str() + alreadyWritten, remaining);
            if(wrote==remaining) {
                req.finish();
            }
        };
    }

    TwsRequestWriterCallbackFunction IndexedStringWriter() {
        return [](TwsRequest &req, int alreadyWritten) {
            auto response = &strings[req.get_client_id()];
            const int remaining = response->length() - alreadyWritten;
            if(remaining <= 0) {
                tws_log_printf("TWS finished %d %d\n", alreadyWritten, response->length());
                req.finish(); // No more data to write, finish the request
                return;
            }
            int wrote = req.write_direct(response->c_str() + alreadyWritten, remaining);
            if(wrote==remaining) {
                req.finish();
            }
        };
    }

    auto MultipartPostHandler(auto onUpload) {
        return [onUpload](TwsRequest& request, const char *filename, size_t index, uint8_t *data, size_t len, bool final) {
            //tws_log_printf("Received upload: %s, index: %zu, len: %zu, final: %d\n", filename, index, len, final);
            if(index==0) {
                // This is the first chunk, we can initialize or reset any state if needed
                tws_log_printf("Starting upload: %s\n", data);
                char* start = reinterpret_cast<char*>(memmem(data, len, "\r\n\r\n", 4));
                if(!start) {
                    tws_log_printf("Invalid multipart data, no headers found.\n");
                    request.finish();
                    return;
                }
                len -= (start + 4) - (char*)data;
                data = reinterpret_cast<uint8_t*>(start + 4);
            }
            if (onUpload) {
                onUpload(request, filename, index, data, len, final);
            }
            // if (final) {
            //     request.write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\nFile uploaded successfully.\n");
            // } else {
            //     request.write("HTTP/1.1 100 Continue\r\n\r\n");
            // }
            // request.finish();
        };
    }
    // TwsRequestWriterCallbackFunction StringWriter(String response) {
    //     return [response = std::move(&response)](TwsRequest &req, int alreadyWritten) {
    //         const int remaining = response->length() - alreadyWritten;
    //         if(remaining <= 0) {
    //             tws_log_printf("TWS finished %d %d\n", alreadyWritten, response->length());
    //             req.finish(); // No more data to write, finish the request
    //             return;
    //         }
    //         req.write_direct(response->c_str() + alreadyWritten, remaining);
    //     };
    // }
    int last_authed_connection[TinyWebServer::MAX_CLIENTS] = {0};

    TwsRequestHandlerEntry default_handlers[] = {
        TwsRequestHandlerEntry("/", TWS_HTTP_GET, [](TwsRequest& request) {
            tws_log_printf("creating response\n");
            auto response = std::make_shared<String>();
            //auto response = String();
            //response->reserve(5000);
            response->concat("HTTP/1.1 200 OK\r\n"
                        "Connection: close\r\n"
                        "Content-Type: text/plain\r\n"
                        "\r\n"
                        "Yes this is a jolly good test.\n");
            response->concat(malloc_log);
            request.set_writer_callback(StringWriter(response));
        }),
        TwsRequestHandlerEntry("/yah", TWS_HTTP_GET, [](TwsRequest& request) {
            int client_id = request.get_client_id();
            strings[client_id].clear();
            auto response = &strings[client_id];
            response->concat("HTTP/1.1 200 OK\r\n"
                        "Connection: close\r\n"
                        "Content-Type: text/plain\r\n"
                        "\r\n"
                        "Yes this is a jolly good test.\n");
            response->concat(malloc_log);
            request.set_writer_callback(IndexedStringWriter());
        }),
        TwsRequestHandlerEntry("/moo", TWS_HTTP_GET, [](TwsRequest& request) {
            request.write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/html\r\n\r\n<form method=\"post\" enctype=\"multipart/form-data\">\n"
                        "<input type=\"file\" name=\"file\" />\n"
                        "<input type=\"submit\" value=\"Upload\" />\n"
                        "</form>\n");
            request.finish();
        }),
        TwsRequestHandlerEntry("/upload", TWS_HTTP_GET, [](TwsRequest& request) {
            request.write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/html\r\n\r\n<input type=\"file\" onchange=\""
                "const c = new FormData;"
                "let l = this.files[0];"
                "let i = new XMLHttpRequest;"
                "i.open('POST', '/ota/upload');"
                "c.append('file', l, l.name);"
                "i.send(c);"
                "\" />\n");

            request.finish();
        }),
        TwsMultipartUploadHandler("/ota/upload", [](TwsRequest& request) {}, [](TwsRequest& request, const char *filename, size_t index, uint8_t *data, size_t len, bool final) {
            tws_log_printf("Received upload: %s, index: %zu, len: %zu, final: %d\n", filename, index, len, final);
        }),
        TwsRequestHandlerEntry("OLD/ota/upload", TWS_HTTP_POST, [](TwsRequest& request) {}, MultipartPostHandler([](TwsRequest& request, const char *filename, size_t index, uint8_t *data, size_t len, bool final) {
            tws_log_printf("Received upload: %s, index: %zu, len: %zu, final: %d\n", filename, index, len, final);
            // if (final) {
            //     request.write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\nFile uploaded successfully.\n");
            // } else {
            //     request.write("HTTP/1.1 100 Continue\r\n\r\n");
            // }
            // request.finish();
        })),
        TwsRequestHandlerEntry("/auth", TWS_HTTP_GET, [](TwsRequest& request) {
            if(last_authed_connection[request.get_client_id()] == request.get_connection_id()) {
                request.write("HTTP/1.1 200 OK\r\n"
                            "Connection: close\r\n"
                            "Content-Type: text/plain\r\n"
                            "\r\n"
                            "You are authenticated!\n");
            } else {
                request.write("HTTP/1.1 401 Unauthorized\r\n"
                            "Connection: close\r\n"
                            "Content-Type: text/plain\r\n"
                            "WWW-Authenticate: Basic realm=\"TinyWebServer\"\r\n"
                            "\r\n"
                            "This is a protected resource.\n");
            }

            request.finish();
        }, nullptr, nullptr, [](TwsRequest &request, const char *line) {
            // Handle headers
            if(strcmp(line, "Authorization: Basic YXV0aDphdXRo")==0) {
                tws_log_printf("Successful auth!\n");
                last_authed_connection[request.get_client_id()] = request.get_connection_id();
            }
            //tws_log_printf("Received header: %s\n", line);
        }),
        TwsRequestHandlerEntry(nullptr, 0, nullptr) // Sentinel entry to mark the end of the array
    };

    int main() {
        mtrace();

        TinyWebServer tws(12345, default_handlers);
        tws.open_port();

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
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].client_id = i;
    }
}

TinyWebServer::~TinyWebServer() {
    if (_listen_socket >= 0) {
        close(_listen_socket);
        _listen_socket = -1;
    }
}

void TinyWebServer::open_port() {
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
    //accept_new_connections();

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].socket >= 0) {
            clients[i].tick();
            handle_client(clients[i]);
        }
    }
}

void TinyWebServer::handle_client(TinyWebServerClient &client) {
    auto trim = [](char *str, int len) {
        // Trim leading and trailing whitespace, returning a pointer into the original string.
        // The last character is always removed.
        len--;
        while(len > 0 && (str[len-1]==' ' || str[len-1]=='\r' || str[len-1]=='\n')) {
            len--;
        }
        while(len > 0 && str[0]==' ') {
            str++;
            len--;
        }
        str[len] = '\0'; // Null-terminate the trimmed string
        return str;
    };

    while(true) {
        int len = 0;

        // Scan up to the next delimiter based on the current parse state
        switch(client.parse_state) {
            case TWS_AWAITING_PATH:
                len = client.scan(' ', '?', '\n');
                break;
            case TWS_AWAITING_QUERY_STRING:
                len = client.scan('&', ' ', '\n');
                break;
            case TWS_AWAITING_HEADER:
                len = client.scan('\n');
                if(len==0 && client.recv_buffer_full()) {
                    // We can't fit the whole header into the buffer, so junk it
                    client.read_flush(client.available());
                }
                break;
            case TWS_AWAITING_BODY:
                len = client.available();
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

        switch(client.parse_state) {
        case TWS_AWAITING_METHOD:
            if(client.match("POST ")) {
                client.method = TWS_HTTP_POST;
                client.parse_state = TWS_AWAITING_PATH;
            } else if(client.match("GET ")) {
                client.method = TWS_HTTP_GET;
                client.parse_state = TWS_AWAITING_PATH;
            } else {
                tws_log_printf("TWS client invalid method! %d %d [%s]\n", client.recv_buffer_read_ptr, client.recv_buffer_scan_ptr, client.recv_buffer);
                client.reset();
            }
            break;
        case TWS_AWAITING_PATH:
            {
                bool found = false;
                for(int j=0;_handlers[j].path!=nullptr;j++) {
                    int path_len = strlen(_handlers[j].path);
                    if(len == (path_len + 1) && client.match(_handlers[j].path, path_len)) {
                        client.handler = &_handlers[j];
                        found = true;
                        break;
                    }
                }
                if(!found) {
                    client.read_flush(len-1);
                }
                // What's the delimiter?
                if(client.read() == '?') {
                    client.parse_state = TWS_AWAITING_QUERY_STRING;
                } else {
                    client.parse_state = TWS_AWAITING_VERSION;
                }
                break;
            }
        case TWS_AWAITING_QUERY_STRING:
            {
                char qbuf[len+1];
                client.read(qbuf, len);

                if(len>0 && client.handler && client.handler->onQueryParam) {
                    // Call the query param handler function
                    bool final = (qbuf[len-1] == '&' || qbuf[len-1] == '\n');
                    client.handler->onQueryParam(client, trim(qbuf, len), final);
                } else {
                    tws_log_printf("TWS query param is %s\n", trim(qbuf, len));
                }

                if(qbuf[len-1] == '&') {
                    client.parse_state = TWS_AWAITING_QUERY_STRING;
                } else if(qbuf[len-1] == '\n') {
                    client.parse_state = TWS_AWAITING_HEADER;
                } else {
                    client.parse_state = TWS_AWAITING_VERSION;
                }
                break;
            }
        case TWS_AWAITING_VERSION:
            if(client.match("HTTP/1.1\r\n") || client.match("HTTP/1.1\n") ||
                client.match("HTTP/1.0\r\n") || client.match("HTTP/1.0\n")) {
                client.parse_state = TWS_AWAITING_HEADER;
            } else {
                char vbuf[len+1];
                client.read(vbuf, len);
                tws_log_printf("TWS client invalid HTTP version! %s\n", trim(vbuf, len));
                client.reset();
            }
            break;
        case TWS_AWAITING_HEADER:
            if(client.match("\r\n") || client.match("\n")) {
                // End of headers, process the request

                if(client.handler) {
                    if(client.method==TWS_HTTP_POST) {
                        // check Content-Length to decide whether to do this?
                        client.parse_state = TWS_AWAITING_BODY;
                    } else {
                        // Call the handler function
                        client.handler->onRequest(client);
                        if(client.writer_callback && client.free()>0) client.writer_callback(client, client.total_written - client.writer_callback_written_offset);
                    }
                } else {
                    client.write("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\n404 Not Found\n");
                    client.finish();
                }
            } else {
                char hbuf[len+1];
                client.read(hbuf, len);

                if(client.handler && client.handler->onHeader) {
                    // Call the header handler function
                    client.handler->onHeader(client, trim(hbuf, len));
                } else {
                    tws_log_printf("TWS client header: %s\n", trim(hbuf, len));
                }
            }
            break;
        case TWS_AWAITING_BODY:
            {
                if(client.body_read==0 && !client.recv_buffer_full()) {
                    // Wait until we have a full buffer for the first chunk.
                    // Gives the best chance of the first chunk containing all
                    // the multipart section headers (at least 134 bytes plus
                    // the field/file names)
                    // FIXME: what if the total body is smaller than the buffer?
                    return;
                }
                char body_buf[len+1];
                client.read(body_buf, len);
                body_buf[len] = '\0'; // Null-terminate the body buffer
                
                if(client.handler && client.handler->onUpload) {
                    // Call the upload handler function
                    bool final = false;
                    client.handler->onUpload(client, "unknown", client.body_read, (uint8_t*)body_buf, len, final);
                } else {
                    tws_log_printf("TWS client body: [%s]\n", body_buf);
                }
                client.body_read += len;
                break;
            }
        }
    }
}

void TinyWebServer::accept_new_connections() {
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
    clients[client_index].connection_id = ++last_connection_id;
    clients[client_index].socket = socket;
    clients[client_index].last_activity = millis();

    //clients[client_index].write((const uint8_t*)"Hello woorld!\n", 14);
}

void TinyWebServer::close_old_connections() {
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
            //tws_log_printf("FD_SET read %d\n", client.connection_id);
            FD_SET(client.socket, &read_sockets);
            max_fds = std::max(max_fds, client.socket + 1);
            if (client.send_buffer_len > 0 || client.pending_direct_write) {
                //tws_log_printf("FD_SET write %d\n", client.connection_id);
                FD_SET(client.socket, &write_sockets);
            }
        }
    }

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 5000000;

    int activity = select(max_fds, &read_sockets, &write_sockets, NULL, &timeout);
    if(activity>0) {
        //tws_log_printf("TWS select activity: %d\n", activity);
        // Check if the socket has closed
        if (FD_ISSET(_listen_socket, &read_sockets)) {
            //tws_log_printf("TWS new connection available\n");
            // New connection available
            accept_new_connections();
        }
        // Check each client socket for activity
        //for (int i = 0; i < MAX_CLIENTS; i++) {
        for (auto &client : clients) {
            if (client.socket >= 0 && FD_ISSET(client.socket, &read_sockets)) {
                //tws_log_printf("Got FD_SET read %d\n", client.connection_id);
                //|| FD_ISSET(clients[i].socket, &write_sockets)) {
                // if(FD_ISSET(clients[i].socket, &write_sockets) && clients[i].pending_direct_write) {
                //     // There's now space so our direct write must have finished
                //     clients[i].pending_direct_write = false;
                // }

                // Client socket has activity, process it
                client.tick();
            } else if(client.socket >= 0 && (client.send_buffer_len > 0 || client.pending_direct_write) && FD_ISSET(client.socket, &write_sockets)) {
                //tws_log_printf("Got FD_SET write %d\n", client.connection_id);
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
    body_read = 0;
    pending_direct_write = false;
    writer_callback = nullptr;
    writer_callback_written_offset = 0;
    handler = nullptr;
    connection_id = 0;
}

void TinyWebServerClient::tick() {
    unsigned long now = millis();

    // uint8_t buf[100];
    // int bytes_read = read(buf, sizeof(buf));
    // if(bytes_read>0) {
    //     write(buf, bytes_read); // Echo back the received data
    //     logging.printf("TWS %d %d %d %d\n", bytes_read, send_buffer_len, send_buffer_read_ptr, send_buffer_write_ptr);
    // }

    // send any data awaiting sending
    while(send_buffer_len>0) {
        const int contiguous_block_size = sizeof(send_buffer) - send_buffer_read_ptr;
        const int bytes_to_send = std::min(send_buffer_len, contiguous_block_size);
        const int bytes_sent = ::send(socket, &send_buffer[send_buffer_read_ptr], bytes_to_send, 0);
        //tws_log_printf("  ::send returned %d\n", bytes_sent);
        if (bytes_sent > 0) {
            // Advance the read pointer, wrapping around the buffer if necessary.
            send_buffer_read_ptr = (send_buffer_read_ptr + bytes_sent) % sizeof(send_buffer);
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
    while(recv_buffer_len < sizeof(recv_buffer)) {
        auto contiguous_block_size = sizeof(recv_buffer) - recv_buffer_write_ptr;
        const int bytes_to_read = std::min(sizeof(recv_buffer) - recv_buffer_len, contiguous_block_size);
        int bytes_received = ::recv(socket, &recv_buffer[recv_buffer_write_ptr], bytes_to_read, 0);
        //printf("  ::recv returned %d\n", bytes_received);
        if (bytes_received > 0) {
            // Advance the write pointer, wrapping around the buffer if necessary.
            recv_buffer_write_ptr = (recv_buffer_write_ptr + bytes_received) % sizeof(recv_buffer);
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
        tws_log_printf("TWS done, closing\n");
        reset();
        return;
    }

    if(send_buffer_len==0 && !done && writer_callback) {
        // If the send buffer is empty and a callback is set, call it
        writer_callback(*this, this->total_written);
    }

    unsigned long elapsed = now - last_activity;
    if(elapsed > 10000 && elapsed < 0x80000000) {
        // No activity for 10 seconds, close the connection
        // (hacky workaround for stale values in millis)
        tws_log_printf("TWS client timeout, closing connection %lu %lu\n", now, last_activity);
        reset();
        return;
    }
}

int TinyWebServerClient::write(const char *buf) {
    if (send_buffer_len >= sizeof(send_buffer)) {
        return 0; // Buffer full
    }
    int i;
    for(i = 0; buf[i] != '\0' && send_buffer_len < sizeof(send_buffer); ++i) {
        // Place the byte at the current write position.
        send_buffer[send_buffer_write_ptr] = buf[i];
        send_buffer_write_ptr = (send_buffer_write_ptr + 1) % sizeof(send_buffer);
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
        //tws_log_printf("TWS write direct %d bytes\n", len);
        bytes_sent = ::send(socket, buf, len, 0);
        //tws_log_printf("  ::send direct returned %d\n", bytes_sent);
        if(bytes_sent>0) {
            len -= bytes_sent;
            buf += bytes_sent;
            total_written += bytes_sent;
            last_activity = millis();
            pending_direct_write = true;
        } else {
            bytes_sent = 0;
        }
        if(len==0) {
            return bytes_sent;
        }
    }
    
    const int available_space = sizeof(send_buffer) - send_buffer_len;
    if (available_space <= 0) {
        return 0;
    }
    const int bytes_to_copy = std::min(len, available_space);

    for (int i = 0; i < bytes_to_copy; ++i) {
        // Place the byte at the current write position.
        send_buffer[send_buffer_write_ptr] = buf[i];
        send_buffer_write_ptr = (send_buffer_write_ptr + 1) % sizeof(send_buffer);
    }

    // Increase the stored data length by the number of bytes copied.
    send_buffer_len += bytes_to_copy;
    total_written += bytes_to_copy; // Update total written bytes
    return bytes_sent + bytes_to_copy;
}

int TinyWebServerClient::write(const char byte) {
    if (send_buffer_len >= sizeof(send_buffer)) {
        return 0; // Buffer full
    }
    send_buffer[send_buffer_write_ptr] = byte;
    send_buffer_write_ptr = (send_buffer_write_ptr + 1) % sizeof(send_buffer);
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
    //tws_log_printf("  ::send write_direct returned %d\n", bytes_written);
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
    recv_buffer_read_ptr = (recv_buffer_read_ptr + 1) % sizeof(recv_buffer);
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
        recv_buffer_read_ptr = (recv_buffer_read_ptr + 1) % sizeof(recv_buffer);
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
    return sizeof(send_buffer) - send_buffer_len; // Return the number of free bytes in the send buffer
}
bool TinyWebServerClient::recv_buffer_full() {
    return recv_buffer_len >= sizeof(recv_buffer); // Return true if the receive buffer is full
}
int TinyWebServerClient::read_flush(int len) {
    const int bytes_to_flush = std::min(len, recv_buffer_len);
    recv_buffer_read_ptr = (recv_buffer_read_ptr + bytes_to_flush) % sizeof(recv_buffer);
    recv_buffer_len -= bytes_to_flush;
    
    const int scan_bytes_to_flush = std::min(len, recv_buffer_scan_len);
    recv_buffer_scan_ptr = (recv_buffer_scan_ptr + scan_bytes_to_flush) % sizeof(recv_buffer);
    recv_buffer_scan_len -= scan_bytes_to_flush;

    return bytes_to_flush;
}

int TinyWebServerClient::scan(char delim1) {
    // Keep looping until we catch up with the write pointer
    while(recv_buffer_scan_len<recv_buffer_len) {
        if (recv_buffer[recv_buffer_scan_ptr] == delim1) {
            // Skip over the delimiter
            recv_buffer_scan_ptr = (recv_buffer_scan_ptr + 1) % sizeof(recv_buffer);
            int count = recv_buffer_scan_len+1;
            recv_buffer_scan_len = 0;
            // Return the length including the delimiter
            return count;
        }
        recv_buffer_scan_ptr = (recv_buffer_scan_ptr + 1) % sizeof(recv_buffer);
        recv_buffer_scan_len++;
    }
    return 0; // No delimiter found, return 0
}
int TinyWebServerClient::scan(char delim1, char delim2) {
    // Keep looping until we catch up with the write pointer
    while(recv_buffer_scan_len<recv_buffer_len) {//recv_buffer_scan_ptr != recv_buffer_write_ptr) {
        if (recv_buffer[recv_buffer_scan_ptr] == delim1 || recv_buffer[recv_buffer_scan_ptr] == delim2) {
            // Skip over the delimiter
            recv_buffer_scan_ptr = (recv_buffer_scan_ptr + 1) % sizeof(recv_buffer);
            int count = recv_buffer_scan_len+1;
            recv_buffer_scan_len = 0;
            // Return the length including the delimiter
            return count;
        }
        recv_buffer_scan_ptr = (recv_buffer_scan_ptr + 1) % sizeof(recv_buffer);
        recv_buffer_scan_len++;
    }
    return 0; // No delimiter found, return 0
}
int TinyWebServerClient::scan(char delim1, char delim2, char delim3) {
    // Keep looping until we catch up with the write pointer
    while(recv_buffer_scan_len<recv_buffer_len) {//recv_buffer_scan_ptr != recv_buffer_write_ptr) {
        if (recv_buffer[recv_buffer_scan_ptr] == delim1 || recv_buffer[recv_buffer_scan_ptr] == delim2 || recv_buffer[recv_buffer_scan_ptr] == delim3) {
            // Skip over the delimiter
            recv_buffer_scan_ptr = (recv_buffer_scan_ptr + 1) % sizeof(recv_buffer);
            int count = recv_buffer_scan_len+1;
            recv_buffer_scan_len = 0;
            // Return the length including the delimiter
            return count;
        }
        recv_buffer_scan_ptr = (recv_buffer_scan_ptr + 1) % sizeof(recv_buffer);
        recv_buffer_scan_len++;
    }
    return 0; // No delimiter found, return 0
}
bool TinyWebServerClient::match(const char *str) {
    int i;
    for(i = 0; str[i] != '\0'; ++i) {
        if (recv_buffer_len <= i || recv_buffer[(recv_buffer_read_ptr + i) % sizeof(recv_buffer)] != str[i]) {
            return false; // Not a match
        }
    }
    // Consume the matched bytes
    recv_buffer_read_ptr = (recv_buffer_read_ptr + i) % sizeof(recv_buffer);
    recv_buffer_len -= i;
    return true; // Match found
}
bool TinyWebServerClient::match(const char *str, int len) {
    if(recv_buffer_len < len) {
        return false;
    }
    // Check if the next 'len' bytes match the given string
    for (int i = 0; i < len; ++i) {
        if (recv_buffer[(recv_buffer_read_ptr + i) % sizeof(recv_buffer)] != str[i]) {
            return false;
        }
    }
    // Consume the matched bytes
    recv_buffer_read_ptr = (recv_buffer_read_ptr + len) % sizeof(recv_buffer);
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
extern String get_firmware_info_processor(const String& var);
extern void debug_logger_processor(String &content);

// class MyString : public String {
//     // subclass that prints everytime it is copied
// public:
//     MyString() : String() {}
//     MyString(const String &other) : String(other) {
//         tws_log_printf("MyString copied!\n");
//     }
//     MyString(MyString &&other) noexcept : String(std::move(other)) {
//         tws_log_printf("MyString moved!\n");
//     }
// };
typedef String MyString;

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

TwsRequestWriterCallbackFunction CharBufWriter(const char* buf, int len) {
    return [buf, len](TwsRequest &req, int alreadyWritten) {
        const int remaining = len - alreadyWritten;
        if(remaining <= 0) {
            tws_log_printf("TWS finished %d %d\n", alreadyWritten, len);
            req.finish(); // No more data to write, finish the request
            return;
        }
        req.write_direct(buf + alreadyWritten, remaining);
    };
}

TwsRequest *can_dumper = nullptr;
int can_dumper_connection_id = 0;

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
    TwsRequestHandlerEntry("/dump_can", TWS_HTTP_GET, [](TwsRequest& request) {
        request.write("HTTP/1.1 200 OK\r\n"
                      "Connection: close\r\n"
                      "Content-Type: text/plain\r\n"
                      "\r\nCAN log follows:\n\n");
        if(can_dumper) {
            can_dumper->write("CAN log disconnected.\n");
            can_dumper->finish();
        }
        can_dumper = &request;
        can_dumper_connection_id = request.get_connection_id();
        datalayer.system.info.can_logging_active2 = true;
    }),
    TwsRequestHandlerEntry("/update", TWS_HTTP_GET, [](TwsRequest& request) {
        const char* HEADER = "HTTP/1.1 200 OK\r\n"
                      "Connection: close\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-Encoding: gzip\r\n"
                      "\r\n";
        request.write(HEADER, strlen(HEADER));
        request.set_writer_callback(CharBufWriter((const char*)ELEGANT_HTML, sizeof(ELEGANT_HTML)));
    }),
    TwsRequestHandlerEntry("/GetFirmwareInfo", TWS_HTTP_GET, [](TwsRequest& request) {
        const char* HEADER = "HTTP/1.1 200 OK\r\n"
                      "Connection: close\r\n"
                      "Content-Type: application/json\r\n"
                      "\r\n";
        request.write(HEADER, strlen(HEADER));
        auto response = std::make_shared<MyString>(get_firmware_info_processor("X"));
        request.set_writer_callback(StringWriter(response));
    }),
    TwsRequestHandlerEntry(nullptr, 0, nullptr) // Sentinel entry to mark the end of the array
};

const char *hex = "0123456789abcdef";

void dump_can_frame2(CAN_frame& frame, frameDirection msgDir) {
    if(!can_dumper) {
        return;
    }
    if(can_dumper->get_connection_id() != can_dumper_connection_id) {
        can_dumper = nullptr;
        return;
    }
    if(can_dumper->free()<20) {
        can_dumper->write('\n');
        return;
    }

    char line[60];
    char *ptr = line;

    unsigned long currentTime = millis();
    *ptr++ = '(';
    //can_dumper->write('(');
    if(currentTime >= 1000000) {
        *ptr++ = (currentTime / 1000000) + '0';
        //can_dumper->write((currentTime / 100000) + '0');
        currentTime = currentTime % 1000000;
    }
    if(currentTime >= 100000) {
        *ptr++ = (currentTime / 100000) + '0';
        //can_dumper->write((currentTime / 100000) + '0');
        currentTime = currentTime % 100000;
    }
    if(currentTime >= 10000) {
        *ptr++ = (currentTime / 10000) + '0';
        //can_dumper->write((currentTime / 10000) + '0');
        currentTime = currentTime % 10000;
    }
    *ptr++ = (currentTime / 1000) + '0';
    //can_dumper->write((currentTime / 1000) + '0');
    currentTime = currentTime % 1000;
    *ptr++ = '.';
    //can_dumper->write('.');
    *ptr++ = (currentTime / 100) + '0';
    //can_dumper->write((currentTime / 100) + '0');
    currentTime = currentTime % 100;
    *ptr++ = (currentTime / 10) + '0';
    //can_dumper->write((currentTime / 10) + '0');
    currentTime = currentTime % 10;
    *ptr++ = (currentTime) + '0';
    //can_dumper->write((currentTime) + '0');
    *ptr++ = ')';
    //can_dumper->write(')');
    *ptr++ = ' ';
    //can_dumper->write(' ');
    if(msgDir == MSG_RX) {
        *ptr++ = 'R';
        *ptr++ = 'X';
        *ptr++ = '0';
    } else {
        *ptr++ = 'T';
        *ptr++ = 'X';
        *ptr++ = '1';
    }
    *ptr++ = ' ';
    //can_dumper->write(msgDir == MSG_RX ? "RX0 " : "TX1 ");

    if(frame.ext_ID) {
        *ptr++ = hex[(frame.ID&0xf0000000) >> 28];
        *ptr++ = hex[(frame.ID&0xf000000) >> 24];
        *ptr++ = hex[(frame.ID&0xf00000) >> 20];
        *ptr++ = hex[(frame.ID&0xf0000) >> 16];
        *ptr++ = hex[(frame.ID&0xf000) >> 12];
    }
    *ptr++ = hex[(frame.ID&0xf00) >> 8];
    //can_dumper->write(hex[(int)((frame.ID&0xf00) >> 8)]);
    *ptr++ = hex[(frame.ID&0x0f0) >> 4];
    //can_dumper->write(hex[(int)((frame.ID&0x0f0) >> 4)]);
    *ptr++ = hex[(frame.ID&0x00f) >> 0];
    //can_dumper->write(hex[(int)((frame.ID&0x00f) >> 0)]);
    *ptr++ = ' ';
    //can_dumper->write(' ');
    *ptr++ = '[';
    //can_dumper->write('[');
    *ptr++ = '0' + (frame.DLC);
    //can_dumper->write('0' + frame.DLC);
    *ptr++ = ']';
    //can_dumper->write(']');
    *ptr++ = ' ';
    //can_dumper->write(' ');
    *ptr++ = hex[(int)((frame.data.u8[0]&0xf0) >> 4)];
    //can_dumper->write(hex[(int)((frame.data.u8[0]&0xf0) >> 4)]);
    *ptr++ = hex[(int)((frame.data.u8[0]&0x0f) >> 0)];
    //can_dumper->write(hex[(int)((frame.data.u8[0]&0x0f) >> 0)]);

    for(int i=1;i<frame.DLC;i++) {
        *ptr++ = ' ';
        //can_dumper->write(' ');
        *ptr++ = hex[(int)((frame.data.u8[i]&0xf0) >> 4)];
        //can_dumper->write(hex[(int)((frame.data.u8[i]&0xf0) >> 4)]);
        *ptr++ = hex[(int)((frame.data.u8[i]&0x0f) >> 0)];
        //can_dumper->write(hex[(int)((frame.data.u8[i]&0x0f) >> 0)]);
    }
    *ptr++ = '\n';
    can_dumper->write(line, ptr - line);
}



TinyWebServer tinyWebServer(12345, default_handlers);

void tiny_web_server_loop(void * pData) {
    TinyWebServer * server = (TinyWebServer *)pData;

    server->open_port();

    while (true) {
        server->poll();
        // Tick incase our select missed anything
        server->tick();
        //vTaskDelay(pdMS_TO_TICKS(50)); // Adjust the delay as needed
    }
}
#endif