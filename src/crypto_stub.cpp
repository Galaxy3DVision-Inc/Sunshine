#include "crypto.h"
#include <string>
#include <sys/timeb.h>

extern "C" {
    // x265 UCRT Time/File Stubs
    int __imp_fseeko64(void* stream, long long offset, int origin) { return 0; }
    void ftime64(struct __timeb64* timeb) {}
    void _ftime64(struct __timeb64* timeb) {}

    // OpenSSL Stubs
    void EVP_MD_CTX_free(EVP_MD_CTX*) {}
    EVP_MD_CTX* EVP_MD_CTX_new(void) { return nullptr; }

    int EVP_DigestInit_ex(EVP_MD_CTX*, const EVP_MD*, ENGINE*) { return 1; }
    int EVP_DigestUpdate(EVP_MD_CTX*, const void*, size_t) { return 1; }
    int EVP_DigestFinal_ex(EVP_MD_CTX*, unsigned char*, unsigned int*) { return 1; }
    
    const EVP_MD* EVP_md5(void) { return nullptr; }
    const EVP_MD* EVP_sha256(void) { return nullptr; }

    void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX*) {}
    const char* ERR_reason_error_string(unsigned long) { return ""; }
    const char* ERR_lib_error_string(unsigned long) { return ""; }
}

namespace http {
    void save_user_creds(std::string const&, std::string const&, std::string const&, bool) {}
}

namespace rtsp_stream {
    void launch_session_clear(unsigned int) {}
}

namespace crypto {
    void md_ctx_destroy(EVP_MD_CTX*) {}

    namespace cipher {
        gcm_t::gcm_t(const crypto::aes_t &key, bool padding) {}
        int gcm_t::encrypt(const std::string_view &plaintext, std::uint8_t *tag, std::uint8_t *ciphertext, aes_t *iv) { return 0; }
        int gcm_t::encrypt(const std::string_view &plaintext, std::uint8_t *tagged_cipher, aes_t *iv) { return 0; }
        int gcm_t::decrypt(const std::string_view &cipher, std::vector<std::uint8_t> &plaintext, aes_t *iv) { return 0; }

        cbc_t::cbc_t(const crypto::aes_t &key, bool padding) {}
        int cbc_t::encrypt(const std::string_view &plaintext, std::uint8_t *cipher, aes_t *iv) { return 0; }
    }
}
