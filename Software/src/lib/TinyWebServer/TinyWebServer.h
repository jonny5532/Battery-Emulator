#include <functional>
#include <stdint.h>

const int TWS_AWAITING_METHOD = 0;
const int TWS_AWAITING_PATH = 1;
const int TWS_AWAITING_VERSION = 2;
const int TWS_AWAITING_HEADER = 3;

typedef enum {
  TWS_HTTP_GET = 0x1,
  TWS_HTTP_POST = 0x2,
} TwsMethod;

class TwsRequest;
typedef std::function<void(TwsRequest &request, int alreadyWritten)> TwsRequestWriterCallbackFunction;
typedef std::function<void(TwsRequest &request)> TwsRequestHandlerFunction;

class TwsRequest {
public:
    virtual void send(int code, const char *content_type = "", const char *content = "") = 0;
    virtual int write(const char *buf) = 0;
    virtual int write(const char *buf, int len) = 0;
    virtual int write_direct(const char *buf, int len) = 0;
    virtual void finish() = 0;
    virtual void set_writer_callback(TwsRequestWriterCallbackFunction callback) = 0;
    virtual int free() = 0;
};

class TwsRequestHandlerEntry;


class TinyWebServerClient : public TwsRequest {
public:
    int socket = -1;
    static const int BUFFER_SIZE = 512;
    char send_buffer[BUFFER_SIZE];
    char recv_buffer[BUFFER_SIZE];
    int send_buffer_len = 0;
    int send_buffer_write_ptr = 0;
    int send_buffer_read_ptr = 0;
    int recv_buffer_len = 0;
    int recv_buffer_write_ptr = 0;
    int recv_buffer_read_ptr = 0;
    int recv_buffer_scan_ptr = 0;
    int recv_buffer_scan_len = 0;

    int total_written = 0;

    int parse_state = TWS_AWAITING_METHOD;
    int method = 0;
    TwsRequestHandlerEntry *handler = nullptr;
    bool pending_direct_write = false;
    bool done = false;

    TwsRequestWriterCallbackFunction writer_callback = nullptr;
    

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
    int scan(char delim1, char delim2);
    bool match(const char *str, int len);

    void send(int code, const char *content_type = "", const char *content = "") override;
    void finish() override;
    void set_writer_callback(TwsRequestWriterCallbackFunction callback) {
        writer_callback = callback;
    }
};



class TwsRequestHandlerEntry {
public:
    const char *uri;
    int method;
    TwsRequestHandlerFunction onRequest;

    TwsRequestHandlerEntry(const char *uri, int method, TwsRequestHandlerFunction onRequest)
        : uri(uri), method(method), onRequest(onRequest) {}
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

    void openPort();
    void tick();
    void poll();

protected:
    uint16_t _port;
    int _listen_socket = -1;
    TwsRequestHandlerEntry *_handlers = nullptr;
    //std::vector<TwsRequestHandlerEntry> _handlers;

    void acceptNewConnections();
    void closeOldConnections();
    void handleClient(TinyWebServerClient &client);

    static const int MAX_CLIENTS = 8;

    TinyWebServerClient clients[MAX_CLIENTS];

    // void begin();
    // void handleClient();
    // void on(const char* path, void (*handler)());
    // void onNotFound(void (*handler)());
};

void tiny_web_server_loop(void * pData);
