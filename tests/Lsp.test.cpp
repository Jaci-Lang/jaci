// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/LspServer.h"
#include "Luau/JsonRpc.h"
#include "Luau/LspProtocol.h"

#include "doctest.h"

#include <sstream>

using namespace Luau;

TEST_SUITE_BEGIN("LspTest");

TEST_CASE("JsonParserAndSerializer")
{
    SUBCASE("Primitives")
    {
        auto val = Json::parse("true");
        CHECK(val.has_value());
        CHECK(val->isBool());
        CHECK(val->getBool() == true);
        CHECK(val->serialize() == "true");

        val = Json::parse("false");
        CHECK(val.has_value());
        CHECK(val->isBool());
        CHECK(val->getBool() == false);
        CHECK(val->serialize() == "false");

        val = Json::parse("null");
        CHECK(val.has_value());
        CHECK(val->isNull());
        CHECK(val->serialize() == "null");

        val = Json::parse("12345");
        CHECK(val.has_value());
        CHECK(val->isNumber());
        CHECK(val->getInt() == 12345);
        CHECK(val->serialize() == "12345");

        val = Json::parse("\"hello\\nworld\"");
        CHECK(val.has_value());
        CHECK(val->isString());
        CHECK(val->getString() == "hello\nworld");
        CHECK(val->serialize() == "\"hello\\nworld\"");
    }

    SUBCASE("ArrayAndObject")
    {
        auto val = Json::parse("{\"key\": [1, \"test\", false], \"nested\": {\"a\": 42}}");
        CHECK(val.has_value());
        CHECK(val->isObject());

        const auto* key = val->get("key");
        CHECK(key != nullptr);
        CHECK(key->isArray());
        CHECK(key->getArray().size() == 3);
        CHECK(key->get(0)->getInt() == 1);
        CHECK(key->get(1)->getString() == "test");
        CHECK(key->get(2)->getBool() == false);

        const auto* nested = val->get("nested");
        CHECK(nested != nullptr);
        CHECK(nested->isObject());
        CHECK(nested->get("a")->getInt() == 42);
    }

    SUBCASE("InvalidJson")
    {
        std::string err;
        auto val = Json::parse("{\"unclosed\": 123", &err);
        CHECK(!val.has_value());
        CHECK(!err.empty());
    }
}

TEST_CASE("JsonRpcFraming")
{
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"test\",\"params\":{}}";
    std::string wire = "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + payload;

    std::istringstream in(wire);
    auto msg = JsonRpc::MessageReader::readMessage(in);
    CHECK(msg.has_value());
    CHECK(*msg == payload);

    std::ostringstream out;
    JsonRpc::Response resp = JsonRpc::Response::ok(JsonRpc::Id(1), Json::Value("success"));
    JsonRpc::MessageWriter::writeResponse(out, resp);

    std::string written = out.str();
    CHECK(written.find("Content-Length:") != std::string::npos);
    CHECK(written.find("\"result\":\"success\"") != std::string::npos);
}

TEST_CASE("DocumentStateOffsetAndIncrementalEdit")
{
    DocumentState doc;
    doc.uri = "file:///test.luau";
    doc.path = "/test.luau";
    doc.updateText("local a = 1\nlocal b = 2\nlocal c = 3");

    CHECK(doc.lineOffsets.lineCount() == 3);
    CHECK(doc.getOffset(Lsp::Position{0, 0}) == 0);
    CHECK(doc.getOffset(Lsp::Position{1, 0}) == 12);
    CHECK(doc.getOffset(Lsp::Position{2, 0}) == 24);

    Lsp::Position p = doc.getPosition(12);
    CHECK(p.line == 1);
    CHECK(p.character == 0);

    // Incremental change: replace '2' with '200' on line 1
    Lsp::Range r;
    r.start = Lsp::Position{1, 10};
    r.end = Lsp::Position{1, 11};
    doc.applyIncrementalChange(r, "200");

    CHECK(doc.text == "local a = 1\nlocal b = 200\nlocal c = 3");
}

TEST_CASE("LspInitializeAndCapabilities")
{
    LspServer server;
    Json::Object initParams;
    initParams.emplace_back("rootUri", Json::Value("file:///workspace"));

    JsonRpc::Request req;
    req.id = JsonRpc::Id(1);
    req.method = "initialize";
    req.params = Json::Value(std::move(initParams));

    JsonRpc::Response resp = server.handleRequest(req);
    CHECK(!resp.error.has_value());
    CHECK(resp.result.has_value());

    const auto* caps = resp.result->get("capabilities");
    CHECK(caps != nullptr);
    CHECK(caps->get("hoverProvider")->getBool() == true);
    CHECK(caps->get("definitionProvider")->getBool() == true);
    CHECK(caps->get("documentSymbolProvider")->getBool() == true);
    CHECK(caps->get("semanticTokensProvider") != nullptr);
}

TEST_CASE("LspDiagnostics")
{
    LspServer server;
    std::string uri = "file:///diagnostic_test.luau";

    // Valid code (using _x to avoid LocalUnused lint)
    server.openDocument(uri, "--!strict\nlocal _x: number = 42\n");
    std::ostringstream out;
    server.publishDiagnostics(uri, out);
    std::string outStr = out.str();
    CHECK(outStr.find("\"diagnostics\":[]") != std::string::npos);

    // Type error: assigning string to number
    server.changeDocument(uri, "--!strict\nlocal _x: number = \"hello\"\n", 2);
    std::ostringstream out2;
    server.publishDiagnostics(uri, out2);
    std::string outStr2 = out2.str();
    CHECK(outStr2.find("TypeError") != std::string::npos);
    CHECK(outStr2.find("number") != std::string::npos);
    CHECK(outStr2.find("string") != std::string::npos);
}

TEST_CASE("LspHover")
{
    LspServer server;
    std::string uri = "file:///hover_test.luau";
    server.openDocument(uri, "local myNumber = 12345\nlocal myString = tostring(myNumber)\n");

    Json::Object params;
    Json::Object textDoc;
    textDoc.emplace_back("uri", Json::Value(uri));
    params.emplace_back("textDocument", Json::Value(std::move(textDoc)));
    params.emplace_back("position", Lsp::Position{0, 8}.toJson());

    JsonRpc::Request req;
    req.id = JsonRpc::Id(10);
    req.method = "textDocument/hover";
    req.params = Json::Value(std::move(params));

    JsonRpc::Response resp = server.handleRequest(req);
    CHECK(!resp.error.has_value());
    CHECK(resp.result.has_value());

    const auto* contents = resp.result->get("contents");
    CHECK(contents != nullptr);
    const auto* val = contents->get("value");
    CHECK(val != nullptr);
    CHECK(val->getString().find("myNumber") != std::string::npos);
    CHECK(val->getString().find("number") != std::string::npos);
}

TEST_CASE("LspAutocomplete")
{
    LspServer server;
    std::string uri = "file:///ac_test.luau";
    server.openDocument(uri, "local mathVal = math.\n");

    Json::Object params;
    Json::Object textDoc;
    textDoc.emplace_back("uri", Json::Value(uri));
    params.emplace_back("textDocument", Json::Value(std::move(textDoc)));
    params.emplace_back("position", Lsp::Position{0, 21}.toJson());

    JsonRpc::Request req;
    req.id = JsonRpc::Id(20);
    req.method = "textDocument/completion";
    req.params = Json::Value(std::move(params));

    JsonRpc::Response resp = server.handleRequest(req);
    CHECK(!resp.error.has_value());
    CHECK(resp.result.has_value());

    const auto* items = resp.result->get("items");
    CHECK(items != nullptr);
    CHECK(items->isArray());

    bool foundSqrt = false;
    for (const auto& item : items->getArray())
    {
        if (const auto* label = item.get("label"))
        {
            if (label->getString() == "sqrt")
                foundSqrt = true;
        }
    }
    CHECK(foundSqrt);
}

TEST_CASE("LspDefinitionAndReferences")
{
    LspServer server;
    std::string uri = "file:///def_test.luau";
    server.openDocument(uri, "local myVar = 100\nlocal res = myVar + 200\n");

    SUBCASE("Definition")
    {
        Json::Object params;
        Json::Object textDoc;
        textDoc.emplace_back("uri", Json::Value(uri));
        params.emplace_back("textDocument", Json::Value(std::move(textDoc)));
        params.emplace_back("position", Lsp::Position{1, 14}.toJson()); // on 'myVar' on line 1

        JsonRpc::Request req;
        req.id = JsonRpc::Id(30);
        req.method = "textDocument/definition";
        req.params = Json::Value(std::move(params));

        JsonRpc::Response resp = server.handleRequest(req);
        CHECK(!resp.error.has_value());
        CHECK(resp.result.has_value());
        CHECK(!resp.result->isNull());

        const auto* range = resp.result->get("range");
        CHECK(range != nullptr);
        const auto* start = range->get("start");
        CHECK(start != nullptr);
        CHECK(start->get("line")->getInt() == 0);
    }

    SUBCASE("References")
    {
        Json::Object params;
        Json::Object textDoc;
        textDoc.emplace_back("uri", Json::Value(uri));
        params.emplace_back("textDocument", Json::Value(std::move(textDoc)));
        params.emplace_back("position", Lsp::Position{0, 8}.toJson()); // on definition

        JsonRpc::Request req;
        req.id = JsonRpc::Id(31);
        req.method = "textDocument/references";
        req.params = Json::Value(std::move(params));

        JsonRpc::Response resp = server.handleRequest(req);
        CHECK(!resp.error.has_value());
        CHECK(resp.result.has_value());
        CHECK(resp.result->isArray());
        CHECK(resp.result->getArray().size() == 2);
    }
}

TEST_CASE("LspDocumentSymbols")
{
    LspServer server;
    std::string uri = "file:///symbols_test.luau";
    server.openDocument(uri, "local function add(a: number, b: number): number\n    return a + b\nend\nlocal total = add(1, 2)\n");

    Json::Object params;
    Json::Object textDoc;
    textDoc.emplace_back("uri", Json::Value(uri));
    params.emplace_back("textDocument", Json::Value(std::move(textDoc)));

    JsonRpc::Request req;
    req.id = JsonRpc::Id(40);
    req.method = "textDocument/documentSymbol";
    req.params = Json::Value(std::move(params));

    JsonRpc::Response resp = server.handleRequest(req);
    CHECK(!resp.error.has_value());
    CHECK(resp.result.has_value());
    CHECK(resp.result->isArray());

    bool foundAdd = false;
    bool foundTotal = false;
    for (const auto& sym : resp.result->getArray())
    {
        if (sym.get("name")->getString() == "add")
            foundAdd = true;
        if (sym.get("name")->getString() == "total")
            foundTotal = true;
    }
    CHECK(foundAdd);
    CHECK(foundTotal);
}

TEST_CASE("LspRename")
{
    LspServer server;
    std::string uri = "file:///rename_test.luau";
    server.openDocument(uri, "local foo = 1\nlocal bar = foo + foo\n");

    Json::Object params;
    Json::Object textDoc;
    textDoc.emplace_back("uri", Json::Value(uri));
    params.emplace_back("textDocument", Json::Value(std::move(textDoc)));
    params.emplace_back("position", Lsp::Position{0, 7}.toJson());
    params.emplace_back("newName", Json::Value("baz"));

    JsonRpc::Request req;
    req.id = JsonRpc::Id(50);
    req.method = "textDocument/rename";
    req.params = Json::Value(std::move(params));

    JsonRpc::Response resp = server.handleRequest(req);
    CHECK(!resp.error.has_value());
    CHECK(resp.result.has_value());

    const auto* changes = resp.result->get("changes");
    CHECK(changes != nullptr);
    const auto* edits = changes->get(uri);
    CHECK(edits != nullptr);
    CHECK(edits->isArray());
    CHECK(edits->getArray().size() == 3); // 1 definition + 2 uses
}

TEST_CASE("LspSemanticTokens")
{
    LspServer server;
    std::string uri = "file:///sem_test.luau";
    server.openDocument(uri, "local x = 123\n");

    Json::Object params;
    Json::Object textDoc;
    textDoc.emplace_back("uri", Json::Value(uri));
    params.emplace_back("textDocument", Json::Value(std::move(textDoc)));

    JsonRpc::Request req;
    req.id = JsonRpc::Id(60);
    req.method = "textDocument/semanticTokens/full";
    req.params = Json::Value(std::move(params));

    JsonRpc::Response resp = server.handleRequest(req);
    CHECK(!resp.error.has_value());
    CHECK(resp.result.has_value());

    const auto* data = resp.result->get("data");
    CHECK(data != nullptr);
    CHECK(data->isArray());
    CHECK(!data->getArray().empty());
}

TEST_CASE("LspInlayHints")
{
    LspServer server;
    std::string uri = "file:///inlay_test.luau";
    server.openDocument(uri, "local count = 42\n");

    Json::Object params;
    Json::Object textDoc;
    textDoc.emplace_back("uri", Json::Value(uri));
    params.emplace_back("textDocument", Json::Value(std::move(textDoc)));

    JsonRpc::Request req;
    req.id = JsonRpc::Id(70);
    req.method = "textDocument/inlayHint";
    req.params = Json::Value(std::move(params));

    JsonRpc::Response resp = server.handleRequest(req);
    CHECK(!resp.error.has_value());
    CHECK(resp.result.has_value());
    CHECK(resp.result->isArray());
    CHECK(resp.result->getArray().size() >= 1);
    CHECK(resp.result->get(0)->get("label")->getString().find("number") != std::string::npos);
}

TEST_CASE("LspCustomEndpoints")
{
    LspServer server;
    std::string uri = "file:///custom_test.luau";
    server.openDocument(uri, "local a = 10 + 20\n");

    SUBCASE("LuauAst")
    {
        Json::Object params;
        Json::Object textDoc;
        textDoc.emplace_back("uri", Json::Value(uri));
        params.emplace_back("textDocument", Json::Value(std::move(textDoc)));

        JsonRpc::Request req;
        req.id = JsonRpc::Id(80);
        req.method = "luau/ast";
        req.params = Json::Value(std::move(params));

        JsonRpc::Response resp = server.handleRequest(req);
        CHECK(!resp.error.has_value());
        CHECK(resp.result.has_value());
    }

    SUBCASE("LuauBytecode")
    {
        Json::Object params;
        Json::Object textDoc;
        textDoc.emplace_back("uri", Json::Value(uri));
        params.emplace_back("textDocument", Json::Value(std::move(textDoc)));

        JsonRpc::Request req;
        req.id = JsonRpc::Id(81);
        req.method = "luau/bytecode";
        req.params = Json::Value(std::move(params));

        JsonRpc::Response resp = server.handleRequest(req);
        CHECK(!resp.error.has_value());
        CHECK(resp.result.has_value());
        CHECK(resp.result->get("bytecodeLength")->getInt() > 0);
    }

    SUBCASE("LuauEval")
    {
        Json::Object params;
        params.emplace_back("expression", Json::Value("100 * 2 + 5"));

        JsonRpc::Request req;
        req.id = JsonRpc::Id(82);
        req.method = "luau/eval";
        req.params = Json::Value(std::move(params));

        JsonRpc::Response resp = server.handleRequest(req);
        CHECK(!resp.error.has_value());
        CHECK(resp.result.has_value());
        CHECK(resp.result->get("result")->getString() == "205");
    }

    SUBCASE("LuauTypes")
    {
        Json::Object params;
        Json::Object textDoc;
        textDoc.emplace_back("uri", Json::Value(uri));
        params.emplace_back("textDocument", Json::Value(std::move(textDoc)));

        JsonRpc::Request req;
        req.id = JsonRpc::Id(83);
        req.method = "luau/types";
        req.params = Json::Value(std::move(params));

        JsonRpc::Response resp = server.handleRequest(req);
        CHECK(!resp.error.has_value());
        CHECK(resp.result.has_value());
        CHECK(resp.result->isObject());
    }

    SUBCASE("LuauRequireGraph")
    {
        server.openDocument(uri, "local m = require('./sibling')\n");
        Json::Object params;
        Json::Object textDoc;
        textDoc.emplace_back("uri", Json::Value(uri));
        params.emplace_back("textDocument", Json::Value(std::move(textDoc)));

        JsonRpc::Request req;
        req.id = JsonRpc::Id(84);
        req.method = "luau/requireGraph";
        req.params = Json::Value(std::move(params));

        JsonRpc::Response resp = server.handleRequest(req);
        CHECK(!resp.error.has_value());
        CHECK(resp.result.has_value());
        CHECK(resp.result->get("dependencies") != nullptr);
    }
}

TEST_CASE("MsgPackBinaryJsonRoundtrip")
{
    Json::Object original;
    original.emplace_back("str", Json::Value("hello binary json"));
    original.emplace_back("num", Json::Value(42));
    original.emplace_back("neg", Json::Value(-15));
    original.emplace_back("double", Json::Value(3.14159));
    original.emplace_back("b_true", Json::Value(true));
    original.emplace_back("b_false", Json::Value(false));
    original.emplace_back("null_val", Json::Value(nullptr));

    Json::Array arr;
    arr.push_back(Json::Value(1));
    arr.push_back(Json::Value("two"));
    arr.push_back(Json::Value(3.0));
    original.emplace_back("arr", Json::Value(std::move(arr)));

    Json::Value origVal(std::move(original));
    std::string binary = origVal.toBinaryMsgPack();
    CHECK(!binary.empty());

    std::string err;
    auto decoded = Json::parseBinaryMsgPack(binary, &err);
    CHECK(decoded.has_value());
    CHECK(err.empty());
    CHECK(decoded->isObject());
    CHECK(decoded->get("str")->getString() == "hello binary json");
    CHECK(decoded->get("num")->getInt() == 42);
    CHECK(decoded->get("neg")->getInt() == -15);
    CHECK(decoded->get("b_true")->getBool() == true);
    CHECK(decoded->get("b_false")->getBool() == false);
    CHECK(decoded->get("null_val")->isNull());
    CHECK(decoded->get("arr")->isArray());
    CHECK(decoded->get("arr")->get(1)->getString() == "two");
}

TEST_CASE("LspBinaryJsonTransport")
{
    LspServer server;
    std::string uri = "file:///binary_test.luau";
    server.openDocument(uri, "function add(a: number, b: number): number\n    return a + b\nend\n");

    Json::Object reqObj;
    reqObj.emplace_back("jsonrpc", Json::Value("2.0"));
    reqObj.emplace_back("id", Json::Value(99));
    reqObj.emplace_back("method", Json::Value("textDocument/completionItem/resolve"));

    Json::Object item;
    item.emplace_back("label", Json::Value("add"));
    item.emplace_back("detail", Json::Value("(a: number, b: number) -> number"));
    reqObj.emplace_back("params", Json::Value(std::move(item)));

    std::string binaryPayload = Json::Value(std::move(reqObj)).toBinaryMsgPack();
    std::string framed;
    framed += "Content-Length: " + std::to_string(binaryPayload.size()) + "\r\n";
    framed += "Content-Type: application/msgpack\r\n\r\n";
    framed += binaryPayload;

    std::istringstream in(framed);
    std::ostringstream out;
    server.run(in, out);

    std::string outStr = out.str();
    CHECK(outStr.find("application/msgpack") != std::string::npos);

    // Verify response can be decoded
    std::istringstream respIn(outStr);
    auto framedResp = JsonRpc::MessageReader::readFramedMessage(respIn);
    CHECK(framedResp.has_value());
    CHECK(framedResp->format == JsonRpc::TransportFormat::BinaryMsgPack);

    std::string decErr;
    auto respVal = Json::parseBinaryMsgPack(framedResp->payload, &decErr);
    CHECK(respVal.has_value());
    CHECK(respVal->get("id")->getInt() == 99);
    CHECK(respVal->get("result") != nullptr);
    CHECK(respVal->get("result")->get("documentation") != nullptr);
}

TEST_CASE("VfsFastBinaryCompression")
{
    std::string code = R"(
        local function fibonacci(n: number): number
            if n <= 1 then
                return n
            else
                return fibonacci(n - 1) + fibonacci(n - 2)
            end
        end

        local function computeStats(items: {number})
            local sum = 0
            for _, v in ipairs(items) do
                sum += v
            end
            return sum / #items
        end

        return {
            fibonacci = fibonacci,
            computeStats = computeStats,
        }
    )";

    std::string compressed = Vfs::compress(code);
    CHECK(!compressed.empty());
    CHECK(compressed.size() < code.size()); // Achieves compression

    std::string decompressed = Vfs::decompress(compressed, code.size());
    CHECK(decompressed == code); // Perfect roundtrip lossless recovery
}

TEST_CASE("VfsCompactLineOffsets")
{
    std::string text = "line 0\nline 1 is longer\n\nline 3\n";
    Vfs::CompactLineOffsets offsets(text);
    CHECK(offsets.lineCount() == 5);

    // Line 1 char 0
    size_t off = offsets.getOffset(1, 0, text.size());
    CHECK(off == 7);

    int line = -1, character = -1;
    offsets.getPosition(7, line, character, text.size());
    CHECK(line == 1);
    CHECK(character == 0);
}

TEST_CASE("LspVfsStatsAndSnapshot")
{
    LspServer server;
    server.openDocument("file:///main.luau", "local x = 1\nlocal y = 2\nlocal z = x + y\n");

    SUBCASE("VfsStats")
    {
        JsonRpc::Request req;
        req.id = JsonRpc::Id(101);
        req.method = "luau/vfsStats";
        req.params = Json::Value(Json::Object{});

        JsonRpc::Response resp = server.handleRequest(req);
        CHECK(!resp.error.has_value());
        CHECK(resp.result.has_value());
        CHECK(resp.result->get("openDocumentsCount")->getInt() == 1);
        CHECK(resp.result->get("openDocumentsMemoryBytes")->getInt() > 0);
    }

    SUBCASE("VfsSnapshot")
    {
        JsonRpc::Request req;
        req.id = JsonRpc::Id(102);
        req.method = "luau/vfsSnapshot";
        req.params = Json::Value(Json::Object{});

        JsonRpc::Response resp = server.handleRequest(req);
        CHECK(!resp.error.has_value());
        CHECK(resp.result.has_value());
        CHECK(resp.result->get("documentCount")->getInt() == 1);
        CHECK(resp.result->get("documents")->isArray());
    }
}

TEST_CASE("LspAutocompleteFragmentAndRanking")
{
    LspServer server;
    std::string uri = "file:///autocomplete_rank.luau";
    server.openDocument(uri, "local tbl = { alpha = 10, beta = 20 }\nlocal x = tbl.");

    Json::Object params;
    Json::Object textDoc;
    textDoc.emplace_back("uri", Json::Value(uri));
    params.emplace_back("textDocument", Json::Value(std::move(textDoc)));
    params.emplace_back("position", Lsp::Position{1, 14}.toJson());

    JsonRpc::Request req;
    req.id = JsonRpc::Id(200);
    req.method = "textDocument/completion";
    req.params = Json::Value(std::move(params));

    JsonRpc::Response resp = server.handleRequest(req);
    CHECK(!resp.error.has_value());
    CHECK(resp.result.has_value());

    const auto* items = resp.result->get("items");
    REQUIRE(items != nullptr);
    REQUIRE(items->isArray());

    bool foundAlpha = false;
    bool foundBeta = false;
    for (const auto& item : items->getArray())
    {
        if (const auto* label = item.get("label"))
        {
            if (label->getString() == "alpha")
            {
                foundAlpha = true;
                const auto* sortText = item.get("sortText");
                CHECK(sortText != nullptr);
                CHECK(sortText->getString().rfind("0003_alpha", 0) == 0);
            }
            if (label->getString() == "beta")
            {
                foundBeta = true;
                const auto* sortText = item.get("sortText");
                CHECK(sortText != nullptr);
                CHECK(sortText->getString().rfind("0003_beta", 0) == 0);
            }
        }
    }
    CHECK(foundAlpha);
    CHECK(foundBeta);

    // Incremental edit testing fragment autocomplete fast path
    server.changeDocument(uri, "local tbl = { alpha = 10, beta = 20 }\nlocal x = tbl.al", 2);

    Json::Object params2;
    Json::Object textDoc2;
    textDoc2.emplace_back("uri", Json::Value(uri));
    params2.emplace_back("textDocument", Json::Value(std::move(textDoc2)));
    params2.emplace_back("position", Lsp::Position{1, 16}.toJson());

    req.id = JsonRpc::Id(201);
    req.params = Json::Value(std::move(params2));

    JsonRpc::Response resp2 = server.handleRequest(req);
    CHECK(!resp2.error.has_value());
    CHECK(resp2.result.has_value());
    const auto* items2 = resp2.result->get("items");
    REQUIRE(items2 != nullptr);
    REQUIRE(items2->isArray());
    bool foundAlpha2 = false;
    for (const auto& item : items2->getArray())
    {
        if (const auto* label = item.get("label"))
        {
            if (label->getString() == "alpha")
                foundAlpha2 = true;
        }
    }
    CHECK(foundAlpha2);
}

TEST_CASE("LspAutocompleteFunctionSnippetsAndResolve")
{
    LspServer server;
    std::string uri = "file:///snippet_test.luau";
    server.openDocument(uri, "local function compute(val: number, multiplier: number): number\n    return val * multiplier\nend\nlocal r = com");

    Json::Object params;
    Json::Object textDoc;
    textDoc.emplace_back("uri", Json::Value(uri));
    params.emplace_back("textDocument", Json::Value(std::move(textDoc)));
    params.emplace_back("position", Lsp::Position{3, 13}.toJson());

    JsonRpc::Request req;
    req.id = JsonRpc::Id(210);
    req.method = "textDocument/completion";
    req.params = Json::Value(std::move(params));

    JsonRpc::Response resp = server.handleRequest(req);
    CHECK(!resp.error.has_value());
    CHECK(resp.result.has_value());

    const auto* items = resp.result->get("items");
    REQUIRE(items != nullptr);
    REQUIRE(items->isArray());

    Json::Value computeItem;
    bool foundCompute = false;
    for (const auto& item : items->getArray())
    {
        if (const auto* label = item.get("label"))
        {
            if (label->getString() == "compute")
            {
                foundCompute = true;
                computeItem = item;
                const auto* insertFormat = item.get("insertTextFormat");
                CHECK(insertFormat != nullptr);
                CHECK(insertFormat->getInt() == 2); // Snippet

                const auto* insertText = item.get("insertText");
                CHECK(insertText != nullptr);
                CHECK(insertText->getString().find("compute(${1:val}, ${2:multiplier})") != std::string::npos);
            }
        }
    }
    CHECK(foundCompute);

    // Test completionItem/resolve
    JsonRpc::Request resolveReq;
    resolveReq.id = JsonRpc::Id(211);
    resolveReq.method = "completionItem/resolve";
    resolveReq.params = computeItem;

    JsonRpc::Response resolveResp = server.handleRequest(resolveReq);
    CHECK(!resolveResp.error.has_value());
    CHECK(resolveResp.result.has_value());
    const auto* docObj = resolveResp.result->get("documentation");
    CHECK(docObj != nullptr);
    CHECK(docObj->get("kind")->getString() == "markdown");
    CHECK(docObj->get("value")->getString().find("```luau\n(val: number, multiplier: number) -> number\n```") != std::string::npos);
}

TEST_CASE("LspAutocompleteRequirePathsAndAliases")
{
    LspServer server;
    std::string uri = "file:///workspace/main.luau";
    server.openDocument(uri, "local m = require(\"@\")\n");
    server.openDocument("file:///workspace/submodule.luau", "return { name = 'sub' }\n");

    server.getConfigResolver().defaultConfig.aliases["pkg"] = { "/workspace/pkg", "cfg", "pkg" };

    Json::Object params;
    Json::Object textDoc;
    textDoc.emplace_back("uri", Json::Value(uri));
    params.emplace_back("textDocument", Json::Value(std::move(textDoc)));
    params.emplace_back("position", Lsp::Position{0, 20}.toJson());

    JsonRpc::Request req;
    req.id = JsonRpc::Id(220);
    req.method = "textDocument/completion";
    req.params = Json::Value(std::move(params));

    JsonRpc::Response resp = server.handleRequest(req);
    CHECK(!resp.error.has_value());
    CHECK(resp.result.has_value());

    const auto* items = resp.result->get("items");
    REQUIRE(items != nullptr);
    REQUIRE(items->isArray());

    bool foundPkg = false;
    for (const auto& item : items->getArray())
    {
        if (const auto* label = item.get("label"))
        {
            if (label->getString() == "@pkg")
            {
                foundPkg = true;
                CHECK(item.get("kind")->getInt() == 17); // File / RequirePath
            }
        }
    }
    CHECK(foundPkg);
}

TEST_SUITE_END();

