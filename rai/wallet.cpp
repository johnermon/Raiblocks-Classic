#include <rai/wallet.hpp>

#include <rai/node.hpp>

#include <argon2.h>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <ed25519-donna/ed25519.h>

rai::work_pool::work_pool () :
current (0),
ticket (0),
done (false)
{
	static_assert (ATOMIC_INT_LOCK_FREE == 2, "Atomic int needed");
	auto count (std::max (1u, std::thread::hardware_concurrency ()));
	for (auto i (0); i < count; ++i)
	{
		threads.push_back (std::thread ([this, i] ()
		{
			loop (i);
		}));
	}
}

rai::work_pool::~work_pool ()
{
	stop ();
	for (auto &i: threads)
	{
		i.join ();
	}
}

namespace
{
class xorshift1024star
{
public:
    xorshift1024star ():
    p (0)
    {
    }
    std::array <uint64_t, 16> s;
    unsigned p;
    uint64_t next ()
    {
        auto p_l (p);
        auto pn ((p_l + 1) & 15);
        p = pn;
        uint64_t s0 = s[ p_l ];
        uint64_t s1 = s[ pn ];
        s1 ^= s1 << 31; // a
        s1 ^= s1 >> 11; // b
        s0 ^= s0 >> 30; // c
        return ( s[ pn ] = s0 ^ s1 ) * 1181783497276652981LL;
    }
};
}

void rai::work_pool::loop (uint64_t thread)
{
    xorshift1024star rng;
    rng.s.fill (0x0123456789abcdef + thread);// No seed here, we're not securing anything, s just can't be 0 per the xorshift1024star spec
	uint64_t work;
	uint64_t output;
    blake2b_state hash;
	blake2b_init (&hash, sizeof (output));
	std::unique_lock <std::mutex> lock (mutex);
	while (!done || !pending.empty())
	{
		auto current_l (current);
		if (!current_l.is_zero ())
		{
			int ticket_l (ticket);
			lock.unlock ();
			output = 0;
			while (ticket == ticket_l && output < rai::work_pool::publish_threshold)
			{
				unsigned iteration (256);
				while (iteration && output < rai::work_pool::publish_threshold)
				{
					work = rng.next ();
					blake2b_update (&hash, reinterpret_cast <uint8_t *> (&work), sizeof (work));
					blake2b_update (&hash, current_l.bytes.data (), current_l.bytes.size ());
					blake2b_final (&hash, reinterpret_cast <uint8_t *> (&output), sizeof (output));
					blake2b_init (&hash, sizeof (output));
					iteration -= 1;
				}
			}
			lock.lock ();
			if (current == current_l)
			{
				assert (output >= rai::work_pool::publish_threshold);
				assert (work_value (current_l, work) == output);
				++ticket;
				completed [current_l] = work;
				consumer_condition.notify_all ();
				// Change current so only one work thread publishes their result
				current.clear ();
			}
		}
		else
		{
			if (!pending.empty ())
			{
				current = pending.front ();
				pending.pop ();
				producer_condition.notify_all ();
			}
			else
			{
				producer_condition.wait (lock);
			}
		}
	}
}

void rai::work_pool::generate (rai::block & block_a)
{
    block_a.block_work_set (generate (block_a.root ()));
}

uint64_t rai::work_pool::work_value (rai::block_hash const & root_a, uint64_t work_a)
{
	uint64_t result;
    blake2b_state hash;
	blake2b_init (&hash, sizeof (result));
    blake2b_update (&hash, reinterpret_cast <uint8_t *> (&work_a), sizeof (work_a));
    blake2b_update (&hash, root_a.bytes.data (), root_a.bytes.size ());
    blake2b_final (&hash, reinterpret_cast <uint8_t *> (&result), sizeof (result));
	return result;
}

bool rai::work_pool::work_validate (rai::block_hash const & root_a, uint64_t work_a)
{
    auto result (work_value (root_a, work_a) < rai::work_pool::publish_threshold);
	return result;
}

bool rai::work_pool::work_validate (rai::block & block_a)
{
    return work_validate (block_a.root (), block_a.block_work ());
}

void rai::work_pool::stop ()
{
	std::lock_guard <std::mutex> lock (mutex);
	done = true;
	producer_condition.notify_all ();
}

uint64_t rai::work_pool::generate (rai::uint256_union const & root_a)
{
	assert (!root_a.is_zero ());
	uint64_t result;
	std::unique_lock <std::mutex> lock (mutex);
	pending.push (root_a);
	producer_condition.notify_one ();
	auto done (false);
	while (!done)
	{
		consumer_condition.wait (lock);
		auto finish (completed.find (root_a));
		if (finish != completed.end ())
		{
			done = true;
			result = finish->second;
			completed.erase (finish);
		}
	}
	return result;
}

rai::uint256_union rai::wallet_store::check (MDB_txn * transaction_a)
{
	rai::wallet_value value (entry_get_raw (transaction_a, rai::wallet_store::check_special));
    return value.key;
}

rai::uint256_union rai::wallet_store::salt (MDB_txn * transaction_a)
{
	rai::wallet_value value (entry_get_raw (transaction_a, rai::wallet_store::salt_special));
    return value.key;
}

rai::uint256_union rai::wallet_store::wallet_key (MDB_txn * transaction_a)
{
	rai::wallet_value value (entry_get_raw (transaction_a, rai::wallet_store::wallet_key_special));
    auto password_l (password.value ());
    auto result (value.key.prv (password_l, salt (transaction_a).owords [0]));
    password_l.clear ();
    return result;
}

bool rai::wallet_store::valid_password (MDB_txn * transaction_a)
{
    rai::uint256_union zero;
    zero.clear ();
    auto wallet_key_l (wallet_key (transaction_a));
    rai::uint256_union check_l (zero, wallet_key_l, salt (transaction_a).owords [0]);
    wallet_key_l.clear ();
    return check (transaction_a) == check_l;
}

void rai::wallet_store::enter_password (MDB_txn * transaction_a, std::string const & password_a)
{
    password.value_set (derive_key (transaction_a, password_a));
}

bool rai::wallet_store::rekey (MDB_txn * transaction_a, std::string const & password_a)
{
    bool result (false);
	if (valid_password (transaction_a))
    {
        auto password_new (derive_key (transaction_a, password_a));
        auto wallet_key_l (wallet_key (transaction_a));
        auto password_l (password.value ());
        (*password.values [0]) ^= password_l;
        (*password.values [0]) ^= password_new;
        rai::uint256_union encrypted (wallet_key_l, password_new, salt (transaction_a).owords [0]);
		entry_put_raw (transaction_a, rai::wallet_store::wallet_key_special, rai::wallet_value (encrypted));
        wallet_key_l.clear ();
    }
    else
    {
        result = true;
    }
    return result;
}

rai::uint256_union rai::wallet_store::derive_key (MDB_txn * transaction_a, std::string const & password_a)
{
	rai::uint256_union result;
	auto salt_l (salt (transaction_a));
    auto success (PHS (result.bytes.data (), result.bytes.size (), password_a.data (), password_a.size (), salt_l.bytes.data (), salt_l.bytes.size (), 1, kdf_work));
	assert (success == 0); (void) success;
    return result;
}

rai::wallet_value::wallet_value (MDB_val const & val_a)
{
	assert (val_a.mv_size == sizeof (*this));
	std::copy (reinterpret_cast <uint8_t const *> (val_a.mv_data), reinterpret_cast <uint8_t const *> (val_a.mv_data) + sizeof (key), key.chars.begin ());
	std::copy (reinterpret_cast <uint8_t const *> (val_a.mv_data) + sizeof (key), reinterpret_cast <uint8_t const *> (val_a.mv_data) + sizeof (key) + sizeof (work), reinterpret_cast <char *> (&work));
}

rai::wallet_value::wallet_value (rai::uint256_union const & value_a) :
key (value_a),
work (0)
{
}

rai::mdb_val rai::wallet_value::val () const
{
	static_assert (sizeof (*this) == sizeof (key) + sizeof (work), "Class not packed");
	return rai::mdb_val (sizeof (*this), const_cast <rai::wallet_value *> (this));
}

rai::uint256_union const rai::wallet_store::version_1 (1);
rai::uint256_union const rai::wallet_store::version_current (version_1);
rai::uint256_union const rai::wallet_store::version_special (0);
rai::uint256_union const rai::wallet_store::salt_special (1);
rai::uint256_union const rai::wallet_store::wallet_key_special (2);
rai::uint256_union const rai::wallet_store::check_special (3);
rai::uint256_union const rai::wallet_store::representative_special (4);
int const rai::wallet_store::special_count (5);

rai::wallet_store::wallet_store (bool & init_a, rai::transaction & transaction_a, rai::account representative_a, std::string const & wallet_a, std::string const & json_a) :
password (0, 1024),
environment (transaction_a.environment)
{
    init_a = false;
    initialize (transaction_a, init_a, wallet_a);
    if (!init_a)
    {
        MDB_val junk;
		assert (mdb_get (transaction_a, handle, version_special.val (), &junk) == MDB_NOTFOUND);
        boost::property_tree::ptree wallet_l;
        std::stringstream istream (json_a);
        boost::property_tree::read_json (istream, wallet_l);
        for (auto i (wallet_l.begin ()), n (wallet_l.end ()); i != n; ++i)
        {
            rai::uint256_union key;
            init_a = key.decode_hex (i->first);
            if (!init_a)
            {
                rai::uint256_union value;
                init_a = value.decode_hex (wallet_l.get <std::string> (i->first));
                if (!init_a)
                {
					entry_put_raw (transaction_a, key, rai::wallet_value (value));
                }
                else
                {
                    init_a = true;
                }
            }
            else
            {
                init_a = true;
            }
        }
		init_a = init_a || mdb_get (transaction_a, handle, version_special.val (), &junk) != 0;
		init_a = init_a || mdb_get (transaction_a, handle, wallet_key_special.val (), & junk) != 0;
		init_a = init_a || mdb_get (transaction_a, handle, salt_special.val (), &junk) != 0;
		init_a = init_a || mdb_get (transaction_a, handle, check_special.val (), &junk) != 0;
		init_a = init_a || mdb_get (transaction_a, handle, representative_special.val (), &junk) != 0;
		password.value_set (0);
    }
}

rai::wallet_store::wallet_store (bool & init_a, rai::transaction & transaction_a, rai::account representative_a, std::string const & wallet_a) :
password (0, 1024),
environment (transaction_a.environment)
{
    init_a = false;
    initialize (transaction_a, init_a, wallet_a);
    if (!init_a)
    {
		int version_status;
		MDB_val version_value;
		version_status = mdb_get (transaction_a, handle, version_special.val (), &version_value);
        if (version_status == MDB_NOTFOUND)
        {
			entry_put_raw (transaction_a, rai::wallet_store::version_special, rai::wallet_value (version_current));
            rai::uint256_union salt_l;
            random_pool.GenerateBlock (salt_l.bytes.data (), salt_l.bytes.size ());
			entry_put_raw (transaction_a, rai::wallet_store::salt_special, rai::wallet_value (salt_l));
            // Wallet key is a fixed random key that encrypts all entries
            rai::uint256_union wallet_key;
            random_pool.GenerateBlock (wallet_key.bytes.data (), sizeof (wallet_key.bytes));
            password.value_set (0);
            // Wallet key is encrypted by the user's password
            rai::uint256_union encrypted (wallet_key, 0, salt_l.owords [0]);
			entry_put_raw (transaction_a, rai::wallet_store::wallet_key_special, rai::wallet_value (encrypted));
            rai::uint256_union zero (0);
            rai::uint256_union check (zero, wallet_key, salt_l.owords [0]);
			entry_put_raw (transaction_a, rai::wallet_store::check_special, rai::wallet_value (check));
            wallet_key.clear ();
			entry_put_raw (transaction_a, rai::wallet_store::representative_special, rai::wallet_value (representative_a));
        }
        else
        {
            enter_password (transaction_a, "");
        }
    }
}

std::vector <rai::account> rai::wallet_store::accounts (MDB_txn * transaction_a)
{
	std::vector <rai::account> result;
	for (auto i (begin (transaction_a)), n (end ()); i != n; ++i)
	{
		rai::account account (i->first);
		result.push_back (account);
	}
	return result;
}

void rai::wallet_store::initialize (MDB_txn * transaction_a, bool & init_a, std::string const & path_a)
{
	assert (strlen (path_a.c_str ()) == path_a.size ());
	auto error (mdb_dbi_open (transaction_a, path_a.c_str (), MDB_CREATE, &handle));
	init_a = error != 0;
}

bool rai::wallet_store::is_representative (MDB_txn * transaction_a)
{
    return exists (transaction_a, representative (transaction_a));
}

void rai::wallet_store::representative_set (MDB_txn * transaction_a, rai::account const & representative_a)
{
	entry_put_raw (transaction_a, rai::wallet_store::representative_special, rai::wallet_value (representative_a));
}

rai::account rai::wallet_store::representative (MDB_txn * transaction_a)
{
	rai::wallet_value value (entry_get_raw (transaction_a, rai::wallet_store::representative_special));
    return value.key;
}

rai::public_key rai::wallet_store::insert (MDB_txn * transaction_a, rai::private_key const & prv)
{
    rai::public_key pub;
    ed25519_publickey (prv.bytes.data (), pub.bytes.data ());
	entry_put_raw (transaction_a, pub, rai::wallet_value (rai::uint256_union (prv, wallet_key (transaction_a), salt (transaction_a).owords [0])));
	return pub;
}

void rai::wallet_store::erase (MDB_txn * transaction_a, rai::public_key const & pub)
{
	auto status (mdb_del (transaction_a, handle, pub.val (), nullptr));
    assert (status == 0);
}

rai::wallet_value rai::wallet_store::entry_get_raw (MDB_txn * transaction_a, rai::public_key const & pub_a)
{
	rai::wallet_value result;
	MDB_val value;
	auto status (mdb_get (transaction_a, handle, pub_a.val (), &value));
	if (status == 0)
	{
		result = rai::wallet_value (value);
	}
	else
	{
		result.key.clear ();
		result.work = 0;
	}
	return result;
}

void rai::wallet_store::entry_put_raw (MDB_txn * transaction_a, rai::public_key const & pub_a, rai::wallet_value const & entry_a)
{
	auto status (mdb_put (transaction_a, handle, pub_a.val (), entry_a.val (), 0));
	assert (status == 0);
}

bool rai::wallet_store::fetch (MDB_txn * transaction_a, rai::public_key const & pub, rai::private_key & prv)
{
    auto result (false);
	rai::wallet_value value (entry_get_raw (transaction_a, pub));
    if (!value.key.is_zero ())
    {
        prv = value.key.prv (wallet_key (transaction_a), salt (transaction_a).owords [0]);
        rai::public_key compare;
        ed25519_publickey (prv.bytes.data (), compare.bytes.data ());
        if (!(pub == compare))
        {
            result = true;
        }
    }
    else
    {
        result = true;
    }
    return result;
}

bool rai::wallet_store::exists (MDB_txn * transaction_a, rai::public_key const & pub)
{
    return find (transaction_a, pub) != end ();
}

void rai::wallet_store::serialize_json (MDB_txn * transaction_a, std::string & string_a)
{
    boost::property_tree::ptree tree;
    for (rai::store_iterator i (transaction_a, handle), n (nullptr); i != n; ++i)
    {
        tree.put (rai::uint256_union (i->first).to_string (), rai::wallet_value (i->second).key.to_string ());
    }
    std::stringstream ostream;
    boost::property_tree::write_json (ostream, tree);
    string_a = ostream.str ();
}

void rai::wallet_store::write_backup (MDB_txn * transaction_a, boost::filesystem::path const & path_a)
{
	std::ofstream backup_file;
	backup_file.open (path_a.string ());
	if (!backup_file.fail ())
	{
		std::string json;
		serialize_json (transaction_a, json);
		backup_file << json;
	}
}

bool rai::wallet_store::move (MDB_txn * transaction_a, rai::wallet_store & other_a, std::vector <rai::public_key> const & keys)
{
    assert (valid_password (transaction_a));
    assert (other_a.valid_password (transaction_a));
    auto result (false);
    for (auto i (keys.begin ()), n (keys.end ()); i != n; ++i)
    {
        rai::private_key prv;
        auto error (other_a.fetch (transaction_a, *i, prv));
        result = result | error;
        if (!result)
        {
            insert (transaction_a, prv);
            other_a.erase (transaction_a, *i);
        }
    }
    return result;
}

bool rai::wallet_store::import (MDB_txn * transaction_a, rai::wallet_store & other_a)
{
    assert (valid_password (transaction_a));
    assert (other_a.valid_password (transaction_a));
    auto result (false);
    for (auto i (other_a.begin (transaction_a)), n (end ()); i != n; ++i)
    {
        rai::private_key prv;
        auto error (other_a.fetch (transaction_a, i->first, prv));
        result = result | error;
        if (!result)
        {
            insert (transaction_a, prv);
            other_a.erase (transaction_a, i->first);
        }
    }
    return result;
}

bool rai::wallet_store::work_get (MDB_txn * transaction_a, rai::public_key const & pub_a, uint64_t & work_a)
{
	auto result (false);
	auto entry (entry_get_raw (transaction_a, pub_a));
	if (!entry.key.is_zero ())
	{
		work_a = entry.work;
	}
	else
	{
		result = true;
	}
	return result;
}

void rai::wallet_store::work_put (MDB_txn * transaction_a, rai::public_key const & pub_a, uint64_t work_a)
{
	auto entry (entry_get_raw (transaction_a, pub_a));
	assert (!entry.key.is_zero ());
	entry.work = work_a;
	entry_put_raw (transaction_a, pub_a, entry);
}

rai::wallet::wallet (bool & init_a, rai::transaction & transaction_a, rai::node & node_a, std::string const & wallet_a) :
store (init_a, transaction_a, node_a.config.random_representative (), wallet_a),
node (node_a)
{
}

rai::wallet::wallet (bool & init_a, rai::transaction & transaction_a, rai::node & node_a, std::string const & wallet_a, std::string const & json) :
store (init_a, transaction_a, node_a.config.random_representative (), wallet_a, json),
node (node_a)
{
}

void rai::wallet::enter_initial_password (MDB_txn * transaction_a)
{
	if (store.password.value ().is_zero ())
	{
		if (store.valid_password (transaction_a))
		{
			// Newly created wallets have a zero key
			store.rekey (transaction_a, "");
		}
		else
		{
			store.enter_password (transaction_a, "");
		}
	}
}

rai::public_key rai::wallet::insert (rai::private_key const & key_a)
{
	rai::block_hash root;
	rai::public_key key;
	{
		rai::transaction transaction (store.environment, nullptr, true);
		key = store.insert (transaction, key_a);
		auto this_l (shared_from_this ());
		root = node.ledger.latest_root (transaction, key);
	}
	work_generate (key, root);
	return key;
}

bool rai::wallet::exists (rai::public_key const & account_a)
{
	rai::transaction transaction (store.environment, nullptr, false);
	return store.exists (transaction, account_a);
}

bool rai::wallet::import (std::string const & json_a, std::string const & password_a)
{
	rai::transaction transaction (store.environment, nullptr, true);
	rai::uint256_union id;
	random_pool.GenerateBlock (id.bytes.data (), id.bytes.size ());
	auto error (false);
	rai::wallet_store temp (error, transaction, 0, id.to_string (), json_a);
	if (!error)
	{
		temp.enter_password (transaction, password_a);
		if (temp.valid_password (transaction))
		{
			error = store.import (transaction, temp);
		}
		else
		{
			error = true;
		}
	}
	temp.destroy (transaction);
	return error;
}

void rai::wallet::serialize (std::string & json_a)
{
	rai::transaction transaction (store.environment, nullptr, false);
	store.serialize_json (transaction, json_a);
}

void rai::wallet_store::destroy (MDB_txn * transaction_a)
{
	auto status (mdb_drop (transaction_a, handle, 1));
	assert (status == 0);
}

namespace {
bool check_ownership (rai::wallets & wallets_a, rai::account const & account_a) {
	std::lock_guard <std::mutex> lock (wallets_a.action_mutex);
	return wallets_a.current_actions.find (account_a) == wallets_a.current_actions.end ();
}
}

bool rai::wallet::receive_action (rai::send_block const & send_a, rai::private_key const & prv_a, rai::account const & representative_a)
{
	assert (!check_ownership (node.wallets, send_a.hashables.destination));
    auto hash (send_a.hash ());
    bool result;
	std::unique_ptr <rai::block> block;
	{
		rai::transaction transaction (node.ledger.store.environment, nullptr, false);
		if (node.ledger.store.pending_exists (transaction, hash))
		{
			rai::account_info info;
			auto new_account (node.ledger.store.account_get (transaction, send_a.hashables.destination, info));
			if (!new_account)
			{
				auto receive (new rai::receive_block (info.head, hash, prv_a, send_a.hashables.destination, work_fetch (transaction, send_a.hashables.destination, info.head)));
				block.reset (receive);
			}
			else
			{
				block.reset (new rai::open_block (hash, representative_a, send_a.hashables.destination, prv_a, send_a.hashables.destination, work_fetch (transaction, send_a.hashables.destination, send_a.hashables.destination)));
			}
			result = false;
		}
		else
		{
			result = true;
			// Ledger doesn't have this marked as available to receive anymore
		}
	}
	if (!result)
	{
		assert (block != nullptr);
		node.process_receive_republish (block->clone (), node.config.creation_rebroadcast);
		work_generate (send_a.hashables.destination, block->hash ());
	}
    return result;
}

bool rai::wallet::change_action (rai::account const & source_a, rai::account const & representative_a)
{
	assert (!check_ownership (node.wallets, source_a));
	std::unique_ptr <rai::change_block> block;
	auto result (false);
	{
		rai::transaction transaction (store.environment, nullptr, false);
		result = !store.valid_password (transaction);
		if (!result)
		{
			auto existing (store.find (transaction, source_a));
			if (existing != store.end ())
			{
				if (!node.ledger.latest (transaction, source_a).is_zero ())
				{
					rai::account_info info;
					result = node.ledger.store.account_get (transaction, source_a, info);
					assert (!result);
					rai::private_key prv;
					result = store.fetch (transaction, source_a, prv);
					assert (!result);
					block.reset (new rai::change_block (info.head, representative_a, prv, source_a, work_fetch (transaction, source_a, info.head)));
					prv.clear ();
				}
				else
				{
					result = true;
				}
			}
			else
			{
				result = true;
			}
		}
	}
	if (!result)
	{
		assert (block != nullptr);
		node.process_receive_republish (block->clone (), node.config.creation_rebroadcast);
		work_generate (source_a, block->hash ());
	}
	return result;
}

bool rai::wallet::send_action (rai::account const & source_a, rai::account const & account_a, rai::uint128_t const & amount_a)
{
	assert (!check_ownership (node.wallets, source_a));
	std::unique_ptr <rai::send_block> block;
	auto result (false);
	{
		rai::transaction transaction (store.environment, nullptr, false);
		result = !store.valid_password (transaction);
		if (!result)
		{
			auto existing (store.find (transaction, source_a));
			if (existing != store.end ())
			{
				auto balance (node.ledger.account_balance (transaction, source_a));
				if (!balance.is_zero ())
				{
					if (balance >= amount_a)
					{
						rai::account_info info;
						result = node.ledger.store.account_get (transaction, source_a, info);
						assert (!result);
						rai::private_key prv;
						result = store.fetch (transaction, source_a, prv);
						assert (!result);
						block.reset (new rai::send_block (info.head, account_a, balance - amount_a, prv, source_a, work_fetch (transaction, source_a, info.head)));
						prv.clear ();
					}
					else
					{
						result = true;
					}
				}
				else
				{
					result = true;
				}
			}
			else
			{
				result = true;
			}
		}
	}
	if (!result)
	{
		assert (block != nullptr);
		node.process_receive_republish (block->clone (), node.config.creation_rebroadcast);
		work_generate (source_a, block->hash ());
	}
	return result;
}

bool rai::wallet::change_sync (rai::account const & source_a, rai::account const & representative_a)
{
	std::mutex complete;
	complete.lock ();
	bool result;
	node.wallets.queue_wallet_action (source_a, std::numeric_limits <rai::uint128_t>::max (), [this, source_a, representative_a, &complete, &result] ()
	{
		result = change_action (source_a, representative_a);
		complete.unlock ();
	});
	complete.lock ();
	return result;
}

bool rai::wallet::receive_sync (rai::send_block const & block_a, rai::private_key const & prv_a, rai::account const & account_a, rai::uint128_t const & amount_a)
{
	std::mutex complete;
	complete.lock ();
	bool result;
	node.wallets.queue_wallet_action (block_a.hashables.destination, amount_a, [this, &block_a, &prv_a, account_a, &result, &complete] ()
	{
		result = receive_action (block_a, prv_a, account_a);
		complete.unlock ();
	});
	complete.lock ();
	return result;
}

bool rai::wallet::send_sync (rai::account const & source_a, rai::account const & account_a, rai::uint128_t const & amount_a)
{
	std::mutex complete;
	complete.lock ();
	bool result;
	node.wallets.queue_wallet_action (source_a, std::numeric_limits <rai::uint128_t>::max (), [this, source_a, account_a, amount_a, &complete, &result] ()
	{
		result = send_action (source_a, account_a, amount_a);
		complete.unlock ();
	});
	complete.lock ();
	return result;
}

// Update work for account if latest root is root_a
void rai::wallet::work_update (MDB_txn * transaction_a, rai::account const & account_a, rai::block_hash const & root_a, uint64_t work_a)
{
    assert (!node.work.work_validate (root_a, work_a));
    assert (store.exists (transaction_a, account_a));
    auto latest (node.ledger.latest_root (transaction_a, account_a));
    if (latest == root_a)
    {
        BOOST_LOG (node.log) << "Successfully cached work";
        store.work_put (transaction_a, account_a, work_a);
    }
    else
    {
        BOOST_LOG (node.log) << "Cached work no longer valid, discarding";
    }
}

// Fetch work for root_a, use cached value if possible
uint64_t rai::wallet::work_fetch (MDB_txn * transaction_a, rai::account const & account_a, rai::block_hash const & root_a)
{
    uint64_t result;
    auto error (store.work_get (transaction_a, account_a, result));
    if (error)
	{
        result = node.work.generate (root_a);
    }
	else
	{
		if (node.work.work_validate (root_a, result))
		{
			BOOST_LOG (node.log) << "Cached work invalid, regenerating";
			result = node.work.generate (root_a);
		}
	}
    return result;
}

namespace
{
class search_action : public std::enable_shared_from_this <search_action>
{
public:
	search_action (std::shared_ptr <rai::wallet> const & wallet_a, MDB_txn * transaction_a) :
	wallet (wallet_a)
	{
		for (auto i (wallet_a->store.begin (transaction_a)), n (wallet_a->store.end ()); i != n; ++i)
		{
			keys.insert (i->first);
		}
	}
	void run ()
	{
		BOOST_LOG (wallet->node.log) << "Beginning pending block search";
		rai::account account;
		std::unique_ptr <rai::block> block;
		{
			rai::transaction transaction (wallet->node.store.environment, nullptr, false);
			for (auto i (wallet->node.store.pending_begin (transaction)), n (wallet->node.store.pending_end ()); i != n && block == nullptr; ++i)
			{
				rai::receivable receivable (i->second);
				auto existing (keys.find (receivable.destination));
				if (existing != keys.end ())
				{
					rai::account_info info;
					wallet->node.store.account_get (transaction, receivable.source, info);
					account = receivable.source;
					BOOST_LOG (wallet->node.log) << boost::str (boost::format ("Found a pending block %1% from account %2% with head %3%") % current_block.to_string () % account.to_string () % info.head.to_string ());
					auto this_l (shared_from_this ());
					auto wallet_l (wallet);
					std::shared_ptr <rai::block> block_l (wallet->node.store.block_get (transaction, info.head).release ());
					wallet->node.background ([this_l, account, block_l, wallet_l]
					{
						wallet_l->node.conflicts.start (*block_l, [this_l, account] (rai::block &)
						{
							this_l->receive_all (account);
						}, true);
					});
					keys.erase (existing);
				}
			}
		}
		BOOST_LOG (wallet->node.log) << "Pending block search phase complete";
	}
	void receive_all (rai::account const & account_a)
	{
		BOOST_LOG (wallet->node.log) << boost::str (boost::format ("Account %1% confirmed, receiving all blocks") % account_a.to_base58check ());
		rai::transaction transaction (wallet->node.store.environment, nullptr, false);
		auto representative (wallet->store.representative (transaction));
		for (auto i (wallet->node.store.pending_begin (transaction)), n (wallet->node.store.pending_end ()); i != n; ++i)
		{
			rai::receivable receivable (i->second);
			if (receivable.source == account_a)
			{
				auto block_l (wallet->node.store.block_get (transaction, i->first));
				assert (dynamic_cast <rai::send_block *> (block_l.get ()) != nullptr);
				std::shared_ptr <rai::send_block> block (static_cast <rai::send_block *> (block_l.release ()));
				rai::private_key prv;
				auto error (wallet->store.fetch (transaction, receivable.destination, prv));
				if (error)
				{
					BOOST_LOG (wallet->node.log) << boost::str (boost::format ("Unable to fetch key for: %1%, stopping pending search") % receivable.destination.to_base58check ());
				}
				auto wallet_l (wallet);
				auto amount (receivable.amount.number ());
				wallet->node.background ([wallet_l, block, representative, prv, amount]
				{
					wallet_l->node.wallets.queue_wallet_action (block->hashables.destination, amount, [wallet_l, block, representative, prv] ()
					{
						BOOST_LOG (wallet_l->node.log) << boost::str (boost::format ("Receiving block: %1%") % block->hash ().to_string ());
						auto error (wallet_l->receive_action (*block, prv, representative));
						if (error)
						{
							BOOST_LOG (wallet_l->node.log) << boost::str (boost::format ("Error receiving block %1%") % block->hash ().to_string ());
						}
					});
				});
				prv.clear ();
			}
		}
	}
	rai::block_hash current_block;
	std::unordered_set <rai::uint256_union> keys;
	std::shared_ptr <rai::wallet> wallet;
};
}

bool rai::wallet::search_pending ()
{
	rai::transaction transaction (store.environment, nullptr, false);
	auto result (!store.valid_password (transaction));
	if (!result)
	{
		auto search (std::make_shared <search_action> (shared_from_this (), transaction));
		node.service.add (std::chrono::system_clock::now (), [search] ()
		{
			search->run ();
		});
	}
	else
	{
		BOOST_LOG (node.log) << "Stopping search, wallet is locked";
	}
	return result;
}

void rai::wallet::work_generate (rai::account const & account_a, rai::block_hash const & root_a)
{
	auto begin (std::chrono::system_clock::now ());
	if (node.config.logging.work_generation_time ())
	{
		BOOST_LOG (node.log) << "Beginning work generation";
	}
    auto work (node.work.generate (root_a));
	if (node.config.logging.work_generation_time ())
	{
		BOOST_LOG (node.log) << "Work generation complete: " << (std::chrono::duration_cast <std::chrono::microseconds> (std::chrono::system_clock::now () - begin).count ()) << "us";
	}
	rai::transaction transaction (store.environment, nullptr, true);
    work_update (transaction, account_a, root_a, work);
}

rai::wallets::wallets (bool & error_a, rai::node & node_a) :
observer ([] (rai::account const &, bool) {}),
node (node_a)
{
	if (!error_a)
	{
		rai::transaction transaction (node.store.environment, nullptr, true);
		auto status (mdb_dbi_open (transaction, nullptr, MDB_CREATE, &handle));
		assert (status == 0);
		std::string beginning (rai::uint256_union (0).to_string ());
		std::string end ((rai::uint256_union (rai::uint256_t (0) - rai::uint256_t (1))).to_string ());
		for (rai::store_iterator i (transaction, handle, rai::mdb_val (beginning.size (), const_cast <char *> (beginning.c_str ()))), n (transaction, handle, rai::mdb_val (end.size (), const_cast <char *> (end.c_str ()))); i != n; ++i)
		{
			rai::uint256_union id;
			std::string text (reinterpret_cast <char const *> (i->first.mv_data), i->first.mv_size);
			auto error (id.decode_hex (text));
			assert (!error);
			assert (items.find (id) == items.end ());
			auto wallet (std::make_shared <rai::wallet> (error, transaction, node_a, text));
			if (!error)
			{
				node_a.service.add (std::chrono::system_clock::now (), [wallet] ()
				{
					rai::transaction transaction (wallet->store.environment, nullptr, true);
					wallet->enter_initial_password (transaction);
				});
				items [id] = wallet;
			}
			else
			{
				// Couldn't open wallet
			}
		}
	}
}

std::shared_ptr <rai::wallet> rai::wallets::open (rai::uint256_union const & id_a)
{
    std::shared_ptr <rai::wallet> result;
    auto existing (items.find (id_a));
    if (existing != items.end ())
    {
        result = existing->second;
    }
    return result;
}

std::shared_ptr <rai::wallet> rai::wallets::create (rai::uint256_union const & id_a)
{
    assert (items.find (id_a) == items.end ());
    std::shared_ptr <rai::wallet> result;
    bool error;
	rai::transaction transaction (node.store.environment, nullptr, true);
    auto wallet (std::make_shared <rai::wallet> (error, transaction, node, id_a.to_string ()));
    if (!error)
    {
		node.service.add (std::chrono::system_clock::now (), [wallet] ()
		{
			rai::transaction transaction (wallet->store.environment, nullptr, true);
			wallet->enter_initial_password (transaction);
		});
        items [id_a] = wallet;
        result = wallet;
    }
    return result;
}

bool rai::wallets::search_pending (rai::uint256_union const & wallet_a)
{
	auto result (false);
	auto existing (items.find (wallet_a));
	result = existing == items.end ();
	if (!result)
	{
		auto wallet (existing->second);
		result = wallet->search_pending ();
	}
	return result;
}

void rai::wallets::destroy (rai::uint256_union const & id_a)
{
	rai::transaction transaction (node.store.environment, nullptr, true);
	auto existing (items.find (id_a));
	assert (existing != items.end ());
	auto wallet (existing->second);
	items.erase (existing);
	wallet->store.destroy (transaction);
}

void rai::wallets::queue_wallet_action (rai::account const & account_a, rai::uint128_t const & amount_a, std::function <void ()> const & action_a)
{
	auto current (std::move (action_a));
	auto perform (false);
	{
		std::lock_guard <std::mutex> lock (action_mutex);
		perform = current_actions.insert (account_a).second;
		if (!perform)
		{
			pending_actions [account_a].insert (decltype (pending_actions)::mapped_type::value_type (amount_a, std::move (current)));
		}
	}
	while (perform)
	{
		observer (account_a, true);
		current ();
		std::lock_guard <std::mutex> lock (node.wallets.action_mutex);
		auto existing (node.wallets.pending_actions.find (account_a));
		if (existing != node.wallets.pending_actions.end ())
		{
			auto & entries (existing->second);
			auto first (entries.begin ());
			assert (first != entries.end ());
			current = std::move (first->second);
			entries.erase (first);
			if (entries.empty ())
			{
				node.wallets.pending_actions.erase (existing);
			}
		}
		else
		{
			auto erased (node.wallets.current_actions.erase (account_a));
			assert (erased == 1); (void) erased;
			perform = false;
		}
		observer (account_a, false);
	}
}

void rai::wallets::foreach_representative (std::function <void (rai::public_key const & pub_a, rai::private_key const & prv_a)> const & action_a)
{
    for (auto i (items.begin ()), n (items.end ()); i != n; ++i)
    {
		rai::transaction transaction (node.store.environment, nullptr, false);
        auto & wallet (*i->second);
		for (auto j (wallet.store.begin (transaction)), m (wallet.store.end ()); j != m; ++j)
        {
			if (wallet.store.valid_password (transaction))
			{
				if (!node.ledger.weight (transaction, j->first).is_zero ())
				{
					rai::private_key prv;
					auto error (wallet.store.fetch (transaction, j->first, prv));
					assert (!error);
					action_a (j->first, prv);
					prv.clear ();
				}
			}
			else
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("Skipping locked wallet %1%") % i->first.to_string ());;
			}
        }
    }
}

rai::store_iterator rai::wallet_store::begin (MDB_txn * transaction_a)
{
    rai::store_iterator result (transaction_a, handle, rai::uint256_union (special_count).val ());
    return result;
}

rai::store_iterator rai::wallet_store::find (MDB_txn * transaction_a, rai::uint256_union const & key)
{
    rai::store_iterator result (transaction_a, handle, key.val ());
    rai::store_iterator end (nullptr);
    if (result != end)
    {
        if (rai::uint256_union (result->first) == key)
        {
            return result;
        }
        else
        {
            return end;
        }
    }
    else
    {
        return end;
    }
}

rai::store_iterator rai::wallet_store::end ()
{
    return rai::store_iterator (nullptr);
}