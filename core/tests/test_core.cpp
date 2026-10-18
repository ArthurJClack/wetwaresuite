#include "wetwaresuite/core/command.hpp"
#include "check.hpp"
#include "wetwaresuite/core/process.hpp"

using namespace wetwaresuite;

namespace {

// Adds `amount` to a shared counter; undo subtracts it again.
class AddAction : public Action {
public:
    AddAction(int& counter, std::int64_t amount) : counter_(counter), amount_(amount) {}
    Result apply() override {
        counter_ += static_cast<int>(amount_);
        return Result::success("added", static_cast<std::int64_t>(counter_));
    }
    void revert() override { counter_ -= static_cast<int>(amount_); }

private:
    int& counter_;
    std::int64_t amount_;
};

void registerAdd(Session& session, int& counter) {
    session.registry().add(
        {"test.add", "Add", "adds to the counter", {{"amount", ParamType::Int, "how much", true}}, true},
        [&counter](const Params& p) {
            return std::make_unique<AddAction>(counter, std::get<std::int64_t>(p.at("amount")));
        });
}

}  // namespace

int main() {
    int counter = 0;
    Session session;
    registerAdd(session, counter);

    CHECK(session.run("test.add", {{"amount", std::int64_t{5}}}).ok);
    CHECK(session.run("test.add", {{"amount", std::int64_t{2}}}).ok);
    CHECK(counter == 7);
    CHECK(session.history().labels().size() == 2);

    CHECK(session.history().undo());
    CHECK(counter == 5);
    CHECK(session.history().redo());
    CHECK(counter == 7);

    // a new action after undo clears the redo stack
    CHECK(session.history().undo());
    CHECK(session.run("test.add", {{"amount", std::int64_t{10}}}).ok);
    CHECK(counter == 15);
    CHECK(!session.history().canRedo());

    // validation errors come back as failed results, not exceptions
    CHECK(!session.run("test.missing", {}).ok);
    CHECK(!session.run("test.add", {}).ok);
    CHECK(!session.run("test.add", {{"amount", std::string("five")}}).ok);
    CHECK(!session.run("test.add", {{"amount", std::int64_t{1}}, {"extra", true}}).ok);
    CHECK(counter == 15);

    // history respects its limit
    History small(2);
    int c = 0;
    for (int i = 0; i < 3; ++i) small.execute(std::make_unique<AddAction>(c, 1), "step", true);
    CHECK(small.labels().size() == 2);

    // a program in the background: output as it arrives, input, exit codes, stopping it
    {
        ChildProcess child;
        std::string error;
#ifdef _WIN32
        const std::vector<std::string> echo = {"cmd", "/d", "/c", "echo hello& exit 3"};
        const std::vector<std::string> cat = {"findstr", "x"};
        const std::vector<std::string> sleeper = {"ping", "-n", "30", "127.0.0.1"};
#else
        const std::vector<std::string> echo = {"sh", "-c", "echo hello; exit 3"};
        const std::vector<std::string> cat = {"cat"};
        const std::vector<std::string> sleeper = {"sleep", "30"};
#endif
        CHECK(child.start(echo, {}, error));
        std::string out;
        CHECK(child.wait(20, out));
        CHECK(out.find("hello") != std::string::npos);
        CHECK(child.exitCode() == 3);
        CHECK(!child.running());

        ChildProcess input;
        CHECK(input.start(cat, {}, error));
        CHECK(input.write("x marks\n"));
        input.closeInput();
        std::string echoed;
        CHECK(input.wait(20, echoed));
        CHECK(echoed.find("x marks") != std::string::npos);

        ChildProcess slow;
        CHECK(slow.start(sleeper, std::filesystem::temp_directory_path(), error));
        CHECK(slow.running());
        slow.kill();
        CHECK(!slow.running());

        ChildProcess missing;
        CHECK(!missing.start({"no-such-program-wetwaresuite"}, {}, error));
        CHECK(!error.empty());
    }

    return check::summary("core");
}
