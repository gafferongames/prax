#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/text.h"

#include "mermaid/src/mermaid_gitgraph.h"
#include "mermaid/tests/mermaid_tests.h"

#include "tools/harness/test.h"

#include <chrono>
#include <string>

namespace pr_impl {

namespace {

// A parsed git graph, freed when it goes out of scope.
struct Parsed {
    MermaidGitGraph graph{};
    PrMermaidError error{};
    PrStatus status = PrStatus_Ok;

    explicit Parsed(const char* source) {
        status = mermaidGitGraphParse(graph, source, &error);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    ~Parsed() {
        mermaidGitGraphReset(graph);
    }

    std::string text(MermaidSpan span) const {
        PrStringView view = mermaidGitGraphText(graph, span);
        return std::string(view.data, view.size);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("mermaid git graph: commands build commits on branches, with merges and cherry-picks") {
    Parsed parsed("gitGraph\n"
        "commit\ncommit id: \"Normal\" tag: \"v1\"\nbranch develop order: 3\ncommit id: \"Dev\" type: HIGHLIGHT\n"
        "checkout main\ncommit type: REVERSE\nmerge develop tag: \"v2\"\ncherry-pick id: \"Dev\"\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.graph.branches.count == 2);
    CHECK(parsed.graph.branches.data[1].order == 3);
    REQUIRE(parsed.graph.commits.count == 6);
    const MermaidCommit* c = parsed.graph.commits.data;
    CHECK(parsed.text(c[0].id).size() > 2);
    CHECK_FALSE(c[0].customId);
    CHECK(c[1].customId);
    CHECK(parsed.text(parsed.graph.tags.data[c[1].firstTag]) == "v1");
    CHECK(c[2].branch == 1);
    CHECK(c[2].parents[0] == 1);
    CHECK(c[2].type == MermaidCommitType_Highlight);
    CHECK(c[3].type == MermaidCommitType_Reverse);
    CHECK(c[4].merge);
    CHECK(c[4].type == MermaidCommitType_Merge);
    CHECK(c[4].parents[0] == 3);
    CHECK(c[4].parents[1] == 2);
    CHECK(c[5].type == MermaidCommitType_CherryPick);
    CHECK(c[5].parents[1] == 2);
    CHECK(parsed.text(parsed.graph.tags.data[c[5].firstTag]) == "cherry-pick:Dev");
    // IDs that the source does not give are the same each time.
    Parsed again("gitGraph\ncommit\n");
    CHECK(again.text(again.graph.commits.data[0].id) == parsed.text(c[0].id));
}

TEST_CASE("mermaid git graph: commands that mermaid.js forbids fail where they are") {
    const char* cases[] = {
        "gitGraph\ncommit\nbranch main\n",
        "gitGraph\ncommit\ncheckout nowhere\n",
        "gitGraph\ncommit\nmerge main\n",
        "gitGraph\ncommit id: \"A\"\ncherry-pick id: \"A\"\n",
        "gitGraph\ncommit\ncommit id: \"x\" type: ODD\n",
        "gitGraph\ncommit\nfrobnicate\n",
    };
    for (const char* source : cases) {
        INFO(source);
        Parsed parsed(source);
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == 3);
    }
}

TEST_CASE("mermaid git graph: a commit's ID, given or made, names it, and the first commit with an ID keeps it") {
    Parsed made("gitGraph\ncommit\ncommit\n");
    REQUIRE(made.status == PrStatus_Ok);
    std::string second = made.text(made.graph.commits.data[1].id);
    // A given ID that the second commit would make for itself leaves both commits that ID, and names the first.
    std::string source = "gitGraph\ncommit id: \"" + second + "\"\ncommit\nbranch side\ncherry-pick id: \"" + second + "\"\n";
    Parsed parsed(source.c_str());
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.graph.commits.count == 3);
    CHECK(parsed.text(parsed.graph.commits.data[1].id) == second);
    CHECK(parsed.graph.commits.data[2].parents[1] == 0);
    Parsed quoted("gitGraph\nbranch \"a b\"\ncommit\ncheckout main\ncommit\nmerge \"a b\"\n");
    REQUIRE(quoted.status == PrStatus_Ok);
    CHECK(quoted.graph.commits.data[2].parents[1] == 0);
}

TEST_CASE("mermaid git graph: many branches, commits and merges lay out in time linear in the commands") {
    // Each merge's line from its branch detours past the commits on the main lane.
    constexpr int32_t kBranches = 1000;
    std::string source = "gitGraph\ncommit\n";
    for (int32_t b = 0; b < kBranches; ++b)
        source += "branch b" + std::to_string(b) + "\ncommit id: \"c" + std::to_string(b) + "\"\ncheckout main\n";
    source += "commit\n";
    for (int32_t b = 0; b < kBranches; ++b)
        source += "merge b" + std::to_string(b) + "\ncherry-pick id: \"c" + std::to_string(kBranches - 1 - b) + "\"\n";
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    MermaidDrawn drawn(source);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    CHECK(drawn.status == PrStatus_Ok);
    CHECK(seconds < 10);
}
