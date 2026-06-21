#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <qdb/sql/parser.h>
#include <qdb/sql/printer.h>

namespace fs = std::filesystem;

using namespace NQdb;

namespace {

fs::path RootDir = "regtest";
fs::path CasesDir = RootDir / "cases";
fs::path GoldensDir = RootDir / "goldens";

bool updateGoldens = false;
bool printOutput = false;

std::string ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

void WriteAll(const fs::path& p, const std::string& s) {
    std::cerr << "Updating golden file: " << p << "\n";
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << s;
}

struct ProgCase { fs::path base; }; // base without extension

std::vector<ProgCase> Collect(const fs::path& root, std::string_view extension = ".sql") {
    std::vector<ProgCase> v;
    if (!fs::exists(root)) {
        return v;
    }
    auto casePath = [&](const fs::path& p) {
        auto rel = fs::relative(p, root);
        return rel.replace_extension();
    };
    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (e.is_regular_file() && e.path().extension() == extension) {
            v.push_back({ casePath(e.path()) });
        }
    }
    std::sort(v.begin(), v.end(),
        [](auto& a, auto& b){ return a.base.string() < b.base.string(); });
    return v;
}

std::string NameFromPath(const fs::path& p) {
    std::string s = p.string();
    for (auto& c : s) {
        if (c == '/' || c == '\\' || c == '.') {
            c = '_';
        }
    }
    return s;
}

std::string BuildAst(std::istream& in) {
    NSql::TTokenStream ts(in);
    NSql::TParser parser;

    auto parsed = parser.Parse(ts);
    if (!parsed) {
        return parsed.error().ToString() + "\n";
    }
    return NSql::PrintAst(parsed.value());
}

} // namespace

class RegAst : public ::testing::TestWithParam<ProgCase> {};

TEST_P(RegAst, Ast) {
    const fs::path src = fs::path(CasesDir / GetParam().base).replace_extension(".sql");
    const fs::path golden = fs::path(GoldensDir / GetParam().base).replace_extension(".ast");

    std::istringstream input(ReadAll(src));
    std::string got = BuildAst(input);

    if (printOutput) {
        std::cout << "=== Output AST for " << src << " ===\n";
        std::cout << got << "\n";
        std::cout << "=== End of output ===\n";
    }

    if (updateGoldens) {
        WriteAll(golden, got);
    }
    if (!fs::exists(golden)) {
        std::cerr << "Missing golden AST file: " << golden << "\n";
        FAIL();
    }
    const auto exp = ReadAll(golden);
    EXPECT_EQ(got, exp);
}

INSTANTIATE_TEST_SUITE_P(
    RegProgAst,
    RegAst,
    ::testing::ValuesIn(Collect(CasesDir)),
    [](const ::testing::TestParamInfo<ProgCase>& i){ return "AST_" + NameFromPath(i.param.base); });

int main(int argc, char** argv) {
    if (argc > 1 && argv[1][0] != '-') {
        RootDir = argv[1];
        CasesDir = RootDir / "cases";
        GoldensDir = RootDir / "goldens";
    }
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--canonize")) {
            updateGoldens = true;
        } else if (!strcmp(argv[i], "--print")) {
            printOutput = true;
        }
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
