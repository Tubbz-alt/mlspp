#pragma once

#include "common.h"
#include "crypto.h"
#include "roster.h"
#include "tls_syntax.h"
#include "tree_math.h"
#include <iosfwd>
#include <optional>

namespace mls {

class RatchetTreeNode : public CipherAware
{
public:
  RatchetTreeNode(CipherSuite suite);
  RatchetTreeNode(const RatchetTreeNode& other) = default;
  RatchetTreeNode& operator=(const RatchetTreeNode& other);

  RatchetTreeNode(CipherSuite suite, const bytes& secret);
  RatchetTreeNode(const DHPrivateKey& priv);
  RatchetTreeNode(const DHPublicKey& pub);

  bool public_equal(const RatchetTreeNode& other) const;
  const std::optional<bytes>& secret() const;
  const std::optional<DHPrivateKey>& private_key() const;
  const DHPublicKey& public_key() const;
  const std::optional<Credential>& credential() const;

  void merge(const RatchetTreeNode& other);
  void set_credential(const Credential& cred);

private:
  std::optional<bytes> _secret;
  std::optional<DHPrivateKey> _priv;
  DHPublicKey _pub;

  // A credential is populated iff this is a leaf node
  tls::optional<Credential> _cred;

  friend RatchetTreeNode operator+(const RatchetTreeNode& lhs,
                                   const RatchetTreeNode& rhs);
  friend bool operator==(const RatchetTreeNode& lhs,
                         const RatchetTreeNode& rhs);
  friend bool operator!=(const RatchetTreeNode& lhs,
                         const RatchetTreeNode& rhs);
  friend std::ostream& operator<<(std::ostream& out,
                                  const RatchetTreeNode& node);
  friend tls::ostream& operator<<(tls::ostream& out,
                                  const RatchetTreeNode& obj);
  friend tls::istream& operator>>(tls::istream& in, RatchetTreeNode& obj);
};

// XXX(rlb@ipv.sx): We have to subclass optional<T> in order to
// ensure that nodes are populated with blank values on unmarshal.
// Otherwise, `*opt` will access uninitialized memory.
struct OptionalRatchetTreeNode
  : public tls::variant_optional<RatchetTreeNode, CipherSuite>
{
  typedef tls::variant_optional<RatchetTreeNode, CipherSuite> parent;
  using parent::parent;

  OptionalRatchetTreeNode(CipherSuite suite, const bytes& secret)
    : parent(RatchetTreeNode(suite, secret))
  {}

  bool blank() const;
  const bytes& hash() const;

  void merge(const RatchetTreeNode& other);
  void set_leaf_hash(CipherSuite suite);
  void set_hash(CipherSuite suite,
                const OptionalRatchetTreeNode& left,
                const OptionalRatchetTreeNode& right);

private:
  bytes _hash;
};

struct RatchetTreeNodeVector
  : public tls::variant_vector<OptionalRatchetTreeNode, CipherSuite, 4>
{
  typedef tls::variant_vector<OptionalRatchetTreeNode, CipherSuite, 4> parent;
  using parent::parent;
  using parent::operator[];

  OptionalRatchetTreeNode& operator[](const NodeIndex index);
  const OptionalRatchetTreeNode& operator[](const NodeIndex index) const;
};

struct RatchetNode;
struct DirectPath;

class RatchetTree : public CipherAware
{
public:
  RatchetTree(CipherSuite suite);
  RatchetTree(CipherSuite suite, const bytes& secret, const Credential& cred);
  RatchetTree(CipherSuite suite,
              const std::vector<bytes>& secrets,
              const std::vector<Credential>& creds);

  struct MergeInfo
  {
    std::vector<DHPublicKey> public_keys;
    std::vector<bytes> secrets;
  };

  DirectPath encrypt(LeafIndex from, const bytes& leaf) const;
  MergeInfo decrypt(LeafIndex from, const DirectPath& path) const;
  void merge_path(LeafIndex from, const MergeInfo& info);

  void add_leaf(LeafIndex index,
                const DHPublicKey& pub,
                const Credential& cred);
  void add_leaf(LeafIndex index,
                const bytes& leaf_secret,
                const Credential& cred);
  void blank_path(LeafIndex index);
  void set_path(LeafIndex index, const bytes& leaf);

  const Credential& get_credential(LeafIndex index) const;

  LeafCount leaf_span() const;
  void truncate(LeafCount leaves);

  uint32_t size() const;
  bool occupied(LeafIndex index) const;
  bytes root_secret() const;
  bytes root_hash() const;
  bool check_invariant(LeafIndex from) const;

protected:
  RatchetTreeNodeVector _nodes;
  size_t _secret_size;

  NodeCount node_size() const;
  RatchetTreeNode new_node(const bytes& path_secret) const;
  bytes path_step(const bytes& path_secret) const;
  bytes node_step(const bytes& path_secret) const;

  void add_leaf_inner(LeafIndex index, const RatchetTreeNode& node_val);
  void set_hash(NodeIndex index);
  void set_hash_path(LeafIndex index);
  void set_hash_all(NodeIndex index);

  friend bool operator==(const RatchetTree& lhs, const RatchetTree& rhs);
  friend std::ostream& operator<<(std::ostream& out, const RatchetTree& obj);
  friend tls::ostream& operator<<(tls::ostream& out, const RatchetTree& obj);
  friend tls::istream& operator>>(tls::istream& in, RatchetTree& obj);
};

namespace test {

// Enable tests to see the internals of the tree
class TestRatchetTree : public RatchetTree
{
public:
  using RatchetTree::RatchetTree;
  const RatchetTreeNodeVector& nodes() const;
};

} // namespace test

} // namespace mls
