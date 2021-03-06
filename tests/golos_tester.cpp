#include "golos_tester.hpp"
#include <eosio/chain/permission_object.hpp>

namespace eosio { namespace testing {

using std::vector;
using std::string;

uint64_t hash64(const std::string& s) {
    return fc::sha256::hash(s.c_str(), s.size())._hash[0];
}

authority create_code_authority(const std::vector<name> contracts) {
    authority auth(1, {});
    for (const auto contract: contracts) {
        auth.accounts.push_back({.permission = {contract, config::eosio_code_name}, .weight = 1});
    }
    return auth;
}

void golos_tester::install_contract(
    account_name acc, const vector<uint8_t>& wasm, const vector<char>& abi, bool produce
) {
    set_code(acc, wasm);
    set_abi (acc, abi.data());
    if (produce)
        produce_block();
    const auto& accnt = control->chaindb().get<account_object>(acc);
    abi_def abi_d;
    BOOST_CHECK_EQUAL(abi_serializer::to_abi(accnt.abi, abi_d), true);
    _abis[acc].set_abi(abi_d, abi_serializer_max_time);
};

vector<permission> golos_tester::get_account_permissions(account_name a) {
    vector<permission> r;
    auto table = _chaindb.get_table<permission_object>();
    auto perms = table.get_index<by_owner>();
    auto perm = perms.lower_bound(boost::make_tuple(a));
    while (perm != perms.end() && perm->owner == a) {
        name parent;
        if (perm->parent._id) {
            const auto* p = _chaindb.find<permission_object,by_id>(perm->parent);
            if (p) {
                parent = p->name;
            }
        }
        r.push_back(permission{perm->name, parent, perm->auth.to_authority()});
        ++perm;
    }
    return r;
}

fc::optional<permission> golos_tester::get_account_permission(account_name a, permission_name p) {
    auto perm = _chaindb.find<permission_object,by_owner>(boost::make_tuple(a,p));
    if (!perm) {
        return fc::optional<permission>();
    }

    name parent;
    if (perm->parent._id) {
        const auto* p = _chaindb.find<permission_object,by_id>(perm->parent);
        if (p) {
            parent = p->name;
        }
    }
    return permission{perm->name, parent, perm->auth.to_authority()};
}

bool golos_tester::has_code_authority(name account, permission_name perm, name code) {
    auto p = get_account_permission(account, perm);
    if (!p.valid()) return false;
    for (auto a: p->required_auth.accounts) {
        if (a.permission.actor == code && a.permission.permission == config::eosio_code_name) {
            return true;
        }
    }
    return false;
}

bool golos_tester::has_link_authority(name account, permission_name perm, name code, action_name action) {
    auto table = _chaindb.get_table<permission_link_object>();
    auto index = table.get_index<by_permission_name>();

    return index.end() != index.find(boost::make_tuple(account, perm, code, action)) ||
           index.end() != index.find(boost::make_tuple(account, perm, code, action_name()));
}

// this method do produce_block() internaly and checks that chain_has_transaction()
base_tester::action_result golos_tester::push_and_check_action(
    account_name code,
    action_name name,
    account_name signer,
    const variant_object& data
) {
    auto& abi = _abis[code];
    action act;
    act.account = code;
    act.name    = name;
    act.data    = abi.variant_to_binary(abi.get_action_type(name), data, abi_serializer_max_time);
    return base_tester::push_action(std::move(act), uint64_t(signer));
}

base_tester::action_result golos_tester::push_action(
    account_name code,
    action_name name,
    account_name signer,
    const variant_object& data
) {
    try {
        base_tester::push_action(code, name, signer, data);
    } catch (const fc::exception& ex) {
        edump((ex.to_detail_string()));
        return error(ex.top_message());
    }
    return success();
}

base_tester::action_result golos_tester::push_action_msig_tx(
    account_name code,
    action_name name,
    vector<permission_level> perms,
    vector<account_name> signers,
    const variant_object& data
) {
    auto& abi = _abis[code];
    action act;
    act.account = code;
    act.name    = name;
    act.data    = abi.variant_to_binary(abi.get_action_type(name), data, abi_serializer_max_time);
    for (const auto& perm : perms) {
        act.authorization.emplace_back(perm);
    }

    signed_transaction tx;
    tx.actions.emplace_back(std::move(act));
    set_transaction_headers(tx);
    for (const auto& a : signers) {
        tx.sign(get_private_key(a, "active"), control->get_chain_id());
    }
    return push_tx(std::move(tx));
}

base_tester::action_result golos_tester::push_tx(signed_transaction&& tx) {
    try {
        push_transaction(tx);
    } catch (const fc::exception& ex) {
        edump((ex.to_detail_string()));
        return error(ex.top_message()); // top_message() is assumed by many tests; otherwise they fail
        //return error(ex.to_detail_string());
    }
    produce_block();
    BOOST_REQUIRE_EQUAL(true, chain_has_transaction(tx.id()));
    return success();
}

variant golos_tester::get_chaindb_struct(name code, uint64_t scope, name tbl, uint64_t id,
    const string& n
) const {
    variant r;
    try {
        r = _chaindb.object_by_pk({code, scope, tbl}, id).value;
    } catch (...) {
        // key not found
    }
    return r;
}

variant golos_tester::get_chaindb_singleton(name code, uint64_t scope, name tbl,
    const string& n
) const {
    return get_chaindb_struct(code, scope, tbl, tbl, n);
}

vector<variant> golos_tester::get_all_chaindb_rows(name code, uint64_t scope, name tbl, bool strict) const {
    vector<variant> all;
    auto info = _chaindb.lower_bound({code, scope, tbl, N(primary)}, cyberway::chaindb::cursor_kind::ManyRecords, nullptr, 0);
    cyberway::chaindb::cursor_request cursor = {code, info.cursor};
    for (auto pk = _chaindb.current(cursor); pk != cyberway::chaindb::primary_key::End; pk = _chaindb.next(cursor)) {
        auto v = _chaindb.object_at_cursor(cursor).value;
        all.push_back(v);
    }
    if (!all.size()) {
        BOOST_TEST_REQUIRE(!strict);
    }
    return all;
}

}} // eosio::tesing
