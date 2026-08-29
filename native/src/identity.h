#pragma once

#include <filesystem>
#include <string>

#include <openssl/types.h>

namespace eclipse {

class Identity {
public:
    explicit Identity(const std::filesystem::path& dataPath);
    ~Identity();

    Identity(const Identity&) = delete;
    Identity& operator=(const Identity&) = delete;

    [[nodiscard]] const std::string& certificatePem() const;
    [[nodiscard]] const std::filesystem::path& certificatePath() const;
    [[nodiscard]] const std::filesystem::path& runtimeKeyPath() const;
    [[nodiscard]] EVP_PKEY* privateKey() const;

private:
    void generate();
    void load();
    void writeRuntimeKey() const;

    std::filesystem::path certificatePath_;
    std::filesystem::path protectedKeyPath_;
    std::filesystem::path runtimeKeyPath_;
    std::string certificatePem_;
    std::string privateKeyPem_;
    EVP_PKEY* privateKey_ = nullptr;
};

}  // namespace eclipse
