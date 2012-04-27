#ifndef CLUSTERING_ADMINISTRATION_REACTOR_DRIVER_TCC_
#define CLUSTERING_ADMINISTRATION_REACTOR_DRIVER_TCC_

#include "errors.hpp"
#include <boost/ptr_container/ptr_map.hpp>
#include <boost/shared_ptr.hpp>

#include "clustering/administration/machine_id_to_peer_id.hpp"
#include "clustering/administration/metadata.hpp"
#include "clustering/administration/reactor_driver.hpp"
#include "clustering/reactor/blueprint.hpp"
#include "clustering/reactor/reactor.hpp"
#include "concurrency/watchable.hpp"
#include "rpc/mailbox/mailbox.hpp"

/* This files contains the class reactor driver whose job is to create and
 * destroy reactors based on blueprints given to the server. */

/* The reactor driver is also responsible for the translation from
`persistable_blueprint_t` to `blueprint_t`. */
template<class protocol_t>
blueprint_t<protocol_t> translate_blueprint(const persistable_blueprint_t<protocol_t> &input, const std::map<peer_id_t, machine_id_t> &translation_table) {

    blueprint_t<protocol_t> output;
    for (typename persistable_blueprint_t<protocol_t>::role_map_t::const_iterator it = input.machines_roles.begin();
            it != input.machines_roles.end(); it++) {
        peer_id_t peer = machine_id_to_peer_id(it->first, translation_table);
        if (peer.is_nil()) {
            /* We can't determine the peer ID that belongs or belonged to this
            peer because we can't reach the peer. So we generate a new peer ID
            for the peer. This works because either way, the `reactor_t` will
            be unable to reach the peer. */
            peer = peer_id_t(generate_uuid());
        }
        output.peers_roles[peer] = it->second;
    }
    return output;
}

/* This is in part because these types aren't copyable so they can't go in
 * a std::pair. This class is used to hold a reactor and a watchable that
 * it's watching. */
template <class protocol_t>
class watchable_and_reactor_t : private master_t<protocol_t>::ack_checker_t {
public:
    watchable_and_reactor_t(reactor_driver_t<protocol_t> *parent_,
                            namespace_id_t _namespace_id,
                            const blueprint_t<protocol_t> &bp,
                            const std::string &_file_path) :
        watchable(bp),
        parent(parent_),
        namespace_id(_namespace_id),
        file_path(_file_path)
    {
        coro_t::spawn_sometime(boost::bind(&watchable_and_reactor_t<protocol_t>::initialize_reactor, this));
    }

    ~watchable_and_reactor_t() {
        /* Make sure that the coro we spawn to initialize this things has
         * actually run. */
        reactor_has_been_initialized.wait_lazily_unordered();

        reactor_directory_subscription.reset();
        master_directory_subscription.reset();
        {
            mutex_assertion_t::acq_t acq(&parent->watchable_variable_lock);
            namespaces_directory_metadata_t<protocol_t> directory = parent->watchable_variable.get_watchable()->get();
            rassert(directory.reactor_bcards.count(namespace_id) == 1);
            rassert(directory.master_maps.count(namespace_id) == 1);
            directory.reactor_bcards.erase(namespace_id);
            directory.master_maps.erase(namespace_id);
            parent->watchable_variable.set_value(directory);
        }

        /* Destroy the reactor. (Dun dun duhnnnn...) */
        reactor.reset();
    }

    std::string get_file_name() {
        return file_path + "/" + uuid_to_str(namespace_id);
    }

    watchable_variable_t<blueprint_t<protocol_t> > watchable;

    bool is_acceptable_ack_set(const std::set<peer_id_t> &acks) {
        /* There are a bunch of weird corner cases: what if the namespace was
        deleted? What if we got an ack from a machine but then it was declared
        dead? What if the namespaces `expected_acks` field is in conflict? We
        handle the weird cases by erring on the side of reporting that there
        are not enough acks yet. If a machine's `expected_acks` field is in
        conflict, for example, then all writes will report that there are not
        enough acks. That's a bit weird, but fortunately it can't lead to data
        corruption. */
        std::multiset<datacenter_id_t> acks_by_dc;
        for (std::set<peer_id_t>::const_iterator it = acks.begin(); it != acks.end(); it++) {
            std::map<peer_id_t, machine_id_t> translation_table_snapshot = parent->machine_id_translation_table->get();
            std::map<peer_id_t, machine_id_t>::iterator tt_it = translation_table_snapshot.find(*it);
            if (tt_it == translation_table_snapshot.end()) continue;
            machines_semilattice_metadata_t mmd = parent->machines_view->get();
            machines_semilattice_metadata_t::machine_map_t::iterator jt = mmd.machines.find(tt_it->second);
            if (jt == mmd.machines.end()) continue;
            if (jt->second.is_deleted()) continue;
            if (jt->second.get().datacenter.in_conflict()) continue;
            datacenter_id_t dc = jt->second.get().datacenter.get();
            acks_by_dc.insert(dc);
        }
        namespaces_semilattice_metadata_t<protocol_t> nmd = parent->namespaces_view->get();
        typename namespaces_semilattice_metadata_t<protocol_t>::namespace_map_t::const_iterator it =
            nmd.namespaces.find(namespace_id);
        if (it == nmd.namespaces.end()) return false;
        if (it->second.is_deleted()) return false;
        if (it->second.get().ack_expectations.in_conflict()) return false;
        std::map<datacenter_id_t, int> expected_acks = it->second.get().ack_expectations.get();
        for (std::map<datacenter_id_t, int>::const_iterator kt = expected_acks.begin(); kt != expected_acks.end(); kt++) {
            if (int(acks_by_dc.count(kt->first)) < kt->second) {
                return false;
            }
        }
        return true;
    }

private:
    std::map<peer_id_t, boost::optional<directory_echo_wrapper_t<reactor_business_card_t<protocol_t> > > > extract_reactor_directory(
            const std::map<peer_id_t, namespaces_directory_metadata_t<protocol_t> > &nss) {
        std::map<peer_id_t, boost::optional<directory_echo_wrapper_t<reactor_business_card_t<protocol_t> > > > out;
        for (typename std::map<peer_id_t, namespaces_directory_metadata_t<protocol_t> >::const_iterator it = nss.begin(); it != nss.end(); it++) {
            typename std::map<namespace_id_t, directory_echo_wrapper_t<reactor_business_card_t<protocol_t> > >::const_iterator jt =
                it->second.reactor_bcards.find(namespace_id);
            if (jt == it->second.reactor_bcards.end()) {
                out.insert(std::make_pair(it->first, boost::optional<directory_echo_wrapper_t<reactor_business_card_t<protocol_t> > >()));
            } else {
                out.insert(std::make_pair(it->first, boost::optional<directory_echo_wrapper_t<reactor_business_card_t<protocol_t> > >(jt->second)));
            }
        }
        return out;
    }

    void on_change_reactor_directory() {
        mutex_assertion_t::acq_t acq(&parent->watchable_variable_lock);
        namespaces_directory_metadata_t<protocol_t> directory = parent->watchable_variable.get_watchable()->get();
        directory.reactor_bcards.find(namespace_id)->second = reactor->get_reactor_directory()->get();
        parent->watchable_variable.set_value(directory);
    }

    void on_change_master_directory() {
        mutex_assertion_t::acq_t acq(&parent->watchable_variable_lock);
        namespaces_directory_metadata_t<protocol_t> directory = parent->watchable_variable.get_watchable()->get();
        directory.master_maps.find(namespace_id)->second = reactor->get_master_directory()->get();
        parent->watchable_variable.set_value(directory);
    }

    void initialize_reactor() {
        int res = access(get_file_name().c_str(), R_OK | W_OK);
        if (res == 0) {
            /* The file already exists thus we don't create it. */
            store.reset(new typename protocol_t::store_t(get_file_name(), false));
        } else {
            /* The file does not exist, create it. */
            store.reset(new typename protocol_t::store_t(get_file_name(), true));

            //Initialize the metadata in the underlying store
            boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> token;
            store->new_write_token(token);

            cond_t dummy_interruptor;
            store->set_metainfo(region_map_t<protocol_t, binary_blob_t>(store->get_region(), binary_blob_t(version_range_t(version_t::zero()))), token, &dummy_interruptor);
        }

        reactor.reset(new reactor_t<protocol_t>(
            parent->mbox_manager,
            this,
            parent->directory_view->subview(boost::bind(&watchable_and_reactor_t<protocol_t>::extract_reactor_directory, this, _1)),
            parent->branch_history,
            watchable.get_watchable(),
            store.get()));

        {
            typename watchable_t<directory_echo_wrapper_t<reactor_business_card_t<protocol_t> > >::freeze_t reactor_directory_freeze(reactor->get_reactor_directory());
            reactor_directory_subscription.reset(
                new typename watchable_t<directory_echo_wrapper_t<reactor_business_card_t<protocol_t> > >::subscription_t(
                    boost::bind(&watchable_and_reactor_t<protocol_t>::on_change_reactor_directory, this),
                    reactor->get_reactor_directory(), &reactor_directory_freeze
                ));
            typename watchable_t<std::map<master_id_t, master_business_card_t<protocol_t> > >::freeze_t master_directory_freeze(reactor->get_master_directory());
            master_directory_subscription.reset(
                new typename watchable_t<std::map<master_id_t, master_business_card_t<protocol_t> > >::subscription_t(
                    boost::bind(&watchable_and_reactor_t<protocol_t>::on_change_master_directory, this),
                    reactor->get_master_directory(), &master_directory_freeze
                ));
            mutex_assertion_t::acq_t acq(&parent->watchable_variable_lock);
            namespaces_directory_metadata_t<protocol_t> directory = parent->watchable_variable.get_watchable()->get();
            rassert(directory.reactor_bcards.count(namespace_id) == 0);
            rassert(directory.master_maps.count(namespace_id) == 0);
            directory.reactor_bcards.insert(std::make_pair(namespace_id, reactor->get_reactor_directory()->get()));
            directory.master_maps.insert(std::make_pair(namespace_id, reactor->get_master_directory()->get()));
            parent->watchable_variable.set_value(directory);
        }

        reactor_has_been_initialized.pulse();
    }

    cond_t reactor_has_been_initialized;

    reactor_driver_t<protocol_t> *parent;
    namespace_id_t namespace_id;
    std::string file_path;

    boost::scoped_ptr<typename protocol_t::store_t> store;
    boost::scoped_ptr<reactor_t<protocol_t> > reactor;

    boost::scoped_ptr<typename watchable_t<directory_echo_wrapper_t<reactor_business_card_t<protocol_t> > >::subscription_t> reactor_directory_subscription;
    boost::scoped_ptr<typename watchable_t<std::map<master_id_t, master_business_card_t<protocol_t> > >::subscription_t> master_directory_subscription;
};

template <class protocol_t>
reactor_driver_t<protocol_t>::reactor_driver_t(mailbox_manager_t *_mbox_manager,
                 const clone_ptr_t<watchable_t<std::map<peer_id_t, namespaces_directory_metadata_t<protocol_t> > > > &_directory_view,
                 boost::shared_ptr<semilattice_readwrite_view_t<namespaces_semilattice_metadata_t<protocol_t> > > _namespaces_view,
                 boost::shared_ptr<semilattice_read_view_t<machines_semilattice_metadata_t> > machines_view_,
                 const clone_ptr_t<watchable_t<std::map<peer_id_t, machine_id_t> > > &_machine_id_translation_table,
                 std::string _file_path)
    : mbox_manager(_mbox_manager),
      directory_view(_directory_view),
      branch_history(metadata_field(&namespaces_semilattice_metadata_t<protocol_t>::branch_history, _namespaces_view)),
      machine_id_translation_table(_machine_id_translation_table),
      namespaces_view(_namespaces_view),
      machines_view(machines_view_),
      file_path(_file_path),
      watchable_variable(namespaces_directory_metadata_t<protocol_t>()),
      semilattice_subscription(boost::bind(&reactor_driver_t<protocol_t>::on_change, this), namespaces_view),
      translation_table_subscription(boost::bind(&reactor_driver_t<protocol_t>::on_change, this))
{
    watchable_t<std::map<peer_id_t, machine_id_t> >::freeze_t freeze(machine_id_translation_table);
    translation_table_subscription.reset(machine_id_translation_table, &freeze);
    on_change();
}

template<class protocol_t>
reactor_driver_t<protocol_t>::~reactor_driver_t() {
    /* This must be defined in the `.tcc` file because the full definition of
    `watchable_and_reactor_t` is not available in the `.hpp` file. */
}

template<class protocol_t>
void reactor_driver_t<protocol_t>::delete_reactor_data(auto_drainer_t::lock_t lock, typename reactor_map_t::auto_type *thing_to_delete) {
    lock.assert_is_holding(&drainer);
    delete thing_to_delete;
}

template<class protocol_t>
void reactor_driver_t<protocol_t>::on_change() {
    typename namespaces_semilattice_metadata_t<protocol_t>::namespace_map_t namespaces = namespaces_view->get().namespaces;

    for (typename namespaces_semilattice_metadata_t<protocol_t>::namespace_map_t::const_iterator it =  namespaces.begin();
                                                                                                 it != namespaces.end();
                                                                                                 it++) {
        if (it->second.is_deleted() && std_contains(reactor_data, it->first)) {
            /* on_change cannot block because it is called as part of
             * semilattice subscription, however the
             * watchable_and_reactor_t destructor can block... therefore
             * bullshit takes place. We must release a value from the
             * ptr_map into this bullshit auto_type so that it's not in the
             * map but the destructor hasn't been called... then this needs
             * to be heap allocated so that it can be safely passed to a
             * coroutine for destruction. */
            coro_t::spawn_sometime(boost::bind(&reactor_driver_t<protocol_t>::delete_reactor_data, this, auto_drainer_t::lock_t(&drainer), new typename reactor_map_t::auto_type(reactor_data.release(reactor_data.find(it->first)))));
        } else {
            persistable_blueprint_t<protocol_t> pbp;

            try {
                pbp = it->second.get().blueprint.get();
            } catch (in_conflict_exc_t) {
                //Nothing to do for this namespaces, its blueprint is in
                //conflict.
                continue;
            }

            blueprint_t<protocol_t> bp = translate_blueprint(pbp, machine_id_translation_table->get());

            if (std_contains(bp.peers_roles, mbox_manager->get_connectivity_service()->get_me())) {
                /* Either construct a new reactor (if this is a namespace we
                 * haven't seen before). Or send the new blueprint to the
                 * existing reactor. */
                if (!std_contains(reactor_data, it->first)) {
                    namespace_id_t tmp = it->first;
                    reactor_data.insert(tmp, new watchable_and_reactor_t<protocol_t>(this, it->first, bp, file_path));
                } else {
                    reactor_data.find(it->first)->second->watchable.set_value(bp);
                }
            } else {
                /* The blueprint does not mentions us so we destroy the
                 * reactor. */
                if (std_contains(reactor_data, it->first)) {
                    coro_t::spawn_sometime(boost::bind(&reactor_driver_t<protocol_t>::delete_reactor_data, this, auto_drainer_t::lock_t(&drainer), new typename reactor_map_t::auto_type(reactor_data.release(reactor_data.find(it->first)))));
                }
            }
        }
    }
}

#endif /* CLUSTERING_ADMINISTRATION_REACTOR_DRIVER_TCC_ */