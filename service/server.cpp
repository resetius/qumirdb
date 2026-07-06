#include <coroio/all.hpp>
#include <coroio/http/httpd.hpp>
#include <coroio/pipe/pipe.hpp>

#include <qdb/io/parquet/source.h>

#include <qumir/parser/core/printer.h>

#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <unistd.h>

using namespace NNet;

namespace {

struct TOptions {
    std::function<TPipe(const std::string&, const std::vector<std::string>&, bool)> PipeFactory;
    std::string StaticDir = QDB_SERVICE_STATIC_DIR;
    std::string BinaryDir = QDB_BUILD_BIN_DIR;
    std::vector<std::string> DataDirs;
};

struct TServerDataset {
    std::string Id;
    std::string Name;
    std::filesystem::path Path;
    llvm::json::Object Json;
};

struct TRunStats {
    bool Found = false;
    int64_t Rows = 0;
    double Seconds = 0.0;
};

TRunStats ParseRunStats(std::string_view stderrText) {
    static constexpr std::string_view prefix = "Returned ";
    auto pos = stderrText.rfind(prefix);
    if (pos == std::string_view::npos) {
        return {};
    }
    pos += prefix.size();

    auto rowsEnd = stderrText.find(" rows in ", pos);
    if (rowsEnd == std::string_view::npos) {
        return {};
    }
    auto secondsBegin = rowsEnd + std::string_view(" rows in ").size();
    auto secondsEnd = stderrText.find(" seconds", secondsBegin);
    if (secondsEnd == std::string_view::npos) {
        return {};
    }

    TRunStats stats;
    std::istringstream rowsIn(std::string(stderrText.substr(pos, rowsEnd - pos)));
    std::istringstream secondsIn(
        std::string(stderrText.substr(secondsBegin, secondsEnd - secondsBegin)));
    if ((rowsIn >> stats.Rows) && (secondsIn >> stats.Seconds)) {
        stats.Found = true;
    }
    return stats;
}

std::string JsonEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 16);
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[ch >> 4];
                    out += hex[ch & 0x0f];
                } else {
                    out += static_cast<char>(ch);
                }
                break;
        }
    }
    return out;
}

std::string ToJsonString(llvm::json::Value value) {
    std::string out;
    llvm::raw_string_ostream os(out);
    os << value;
    return out;
}

llvm::json::Object ErrorJson(std::string stage, std::string message) {
    return llvm::json::Object{
        {"ok", false},
        {"error", llvm::json::Object{
            {"stage", std::move(stage)},
            {"message", std::move(message)},
        }},
    };
}

std::string BaseName(const std::filesystem::path& path) {
    auto name = path.filename().string();
    return name.empty() ? path.string() : name;
}

std::string TableNameFromPath(const std::filesystem::path& path) {
    auto name = path.filename().string();
    const std::string suffix = ".parquet";
    if (name.ends_with(suffix)) {
        name.resize(name.size() - suffix.size());
    }
    return name;
}

} // namespace

class TRouter final : public IRouter {
public:
    explicit TRouter(TOptions options)
        : Options_(std::move(options))
    {
        std::error_code ec;
        StaticBase_ = std::filesystem::weakly_canonical(Options_.StaticDir, ec);
        if (ec || StaticBase_.empty()) {
            StaticBase_ = std::filesystem::path(Options_.StaticDir).lexically_normal();
        }
        BinaryBase_ = std::filesystem::weakly_canonical(Options_.BinaryDir, ec);
        if (ec || BinaryBase_.empty()) {
            BinaryBase_ = std::filesystem::path(Options_.BinaryDir).lexically_normal();
        }
        BuildDatasets();
    }

    TFuture<void> HandleRequest(TRequest& request, TResponse& response) override {
        if (request.Method() == "OPTIONS") {
            response.SetStatus(200);
            response.SetHeader("Access-Control-Allow-Origin", "*");
            response.SetHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            response.SetHeader("Access-Control-Allow-Headers", "Content-Type");
            response.SetHeader("Content-Length", "0");
            co_await response.SendHeaders();
            co_return;
        }

        if (request.Method() == "GET") {
            co_await Get(request, response);
            co_return;
        }

        if (request.Method() == "POST") {
            co_await Post(request, response);
            co_return;
        }

        response.SetStatus(405);
        co_await SendText(response, "Method Not Allowed", "text/plain; charset=utf-8");
    }

private:
    TFuture<void> Get(const TRequest& request, TResponse& response) {
        const auto& path = request.Uri().Path();
        if (path == "/api/version") {
            co_await SendJson(response,
                "{\"name\":\"QumirDB Workbench\",\"version\":1}");
            co_return;
        }
        if (path == "/api/datasets") {
            co_await SendJson(response, ServerDatasetsJson());
            co_return;
        }
        co_await ServeStatic(response, path);
    }

    TFuture<void> Post(TRequest& request, TResponse& response) {
        const auto& path = request.Uri().Path();
        if (path == "/api/explain") {
            co_await Explain(request, response);
            co_return;
        }
        if (path == "/api/run") {
            co_await Run(request, response);
            co_return;
        }
        response.SetStatus(404);
        co_await SendText(response, "Not Found", "text/plain; charset=utf-8");
    }

    TFuture<void> Explain(TRequest& request, TResponse& response) {
        std::string body = co_await request.ReadBodyFull();
        auto exporter = (BinaryBase_ / "qdb_plan_export").generic_string();
        auto pipe = Options_.PipeFactory(
            exporter,
            {"--stdin-json", "--stdout-json"},
            /*stderrToStdout=*/true);

        co_await TByteWriter(pipe).Write(body.data(), body.size());
        pipe.CloseWrite();

        std::string output;
        char buf[4096];
        auto reader = TByteReader(pipe);
        for (;;) {
            ssize_t n = co_await reader.ReadSome(buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            output.append(buf, static_cast<size_t>(n));
        }

        const int exitCode = pipe.Wait();
        if (exitCode != 0) {
            std::string json =
                "{\"ok\":false,\"error\":{\"stage\":\"export\","
                "\"message\":\"qdb_plan_export failed with code " +
                std::to_string(exitCode) + "\",\"output\":\"" +
                JsonEscape(output) + "\"}}";
            co_await SendJson(response, json, 500);
            co_return;
        }

        co_await SendJson(response, output);
    }

    TFuture<void> Run(TRequest& request, TResponse& response) {
        std::string body = co_await request.ReadBodyFull();
        auto parsed = llvm::json::parse(body);
        if (!parsed) {
            co_await SendJson(response,
                ToJsonString(ErrorJson("request", llvm::toString(parsed.takeError()))),
                400);
            co_return;
        }

        const auto* root = parsed->getAsObject();
        auto sql = root ? root->getString("sql") : std::optional<llvm::StringRef>{};
        const auto* datasetValue = root ? root->get("dataset") : nullptr;
        const auto* dataset = datasetValue ? datasetValue->getAsObject() : nullptr;
        auto datasetId = dataset ? dataset->getString("id") : std::optional<llvm::StringRef>{};
        if (!root || !sql || sql->trim().empty() || !datasetId) {
            co_await SendJson(response,
                ToJsonString(ErrorJson("request", "run requires sql and dataset.id")),
                400);
            co_return;
        }

        auto* serverDataset = FindDataset(*datasetId);
        if (!serverDataset) {
            co_await SendJson(response,
                ToJsonString(ErrorJson(
                    "run",
                    "selected dataset is not a server-side dataset")),
                400);
            co_return;
        }

        auto queryFile = TempSqlPath();
        {
            std::ofstream out(queryFile);
            out << std::string(*sql);
            if (!sql->ends_with("\n")) {
                out << "\n";
            }
        }

        auto qdb = (BinaryBase_ / "qdb").generic_string();
        const auto started = std::chrono::steady_clock::now();
        auto pipe = Options_.PipeFactory(
            qdb,
            {
                "--sql",
                "--data", serverDataset->Path.string(),
                "--format", "<escape>csv",
                "-i", queryFile.string(),
            },
            /*stderrToStdout=*/false);
        pipe.CloseWrite();

        std::string output;
        char buf[8192];
        for (;;) {
            ssize_t n = co_await pipe.ReadSome(buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            output.append(buf, static_cast<size_t>(n));
        }

        std::string errorOutput;
        for (;;) {
            ssize_t n = co_await pipe.ReadSomeErr(buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            errorOutput.append(buf, static_cast<size_t>(n));
        }

        const int exitCode = pipe.Wait();
        std::error_code ec;
        std::filesystem::remove(queryFile, ec);

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (exitCode != 0) {
            co_await SendJson(response,
                ToJsonString(llvm::json::Object{
                    {"ok", false},
                    {"error", llvm::json::Object{
                        {"stage", "run"},
                        {"message", "QumirDB failed with code " + std::to_string(exitCode)},
                        {"output", output},
                        {"stderr", errorOutput},
                    }},
                }),
                500);
            co_return;
        }

        auto runStats = ParseRunStats(errorOutput);
        llvm::json::Object result{
            {"ok", true},
            {"format", "csv"},
            {"csv", std::move(output)},
            {"stderr", std::move(errorOutput)},
            {"elapsedMs", elapsed},
        };
        if (runStats.Found) {
            result["processingMs"] = static_cast<int64_t>(runStats.Seconds * 1000.0);
            result["processingSeconds"] = runStats.Seconds;
            result["rows"] = runStats.Rows;
        }
        co_await SendJson(response, ToJsonString(std::move(result)));
    }

    void BuildDatasets() {
        namespace fs = std::filesystem;
        ServerDatasets_.clear();
        size_t index = 0;
        for (const auto& dirValue : Options_.DataDirs) {
            std::error_code ec;
            fs::path dir = fs::weakly_canonical(dirValue, ec);
            if (ec || dir.empty()) {
                dir = fs::path(dirValue).lexically_normal();
            }
            if (!fs::exists(dir) || !fs::is_directory(dir)) {
                std::cerr << "[QumirDB Web] skip data dir: " << dir << "\n";
                continue;
            }

            TServerDataset dataset;
            dataset.Id = "server:" + std::to_string(index++);
            dataset.Name = BaseName(dir);
            dataset.Path = dir;
            dataset.Json = BuildDatasetJson(dataset);
            ServerDatasets_.push_back(std::move(dataset));
        }
    }

    llvm::json::Object BuildDatasetJson(const TServerDataset& dataset) const {
        namespace fs = std::filesystem;
        llvm::json::Array tables;
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(dataset.Path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".parquet") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());

        for (const auto& file : files) {
            const auto tableName = TableNameFromPath(file);
            try {
                NQdb::TParquetSource source(file.string());
                llvm::json::Array columns;
                for (const auto& column : source.Schema().Columns) {
                    columns.push_back(llvm::json::Object{
                        {"name", std::string(column.Name)},
                        {"type", NQumir::NAst::NCore::PrintType(column.Type)},
                    });
                }

                int64_t rows = 0;
                int64_t bytes = 0;
                auto rowGroups = source.ScanRowGroups();
                for (const auto& rowGroup : rowGroups) {
                    rows += rowGroup.RowCount;
                    bytes += rowGroup.ByteSize;
                }

                tables.push_back(llvm::json::Object{
                    {"name", tableName},
                    {"columns", std::move(columns)},
                    {"stats", llvm::json::Object{
                        {"rows", rows},
                        {"bytes", bytes},
                        {"rowGroups", static_cast<int64_t>(rowGroups.size())},
                    }},
                });
            } catch (const std::exception& e) {
                std::cerr << "[QumirDB Web] failed to read schema for "
                          << file << ": " << e.what() << "\n";
            }
        }

        return llvm::json::Object{
            {"id", dataset.Id},
            {"name", dataset.Name},
            {"source", llvm::json::Object{
                {"kind", "server"},
                {"path", dataset.Path.string()},
            }},
            {"tables", std::move(tables)},
        };
    }

    std::string ServerDatasetsJson() const {
        llvm::json::Array datasets;
        for (const auto& dataset : ServerDatasets_) {
            datasets.push_back(llvm::json::Object(dataset.Json));
        }
        return ToJsonString(llvm::json::Object{
            {"ok", true},
            {"datasets", std::move(datasets)},
        });
    }

    TServerDataset* FindDataset(llvm::StringRef id) {
        for (auto& dataset : ServerDatasets_) {
            if (dataset.Id == id) {
                return &dataset;
            }
        }
        return nullptr;
    }

    std::filesystem::path TempSqlPath() const {
        static std::atomic<uint64_t> nextId{0};
        auto name = "qumirdb-web-" + std::to_string(::getpid()) + "-" +
            std::to_string(nextId.fetch_add(1, std::memory_order_relaxed)) + ".sql";
        return std::filesystem::temp_directory_path() / name;
    }

    TFuture<void> ServeStatic(TResponse& response, std::string uriPath) {
        namespace fs = std::filesystem;
        if (uriPath.empty() || uriPath == "/") {
            uriPath = "/index.html";
        }
        if (!uriPath.empty() && uriPath.front() == '/') {
            uriPath.erase(0, 1);
        }

        fs::path rel = fs::path(uriPath).lexically_normal();
        const auto relStr = rel.generic_string();
        if (relStr.find("..") != std::string::npos) {
            response.SetStatus(404);
            co_await SendText(response, "Not Found", "text/plain; charset=utf-8");
            co_return;
        }

        fs::path target = StaticBase_ / rel;
        std::error_code ec;
        target = fs::canonical(target, ec);
        if (ec || !fs::exists(target) || !fs::is_regular_file(target)) {
            response.SetStatus(404);
            co_await SendText(response, "Not Found", "text/plain; charset=utf-8");
            co_return;
        }

        std::ifstream in(target, std::ios::binary);
        std::string data(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());

        auto contentType = ContentType(target.extension().string());
        co_await SendText(response, data, contentType);
    }

    std::string ContentType(const std::string& ext) const {
        static const std::unordered_map<std::string, std::string> types = {
            {".html", "text/html; charset=utf-8"},
            {".css", "text/css; charset=utf-8"},
            {".js", "application/javascript; charset=utf-8"},
            {".json", "application/json; charset=utf-8"},
            {".svg", "image/svg+xml"},
            {".txt", "text/plain; charset=utf-8"},
        };
        auto it = types.find(ext);
        return it == types.end() ? "application/octet-stream" : it->second;
    }

    TFuture<void> SendJson(
        TResponse& response,
        const std::string& json,
        int status = 200)
    {
        response.SetStatus(status);
        co_await SendText(response, json, "application/json; charset=utf-8");
    }

    TFuture<void> SendText(
        TResponse& response,
        const std::string& body,
        const std::string& contentType)
    {
        response.SetHeader("Content-Type", contentType);
        response.SetHeader("Content-Length", std::to_string(body.size()));
        co_await response.SendHeaders();
        co_await response.WriteBodyFull(body);
    }

private:
    TOptions Options_;
    std::filesystem::path StaticBase_;
    std::filesystem::path BinaryBase_;
    std::vector<TServerDataset> ServerDatasets_;
};

int main(int argc, char** argv) {
    NNet::TInitializer init;

    int port = 8080;
    TOptions options;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--static-dir") && i + 1 < argc) {
            options.StaticDir = argv[++i];
        } else if (!std::strcmp(argv[i], "--binary-dir") && i + 1 < argc) {
            options.BinaryDir = argv[++i];
        } else if (!std::strcmp(argv[i], "--data") && i + 1 < argc) {
            options.DataDirs.push_back(argv[++i]);
        } else if (!std::strcmp(argv[i], "--help")) {
            std::cout << "Usage: " << argv[0]
                      << " [--port n] [--static-dir dir] [--binary-dir dir]"
                      << " [--data dir ...]\n";
            return 0;
        }
    }

    TAddress address{"::", port};
    using TPoller = TDefaultPoller;
    using TSocket = typename TPoller::TSocket;

    TLoop<TPoller> loop;
    TSocket listenSocket(loop.Poller(), address.Domain());
    listenSocket.Bind(address);
    listenSocket.Listen();
    const auto listenAddress = listenSocket.LocalAddr()->ToString();

    options.PipeFactory = [&](const std::string& cmd,
                              const std::vector<std::string>& args,
                              bool stderrToStdout)
    {
        return TPipe(loop.Poller(), cmd, args, stderrToStdout);
    };

    TRouter router(std::move(options));
    auto logger = [](const std::string& msg) {
        std::cerr << "[QumirDB Web] " << msg << "\n";
    };
    TWebServer<TSocket> server(std::move(listenSocket), router, logger);
    server.Start();

    std::cerr << "QumirDB web listening on " << listenAddress << "\n";
    loop.Loop();
    return 0;
}
