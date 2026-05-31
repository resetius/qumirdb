#include <qdb/exec/planner.h>
#include <qdb/io/parquet/source.h>
#include <qdb/io/text/sink.h>
#include <qdb/ops/filter.h>
#include <qdb/ops/project.h>
#include <qdb/ops/source.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct TFormatSpec {
    std::string Name = "console";
    char Separator = ',';
};

TFormatSpec ParseFormat(const std::string& spec) {
    TFormatSpec result;
    if (!spec.starts_with('<')) {
        result.Name = spec;
        return result;
    }
    auto close = spec.find('>');
    if (close == std::string::npos) {
        throw std::invalid_argument("unclosed '<' in format spec");
    }
    std::string opts = spec.substr(1, close - 1);
    auto eq = opts.find('=');
    if (eq != std::string::npos) {
        std::string key = opts.substr(0, eq);
        std::string val = opts.substr(eq + 1);
        if (key == "separator") {
            if (val.size() != 1) {
                throw std::invalid_argument("separator must be a single character");
            }
            result.Separator = val[0];
        }
    }
    result.Name = spec.substr(close + 1);
    return result;
}

std::vector<std::string> SplitComma(const std::string& s) {
    std::vector<std::string> result;
    std::istringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        result.push_back(token);
    }
    return result;
}

NQqb::TSchema SchemaFromType(
    const NQumir::NAst::TTypePtr& type,
    std::vector<std::string>& names,
    std::vector<NQqb::TColumnSchema>& columns)
{
    auto* structType = static_cast<NQumir::NAst::TStructType*>(type.get());
    if (!structType) {
        throw std::runtime_error("executor output type must be TStructType");
    }

    names.clear();
    columns.clear();
    names.reserve(structType->Fields.size());
    columns.reserve(structType->Fields.size());
    for (const auto& [name, fieldType] : structType->Fields) {
        names.push_back(name);
    }
    for (size_t i = 0; i < structType->Fields.size(); ++i) {
        columns.push_back({
            .Name = names[i],
            .Type = structType->Fields[i].second,
        });
    }
    return NQqb::TSchema{std::span<const NQqb::TColumnSchema>(columns)};
}

} // namespace

int main(int argc, char** argv) {
    std::string inputFile;
    TFormatSpec formatSpec;
    int maxRowSets = -1;
    std::string filterPredicate;
    std::vector<std::string> projectCols;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-i") || !std::strcmp(argv[i], "--input-file")) {
            if (i + 1 < argc) {
                inputFile = argv[++i];
            } else {
                std::cerr << argv[i] << " requires a filename argument\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--format")) {
            if (i + 1 < argc) {
                try {
                    formatSpec = ParseFormat(argv[++i]);
                } catch (const std::invalid_argument& e) {
                    std::cerr << "Invalid format spec: " << e.what() << "\n";
                    return 1;
                }
                if (formatSpec.Name != "console" && formatSpec.Name != "csv") {
                    std::cerr << "Unknown format: " << formatSpec.Name << " (use console or csv)\n";
                    return 1;
                }
            } else {
                std::cerr << "--format requires an argument\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--rowsets")) {
            if (i + 1 < argc) {
                maxRowSets = std::atoi(argv[++i]);
                if (maxRowSets <= 0) {
                    std::cerr << "--rowsets requires a positive integer\n";
                    return 1;
                }
            } else {
                std::cerr << "--rowsets requires an argument\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--filter")) {
            if (i + 1 < argc) {
                filterPredicate = argv[++i];
            } else {
                std::cerr << "--filter requires a predicate argument\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--project")) {
            if (i + 1 < argc) {
                projectCols = SplitComma(argv[++i]);
            } else {
                std::cerr << "--project requires a column list argument\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            std::cout << "qdb [options]\n"
                         "Options:\n"
                         "  -i|--input-file <file>       Input parquet file\n"
                         "  --filter '<expr>'            Filter predicate in core-lang\n"
                         "                               Example: --filter '(> o_totalprice (: 1000.0 f64))'\n"
                         "  --project col1,col2,...      Project (select) columns\n"
                         "  --format <spec>              Output format (default: console)\n"
                         "    console                    Aligned table (PostgreSQL style)\n"
                         "    csv                        CSV with default separator ','\n"
                         "    <separator=X>csv           CSV with custom separator X\n"
                         "  --rowsets <n>                Stop after n rowsets\n"
                         "  --help|-h                    Show this help message\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            return 1;
        }
    }

    if (inputFile.empty()) {
        std::cerr << "No input file specified. Use -i <file.parquet>\n";
        return 1;
    }

    try {
        NQqb::TParquetSource source(inputFile);

        // Build logical plan
        NQqb::TOperatorPtr plan = std::make_shared<NQqb::TSourceOperator>(source);

        if (!filterPredicate.empty()) {
            auto result = NQqb::MakeFilter(plan, filterPredicate);
            if (!result) {
                std::cerr << "Filter error: " << result.error().ToString() << "\n";
                return 1;
            }
            plan = std::move(*result);
        }

        if (!projectCols.empty()) {
            std::vector<NQqb::TProjectionSpec> specs;
            for (const auto& col : projectCols) {
                specs.push_back({col, col}); // expression = column name (bare identifier)
            }
            auto result = NQqb::MakeProject(plan, std::move(specs));
            if (!result) {
                std::cerr << "Project error: " << result.error().ToString() << "\n";
                return 1;
            }
            plan = std::move(*result);
        }

        // Build physical plan
        NQqb::TPhysicalPlanner planner;
        auto executor = planner.Build(plan);

        std::vector<std::string> outputNames;
        std::vector<NQqb::TColumnSchema> outputColumns;
        auto schema = SchemaFromType(executor->OutputType(), outputNames, outputColumns);
        std::unique_ptr<NQqb::ISink> sink;
        if (formatSpec.Name == "csv") {
            sink = std::make_unique<NQqb::TCsvSink>(schema, std::cout, formatSpec.Separator);
        } else {
            sink = std::make_unique<NQqb::TConsoleSink>(schema, std::cout);
        }

        NQqb::TRowSet rowSet = {};
        int count = 0;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        while ((maxRowSets < 0 || count < maxRowSets) && executor->Next(rowSet)) {
            sink->Write(rowSet);
            NQqb::Release(&rowSet);
            ++count;
        }
        sink->Flush();
        std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = end - start;
        std::cerr << "Processed " << count << " rowsets in " << elapsed.count() << " seconds\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    return 0;
}
