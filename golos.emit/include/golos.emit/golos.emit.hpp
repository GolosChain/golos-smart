#pragma once
#include "golos.emit/parameters.hpp"
#include <eosiolib/eosio.hpp>
#include <eosiolib/singleton.hpp>
#include <eosiolib/asset.hpp>


namespace golos {

using namespace eosio;

struct [[eosio::table]] state {
    uint64_t prev_emit;
    uint128_t tx_id;
    uint64_t start_time;
    bool active;
};
using state_singleton = eosio::singleton<N(state), state>;


class emission: public contract {
public:
    emission(account_name self, symbol_type token, uint64_t action = 0);

    [[eosio::action]] void validateprms(std::vector<emit_param>);
    [[eosio::action]] void setparams(std::vector<emit_param>);

    [[eosio::action]] void emit();
    [[eosio::action]] void start();
    [[eosio::action]] void stop();

private:
    void recalculate_state(std::vector<emit_param>);
    symbol_type _token;
    state_singleton _state;
    emit_params_singleton _cfg;

    void schedule_next(state& s, uint32_t delay);
};

} // golos
