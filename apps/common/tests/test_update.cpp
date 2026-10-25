// The updater: versions, SHA-256, reading a release, telling installs apart,
// when the background check runs, and (on linux and macOS, with curl) a whole
// update of a fake installed folder from a file:// release feed.
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "app_commands.hpp"
#include "apps.hpp"
#include "check.hpp"
#include "handoff.hpp"
#include "update.hpp"
#include "wetwaresuite/core/process.hpp"

using namespace wetwaresuite;
namespace fs = std::filesystem;
namespace up = wetwaresuite::apps::update;

namespace {

void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
#endif
}

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string fileUrl(const fs::path& p) { return "file://" + fs::absolute(p).generic_string(); }

std::string text(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    const std::string* s = v ? std::get_if<std::string>(v) : nullptr;
    return s ? *s : std::string();
}

#ifndef _WIN32
// A stand-in program that answers --version like the real ones.
void fakeProgram(const fs::path& p, const std::string& version) {
    write(p, "#!/bin/sh\necho \"wetwaresuite " + version + "\"\n");
    fs::permissions(p, fs::perms::owner_all);
}
#endif

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "wetwaresuite_test_update";
    fs::remove_all(root);
    setEnv("WETWARESUITE_HOME", (root / "home").string());

    // ---- versions ----
    {
        auto v = up::parseVersion("v1.2.3");
        CHECK(v && v->major == 1 && v->minor == 2 && v->patch == 3 && v->pre.empty());
        CHECK(up::parseVersion("0.2") && up::parseVersion("0.2")->patch == 0);
        auto pre = up::parseVersion("0.2.0-preview");
        CHECK(pre && pre->pre == "preview" && pre->text() == "0.2.0-preview");
        CHECK(!up::parseVersion("preview") && !up::parseVersion("") && !up::parseVersion("1.x.0") && !up::parseVersion("1.2.3.4"));
        auto cmp = [](const char* a, const char* b) { return up::compare(*up::parseVersion(a), *up::parseVersion(b)); };
        CHECK(cmp("0.1.1", "0.2.0") < 0 && cmp("0.10.0", "0.9.9") > 0 && cmp("v1.0.0", "1.0") == 0);
        CHECK(cmp("1.0.0-preview", "1.0.0") < 0 && cmp("1.0.0", "1.0.0-preview") > 0 && cmp("1.0.1-preview", "1.0.0") > 0);
        CHECK(up::parseVersion(apps::kVersion).has_value());  // what CMake set
    }

    // ---- SHA-256 (FIPS 180-4 test vectors) ----
    {
        CHECK(up::sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        CHECK(up::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        CHECK(up::sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
        CHECK(up::sha256Hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        write(root / "abc.txt", "abc");
        CHECK(up::sha256File(root / "abc.txt") == up::sha256Hex("abc"));
        CHECK(up::sha256File(root / "missing").empty());
    }

    // ---- reading a release ----
    {
        const std::string json = R"({"tag_name":"v0.3.0","html_url":"https://example.test/r","assets":[
            {"name":"wetwaresuite-linux-x64.zip","size":10,"browser_download_url":"https://example.test/a.zip",
             "digest":"sha256:ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789"},
            {"name":"wetwaresuite-linux-amd64.deb","size":20,"browser_download_url":"https://example.test/b.deb","digest":null},
            {"name":"SHA256SUMS","browser_download_url":"https://example.test/SHA256SUMS"}]})";
        std::string error;
        auto r = up::parseRelease(json, error);
        CHECK(r && r->tag == "v0.3.0" && r->version.minor == 3 && r->assets.size() == 3 && r->page == "https://example.test/r");
        CHECK(r && r->assets[0].sha256 == "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789" && r->assets[0].size == 10);
        CHECK(r && r->assets[1].sha256.empty());
        if (r) {
            up::applyChecksums(*r, std::string(64, '1') + "  wetwaresuite-linux-amd64.deb\r\n" + std::string(64, '2') +
                                       " *wetwaresuite-linux-x64.zip\nnot a line\n");
            CHECK(r->assets[1].sha256 == std::string(64, '1'));
            CHECK(r->assets[0].sha256[0] == 'a');  // GitHub's own digest wins
        }
        CHECK(!up::parseRelease("<html>", error) && !error.empty());
        CHECK(!up::parseRelease(R"({"message":"Not Found"})", error) && error.find("no release") != std::string::npos);
    }

    // ---- telling installs apart ----
    {
        const fs::path zip = root / "zip";
#ifdef _WIN32
        write(zip / "wetwaresuite-hub.exe", "x");
#else
        write(zip / "wetwaresuite-hub", "x");
#endif
        CHECK(up::detectInstall(zip / "wetwaresuite-image", "").kind == up::Kind::Folder);
        CHECK(up::detectInstall(zip / "wetwaresuite-image", "").writable);
        write(root / "setup" / "unins000.exe", "x");
        CHECK(up::detectInstall(root / "setup" / "wetwaresuite-image.exe", "").kind == up::Kind::Setup);
        const fs::path mac = root / "Applications" / "wetwaresuite";
        auto in = up::detectInstall(mac / "wetwaresuite image.app" / "Contents" / "MacOS" / "wetwaresuite-image", "");
        CHECK(in.kind == up::Kind::MacBundles && in.dir == mac);
        write(root / "Downloads" / "wetwaresuite-linux-x86_64.AppImage", "x");
        in = up::detectInstall("/tmp/.mount_abc/usr/bin/wetwaresuite-image", (root / "Downloads" / "wetwaresuite-linux-x86_64.AppImage").string());
        CHECK(in.kind == up::Kind::AppImage && in.dir == root / "Downloads");
        CHECK(up::detectInstall(root / "build" / "bin" / "wetwaresuite-image", "").kind == up::Kind::Source);
        fs::create_directories(apps::dataRoot() / "bin");
        CHECK(up::detectInstall(apps::dataRoot() / "bin" / "wetwaresuite-image", "").kind == up::Kind::Folder);  // hub installs
        CHECK(std::string(up::kindName(up::Kind::Deb)) == "deb");
    }

    // ---- when the background check runs ----
    {
        up::Install folder;
        folder.kind = up::Kind::Folder;
        folder.writable = true;
        const bool here = !up::assetName(folder).empty();  // a build exists for this computer
        up::Settings on, off;
        off.autoUpdate = false;
        up::State fresh, recent, offline;
        const std::int64_t t = 2000000000;
        recent.checked = t - 3600;
        offline.checked = t - 3 * 86400;
        offline.tried = t - 600;
        std::string why;
        CHECK(up::due(on, fresh, folder, "0.2.0", t, why) == here);
        CHECK(!up::due(off, fresh, folder, "0.2.0", t, why) && why == "auto updates are off");
        CHECK(!up::due(on, recent, folder, "0.2.0", t, why) && why.find("day") != std::string::npos);
        CHECK(!up::due(on, offline, folder, "0.2.0", t, why));
        offline.tried = t - 7200;
        CHECK(up::due(on, offline, folder, "0.2.0", t, why) == here);
        CHECK(!up::due(on, fresh, folder, "0.2.0-preview", t, why) && why.find("preview") != std::string::npos);
        up::Install source;
        CHECK(!up::due(on, fresh, source, "0.2.0", t, why) && why.find("source") != std::string::npos);
    }

    // ---- settings, through the commands ----
    {
        Session s;
        apps::registerCommands(s, "image");
        for (const char* id : {"app.update_status", "app.update_check", "app.update_install", "app.update_auto", "app.update_settings"})
            CHECK(s.registry().find(id) != nullptr);
        CHECK(up::loadSettings().autoUpdate);  // on until someone turns it off
        CHECK(s.run("app.update_settings", {{"auto", false}}).ok && !up::loadSettings().autoUpdate);
        CHECK(read(up::settingsFile()).find("auto=off") != std::string::npos);
        Result st = s.run("app.update_status", {});
        CHECK(st.ok && text(st, "current") == apps::kVersion && st.message.find("updates by itself: off") != std::string::npos);
        // off: the background run does nothing, without touching the network
        Result bg = s.run("app.update_auto", {});
        CHECK(bg.ok && std::get<std::string>(bg.value) == "skipped");
        CHECK(s.run("app.update_settings", {{"auto", true}}).ok && up::loadSettings().autoUpdate);
        up::State state;
        state.status = "uptodate";
        state.latest = "0.2.0";
        state.checked = 1;
        up::saveState(state);
        CHECK(up::loadState().status == "uptodate" && up::loadState().latest == "0.2.0" && up::loadState().checked == 1);
    }

    // ---- left-over programs from an update are cleaned up ----
    {
        const fs::path d = root / "left";
        write(d / "wetwaresuite-image.old", "x");
        write(d / "wetwaresuite-image.old2", "x");
        write(d / ".wetwaresuite-update" / "x", "x");
        write(d / "wetwaresuite-image", "x");
        write(d / "notes.old", "x");
        up::cleanupOld(d);
        CHECK(fs::exists(d / "wetwaresuite-image") && fs::exists(d / "notes.old"));
        CHECK(!fs::exists(d / "wetwaresuite-image.old") && !fs::exists(d / "wetwaresuite-image.old2") && !fs::exists(d / ".wetwaresuite-update"));
    }

#ifndef _WIN32
    // ---- a whole update of an unpacked zip, from a file:// feed ----
    up::Install probe;
    probe.kind = up::Kind::Folder;
    probe.writable = true;
    const std::string asset = up::assetName(probe);
    const bool tools = findProgram("curl") && (findProgram("zip") || findProgram("python3")) &&
                       (findProgram("unzip") || findProgram("ditto") || findProgram("python3"));
    if (asset.empty() || !tools) {
        std::cout << "skipping the full update: no build for this computer, or no curl/zip here\n";
    } else {
        const fs::path install = root / "installed";
        fakeProgram(install / "wetwaresuite", "0.0.1");
        fakeProgram(install / "wetwaresuite-image", "0.0.1");
        write(install / "wetwaresuite-hub", "old hub");
        write(install / "my-notes.txt", "keep me");
        const fs::path rel = root / "release";
        const fs::path inner = rel / "stage" / "wetwaresuite-linux-x64";
        fakeProgram(inner / "wetwaresuite", "99.0.0");
        fakeProgram(inner / "wetwaresuite-image", "99.0.0");
        write(inner / "wetwaresuite-hub", "new hub");
        write(inner / "README.txt", "readme");
        const fs::path zip = rel / asset;
        ProcessResult z = findProgram("zip") ? runProgram({"sh", "-c", "cd '" + (rel / "stage").string() + "' && zip -qr '" + zip.string() + "' ."})
                                             : runProgram({"python3", "-m", "zipfile", "-c", zip.string(), inner.string()});
        CHECK(z.exitCode == 0);
        auto feed = [&](const std::string& sha) {
            const std::string json = R"({"tag_name":"v99.0.0","html_url":"https://example.test/v99","assets":[{"name":")" + asset +
                                     R"(","size":)" + std::to_string(fs::file_size(zip)) + R"(,"browser_download_url":")" + fileUrl(zip) +
                                     R"(","digest":"sha256:)" + sha + R"("}]})";
            write(rel / "latest.json", json);
        };
        setEnv("WETWARESUITE_UPDATE_URL", fileUrl(rel / "latest.json"));
        setEnv("WETWARESUITE_UPDATE_EXE", (install / "wetwaresuite-image").string());
        Session s;
        apps::registerCommands(s, "image");

        // a download that doesn't match its SHA-256 is thrown away and nothing changes
        feed(std::string(64, '0'));
        Result bad = s.run("app.update_install", {});
        CHECK(!bad.ok && bad.message.find("SHA-256") != std::string::npos);
        CHECK(runProgram({(install / "wetwaresuite").string(), "--version"}).output.find("0.0.1") != std::string::npos);

        feed(up::sha256File(zip));
        Result check = s.run("app.update_check", {});
        CHECK(check.ok && text(check, "status") == "available" && text(check, "latest") == "99.0.0" && text(check, "install") == "folder");
        Result r = s.run("app.update_install", {});
        if (!r.ok) std::cout << r.message << "\n";
        CHECK(r.ok && text(r, "status") == "installed");
        CHECK(runProgram({(install / "wetwaresuite").string(), "--version"}).output.find("99.0.0") != std::string::npos);
        CHECK(runProgram({(install / "wetwaresuite-image").string(), "--version"}).output.find("99.0.0") != std::string::npos);
        CHECK(read(install / "wetwaresuite-hub") == "new hub" && read(install / "my-notes.txt") == "keep me");
        CHECK(!fs::exists(install / "README.txt") && !fs::exists(install / "wetwaresuite.old") && !fs::exists(install / ".wetwaresuite-update"));
        CHECK(up::loadState().status == "installed" && up::loadState().latest == "99.0.0");

        // a new build that doesn't start never replaces the old one
        fakeProgram(install / "wetwaresuite", "0.0.1");
        write(inner / "wetwaresuite", "#!/bin/sh\nexit 3\n");
        fs::remove(zip);
        z = findProgram("zip") ? runProgram({"sh", "-c", "cd '" + (rel / "stage").string() + "' && zip -qr '" + zip.string() + "' ."})
                               : runProgram({"python3", "-m", "zipfile", "-c", zip.string(), inner.string()});
        feed(up::sha256File(zip));
        fs::remove_all(up::updatesDir() / "v99.0.0");
        Result broken = s.run("app.update_install", {});
        CHECK(!broken.ok && broken.message.find("didn't start") != std::string::npos);
        CHECK(runProgram({(install / "wetwaresuite").string(), "--version"}).output.find("0.0.1") != std::string::npos);

        // offline: a plain message, and the background run waits an hour before trying again
        setEnv("WETWARESUITE_UPDATE_URL", fileUrl(rel / "missing.json"));
        Result off = s.run("app.update_check", {});
        CHECK(!off.ok && off.message.find("tries again later") != std::string::npos);
        std::string why;
        CHECK(!up::due(up::Settings{}, up::loadState(), up::detectInstall(install / "wetwaresuite-image", ""), "0.0.1",
                       static_cast<std::int64_t>(std::time(nullptr)), why));
        setEnv("WETWARESUITE_UPDATE_URL", "");
        setEnv("WETWARESUITE_UPDATE_EXE", "");
    }
#endif

    fs::remove_all(root);
    return check::summary("update");
}
