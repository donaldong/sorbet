#ifndef RUBY_TYPER_LSPLOOP_H
#define RUBY_TYPER_LSPLOOP_H

#include "core/core.h"
#include "main/lsp/LSPMessage.h"
#include "main/lsp/LSPPreprocessor.h"
#include "main/lsp/LSPTypecheckerCoordinator.h"
#include <chrono>
#include <optional>

//  _     ____  ____
// | |   / ___||  _ _\
// | |   \___ \| |_) |
// | |___ ___) |  __/
// |_____|____/|_|
//
//
// This is an implementation of LSP protocol (version 3.13) for Sorbet
namespace sorbet::realmain::lsp {

class LSPInput;
class LSPConfiguration;

enum class LSPErrorCodes {
    // Defined by JSON RPC
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602, // todo
    InternalError = -32603,
    ServerErrorStart = -32099,
    ServerErrorEnd = -32000,
    ServerNotInitialized = -32002,
    UnknownErrorCode = -32001,

    // Defined by the LSP
    RequestCancelled = -32800,
};

class LSPLoop {
    friend class LSPWrapper;

    /** Encapsulates the active configuration for the language server. */
    std::shared_ptr<const LSPConfiguration> config;
    /** The LSP preprocessor standardizes incoming messages and combines edits. */
    LSPPreprocessor preprocessor;
    /** The LSP typechecker coordinator typechecks file updates and runs queries. */
    LSPTypecheckerCoordinator typecheckerCoord;
    /**
     * The time that LSP last sent metrics to statsd -- if `opts.statsdHost` was specified.
     */
    std::chrono::time_point<std::chrono::steady_clock> lastMetricUpdateTime;
    /** ID of the main thread, which actually processes LSP requests and performs typechecking. */
    std::thread::id mainThreadId;
    /** Global state that we keep up-to-date with file edits. We do _not_ typecheck using this global state! We clone
     * this global state every time we need to perform a slow path typechecking operation. */
    std::unique_ptr<core::GlobalState> initialGS;
    /** Contains file hashes for the files stored in `initialGS`. Used to determine if an edit can be typechecked
     * incrementally. */
    std::vector<core::FileHash> globalStateHashes;
    /** Contains a copy of the last edit committed on the slow path. Used in slow path cancelation logic. */
    LSPFileUpdates lastSlowPathUpdate;
    /** Contains globalStatehashes evicted in `updates` */
    UnorderedMap<int, core::FileHash> lastSlowPathEvictedStateHashes;

    std::unique_ptr<KeyValueStore> kvstore; // always null for now.

    void addLocIfExists(const core::GlobalState &gs, std::vector<std::unique_ptr<Location>> &locs, core::Loc loc) const;
    std::vector<std::unique_ptr<Location>>
    extractLocations(const core::GlobalState &gs,
                     const std::vector<std::unique_ptr<core::lsp::QueryResponse>> &queryResponses,
                     std::vector<std::unique_ptr<Location>> locations = {}) const;

    LSPQueryResult queryByLoc(LSPTypechecker &typechecker, std::string_view uri, const Position &pos,
                              const LSPMethod forMethod, bool errorIfFileIsUntyped = true) const;
    LSPQueryResult queryBySymbol(LSPTypechecker &typechecker, core::SymbolRef symbol,
                                 const std::optional<std::string_view> uri = std::nullopt) const;

    std::unique_ptr<ResponseMessage>
    handleTextDocumentDocumentHighlight(LSPTypechecker &typechecker, const MessageId &id,
                                        const TextDocumentPositionParams &params) const;
    std::unique_ptr<ResponseMessage> handleTextDocumentHover(LSPTypechecker &typechecker, const MessageId &id,
                                                             const TextDocumentPositionParams &params) const;
    std::unique_ptr<ResponseMessage> handleTextDocumentDocumentSymbol(LSPTypechecker &typechecker, const MessageId &id,
                                                                      const DocumentSymbolParams &params) const;
    std::unique_ptr<ResponseMessage> handleWorkspaceSymbols(LSPTypechecker &typechecker, const MessageId &id,
                                                            const WorkspaceSymbolParams &params) const;
    std::vector<std::unique_ptr<Location>>
    getReferencesToSymbol(LSPTypechecker &typechecker, core::SymbolRef symbol,
                          std::vector<std::unique_ptr<Location>> locations = {}) const;
    std::vector<std::unique_ptr<DocumentHighlight>>
    getHighlightsToSymbolInFile(LSPTypechecker &typechecker, std::string_view uri, core::SymbolRef symbol,
                                std::vector<std::unique_ptr<DocumentHighlight>> highlights = {}) const;
    std::unique_ptr<ResponseMessage> handleTextDocumentReferences(LSPTypechecker &typechecker, const MessageId &id,
                                                                  const ReferenceParams &params) const;
    std::unique_ptr<ResponseMessage> handleTextDocumentDefinition(LSPTypechecker &typechecker, const MessageId &id,
                                                                  const TextDocumentPositionParams &params) const;
    std::unique_ptr<ResponseMessage> handleTextDocumentTypeDefinition(LSPTypechecker &typechecker, const MessageId &id,
                                                                      const TextDocumentPositionParams &params) const;
    std::unique_ptr<ResponseMessage> handleTextDocumentCompletion(LSPTypechecker &typechecker, const MessageId &id,
                                                                  const CompletionParams &params) const;
    std::unique_ptr<ResponseMessage> handleTextDocumentCodeAction(LSPTypechecker &typechecker, const MessageId &id,
                                                                  const CodeActionParams &params) const;
    std::unique_ptr<CompletionItem> getCompletionItemForMethod(LSPTypechecker &typechecker, core::SymbolRef what,
                                                               core::TypePtr receiverType,
                                                               const core::TypeConstraint *constraint,
                                                               const core::Loc queryLoc, std::string_view prefix,
                                                               size_t sortIdx) const;
    void findSimilarConstantOrIdent(const core::GlobalState &gs, const core::TypePtr receiverType,
                                    const core::Loc queryLoc,
                                    std::vector<std::unique_ptr<CompletionItem>> &items) const;
    std::unique_ptr<ResponseMessage> handleTextSignatureHelp(LSPTypechecker &typechecker, const MessageId &id,
                                                             const TextDocumentPositionParams &params) const;

    void processRequestInternal(LSPMessage &msg);

    /** Returns `true` if 5 minutes have elapsed since LSP last sent counters to statsd. */
    bool shouldSendCountersToStatsd(std::chrono::time_point<std::chrono::steady_clock> currentTime) const;
    /** Sends counters to statsd. */
    void sendCountersToStatsd(std::chrono::time_point<std::chrono::steady_clock> currentTime);

    /** Commits the given edit to `initialGS`, and returns a canonical LSPFileUpdates object containing indexed trees
     * and file hashes. */
    LSPFileUpdates commitEdit(SorbetWorkspaceEditParams &edit);

    LSPFileUpdates mergeUpdates(const LSPFileUpdates &older, const UnorderedMap<int, core::FileHash> &olderEvictions,
                                const LSPFileUpdates &newer,
                                const UnorderedMap<int, core::FileHash> &newerEvictions) const;

public:
    LSPLoop(std::unique_ptr<core::GlobalState> initialGS, const std::shared_ptr<LSPConfiguration> &config);
    /**
     * Runs the language server on a dedicated thread. Returns the final global state if it exits cleanly, or nullopt
     * on error.
     *
     * Reads input messages from the provided input object.
     */
    std::optional<std::unique_ptr<core::GlobalState>> runLSP(std::shared_ptr<LSPInput> input);
    void processRequest(std::unique_ptr<LSPMessage> msg);
    void processRequest(const std::string &json);
    /**
     * Processes a batch of requests. Performs pre-processing to avoid unnecessary work.
     */
    void processRequests(std::vector<std::unique_ptr<LSPMessage>> messages);
    /**
     * (For tests only) Retrieve the number of times typechecking has run.
     */
    int getTypecheckCount();
};

std::optional<std::string> findDocumentation(std::string_view sourceCode, int beginIndex);
bool hasSimilarName(const core::GlobalState &gs, core::NameRef name, std::string_view pattern);
bool hideSymbol(const core::GlobalState &gs, core::SymbolRef sym);
std::unique_ptr<MarkupContent> formatRubyMarkup(MarkupKind markupKind, std::string_view rubyMarkup,
                                                std::optional<std::string_view> explanation);
std::string prettyTypeForMethod(const core::GlobalState &gs, core::SymbolRef method, core::TypePtr receiver,
                                core::TypePtr retType, const core::TypeConstraint *constraint);
core::TypePtr getResultType(const core::GlobalState &gs, core::TypePtr type, core::SymbolRef inWhat,
                            core::TypePtr receiver, const core::TypeConstraint *constr);
SymbolKind symbolRef2SymbolKind(const core::GlobalState &gs, core::SymbolRef);

} // namespace sorbet::realmain::lsp
#endif // RUBY_TYPER_LSPLOOP_H
