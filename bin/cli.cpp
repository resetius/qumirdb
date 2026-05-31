#include <qdb/io/parquet/source.h>
#include <qdb/io/text/sink.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

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

} // namespace

int main(int argc, char** argv) {
    std::string inputFile;
    TFormatSpec formatSpec;
    int maxRowSets = -1;

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
        } else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            std::cout << "qdb [options]\n"
                         "Options:\n"
                         "  -i|--input-file <file>       Input parquet file\n"
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

        std::unique_ptr<NQqb::ISink> sink;
        if (formatSpec.Name == "csv") {
            sink = std::make_unique<NQqb::TCsvSink>(source.Schema(), std::cout, formatSpec.Separator);
        } else {
            sink = std::make_unique<NQqb::TConsoleSink>(source.Schema(), std::cout);
        }

        NQqb::TRowSet rowSet = {};
        int count = 0;
        while ((maxRowSets < 0 || count < maxRowSets) && source.Next(rowSet)) {
            sink->Write(rowSet);
            NQqb::Release(&rowSet);
            ++count;
        }
        sink->Flush();
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    return 0;
}
