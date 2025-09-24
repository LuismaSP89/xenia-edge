group("third_party")
project("wolfssl")
  uuid("a34e3e3f-f08b-4718-8f96-cfc347f07931")
  kind("StaticLib")
  language("C")
  links({

  })
  filter { "platforms:Windows" }
    defines({
      "WOLFSSL_LIB",
      "WOLFSSL_USER_SETTINGS"
    })
    includedirs({
      "wolfssl",
      "wolfssl/IDE/WIN"
    })

  filter { "platforms:Linux" }
    defines({
      "WOLFSSL_LIB",
      -- Core defines
      "USE_FAST_MATH",
      "TFM_TIMING_RESISTANT",
      "ECC_TIMING_RESISTANT",
      "WC_RSA_BLINDING",

      -- Hash algorithms
      "WOLFSSL_SHA512",
      "WOLFSSL_SHA384",
      "WOLFSSL_SHA224",
      "NO_MD4",

      -- Cipher suites
      "HAVE_AESGCM",
      "WOLFSSL_AES_COUNTER",
      "WOLFSSL_AES_DIRECT",
      "HAVE_AESCCM",
      "HAVE_POLY1305",
      "HAVE_CHACHA",
      "NO_RC4",
      "NO_DES3",

      -- ECC and crypto
      "HAVE_ECC",
      "NO_DSA",
      "HAVE_CURVE25519",
      "HAVE_ED25519",

      -- TLS features
      "HAVE_TLS_EXTENSIONS",
      "HAVE_SUPPORTED_CURVES",
      "HAVE_EXTENDED_MASTER",
      "HAVE_SNI",
      "HAVE_ALPN",
      "NO_PSK",
      "WOLFSSL_CERT_GEN",
      "WOLFSSL_CERT_REQ",
      "WOLFSSL_CERT_EXT",
      "OPENSSL_EXTRA",
      "OPENSSL_ALL",
      "WOLFSSL_OPENVPN",
      "HAVE_OCSP",
      "HAVE_CRL",
      "WOLFSSL_ALT_CERT_CHAINS",
      "WOLFSSL_TRUST_PEER_CERT",
      "HAVE_EX_DATA",

      -- Session and context
      "HAVE_SESSION_TICKET",
      "WOLFSSL_SESS_CACHE",
      "SESSION_CERTS",
      "KEEP_PEER_CERT",

      -- Platform specific
      "WOLFSSL_LINUX",
      "WOLFSSL_STATIC_RSA",
      "WOLFSSL_DH_CONST",
      "DH_CONST",

      -- Additional compatibility
      "WOLFSSL_MULTI_ATTRIB",
      "HAVE_CERTIFICATE_STATUS_REQUEST",
      "HAVE_CERTIFICATE_STATUS_REQUEST_V2",
      "WOLFSSL_SIGNER_DER_CERT",
      "NO_WOLFSSL_STUB"
    })
    includedirs({
      "wolfssl",
    })

  filter {}
  files({
    "wolfcrypt/src/*.c",

    "wolfssl/src/crl.c",
    "wolfssl/src/dtls.c",
    "wolfssl/src/dtls13.c",
    "wolfssl/src/internal.c",
    "wolfssl/src/keys.c",
    "wolfssl/src/ocsp.c",
    "wolfssl/src/ssl.c",
    "wolfssl/src/tls.c",
    "wolfssl/src/tls13.c",
    "wolfssl/src/wolfio.c",
  })
