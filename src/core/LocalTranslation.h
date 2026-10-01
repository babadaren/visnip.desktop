#pragma once

#include "core/TranslationCompositor.h"

#include <QByteArray>
#include <QMap>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

// Lite offline translation: request construction, output validation and
// paragraph planning for a local llama.cpp server running Hy-MT. Pure logic,
// no process or network access. The rules follow the precise engine's
// resources/offline/native_translation.py and translation_policy.py so both
// offline tiers accept and reject the same model outputs.
namespace Visnip::LocalMt {

// Only targets whose validation rules were verified are exposed.
bool supportsTargetLanguage(const QString& target);
// The Chinese language name used inside Hy-MT's prompt templates.
QString promptLanguageName(const QString& target);

// Text that must stay unchanged: URLs, owner/repo paths, numeric metadata,
// programming-language names.
bool isPreservedText(const QString& text);
// Chinese UI copy (possibly with product names) is already in zh-Hans.
bool alreadyInTarget(const QString& text, const QString& target);
// One or two capitalised words such as a product name ("Copilot").
bool looksLikeKeptName(const QString& text);

// Numbers compare by value (1,200 == 1200, full-width digits are ASCII);
// placeholders such as {name} or %s compare literally.
QMap<QString, int> protectedTokenCounts(const QString& text);
bool protectedTokensMatch(const QString& source, const QString& translation);

using GlossaryTerms = QVector<QPair<QString, QString>>;
// Terms occurring in this text only; unrelated entries bias a small model.
GlossaryTerms glossaryTerms(const QString& text,
                            const QStringList& pageTexts,
                            const QString& target);

// Shared page context placed first in every prompt so llama.cpp reuses the
// KV cache of the common prefix. Bounded by an approximate token budget.
QString buildPageContext(const QStringList& pageTexts);
QString buildPrompt(const QString& pageContext,
                    const GlossaryTerms& terms,
                    const QString& text,
                    const QString& target);
// Context-free prompt for one bounded retry, so a context-induced echo or
// refusal is not simply repeated.
QString buildRetryPrompt(const QString& text, const QString& target);
int maxOutputTokens(const QString& text);

// Removes wrapper quotes the model added and joins the lines of a one-line
// source.
QString cleanOutput(const QString& value, const QString& source, const QString& target);

// Empty when the translation is acceptable, otherwise a stable reason code:
// invalid_translation, protected_token_mismatch or sentence_not_translated.
QString rejectionReason(const QString& source, const QString& translation, const QString& target);

// Approximate model tokens: one per ideograph, about four Latin characters.
int estimateTokens(const QString& text);

// Parallel slots each prefill the shared page context, but decode several
// regions per step. Which wins depends on the machine: on a slow CPU prefill
// dominates and requests should run one after another (the context then stays
// cached in one slot); with fast prefill (GPU, recent CPU) parallel decoding
// shortens the job. Returns 1, 2 or 4 (bounded by maxSlots and the entries).
int chooseConcurrency(const QVector<int>& entryPromptTokens,
                      const QVector<int>& entryOutputTokens,
                      int sharedPrefixTokens,
                      double prefillTokensPerSecond,
                      double decodeTokensPerSecond,
                      int maxSlots);

QByteArray buildChatRequest(const QString& prompt, int maxTokens);
struct ChatResult {
    QString content;
    QString finishReason;
    int promptTokens = 0;     // whole prompt, cached or not
    int promptProcessed = 0;  // tokens actually prefilled for this request
    int completionTokens = 0;
    double promptMs = 0.0;
    double generationMs = 0.0;
};
bool parseChatResponse(const QByteArray& body, ChatResult* result, QString* error);

// Wrapped prose is translated as one paragraph and the translation is spread
// back over the original one-line units, so sentences are not cut at visual
// line ends. Short labels, menus and list items stay single units.
struct ParagraphPlan {
    QStringList texts;               // one request per entry, reading order
    QVector<QVector<int>> members;   // unit indices per entry, top to bottom
};
ParagraphPlan planParagraphs(const QVector<Translate::TextBlock>& units,
                             const QVector<Translate::TextBlock>& contextBlocks);

// Splits a paragraph translation over lines in proportion to their widths,
// breaking only where the target script allows it. Returns an empty list when
// every line cannot receive a non-empty piece.
QStringList splitAcrossLines(const QString& translation,
                             const QVector<int>& lineWidths,
                             const QString& target);

// Maps entry translations back to one string per unit. A paragraph whose
// translation cannot be spread keeps its source lines and is reported.
QStringList distributeTranslations(const ParagraphPlan& plan,
                                   const QVector<Translate::TextBlock>& units,
                                   const QStringList& translations,
                                   const QString& target,
                                   QVector<int>* failedEntries = nullptr);

} // namespace Visnip::LocalMt
