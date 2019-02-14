// Copyright (c) 2012-2018, The CryptoNote developers, The Bytecoin developers.
// Licensed under the GNU Lesser General Public License. See LICENSE for details.

#include "Emulator.hpp"
#include <ctime>
#include <iostream>
#include "Core/TransactionBuilder.hpp"
#include "CryptoNote.hpp"
#include "common/BIPs.hpp"
#include "common/Invariant.hpp"
#include "common/MemoryStreams.hpp"
#include "common/StringTools.hpp"
#include "common/Varint.hpp"
#include "crypto/bernstein/crypto-ops.h"
#include "crypto/crypto_helpers.hpp"

static const bool debug_print = true;

using namespace cn::hardware;
using namespace crypto;
using namespace common;

static Hash derive_from_seed(const Hash &seed, const std::string &append) {
	BinaryArray seed_data = seed.as_binary_array() | as_binary_array(append);
	return crypto::cn_fast_hash(seed_data.data(), seed_data.size());
}

void Emulator::KeccakStream::append(const unsigned char *data, size_t size) { common::append(ba, data, data + size); }
void Emulator::KeccakStream::append(uint64_t a) { common::append(ba, common::get_varint_data(a)); }
void Emulator::KeccakStream::append_byte(uint8_t b) { ba.push_back(b); }
Hash Emulator::KeccakStream::cn_fast_hash() const {
	Hash result = crypto::cn_fast_hash(ba.data(), ba.size());
	if (debug_print)
		std::cout << "KeccakStream hash( " << common::to_hex(ba) << " )= " << result << std::endl;
	return result;
}
SecretKey Emulator::KeccakStream::hash_to_scalar() const { return crypto::hash_to_scalar(ba.data(), ba.size()); }
SecretKey Emulator::KeccakStream::hash_to_scalar64() const {
	return crypto::hash_to_scalar(ba.data(), ba.size());
	/*	SecretKey k1 = crypto::hash_to_scalar64(ba.data(), ba.size());
	    Hash h = crypto::cn_fast_hash(ba.data(), ba.size());
	    Hash h2 = crypto::cn_fast_hash(h.data, sizeof(h.data));
	    SecretKey s_h;
	    sc_reduce32(&s_h, h.data);
	    SecretKey s_h2;
	    sc_reduce32(&s_h2, h2.data);
	    unsigned char ff[32];
	    memset(ff, 0xff, sizeof(ff));
	    SecretKey s_max;
	    SecretKey s_one;
	    sc_reduce32(&s_max, ff);
	    sc_1(&s_one);
	    SecretKey hi = s_h2 * s_max;
	    SecretKey res = hi + s_h;
	    return res;*/
}

PublicKey Emulator::KeccakStream::hash_to_good_point() const {
	return crypto::hash_to_good_point(ba.data(), ba.size());
}

inline bool add_amount(uint64_t &sum, uint64_t amount) {
	if (std::numeric_limits<uint64_t>::max() - amount < sum)
		return false;
	sum += amount;
	return true;
}

Emulator::Emulator(const std::string &mnemonic, std::unique_ptr<HardwareWallet> &&proxy) : m_proxy(std::move(proxy)) {
	// read m_wallet_key, m_spend_key_base_public_key from device

	m_mnemonics                   = cn::Bip32Key::check_bip39_mnemonic(mnemonic);
	const cn::Bip32Key master_key = cn::Bip32Key::create_master_key(m_mnemonics, std::string());

	const cn::Bip32Key k0 = master_key.derive_key(0x8000002c);
	const cn::Bip32Key k1 = k0.derive_key(0x800000cc);
	const cn::Bip32Key k2 = k1.derive_key(0x80000000 + uint32_t(m_address_type));
	const cn::Bip32Key k3 = k2.derive_key(0);
	const cn::Bip32Key k4 = k3.derive_key(0);
	const Hash m_seed     = crypto::cn_fast_hash(k4.get_priv_key().data(), k4.get_priv_key().size());

	m_tx_derivation_seed        = derive_from_seed(m_seed, "tx_derivation");
	BinaryArray vk_data         = m_seed.as_binary_array() | as_binary_array("view_key");
	m_view_secret_key           = crypto::hash_to_scalar(vk_data.data(), vk_data.size());
	BinaryArray ak_data         = m_seed.as_binary_array() | as_binary_array("audit_key_base");
	m_audit_key_base_secret_key = crypto::hash_to_scalar(ak_data.data(), ak_data.size());
	BinaryArray sk_data         = m_seed.as_binary_array() | as_binary_array("spend_key");
	m_spend_secret_key          = crypto::hash_to_scalar(sk_data.data(), sk_data.size());

	m_sH = crypto::A_mul_b(crypto::get_H(), m_spend_secret_key);

	invariant(crypto::secret_key_to_public_key(m_view_secret_key, &m_view_public_key), "");
	PublicKey A;
	invariant(crypto::secret_key_to_public_key(m_audit_key_base_secret_key, &A), "");
	m_A_plus_sH       = crypto::A_plus_B(A, m_sH);
	m_v_mul_A_plus_sH = A_mul_b(m_A_plus_sH, m_view_secret_key);  // for hw debug only

	m_wallet_key = derive_from_seed(m_seed, "wallet_key");

	if (debug_print) {
		std::cout << "bip44 child private key " << common::to_hex(k4.get_priv_key()) << std::endl;
		std::cout << "m_seed " << m_seed << std::endl;
		std::cout << "m_tx_derivation_seed " << m_tx_derivation_seed << std::endl;
		std::cout << "m_audit_key_base_secret_key " << m_audit_key_base_secret_key << std::endl;
		std::cout << "A " << A << std::endl;
		std::cout << "m_view_secret_key " << m_view_secret_key << std::endl;
		std::cout << "m_view_public_key " << m_view_public_key << std::endl;
		std::cout << "m_spend_secret_key " << m_spend_secret_key << std::endl;
		std::cout << "m_sH " << m_sH << std::endl;
		std::cout << "m_wallet_key " << m_wallet_key << std::endl;
	}
	if (m_proxy) {
		invariant(get_A_plus_SH() == m_proxy->get_A_plus_SH(), "");
		invariant(get_v_mul_A_plus_SH() == m_proxy->get_v_mul_A_plus_SH(), "");
		invariant(get_public_view_key() == m_proxy->get_public_view_key(), "");
		invariant(get_wallet_key() == m_proxy->get_wallet_key(), "");
	}
	test_all_methods();
}

Emulator::~Emulator() {}

std::string Emulator::get_hardware_type() const {
	std::string result = "Emulator";
	if (m_proxy)
		result += " connected to " + m_proxy->get_hardware_type();
	return result + ", mnemonic=" + m_mnemonics;
}

// When we need only secrets
void Emulator::prepare_address(size_t address_index) const {
	if (address_index != last_address_index) {
		last_address_index = address_index;
		last_address_audit_secret_key =
		    crypto::generate_hd_secretkey(m_audit_key_base_secret_key, m_A_plus_sH, address_index);
		std::cout << "HW::prepare_address[" << address_index << "]=" << last_address_audit_secret_key << std::endl;
	}
}

// When we need also public address part
void Emulator::prepare_address(size_t address_index, PublicKey *address_S, PublicKey *address_Sv) const {
	prepare_address(address_index);
	PublicKey last_address_audit_public_key;
	invariant(crypto::secret_key_to_public_key(last_address_audit_secret_key, &last_address_audit_public_key), "");
	*address_S  = crypto::A_plus_B(last_address_audit_public_key, m_sH);
	*address_Sv = crypto::A_mul_b(*address_S, m_view_secret_key);
}

std::vector<PublicKey> Emulator::mul_by_view_secret_key(const std::vector<PublicKey> &output_public_keys) {
	// multiply by m_view_secret_key on device, throw if PublicKey detected to be invalid by device
	std::vector<PublicKey> result(output_public_keys.size());
	for (size_t i = 0; i != result.size(); ++i)
		result.at(i) = crypto::unlinkable_underive_address_S_step1(m_view_secret_key, output_public_keys.at(i));
	if (m_proxy)
		invariant(m_proxy->mul_by_view_secret_key(output_public_keys) == result, "");
	return result;
}

KeyImage Emulator::generate_keyimage(
    const PublicKey &output_public_key, const SecretKey &inv_output_secret_hash, size_t address_index) {
	prepare_address(address_index);
	SecretKey output_secret_key_a = crypto::sc_mul(last_address_audit_secret_key, inv_output_secret_hash);
	auto result                   = crypto::generate_key_image(output_public_key, output_secret_key_a);

	// Experimental code below (3 lines) - do not implement yet in ledger
	SecretKey output_secret_key_s = crypto::sc_mul(m_spend_secret_key, inv_output_secret_hash);
	PublicKey output_public_key2  = crypto::secret_keys_to_public_key(output_secret_key_a, output_secret_key_s);
	invariant(output_public_key == output_public_key2, "");
	// We will ignore output_public_key parameter for security reasons.

	if (m_proxy)
		invariant(m_proxy->generate_keyimage(output_public_key, inv_output_secret_hash, address_index) == result, "");
	return result;
}

void Emulator::generate_output_seed(const Hash &tx_inputs_hash, size_t out_index, PublicKey *output_seed) {
	*output_seed = cn::TransactionBuilder::deterministic_keys_from_seed(
	    tx_inputs_hash, m_tx_derivation_seed, common::get_varint_data(out_index))
	                   .public_key;
	if (m_proxy) {
		PublicKey p_output_seed;
		m_proxy->generate_output_seed(tx_inputs_hash, out_index, &p_output_seed);
		invariant(*output_seed == p_output_seed, "");
	}
}

// TODO - check sig methods for resuls with proxy

void Emulator::sign_start(size_t version, uint64_t ut, size_t inputs_size, size_t outputs_size, size_t extra_size) {
	invariant(inputs_size != 0, "");   // 0 inputs not allowed in consensus
	invariant(outputs_size != 0, "");  // 0 outputs allowed in consensus, we prohibit it to simplify our state machine
	invariant(version != 0, "Wrong transaction version ");
	sign              = SigningState{};
	sign.inputs_size  = inputs_size;
	sign.outputs_size = outputs_size;
	sign.extra_size   = extra_size;
	sign.state        = SigningState::EXPECT_ADD_INPUT;

	sign.tx_prefix_stream.append(version);
	sign.tx_prefix_stream.append(ut);
	sign.tx_prefix_stream.append(inputs_size);
	sign.tx_inputs_stream.append(inputs_size);

	sign.random_seed = Hash{};  // = crypto::rand<Hash>(); - uncomment in final code for full security
	if (m_proxy) {
		m_proxy->sign_start(version, ut, inputs_size, outputs_size, extra_size);
	}
}

SecretKey Emulator::generate_sign_secret(size_t i, const char secret_name[2]) const {
	KeccakStream ks{};
	ks.append(sign.random_seed.data, 32);
	ks.append(m_spend_secret_key.data, 32);
	ks.append_byte(secret_name[0]);
	ks.append_byte(secret_name[1]);
	ks.append(i);
	SecretKey b = ks.hash_to_scalar64();
	if (debug_print)
		std::cout << secret_name[0] << secret_name[1] << "[" << i << "]=" << b << std::endl;
	return b;
}

void Emulator::sign_add_input(uint64_t amount, const std::vector<size_t> &output_indexes,
    SecretKey inv_output_secret_hash, size_t address_index) {
	if (m_proxy) {
		m_proxy->sign_add_input(amount, output_indexes, inv_output_secret_hash, address_index);
	}
	invariant(sign.state == SigningState::EXPECT_ADD_INPUT && sign.inputs_counter < sign.inputs_size, "");
	invariant(add_amount(sign.inputs_amount, amount), "");
	const uint8_t tag = cn::InputKey::type_tag;
	sign.tx_prefix_stream.append_byte(tag);
	sign.tx_inputs_stream.append_byte(tag);
	sign.tx_prefix_stream.append(amount);
	sign.tx_inputs_stream.append(amount);
	sign.tx_prefix_stream.append(output_indexes.size());
	sign.tx_inputs_stream.append(output_indexes.size());
	for (size_t j = 0; j != output_indexes.size(); ++j) {
		sign.tx_prefix_stream.append(output_indexes[j]);
		sign.tx_inputs_stream.append(output_indexes[j]);
	}
	prepare_address(address_index);
	SecretKey output_secret_key_a = crypto::sc_mul(last_address_audit_secret_key, inv_output_secret_hash);
	SecretKey output_secret_key_s = crypto::sc_mul(m_spend_secret_key, inv_output_secret_hash);
	PublicKey output_public_key   = crypto::secret_keys_to_public_key(output_secret_key_a, output_secret_key_s);
	KeyImage key_image            = crypto::generate_key_image(output_public_key, output_secret_key_a);

	sign.tx_prefix_stream.append(key_image.data, 32);
	sign.tx_inputs_stream.append(key_image.data, 32);

	if (++sign.inputs_counter < sign.inputs_size)
		return;
	sign.state          = SigningState::EXPECT_ADD_OUTPUT;
	sign.tx_inputs_hash = sign.tx_inputs_stream.cn_fast_hash();
	sign.tx_prefix_stream.append(sign.outputs_size);
}

void Emulator::add_output_or_change(uint64_t amount, uint8_t dst_address_tag, PublicKey dst_address_s,
    PublicKey dst_address_s_v, PublicKey *public_key, PublicKey *encrypted_secret,
    uint8_t *encrypted_address_type) const {
	KeyPair output_seed_keys = cn::TransactionBuilder::deterministic_keys_from_seed(
	    sign.tx_inputs_hash, m_tx_derivation_seed, common::get_varint_data(sign.outputs_counter));
	if (debug_print)
		std::cout << "output_seed_keys=" << output_seed_keys.public_key << std::endl;
	SecretKey output_secret_scalar;
	PublicKey output_secret_point;
	Hash output_secret_address_type;
	cn::TransactionBuilder::generate_output_secrets(
	    output_seed_keys.public_key, &output_secret_scalar, &output_secret_point, &output_secret_address_type);
	if (debug_print) {
		std::cout << "output_secret_scalar=" << output_secret_scalar << std::endl;
		std::cout << "output_secret_point=" << output_secret_point << std::endl;
		std::cout << "output_secret_address_type=" << output_secret_address_type << std::endl;
	}
	uint8_t output_tag = cn::OutputKey::type_tag;

	*encrypted_address_type = dst_address_tag ^ output_secret_address_type.data[0];
	if (dst_address_tag == cn::AccountAddressSimple::type_tag) {
		*public_key = crypto::linkable_derive_output_public_key(output_secret_scalar, sign.tx_inputs_hash,
		    sign.outputs_counter, dst_address_s, dst_address_s_v, encrypted_secret);
	} else {
		*public_key = crypto::unlinkable_derive_output_public_key(output_secret_point, sign.tx_inputs_hash,
		    sign.outputs_counter, dst_address_s, dst_address_s_v, encrypted_secret);
	}

	sign.tx_prefix_stream.append_byte(output_tag);
	sign.tx_prefix_stream.append(amount);
	sign.tx_prefix_stream.append(public_key->data, 32);
	sign.tx_prefix_stream.append(encrypted_secret->data, 32);
	sign.tx_prefix_stream.append_byte(*encrypted_address_type);
}

void Emulator::sign_add_output(bool change, uint64_t amount, size_t change_address_index, uint8_t dst_address_tag,
    PublicKey dst_address_s, PublicKey dst_address_s_v, PublicKey *public_key, PublicKey *encrypted_secret,
    uint8_t *encrypted_address_type) {
	invariant(sign.state == SigningState::EXPECT_ADD_OUTPUT && sign.outputs_counter < sign.outputs_size, "");
	if (change) {
		invariant(add_amount(sign.change_amount, amount), "");
		PublicKey change_address_s;
		PublicKey change_address_s_v;
		prepare_address(change_address_index, &change_address_s, &change_address_s_v);

		add_output_or_change(amount, cn::AccountAddressUnlinkable::type_tag, change_address_s, change_address_s_v,
		    public_key, encrypted_secret, encrypted_address_type);
	} else {
		if (!sign.dst_address_set) {
			sign.dst_address_set = true;
			sign.dst_address_tag = dst_address_tag;
			sign.dst_address_s   = dst_address_s;
			sign.dst_address_s_v = dst_address_s_v;
		} else {
			invariant(sign.dst_address_tag == dst_address_tag && sign.dst_address_s == dst_address_s &&
			              sign.dst_address_s_v == dst_address_s_v,
			    "");
		}
		invariant(add_amount(sign.dst_amount, amount), "");
		add_output_or_change(amount, sign.dst_address_tag, sign.dst_address_s, sign.dst_address_s_v, public_key,
		    encrypted_secret, encrypted_address_type);
	}
	if (m_proxy) {
		PublicKey encrypted_secret2;
		PublicKey public_key2;
		uint8_t encrypted_address_type2 = 0;
		m_proxy->sign_add_output(change, amount, change_address_index, dst_address_tag, dst_address_s, dst_address_s_v,
		    &public_key2, &encrypted_secret2, &encrypted_address_type2);
		invariant(*public_key == public_key2 && *encrypted_secret == encrypted_secret2 &&
		              *encrypted_address_type == encrypted_address_type2,
		    "");
	}

	if (++sign.outputs_counter < sign.outputs_size)
		return;
	uint64_t outputs_amount = sign.dst_amount;
	invariant(add_amount(outputs_amount, sign.change_amount), "");
	invariant(sign.inputs_amount >= outputs_amount, "");
	uint64_t fee = sign.inputs_amount - outputs_amount;
	std::cout << "fee=" << fee << std::endl;
	// Here, show user 2 dialogs
	// 1. Do you wish to send 'dst_amount' to 'dst_address'?
	// 2. Fee will be 'fee'
	// If both answered yes, continue to signing. Otherwise cancel
	sign.state = SigningState::EXPECT_ADD_EXTRA_CHUNK;
	sign.tx_prefix_stream.append(sign.extra_size);
}

void Emulator::sign_add_extra(const BinaryArray &chunk) {
	if (m_proxy) {
		m_proxy->sign_add_extra(chunk);
	}
	invariant(sign.state == SigningState::EXPECT_ADD_EXTRA_CHUNK, "");
	invariant(sign.extra_counter + chunk.size() <= sign.extra_size, "");  // <= because we call it also for empty extra
	sign.tx_prefix_stream.append(chunk.data(), chunk.size());
	sign.extra_counter += chunk.size();
	if (sign.extra_counter < sign.extra_size)
		return;
	sign.state            = SigningState::EXPECT_STEP_A;
	sign.tx_prefix_hash   = sign.tx_prefix_stream.cn_fast_hash();
	sign.inputs_counter   = 0;
	sign.tx_inputs_stream = KeccakStream{};
	sign.tx_inputs_stream.append(sign.tx_prefix_hash.data, 32);
}

void Emulator::sign_step_a(SecretKey inv_output_secret_hash, size_t address_index, EllipticCurvePoint *sig_p,
    EllipticCurvePoint *x, EllipticCurvePoint *y) {
	if (sign.state == SigningState::EXPECT_STEP_A_MORE_DATA && sign.inputs_counter + 1 < sign.inputs_size) {
		sign.inputs_counter += 1;
		sign.state = SigningState::EXPECT_STEP_A;
	}
	invariant(sign.state == SigningState::EXPECT_STEP_A && sign.inputs_counter < sign.inputs_size, "");

	prepare_address(address_index);
	SecretKey output_secret_key_a = crypto::sc_mul(last_address_audit_secret_key, inv_output_secret_hash);
	SecretKey output_secret_key_s = crypto::sc_mul(m_spend_secret_key, inv_output_secret_hash);
	PublicKey output_public_key   = crypto::secret_keys_to_public_key(output_secret_key_a, output_secret_key_s);
	KeyImage key_image            = crypto::generate_key_image(output_public_key, output_secret_key_a);

	const P3 b_coin_p3(hash_to_good_point_p3(key_image));
	const PublicKey b_coin = to_bytes(b_coin_p3);
	const P3 hash_pubs_sec_p3(hash_to_good_point_p3(output_public_key));
	if (debug_print)
		std::cout << "b_coin[" << sign.inputs_counter << "]=" << b_coin << std::endl;
	const P3 p_p3 = H * output_secret_key_s - b_coin_p3 * output_secret_key_a;
	*sig_p        = to_bytes(p_p3);
	if (debug_print)
		std::cout << "p[" << sign.inputs_counter << "]=" << *sig_p << std::endl;
	sign.tx_inputs_stream.append(sig_p->data, 32);

	const SecretKey ka = generate_sign_secret(sign.inputs_counter, "ka");
	const SecretKey kb = generate_sign_secret(sign.inputs_counter, "kb");
	const SecretKey kc = generate_sign_secret(sign.inputs_counter, "kc");

	const PublicKey z = to_bytes(kb * H + kc * b_coin_p3);
	if (debug_print)
		std::cout << "z[" << sign.inputs_counter << "]=" << z << std::endl;
	sign.tx_inputs_stream.append(z.data, 32);

	const P3 G_plus_B_p3 = P3(G) + b_coin_p3;
	if (debug_print)
		std::cout << "pk[" << sign.inputs_counter << ", my]=" << output_public_key << std::endl;
	*x = to_bytes(ka * G_plus_B_p3);
	if (debug_print)
		std::cout << "x[" << sign.inputs_counter << ", my]=" << *x << std::endl;
	*y = to_bytes(ka * hash_pubs_sec_p3);
	if (debug_print)
		std::cout << "y[" << sign.inputs_counter << ", my]=" << *y << std::endl;

	sign.state = SigningState::EXPECT_STEP_A_MORE_DATA;
	if (m_proxy) {
		EllipticCurvePoint sigp2;
		EllipticCurvePoint x2, y2;

		m_proxy->sign_step_a(inv_output_secret_hash, address_index, &sigp2, &x2, &y2);
		invariant(*sig_p == sigp2 && *x == x2 && *y == y2, "");
	}
}

void Emulator::sign_step_a_more_data(const BinaryArray &data) {
	if (m_proxy) {
		m_proxy->sign_step_a_more_data(data);
	}
	invariant(sign.state == SigningState::EXPECT_STEP_A_MORE_DATA, "");

	sign.tx_inputs_stream.append(data.data(), data.size());
}

crypto::EllipticCurveScalar Emulator::sign_get_c0() {
	invariant(sign.state == SigningState::EXPECT_STEP_A_MORE_DATA && sign.inputs_counter + 1 == sign.inputs_size, "");

	sign.c0 = sign.tx_inputs_stream.hash_to_scalar();
	if (debug_print)
		std::cout << "c0=" << sign.c0 << std::endl;

	sign.state          = SigningState::EXPECT_STEP_B;
	sign.inputs_counter = 0;

	if (m_proxy) {
		auto c02 = m_proxy->sign_get_c0();
		invariant(c02 == sign.c0, "");
	}

	return sign.c0;
}

void Emulator::sign_step_b(SecretKey inv_output_secret_hash, size_t address_index, EllipticCurveScalar my_c,
    EllipticCurveScalar *sig_my_ra, EllipticCurveScalar *sig_rb, EllipticCurveScalar *sig_rc) {
	invariant(sign.state == SigningState::EXPECT_STEP_B && sign.inputs_counter < sign.inputs_size, "");

	prepare_address(address_index);
	SecretKey output_secret_key_a = crypto::sc_mul(last_address_audit_secret_key, inv_output_secret_hash);
	SecretKey output_secret_key_s = crypto::sc_mul(m_spend_secret_key, inv_output_secret_hash);

	const SecretKey ka = generate_sign_secret(sign.inputs_counter, "ka");
	const SecretKey kb = generate_sign_secret(sign.inputs_counter, "kb");
	const SecretKey kc = generate_sign_secret(sign.inputs_counter, "kc");

	*sig_rb    = kb - sign.c0 * output_secret_key_s;
	*sig_rc    = kc + sign.c0 * output_secret_key_a;
	*sig_my_ra = ka - my_c * output_secret_key_a;

	if (debug_print)
		std::cout << "ra[" << sign.inputs_counter << ", my]=" << *sig_my_ra << std::endl;
	if (debug_print)
		std::cout << "rb[" << sign.inputs_counter << "]=" << *sig_rb << std::endl;
	if (debug_print)
		std::cout << "rc[" << sign.inputs_counter << "]=" << *sig_rc << std::endl;

	if (m_proxy) {
		crypto::EllipticCurveScalar sig_my_ra2, rb2, rc2;
		m_proxy->sign_step_b(inv_output_secret_hash, address_index, my_c, &sig_my_ra2, &rb2, &rc2);
		invariant(*sig_my_ra == sig_my_ra2 && *sig_rb == rb2 && *sig_rc == rc2, "");
	}

	if (++sign.inputs_counter < sign.inputs_size)
		return;
	sign.state = SigningState::FINISHED;
}

void Emulator::proof_start(const common::BinaryArray &data) {
	sign             = SigningState{};
	sign.inputs_size = 1;

	sign.tx_prefix_stream.append_byte(0);                    // guard_byte
	sign.tx_prefix_stream.append(data.data(), data.size());  // will require separate sign.state on real device
	sign.tx_prefix_hash = sign.tx_prefix_stream.cn_fast_hash();
	sign.random_seed    = Hash{};  // = crypto::rand<Hash>(); - uncomment in final code for full security

	sign.tx_inputs_stream.append(sign.tx_prefix_hash.data, 32);
	sign.state = SigningState::EXPECT_STEP_A;
	if (m_proxy) {
		m_proxy->proof_start(data);
	}
}

void Emulator::export_view_only(SecretKey *audit_key_base_secret_key, SecretKey *view_secret_key,
    Hash *tx_derivation_seed, Signature *view_secrets_signature) {
	*view_secret_key           = m_view_secret_key;
	*audit_key_base_secret_key = m_audit_key_base_secret_key;
	// Ask user if he wants view wallet to view outgoing addresses
	bool view_outgoing_addresses = true;
	if (view_outgoing_addresses)
		*tx_derivation_seed = m_tx_derivation_seed;
	KeccakStream ks;
	ks.append(audit_key_base_secret_key->data, 32);
	ks.append(m_view_secret_key.data, 32);
	Hash view_secrets_hash = ks.cn_fast_hash();

	*view_secrets_signature = crypto::generate_signature_H(view_secrets_hash, m_sH, m_spend_secret_key);
	if (debug_print) {
		std::cout << "audit_key_base_secret_key=" << *audit_key_base_secret_key << std::endl;
		std::cout << "view_secret_key=" << view_secret_key << std::endl;
		std::cout << "m_sH=" << m_sH << std::endl;
		std::cout << "view_secrets_hash=" << view_secrets_hash << std::endl;
		std::cout << "view_secrets_signature=" << view_secrets_signature->c << view_secrets_signature->r << std::endl;
	}
	if (m_proxy) {
		SecretKey audit_key_base_secret_key2, view_secret_key2;
		Hash tx_derivation_seed2;
		Signature view_secrets_signature2;
		m_proxy->export_view_only(
		    &audit_key_base_secret_key2, &view_secret_key2, &tx_derivation_seed2, &view_secrets_signature2);
		invariant(*audit_key_base_secret_key == audit_key_base_secret_key2 && *view_secret_key == view_secret_key2 &&
		              *tx_derivation_seed == tx_derivation_seed2,
		    "");
		// Cannot compare signatures - they include random component
	}
}
