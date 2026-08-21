// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/Config.h"
#include "Luau/ConfigResolver.h"
#include "Luau/FileResolver.h"
#include "Luau/Frontend.h"
#include "Luau/JsonRpc.h"
#include "Luau/LspProtocol.h"
#include "Luau/VfsCompress.h"

#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Luau
{

struct DocumentState
{
    std::string uri;
    std::string path;
    int version = 0;
    std::string text;
    Vfs::CompactLineOffsets lineOffsets;

    void updateText(std::string newText);
    void applyIncrementalChange(const Lsp::Range& range, const std::string& newText);
    size_t getOffset(const Lsp::Position& pos) const;
    Lsp::Position getPosition(size_t offset) const;
    size_t getMemoryUsage() const;
};

class LspFileResolver : public FileResolver
{
public:
    std::unordered_map<std::string, DocumentState>* documents = nullptr;
    mutable std::unordered_map<std::string, Vfs::CompressedFileBuffer> cachedFiles;

    explicit LspFileResolver(std::unordered_map<std::string, DocumentState>* docs)
        : documents(docs)
    {
    }

    std::optional<SourceCode> readSource(const ModuleName& name) override;
    std::optional<ModuleInfo> resolveModule(const ModuleInfo* context, AstExpr* node, const TypeCheckLimits& limits) override;
    std::string getHumanReadableModuleName(const ModuleName& name) const override;

    void clearCache() const { cachedFiles.clear(); }
    size_t getCacheMemoryUsage() const;
};

class LspConfigResolver : public ConfigResolver
{
public:
    Config defaultConfig;
    mutable std::unordered_map<std::string, Config> configCache;

    explicit LspConfigResolver(Mode mode = Mode::Nonstrict)
    {
        defaultConfig.mode = mode;
    }

    const Config& getConfig(const ModuleName& name, const TypeCheckLimits& limits) const override;
};

class LspServer
{
public:
    LspServer();
    ~LspServer();

    void run(std::istream& in, std::ostream& out);
    static int runStdio();

    // Internal request handling (public for unit tests)
    JsonRpc::Response handleRequest(const JsonRpc::Request& req, std::ostream* outNotification = nullptr);
    void handleNotification(const JsonRpc::Request& notif, std::ostream* outNotification = nullptr);

    // Diagnostics
    void publishDiagnostics(const std::string& uri, std::ostream& out);

    // Protocol handlers
    Json::Value handleInitialize(const Json::Value& params);
    Json::Value handleHover(const Json::Value& params);
    Json::Value handleCompletion(const Json::Value& params);
    Json::Value handleCompletionResolve(const Json::Value& params);
    Json::Value handleDefinition(const Json::Value& params);
    Json::Value handleTypeDefinition(const Json::Value& params);
    Json::Value handleDocumentSymbol(const Json::Value& params);
    Json::Value handleDocumentHighlight(const Json::Value& params);
    Json::Value handleReferences(const Json::Value& params);
    Json::Value handlePrepareRename(const Json::Value& params);
    Json::Value handleRename(const Json::Value& params);
    Json::Value handleSemanticTokensFull(const Json::Value& params);
    Json::Value handleSignatureHelp(const Json::Value& params);
    Json::Value handleInlayHint(const Json::Value& params);
    Json::Value handleCodeAction(const Json::Value& params);

    // Custom Luau extensions
    Json::Value handleLuauAst(const Json::Value& params);
    Json::Value handleLuauTypes(const Json::Value& params);
    Json::Value handleLuauBytecode(const Json::Value& params);
    Json::Value handleLuauEval(const Json::Value& params);
    Json::Value handleLuauRequireGraph(const Json::Value& params);
    Json::Value handleLuauVfsStats(const Json::Value& params);
    Json::Value handleLuauVfsSnapshot(const Json::Value& params);

    // Direct document management for testing
    void openDocument(const std::string& uri, const std::string& text, int version = 1);
    void changeDocument(const std::string& uri, const std::string& text, int version = 2);
    void closeDocument(const std::string& uri);
    DocumentState* getDocument(const std::string& uri);

    void loadDefinitionFile(const std::string& path);
    void loadWorkspaceDefinitions(const std::string& root);

    Frontend& getFrontend() { return frontend; }

private:
    std::unordered_map<std::string, DocumentState> documents;
    LspFileResolver fileResolver;
    LspConfigResolver configResolver;
    Frontend frontend;
    bool isInitialized = false;
    bool isShutdown = false;
    std::string rootUri;
    std::string rootPath;
};

int runLspServer();

} // namespace Luau
