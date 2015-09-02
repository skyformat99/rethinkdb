// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "protob/json_shim.hpp"

#include "arch/io/network.hpp"
#include "rapidjson/rapidjson.h"
#include "rdb_protocol/backtrace.hpp"
#include "rdb_protocol/query.hpp"
#include "rdb_protocol/response.hpp"

#include "debug.hpp"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "rdb_protocol/ql2.pb.h"
#include "utils.hpp"
#include "arch/runtime/coroutines.hpp"

// The minimum amount of stack space we require to be available on a coroutine
// before attempting to extract a value.
const size_t MIN_EXTRACT_STACK_SPACE = 16 * KILOBYTE;
const uint32_t wire_protocol_t::TOO_LARGE_QUERY_SIZE = 64 * MEGABYTE;
const uint32_t wire_protocol_t::TOO_LARGE_RESPONSE_SIZE =
    std::numeric_limits<uint32_t>::max();

const std::string wire_protocol_t::unparseable_query_message =
    "Client is buggy (failed to deserialize query).";

std::string wire_protocol_t::too_large_query_message(uint32_t size) {
    return strprintf("Query size (%" PRIu32 ") greater than maximum (%" PRIu32 ").",
                     size, TOO_LARGE_QUERY_SIZE - 1);

std::string wire_protocol_t::too_large_response_message(size_t size) {
    return strprintf("Response size (%zu) greater than maximum (%" PRIu32 ").",
                     size, TOO_LARGE_RESPONSE_SIZE - 1);
}

namespace json_shim {

class exc_t : public std::exception {
public:
    exc_t() { }
    ~exc_t() throw () { }
    const char *what() const throw () { return "json_shim::exc_t"; }
};

// The first value is != nullptr if this is a key/value pair from an object.
// In that case it is the key and the second value the value.
template<class T>
typename std::enable_if<!((std::is_enum<T>::value || std::is_fundamental<T>::value)
                          && !std::is_same<T, bool>::value)>::type
extract(const Value *, const Value &, T *);

template<class T>
typename std::enable_if<(std::is_enum<T>::value || std::is_fundamental<T>::value)
                        && !std::is_same<T, bool>::value>::type
extract(const Value *, const Value &field, T *dest) {
    if (!field.IsNumber()) {
        throw exc_t();
    }
    T t = static_cast<T>(field.GetDouble());
    if (static_cast<double>(t) != field.GetDouble()) {
        throw exc_t();
    }
    *dest = t;
}

template<class T>
void safe_extract(const Value *key, const Value &val, T *t) {
    if (t != nullptr) {
        call_with_enough_stack([&] () {
                extract<T>(key, val, t);
            }, MIN_EXTRACT_STACK_SPACE);
    }
}

template<class T, class U>
void transfer(const Value &json, T *dest, void (T::*setter)(U)) {
    U tmp;
    safe_extract(nullptr, json, &tmp);
    (dest->*setter)(std::move(tmp));
}

scoped_ptr_t<ql::query_params_t> json_protocol_t::parse_query_from_buffer(
        scoped_array_t<char> &&buffer, size_t offset,
        ql::query_cache_t *query_cache, int64_t token) {
    rapidjson::Document doc;
    doc.ParseInsitu(buffer.data() + offset);

    scoped_ptr_t<ql::query_params_t> res;
    if (!doc.HasParseError()) {
        try {
            res = make_scoped<ql::query_params_t>(token, query_cache,
                                                  std::move(buffer), std::move(doc));
        } catch (const ql::bt_exc_t &ex) {
            // Parse error, let the caller handle the response
        }
    }
    return res;
}

scoped_ptr_t<ql::query_params_t> json_protocol_t::parse_query(
        tcp_conn_t *conn,
        signal_t *interruptor,
        ql::query_cache_t *query_cache) {
    int64_t token;
    uint32_t size;
    conn->read(&token, sizeof(token), interruptor);
    conn->read(&size, sizeof(size), interruptor);

    if (size >= wire_protocol_t::TOO_LARGE_QUERY_SIZE) {
        ql::response_t error;
        error.fill_error(Response::CLIENT_ERROR,
                         Response::RESOURCE_LIMIT,
                         wire_protocol_t::too_large_query_message(size),
                         ql::backtrace_registry_t::EMPTY_BACKTRACE);
        send_response(&error, token, conn, interruptor);
        throw tcp_conn_read_closed_exc_t();
    }

    scoped_array_t<char> data(size + 1);
    conn->read(data.data(), size, interruptor);
    data[size] = 0; // Null terminate the string, which the json parser requires

    scoped_ptr_t<ql::query_params_t> res =
        parse_query_from_buffer(std::move(data), 0, query_cache, token);
    if (!res.has()) {
        ql::response_t error;
        error.fill_error(Response::CLIENT_ERROR,
                         Response::RESOURCE_LIMIT,
                         wire_protocol_t::unparseable_query_message,
                         ql::backtrace_registry_t::EMPTY_BACKTRACE);
        send_response(&error, token, conn, interruptor);
    }
    return res;
}

void write_response_internal(ql::response_t *response,
                             rapidjson::StringBuffer *buffer_out,
                             bool throw_errors) {
    rapidjson::Writer<rapidjson::StringBuffer> writer(*buffer_out);
    size_t start_offset = buffer_out->GetSize();

    try {
        writer.StartObject();
        writer.Key("t", 1);
        writer.Int(response->type());
        if (response->type() == Response::RUNTIME_ERROR &&
            response->error_type()) {
            writer.Key("e", 1);
            writer.Int(response->error_type());
        }
        writer.Key("r", 1);
        writer.StartArray();
        for (auto const &item : response->data()) {
            item.write_json(&writer);
        }
        writer.EndArray();
        if (response->backtrace()) {
            writer.Key("b", 1);
            response->backtrace()->write_json(&writer);
        }
        if (response->profile()) {
            writer.Key("p", 1);
            response->profile()->write_json(&writer);
        }
        if (response->type() == Response::SUCCESS_PARTIAL ||
            response->type() == Response::SUCCESS_SEQUENCE) {
            writer.Key("n", 1);
            writer.StartArray();
            for (auto const &note : response->notes()) {
                writer.Int(note);
            }
            writer.EndArray();
        }
        writer.EndObject();
    } catch (const ql::base_exc_t &ex) {
        buffer_out->Pop(buffer_out->GetSize() - start_offset);
        response->fill_error(Response::RUNTIME_ERROR, ex.what(),
                             ql::backtrace_registry_t::EMPTY_BACKTRACE);
        write_response_internal(response, buffer_out, true);
    } catch (const std::exception &ex) {
        if (throw_errors) {
            throw;
        }

        buffer_out->Pop(buffer_out->GetSize() - start_offset);
        response->fill_error(Response::RUNTIME_ERROR, Response::INTERNAL,
            strprintf("Internal error in json_protocol_t::write: %s", ex.what()),
            ql::backtrace_registry_t::EMPTY_BACKTRACE);
        write_response_internal(response, buffer_out, true);
    }
}

// Small wrapper - in debug mode we would rather crash than send the error back
void json_protocol_t::write_response_to_buffer(ql::response_t *response,
                                               rapidjson::StringBuffer *buffer_out) {
#ifdef NDEBUG
    write_response_internal(response, buffer_out, false);
#else
    write_response_internal(response, buffer_out, true);
#endif
}

void json_protocol_t::send_response(ql::response_t *response,
                                    int64_t token,
                                    tcp_conn_t *conn,
                                    signal_t *interruptor) {
    uint32_t data_size; // filled in below
    const size_t prefix_size = sizeof(token) + sizeof(data_size);

    // Reserve space for the token and the size
    rapidjson::StringBuffer buffer;
    buffer.Push(prefix_size);

    write_response_to_buffer(response, &buffer);
    int64_t payload_size = buffer.GetSize() - prefix_size;
    guarantee(payload_size > 0);

    if (payload_size >= wire_protocol_t::TOO_LARGE_RESPONSE_SIZE) {
        response->fill_error(Response::RUNTIME_ERROR,
                             Response::RESOURCE_LIMIT,
                             wire_protocol_t::too_large_response_message(payload_size),
                             ql::backtrace_registry_t::EMPTY_BACKTRACE);
        send_response(response, token, conn, interruptor);
        return;
    }

    // Fill in the token and size
    char *mutable_buffer = buffer.GetMutableBuffer();
    for (size_t i = 0; i < sizeof(token); ++i) {
        mutable_buffer[i] = reinterpret_cast<const char *>(&token)[i];
    }

    data_size = static_cast<uint32_t>(payload_size);
    for (size_t i = 0; i < sizeof(data_size); ++i) {
        mutable_buffer[i + sizeof(token)] = reinterpret_cast<const char *>(&data_size)[i];
    }

    conn->write(buffer.GetString(), buffer.GetSize(), interruptor);
}

