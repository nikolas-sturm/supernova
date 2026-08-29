#include "identity.h"

#include <fstream>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <openssl/buffer.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#undef X509_NAME
#endif

namespace eclipse {
namespace {
template <typename Type, auto FreeFunction>
struct OpenSslDeleter {
    void operator()(Type* value) const {
        if (value != nullptr) {
            static_cast<void>(FreeFunction(value));
        }
    }
};

template <typename Type, auto FreeFunction>
using OpenSslPointer = std::unique_ptr<Type, OpenSslDeleter<Type, FreeFunction>>;

[[noreturn]] void throwOpenSsl(const std::string& message) {
    char detail[256]{};
    const auto error = ERR_get_error();
    if (error != 0) {
        ERR_error_string_n(error, detail, sizeof(detail));
    }
    throw std::runtime_error(message + (error == 0 ? "" : ": " + std::string{detail}));
}

void requireOpenSsl(int result, const std::string& message) {
    if (result <= 0) {
        throwOpenSsl(message);
    }
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error("Cannot read identity file: " + path.string());
    }
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::vector<unsigned char> readBytes(const std::filesystem::path& path) {
    const auto text = readText(path);
    return {text.begin(), text.end()};
}

void writeBytes(const std::filesystem::path& path, const unsigned char* data, std::size_t size) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        throw std::runtime_error("Cannot write identity file: " + path.string());
    }
    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!output) {
        throw std::runtime_error("Cannot finish identity file: " + path.string());
    }
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
}

std::string bioText(BIO* bio) {
    BUF_MEM* memory = nullptr;
    BIO_get_mem_ptr(bio, &memory);
    if (memory == nullptr || memory->data == nullptr || memory->length == 0) {
        throw std::runtime_error("OpenSSL produced an empty identity value.");
    }
    return {memory->data, memory->length};
}

#ifdef _WIN32
std::vector<unsigned char> protectKey(const std::string& key) {
    DATA_BLOB input{static_cast<DWORD>(key.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(key.data()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"Eclipse GameStream identity", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        throw std::runtime_error("Windows could not protect client identity key.");
    }
    std::vector<unsigned char> protectedValue{output.pbData, output.pbData + output.cbData};
    LocalFree(output.pbData);
    return protectedValue;
}

std::string unprotectKey(const std::vector<unsigned char>& protectedValue) {
    DATA_BLOB input{static_cast<DWORD>(protectedValue.size()),
                    const_cast<BYTE*>(protectedValue.data())};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        throw std::runtime_error(
            "Windows could not unlock client identity key. Restore or remove identity files explicitly.");
    }
    std::string key{reinterpret_cast<const char*>(output.pbData), output.cbData};
    LocalFree(output.pbData);
    return key;
}
#endif
}  // namespace

Identity::Identity(const std::filesystem::path& dataPath)
    : certificatePath_(dataPath / "identity-cert.pem"),
#ifdef _WIN32
      protectedKeyPath_(dataPath / "identity-key.dpapi"),
      runtimeKeyPath_(dataPath / ".identity-key.runtime.pem") {
#else
      protectedKeyPath_(dataPath / "identity-key.pem"),
      runtimeKeyPath_(protectedKeyPath_) {
#endif
    const bool hasCertificate = std::filesystem::exists(certificatePath_);
    const bool hasKey = std::filesystem::exists(protectedKeyPath_);
    if (hasCertificate != hasKey) {
        throw std::runtime_error(
            "Client identity is incomplete. Restore both identity files or remove both to re-pair hosts.");
    }
    if (!hasCertificate) {
        generate();
    }
    load();
    writeRuntimeKey();
}

Identity::~Identity() {
    EVP_PKEY_free(privateKey_);
#ifdef _WIN32
    std::error_code error;
    std::filesystem::remove(runtimeKeyPath_, error);
#endif
}

const std::string& Identity::certificatePem() const {
    return certificatePem_;
}

const std::filesystem::path& Identity::certificatePath() const {
    return certificatePath_;
}

const std::filesystem::path& Identity::runtimeKeyPath() const {
    return runtimeKeyPath_;
}

EVP_PKEY* Identity::privateKey() const {
    return privateKey_;
}

void Identity::generate() {
    OpenSslPointer<EVP_PKEY_CTX, EVP_PKEY_CTX_free> keyContext{
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr)};
    if (!keyContext) {
        throwOpenSsl("Cannot create RSA key context");
    }
    requireOpenSsl(EVP_PKEY_keygen_init(keyContext.get()), "Cannot initialize RSA key generation");
    requireOpenSsl(EVP_PKEY_CTX_set_rsa_keygen_bits(keyContext.get(), 2048),
                   "Cannot configure RSA key size");

    EVP_PKEY* generatedKey = nullptr;
    requireOpenSsl(EVP_PKEY_keygen(keyContext.get(), &generatedKey), "Cannot generate RSA key");
    OpenSslPointer<EVP_PKEY, EVP_PKEY_free> key{generatedKey};
    OpenSslPointer<X509, X509_free> certificate{X509_new()};
    if (!certificate) {
        throwOpenSsl("Cannot create client certificate");
    }

    requireOpenSsl(X509_set_version(certificate.get(), 2), "Cannot set certificate version");
    requireOpenSsl(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 0),
                   "Cannot set certificate serial number");
    if (!X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 60L * 60 * 24 * 365 * 20)) {
        throwOpenSsl("Cannot set certificate lifetime");
    }
    requireOpenSsl(X509_set_pubkey(certificate.get(), key.get()),
                   "Cannot set certificate public key");

    X509_NAME* name = X509_get_subject_name(certificate.get());
    if (name == nullptr) {
        throwOpenSsl("Cannot access certificate subject");
    }
    requireOpenSsl(X509_NAME_add_entry_by_txt(
                       name, "CN", MBSTRING_ASC,
                       reinterpret_cast<const unsigned char*>("NVIDIA GameStream Client"), -1, -1, 0),
                   "Cannot set certificate subject");
    requireOpenSsl(X509_set_issuer_name(certificate.get(), name),
                   "Cannot set certificate issuer");
    requireOpenSsl(X509_sign(certificate.get(), key.get(), EVP_sha256()),
                   "Cannot sign client certificate");

    OpenSslPointer<BIO, BIO_free> certificateBio{BIO_new(BIO_s_mem())};
    OpenSslPointer<BIO, BIO_free> keyBio{BIO_new(BIO_s_mem())};
    if (!certificateBio || !keyBio) {
        throwOpenSsl("Cannot allocate identity serialization buffer");
    }
    requireOpenSsl(PEM_write_bio_X509(certificateBio.get(), certificate.get()),
                   "Cannot serialize client certificate");
    requireOpenSsl(PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0, nullptr,
                                            nullptr),
                   "Cannot serialize client private key");

    const auto certificatePem = bioText(certificateBio.get());
    const auto privateKeyPem = bioText(keyBio.get());
    writeBytes(certificatePath_, reinterpret_cast<const unsigned char*>(certificatePem.data()),
               certificatePem.size());
#ifdef _WIN32
    const auto protectedKey = protectKey(privateKeyPem);
    writeBytes(protectedKeyPath_, protectedKey.data(), protectedKey.size());
#else
    writeBytes(protectedKeyPath_, reinterpret_cast<const unsigned char*>(privateKeyPem.data()),
               privateKeyPem.size());
#endif
}

void Identity::load() {
    certificatePem_ = readText(certificatePath_);
#ifdef _WIN32
    privateKeyPem_ = unprotectKey(readBytes(protectedKeyPath_));
#else
    privateKeyPem_ = readText(protectedKeyPath_);
#endif

    OpenSslPointer<BIO, BIO_free> certificateBio{
        BIO_new_mem_buf(certificatePem_.data(), static_cast<int>(certificatePem_.size()))};
    OpenSslPointer<BIO, BIO_free> keyBio{
        BIO_new_mem_buf(privateKeyPem_.data(), static_cast<int>(privateKeyPem_.size()))};
    if (!certificateBio || !keyBio) {
        throwOpenSsl("Cannot allocate identity parser");
    }
    OpenSslPointer<X509, X509_free> certificate{
        PEM_read_bio_X509(certificateBio.get(), nullptr, nullptr, nullptr)};
    privateKey_ = PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr);
    if (!certificate || privateKey_ == nullptr) {
        throwOpenSsl("Client identity files are unreadable");
    }
    requireOpenSsl(X509_check_private_key(certificate.get(), privateKey_),
                   "Client certificate and private key do not match");
}

void Identity::writeRuntimeKey() const {
#ifdef _WIN32
    std::error_code error;
    std::filesystem::remove(runtimeKeyPath_, error);
    writeBytes(runtimeKeyPath_, reinterpret_cast<const unsigned char*>(privateKeyPem_.data()),
               privateKeyPem_.size());
#endif
}

}  // namespace eclipse
