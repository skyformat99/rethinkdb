// Copyright 2010-2012 RethinkDB, all rights reserved.
#include "serializer/serializer.hpp"
#include "arch/arch.hpp"

file_account_t *serializer_t::make_io_account(int priority) {
    assert_thread();
    return make_io_account(priority, UNLIMITED_OUTSTANDING_REQUESTS);
}

serializer_write_t serializer_write_t::make_touch(block_id_t block_id, repli_timestamp_t recency) {
    serializer_write_t w;
    w.block_id = block_id;
    w.action_type = TOUCH;
    w.action.touch.recency = recency;
    return w;
}

serializer_write_t serializer_write_t::make_update(
        block_id_t block_id, repli_timestamp_t recency, const void *buf,
        iocallback_t *io_callback, serializer_write_launched_callback_t *launch_callback)
{
    serializer_write_t w;
    w.block_id = block_id;
    w.action_type = UPDATE;
    w.action.update.buf = buf;
    w.action.update.recency = recency;
    w.action.update.io_callback = io_callback;
    w.action.update.launch_callback = launch_callback;
    return w;
}

serializer_write_t serializer_write_t::make_delete(block_id_t block_id) {
    serializer_write_t w;
    w.block_id = block_id;
    w.action_type = DELETE;
    return w;
}

struct write_cond_t : public cond_t, public iocallback_t {
    explicit write_cond_t(iocallback_t *cb) : callback(cb) { }
    void on_io_complete() {
        if (callback)
            callback->on_io_complete();
        pulse();
    }
    iocallback_t *callback;
};

ser_buffer_t *convert_buffer_cache_buf_to_ser_buffer(const void *buf) {
    return static_cast<ser_buffer_t *>(const_cast<void *>(buf)) - 1;
}

void perform_write(const serializer_write_t *write, serializer_t *ser, file_account_t *acct, std::vector<write_cond_t *> *conds, index_write_op_t *op) {
    switch (write->action_type) {
    case serializer_write_t::UPDATE: {
        conds->push_back(new write_cond_t(write->action.update.io_callback));
        std::vector<buf_write_info_t> write_infos;
        write_infos.push_back(buf_write_info_t(convert_buffer_cache_buf_to_ser_buffer(write->action.update.buf),
                                               ser->get_block_size().ser_value(),  // RSI: use actual block size
                                               op->block_id));
        std::vector<counted_t<standard_block_token_t> > tokens
            = ser->block_writes(write_infos, acct, conds->back());
        guarantee(tokens.size() == 1);
        op->token = tokens[0];

        if (write->action.update.launch_callback) {
            write->action.update.launch_callback->on_write_launched(op->token.get());
        }
        op->recency = write->action.update.recency;
    } break;
    case serializer_write_t::DELETE: {
        op->token = counted_t<standard_block_token_t>();
        op->recency = repli_timestamp_t::invalid;
    } break;
    case serializer_write_t::TOUCH: {
        op->recency = write->action.touch.recency;
    } break;
    default:
        unreachable();
    }
}

void do_writes(serializer_t *ser, const std::vector<serializer_write_t>& writes, file_account_t *io_account) {
    ser->assert_thread();
    std::vector<write_cond_t*> block_write_conds;
    std::vector<index_write_op_t> index_write_ops;
    block_write_conds.reserve(writes.size());
    index_write_ops.reserve(writes.size());

    // Step 1: Write buffers to disk and assemble index operations
    for (size_t i = 0; i < writes.size(); ++i) {
        const serializer_write_t *write = &writes[i];
        index_write_op_t op(write->block_id);

        perform_write(write, ser, io_account, &block_write_conds, &op);

        index_write_ops.push_back(op);
    }

    // Step 2: Wait on all writes to finish
    for (size_t i = 0; i < block_write_conds.size(); ++i) {
        block_write_conds[i]->wait();
        delete block_write_conds[i];
    }
    block_write_conds.clear();

    // Step 3: Commit the transaction to the serializer
    ser->index_write(index_write_ops, io_account);
}

void serializer_data_ptr_t::free() {
    rassert(ptr_.has());
    ptr_.reset();
}

void serializer_data_ptr_t::init_malloc(serializer_t *ser) {
    rassert(!ptr_.has());
    ptr_ = ser->malloc();
}

void serializer_data_ptr_t::init_clone(serializer_t *ser, const serializer_data_ptr_t& other) {
    rassert(other.ptr_.has());
    rassert(!ptr_.has());
    ptr_ = ser->clone(other.ptr_.get());
}

// RSI: take a ser_buffer_t * arg.
counted_t<standard_block_token_t> serializer_block_write(serializer_t *ser, const void *buf,
                                                         block_id_t block_id, file_account_t *io_account) {
    struct : public cond_t, public iocallback_t {
        void on_io_complete() { pulse(); }
    } cb;
    ser_buffer_t *ser_buf = convert_buffer_cache_buf_to_ser_buffer(buf);

    // RSI: use the real block size thank you.
    std::vector<counted_t<standard_block_token_t> > tokens
        = ser->block_writes({ buf_write_info_t(ser_buf,
                                               ser->get_block_size().ser_value(),
                                               block_id) }, io_account, &cb);
    guarantee(tokens.size() == 1);
    cb.wait();
    return tokens[0];

}

