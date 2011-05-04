#include "arch/linux/disk/pool.hpp"

#define BLOCKER_POOL_QUEUE_DEPTH (MAX_CONCURRENT_IO_REQUESTS * 3)

pool_diskmgr_t::pool_diskmgr_t(
        linux_event_queue_t *queue, passive_producer_t<action_t *> *source) :
    source(source),
    blocker_pool(MAX_CONCURRENT_IO_REQUESTS, queue)
{
    if (source->available->get()) pump();
    source->available->add_watcher(this);
}

pool_diskmgr_t::~pool_diskmgr_t() {
    source->available->remove_watcher(this);
}

void pool_diskmgr_t::action_t::run() {
    int res;
    if (is_read) res = pread(fd, buf, count, offset);
    else res = pwrite(fd, buf, count, offset);
    guarantee_err((size_t)res == count, "pread()/pwrite() failed.");
}

void pool_diskmgr_t::action_t::done() {
    parent->n_pending--;
    parent->pump();
    parent->done_fun(this);
}

void pool_diskmgr_t::on_watchable_changed() {
    /* This is called when the queue used to be empty but now has requests on
    it, and also when the queue's last request is consumed. */
    if (source->available->get()) pump();
}

void pool_diskmgr_t::pump() {
    while (source->available->get() && n_pending < BLOCKER_POOL_QUEUE_DEPTH) {
        action_t *a = source->pop();
        a->parent = this;
        n_pending++;
        blocker_pool.do_job(a);
    }
}