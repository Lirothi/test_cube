#pragma once

// Package encryption (docs/distribution_build_plan.md §4): AES-256-GCM per block through Windows CNG (bcrypt.dll -- no
// library of our own), the master key split by HKDF-SHA256 into an AES key and a nonce key, and a SYNTHETIC nonce: the
// first 12 bytes of HMAC-SHA256(nonce key, AAD || the bytes encrypted). Different bytes under one key never share a
// nonce, whatever compression level or "store" decision changed between builds, and the same bytes encrypt the same
// way -- the package stays deterministic. The AAD binds a block to its package, path and place: a block moved to
// another place, file or package fails its tag. The key lives in the exe (Shipping: generated/PakKey.g.h, split and
// masked), so this keeps out the curious and generic unpackers, not a determined reverse engineer.

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vfs::pak
{
    struct Key
    {
        std::array<std::uint8_t, 16> guid{};   // which key: in every package it encrypts
        std::array<std::uint8_t, 32> bytes{};  // the master key
    };

    [[nodiscard]] std::string GuidText(const std::array<std::uint8_t, 16>& guid);

    // A new key from the system's random source.
    [[nodiscard]] Key GenerateKey();
    // {"guid": "<32 hex>", "key": "<64 hex>"}.
    bool SaveKeyFile(const Key& key, const std::filesystem::path& file, std::string& error);
    std::optional<Key> LoadKeyFile(const std::filesystem::path& file, std::string& error);
    // A C++ header that rebuilds the key from four XOR-masked parts at run time (Shipping, plan §4).
    bool WriteKeyHeader(const Key& key, const std::filesystem::path& header, std::string& error);
    // The key the build embedded (core/vfs/generated/PakKey.g.h), if it was generated before the build.
    [[nodiscard]] std::optional<Key> EmbeddedKey();

    // The keys a process can open packages with.
    class KeyRing
    {
    public:
        void Add(const Key& key);
        [[nodiscard]] const Key* Find(const std::array<std::uint8_t, 16>& guid) const;
        [[nodiscard]] bool Empty() const noexcept { return keys_.empty(); }

    private:
        std::vector<Key> keys_;
    };

    // One package's cipher: the HKDF sub-keys and the CNG key, duplicated per thread for reads (a CNG key handle is not
    // shared between threads).
    class Cipher
    {
    public:
        explicit Cipher(const Key& key);
        ~Cipher();
        Cipher(const Cipher&) = delete;
        Cipher& operator=(const Cipher&) = delete;

        [[nodiscard]] bool Ok() const noexcept { return ok_; }
        // The synthetic nonce of these bytes under this AAD. False = CNG failed: nothing may be encrypted with it.
        [[nodiscard]] bool Nonce(const std::uint8_t* aad, std::size_t aadBytes, const std::uint8_t* data, std::size_t bytes,
                                 std::uint8_t nonce[12]) const;
        // In place. Encrypt fills the tag; Decrypt checks it (false = the tag does not match: damaged, moved or the
        // wrong key).
        bool Encrypt(const std::uint8_t nonce[12], const std::uint8_t* aad, std::size_t aadBytes, std::uint8_t* data,
                     std::size_t bytes, std::uint8_t tag[16]) const;
        bool Decrypt(const std::uint8_t nonce[12], const std::uint8_t* aad, std::size_t aadBytes, std::uint8_t* data,
                     std::size_t bytes, const std::uint8_t tag[16]) const;

    private:
        void* ThreadKey() const;

        bool ok_ = false;
        std::array<std::uint8_t, 32> encKey_{};   // zeroed once the CNG key holds it
        std::array<std::uint8_t, 32> nonceKey_{}; // zeroed with the cipher
        void* key_ = nullptr;      // BCRYPT_KEY_HANDLE (the original, duplicated per thread)
        std::uint64_t serial_ = 0; // which cipher a thread's duplicate belongs to
    };
}
