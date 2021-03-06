#include "hkdf.h"
#include "openssl_common.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdexcept>

namespace hpke {

HKDF
make_hkdf(const Digest& digest)
{
  return HKDF(digest);
}

template<>
const HKDF HKDF::instance<Digest::ID::SHA256> =
  make_hkdf(Digest::get<Digest::ID::SHA256>());

template<>
const HKDF HKDF::instance<Digest::ID::SHA384> =
  make_hkdf(Digest::get<Digest::ID::SHA384>());

template<>
const HKDF HKDF::instance<Digest::ID::SHA512> =
  make_hkdf(Digest::get<Digest::ID::SHA512>());

template<>
const HKDF&
HKDF::get<Digest::ID::SHA256>()
{
  return HKDF::instance<Digest::ID::SHA256>;
}

template<>
const HKDF&
HKDF::get<Digest::ID::SHA384>()
{
  return HKDF::instance<Digest::ID::SHA384>;
}

template<>
const HKDF&
HKDF::get<Digest::ID::SHA512>()
{
  return HKDF::instance<Digest::ID::SHA512>;
}

static KDF::ID
digest_to_kdf(Digest::ID digest_id)
{
  switch (digest_id) {
    case Digest::ID::SHA256:
      return KDF::ID::HKDF_SHA256;
    case Digest::ID::SHA384:
      return KDF::ID::HKDF_SHA384;
    case Digest::ID::SHA512:
      return KDF::ID::HKDF_SHA512;
  }

  throw std::runtime_error("Unsupported algorithm");
}

HKDF::HKDF(const Digest& digest_in)
  : KDF(digest_to_kdf(digest_in.id))
  , digest(digest_in)
{}

bytes
HKDF::extract(const bytes& salt, const bytes& ikm) const
{
  return digest.hmac(salt, ikm);
}

bytes
HKDF::expand(const bytes& prk, const bytes& info, size_t size) const
{
  auto okm = bytes{};
  auto i = uint8_t(0x00);
  auto Ti = bytes{};
  while (okm.size() < size) {
    i += 1;
    auto block = Ti + info + bytes{ i };

    Ti = digest.hmac(prk, block);
    okm += Ti;
  }

  okm.resize(size);
  return okm;
}

size_t
HKDF::hash_size() const
{
  return digest.hash_size();
}

} // namespace hpke
