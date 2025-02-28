// Copyright (c) 2016 Sandstorm Development Group, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#pragma once
// This file implements TLS (aka SSL) encrypted networking. It is actually a wrapper, currently
// around OpenSSL / BoringSSL / LibreSSL, but the interface is intended to remain
// implementation-agnostic.
//
// Unlike OpenSSL's API, the API defined in this file is intended to be hard to use wrong. Good
// ciphers and settings are used by default. Certificates validation is performed automatically
// and cannot be bypassed.

#include <zc/async/async-io.h>

ZC_BEGIN_HEADER

namespace zc {

class TlsPrivateKey;
class TlsCertificate;
struct TlsKeypair;
class TlsSniCallback;
class TlsConnection;

enum class TlsVersion {
  SSL_3,    // avoid; cryptographically broken
  TLS_1_0,  // avoid; cryptographically weak
  TLS_1_1,  // avoid; cryptographically weak
  TLS_1_2,
  TLS_1_3
};

using TlsErrorHandler = zc::Function<void(zc::Exception&&)>;
// Use a simple zc::Function for handling errors during parallel accept().

class TlsContext : public zc::SecureNetworkWrapper {
  // TLS system. Allocate one of these, configure it with the proper keys and certificates (or
  // use the defaults), and then use it to wrap the standard ZC network interfaces in
  // implementations that transparently use TLS.

public:
  struct Options {
    Options();
    // Initializes all values to reasonable defaults.

    ZC_DISALLOW_COPY(Options);
    Options(Options&&) = default;
    Options& operator=(Options&&) = default;
    // Options is a move-only value type.

    bool useSystemTrustStore;
    // Whether or not to trust the system's default trust store. Default: true.

    bool verifyClients;
    // If true, when acting as a server, require the client to present a certificate. The
    // certificate must be signed by one of the trusted CAs, otherwise the client will be rejected.
    // (Typically you should set `useSystemTrustStore` false when using this flag, and specify
    // your specific trusted CAs in `trustedCertificates`.)
    // Default: false

    zc::ArrayPtr<const TlsCertificate> trustedCertificates;
    // Additional certificates which should be trusted. Default: none.

    TlsVersion minVersion;
    // Minimum version. Defaults to minimum version that hasn't been cryptographically broken.
    // If you override this, consider doing:
    //
    //     options.minVersion = zc::max(myVersion, options.minVersion);

    zc::StringPtr cipherList;
    // OpenSSL cipher list string. The default is a curated list designed to be compatible with
    // almost all software in current use (specifically, based on Mozilla's "intermediate"
    // recommendations). The defaults will change in future versions of this library to account
    // for the latest cryptanalysis.
    //
    // Generally you should only specify your own `cipherList` if:
    // - You have extreme backwards-compatibility needs and wish to enable obsolete and/or broken
    //   algorithms.
    // - You need quickly to disable an algorithm recently discovered to be broken.

    zc::Maybe<zc::StringPtr> curveList;
    // Sets the preferred curves (Groups in TLS 1.3), by default this is not set. Similar to the
    // cipher list, this is a colon separated list of human readable names or NIDs.
    // https://boringssl.googlesource.com/boringssl/+/refs/heads/master/include/openssl/nid.h

    zc::Maybe<const TlsKeypair&> defaultKeypair;
    // Default keypair to use for all connections. Required for servers; optional for clients.

    zc::Maybe<TlsSniCallback&> sniCallback;
    // Callback that can be used to choose a different key/certificate based on the specific
    // hostname requested by the client.

    zc::Maybe<zc::Timer&> timer;
    // The timer used for `acceptTimeout` below.

    zc::Maybe<zc::Duration> acceptTimeout;
    // Timeout applied to accepting a new TLS connection. `timer` is required if this is set.

    zc::Maybe<TlsErrorHandler> acceptErrorHandler;
    // Error handler used for TLS accept errors.
  };

  TlsContext(Options options = Options());
  ~TlsContext() noexcept(false);
  ZC_DISALLOW_COPY_AND_MOVE(TlsContext);

  zc::Promise<zc::Own<zc::AsyncIoStream>> wrapServer(zc::Own<zc::AsyncIoStream> stream) override;
  // Upgrade a regular network stream to TLS and begin the initial handshake as the server. The
  // returned promise resolves when the handshake has completed successfully.

  zc::Promise<zc::Own<zc::AsyncIoStream>> wrapClient(zc::Own<zc::AsyncIoStream> stream,
                                                     zc::StringPtr expectedServerHostname) override;
  // Upgrade a regular network stream to TLS and begin the initial handshake as a client. The
  // returned promise resolves when the handshake has completed successfully, including validating
  // the server's certificate.
  //
  // You must specify the server's hostname. This is used for two purposes:
  // 1. It is sent to the server in the initial handshake via the TLS SNI extension, so that a
  //    server serving multiple hosts knows which certificate to use.
  // 2. The server's certificate is validated against this hostname. If validation fails, the
  //    promise returned by wrapClient() will be broken; you'll never get a stream.

  zc::Promise<zc::AuthenticatedStream> wrapServer(zc::AuthenticatedStream stream) override;
  zc::Promise<zc::AuthenticatedStream> wrapClient(zc::AuthenticatedStream stream,
                                                  zc::StringPtr expectedServerHostname) override;
  // Like wrapServer() and wrapClient(), but also produces information about the peer's
  // certificate (if any). The returned `peerIdentity` will be a `TlsPeerIdentity`.

  zc::Own<zc::ConnectionReceiver> wrapPort(zc::Own<zc::ConnectionReceiver> port) override;
  // Upgrade a ConnectionReceiver to one that automatically upgrades all accepted connections to
  // TLS (acting as the server).

  zc::Own<zc::NetworkAddress> wrapAddress(zc::Own<zc::NetworkAddress> address,
                                          zc::StringPtr expectedServerHostname) override;
  // Upgrade a NetworkAddress to one that automatically upgrades all connections to TLS, acting
  // as the client when `connect()` is called or the server if `listen()` is called.
  // `connect()` will athenticate the server as `expectedServerHostname`.

  zc::Own<zc::Network> wrapNetwork(zc::Network& network) override;
  // Upgrade a Network to one that automatically upgrades all connections to TLS. The network will
  // only accept addresses of the form "hostname" and "hostname:port" (it does not accept raw IP
  // addresses). It will automatically use SNI and verify certificates based on these hostnames.

private:
  void* ctx;  // actually type SSL_CTX, but we don't want to #include the OpenSSL headers here
  zc::Maybe<zc::Timer&> timer;
  zc::Maybe<zc::Duration> acceptTimeout;
  zc::Maybe<TlsErrorHandler> acceptErrorHandler;

  struct SniCallback;
};

class TlsPrivateKey {
  // A private key suitable for use in a TLS server.

public:
  TlsPrivateKey(zc::ArrayPtr<const byte> asn1);
  // Parse a single binary (ASN1) private key. Supports PKCS8 keys as well as "traditional format"
  // RSA and DSA keys. Does not accept encrypted keys; it is the caller's responsibility to
  // decrypt.

  TlsPrivateKey(zc::StringPtr pem, zc::Maybe<zc::StringPtr> password = zc::none);
  // Parse a single PEM-encoded private key. Supports PKCS8 keys as well as "traditional format"
  // RSA and DSA keys. A password may optionally be provided and will be used if the key is
  // encrypted.

  ~TlsPrivateKey() noexcept(false);

  TlsPrivateKey(const TlsPrivateKey& other);
  TlsPrivateKey& operator=(const TlsPrivateKey& other);
  // Copy-by-refcount.

  inline TlsPrivateKey(TlsPrivateKey&& other) : pkey(other.pkey) { other.pkey = nullptr; }
  inline TlsPrivateKey& operator=(TlsPrivateKey&& other) {
    pkey = other.pkey;
    other.pkey = nullptr;
    return *this;
  }

private:
  void* pkey;  // actually type EVP_PKEY*

  friend class TlsContext;

  static int passwordCallback(char* buf, int size, int rwflag, void* u);
};

class TlsCertificate {
  // A TLS certificate, possibly with chained intermediate certificates.

public:
  TlsCertificate(zc::ArrayPtr<const byte> asn1);
  // Parse a single binary (ASN1) X509 certificate.

  TlsCertificate(zc::ArrayPtr<const zc::ArrayPtr<const byte>> asn1);
  // Parse a chain of binary (ASN1) X509 certificates.

  TlsCertificate(zc::StringPtr pem);
  // Parse a PEM-encode X509 certificate or certificate chain. A chain can be constructed by
  // concatenating multiple PEM-encoded certificates, starting with the leaf certificate.

  ~TlsCertificate() noexcept(false);

  TlsCertificate(const TlsCertificate& other);
  TlsCertificate& operator=(const TlsCertificate& other);
  // Copy-by-refcount.

  inline TlsCertificate(TlsCertificate&& other) {
    memcpy(chain, other.chain, sizeof(chain));
    memset(other.chain, 0, sizeof(chain));
  }
  inline TlsCertificate& operator=(TlsCertificate&& other) {
    memcpy(chain, other.chain, sizeof(chain));
    memset(other.chain, 0, sizeof(chain));
    return *this;
  }

private:
  void* chain[10];
  // Actually type X509*[10].
  //
  // Note that OpenSSL has a default maximum cert chain length of 10. Although configurable at
  // runtime, you'd actually have to convince the _peer_ to reconfigure, which is unlikely except
  // in specific use cases. So to avoid excess allocations we just assume a max of 10 certs.
  //
  // If this proves to be a problem, we should maybe use STACK_OF(X509) here, but stacks are not
  // refcounted -- the X509_chain_up_ref() function actually allocates a new stack and uprefs all
  // the certs.

  friend class TlsContext;
};

struct TlsKeypair {
  // A pair of a private key and a certificate, for use by a server.

  TlsPrivateKey privateKey;
  TlsCertificate certificate;
};

class TlsSniCallback {
  // Callback object to implement Server Name Indication, in which the server is able to decide
  // what key and certificate to use based on the hostname that the client is requesting.
  //
  // TODO(someday): Currently this callback is synchronous, because the OpenSSL API seems to be
  //   synchronous. Other people (e.g. Node) have figured out how to do it asynchronously, but
  //   it's unclear to me if and how this is possible while using the OpenSSL APIs. It looks like
  //   Node may be manually parsing the ClientHello message rather than relying on OpenSSL. We
  //   could do that but it's too much work for today.

public:
  virtual zc::Maybe<TlsKeypair> getKey(zc::StringPtr hostname) = 0;
  // Get the key to use for `hostname`. Null return means use the default from
  // TlsContext::Options::defaultKeypair.
};

class TlsPeerIdentity final : public zc::PeerIdentity {
public:
  ZC_DISALLOW_COPY_AND_MOVE(TlsPeerIdentity);
  ~TlsPeerIdentity() noexcept(false);

  zc::String toString() override;

  zc::PeerIdentity& getNetworkIdentity() { return *inner; }
  // Gets the PeerIdentity of the underlying network connection.

  bool hasCertificate() { return cert != nullptr; }
  // Did the peer even present a (trusted) certificate? Servers must always present certificates.
  // Clients need only present certificates when the `verifyClients` option is enabled.
  //
  // Methods of this class that read details of the certificate will throw exceptions when no
  // certificate was presented. We don't have them return `Maybe`s because most applications know
  // in advance whether or not a certificate should be present, so it would lead to lots of
  // `ZC_ASSERT_NONNULL`...

  zc::String getCommonName();
  // Get the authenticated common name from the certificate.

  bool matchesHostname(zc::StringPtr hostname);
  // Check if the certificate authenticates the given hostname, considering wildcards and SAN
  // extensions. If no certificate was provided, always returns false.

  // TODO(someday): Methods for other things. Match hostnames (i.e. evaluate wildcards and SAN)?
  //   Key fingerprint? Other certificate fields?

private:
  void* cert;  // actually type X509*, but we don't want to #include the OpenSSL headers here.
  zc::Own<zc::PeerIdentity> inner;

public:  // (not really public, only TlsConnection can call this)
  TlsPeerIdentity(void* cert, zc::Own<zc::PeerIdentity> inner, zc::Badge<TlsConnection>)
      : cert(cert), inner(zc::mv(inner)) {}
};

}  // namespace zc

ZC_END_HEADER
