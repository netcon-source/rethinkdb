#include "clustering/administration/http/server.hpp"

#include "clustering/administration/http/directory_app.hpp"
#include "clustering/administration/http/distribution_app.hpp"
#include "clustering/administration/http/issues_app.hpp"
#include "clustering/administration/http/last_seen_app.hpp"
#include "clustering/administration/http/log_app.hpp"
#include "clustering/administration/http/progress_app.hpp"
#include "clustering/administration/http/semilattice_app.hpp"
#include "clustering/administration/http/stat_app.hpp"
#include "http/file_app.hpp"
#include "http/http.hpp"
#include "http/routing_app.hpp"
#include "rpc/directory/watchable_copier.hpp"
#include "rpc/semilattice/view/field.hpp"

std::map<peer_id_t, log_server_business_card_t> get_log_mailbox(const std::map<peer_id_t, cluster_directory_metadata_t> &md) {
    std::map<peer_id_t, log_server_business_card_t> out;
    for (std::map<peer_id_t, cluster_directory_metadata_t>::const_iterator it = md.begin(); it != md.end(); it++) {
        out.insert(std::make_pair(it->first, it->second.log_mailbox));
    }
    return out;
}

std::map<peer_id_t, machine_id_t> get_machine_id(const std::map<peer_id_t, cluster_directory_metadata_t> &md) {
    std::map<peer_id_t, machine_id_t> out;
    for (std::map<peer_id_t, cluster_directory_metadata_t>::const_iterator it = md.begin(); it != md.end(); it++) {
        out.insert(std::make_pair(it->first, it->second.machine_id));
    }
    return out;
}

administrative_http_server_manager_t::administrative_http_server_manager_t(
        int port,
        mailbox_manager_t *mbox_manager,
        boost::shared_ptr<semilattice_readwrite_view_t<cluster_semilattice_metadata_t> > _semilattice_metadata,
        clone_ptr_t<watchable_t<std::map<peer_id_t, cluster_directory_metadata_t> > > _directory_metadata,
        namespace_repo_t<memcached_protocol_t> *_namespace_repo,
        global_issue_tracker_t *_issue_tracker,
        last_seen_tracker_t *_last_seen_tracker,
        boost::uuids::uuid _us,
        std::string path)
{
    std::set<std::string> white_list;
    white_list.insert("/cluster.css");
    white_list.insert("/cluster.html");
    white_list.insert("/cluster-min.js");
    white_list.insert("/favicon.ico");
    white_list.insert("/js/backbone.js");
    white_list.insert("/js/bootstrap/bootstrap-alert.js");
    white_list.insert("/js/bootstrap/bootstrap-modal.js");
    white_list.insert("/js/bootstrap/bootstrap-tab.js");
    white_list.insert("/js/bootstrap/bootstrap-typeahead.js");
    white_list.insert("/js/bootstrap/bootstrap-collapse.js");
    white_list.insert("/js/d3.v2.min.js");
    white_list.insert("/js/date-en-US.js");
    white_list.insert("/js/flot/jquery.flot.js");
    white_list.insert("/js/flot/jquery.flot.resize.js");
    white_list.insert("/js/handlebars-1.0.0.beta.6.js");
    white_list.insert("/js/jquery-1.7.1.min.js");
    white_list.insert("/js/jquery.dataTables.min.js");
    white_list.insert("/js/jquery.form.js");
    white_list.insert("/js/jquery.hotkeys.js");
    white_list.insert("/js/jquery.sparkline.min.js");
    white_list.insert("/js/jquery.timeago.js");
    white_list.insert("/js/jquery.validate.min.js");
    white_list.insert("/js/underscore-min.js");
    white_list.insert("/images/alert-icon_small.png");
    white_list.insert("/images/critical-issue_small.png");
    white_list.insert("/images/information-icon_small.png");
    white_list.insert("/images/mini_right-arrow.png");
    white_list.insert("/images/mini_down-arrow.png");
    white_list.insert("/index.html");
    file_app.reset(new file_http_app_t(white_list, path));

    semilattice_app.reset(new semilattice_http_app_t(_semilattice_metadata, _directory_metadata, _us));
    directory_app.reset(new directory_http_app_t(_directory_metadata));
    issues_app.reset(new issues_http_app_t(_issue_tracker));
    stat_app.reset(new stat_http_app_t(mbox_manager, _directory_metadata));
    last_seen_app.reset(new last_seen_http_app_t(_last_seen_tracker));
    log_app.reset(new log_http_app_t(mbox_manager,
        _directory_metadata->subview(&get_log_mailbox),
        _directory_metadata->subview(&get_machine_id)
        ));
    progress_app.reset(new progress_app_t(_directory_metadata, mbox_manager));
    distribution_app.reset(new distribution_app_t(metadata_field(&cluster_semilattice_metadata_t::memcached_namespaces, _semilattice_metadata), _namespace_repo));

    std::map<std::string, http_app_t *> ajax_routes;
    ajax_routes["directory"] = directory_app.get();
    ajax_routes["issues"] = issues_app.get();
    ajax_routes["stat"] = stat_app.get();
    ajax_routes["last_seen"] = last_seen_app.get();
    ajax_routes["log"] = log_app.get();
    ajax_routes["progress"] = progress_app.get();
    ajax_routes["distribution"] = distribution_app.get();
    ajax_routing_app.reset(new routing_http_app_t(semilattice_app.get(), ajax_routes));

    std::map<std::string, http_app_t *> root_routes;
    root_routes["ajax"] = ajax_routing_app.get();
    root_routing_app.reset(new routing_http_app_t(file_app.get(), root_routes));

    server.reset(new http_server_t(port, root_routing_app.get()));
}

administrative_http_server_manager_t::~administrative_http_server_manager_t() {
    /* This must be declared in the `.cc` file because the definitions of the
    destructors for the things in `boost::scoped_ptr`s are not available from
    the `.hpp` file. */
}