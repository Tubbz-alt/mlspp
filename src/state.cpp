#include "state.h"

namespace mls {

///
/// Constructors
///

static const epoch_t zero_epoch{ 0 };

State::State(const bytes& group_id,
             CipherSuite suite,
             const SignaturePrivateKey& identity_priv)
  : _index(0)
  , _identity_priv(identity_priv)
  , _epoch(zero_epoch)
  , _group_id(group_id)
  , _suite(suite)
  , _message_master_secret()
  , _init_secret(zero_bytes(32))
  , _tree(suite, random_bytes(32))
{
  RawKeyCredential cred{ identity_priv.public_key() };
  _roster.add(cred);
}

State::State(const SignaturePrivateKey& identity_priv,
             const bytes& init_secret,
             const Welcome& welcome,
             const Handshake& handshake)
  : _identity_priv(identity_priv)
  , _group_id(welcome.group_id)
  , _suite(welcome.cipher_suite)
  , _epoch(welcome.epoch + 1)
  , _roster(welcome.roster)
  , _tree(welcome.tree)
  , _transcript(welcome.transcript)
  , _index(welcome.tree.size())
  , _init_secret(welcome.init_secret)
{
  if (handshake.operation.type != GroupOperationType::add) {
    throw InvalidParameterError("Incorrect handshake type");
  }

  // XXX(rlb@ipv.sx): Assuming exactly one init key, of the same
  // algorithm.  Should do algorithm negotiation.
  auto add = handshake.operation.add;
  auto identity_key = add.init_key.identity_key;
  if (identity_key != identity_priv.public_key()) {
    throw InvalidParameterError("Group add not targeted for this node");
  }

  // Make sure that the init key for the chosen ciphersuite is the
  // one we sent
  bool init_verified = false;
  for (int i = 0; i < add.init_key.cipher_suites.size(); ++i) {
    auto suite = add.init_key.cipher_suites[i];
    if (suite != _suite) {
      continue;
    }

    auto init_priv = DHPrivateKey::derive(_suite, init_secret);
    auto init_uik = DHPublicKey(_suite, add.init_key.init_keys[i]);

    if (init_uik != init_priv.public_key()) {
      throw ProtocolError("Incorrect init key");
    }

    init_verified = true;
    break;
  }
  if (!init_verified) {
    throw ProtocolError("Selected cipher suite not supported");
  }

  // Initialize shared state
  RawKeyCredential cred{ identity_key };
  _roster.add(cred);
  update_leaf(_index, add.path, welcome.leaf_secret);

  if (!verify(handshake.signer_index, handshake.signature)) {
    throw InvalidParameterError("Handshake signature failed to verify");
  }
}

///
/// Message factories
///

std::pair<Welcome, Handshake>
State::add(const UserInitKey& user_init_key) const
{
  if (!user_init_key.verify()) {
    throw InvalidParameterError("bad signature on user init key");
  }

  // XXX(rlb@ipv.sx): This is all the algorithm negotiation we need
  // for the moment.  When we encrypt the Welcome, we will need to
  // choose the proper DH key to use for the encryption.
  bool cipher_supported = false;
  for (auto suite : user_init_key.cipher_suites) {
    cipher_supported = cipher_supported || (suite == _suite);
  }
  if (!cipher_supported) {
    throw ProtocolError("New member does not support the groups ciphersuite");
  }

  auto leaf_secret = random_bytes(32);
  auto path = _tree.encrypt(_tree.size(), leaf_secret);

  Welcome welcome{ _group_id, _epoch,      _suite,       _roster,
                   _tree,     _transcript, _init_secret, leaf_secret };
  auto add = sign(Add{ path, user_init_key });
  return std::pair<Welcome, Handshake>(welcome, add);
}

Handshake
State::update(const bytes& leaf_secret)
{
  auto path = _tree.encrypt(_index, leaf_secret);
  _cached_leaf_secret = leaf_secret;
  return sign(Update{ path });
}

Handshake
State::remove(uint32_t index) const
{
  auto evict_secret = random_bytes(32);
  auto path = _tree.encrypt(index, evict_secret);
  return sign(Remove{ index, path });
}

///
/// Message handlers
///

State
State::handle(const Handshake& handshake) const
{
  if (handshake.prior_epoch != _epoch) {
    throw InvalidParameterError("Epoch mismatch");
  }

  auto next = handle(handshake.signer_index, handshake.operation);

  if (!next.verify(handshake.signer_index, handshake.signature)) {
    throw InvalidParameterError("Invalid handshake message signature");
  }

  return next;
}

State
State::handle(uint32_t signer_index, const GroupOperation& operation) const
{
  auto next = *this;
  next._epoch = _epoch + 1;

  switch (operation.type) {
    case GroupOperationType::add:
      next.handle(operation.add);
      break;
    case GroupOperationType::update:
      next.handle(signer_index, operation.update);
      break;
    case GroupOperationType::remove:
      next.handle(signer_index, operation.remove);
      break;
  }

  return next;
}

void
State::handle(const Add& add)
{
  // Verify the UserInitKey in the Add message
  if (!add.init_key.verify()) {
    throw InvalidParameterError("Invalid signature on init key in group add");
  }

  // Add the new leaf to the ratchet tree
  // XXX(rlb@ipv.sx): Assumes only one initkey
  auto init_key = add.init_key.init_keys[0];
  auto identity_key = add.init_key.identity_key;

  auto tree_size = _tree.size();
  auto path = add.path;
  _tree.decrypt(tree_size, path);
  _tree.merge(tree_size, path);

  // Add to the roster
  RawKeyCredential cred{ identity_key };
  _roster.add(cred);

  // Update symmetric state
  auto update_secret = *(_tree.root().secret());
  derive_epoch_keys(update_secret);
}

void
State::handle(uint32_t index, const Update& update)
{
  optional<bytes> leaf_secret = std::experimental::nullopt;
  if (index == _index) {
    if (_cached_leaf_secret.size() == 0) {
      throw InvalidParameterError("Got self-update without generating one");
    }

    leaf_secret = _cached_leaf_secret;
    _cached_leaf_secret.resize(0);
  }

  update_leaf(index, update.path, leaf_secret);
}

void
State::handle(uint32_t index, const Remove& remove)
{
  auto leaf_secret = std::experimental::nullopt;
  update_leaf(remove.removed, remove.path, leaf_secret);

  _roster.copy(remove.removed, index);
}

///
/// Inner logic and convenience functions
///

bool
operator==(const State& lhs, const State& rhs)
{
  auto epoch = (lhs._epoch == rhs._epoch);
  auto group_id = (lhs._group_id == rhs._group_id);
  auto roster = (lhs._roster == rhs._roster);
  auto ratchet_tree = (lhs._tree == rhs._tree);
  auto message_master_secret =
    (lhs._message_master_secret == rhs._message_master_secret);
  auto init_secret = (lhs._init_secret == rhs._init_secret);

  // Uncomment for debug info
  /*
  std::cout << "== == == == ==" << std::endl
            << std::endl
            << "_epoch " << epoch << " " << lhs._epoch << " " << rhs._epoch
            << std::endl
            << "_group_id " << group_id << std::endl
            << "_roster " << roster << std::endl
            << "_tree " << ratchet_tree << std::endl
            << "_message_master_secret " << message_master_secret << std::endl
            << "_init_secret " << init_secret << std::endl
  */

  return epoch && group_id && roster && ratchet_tree && message_master_secret &&
         init_secret;
}

bool
operator!=(const State& lhs, const State& rhs)
{
  return !(lhs == rhs);
}

void
State::update_leaf(uint32_t index,
                   const RatchetPath& path,
                   const optional<bytes>& leaf_secret)
{
  if (leaf_secret) {
    _tree.set_leaf(index, *leaf_secret);
  } else {
    auto temp_path = path;
    _tree.decrypt(index, temp_path);
    _tree.merge(index, temp_path);
  }

  auto update_secret = *(_tree.root().secret());
  derive_epoch_keys(update_secret);
}

void
State::derive_epoch_keys(const bytes& update_secret)
{
  auto epoch_secret = hkdf_extract(_suite, _init_secret, update_secret);
  _message_master_secret = derive_secret(
    _suite, epoch_secret, "msg", *this, Digest(_suite).output_size());
  _init_secret = derive_secret(
    _suite, epoch_secret, "init", *this, Digest(_suite).output_size());
}

Handshake
State::sign(const GroupOperation& operation) const
{
  auto next = handle(_index, operation);
  auto tbs = tls::marshal(next);
  auto sig = _identity_priv.sign(tbs);
  return Handshake{ _epoch, operation, _index, sig };
}

bool
State::verify(uint32_t signer_index, const bytes& signature) const
{
  auto tbs = tls::marshal(*this);
  auto pub = _roster.get(signer_index).public_key();
  return pub.verify(tbs, signature);
}

// struct {
//   opaque group_id<0..255>;
//   uint32 epoch;
//   Credential roster<1..2^24-1>;
//   PublicKey tree<1..2^24-1>;
//   GroupOperation transcript<0..2^24-1>;
// } GroupState;
tls::ostream&
operator<<(tls::ostream& out, const State& obj)
{
  return out << obj._group_id << obj._epoch << obj._roster << obj._tree
             << obj._transcript;
}

InitialGroupInfo
create_group(const bytes& group_id,
             const std::vector<CipherSuite> supported_ciphersuites,
             const SignaturePrivateKey& identity_priv,
             const UserInitKey& user_init_key)
{
  // Negotiate a ciphersuite with the other party
  CipherSuite suite;
  auto selected = false;
  for (auto my_suite : supported_ciphersuites) {
    for (auto other_suite : user_init_key.cipher_suites) {
      if (my_suite == other_suite) {
        selected = true;
        suite = my_suite;
        break;
      }
    }

    if (selected) {
      break;
    }
  }

  auto state = State{ group_id, suite, identity_priv };
  auto welcome_add = state.add(user_init_key);
  state.handle(welcome_add.second);

  return InitialGroupInfo(state, welcome_add);
}

} // namespace mls
