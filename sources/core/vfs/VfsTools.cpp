// The VFS's command line and its tools (docs/distribution_build_plan.md §2.3, §3, §9): the mounts from --vfs / --pak-dir
// / --vfs-record, and --pak-build / --pak-list / --pak-verify / --vfs-selftest, which run with no window. Every verdict
// is a line in the session log; the exit code is the number of failures.

#include "core/logging/Log.h"
#include "core/vfs/Pak.h"
#include "core/vfs/Vfs.h"
#include "core/vfs/VfsMounts.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <optional>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace vfs
{
    namespace
    {
        std::string ArgValue(const char* cmd, const char* key)
        {
            const char* p = std::strstr(cmd, key);
            if (!p) { return {}; }
            p += std::strlen(key);
            std::string v;
            if (*p == '"')
            {
                ++p;
                while (*p && *p != '"') { v.push_back(*p++); }
            }
            else
            {
                while (*p && !std::isspace(static_cast<unsigned char>(*p))) { v.push_back(*p++); }
            }
            return v;
        }

        bool HasFlag(const char* cmd, const char* flag) { return std::strstr(cmd, flag) != nullptr; }

        std::filesystem::path FsPath(const std::string& utf8) { return std::filesystem::path(Widen(utf8)); }

        // --pak-build=<out.tcpak> --pak-files=<list> [--pak-name=base] [--pak-level=19] [--pak-priority=0]: the list is
        // one content path a line (# starts a comment), each read from the loose files under the content root.
        int BuildTool(const char* cmd)
        {
            const std::string out = ArgValue(cmd, "--pak-build=");
            const std::string listFile = ArgValue(cmd, "--pak-files=");
            if (out.empty() || listFile.empty())
            {
                LOG_ERROR(logging::LogCategory::Vfs, "pak build: --pak-build=<out.tcpak> needs --pak-files=<list>");
                return 1;
            }
            std::ifstream list(FsPath(listFile));
            if (!list)
            {
                LOG_ERROR(logging::LogCategory::Vfs, "pak build: cannot read the list {}", listFile);
                return 1;
            }
            std::vector<pak::BuildFile> files;
            std::string line;
            while (std::getline(list, line))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) { line.pop_back(); }
                if (line.empty() || line[0] == '#') { continue; }
                pak::BuildFile file;
                file.path = Path(line).Utf8();
                file.source = ContentRoot() / Widen(file.path);
                files.push_back(std::move(file));
            }
            pak::BuildOptions options;
            if (const std::string name = ArgValue(cmd, "--pak-name="); !name.empty()) { options.name = name; }
            if (const std::string level = ArgValue(cmd, "--pak-level="); !level.empty()) { options.level = std::atoi(level.c_str()); }
            if (const std::string priority = ArgValue(cmd, "--pak-priority="); !priority.empty())
            {
                options.priority = static_cast<std::uint32_t>(std::atoi(priority.c_str()));
            }
            std::optional<pak::Key> key;
            if (HasFlag(cmd, "--pak-encrypt"))
            {
                const std::string keyFile = ArgValue(cmd, "--pak-key=");
                std::string why;
                key = keyFile.empty() ? std::nullopt : pak::LoadKeyFile(FsPath(keyFile), why);
                if (!key)
                {
                    LOG_ERROR(logging::LogCategory::Vfs, "pak build: --pak-encrypt needs --pak-key=<key.json> ({})",
                              keyFile.empty() ? std::string("none given") : why);
                    return 1;
                }
                options.key = &*key;
            }
            const pak::BuildResult result = pak::Build(FsPath(out), std::move(files), options);
            if (!result.ok)
            {
                LOG_ERROR(logging::LogCategory::Vfs, "pak build: FAIL {}", result.error);
                return 1;
            }
            LOG_INFO(logging::LogCategory::Vfs,
                     "pak build: {} -- {} files, {:.1f} MB -> {:.1f} MB ({:.1f} %), {} blocks ({} stored as they were), zstd {}{}, {:.1f} s",
                     out, result.files, result.rawBytes / 1048576.0, result.packageBytes / 1048576.0,
                     result.rawBytes ? 100.0 * static_cast<double>(result.packageBytes) / static_cast<double>(result.rawBytes) : 0.0,
                     result.blocks, result.storedBlocks, options.level,
                     key ? ", encrypted with key " + pak::GuidText(key->guid) : std::string(), result.seconds);
            return 0;
        }

        int ListTool(const char* cmd)
        {
            const std::string file = ArgValue(cmd, "--pak-list=");
            std::string error;
            const std::unique_ptr<pak::Package> package = pak::Package::Open(FsPath(file), error, &detail::Keys());
            if (!package)
            {
                LOG_ERROR(logging::LogCategory::Vfs, "pak list: {}", error);
                return 1;
            }
            std::uint64_t bytes = 0;
            for (std::size_t e = 0; e < package->EntryCount(); ++e)
            {
                const pak::Package::EntryInfo info = package->Entry(static_cast<int>(e));
                bytes += info.size;
                LOG_INFO(logging::LogCategory::Vfs, "pak list: {} {} bytes, {} blocks, hash {:016x}{}", info.path, info.size,
                         info.blocks, info.contentHash, info.deleted ? " (deleted)" : "");
            }
            LOG_INFO(logging::LogCategory::Vfs, "pak list: {} '{}' priority {} -- {} files, {:.1f} MB of content in {:.1f} MB, id {:016x}{}",
                     file, package->Name(), package->Priority(), package->EntryCount(), bytes / 1048576.0,
                     package->FileBytes() / 1048576.0, package->ContentId(),
                     package->Encrypted() ? ", encrypted with key " + package->KeyGuid() : std::string());
            return 0;
        }

        // --pak-keygen=<key.json>: a new key (never over an existing file). --pak-key-header=<key.json>,<out.h>: the header a
        // Shipping build embeds it from.
        int KeyTool(const char* cmd)
        {
            std::string error;
            if (HasFlag(cmd, "--pak-keygen="))
            {
                const std::string file = ArgValue(cmd, "--pak-keygen=");
                const pak::Key key = pak::GenerateKey();
                if (!pak::SaveKeyFile(key, FsPath(file), error))
                {
                    LOG_ERROR(logging::LogCategory::Vfs, "pak keygen: {}", error);
                    return 1;
                }
                LOG_INFO(logging::LogCategory::Vfs, "pak keygen: {} -- key {}. Keep a copy: packages built with it cannot be read without it.",
                         file, pak::GuidText(key.guid));
                return 0;
            }
            const std::string arg = ArgValue(cmd, "--pak-key-header=");
            const std::size_t comma = arg.find(',');
            if (comma == std::string::npos)
            {
                LOG_ERROR(logging::LogCategory::Vfs, "pak key header: --pak-key-header=<key.json>,<out.h>");
                return 1;
            }
            const std::optional<pak::Key> key = pak::LoadKeyFile(FsPath(arg.substr(0, comma)), error);
            if (!key || !pak::WriteKeyHeader(*key, FsPath(arg.substr(comma + 1)), error))
            {
                LOG_ERROR(logging::LogCategory::Vfs, "pak key header: {}", error);
                return 1;
            }
            LOG_INFO(logging::LogCategory::Vfs, "pak key header: {} -- key {} in four masked parts", arg.substr(comma + 1), pak::GuidText(key->guid));
            return 0;
        }

        int VerifyTool(const char* cmd)
        {
            const std::string file = ArgValue(cmd, "--pak-verify=");
            std::string error;
            const auto started = std::chrono::steady_clock::now();
            const std::unique_ptr<pak::Package> package = pak::Package::Open(FsPath(file), error, &detail::Keys());
            if (!package)
            {
                LOG_ERROR(logging::LogCategory::Vfs, "pak verify: FAIL {}", error);
                return 1;
            }
            int failures = 0;
            for (std::size_t e = 0; e < package->EntryCount(); ++e)
            {
                std::string why;
                if (!package->Verify(static_cast<int>(e), why))
                {
                    LOG_ERROR(logging::LogCategory::Vfs, "pak verify: {}", why);
                    ++failures;
                }
            }
            LOG_INFO(logging::LogCategory::Vfs, "pak verify: {} {} -- {} files, {} damaged{}, {:.2f} s", file, failures == 0 ? "PASS" : "FAIL",
                     package->EntryCount(), failures, package->Encrypted() ? ", every tag checked" : "",
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
            return failures;
        }

        // ================================ the self-test ================================

        struct SelfTest
        {
            int checks = 0;
            int failures = 0;

            void Expect(bool ok, const std::string& what)
            {
                ++checks;
                if (!ok)
                {
                    ++failures;
                    LOG_ERROR(logging::LogCategory::Vfs, "vfs self-test: FAILED {}", what);
                }
            }
        };

        std::vector<std::uint8_t> Pattern(std::size_t size, std::uint32_t seed, bool compressible)
        {
            std::vector<std::uint8_t> bytes(size);
            std::mt19937 rng(seed);
            for (std::size_t i = 0; i < size; ++i)
            {
                bytes[i] = compressible ? static_cast<std::uint8_t>((i / 7) % 23 + (i % 3)) : static_cast<std::uint8_t>(rng());
            }
            return bytes;
        }

        bool WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            return static_cast<bool>(out);
        }

        std::vector<std::uint8_t> ReadOs(const std::filesystem::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }

        int SelfTestTool()
        {
            SelfTest t;
            wchar_t tempRoot[MAX_PATH] = {};
            ::GetTempPathW(MAX_PATH, tempRoot);
            const std::filesystem::path root = std::filesystem::path(tempRoot) /
                                               (L"test_cube_vfs_selftest_" + std::to_wstring(::GetCurrentProcessId()));
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            const std::filesystem::path src = root / L"src";

            struct Case { std::string path; std::size_t size; bool compressible; };
            const std::vector<Case> cases = {
                { "dir/empty.bin", 0, true },
                { "dir/one.bin", 1, true },
                { "dir/Sub/edge_minus.bin", pak::kBlockSize - 1, true },
                { "dir/Sub/edge.bin", pak::kBlockSize, false },
                { "dir/Sub/edge_plus.bin", pak::kBlockSize + 1, true },
                { "Mixed/CASE.json", 3 * pak::kBlockSize + 123, true },
                { "big/compressible.bin", 5u << 20, true },
                { "big/random.bin", 2u << 20, false },
                { "data/\xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb.txt", 4321, true }, // Cyrillic in the name
            };
            std::vector<std::vector<std::uint8_t>> contents;
            std::vector<pak::BuildFile> files;
            for (std::size_t i = 0; i < cases.size(); ++i)
            {
                contents.push_back(Pattern(cases[i].size, static_cast<std::uint32_t>(1000 + i), cases[i].compressible));
                const std::filesystem::path at = src / Widen(cases[i].path);
                t.Expect(WriteBytes(at, contents.back()), "writing the source " + cases[i].path);
                files.push_back({ cases[i].path, at, false });
            }

            // Build twice: the same files give the same bytes.
            pak::BuildOptions options;
            options.name = "selftest";
            options.level = 3;
            const std::filesystem::path pakA = root / L"a.tcpak";
            const std::filesystem::path pakB = root / L"b.tcpak";
            const pak::BuildResult built = pak::Build(pakA, files, options);
            t.Expect(built.ok, "build: " + built.error);
            t.Expect(built.storedBlocks > 0 && built.storedBlocks < built.blocks, "some blocks stored as they were, the rest compressed");
            options.threads = 1; // and the thread count does not change them
            t.Expect(pak::Build(pakB, files, options).ok, "second build");
            t.Expect(ReadOs(pakA) == ReadOs(pakB), "the same files give the same package, byte for byte");

            std::string error;
            std::unique_ptr<pak::Package> package = pak::Package::Open(pakA, error);
            t.Expect(package != nullptr, "open: " + error);
            if (!package)
            {
                LOG_ERROR(logging::LogCategory::Vfs, "vfs self-test: FAIL ({} of {} checks)", t.failures, t.checks);
                return std::max(t.failures, 1);
            }

            // Every file: found case-insensitively, whole, and at random ranges across block boundaries.
            std::mt19937 rng(7);
            for (std::size_t i = 0; i < cases.size(); ++i)
            {
                const Case& c = cases[i];
                std::string upper = c.path;
                for (char& ch : upper) { if (ch >= 'a' && ch <= 'z') { ch = static_cast<char>(ch - 'a' + 'A'); } }
                const int entry = package->Find(c.path);
                t.Expect(entry >= 0 && package->Find(upper) == entry, "find " + c.path + " in any case");
                if (entry < 0) { continue; }
                t.Expect(package->Entry(entry).size == c.size, "size of " + c.path);
                std::vector<std::uint8_t> whole(c.size);
                t.Expect(package->Read(entry, 0, whole.data(), whole.size()) && whole == contents[i], "whole " + c.path);
                std::string why;
                t.Expect(package->Verify(entry, why), "verify " + c.path + " " + why);
                for (int r = 0; r < 200 && c.size > 0; ++r)
                {
                    const std::size_t at = rng() % c.size;
                    const std::size_t len = std::min<std::size_t>(c.size - at, rng() % (3 * pak::kBlockSize));
                    std::vector<std::uint8_t> part(len);
                    if (!package->Read(entry, at, part.data(), len) ||
                        !std::equal(part.begin(), part.end(), contents[i].begin() + static_cast<std::ptrdiff_t>(at)))
                    {
                        t.Expect(false, "range " + std::to_string(at) + "+" + std::to_string(len) + " of " + c.path);
                        break;
                    }
                }
                t.Expect(!package->Read(entry, c.size, whole.data(), 1), "a read past the end of " + c.path + " is refused");
            }
            t.Expect(package->Find("dir/missing.bin") < 0, "a missing file is missing");
            t.Expect(package->HasDirectory("dir") && package->HasDirectory("DIR/sub") && !package->HasDirectory("di"),
                     "directories, case-insensitive, whole names only");
            std::vector<std::string> listed;
            package->ListDirectory("dir", [&](std::string_view name, bool directory)
            {
                listed.push_back(std::string(name) + (directory ? "/" : ""));
            });
            t.Expect(listed == std::vector<std::string>{ "empty.bin", "one.bin", "Sub/" }, "list dir: files and the folder, once");

            // A mounted table: the VFS answers through the package as it does through files.
            {
                detail::Mounts mounts;
                mounts.loose = false;
                std::unique_ptr<pak::Package> mounted = pak::Package::Open(pakA, error);
                const pak::Package* raw = mounted.get();
                mounts.packages.push_back(std::move(mounted));
                const std::optional<Stat> stat = detail::StatIn(mounts, Path("Mixed/case.JSON"));
                t.Expect(stat && stat->size == cases[5].size && stat->version == raw->Entry(raw->Find("Mixed/CASE.json")).contentHash,
                         "stat through the mounts: size and the content hash as version");
                t.Expect(detail::StatIn(mounts, Path("dir/Sub")) && detail::StatIn(mounts, Path("dir/Sub"))->directory, "a packaged directory");
                File file = detail::OpenIn(mounts, Path("big\\compressible.bin"));
                InStream stream(std::move(file));
                stream.seekg(static_cast<std::streamoff>(pak::kBlockSize - 5));
                char five[10] = {};
                stream.read(five, 10);
                t.Expect(static_cast<bool>(stream) &&
                         std::equal(five, five + 10, reinterpret_cast<const char*>(contents[6].data()) + pak::kBlockSize - 5),
                         "an InStream over a packaged file seeks and reads across a block boundary");
                stream.seekg(0, std::ios::end);
                t.Expect(static_cast<std::uint64_t>(stream.tellg()) == cases[6].size, "tellg at the end is the size");
                int names = 0;
                detail::ListIn(mounts, Path("dir/Sub"), "*.bin", [&](const ListEntry&) { ++names; });
                t.Expect(names == 3, "List through the mounts with a pattern");

                // Eight threads reading the same file at random ranges at once.
                std::atomic<int> bad{ 0 };
                std::vector<std::thread> threads;
                const int big = raw->Find("big/compressible.bin");
                for (int k = 0; k < 8; ++k)
                {
                    threads.emplace_back([&, k]()
                    {
                        std::mt19937 local(100 + k);
                        std::vector<std::uint8_t> part;
                        for (int r = 0; r < 300; ++r)
                        {
                            const std::size_t at = local() % cases[6].size;
                            const std::size_t len = std::min<std::size_t>(cases[6].size - at, local() % 200000);
                            part.resize(len);
                            if (!raw->Read(big, at, part.data(), len) ||
                                !std::equal(part.begin(), part.end(), contents[6].begin() + static_cast<std::ptrdiff_t>(at)))
                            {
                                bad.fetch_add(1);
                            }
                        }
                    });
                }
                for (std::thread& th : threads) { th.join(); }
                t.Expect(bad.load() == 0, "eight threads reading at once");

                // A patch above it deletes one file and replaces another.
                const std::filesystem::path patchSource = root / L"patch_one.bin";
                const std::vector<std::uint8_t> replaced = Pattern(777, 9, true);
                t.Expect(WriteBytes(patchSource, replaced), "writing the patch's file");
                pak::BuildOptions patchOptions;
                patchOptions.name = "patch_001";
                patchOptions.priority = 10;
                patchOptions.level = 3;
                const std::filesystem::path patch = root / L"patch.tcpak";
                t.Expect(pak::Build(patch, { { "dir/one.bin", patchSource, false }, { "dir/empty.bin", {}, true } }, patchOptions).ok,
                         "build a patch");
                std::unique_ptr<pak::Package> patchPackage = pak::Package::Open(patch, error);
                t.Expect(patchPackage != nullptr, "open the patch: " + error);
                if (patchPackage) { mounts.packages.push_back(std::move(patchPackage)); }
                detail::SortPackages(mounts.packages);
                t.Expect(mounts.packages.front()->Name() == "patch_001", "the patch mounts first (priority)");
                t.Expect(!detail::StatIn(mounts, Path("dir/empty.bin")), "a deleted record hides the base's file");
                const std::optional<Stat> one = detail::StatIn(mounts, Path("dir/one.bin"));
                t.Expect(one && one->size == replaced.size(), "the patch's file wins");
                std::vector<std::string> merged;
                detail::ListIn(mounts, Path("dir"), "*", [&](const ListEntry& e) { merged.push_back(e.name); });
                t.Expect(std::find(merged.begin(), merged.end(), "empty.bin") == merged.end() &&
                         std::count(merged.begin(), merged.end(), std::string("one.bin")) == 1,
                         "a merged list: the deleted file gone, the replaced one once");

                // The user overlay (§2.1, Shipping's data/bot_memory/): what the player saved under the prefix, over
                // every package; nothing outside the prefix is looked for there (the control: one.bin in the folder).
                const std::filesystem::path overlay = root / L"overlay";
                const std::vector<std::uint8_t> saved = Pattern(123, 11, true);
                t.Expect(WriteBytes(overlay / L"edge.bin", saved) && WriteBytes(overlay / L"new.bin", saved) &&
                         WriteBytes(overlay / L"one.bin", saved), "writing the overlay's files");
                mounts.overlayPrefix = "dir/Sub/";
                mounts.overlayFolder = overlay;
                const std::optional<Stat> over = detail::StatIn(mounts, Path("DIR/sub/edge.bin"));
                t.Expect(over && over->size == saved.size(), "the overlay's file wins over the package's (any case)");
                std::vector<std::uint8_t> back;
                File overFile = detail::OpenIn(mounts, Path("dir/Sub/edge.bin"));
                back.resize(static_cast<std::size_t>(overFile.Size()));
                t.Expect(overFile && overFile.Read(0, back.data(), back.size()) && back == saved, "the overlay's bytes are read");
                const std::optional<Stat> under = detail::StatIn(mounts, Path("dir/Sub/edge_minus.bin"));
                t.Expect(under && under->size == cases[2].size, "a file the overlay lacks comes from the package");
                const std::optional<Stat> outside = detail::StatIn(mounts, Path("dir/one.bin"));
                t.Expect(outside && outside->size == replaced.size(), "outside the prefix the overlay is not asked (control)");
                t.Expect(detail::LooseServesIn(mounts, Path("dir/Sub/edge.bin")) &&
                         !detail::LooseServesIn(mounts, Path("dir/Sub/edge_minus.bin")), "the overlay's file is loose, the package's not");
                std::vector<std::string> overList;
                detail::ListIn(mounts, Path("dir/Sub"), "*.bin", [&](const ListEntry& e) { overList.push_back(e.name); });
                t.Expect(std::count(overList.begin(), overList.end(), std::string("new.bin")) == 1 &&
                         std::count(overList.begin(), overList.end(), std::string("edge.bin")) == 1 && overList.size() == 5,
                         "a list under the prefix: the overlay's files (its one.bin is dir/Sub/one.bin), the shared one once");
                mounts.overlayPrefix.clear();
            }
            package.reset();

            // Encryption (§4): the same reads through an encrypted package, nothing in the clear, and the controls.
            {
                const pak::Key key = pak::GenerateKey();
                pak::KeyRing ring;
                ring.Add(key);
                pak::BuildOptions secret;
                secret.name = "secret";
                secret.level = 3;
                secret.key = &key;
                std::vector<pak::BuildFile> encFiles = files;
                encFiles.push_back({ "twin/random_copy.bin", src / Widen(cases[7].path), false }); // the same bytes, another path
                const std::filesystem::path encA = root / L"enc_a.tcpak";
                const std::filesystem::path encB = root / L"enc_b.tcpak";
                const pak::BuildResult sealed = pak::Build(encA, encFiles, secret);
                t.Expect(sealed.ok, "encrypted build: " + sealed.error);
                secret.threads = 1;
                t.Expect(pak::Build(encB, encFiles, secret).ok && ReadOs(encA) == ReadOs(encB),
                         "an encrypted build is deterministic too (the synthetic nonce)");
                std::string why;
                std::unique_ptr<pak::Package> enc = pak::Package::Open(encA, why, &ring);
                t.Expect(enc && enc->Encrypted(), "open with the key: " + why);
                if (enc)
                {
                    for (std::size_t i = 0; i < cases.size(); ++i)
                    {
                        const int entry = enc->Find(cases[i].path);
                        std::vector<std::uint8_t> whole(cases[i].size);
                        std::string verifyWhy;
                        t.Expect(entry >= 0 && enc->Read(entry, 0, whole.data(), whole.size()) && whole == contents[i] &&
                                 enc->Verify(entry, verifyWhy), "encrypted: whole + verify " + cases[i].path);
                        if (entry >= 0 && cases[i].size > 2 * pak::kBlockSize)
                        {
                            std::vector<std::uint8_t> part(1000);
                            t.Expect(enc->Read(entry, pak::kBlockSize - 500, part.data(), part.size()) &&
                                     std::equal(part.begin(), part.end(), contents[i].begin() + (pak::kBlockSize - 500)),
                                     "encrypted: a range across a block boundary of " + cases[i].path);
                        }
                    }
                    const std::vector<std::uint8_t> raw = ReadOs(encA);
                    const auto plain = contents[7].begin() + 4096;
                    t.Expect(std::search(raw.begin(), raw.end(), plain, plain + 64) == raw.end(),
                             "the random file's bytes are not in the package in the clear");
                    const std::string name = "compressible.bin";
                    t.Expect(std::search(raw.begin(), raw.end(), name.begin(), name.end()) == raw.end(),
                             "the file names are not in the package in the clear (the table of contents is encrypted)");
                    std::uint32_t n1 = 0, n2 = 0;
                    const std::uint8_t* b1 = enc->StoredBytes(enc->Find("big/random.bin"), 0, n1);
                    const std::uint8_t* b2 = enc->StoredBytes(enc->Find("twin/random_copy.bin"), 0, n2);
                    t.Expect(b1 && b2 && n1 == n2 && std::memcmp(b1, b2, n1) != 0, "the same bytes under two paths encrypt differently");
                }
                enc.reset();
                why.clear();
                t.Expect(!pak::Package::Open(encA, why) && why.find(pak::GuidText(key.guid)) != std::string::npos,
                         "control: without the key the package is refused, naming its key");
                pak::Key impostor = pak::GenerateKey();
                impostor.guid = key.guid;
                pak::KeyRing wrong;
                wrong.Add(impostor);
                why.clear();
                t.Expect(!pak::Package::Open(encA, why, &wrong) && why.find("authenticate") != std::string::npos,
                         "control: another key under the same GUID does not open it");

                // The cipher itself: a changed byte, tag or AAD is refused.
                pak::Cipher cipher(key);
                std::vector<std::uint8_t> data = Pattern(1000, 5, false);
                const std::vector<std::uint8_t> original = data;
                const std::uint8_t aadA[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
                const std::uint8_t aadB[8] = { 1, 2, 3, 4, 5, 6, 7, 9 };
                std::uint8_t nonce[12] = {};
                std::uint8_t tag[16] = {};
                cipher.Nonce(aadA, sizeof(aadA), data.data(), data.size(), nonce);
                t.Expect(cipher.Encrypt(nonce, aadA, sizeof(aadA), data.data(), data.size(), tag) && data != original, "encrypt");
                std::vector<std::uint8_t> copy = data;
                copy[17] ^= 1;
                t.Expect(!cipher.Decrypt(nonce, aadA, sizeof(aadA), copy.data(), copy.size(), tag), "control: a changed byte fails the tag");
                copy = data;
                t.Expect(!cipher.Decrypt(nonce, aadB, sizeof(aadB), copy.data(), copy.size(), tag), "control: another AAD (a moved block) fails the tag");
                copy = data;
                std::uint8_t badTag[16];
                std::memcpy(badTag, tag, 16);
                badTag[0] ^= 0x80;
                t.Expect(!cipher.Decrypt(nonce, aadA, sizeof(aadA), copy.data(), copy.size(), badTag), "control: a changed tag fails");
                copy = data;
                t.Expect(cipher.Decrypt(nonce, aadA, sizeof(aadA), copy.data(), copy.size(), tag) && copy == original, "decrypt");
            }

            // Control arms: damage must be caught, and named.
            {
                std::vector<std::uint8_t> bytes = ReadOs(pakA);
                // The random file's blocks are stored as they were: flip a byte in its first block.
                pak::Header header;
                std::memcpy(&header, bytes.data(), sizeof(header));
                std::size_t flip = 0;
                for (std::size_t at = pak::kHeaderBytes; at + 16 < header.tocOffset; at += pak::kAlign)
                {
                    // the first aligned offset whose bytes match the random file's start
                    if (std::equal(contents[7].begin(), contents[7].begin() + 16, bytes.begin() + static_cast<std::ptrdiff_t>(at)))
                    {
                        flip = at + 1000;
                        break;
                    }
                }
                t.Expect(flip != 0, "found the random file's data to damage");
                bytes[flip] ^= 0x5A;
                const std::filesystem::path damaged = root / L"damaged.tcpak";
                t.Expect(WriteBytes(damaged, bytes), "write the damaged package");
                std::unique_ptr<pak::Package> broken = pak::Package::Open(damaged, error);
                t.Expect(broken != nullptr, "a damaged block does not stop the package opening");
                if (broken)
                {
                    const int e = broken->Find("big/random.bin");
                    std::vector<std::uint8_t> part(100);
                    t.Expect(!broken->Read(e, 950, part.data(), part.size()), "control: reading the damaged block FAILS");
                    t.Expect(broken->Read(e, pak::kBlockSize + 10, part.data(), part.size()), "the next block still reads");
                    std::string why;
                    t.Expect(!broken->Verify(e, why) && why.find("big/random.bin") != std::string::npos, "control: verify names the file");
                }
                std::vector<std::uint8_t> tocBroken = ReadOs(pakA);
                tocBroken[static_cast<std::size_t>(header.tocOffset) + 3] ^= 0xFF;
                const std::filesystem::path badToc = root / L"bad_toc.tcpak";
                t.Expect(WriteBytes(badToc, tocBroken), "write the package with a damaged table of contents");
                std::string tocError;
                t.Expect(!pak::Package::Open(badToc, tocError) && tocError.find("table of contents") != std::string::npos,
                         "control: a damaged table of contents is refused, and says so");
            }

            std::filesystem::remove_all(root, ec);
            if (t.failures == 0)
            {
                LOG_INFO(logging::LogCategory::Vfs, "vfs self-test: PASS ({} checks)", t.checks);
            }
            else
            {
                LOG_ERROR(logging::LogCategory::Vfs, "vfs self-test: FAIL ({} of {} checks)", t.failures, t.checks);
            }
            return t.failures;
        }
    }

    int RunCommandLine(const char* cmd)
    {
        if (!cmd) { return -1; }
        Config config;
        const std::string mode = ArgValue(cmd, "--vfs=");
        if (mode == "packages") { config.mode = Config::Mode::Packages; }
        else if (mode == "both") { config.mode = Config::Mode::Both; }
        else if (!mode.empty() && mode != "loose")
        {
            LOG_ERROR(logging::LogCategory::Vfs, "vfs: unknown --vfs={} (loose, packages, both)", mode);
            return 1;
        }
        if (const std::string dir = ArgValue(cmd, "--pak-dir="); !dir.empty()) { config.packageDir = FsPath(dir); }
        if (const std::string record = ArgValue(cmd, "--vfs-record="); !record.empty()) { config.recordFile = FsPath(record); }
        // The key of encrypted packages: --pak-key=<key.json>, or the TC_PAK_KEY environment variable naming one.
        if (const std::string keyFile = ArgValue(cmd, "--pak-key="); !keyFile.empty() && !HasFlag(cmd, "--pak-keygen="))
        {
            config.keyFiles.push_back(FsPath(keyFile));
        }
        else if (const DWORD n = ::GetEnvironmentVariableW(L"TC_PAK_KEY", nullptr, 0); n > 1)
        {
            std::wstring value(n, L'\0');
            value.resize(::GetEnvironmentVariableW(L"TC_PAK_KEY", value.data(), n));
            config.keyFiles.push_back(std::filesystem::path(value));
        }
        if (config.mode != Config::Mode::Loose || !config.recordFile.empty() || !config.keyFiles.empty())
        {
            std::string error;
            if (!Configure(config, error))
            {
                LOG_FATAL(logging::LogCategory::Vfs, "vfs: {}", error);
                return 3;
            }
        }
        if (config.mode != Config::Mode::Loose)
        {
            for (const std::string& m : DescribeMounts()) { LOG_INFO(logging::LogCategory::Vfs, "vfs: mounted {}", m); }
        }

        if (HasFlag(cmd, "--vfs-selftest")) { return SelfTestTool(); }
        if (HasFlag(cmd, "--pak-keygen=") || HasFlag(cmd, "--pak-key-header=")) { return KeyTool(cmd); }
        if (HasFlag(cmd, "--pak-build=")) { return BuildTool(cmd); }
        if (HasFlag(cmd, "--pak-list=")) { return ListTool(cmd); }
        if (HasFlag(cmd, "--pak-verify=")) { return VerifyTool(cmd); }
        return -1;
    }
}
