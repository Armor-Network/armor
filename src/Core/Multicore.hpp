// Copyright (c) 2012-2018, The CryptoNote developers, The Bytecoin developers.
// Licensed under the GNU Lesser General Public License. See LICENSE for details.

#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "BlockChain.hpp"  // for PreparedBlock
#include "CryptoNote.hpp"
#include "Wallet.hpp"  // for OutputHandler
#include "rpc_api.hpp"

// Experimental machinery to offload heavy calcs to other cores
// Without making any critical part of the Core multithreaded
// We do it by confining threads to "boxes"
namespace platform {
class EventLoop;
}
namespace cn {

class IBlockChainState;  // We will read keyimages and outputs from it
class Currency;

class BlockPreparatorMulticore {
	const Currency &currency;

	std::vector<std::thread> threads;
	mutable std::mutex mu;
	std::condition_variable have_work;
	platform::EventLoop *main_loop = nullptr;
	//	std::condition_variable prepared_blocks_ready;
	bool quit = false;

	std::deque<std::tuple<Hash, bool, RawBlock>> work;
	std::map<Hash, PreparedBlock> prepared_blocks;

	void thread_run();

public:
	explicit BlockPreparatorMulticore(const Currency &currency, platform::EventLoop *main_loop);
	~BlockPreparatorMulticore();

	void add_block(Hash bid, bool check_pow, RawBlock &&rb);
	bool get_prepared_block(Hash bid, PreparedBlock *pb);
	bool has_prepared_block(Hash bid) const;
};

struct RingSignatureArg {
	Hash tx_prefix_hash;
	Height newest_referenced_height = 0;
	KeyImage key_image;
	std::vector<PublicKey> output_keys;
	RingSignature input_signature;
};

struct RingSignatureArgA {
	Hash tx_prefix_hash;
	Height newest_referenced_height = 0;
	std::vector<KeyImage> key_images;
	std::vector<PublicKey> ps;
	std::vector<std::vector<PublicKey>> output_keys;
	RingSignatureAmethyst input_signature;
};

class RingCheckerMulticore {
	std::vector<std::thread> threads;
	mutable std::mutex mu;
	mutable std::condition_variable have_work;
	mutable std::condition_variable result_ready;
	bool quit = false;

	size_t total_counter = 0;
	size_t ready_counter = 0;
	std::vector<ConsensusErrorBadOutputOrSignature> errors;

	std::deque<RingSignatureArg> args;
	std::deque<RingSignatureArgA> argsa;
	int work_counter = 0;
	void thread_run();

public:
	RingCheckerMulticore();
	~RingCheckerMulticore();
	void cancel_work();
	void start_work(IBlockChainState *state, const Currency &currency, const Block &block, Height unlock_height,
	    Timestamp block_timestamp, Timestamp block_median_timestamp);  // can throw ConsensusError immediately
	std::vector<ConsensusErrorBadOutputOrSignature> move_errors();
};

struct PreparedWalletTransaction {
	TransactionPrefix tx;
	Hash prefix_hash;
	Hash inputs_hash;
	KeyDerivation derivation;  // Will be KeyDerivation{} if invalid or no tx_public_key
	std::vector<PublicKey> address_public_keys;
	std::vector<SecretKey> output_secret_hashes;

	PreparedWalletTransaction() = default;
	PreparedWalletTransaction(
	    TransactionPrefix &&tx, const Wallet::OutputHandler &o_handler, const SecretKey &view_secret_key);
	PreparedWalletTransaction(
	    Transaction &&tx, const Wallet::OutputHandler &o_handler, const SecretKey &view_secret_key);
};

struct PreparedWalletBlock {
	BlockTemplate header;
	PreparedWalletTransaction base_transaction;
	Hash base_transaction_hash;
	std::vector<PreparedWalletTransaction> transactions;
	PreparedWalletBlock() = default;
	PreparedWalletBlock(BlockTemplate &&bc_header, std::vector<TransactionPrefix> &&raw_transactions,
	    Hash base_transaction_hash, const Wallet::OutputHandler &o_handler, const SecretKey &view_secret_key);
};

class WalletPreparatorMulticore {
	std::vector<std::thread> threads;
	std::mutex mu;
	std::condition_variable have_work;
	std::condition_variable result_ready;
	bool quit = false;

	std::map<Height, PreparedWalletBlock> prepared_blocks;
	api::cnd::SyncBlocks::Response work;
	int work_counter = 0;
	Wallet::OutputHandler m_o_handler;
	SecretKey m_view_secret_key;
	void thread_run();

public:
	WalletPreparatorMulticore();
	~WalletPreparatorMulticore();
	void cancel_work();
	void start_work(const api::cnd::SyncBlocks::Response &new_work, Wallet::OutputHandler &&o_handler,
	    const SecretKey &view_secret_key);
	PreparedWalletBlock get_ready_work(Height height);
};
}  // namespace cn
