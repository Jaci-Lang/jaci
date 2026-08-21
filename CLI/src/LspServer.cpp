// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/LspServer.h"

#include "Luau/AnalyzeRequirer.h"
#include "Luau/AstJsonEncoder.h"
#include "Luau/AstQuery.h"
#include "Luau/Autocomplete.h"
#include "Luau/BuiltinDefinitions.h"
#include "Luau/Compiler.h"
#include "Luau/FileUtils.h"
#include "Luau/Frontend.h"
#include "Luau/Lexer.h"
#include "Luau/LuauConfig.h"
#include "Luau/Parser.h"
#include "Luau/RequireNavigator.h"
#include "Luau/ToString.h"
#include "Luau/TypeAttach.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>

namespace Luau
{

void DocumentState::updateText(std::string newText)
{
    text = std::move(newText);
    text.shrink_to_fit();
    lineOffsets.build(text);
}

size_t DocumentState::getOffset(const Lsp::Position& pos) const
{
    return lineOffsets.getOffset(pos.line, pos.character, text.size());
}

Lsp::Position DocumentState::getPosition(size_t offset) const
{
    int line = 0;
    int character = 0;
    lineOffsets.getPosition(offset, line, character, text.size());
    return Lsp::Position{line, character};
}

size_t DocumentState::getMemoryUsage() const
{
    return text.capacity() + lineOffsets.memoryUsage() + uri.capacity() + path.capacity();
}

void DocumentState::applyIncrementalChange(const Lsp::Range& range, const std::string& newText)
{
    size_t startOffset = getOffset(range.start);
    size_t endOffset = getOffset(range.end);

    if (startOffset > endOffset)
        std::swap(startOffset, endOffset);

    if (startOffset > text.size())
        startOffset = text.size();
    if (endOffset > text.size())
        endOffset = text.size();

    std::string updated = text.substr(0, startOffset) + newText + text.substr(endOffset);
    updateText(std::move(updated));
}

std::optional<SourceCode> LspFileResolver::readSource(const ModuleName& name)
{
    // Try to find open document first
    if (documents)
    {
        for (const auto& pair : *documents)
        {
            if (pair.first == name || pair.second.path == name)
            {
                return SourceCode{pair.second.text, SourceCode::Module};
            }
        }
    }

    auto it = cachedFiles.find(name);
    if (it != cachedFiles.end())
    {
        return SourceCode{it->second.getText(), SourceCode::Module};
    }

    std::optional<std::string> diskContent = readFile(name);
    if (diskContent)
    {
        cachedFiles.emplace(name, Vfs::CompressedFileBuffer(*diskContent, /* autoCompress = */ true));
        return SourceCode{std::move(*diskContent), SourceCode::Module};
    }

    return std::nullopt;
}

size_t LspFileResolver::getCacheMemoryUsage() const
{
    size_t total = 0;
    for (const auto& pair : cachedFiles)
        total += pair.first.capacity() + pair.second.memoryUsage();
    return total;
}

std::optional<ModuleInfo> LspFileResolver::resolveModule(const ModuleInfo* context, AstExpr* node, const TypeCheckLimits& limits)
{
    if (AstExprConstantString* expr = node->as<AstExprConstantString>())
    {
        std::string path{expr->value.data, expr->value.size};

        if (context)
        {
            FileNavigationContext navigationContext{context->name};
            Luau::Require::ErrorHandler nullErrorHandler{};
            Luau::Require::Navigator navigator(navigationContext, nullErrorHandler);

            if (navigator.navigate(std::move(path)) == Luau::Require::Navigator::Status::Success)
            {
                if (navigationContext.isModulePresent())
                {
                    if (std::optional<std::string> id = navigationContext.getIdentifier())
                        return {{*id}};
                }
            }
        }
    }
    return std::nullopt;
}

std::string LspFileResolver::getHumanReadableModuleName(const ModuleName& name) const
{
    return name;
}

const Config& LspConfigResolver::getConfig(const ModuleName& name, const TypeCheckLimits& limits) const
{
    std::optional<std::string> path = getParentPath(name);
    if (!path)
        return defaultConfig;

    auto it = configCache.find(*path);
    if (it != configCache.end())
        return it->second;

    Config config = defaultConfig;
    std::optional<std::string> configPath = joinPaths(*path, kConfigName);
    if (!isFile(*configPath))
        configPath = std::nullopt;

    std::optional<std::string> luauConfigPath = joinPaths(*path, kLuauConfigName);
    if (!isFile(*luauConfigPath))
        luauConfigPath = std::nullopt;

    if (configPath)
    {
        if (std::optional<std::string> contents = readFile(*configPath))
        {
            Luau::ConfigOptions::AliasOptions aliasOpts;
            aliasOpts.configLocation = *configPath;
            aliasOpts.overwriteAliases = true;
            Luau::ConfigOptions opts;
            opts.aliasOptions = std::move(aliasOpts);
            Luau::parseConfig(*contents, config, opts);
        }
    }
    else if (luauConfigPath)
    {
        if (std::optional<std::string> contents = readFile(*luauConfigPath))
        {
            Luau::ConfigOptions::AliasOptions aliasOpts;
            aliasOpts.configLocation = *luauConfigPath;
            aliasOpts.overwriteAliases = true;
            Luau::InterruptCallbacks callbacks;
            Luau::extractLuauConfig(*contents, config, aliasOpts, std::move(callbacks));
        }
    }

    auto inserted = configCache.emplace(*path, std::move(config));
    return inserted.first->second;
}

static FrontendOptions makeFrontendOptions()
{
    FrontendOptions opts;
    opts.retainFullTypeGraphs = true;
    opts.runLintChecks = true;
    opts.forAutocomplete = false;
    return opts;
}

LspServer::LspServer()
    : fileResolver(&documents)
    , configResolver(Mode::Nonstrict)
    , frontend(SolverMode::New, &fileResolver, &configResolver, makeFrontendOptions())
{
    registerBuiltinGlobals(frontend, frontend.globals);
    freeze(frontend.globals.globalTypes);
}

LspServer::~LspServer() = default;

void LspServer::openDocument(const std::string& uri, const std::string& text, int version)
{
    std::string path = Lsp::uriToPath(uri);
    DocumentState doc;
    doc.uri = uri;
    doc.path = path;
    doc.version = version;
    doc.updateText(text);

    documents[uri] = std::move(doc);
    frontend.markDirty(path);
}

void LspServer::changeDocument(const std::string& uri, const std::string& text, int version)
{
    auto it = documents.find(uri);
    if (it != documents.end())
    {
        it->second.version = version;
        it->second.updateText(text);
        frontend.markDirty(it->second.path);
    }
}

void LspServer::closeDocument(const std::string& uri)
{
    auto it = documents.find(uri);
    if (it != documents.end())
    {
        std::string path = it->second.path;
        documents.erase(it);
        frontend.markDirty(path);
    }
}

DocumentState* LspServer::getDocument(const std::string& uri)
{
    auto it = documents.find(uri);
    if (it != documents.end())
        return &it->second;
    return nullptr;
}

void LspServer::publishDiagnostics(const std::string& uri, std::ostream& out)
{
    DocumentState* doc = getDocument(uri);
    if (!doc)
        return;

    CheckResult cr = frontend.check(doc->path);

    std::vector<Lsp::Diagnostic> lspDiagnostics;

    for (const TypeError& err : cr.errors)
    {
        Lsp::Diagnostic d;
        d.range = Lsp::Range::fromLuau(err.location);
        d.severity = Lsp::DiagnosticSeverity::Error;
        d.code = "TypeError";
        d.source = "luau";
        d.message = toString(err, TypeErrorToStringOptions{&fileResolver});
        lspDiagnostics.push_back(std::move(d));
    }

    for (const LintWarning& warn : cr.lintResult.errors)
    {
        Lsp::Diagnostic d;
        d.range = Lsp::Range::fromLuau(warn.location);
        d.severity = Lsp::DiagnosticSeverity::Error;
        d.code = LintWarning::getName(warn.code);
        d.source = "luau-lint";
        d.message = warn.text;
        lspDiagnostics.push_back(std::move(d));
    }

    for (const LintWarning& warn : cr.lintResult.warnings)
    {
        Lsp::Diagnostic d;
        d.range = Lsp::Range::fromLuau(warn.location);
        d.severity = Lsp::DiagnosticSeverity::Warning;
        d.code = LintWarning::getName(warn.code);
        d.source = "luau-lint";
        d.message = warn.text;
        lspDiagnostics.push_back(std::move(d));
    }

    Json::Object params;
    params.emplace_back("uri", Json::Value(uri));
    params.emplace_back("version", Json::Value(doc->version));

    Json::Array diagArray;
    for (const auto& d : lspDiagnostics)
        diagArray.push_back(d.toJson());

    params.emplace_back("diagnostics", Json::Value(std::move(diagArray)));

    JsonRpc::MessageWriter::writeNotification(out, "textDocument/publishDiagnostics", Json::Value(std::move(params)));
}

Json::Value LspServer::handleInitialize(const Json::Value& params)
{
    isInitialized = true;

    if (const auto* rootUriVal = params.get("rootUri"))
    {
        if (rootUriVal->isString())
        {
            rootUri = rootUriVal->getString();
            rootPath = Lsp::uriToPath(rootUri);
        }
    }

    Json::Object capabilities;

    // Text document sync: Incremental = 2
    capabilities.emplace_back("textDocumentSync", Json::Value(2));
    capabilities.emplace_back("hoverProvider", Json::Value(true));

    // Completion provider
    Json::Object completionProvider;
    completionProvider.emplace_back("resolveProvider", Json::Value(false));
    Json::Array triggerChars;
    triggerChars.push_back(Json::Value("."));
    triggerChars.push_back(Json::Value(":"));
    triggerChars.push_back(Json::Value("\""));
    triggerChars.push_back(Json::Value("'"));
    triggerChars.push_back(Json::Value("/"));
    triggerChars.push_back(Json::Value("@"));
    completionProvider.emplace_back("triggerCharacters", Json::Value(std::move(triggerChars)));
    capabilities.emplace_back("completionProvider", Json::Value(std::move(completionProvider)));

    // Definition & Type Definition
    capabilities.emplace_back("definitionProvider", Json::Value(true));
    capabilities.emplace_back("typeDefinitionProvider", Json::Value(true));

    // Document symbol & Highlight & References
    capabilities.emplace_back("documentSymbolProvider", Json::Value(true));
    capabilities.emplace_back("documentHighlightProvider", Json::Value(true));
    capabilities.emplace_back("referencesProvider", Json::Value(true));

    // Rename
    Json::Object renameProvider;
    renameProvider.emplace_back("prepareProvider", Json::Value(true));
    capabilities.emplace_back("renameProvider", Json::Value(std::move(renameProvider)));

    // Signature Help
    Json::Object sigHelpProvider;
    Json::Array sigTriggers;
    sigTriggers.push_back(Json::Value("("));
    sigTriggers.push_back(Json::Value(","));
    sigHelpProvider.emplace_back("triggerCharacters", Json::Value(std::move(sigTriggers)));
    capabilities.emplace_back("signatureHelpProvider", Json::Value(std::move(sigHelpProvider)));

    // Inlay hints & Code Action
    capabilities.emplace_back("inlayHintProvider", Json::Value(true));
    capabilities.emplace_back("codeActionProvider", Json::Value(true));

    // Semantic tokens full
    Json::Object semanticTokensProvider;
    Json::Object legend;
    Json::Array tokenTypes;
    tokenTypes.push_back(Json::Value("type"));
    tokenTypes.push_back(Json::Value("class"));
    tokenTypes.push_back(Json::Value("enum"));
    tokenTypes.push_back(Json::Value("interface"));
    tokenTypes.push_back(Json::Value("typeParameter"));
    tokenTypes.push_back(Json::Value("parameter"));
    tokenTypes.push_back(Json::Value("variable"));
    tokenTypes.push_back(Json::Value("property"));
    tokenTypes.push_back(Json::Value("function"));
    tokenTypes.push_back(Json::Value("method"));
    tokenTypes.push_back(Json::Value("keyword"));
    tokenTypes.push_back(Json::Value("comment"));
    tokenTypes.push_back(Json::Value("string"));
    tokenTypes.push_back(Json::Value("number"));
    tokenTypes.push_back(Json::Value("operator"));
    legend.emplace_back("tokenTypes", Json::Value(std::move(tokenTypes)));
    legend.emplace_back("tokenModifiers", Json::Value(Json::Array{}));
    semanticTokensProvider.emplace_back("legend", Json::Value(std::move(legend)));
    semanticTokensProvider.emplace_back("full", Json::Value(true));
    capabilities.emplace_back("semanticTokensProvider", Json::Value(std::move(semanticTokensProvider)));

    Json::Object serverInfo;
    serverInfo.emplace_back("name", Json::Value("jaci-lsp"));
    serverInfo.emplace_back("version", Json::Value("1.0.0"));

    Json::Object result;
    result.emplace_back("capabilities", Json::Value(std::move(capabilities)));
    result.emplace_back("serverInfo", Json::Value(std::move(serverInfo)));

    return Json::Value(std::move(result));
}

Json::Value LspServer::handleHover(const Json::Value& params)
{
    const auto* textDoc = params.get("textDocument");
    const auto* posVal = params.get("position");
    if (!textDoc || !posVal)
        return Json::Value(nullptr);

    const auto* uriVal = textDoc->get("uri");
    if (!uriVal || !uriVal->isString())
        return Json::Value(nullptr);

    std::string uri = uriVal->getString();
    DocumentState* doc = getDocument(uri);
    if (!doc)
        return Json::Value(nullptr);

    Lsp::Position lspPos = Lsp::Position::fromJson(*posVal);
    Position luauPos = lspPos.toLuau();

    frontend.check(doc->path);

    SourceModule* sm = frontend.getSourceModule(doc->path);
    ModulePtr module = frontend.moduleResolver.getModule(doc->path);
    if (!sm || !module)
        return Json::Value(nullptr);

    std::string hoverText;
    std::optional<Lsp::Range> hoverRange;

    ExprOrLocal exprOrLocal = findExprOrLocalAtPosition(*sm, luauPos);
    if (auto local = exprOrLocal.getLocal())
    {
        hoverRange = Lsp::Range::fromLuau(local->location);
        ScopePtr scope = findScopeAtPosition(*module, luauPos);
        std::string typeStr = "unknown";
        if (scope)
        {
            auto it = scope->bindings.find(local);
            if (it != scope->bindings.end())
                typeStr = toString(it->second.typeId);
        }
        hoverText = "```luau\n(local) " + std::string(local->name.value) + ": " + typeStr + "\n```";
    }
    else if (auto expr = exprOrLocal.getExpr())
    {
        hoverRange = Lsp::Range::fromLuau(expr->location);
        if (auto it = module->astTypes.find(expr))
        {
            TypeId ty = follow(*it);
            std::string typeStr = toString(ty);

            if (auto ident = expr->as<AstExprGlobal>())
            {
                hoverText = "```luau\n(global) " + std::string(ident->name.value) + ": " + typeStr + "\n```";
            }
            else if (auto idx = expr->as<AstExprIndexName>())
            {
                hoverText = "```luau\n(property) " + std::string(idx->index.value) + ": " + typeStr + "\n```";
            }
            else if (auto str = expr->as<AstExprConstantString>())
            {
                std::string modPath{str->value.data, str->value.size};
                FileNavigationContext navContext{doc->path};
                Require::ErrorHandler errHandler;
                Require::Navigator nav(navContext, errHandler);
                if (nav.navigate(std::move(modPath)) == Require::Navigator::Status::Success)
                {
                    if (std::optional<std::string> resolvedId = navContext.getIdentifier())
                    {
                        hoverText = "**Module**: `" + *resolvedId + "`\n";
                        if (ModulePtr reqMod = frontend.moduleResolver.getModule(*resolvedId))
                        {
                            if (reqMod->returnType)
                            {
                                hoverText += "\n```luau\n-- Module Exports:\n" + toString(reqMod->returnType) + "\n```";
                            }
                        }
                    }
                }
            }
            else
            {
                hoverText = "```luau\n" + typeStr + "\n```";
            }
        }
    }

    if (hoverText.empty())
    {
        if (std::optional<TypeId> ty = findTypeAtPosition(*module, *sm, luauPos))
        {
            hoverText = "```luau\n" + toString(*ty) + "\n```";
        }
    }

    if (hoverText.empty())
        return Json::Value(nullptr);

    Json::Object contents;
    contents.emplace_back("kind", Json::Value("markdown"));
    contents.emplace_back("value", Json::Value(std::move(hoverText)));

    Json::Object result;
    result.emplace_back("contents", Json::Value(std::move(contents)));
    if (hoverRange)
        result.emplace_back("range", hoverRange->toJson());

    return Json::Value(std::move(result));
}

static Lsp::CompletionItemKind mapCompletionKind(AutocompleteEntryKind kind, std::optional<TypeId> ty)
{
    if (ty)
    {
        TypeId followed = follow(*ty);
        if (get<FunctionType>(followed))
            return (kind == AutocompleteEntryKind::Property) ? Lsp::CompletionItemKind::Method : Lsp::CompletionItemKind::Function;
    }

    switch (kind)
    {
    case AutocompleteEntryKind::Property:
        return Lsp::CompletionItemKind::Field;
    case AutocompleteEntryKind::Binding:
        return Lsp::CompletionItemKind::Variable;
    case AutocompleteEntryKind::Keyword:
        return Lsp::CompletionItemKind::Keyword;
    case AutocompleteEntryKind::String:
        return Lsp::CompletionItemKind::Text;
    case AutocompleteEntryKind::Type:
        return Lsp::CompletionItemKind::Interface;
    case AutocompleteEntryKind::Module:
        return Lsp::CompletionItemKind::Module;
    case AutocompleteEntryKind::GeneratedFunction:
        return Lsp::CompletionItemKind::Snippet;
    case AutocompleteEntryKind::RequirePath:
        return Lsp::CompletionItemKind::File;
    case AutocompleteEntryKind::HotComment:
        return Lsp::CompletionItemKind::Snippet;
    }
    return Lsp::CompletionItemKind::Text;
}

Json::Value LspServer::handleCompletion(const Json::Value& params)
{
    const auto* textDoc = params.get("textDocument");
    const auto* posVal = params.get("position");
    if (!textDoc || !posVal)
        return Json::Value(nullptr);

    const auto* uriVal = textDoc->get("uri");
    if (!uriVal || !uriVal->isString())
        return Json::Value(nullptr);

    std::string uri = uriVal->getString();
    DocumentState* doc = getDocument(uri);
    if (!doc)
        return Json::Value(nullptr);

    Lsp::Position lspPos = Lsp::Position::fromJson(*posVal);
    Position luauPos = lspPos.toLuau();

    FrontendOptions opts;
    opts.forAutocomplete = true;
    opts.retainFullTypeGraphs = true;
    frontend.check(doc->path, opts);

    AutocompleteResult acResult = autocomplete(
        frontend,
        doc->path,
        luauPos,
        [](std::string, std::optional<const ExternType*>, std::optional<std::string>) -> std::optional<AutocompleteEntryMap>
        {
            return std::nullopt;
        }
    );

    Json::Array items;
    for (const auto& pair : acResult.entryMap)
    {
        Lsp::CompletionItem item;
        item.label = pair.first;
        item.kind = mapCompletionKind(pair.second.kind, pair.second.type);
        item.deprecated = pair.second.deprecated;

        if (pair.second.type)
        {
            item.detail = toString(*pair.second.type);
            TypeId followed = follow(*pair.second.type);
            if (const FunctionType* fn = get<FunctionType>(followed))
            {
                std::string sig = "(";
                size_t argIdx = 0;
                for (auto pit = TypePackIterator(fn->argTypes); pit != end(fn->argTypes); ++pit, ++argIdx)
                {
                    if (argIdx > 0) sig += ", ";
                    std::string pName = (argIdx < fn->argNames.size() && fn->argNames[argIdx]) ? fn->argNames[argIdx]->name : ("arg" + std::to_string(argIdx + 1));
                    sig += pName + ": " + toString(*pit);
                }
                sig += ")";
                if (fn->retTypes)
                    sig += " -> " + toString(fn->retTypes);
                item.detail = sig;
            }
        }

        if (pair.second.insertText)
            item.insertText = *pair.second.insertText;

        if (pair.second.documentationSymbol)
            item.documentation = *pair.second.documentationSymbol;

        items.push_back(item.toJson());
    }

    Json::Object result;
    result.emplace_back("isIncomplete", Json::Value(false));
    result.emplace_back("items", Json::Value(std::move(items)));

    return Json::Value(std::move(result));
}

Json::Value LspServer::handleCompletionResolve(const Json::Value& params)
{
    Json::Object item = params.isObject() ? params.getObject() : Json::Object{};
    if (!params.has("documentation"))
    {
        std::string detail = params.has("detail") ? params.get("detail")->getString() : "";
        if (!detail.empty())
        {
            Json::Object doc;
            doc.emplace_back("kind", Json::Value("markdown"));
            doc.emplace_back("value", Json::Value("```luau\n" + detail + "\n```"));
            item.emplace_back("documentation", Json::Value(std::move(doc)));
        }
    }
    return Json::Value(std::move(item));
}

Json::Value LspServer::handleDefinition(const Json::Value& params)
{
    const auto* textDoc = params.get("textDocument");
    const auto* posVal = params.get("position");
    if (!textDoc || !posVal)
        return Json::Value(nullptr);

    const auto* uriVal = textDoc->get("uri");
    if (!uriVal || !uriVal->isString())
        return Json::Value(nullptr);

    std::string uri = uriVal->getString();
    DocumentState* doc = getDocument(uri);
    if (!doc)
        return Json::Value(nullptr);

    Lsp::Position lspPos = Lsp::Position::fromJson(*posVal);
    Position luauPos = lspPos.toLuau();

    frontend.check(doc->path);

    SourceModule* sm = frontend.getSourceModule(doc->path);
    ModulePtr module = frontend.moduleResolver.getModule(doc->path);
    if (!sm || !module)
        return Json::Value(nullptr);

    std::optional<Binding> binding = findBindingAtPosition(*module, *sm, luauPos);
    if (binding && binding->location.begin != Position{0, 0})
    {
        Lsp::Location loc;
        loc.uri = uri;
        loc.range = Lsp::Range::fromLuau(binding->location);
        return loc.toJson();
    }

    std::vector<AstNode*> ancestry = findAstAncestryOfPosition(*sm, luauPos);
    if (!ancestry.empty())
    {
        AstNode* target = ancestry.back();
        if (AstExprConstantString* str = target->as<AstExprConstantString>())
        {
            std::string modPath{str->value.data, str->value.size};
            FileNavigationContext navContext{doc->path};
            Require::ErrorHandler errHandler;
            Require::Navigator nav(navContext, errHandler);
            if (nav.navigate(std::move(modPath)) == Require::Navigator::Status::Success)
            {
                if (std::optional<std::string> resolvedId = navContext.getIdentifier())
                {
                    Lsp::Location loc;
                    loc.uri = Lsp::pathToUri(*resolvedId);
                    loc.range = Lsp::Range{Lsp::Position{0, 0}, Lsp::Position{0, 0}};
                    return loc.toJson();
                }
            }
        }
    }

    return Json::Value(nullptr);
}

Json::Value LspServer::handleTypeDefinition(const Json::Value& params)
{
    return handleDefinition(params);
}

namespace
{
struct SymbolCollector : public AstVisitor
{
    using AstVisitor::visit;
    std::vector<Lsp::DocumentSymbol> symbols;

    bool visit(AstStatLocalFunction* func) override
    {
        Lsp::DocumentSymbol sym;
        sym.name = func->name->name.value;
        sym.kind = Lsp::SymbolKind::Function;
        sym.range = Lsp::Range::fromLuau(func->location);
        sym.selectionRange = Lsp::Range::fromLuau(func->name->location);
        symbols.push_back(std::move(sym));
        return true;
    }

    bool visit(AstStatFunction* func) override
    {
        Lsp::DocumentSymbol sym;
        sym.name = "<function>";
        if (auto id = func->name->as<AstExprGlobal>())
            sym.name = id->name.value;
        else if (auto idx = func->name->as<AstExprIndexName>())
            sym.name = idx->index.value;

        sym.kind = Lsp::SymbolKind::Function;
        sym.range = Lsp::Range::fromLuau(func->location);
        sym.selectionRange = Lsp::Range::fromLuau(func->name->location);
        symbols.push_back(std::move(sym));
        return true;
    }

    bool visit(AstStatTypeAlias* alias) override
    {
        Lsp::DocumentSymbol sym;
        sym.name = alias->name.value;
        sym.kind = Lsp::SymbolKind::Interface;
        sym.range = Lsp::Range::fromLuau(alias->location);
        sym.selectionRange = Lsp::Range::fromLuau(alias->location);
        symbols.push_back(std::move(sym));
        return true;
    }

    bool visit(AstStatLocal* local) override
    {
        for (size_t i = 0; i < local->vars.size; ++i)
        {
            AstLocal* var = local->vars.data[i];
            Lsp::DocumentSymbol sym;
            sym.name = var->name.value;
            sym.kind = Lsp::SymbolKind::Variable;
            sym.range = Lsp::Range::fromLuau(local->location);
            sym.selectionRange = Lsp::Range::fromLuau(var->location);
            symbols.push_back(std::move(sym));
        }
        return true;
    }

    bool visit(AstStatDeclareGlobal* decl) override
    {
        Lsp::DocumentSymbol sym;
        sym.name = decl->name.value;
        sym.kind = Lsp::SymbolKind::Variable;
        sym.range = Lsp::Range::fromLuau(decl->location);
        sym.selectionRange = Lsp::Range::fromLuau(decl->location);
        symbols.push_back(std::move(sym));
        return true;
    }

    bool visit(AstStatDeclareFunction* decl) override
    {
        Lsp::DocumentSymbol sym;
        sym.name = decl->name.value;
        sym.kind = Lsp::SymbolKind::Function;
        sym.range = Lsp::Range::fromLuau(decl->location);
        sym.selectionRange = Lsp::Range::fromLuau(decl->location);
        symbols.push_back(std::move(sym));
        return true;
    }

    bool visit(AstStatDeclareExternType* decl) override
    {
        Lsp::DocumentSymbol sym;
        sym.name = decl->name.value;
        sym.kind = Lsp::SymbolKind::Class;
        sym.range = Lsp::Range::fromLuau(decl->location);
        sym.selectionRange = Lsp::Range::fromLuau(decl->location);
        symbols.push_back(std::move(sym));
        return true;
    }
};
} // namespace

Json::Value LspServer::handleDocumentSymbol(const Json::Value& params)
{
    const auto* textDoc = params.get("textDocument");
    if (!textDoc)
        return Json::Value(Json::Array{});

    const auto* uriVal = textDoc->get("uri");
    if (!uriVal || !uriVal->isString())
        return Json::Value(Json::Array{});

    std::string uri = uriVal->getString();
    DocumentState* doc = getDocument(uri);
    if (!doc)
        return Json::Value(Json::Array{});

    frontend.check(doc->path);
    SourceModule* sm = frontend.getSourceModule(doc->path);
    if (!sm || !sm->root)
        return Json::Value(Json::Array{});

    SymbolCollector collector;
    sm->root->visit(&collector);

    Json::Array result;
    for (const auto& s : collector.symbols)
        result.push_back(s.toJson());

    return Json::Value(std::move(result));
}

namespace
{
struct LocalReferenceFinder : public AstVisitor
{
    using AstVisitor::visit;
    const AstLocal* targetLocal = nullptr;
    std::vector<Location> occurrences;

    explicit LocalReferenceFinder(const AstLocal* local)
        : targetLocal(local)
    {
    }

    bool visit(AstExprLocal* expr) override
    {
        if (expr->local == targetLocal)
            occurrences.push_back(expr->location);
        return true;
    }

    bool visit(AstStatLocal* stat) override
    {
        for (size_t i = 0; i < stat->vars.size; ++i)
        {
            if (stat->vars.data[i] == targetLocal)
                occurrences.push_back(stat->vars.data[i]->location);
        }
        return true;
    }

    bool visit(AstStatLocalFunction* stat) override
    {
        if (stat->name == targetLocal)
            occurrences.push_back(stat->name->location);
        return true;
    }
};
} // namespace

Json::Value LspServer::handleDocumentHighlight(const Json::Value& params)
{
    const auto* textDoc = params.get("textDocument");
    const auto* posVal = params.get("position");
    if (!textDoc || !posVal)
        return Json::Value(Json::Array{});

    const auto* uriVal = textDoc->get("uri");
    if (!uriVal || !uriVal->isString())
        return Json::Value(Json::Array{});

    std::string uri = uriVal->getString();
    DocumentState* doc = getDocument(uri);
    if (!doc)
        return Json::Value(Json::Array{});

    Lsp::Position lspPos = Lsp::Position::fromJson(*posVal);
    Position luauPos = lspPos.toLuau();

    frontend.check(doc->path);
    SourceModule* sm = frontend.getSourceModule(doc->path);
    if (!sm || !sm->root)
        return Json::Value(Json::Array{});

    ExprOrLocal exprOrLocal = findExprOrLocalAtPosition(*sm, luauPos);
    AstLocal* local = exprOrLocal.getLocal();
    if (!local && exprOrLocal.getExpr())
    {
        if (auto el = exprOrLocal.getExpr()->as<AstExprLocal>())
            local = el->local;
    }

    if (!local)
        return Json::Value(Json::Array{});

    LocalReferenceFinder finder(local);
    sm->root->visit(&finder);

    Json::Array highlights;
    for (const auto& loc : finder.occurrences)
    {
        Json::Object h;
        h.emplace_back("range", Lsp::Range::fromLuau(loc).toJson());
        h.emplace_back("kind", Json::Value(1)); // Text = 1
        highlights.push_back(Json::Value(std::move(h)));
    }

    return Json::Value(std::move(highlights));
}

Json::Value LspServer::handleReferences(const Json::Value& params)
{
    const auto* textDoc = params.get("textDocument");
    const auto* posVal = params.get("position");
    if (!textDoc || !posVal)
        return Json::Value(Json::Array{});

    const auto* uriVal = textDoc->get("uri");
    if (!uriVal || !uriVal->isString())
        return Json::Value(Json::Array{});

    std::string uri = uriVal->getString();
    DocumentState* doc = getDocument(uri);
    if (!doc)
        return Json::Value(Json::Array{});

    Lsp::Position lspPos = Lsp::Position::fromJson(*posVal);
    Position luauPos = lspPos.toLuau();

    frontend.check(doc->path);
    SourceModule* sm = frontend.getSourceModule(doc->path);
    if (!sm || !sm->root)
        return Json::Value(Json::Array{});

    ExprOrLocal exprOrLocal = findExprOrLocalAtPosition(*sm, luauPos);
    AstLocal* local = exprOrLocal.getLocal();
    if (!local && exprOrLocal.getExpr())
    {
        if (auto el = exprOrLocal.getExpr()->as<AstExprLocal>())
            local = el->local;
    }

    if (!local)
        return Json::Value(Json::Array{});

    LocalReferenceFinder finder(local);
    sm->root->visit(&finder);

    Json::Array references;
    for (const auto& loc : finder.occurrences)
    {
        Lsp::Location l;
        l.uri = uri;
        l.range = Lsp::Range::fromLuau(loc);
        references.push_back(l.toJson());
    }

    return Json::Value(std::move(references));
}

Json::Value LspServer::handlePrepareRename(const Json::Value& params)
{
    const auto* textDoc = params.get("textDocument");
    const auto* posVal = params.get("position");
    if (!textDoc || !posVal)
        return Json::Value(nullptr);

    const auto* uriVal = textDoc->get("uri");
    if (!uriVal || !uriVal->isString())
        return Json::Value(nullptr);

    std::string uri = uriVal->getString();
    DocumentState* doc = getDocument(uri);
    if (!doc)
        return Json::Value(nullptr);

    Lsp::Position lspPos = Lsp::Position::fromJson(*posVal);
    Position luauPos = lspPos.toLuau();

    frontend.check(doc->path);
    SourceModule* sm = frontend.getSourceModule(doc->path);
    if (!sm || !sm->root)
        return Json::Value(nullptr);

    ExprOrLocal exprOrLocal = findExprOrLocalAtPosition(*sm, luauPos);
    AstLocal* local = exprOrLocal.getLocal();
    if (!local && exprOrLocal.getExpr())
    {
        if (auto el = exprOrLocal.getExpr()->as<AstExprLocal>())
            local = el->local;
    }

    if (!local)
        return Json::Value(nullptr);

    Lsp::Range r = Lsp::Range::fromLuau(local->location);
    return r.toJson();
}

Json::Value LspServer::handleRename(const Json::Value& params)
{
    const auto* textDoc = params.get("textDocument");
    const auto* posVal = params.get("position");
    const auto* newNameVal = params.get("newName");
    if (!textDoc || !posVal || !newNameVal || !newNameVal->isString())
        return Json::Value(nullptr);

    const auto* uriVal = textDoc->get("uri");
    if (!uriVal || !uriVal->isString())
        return Json::Value(nullptr);

    std::string uri = uriVal->getString();
    std::string newName = newNameVal->getString();
    DocumentState* doc = getDocument(uri);
    if (!doc)
        return Json::Value(nullptr);

    Lsp::Position lspPos = Lsp::Position::fromJson(*posVal);
    Position luauPos = lspPos.toLuau();

    frontend.check(doc->path);
    SourceModule* sm = frontend.getSourceModule(doc->path);
    if (!sm || !sm->root)
        return Json::Value(nullptr);

    ExprOrLocal exprOrLocal = findExprOrLocalAtPosition(*sm, luauPos);
    AstLocal* local = exprOrLocal.getLocal();
    if (!local && exprOrLocal.getExpr())
    {
        if (auto el = exprOrLocal.getExpr()->as<AstExprLocal>())
            local = el->local;
    }

    if (!local)
        return Json::Value(nullptr);

    LocalReferenceFinder finder(local);
    sm->root->visit(&finder);

    std::vector<Lsp::TextEdit> edits;
    for (const auto& loc : finder.occurrences)
    {
        Lsp::TextEdit edit;
        edit.range = Lsp::Range::fromLuau(loc);
        edit.newText = newName;
        edits.push_back(std::move(edit));
    }

    Lsp::WorkspaceEdit wsEdit;
    wsEdit.changes.emplace_back(uri, std::move(edits));

    return wsEdit.toJson();
}

namespace
{
struct SemanticTokenCollector : public AstVisitor
{
    using AstVisitor::visit;
    struct RawToken
    {
        int line = 0;
        int col = 0;
        int length = 0;
        int tokenType = 0;
        int tokenModifiers = 0;
    };

    std::vector<RawToken> tokens;

    // Token types index mapping
    // 0: type, 1: class, 2: enum, 3: interface, 4: typeParameter, 5: parameter, 6: variable,
    // 7: property, 8: function, 9: method, 10: keyword, 11: comment, 12: string, 13: number, 14: operator

    void addToken(const Location& loc, int tokenType, int tokenModifiers = 0)
    {
        if (loc.begin.line != loc.end.line)
            return;

        int len = static_cast<int>(loc.end.column - loc.begin.column);
        if (len <= 0)
            return;

        RawToken tok;
        tok.line = static_cast<int>(loc.begin.line);
        tok.col = static_cast<int>(loc.begin.column);
        tok.length = len;
        tok.tokenType = tokenType;
        tok.tokenModifiers = tokenModifiers;
        tokens.push_back(tok);
    }

    bool visit(AstStatLocalFunction* func) override
    {
        addToken(func->name->location, 8); // function
        return true;
    }

    bool visit(AstStatFunction* func) override
    {
        addToken(func->name->location, 8); // function
        return true;
    }

    bool visit(AstStatTypeAlias* alias) override
    {
        addToken(alias->location, 0); // type
        return true;
    }

    bool visit(AstTypeReference* typeRef) override
    {
        addToken(typeRef->location, 0); // type
        return true;
    }

    bool visit(AstExprLocal* expr) override
    {
        addToken(expr->location, 6); // variable
        return true;
    }

    bool visit(AstExprIndexName* expr) override
    {
        addToken(expr->indexLocation, 7); // property
        return true;
    }

    bool visit(AstExprConstantNumber* expr) override
    {
        addToken(expr->location, 13); // number
        return true;
    }

    bool visit(AstExprConstantString* expr) override
    {
        addToken(expr->location, 12); // string
        return true;
    }
};
} // namespace

Json::Value LspServer::handleSemanticTokensFull(const Json::Value& params)
{
    const auto* textDoc = params.get("textDocument");
    if (!textDoc)
        return Json::Value(nullptr);

    const auto* uriVal = textDoc->get("uri");
    if (!uriVal || !uriVal->isString())
        return Json::Value(nullptr);

    std::string uri = uriVal->getString();
    DocumentState* doc = getDocument(uri);
    if (!doc)
        return Json::Value(nullptr);

    frontend.check(doc->path);
    SourceModule* sm = frontend.getSourceModule(doc->path);
    if (!sm || !sm->root)
        return Json::Value(nullptr);

    SemanticTokenCollector collector;
    sm->root->visit(&collector);

    std::sort(
        collector.tokens.begin(),
        collector.tokens.end(),
        [](const auto& a, const auto& b)
        {
            if (a.line != b.line)
                return a.line < b.line;
            return a.col < b.col;
        }
    );

    Json::Array data;
    int prevLine = 0;
    int prevCol = 0;

    for (const auto& tok : collector.tokens)
    {
        int deltaLine = tok.line - prevLine;
        int deltaCol = (deltaLine == 0) ? (tok.col - prevCol) : tok.col;

        data.push_back(Json::Value(deltaLine));
        data.push_back(Json::Value(deltaCol));
        data.push_back(Json::Value(tok.length));
        data.push_back(Json::Value(tok.tokenType));
        data.push_back(Json::Value(tok.tokenModifiers));

        prevLine = tok.line;
        prevCol = tok.col;
    }

    Json::Object result;
    result.emplace_back("data", Json::Value(std::move(data)));
    return Json::Value(std::move(result));
}

namespace
{
struct CallFinder : public AstVisitor
{
    using AstVisitor::visit;
    Position pos;
    AstExprCall* targetCall = nullptr;

    explicit CallFinder(Position p) : pos(p) {}

    bool visit(AstExprCall* call) override
    {
        if (call->location.containsClosed(pos))
        {
            targetCall = call;
        }
        return true;
    }
};
} // namespace

Json::Value LspServer::handleSignatureHelp(const Json::Value& params)
{
    const auto* textDoc = params.get("textDocument");
    const auto* posVal = params.get("position");
    if (!textDoc || !posVal)
        return Json::Value(nullptr);

    const auto* uriVal = textDoc->get("uri");
    if (!uriVal || !uriVal->isString())
        return Json::Value(nullptr);

    std::string uri = uriVal->getString();
    DocumentState* doc = getDocument(uri);
    if (!doc)
        return Json::Value(nullptr);

    Lsp::Position lspPos = Lsp::Position::fromJson(*posVal);
    Position luauPos = lspPos.toLuau();

    frontend.check(doc->path);
    SourceModule* sm = frontend.getSourceModule(doc->path);
    ModulePtr module = frontend.moduleResolver.getModule(doc->path);
    if (!sm || !sm->root || !module)
        return Json::Value(nullptr);

    CallFinder finder(luauPos);
    sm->root->visit(&finder);

    if (!finder.targetCall)
        return Json::Value(nullptr);

    AstExprCall* call = finder.targetCall;

    int activeParam = 0;
    for (size_t i = 0; i < call->args.size; ++i)
    {
        if (luauPos > call->args.data[i]->location.end)
            activeParam = static_cast<int>(i + 1);
    }

    std::string sigLabel;
    Json::Object sigInfo;

    if (auto it = module->astTypes.find(call->func))
    {
        TypeId ty = follow(*it);
        sigLabel = toString(ty);

        if (const FunctionType* fn = get<FunctionType>(ty))
        {
            Json::Array paramsArr;
            size_t argIdx = 0;
            for (auto pit = TypePackIterator(fn->argTypes); pit != end(fn->argTypes); ++pit, ++argIdx)
            {
                std::string pName = (argIdx < fn->argNames.size() && fn->argNames[argIdx]) ? fn->argNames[argIdx]->name : ("arg" + std::to_string(argIdx + 1));
                std::string pType = toString(*pit);
                std::string pLabel = pName + ": " + pType;

                Json::Object paramObj;
                paramObj.emplace_back("label", Json::Value(pLabel));
                paramsArr.push_back(Json::Value(std::move(paramObj)));
            }
            sigInfo.emplace_back("parameters", Json::Value(std::move(paramsArr)));
        }
    }
    else
    {
        sigLabel = "function(...)";
    }

    sigInfo.emplace_back("label", Json::Value(sigLabel));

    Json::Array signatures;
    signatures.push_back(Json::Value(std::move(sigInfo)));

    Json::Object result;
    result.emplace_back("signatures", Json::Value(std::move(signatures)));
    result.emplace_back("activeSignature", Json::Value(0));
    result.emplace_back("activeParameter", Json::Value(activeParam));

    return Json::Value(std::move(result));
}

namespace
{
struct InlayHintCollector : public AstVisitor
{
    using AstVisitor::visit;
    const Module& module;
    std::vector<Lsp::InlayHint> hints;

    explicit InlayHintCollector(const Module& m) : module(m) {}

    bool visit(AstStatLocal* stat) override
    {
        for (size_t i = 0; i < stat->vars.size; ++i)
        {
            AstLocal* var = stat->vars.data[i];
            if (!var->annotation && i < stat->values.size)
            {
                if (auto it = module.astTypes.find(stat->values.data[i]))
                {
                    TypeId ty = follow(*it);
                    std::string typeStr = toString(ty);
                    if (typeStr != "any" && typeStr != "nil" && !typeStr.empty())
                    {
                        Lsp::InlayHint hint;
                        hint.position = Lsp::Position::fromLuau(var->location.end);
                        hint.label = ": " + typeStr;
                        hint.kind = 1; // Type hint
                        hint.paddingLeft = false;
                        hint.paddingRight = true;
                        hints.push_back(std::move(hint));
                    }
                }
            }
        }
        return true;
    }
};
} // namespace

Json::Value LspServer::handleInlayHint(const Json::Value& params)
{
    const auto* textDoc = params.get("textDocument");
    if (!textDoc)
        return Json::Value(Json::Array{});

    const auto* uriVal = textDoc->get("uri");
    if (!uriVal || !uriVal->isString())
        return Json::Value(Json::Array{});

    std::string uri = uriVal->getString();
    DocumentState* doc = getDocument(uri);
    if (!doc)
        return Json::Value(Json::Array{});

    frontend.check(doc->path);
    SourceModule* sm = frontend.getSourceModule(doc->path);
    ModulePtr module = frontend.moduleResolver.getModule(doc->path);
    if (!sm || !sm->root || !module)
        return Json::Value(Json::Array{});

    InlayHintCollector collector(*module);
    sm->root->visit(&collector);

    Json::Array result;
    for (const auto& h : collector.hints)
        result.push_back(h.toJson());

    return Json::Value(std::move(result));
}

Json::Value LspServer::handleCodeAction(const Json::Value& params)
{
    return Json::Value(Json::Array{});
}

Json::Value LspServer::handleLuauAst(const Json::Value& params)
{
    const auto* textDoc = params.get("textDocument");
    if (!textDoc)
        return Json::Value(nullptr);

    const auto* uriVal = textDoc->get("uri");
    if (!uriVal || !uriVal->isString())
        return Json::Value(nullptr);

    std::string uri = uriVal->getString();
    DocumentState* doc = getDocument(uri);
    if (!doc)
        return Json::Value(nullptr);

    frontend.check(doc->path);
    SourceModule* sm = frontend.getSourceModule(doc->path);
    if (!sm || !sm->root)
        return Json::Value(nullptr);

    std::string jsonAst = toJson(sm->root);
    auto parsed = Json::parse(jsonAst);
    return parsed ? *parsed : Json::Value(jsonAst);
}

Json::Value LspServer::handleLuauTypes(const Json::Value& params)
{
    const auto* textDoc = params.get("textDocument");
    if (!textDoc)
        return Json::Value(nullptr);

    const auto* uriVal = textDoc->get("uri");
    if (!uriVal || !uriVal->isString())
        return Json::Value(nullptr);

    std::string uri = uriVal->getString();
    DocumentState* doc = getDocument(uri);
    if (!doc)
        return Json::Value(nullptr);

    frontend.check(doc->path);
    SourceModule* sm = frontend.getSourceModule(doc->path);
    ModulePtr module = frontend.moduleResolver.getModule(doc->path);
    if (!sm || !module)
        return Json::Value(nullptr);

    Json::Object typesObj;
    for (const auto& pair : module->declaredGlobals)
    {
        typesObj.emplace_back(pair.first, Json::Value(toString(pair.second)));
    }

    for (const auto& pair : module->astTypes)
    {
        if (auto local = pair.first->as<AstExprLocal>())
        {
            typesObj.emplace_back(local->local->name.value, Json::Value(toString(pair.second)));
        }
    }

    return Json::Value(std::move(typesObj));
}

Json::Value LspServer::handleLuauBytecode(const Json::Value& params)
{
    const auto* textDoc = params.get("textDocument");
    if (!textDoc)
        return Json::Value(nullptr);

    const auto* uriVal = textDoc->get("uri");
    if (!uriVal || !uriVal->isString())
        return Json::Value(nullptr);

    std::string uri = uriVal->getString();
    DocumentState* doc = getDocument(uri);
    if (!doc)
        return Json::Value(nullptr);

    CompileOptions opts;
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    std::string bytecode = compile(doc->text, opts);

    Json::Object res;
    res.emplace_back("bytecodeLength", Json::Value(static_cast<int64_t>(bytecode.size())));
    return Json::Value(std::move(res));
}

Json::Value LspServer::handleLuauEval(const Json::Value& params)
{
    const auto* exprVal = params.get("expression");
    if (!exprVal || !exprVal->isString())
        return Json::Value(nullptr);

    std::string code = exprVal->getString();
    std::string toRun = "return " + code;

    std::unique_ptr<lua_State, void (*)(lua_State*)> L(luaL_newstate(), lua_close);
    luaL_openlibs(L.get());
    luaL_sandbox(L.get());

    CompileOptions opts;
    std::string bytecode = compile(toRun, opts);
    if (bytecode.empty() || bytecode[0] == '\0')
    {
        bytecode = compile(code, opts);
    }

    if (luau_load(L.get(), "=lsp_eval", bytecode.data(), bytecode.size(), 0) != 0)
    {
        std::string err = lua_tostring(L.get(), -1);
        Json::Object res;
        res.emplace_back("error", Json::Value(err));
        return Json::Value(std::move(res));
    }

    if (lua_pcall(L.get(), 0, 1, 0) != 0)
    {
        std::string err = lua_tostring(L.get(), -1);
        Json::Object res;
        res.emplace_back("error", Json::Value(err));
        return Json::Value(std::move(res));
    }

    std::string evalResult = luaL_tolstring(L.get(), -1, nullptr);
    Json::Object res;
    res.emplace_back("result", Json::Value(evalResult));
    return Json::Value(std::move(res));
}

namespace
{
struct RequireCollector : public AstVisitor
{
    using AstVisitor::visit;
    std::vector<std::pair<std::string, Location>> moduleRequires;

    bool visit(AstExprCall* call) override
    {
        if (auto global = call->func->as<AstExprGlobal>())
        {
            if (global->name == "require" && call->args.size >= 1)
            {
                if (auto str = call->args.data[0]->as<AstExprConstantString>())
                {
                    moduleRequires.emplace_back(std::string(str->value.data, str->value.size), call->location);
                }
            }
        }
        return true;
    }
};
} // namespace

Json::Value LspServer::handleLuauRequireGraph(const Json::Value& params)
{
    const auto* textDoc = params.get("textDocument");
    if (!textDoc)
        return Json::Value(nullptr);

    const auto* uriVal = textDoc->get("uri");
    if (!uriVal || !uriVal->isString())
        return Json::Value(nullptr);

    std::string uri = uriVal->getString();
    DocumentState* doc = getDocument(uri);
    if (!doc)
        return Json::Value(nullptr);

    frontend.check(doc->path);
    SourceModule* sm = frontend.getSourceModule(doc->path);
    if (!sm || !sm->root)
        return Json::Value(nullptr);

    RequireCollector collector;
    sm->root->visit(&collector);

    Json::Array dependencies;
    for (const auto& req : collector.moduleRequires)
    {
        Json::Object item;
        item.emplace_back("module", Json::Value(req.first));
        item.emplace_back("range", Lsp::Range::fromLuau(req.second).toJson());

        FileNavigationContext navContext{doc->path};
        Require::ErrorHandler errHandler;
        Require::Navigator nav(navContext, errHandler);
        if (nav.navigate(std::string(req.first)) == Require::Navigator::Status::Success)
        {
            if (auto id = navContext.getIdentifier())
            {
                item.emplace_back("resolvedPath", Json::Value(*id));
                item.emplace_back("resolvedUri", Json::Value(Lsp::pathToUri(*id)));
            }
        }

        dependencies.push_back(Json::Value(std::move(item)));
    }

    Json::Object result;
    result.emplace_back("uri", Json::Value(uri));
    result.emplace_back("dependencies", Json::Value(std::move(dependencies)));
    return Json::Value(std::move(result));
}

Json::Value LspServer::handleLuauVfsStats(const Json::Value& params)
{
    size_t openDocsMemory = 0;
    size_t openDocsCount = documents.size();
    for (const auto& pair : documents)
        openDocsMemory += pair.second.getMemoryUsage();

    size_t cachedFilesCount = fileResolver.cachedFiles.size();
    size_t cachedFilesMemory = fileResolver.getCacheMemoryUsage();

    size_t rawCachedSize = 0;
    for (const auto& pair : fileResolver.cachedFiles)
        rawCachedSize += pair.second.uncompressedSize();

    double savingsPercent = 0.0;
    if (rawCachedSize > 0 && cachedFilesMemory < rawCachedSize)
    {
        savingsPercent = (1.0 - (static_cast<double>(cachedFilesMemory) / static_cast<double>(rawCachedSize))) * 100.0;
    }

    Json::Object stats;
    stats.emplace_back("openDocumentsCount", Json::Value(static_cast<int64_t>(openDocsCount)));
    stats.emplace_back("openDocumentsMemoryBytes", Json::Value(static_cast<int64_t>(openDocsMemory)));
    stats.emplace_back("cachedFilesCount", Json::Value(static_cast<int64_t>(cachedFilesCount)));
    stats.emplace_back("cachedFilesMemoryBytes", Json::Value(static_cast<int64_t>(cachedFilesMemory)));
    stats.emplace_back("rawCachedSizeBytes", Json::Value(static_cast<int64_t>(rawCachedSize)));
    stats.emplace_back("compressionSavingsPercent", Json::Value(savingsPercent));

    return Json::Value(std::move(stats));
}

Json::Value LspServer::handleLuauVfsSnapshot(const Json::Value& params)
{
    Json::Array files;
    for (const auto& pair : documents)
    {
        Json::Object docObj;
        docObj.emplace_back("uri", Json::Value(pair.first));
        docObj.emplace_back("path", Json::Value(pair.second.path));
        docObj.emplace_back("version", Json::Value(pair.second.version));
        docObj.emplace_back("compressed", Json::Value(Vfs::compress(pair.second.text)));
        files.push_back(Json::Value(std::move(docObj)));
    }

    Json::Object res;
    res.emplace_back("documentCount", Json::Value(static_cast<int64_t>(documents.size())));
    res.emplace_back("documents", Json::Value(std::move(files)));
    return Json::Value(std::move(res));
}

JsonRpc::Response LspServer::handleRequest(const JsonRpc::Request& req, std::ostream* outNotification)
{
    if (req.method == "initialize")
        return JsonRpc::Response::ok(req.id, handleInitialize(req.params));

    if (req.method == "shutdown")
    {
        isShutdown = true;
        return JsonRpc::Response::ok(req.id, Json::Value(nullptr));
    }

    if (req.method == "textDocument/hover")
        return JsonRpc::Response::ok(req.id, handleHover(req.params));

    if (req.method == "textDocument/completion")
        return JsonRpc::Response::ok(req.id, handleCompletion(req.params));

    if (req.method == "completionItem/resolve" || req.method == "textDocument/completionItem/resolve")
        return JsonRpc::Response::ok(req.id, handleCompletionResolve(req.params));

    if (req.method == "textDocument/definition")
        return JsonRpc::Response::ok(req.id, handleDefinition(req.params));

    if (req.method == "textDocument/typeDefinition")
        return JsonRpc::Response::ok(req.id, handleTypeDefinition(req.params));

    if (req.method == "textDocument/documentSymbol")
        return JsonRpc::Response::ok(req.id, handleDocumentSymbol(req.params));

    if (req.method == "textDocument/documentHighlight")
        return JsonRpc::Response::ok(req.id, handleDocumentHighlight(req.params));

    if (req.method == "textDocument/references")
        return JsonRpc::Response::ok(req.id, handleReferences(req.params));

    if (req.method == "textDocument/prepareRename")
        return JsonRpc::Response::ok(req.id, handlePrepareRename(req.params));

    if (req.method == "textDocument/rename")
        return JsonRpc::Response::ok(req.id, handleRename(req.params));

    if (req.method == "textDocument/semanticTokens/full")
        return JsonRpc::Response::ok(req.id, handleSemanticTokensFull(req.params));

    if (req.method == "textDocument/signatureHelp")
        return JsonRpc::Response::ok(req.id, handleSignatureHelp(req.params));

    if (req.method == "textDocument/inlayHint")
        return JsonRpc::Response::ok(req.id, handleInlayHint(req.params));

    if (req.method == "textDocument/codeAction")
        return JsonRpc::Response::ok(req.id, handleCodeAction(req.params));

    // Luau extensions
    if (req.method == "luau/ast")
        return JsonRpc::Response::ok(req.id, handleLuauAst(req.params));

    if (req.method == "luau/types")
        return JsonRpc::Response::ok(req.id, handleLuauTypes(req.params));

    if (req.method == "luau/bytecode")
        return JsonRpc::Response::ok(req.id, handleLuauBytecode(req.params));

    if (req.method == "luau/eval")
        return JsonRpc::Response::ok(req.id, handleLuauEval(req.params));

    if (req.method == "luau/requireGraph")
        return JsonRpc::Response::ok(req.id, handleLuauRequireGraph(req.params));

    if (req.method == "luau/vfsStats")
        return JsonRpc::Response::ok(req.id, handleLuauVfsStats(req.params));

    if (req.method == "luau/vfsSnapshot")
        return JsonRpc::Response::ok(req.id, handleLuauVfsSnapshot(req.params));

    return JsonRpc::Response::err(req.id, -32601, "Method not found: " + req.method);
}

void LspServer::handleNotification(const JsonRpc::Request& notif, std::ostream* outNotification)
{
    if (notif.method == "initialized")
    {
        return;
    }

    if (notif.method == "exit")
    {
        exit(isShutdown ? 0 : 1);
    }

    if (notif.method == "textDocument/didOpen")
    {
        const auto* textDoc = notif.params.get("textDocument");
        if (textDoc)
        {
            const auto* uriVal = textDoc->get("uri");
            const auto* textVal = textDoc->get("text");
            const auto* verVal = textDoc->get("version");

            if (uriVal && uriVal->isString() && textVal && textVal->isString())
            {
                std::string uri = uriVal->getString();
                int ver = verVal ? static_cast<int>(verVal->getInt(1)) : 1;
                openDocument(uri, textVal->getString(), ver);

                if (outNotification)
                    publishDiagnostics(uri, *outNotification);
            }
        }
    }
    else if (notif.method == "textDocument/didChange")
    {
        const auto* textDoc = notif.params.get("textDocument");
        const auto* changes = notif.params.get("contentChanges");
        if (textDoc && changes && changes->isArray())
        {
            const auto* uriVal = textDoc->get("uri");
            const auto* verVal = textDoc->get("version");
            if (uriVal && uriVal->isString())
            {
                std::string uri = uriVal->getString();
                int ver = verVal ? static_cast<int>(verVal->getInt(1)) : 1;

                DocumentState* doc = getDocument(uri);
                if (doc)
                {
                    doc->version = ver;
                    for (const auto& change : changes->getArray())
                    {
                        if (const auto* rangeVal = change.get("range"))
                        {
                            if (const auto* textVal = change.get("text"))
                            {
                                Lsp::Range r = Lsp::Range::fromJson(*rangeVal);
                                doc->applyIncrementalChange(r, textVal->getString());
                            }
                        }
                        else if (const auto* textVal = change.get("text"))
                        {
                            doc->updateText(textVal->getString());
                        }
                    }
                    frontend.markDirty(doc->path);

                    if (outNotification)
                        publishDiagnostics(uri, *outNotification);
                }
            }
        }
    }
    else if (notif.method == "textDocument/didClose")
    {
        const auto* textDoc = notif.params.get("textDocument");
        if (textDoc)
        {
            const auto* uriVal = textDoc->get("uri");
            if (uriVal && uriVal->isString())
            {
                closeDocument(uriVal->getString());
            }
        }
    }
    else if (notif.method == "textDocument/didSave")
    {
        const auto* textDoc = notif.params.get("textDocument");
        if (textDoc)
        {
            const auto* uriVal = textDoc->get("uri");
            if (uriVal && uriVal->isString())
            {
                if (outNotification)
                    publishDiagnostics(uriVal->getString(), *outNotification);
            }
        }
    }
}

void LspServer::run(std::istream& in, std::ostream& out)
{
    while (true)
    {
        auto framed = JsonRpc::MessageReader::readFramedMessage(in);
        if (!framed)
            break;

        std::string parseErr;
        std::optional<Json::Value> jsonVal;
        if (framed->format == JsonRpc::TransportFormat::BinaryMsgPack)
        {
            jsonVal = Json::parseBinaryMsgPack(framed->payload, &parseErr);
        }
        else
        {
            jsonVal = Json::parse(framed->payload, &parseErr);
        }

        if (!jsonVal || !jsonVal->isObject())
        {
            JsonRpc::Response err = JsonRpc::Response::err(JsonRpc::Id(), -32700, "Parse error: " + parseErr);
            JsonRpc::MessageWriter::writeResponse(out, err, framed->format);
            continue;
        }

        JsonRpc::Request req;
        req.format = framed->format;
        if (const auto* method = jsonVal->get("method"))
        {
            if (method->isString())
                req.method = method->getString();
        }

        if (const auto* id = jsonVal->get("id"))
        {
            if (id->isNumber())
                req.id = JsonRpc::Id(id->getInt());
            else if (id->isString())
                req.id = JsonRpc::Id(id->getString());
        }
        else
        {
            req.isNotification = true;
        }

        if (const auto* params = jsonVal->get("params"))
            req.params = *params;

        if (req.isNotification)
        {
            handleNotification(req, &out);
        }
        else
        {
            JsonRpc::Response resp = handleRequest(req, &out);
            JsonRpc::MessageWriter::writeResponse(out, resp, framed->format);
        }
    }
}

int LspServer::runStdio()
{
    LspServer server;
    server.run(std::cin, std::cout);
    return 0;
}

int runLspServer()
{
    return LspServer::runStdio();
}

} // namespace Luau
