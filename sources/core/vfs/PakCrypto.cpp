#include "core/vfs/PakCrypto.h"

#include "core/vfs/Vfs.h"
#include "third_party/json/json.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>

#pragma comment(lib, "bcrypt.lib")

// Always there: test_cube.vcxproj writes a "no key" one when it is missing, --pak-key-header a real one. Not
// __has_include: MSBuild does not see a header that was absent at the last compile, so a key written later would never
// reach the exe (the first full build's encrypted packages did not open, 2026-11-13).
#include "core/vfs/generated/PakKey.g.h"
#ifndef TC_PAK_HAS_EMBEDDED_KEY
#define TC_PAK_HAS_EMBEDDED_KEY 0
#endif

namespace vfs::pak
{
    namespace
    {
        bool Succeeded(NTSTATUS status) { return status >= 0; }

        struct Algorithms
        {
            BCRYPT_ALG_HANDLE aes = nullptr;
            BCRYPT_ALG_HANDLE hmac = nullptr;
            bool ok = false;
        };

        const Algorithms& Algs()
        {
            static const Algorithms algs = []
            {
                Algorithms a;
                a.ok = Succeeded(::BCryptOpenAlgorithmProvider(&a.aes, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
                       Succeeded(::BCryptSetProperty(a.aes, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0)) &&
                       Succeeded(::BCryptOpenAlgorithmProvider(&a.hmac, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG));
                return a;
            }();
            return algs;
        }

        // HMAC-SHA256 over the concatenation of the parts. False when CNG failed: a zero MAC taken as a nonce would repeat
        // across every block (GCM's one forbidden thing), and a zero PRK would make the AES key a public constant.
        bool Hmac(const std::uint8_t* key, std::size_t keyBytes,
                  std::initializer_list<std::pair<const std::uint8_t*, std::size_t>> parts, std::uint8_t out[32])
        {
            std::memset(out, 0, 32);
            BCRYPT_HASH_HANDLE hash = nullptr;
            if (!Succeeded(::BCryptCreateHash(Algs().hmac, &hash, nullptr, 0, const_cast<PUCHAR>(key), static_cast<ULONG>(keyBytes), 0)))
            {
                return false;
            }
            bool ok = true;
            for (const auto& [data, bytes] : parts)
            {
                if (ok && bytes > 0) { ok = Succeeded(::BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(bytes), 0)); }
            }
            ok = ok && Succeeded(::BCryptFinishHash(hash, out, 32, 0));
            ::BCryptDestroyHash(hash);
            if (!ok) { std::memset(out, 0, 32); }
            return ok;
        }

        // HKDF-SHA256 (RFC 5869), one 32-byte block of output.
        bool Hkdf(const std::array<std::uint8_t, 32>& master, std::string_view info, std::array<std::uint8_t, 32>& okm)
        {
            static constexpr char kSalt[] = "test_cube .tcpak";
            std::uint8_t prk[32];
            const std::uint8_t one = 1;
            const bool ok = Hmac(reinterpret_cast<const std::uint8_t*>(kSalt), sizeof(kSalt) - 1, { { master.data(), master.size() } }, prk) &&
                            Hmac(prk, sizeof(prk), { { reinterpret_cast<const std::uint8_t*>(info.data()), info.size() }, { &one, 1 } },
                                 okm.data());
            ::SecureZeroMemory(prk, sizeof(prk));
            return ok;
        }

        std::atomic<std::uint64_t> g_cipherSerial{ 1 };

        // A thread's duplicates of the ciphers' keys.
        struct ThreadKeys
        {
            struct Slot { std::uint64_t serial = 0; BCRYPT_KEY_HANDLE key = nullptr; };
            Slot slots[4];
            int next = 0;
            ~ThreadKeys()
            {
                for (Slot& s : slots) { if (s.key) { ::BCryptDestroyKey(s.key); } }
            }
        };
        thread_local ThreadKeys t_keys;

        std::string Hex(const std::uint8_t* p, std::size_t n)
        {
            static constexpr char kDigits[] = "0123456789abcdef";
            std::string s;
            s.reserve(n * 2);
            for (std::size_t i = 0; i < n; ++i)
            {
                s.push_back(kDigits[p[i] >> 4]);
                s.push_back(kDigits[p[i] & 15]);
            }
            return s;
        }

        bool Unhex(const std::string& text, std::uint8_t* out, std::size_t n)
        {
            if (text.size() != n * 2) { return false; }
            const auto nibble = [](char c) -> int
            {
                if (c >= '0' && c <= '9') { return c - '0'; }
                if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
                if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
                return -1;
            };
            for (std::size_t i = 0; i < n; ++i)
            {
                const int hi = nibble(text[2 * i]);
                const int lo = nibble(text[2 * i + 1]);
                if (hi < 0 || lo < 0) { return false; }
                out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
            }
            return true;
        }

        void Random(std::uint8_t* out, std::size_t n)
        {
            ::BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        }
    }

    std::string GuidText(const std::array<std::uint8_t, 16>& guid) { return Hex(guid.data(), guid.size()); }

    Key GenerateKey()
    {
        Key key;
        Random(key.guid.data(), key.guid.size());
        Random(key.bytes.data(), key.bytes.size());
        return key;
    }

    bool SaveKeyFile(const Key& key, const std::filesystem::path& file, std::string& error)
    {
        std::error_code ec;
        if (std::filesystem::exists(file, ec))
        {
            error = "refusing to overwrite the key " + vfs::Narrow(file.wstring()) + ": packages built with it would be unreadable";
            return false;
        }
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error = "cannot write " + vfs::Narrow(file.wstring());
            return false;
        }
        nlohmann::json j;
        j["guid"] = Hex(key.guid.data(), key.guid.size());
        j["key"] = Hex(key.bytes.data(), key.bytes.size());
        j["note"] = "test_cube package key. Keep a copy: without it the packages built with it cannot be read.";
        out << j.dump(2) << '\n';
        return static_cast<bool>(out);
    }

    std::optional<Key> LoadKeyFile(const std::filesystem::path& file, std::string& error)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            error = "cannot read the key " + vfs::Narrow(file.wstring());
            return std::nullopt;
        }
        const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        Key key;
        // A "guid" or "key" that is not a string made value() throw out of here.
        const auto text = [&j](const char* name) { return j.is_object() && j.contains(name) && j[name].is_string() ? j[name].get<std::string>() : std::string(); };
        if (!j.is_object() || !Unhex(text("guid"), key.guid.data(), key.guid.size()) || !Unhex(text("key"), key.bytes.data(), key.bytes.size()))
        {
            error = vfs::Narrow(file.wstring()) + " is not a package key (guid: 32 hex, key: 64 hex)";
            return std::nullopt;
        }
        return key;
    }

    bool WriteKeyHeader(const Key& key, const std::filesystem::path& header, std::string& error)
    {
        std::array<std::array<std::uint8_t, 32>, 4> parts{};
        for (int p = 1; p < 4; ++p) { Random(parts[static_cast<std::size_t>(p)].data(), 32); }
        for (std::size_t i = 0; i < 32; ++i)
        {
            parts[0][i] = static_cast<std::uint8_t>(key.bytes[i] ^ parts[1][i] ^ parts[2][i] ^ parts[3][i]);
        }
        std::error_code ec;
        std::filesystem::create_directories(header.parent_path(), ec);
        std::ofstream out(header, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error = "cannot write " + vfs::Narrow(header.wstring());
            return false;
        }
        const auto bytes = [](const std::uint8_t* p, std::size_t n)
        {
            std::string s;
            for (std::size_t i = 0; i < n; ++i) { s += (i ? ", " : "") + std::to_string(p[i]); }
            return s;
        };
        out << "// Generated by test_cube.exe --pak-key-header: the package key, in four XOR-masked parts.\r\n"
               "// NEVER COMMIT (sources/core/vfs/generated/ is in .gitignore). docs/distribution_build_plan.md section 4.\r\n"
               "#pragma once\r\n#include <cstdint>\r\n#define TC_PAK_HAS_EMBEDDED_KEY 1\r\nnamespace vfs::pak::embedded\r\n{\r\n"
            << "    inline constexpr std::uint8_t kGuid[16] = { " << bytes(key.guid.data(), 16) << " };\r\n"
            << "    inline constexpr std::uint8_t kParts[4][32] = {\r\n";
        for (int p = 0; p < 4; ++p)
        {
            out << "        { " << bytes(parts[static_cast<std::size_t>(p)].data(), 32) << " },\r\n";
        }
        out << "    };\r\n}\r\n";
        return static_cast<bool>(out);
    }

    std::optional<Key> EmbeddedKey()
    {
#if TC_PAK_HAS_EMBEDDED_KEY
        Key key;
        std::memcpy(key.guid.data(), embedded::kGuid, 16);
        // Read through volatile: folded at compile time, the XOR would put the key in the exe in the clear.
        const volatile std::uint8_t* parts = &embedded::kParts[0][0];
        for (std::size_t i = 0; i < 32; ++i)
        {
            key.bytes[i] = static_cast<std::uint8_t>(parts[i] ^ parts[32 + i] ^ parts[64 + i] ^ parts[96 + i]);
        }
        return key;
#else
        return std::nullopt;
#endif
    }

    void KeyRing::Add(const Key& key)
    {
        if (!Find(key.guid)) { keys_.push_back(key); }
    }

    const Key* KeyRing::Find(const std::array<std::uint8_t, 16>& guid) const
    {
        for (const Key& key : keys_)
        {
            if (key.guid == guid) { return &key; }
        }
        return nullptr;
    }

    Cipher::Cipher(const Key& key)
    {
        if (!Algs().ok) { return; }
        if (!Hkdf(key.bytes, "tcpak aes-256-gcm v1", encKey_) || !Hkdf(key.bytes, "tcpak synthetic nonce v1", nonceKey_)) { return; }
        BCRYPT_KEY_HANDLE handle = nullptr;
        ok_ = Succeeded(::BCryptGenerateSymmetricKey(Algs().aes, &handle, nullptr, 0, encKey_.data(), static_cast<ULONG>(encKey_.size()), 0));
        key_ = handle;
        ::SecureZeroMemory(encKey_.data(), encKey_.size()); // the key object holds it now
        serial_ = g_cipherSerial.fetch_add(1);
    }

    Cipher::~Cipher()
    {
        if (key_) { ::BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_)); }
        ::SecureZeroMemory(encKey_.data(), encKey_.size());
        ::SecureZeroMemory(nonceKey_.data(), nonceKey_.size());
    }

    void* Cipher::ThreadKey() const
    {
        for (ThreadKeys::Slot& slot : t_keys.slots)
        {
            if (slot.serial == serial_) { return slot.key; }
        }
        ThreadKeys::Slot& slot = t_keys.slots[t_keys.next];
        t_keys.next = (t_keys.next + 1) % 4;
        if (slot.key) { ::BCryptDestroyKey(slot.key); }
        slot.key = nullptr;
        slot.serial = 0;
        BCRYPT_KEY_HANDLE dup = nullptr;
        if (Succeeded(::BCryptDuplicateKey(static_cast<BCRYPT_KEY_HANDLE>(key_), &dup, nullptr, 0, 0)))
        {
            slot.key = dup;
            slot.serial = serial_;
        }
        return dup;
    }

    bool Cipher::Nonce(const std::uint8_t* aad, std::size_t aadBytes, const std::uint8_t* data, std::size_t bytes,
                       std::uint8_t nonce[12]) const
    {
        std::uint8_t mac[32];
        const bool ok = Hmac(nonceKey_.data(), nonceKey_.size(), { { aad, aadBytes }, { data, bytes } }, mac);
        std::memcpy(nonce, mac, 12);
        return ok;
    }

    bool Cipher::Encrypt(const std::uint8_t nonce[12], const std::uint8_t* aad, std::size_t aadBytes, std::uint8_t* data,
                         std::size_t bytes, std::uint8_t tag[16]) const
    {
        const BCRYPT_KEY_HANDLE key = static_cast<BCRYPT_KEY_HANDLE>(ThreadKey());
        if (!key) { return false; }
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = const_cast<PUCHAR>(nonce);
        info.cbNonce = 12;
        info.pbAuthData = const_cast<PUCHAR>(aad);
        info.cbAuthData = static_cast<ULONG>(aadBytes);
        info.pbTag = tag;
        info.cbTag = 16;
        ULONG written = 0;
        return Succeeded(::BCryptEncrypt(key, data, static_cast<ULONG>(bytes), &info, nullptr, 0, data, static_cast<ULONG>(bytes), &written, 0)) &&
               written == bytes;
    }

    bool Cipher::Decrypt(const std::uint8_t nonce[12], const std::uint8_t* aad, std::size_t aadBytes, std::uint8_t* data,
                         std::size_t bytes, const std::uint8_t tag[16]) const
    {
        const BCRYPT_KEY_HANDLE key = static_cast<BCRYPT_KEY_HANDLE>(ThreadKey());
        if (!key) { return false; }
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = const_cast<PUCHAR>(nonce);
        info.cbNonce = 12;
        info.pbAuthData = const_cast<PUCHAR>(aad);
        info.cbAuthData = static_cast<ULONG>(aadBytes);
        info.pbTag = const_cast<PUCHAR>(tag);
        info.cbTag = 16;
        ULONG written = 0;
        return Succeeded(::BCryptDecrypt(key, data, static_cast<ULONG>(bytes), &info, nullptr, 0, data, static_cast<ULONG>(bytes), &written, 0)) &&
               written == bytes;
    }
}
