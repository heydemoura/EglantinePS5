// SPDX-License-Identifier: GPL-3.0-or-later
#include "profile_backend.h"
#include "common/fs/path_util.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>


int main() {
    using namespace Eden;
    using Service::AM::Frontend::UiMode;
    char directory[] = "/tmp/eden-profile-applet-XXXXXX";
    assert(mkdtemp(directory));
    const std::filesystem::path root{directory};
    const auto file = (root / "nand/system/save/8000000000000010/su/avators/profiles.dat").string();
    const auto settings = (root / "settings.json").string();
    assert(Profiles::Write({Profiles::Make("Existing")}, file));
    Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, root / "nand");
    Service::Account::ProfileManager manager;
    const auto original = manager.GetUser(0).value();
    const auto create = [&](const std::string& name) { return CreateProfileUser(manager, name, file, settings); };
    const auto read = [&] { return ReadProfileUsers(manager); };
    assert(!create("Existing"));
    assert(!create(" "));
    assert(!CreateProfileUser(manager, "Missing", file + ".absent", settings));
    assert(manager.GetUserCount() == 1);
    const auto bytes = [&] {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const auto original_bytes = bytes();
    std::filesystem::create_directory(file + ".new");
    assert(!create("Cannot publish"));
    assert(bytes() == original_bytes && manager.GetUserCount() == 1);
    std::filesystem::remove_all(file + ".new");
    std::filesystem::create_directory(settings + ".tmp");
    assert(!create("Cannot seed settings"));
    assert(bytes() == original_bytes && manager.GetUserCount() == 1);
    std::filesystem::remove_all(settings + ".tmp");

    auto run = [&](Core::Frontend::ProfileSelectParameters parameters, TextAnswer answer,
                   std::optional<Common::UUID> expected, bool expect_new = false) {
        std::promise<std::optional<Common::UUID>> done;
        auto future = done.get_future();
        SystemProfileApplet app(read, create, [answer](const auto&, const auto&) { return answer; });
        assert(app.PreservesCancellation());
        app.SelectProfile([&](auto result) { app.Close(); done.set_value(result); }, parameters);
        assert(future.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        const auto result = future.get();
        if (expect_new) assert(result && *result != original && manager.UserExists(*result));
        else assert(result == expected);
        return result;
    };
    Core::Frontend::ProfileSelectParameters select{};
    select.mode = UiMode::UserSelector;
    run(select, {TextOutcome::accepted, u"Existing"}, original);
    run(select, {TextOutcome::cancelled, {}}, std::nullopt);
    run(select, {TextOutcome::unavailable, {}}, std::nullopt);
    select.invalid_uid_list[0] = original;
    run(select, {TextOutcome::accepted, u"Existing"}, std::nullopt);
    assert(manager.GetUserCount() == 1);
    select.display_options.is_registration_permitted = true;
    const auto second = run(select, {TextOutcome::accepted, u"New player"}, std::nullopt, true).value();
    assert(manager.GetUser(0) == original && manager.GetUserCount() == 2);
    Service::Account::ProfileManager restarted;
    assert(restarted.UserExists(second) && restarted.UserExists(original));
    Core::Frontend::ProfileSelectParameters creator{};
    creator.mode = UiMode::UserCreator;
    run(creator, {TextOutcome::accepted, u"中文"}, std::nullopt, true);
    run(creator, {TextOutcome::accepted, u"Existing"}, std::nullopt);
    assert(manager.GetUserCount() == 3);
    while (manager.GetUserCount() < 8) assert(create("Extra " + std::to_string(manager.GetUserCount())));
    run(creator, {TextOutcome::accepted, u"Overflow"}, std::nullopt);
    assert(manager.GetUserCount() == 8 && Profiles::Read(file).size() == 8);

    std::promise<void> opened;
    auto entered = opened.get_future();
    std::promise<void> finished;
    auto stopped = finished.get_future();
    std::atomic<bool> replied{};
    SystemProfileApplet app(read, create, [&](const auto&, const auto& stop) {
        opened.set_value();
        while (!stop.load()) std::this_thread::yield();
        finished.set_value();
        return TextAnswer{TextOutcome::accepted, u"Existing"};
    });
    select.invalid_uid_list = {};
    app.SelectProfile([&](auto) { replied = true; }, select);
    entered.wait();
    app.Close();
    assert(stopped.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    assert(!replied);
    // A completion can destroy its own applet; the worker owns its remaining state.
    {
        std::promise<void> destroyed;
        auto completed = destroyed.get_future();
        auto self = std::make_unique<SystemProfileApplet>(read, create,
            [](const auto&, const auto&) { return TextAnswer{TextOutcome::accepted, u"Existing"}; });
        self->SelectProfile([&](auto result) {
            assert(result == original);
            self.reset();
            destroyed.set_value();
        }, select);
        assert(completed.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    }

    // Close from another thread waits for a claimed callback, but that callback can Close too.
    {
        std::promise<void> callback_started, release_callback, closing;
        auto started = callback_started.get_future();
        auto release = release_callback.get_future();
        auto close_started = closing.get_future();
        SystemProfileApplet completing(read, create,
            [](const auto&, const auto&) { return TextAnswer{TextOutcome::accepted, u"Existing"}; });
        completing.SelectProfile([&](auto result) {
            assert(result == original);
            callback_started.set_value();
            release.wait();
            completing.Close();
        }, select);
        assert(started.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        auto closed = std::async(std::launch::async, [&] { closing.set_value(); completing.Close(); });
        close_started.wait();
        assert(closed.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
        release_callback.set_value();
        assert(closed.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    }

    // Replacing an outstanding keyboard request cancels its completion, not the replacement.
    {
        std::promise<void> first_opened, first_stopped, replacement_done;
        auto first_ready = first_opened.get_future();
        auto first_finished = first_stopped.get_future();
        auto replacement = replacement_done.get_future();
        std::atomic<unsigned> requests{};
        std::atomic<bool> obsolete_callback{};
        SystemProfileApplet replacing(read, create, [&](const auto&, const auto& stop) {
            if (requests.fetch_add(1) == 0) {
                first_opened.set_value();
                while (!stop.load()) std::this_thread::yield();
                first_stopped.set_value();
            }
            return TextAnswer{TextOutcome::accepted, u"Existing"};
        });
        replacing.SelectProfile([&](auto) { obsolete_callback = true; }, select);
        first_ready.wait();
        replacing.SelectProfile([&](auto result) {
            assert(result == original);
            replacement_done.set_value();
        }, select);
        assert(replacement.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        assert(first_finished.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        replacing.Close();
        assert(!obsolete_callback);
    }

    // Shutdown keeps services alive until account creation and any claimed completion finish.
    {
        std::atomic<bool> services_alive{true};
        std::promise<void> creating, release_create, closing;
        auto creation_started = creating.get_future();
        auto release = release_create.get_future();
        auto close_started = closing.get_future();
        SystemProfileApplet active([] { return std::vector<SystemProfileApplet::User>{}; },
            [&](const std::string&) -> std::optional<Common::UUID> {
                assert(services_alive.load());
                creating.set_value();
                release.wait();
                assert(services_alive.load());
                return original;
            }, [](const auto&, const auto&) { return TextAnswer{TextOutcome::accepted, u"New"}; });
        active.SelectProfile([&](auto result) {
            assert(services_alive.load() && result == original);
            active.Close();
        }, creator);
        assert(creation_started.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        auto shutdown = std::async(std::launch::async, [&] {
            closing.set_value();
            active.Shutdown();
            services_alive.store(false);
        });
        close_started.wait();
        assert(shutdown.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
        release_create.set_value();
        assert(shutdown.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        assert(!services_alive.load());
    }

    // Terminal shutdown rejects later requests, including a request racing service teardown.
    {
        std::atomic<unsigned> reads{}, asks{}, creates{}, callbacks{};
        SystemProfileApplet stopped(
            [&] { ++reads; return std::vector<SystemProfileApplet::User>{}; },
            [&](const std::string&) -> std::optional<Common::UUID> { ++creates; return original; },
            [&](const auto&, const auto&) { ++asks; return TextAnswer{TextOutcome::accepted, u"New"}; });
        const Core::Frontend::ProfileSelectApplet& frontend = stopped;
        frontend.Shutdown();
        stopped.Close(); // Ordinary cancellation cannot reopen a stopped session.
        stopped.SelectProfile([&](auto) { ++callbacks; }, creator);
        assert(reads == 0 && asks == 0 && creates == 0 && callbacks == 0);
    }
    {
        std::atomic<bool> services_alive{true};
        std::promise<void> reading, release_read, shutting_down;
        auto read_started = reading.get_future();
        auto release = release_read.get_future();
        auto shutdown_started = shutting_down.get_future();
        std::atomic<unsigned> callbacks{};
        SystemProfileApplet racing([&] {
                assert(services_alive.load());
                reading.set_value();
                release.wait();
                assert(services_alive.load());
                return std::vector<SystemProfileApplet::User>{};
            }, [&](const std::string&) -> std::optional<Common::UUID> {
                assert(services_alive.load());
                return original;
            }, [](const auto&, const auto& stop) {
                while (!stop.load()) std::this_thread::yield();
                return TextAnswer{TextOutcome::accepted, u"New"};
            });
        auto select_request = std::async(std::launch::async, [&] {
            racing.SelectProfile([&](auto) { ++callbacks; }, creator);
        });
        assert(read_started.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        auto shutdown = std::async(std::launch::async, [&] {
            shutting_down.set_value();
            racing.Shutdown();
            services_alive.store(false);
        });
        shutdown_started.wait();
        assert(shutdown.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
        release_read.set_value();
        assert(select_request.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        assert(shutdown.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        racing.SelectProfile([&](auto) { ++callbacks; }, creator);
        assert(!services_alive.load() && callbacks == 0);
    }

    // A keyboard reply arriving after shutdown must not create an account or call into HLE.
    {
        struct Lifetime {
            std::promise<void> released;
            ~Lifetime() { released.set_value(); }
        };
        auto lifetime = std::make_shared<Lifetime>();
        auto released = lifetime->released.get_future();
        std::promise<void> keyboard_opened, release_keyboard;
        auto keyboard_ready = keyboard_opened.get_future();
        auto reply = release_keyboard.get_future();
        std::atomic<bool> services_alive{true};
        std::atomic<bool> late_callback{};
        auto pending = std::make_unique<SystemProfileApplet>(
            [] { return std::vector<SystemProfileApplet::User>{}; },
            [&](const std::string&) -> std::optional<Common::UUID> {
                assert(services_alive.load());
                return original;
            }, [&, lifetime](const auto&, const auto&) {
                keyboard_opened.set_value();
                reply.wait();
                return TextAnswer{TextOutcome::accepted, u"New"};
            });
        lifetime.reset();
        pending->SelectProfile([&](auto) { late_callback = true; }, creator);
        keyboard_ready.wait();
        pending->Shutdown();
        services_alive.store(false);
        pending.reset();
        release_keyboard.set_value();
        assert(released.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        assert(!late_callback);
    }
    std::filesystem::remove_all(root);
    std::cout << "Profile applet: selection, exclusions, creation, persistence, capacity, cancellation, callback lifetime and shutdown ordering PASS\n";
}
