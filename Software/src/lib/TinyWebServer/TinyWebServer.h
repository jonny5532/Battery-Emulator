#include <functional>
#include <stdint.h>
#include <stdio.h>

const int TWS_AWAITING_METHOD = 0;
const int TWS_AWAITING_PATH = 1;
const int TWS_AWAITING_QUERY_STRING = 2;
const int TWS_AWAITING_VERSION = 3;
const int TWS_AWAITING_HEADER = 4;
const int TWS_AWAITING_BODY = 5;

typedef enum {
  TWS_HTTP_GET = 0x1,
  TWS_HTTP_POST = 0x2,
} TwsMethod;

class TwsRequest;
typedef std::function<void(TwsRequest &request, int alreadyWritten)> TwsRequestWriterCallbackFunction;
typedef std::function<void(TwsRequest &request)> TwsRequestHandlerFunction;
typedef std::function<void(TwsRequest &request, const char *filename, size_t index, uint8_t *data, size_t len, bool final)> TwsUploadHandlerFunction;
typedef std::function<void(TwsRequest &request, const char *param, bool final)> TwsQueryParamHandlerFunction;
typedef std::function<void(TwsRequest &request, const char *line)> TwsHeaderHandlerFunction;


class TwsRequest {
public:
    virtual void send(int code, const char *content_type = "", const char *content = "") = 0;
    virtual int write(char byte) = 0;
    virtual int write(const char *buf) = 0;
    virtual int write(const char *buf, int len) = 0;
    virtual int write_direct(const char *buf, int len) = 0;
    virtual void finish() = 0;
    virtual void set_writer_callback(TwsRequestWriterCallbackFunction callback) = 0;
    virtual int free() = 0;
    virtual bool alive() = 0;
    virtual int get_client_id() = 0;
    virtual int get_connection_id() = 0;
};

class TwsRequestHandlerEntry;


class TinyWebServerClient : public TwsRequest {
public:
    int socket = -1;
    int client_id = 0;
    int connection_id = 0;
    static const int SEND_BUFFER_SIZE = 256;
    static const int RECV_BUFFER_SIZE = 256;
    char send_buffer[SEND_BUFFER_SIZE];
    char recv_buffer[RECV_BUFFER_SIZE];
    int send_buffer_len = 0;
    int send_buffer_write_ptr = 0;
    int send_buffer_read_ptr = 0;
    int recv_buffer_len = 0;
    int recv_buffer_write_ptr = 0;
    int recv_buffer_read_ptr = 0;
    int recv_buffer_scan_ptr = 0;
    int recv_buffer_scan_len = 0;

    int total_written = 0;
    int body_read = 0;

    int parse_state = TWS_AWAITING_METHOD;
    int method = 0;
    TwsRequestHandlerEntry *handler = nullptr;
    bool pending_direct_write = false;
    bool done = false;

    TwsRequestWriterCallbackFunction writer_callback = nullptr;
    int writer_callback_written_offset = 0;

    //bool keep_alive = false;

    unsigned long last_activity = 0;
   

    void reset();
    void tick();
    int write(const char *buf);
    int write(const char *buf, int len);
    int write(char byte);
    int write_direct(const char *buf, int len);
    int16_t read();
    int read(char *buf, int len);
    int available();
    int free();
    bool recv_buffer_full();
    int read_flush(int len);
    int scan(char delim1);
    int scan(char delim1, char delim2);
    int scan(char delim1, char delim2, char delim3);
    bool match(const char *str);
    bool match(const char *str, int len);

    void send(int code, const char *content_type = "", const char *content = "") override;
    void finish() override;
    void set_writer_callback(TwsRequestWriterCallbackFunction callback) {
        writer_callback = callback;
        // record the current total written bytes to use as an offset
        writer_callback_written_offset = total_written;
    }
    bool alive() override {
        return socket >= 0;
    }
    int get_client_id() override {
        return client_id;
    }
    int get_connection_id() override {
        return connection_id;
    }
};






// typedef std::function<void(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final)>
//   ArUploadHandlerFunction;
// typedef std::function<void(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)> ArBodyHandlerFunction;

class TinyWebServer {
public:
    TinyWebServer(uint16_t port, TwsRequestHandlerEntry *handlers = nullptr);
    ~TinyWebServer();

    void on(
        const char *uri, 
        int method, 
        TwsRequestHandlerFunction onRequest
        // ArUploadHandlerFunction onUpload, ArBodyHandlerFunction onBody
    );

    void open_port();
    void tick();
    void poll();

    static const int MAX_CLIENTS = 8;

protected:
    uint16_t _port;
    int _listen_socket = -1;
    TwsRequestHandlerEntry *_handlers = nullptr;
    //std::vector<TwsRequestHandlerEntry> _handlers;

    void accept_new_connections();
    void close_old_connections();
    void handle_client(TinyWebServerClient &client);

    TinyWebServerClient clients[MAX_CLIENTS];
    int last_connection_id = 0;

    // void begin();
    // void handleClient();
    // void on(const char* path, void (*handler)());
    // void onNotFound(void (*handler)());
};

class TwsRequestHandlerEntry {
public:
    const char *path;
    int method;
    TwsRequestHandlerFunction onRequest;
    TwsUploadHandlerFunction onUpload = nullptr;
    TwsQueryParamHandlerFunction onQueryParam = nullptr;
    TwsHeaderHandlerFunction onHeader = nullptr;

    void set_upload_handler(TwsUploadHandlerFunction handler) {
        onUpload = handler;
    }

    TwsRequestHandlerEntry(
        const char *path, 
        int method, 
        TwsRequestHandlerFunction onRequest,
        TwsUploadHandlerFunction onUpload = nullptr,
        TwsQueryParamHandlerFunction onQueryParam = nullptr,
        TwsHeaderHandlerFunction onHeader = nullptr
    ) : path(path), method(method), onRequest(onRequest), 
        onUpload(onUpload), onQueryParam(onQueryParam), onHeader(onHeader) {}
};

template<class T> class TwsStatefulRequestHandler {
public:
    typedef struct {
        int connection_id;
        T state_data;
    } _client_slot;
    _client_slot state[TinyWebServer::MAX_CLIENTS];

    T& get_state(TwsRequest &request) {
        auto client_id = request.get_client_id();
        auto connection_id = request.get_connection_id();
        if(state[client_id].connection_id != connection_id) {
            // Reset state for this client
            state[client_id].connection_id = connection_id;
            state[client_id].state_data = T();
        }
        return state[client_id].state_data;
    }
};

typedef struct { 
    int content_length;
} TwsMultipartUploadState;

class TwsMultipartUploadHandler : public TwsRequestHandlerEntry, public TwsStatefulRequestHandler<TwsMultipartUploadState> {
public:

    TwsMultipartUploadHandler(
        const char *path, 
        TwsRequestHandlerFunction onRequest,
        TwsUploadHandlerFunction onUpload
        //TwsQueryParamHandlerFunction onQueryParam = nullptr,
        //TwsHeaderHandlerFunction onHeader = nullptr
    ) : TwsRequestHandlerEntry(path, TWS_HTTP_POST, onRequest, onUpload) {
        set_upload_handler([this](TwsRequest &request, const char *filename, size_t index, uint8_t *data, size_t len, bool final) {
            auto &state = get_state(request);
            state.content_length += 1;
            printf("woooh %d %d\n", index, state.content_length);
        });
    }
};

void tiny_web_server_loop(void * pData);
