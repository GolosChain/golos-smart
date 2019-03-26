#include "golos.vesting.hpp"
#include "config.hpp"
#include "headers.hpp"
#include <eosiolib/transaction.hpp>
#include <eosiolib/event.hpp>
#include <golos.charge/golos.charge.hpp>
#include <common/dispatchers.hpp>

namespace golos {


using namespace eosio;

struct vesting_params_setter: set_params_visitor<vesting_state> {
    using set_params_visitor::set_params_visitor; // enable constructor

    bool operator()(const vesting_withdraw_param& p) {
        return set_param(p, &vesting_state::withdraw);
    }

    bool operator()(const vesting_min_amount_param& p) {
        return set_param(p, &vesting_state::min_amount);
    }

    bool operator()(const delegation_param& p) {
        return set_param(p, &vesting_state::delegation);
    }
};

void vesting::validateprms(symbol symbol, std::vector<vesting_param> params) {
    vesting_params_singleton cfg(_self, symbol.code().raw());
    param_helper::check_params(params, cfg.exists());
}

void vesting::setparams(symbol symbol, std::vector<vesting_param> params) {
    eosio_assert(symbol.is_valid(), "not valid symbol");

    require_auth(name(token::get_issuer(config::token_name, symbol.code())));

    vesting_params_singleton cfg(_self, symbol.code().raw());
    param_helper::set_parameters<vesting_params_setter>(params, cfg, name(token::get_issuer(config::token_name, symbol.code())));
}

name get_recipient(const std::string& memo) {
    const size_t pref_size = config::send_prefix.size();
    const size_t memo_size = memo.size();
    if (memo_size < pref_size || memo.substr(0, pref_size) != config::send_prefix)
        return name();
    eosio_assert(memo_size > pref_size, "must provide recipient's name");
    return name(memo.substr(pref_size).c_str());
}

void vesting::on_transfer(name from, name to, asset quantity, std::string memo) {
    if (_self != to)
        return;

    auto recipient = get_recipient(memo);
    if(token::get_issuer(config::token_name, quantity.symbol.code()) == from && recipient == name())
        return;     // just increase token supply

    vesting_table table_vesting(_self, _self.value);    // TODO: use symbol as scope
    auto vesting = table_vesting.find(quantity.symbol.code().raw());
    eosio_assert(vesting != table_vesting.end(), "Token not found");

    asset converted = convert_to_vesting(quantity, *vesting);
    table_vesting.modify(vesting, name(), [&](auto& item) {
        item.supply += converted;
        // TODO Add notify about supply changes
    });

    add_balance(recipient != name() ? recipient : from, converted, has_auth(to) ? to : from);
}

void vesting::retire(asset quantity, name user) {
    require_auth(name(token::get_issuer(config::token_name, quantity.symbol.code())));
    eosio_assert(quantity.is_valid(), "invalid quantity");
    eosio_assert(quantity.amount > 0, "must retire positive quantity");

    vesting_table table_vesting(_self, _self.value);
    auto vesting = table_vesting.find(quantity.symbol.code().raw());
    eosio_assert(vesting != table_vesting.end(), "Vesting not found");
    eosio_assert(quantity.symbol == vesting->supply.symbol, "symbol precision mismatch");
    eosio_assert(quantity.amount <= vesting->supply.amount, "invalid amount");

    sub_balance(user, quantity, true);
    table_vesting.modify(vesting, name(), [&](auto& item) {
        item.supply -= quantity;
        // TODO Add notify about supply changes
    });
}

void vesting::withdraw(name from, name to, asset quantity) {
    require_auth(from);

    vesting_params_singleton cfg(_self, quantity.symbol.code().raw());
    eosio_assert(cfg.exists(), "not found vesting params");
    const auto& withdraw_params = cfg.get().withdraw;
    const auto& min_amount = cfg.get().min_amount.min_amount;

    account_table account(_self, from.value);
    auto vest = account.find(quantity.symbol.code().raw());
    eosio_assert(vest != account.end(), "unknown asset");
    eosio_assert(vest->vesting.symbol == quantity.symbol, "wrong asset precision");
    eosio_assert(vest->available_vesting().amount >= quantity.amount, "Insufficient funds");
    eosio_assert(vest->vesting.amount >= min_amount, "Insufficient funds for converting");
    eosio_assert(quantity.amount > 0, "quantity must be positive");

    withdraw_table table(_self, quantity.symbol.code().raw());
    auto record = table.find(from.value);

    const auto intervals = withdraw_params.intervals;
    auto fill_record = [&](auto& item) {
        item.to = to;
        item.number_of_payments = intervals;
        item.next_payout = time_point_sec(now() + withdraw_params.interval_seconds);
        item.withdraw_rate = quantity / intervals;
        item.target_amount = quantity;
    };

    if (record != table.end()) {
        table.modify(record, from, fill_record);
    } else {
        table.emplace(from, [&](auto& item) {
            item.from = from;
            fill_record(item);
        });
    }
}

void vesting::stopwithdraw(name owner, symbol sym) {
    require_auth(owner);
    withdraw_table table(_self, sym.code().raw());
    auto record = table.find(owner.value);
    eosio_assert(record != table.end(), "Not found convert record sender");
    table.erase(record);
}

void vesting::unlocklimit(name owner, asset quantity) {
    require_auth(owner);
    auto sym = quantity.symbol;
    eosio_assert(quantity.is_valid(), "invalid quantity");
    eosio_assert(quantity.amount >= 0, "the number of tokens should not be less than 0");
    account_table accounts(_self, owner.value);
    const auto& b = accounts.get(sym.code().raw(), "no balance object found");
    eosio_assert(b.unlocked_limit.symbol == sym, "symbol precision mismatch");

    accounts.modify(b, name(), [&](auto& item) {
        item.unlocked_limit = quantity;
    });
}

void vesting::delegate(name from, name to, asset quantity, uint16_t interest_rate, uint8_t payout_strategy) {
    require_auth(from);

    vesting_params_singleton cfg(_self, quantity.symbol.code().raw());
    eosio_assert(cfg.exists(), "not found vesting params");
    const auto& params = cfg.get();
    const auto& min_amount = params.min_amount.min_amount;
    const auto& delegation_params = params.delegation;
    const auto& withdraw_params = params.withdraw;

    eosio_assert(from != to, "You can not delegate to yourself");
    eosio_assert(payout_strategy == config::to_delegator || payout_strategy == config::to_delegated_vesting,
        "not valid value payout_strategy");
    eosio_assert(quantity.amount > 0, "the number of tokens should not be less than 0");
    eosio_assert(quantity.amount >= min_amount, "Insufficient funds for delegation");
    eosio_assert(interest_rate <= delegation_params.max_interest, "Exceeded the percentage of delegated vesting");

    auto token_code = quantity.symbol.code();
    auto sname = token_code.raw();
    account_table account_sender(_self, from.value);
    auto balance_sender = account_sender.find(sname);
    eosio_assert(balance_sender != account_sender.end(), "Not found token");
    auto user_balance = balance_sender->vesting;

    withdraw_table convert_tbl(_self, sname);
    auto convert_obj = convert_tbl.find(from.value);
    if (convert_obj != convert_tbl.end()) {
        // TODO: this calculation must not depend on parameters, object should contain all required info inside
        // TODO: it's simpler to have "ramaining_amount" inside object, so all this calculations will became unneeded
        auto remains_int = convert_obj->withdraw_rate * convert_obj->number_of_payments;
        auto remains_fract = convert_obj->target_amount - convert_obj->withdraw_rate * withdraw_params.intervals;
        user_balance -= (remains_int + remains_fract);
    }
    auto deleg_after = quantity + balance_sender->delegated;
    eosio_assert(user_balance >= deleg_after, "insufficient funds for delegation");
    int64_t deleg_prop = user_balance.amount ?
        (static_cast<int128_t>(deleg_after.amount) * config::_100percent) / user_balance.amount : 0;
    eosio_assert(charge::get_current_value(config::charge_name, from, token_code) <= config::_100percent - deleg_prop,
        "can't delegate, not enough power");

    account_sender.modify(balance_sender, from, [&](auto& item){
        item.delegated += quantity;
        // TODO Add notify about vesting changed
    });

    delegation_table table(_self, sname);
    auto index_table = table.get_index<"delegator"_n>();
    auto delegate_record = index_table.find({from, to});
    if (delegate_record != index_table.end()) {
        eosio_assert(delegate_record->interest_rate == interest_rate, "interest_rate does not match");
        eosio_assert(delegate_record->payout_strategy == payout_strategy, "payout_strategy does not match");
        index_table.modify(delegate_record, name(), [&](auto& item) {
            item.quantity += quantity;
            // TODO: we need to update `min_delegation_time` here to make it work properly,
            //       else it's restriction can be easily evaded
        });
    } else {
        table.emplace(from, [&](auto& item){
            item.id = table.available_primary_key();
            item.delegator = from;
            item.delegatee = to;
            item.quantity = quantity;
            item.interest_rate = interest_rate;
            item.payout_strategy = payout_strategy;
            item.min_delegation_time = time_point_sec(now() + delegation_params.min_time);
        });
    }

    account_table account_recipient(_self, to.value);
    auto balance_recipient = account_recipient.find(sname);
    eosio_assert(balance_recipient != account_recipient.end(), "Not found balance token vesting");
    account_recipient.modify(balance_recipient, from, [&](auto& item) {
        item.received += quantity;
        // TODO Add notify about vesting changed
    });

    eosio_assert(balance_recipient->received.amount >= delegation_params.min_remainder, "delegated vesting withdrawn");
}

void vesting::undelegate(name from, name to, asset quantity) {
    require_auth(from);

    vesting_params_singleton cfg(_self, quantity.symbol.code().raw());
    eosio_assert(cfg.exists(), "not found vesting params");
    const auto& delegation_params = cfg.get().delegation;

    delegation_table table(_self, quantity.symbol.code().raw());
    auto index_table = table.get_index<"delegator"_n>();
    auto delegate_record = index_table.find({from, to});
    eosio_assert(delegate_record != index_table.end(), "Not enough delegated vesting"); // wrong

    eosio_assert(quantity.amount >= delegation_params.min_amount, "Insufficient funds for undelegation");
    eosio_assert(delegate_record->min_delegation_time <= time_point_sec(now()), "Tokens are frozen until the end of the period");
    eosio_assert(delegate_record->quantity >= quantity, "There are not enough delegated tools for output");

    if (delegate_record->quantity == quantity) {
        index_table.erase(delegate_record);
    } else {
        index_table.modify(delegate_record, from, [&](auto& item) {
            item.quantity -= quantity;
        });
    }

    return_delegation_table table_delegate_vesting(_self, _self.value);
    table_delegate_vesting.emplace(from, [&](auto& item) {
        item.id = table_delegate_vesting.available_primary_key();
        item.delegator = from;
        item.quantity = quantity;
        item.date = time_point_sec(now() + delegation_params.return_time);
    });

    account_table account_recipient(_self, to.value);
    auto balance = account_recipient.find(quantity.symbol.code().raw());
    eosio_assert(balance != account_recipient.end(), "This token is not on the recipient balance sheet");
    account_recipient.modify(balance, from, [&](auto& item) {
        item.received -= quantity;
        // TODO Add notify about vesting changed
    });

    eosio_assert(balance->received.amount >= delegation_params.min_remainder, "delegated vesting withdrawn");
}

void vesting::create(symbol symbol, name notify_acc) {
    require_auth(name(token::get_issuer(name(account_token), symbol.code())));

    vesting_table table_vesting(_self, _self.value);
    auto vesting = table_vesting.find(symbol.code().raw());
    eosio_assert(vesting == table_vesting.end(), "Vesting already exists");

    table_vesting.emplace(_self, [&](auto& item){
        item.supply = asset(0, symbol);
        item.notify_acc = notify_acc;
        // TODO Add notify about supply changes
    });
}

void vesting::timeoutconv() {
    require_auth(_self);
    vesting_table table_vesting(_self, _self.value);

    for (auto vesting : table_vesting) {
        vesting_params_singleton cfg(_self, vesting.supply.symbol.code().raw());
        eosio_assert(cfg.exists(), "not found vesting params");
        const auto& withdraw_params = cfg.get().withdraw;

        withdraw_table table(_self, vesting.supply.symbol.code().raw());
        auto index = table.get_index<"nextpayout"_n>();
        for (auto obj = index.cbegin(); obj != index.cend(); ) {
            if (obj->next_payout > time_point_sec(now()))
                break;

            if (obj->number_of_payments > 0) {
                // TODO: withdraw_record must not depend on parameters because they can change between calls
                // TODO: this action should never fail, because fail will prevent all withdrawals
                index.modify(obj, name(), [&](auto& item) {
                    item.next_payout = time_point_sec(now() + withdraw_params.interval_seconds);
                    --item.number_of_payments;

                    account_table account(_self, obj->from.value);
                    auto balance = account.find(obj->withdraw_rate.symbol.code().raw());
                    eosio_assert(balance != account.end(), "Vesting balance not found");    // must not happen here, checked earlier

                    auto quantity = balance->vesting;
                    if (balance->vesting < obj->withdraw_rate) {
                        item.number_of_payments = 0;
                    } else if (!obj->number_of_payments) { // TODO obj->number_of_payments == 0
                        quantity = obj->target_amount - (obj->withdraw_rate * (withdraw_params.intervals - 1));
                    } else {
                        quantity = obj->withdraw_rate;
                    }

                    sub_balance(obj->from, quantity);
                    auto vest = table_vesting.find(quantity.symbol.code().raw());
                    eosio_assert(vest != table_vesting.end(), "Vesting not found"); // must not happen at this point
                    table_vesting.modify(vest, name(), [&](auto& item) {
                        item.supply -= quantity;
                        // TODO Add notify about supply change
                    });
                    INLINE_ACTION_SENDER(token, transfer)(config::token_name, {_self, config::active_name},
                        {_self, obj->to, convert_to_token(quantity, *vest), "Convert vesting"});
                });

                ++obj;
            } else {
                obj = index.erase(obj);
            }

        }
    }
}

void vesting::timeoutrdel() {
    // TODO: this action must never throw because it will break returning of delegations on all accounts
    require_auth(_self);
    return_delegation_table table_delegate_vesting(_self, _self.value);
    auto index = table_delegate_vesting.get_index<"date"_n>();
    auto till = time_point_sec(now());
    for (auto obj = index.cbegin(); obj != index.cend() && obj->date <= till;) {
        account_table account_recipient(_self, obj->delegator.value);
        auto balance_recipient = account_recipient.find(obj->quantity.symbol.code().raw());
        eosio_assert(balance_recipient != account_recipient.end(), "This token is not on the sender balance sheet");
        account_recipient.modify(balance_recipient, name(), [&](auto &item){
            item.delegated -= obj->quantity;
            // TODO Add event about vesting changed
        });
        obj = index.erase(obj);
    }
}

void vesting::open(name owner, symbol symbol, name ram_payer) {
    require_auth(ram_payer);
    account_table accounts(_self, owner.value);
    auto it = accounts.find(symbol.code().raw());
    eosio_assert(it == accounts.end(), "already exists");
    accounts.emplace(ram_payer, [&](auto& a) {
        a.vesting.symbol = symbol;
        a.delegated.symbol = symbol;
        a.received.symbol = symbol;
        a.unlocked_limit.symbol = symbol;
    });
}

void vesting::close(name owner, symbol symbol) {
    require_auth(owner);
    account_table account(_self, owner.value);
    auto it = account.find(symbol.code().raw());
    eosio_assert(it != account.end(), "Balance row already deleted or never existed. Action won't have any effect");
    eosio_assert(it->vesting.amount == 0, "Cannot close because the balance vesting is not zero");
    eosio_assert(it->delegated.amount == 0, "Cannot close because the balance delegate vesting is not zero");
    eosio_assert(it->received.amount == 0, "Cannot close because the balance received vesting not zero");
    account.erase(it);
}

void vesting::notify_balance_change(name owner, asset diff) {
    vesting_table table_vesting(_self, _self.value);
    auto notify = table_vesting.find(diff.symbol.code().raw());
    action(
        permission_level{_self, config::active_name},
        notify->notify_acc,
        "changevest"_n,
        std::make_tuple(owner, diff)
    ).send();
}

void vesting::sub_balance(name owner, asset value, bool retire_mode) {
    eosio_assert(value.amount >= 0, "sub_balance: value.amount < 0");
    if (value.amount == 0)
        return;
    account_table account(_self, owner.value);
    const auto& from = account.get(value.symbol.code().raw(), "no balance object found");
    if (retire_mode)
        eosio_assert(from.unlocked_vesting() >= value, "overdrawn unlocked balance");
    else
        eosio_assert(from.available_vesting() >= value, "overdrawn balance");

    account.modify(from, name(), [&](auto& a) {
        a.vesting -= value;
        if (retire_mode)
            a.unlocked_limit -= value;
        // Add notify about vesting changed
    });
    notify_balance_change(owner, -value);
}

void vesting::add_balance(name owner, asset value, name ram_payer) {
    eosio_assert(value.amount >= 0, "add_balance: value.amount < 0");
    if (value.amount == 0)
        return;
    account_table account(_self, owner.value);
    auto to = account.find(value.symbol.code().raw());
    if (to == account.end()) {
        account.emplace(ram_payer, [&](auto& a) {
            a.vesting = value;
            a.delegated.symbol = value.symbol;
            a.received.symbol = value.symbol;
            // TODO Add notify about vesting changed
        });
    } else {
        account.modify(to, name(), [&](auto& a) {
            a.vesting += value;
            // TODO Add notify about vesting changed
        });
    }
    notify_balance_change(owner, value);
}

void vesting::send_account_event(name account, const struct account& balance) {
    eosio::event(_self, "balance"_n, std::make_tuple(account, balance)).send();
}

void vesting::send_vesting_event(const vesting_stats& info) {
    eosio::event(_self, "vesting"_n, info.supply).send();
}

int64_t fix_precision(const asset from, const symbol to) {
    int a = from.symbol.precision();
    int b = to.precision();
    if (a == b) {
        return from.amount;
    }
    auto e10 = [](int x) {
        int r = 1;
        while (x-- > 0)
            r *= 10;
        return r;
    };
    int min = std::min(a, b);
    auto div = e10(a - min);
    auto mult = e10(b - min);
    return static_cast<int128_t>(from.amount) * mult / div;
}

const asset vesting::convert_to_token(const asset& src, const vesting_stats& vinfo) const {
    auto sym = src.symbol;
    auto token_supply = token::get_balance(config::token_name, _self, sym.code());
    // eosio_assert(sym.name() == token_supply.symbol.name() && sym == vinfo.supply.symbol, "The token type does not match");   // guaranteed to be valid here

    int64_t amount = vinfo.supply.amount && token_supply.amount
        ? static_cast<int64_t>((static_cast<int128_t>(src.amount) * token_supply.amount)
            / (vinfo.supply.amount + src.amount))
        : fix_precision(src, token_supply.symbol);
    return asset(amount, token_supply.symbol);
}

const asset vesting::convert_to_vesting(const asset& src, const vesting_stats& vinfo) const {
    auto sym = src.symbol;
    auto balance = token::get_balance(config::token_name, _self, sym.code()) - src;

    int64_t amount = vinfo.supply.amount && balance.amount
        ? static_cast<int64_t>((static_cast<int128_t>(src.amount) * vinfo.supply.amount) / balance.amount)
        : fix_precision(src, vinfo.supply.symbol);
    return asset(amount, vinfo.supply.symbol);
}

void vesting::timeout() {
    require_auth(_self);
    transaction trx;
    trx.actions.emplace_back(action{permission_level(_self, config::active_name), _self, "timeoutrdel"_n, ""});
    trx.actions.emplace_back(action{permission_level(_self, config::active_name), _self, "timeoutconv"_n, ""});
    trx.actions.emplace_back(action{permission_level(_self, config::active_name), _self, "timeout"_n,     ""});
    trx.delay_sec = config::vesting_delay_tx_timeout;
    trx.send(_self.value, _self);
}

void vesting::paydelegator(name account, asset reward, name delegator, uint8_t payout_strategy) {
    require_auth(_self);
    if (payout_strategy == config::payout_strategy::to_delegated_vesting) {
        delegation_table table(_self, reward.symbol.code().raw());
        auto index_table = table.get_index<"delegator"_n>();
        auto delegate_record = index_table.find({delegator, account});
        if (delegate_record != index_table.end()) {
            account_table acc_table_dlg(_self, delegator.value);
            auto balance_dlg = acc_table_dlg.find(reward.symbol.code().raw());
            acc_table_dlg.modify(balance_dlg, name(), [&](auto& item) {
                item.delegated += reward;
            });
            account_table acc_table_rcv(_self, account.value);
            auto balance_rcv = acc_table_rcv.find(reward.symbol.code().raw());
            acc_table_rcv.modify(balance_rcv, name(), [&](auto& item) {
                item.received += reward;
            });
            index_table.modify(delegate_record, name(), [&](auto& item) {
                item.quantity += reward;
            });
        }
    }
    add_balance(delegator, reward, same_payer);
}

} // golos

DISPATCH_WITH_TRANSFER(golos::vesting, on_transfer, (validateprms)(setparams)
        (retire)(unlocklimit)(withdraw)(stopwithdraw)(delegate)(undelegate)(create)
        (open)(close)(timeout)(timeoutconv)(timeoutrdel)(paydelegator))
