#include "http_msg_creator.h"
#include "nlohmann/json.hpp"
#include "utility/logger.h"
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

using json = nlohmann::json;

namespace beam {

namespace {

struct CurrentOutput {
    CurrentOutput(io::SerializedMsg& out, io::SerializedMsg** currentMsg)
        : _currentMsg(currentMsg)
    {
        out.clear();
        *_currentMsg = &out;
    }
    ~CurrentOutput() { *_currentMsg = 0; }

    io::SerializedMsg** _currentMsg;
};

bool write_fmt(io::FragmentWriter& fw, const char* fmt, ...) {
    static const int MAX_BUFSIZE = 4096;
    char buf[MAX_BUFSIZE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, MAX_BUFSIZE, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return false;
    }
    fw.write(buf, n);
    return true;
}

bool create_message(
    io::FragmentWriter& fw,
    const HeaderPair* headers, size_t num_headers,
    const char* content_type,
    size_t bodySize
) {
    for (size_t i=0; i<num_headers; ++i) {
        const HeaderPair& p = headers[i];
        assert(p.head);
        if (!p.is_number) {
            assert(p.content_str);
            if (!write_fmt(fw, "%s: %s\r\n", p.head, p.content_str)) return false;
        } else {
            if (!write_fmt(fw, "%s: %lu\r\n", p.head, p.content_num)) return false;
        }
    }

    if (bodySize > 0) {
        assert(content_type != nullptr);
        if (!write_fmt(fw, "%s: %s\r\n", "Content-Type", content_type)) return false;
        if (!write_fmt(fw, "%s: %lu\r\n", "Content-Length", (unsigned long)bodySize)) return false;
    }

    fw.write("\r\n", 2);
    fw.finalize();

    return true;
}

} //namespace

bool HttpMsgCreator::create_request(
    io::SerializedMsg& out,
    const char* method,
    const char* path,
    const HeaderPair* headers, size_t num_headers,
    int http_minor_version,
    const char* content_type,
    size_t bodySize
) {
    CurrentOutput co(out, &_currentMsg);

    assert(method != nullptr);
    assert(path != nullptr);
    if (!write_fmt(_fragmentWriter, "%s %s HTTP/1.%d\r\n", method, path, http_minor_version)) return false;

    return create_message(_fragmentWriter, headers, num_headers, content_type, bodySize);
}

bool HttpMsgCreator::create_response(
    io::SerializedMsg& out,
    int code,
    const char* message,
    const HeaderPair* headers, size_t num_headers,
    int http_minor_version,
    const char* content_type,
    size_t bodySize
) {
    CurrentOutput co(out, &_currentMsg);

    assert(message != nullptr);
    if (!write_fmt(_fragmentWriter, "HTTP/1.%d %d %s\r\n", http_minor_version, code, message)) return false;

    return create_message(_fragmentWriter, headers, num_headers, content_type, bodySize);
}

namespace {

    struct JsonOutputAdapter : nlohmann::detail::output_adapter_protocol<char> {
        JsonOutputAdapter(io::FragmentWriter& _fw) : fw(_fw) {}

        void write_character(char c) override {
            fw.write(&c, 1);
        }

        void write_characters(const char* s, std::size_t length) override {
            fw.write(s, length);
        }

        io::FragmentWriter& fw;
    };

} //namespace

bool append_json_msg(io::SerializedMsg& out, HttpMsgCreator& packer, const json& o) {
    bool result = true;
    size_t initialFragments = out.size();
    io::FragmentWriter& fw = packer.acquire_writer(out);
    try {
        // TODO make stateful object out of these fns if performance issues occur
        nlohmann::detail::serializer<json> s(std::make_shared<JsonOutputAdapter>(fw), ' ');
        s.dump(o, false, false, 0);
    } catch (const std::exception& e) {
        LOG_ERROR() << "dump json: " << e.what();
        result = false;
    }

    fw.finalize();
    if (!result) out.resize(initialFragments);
    packer.release_writer();
    return result;
}

} //namepsace
