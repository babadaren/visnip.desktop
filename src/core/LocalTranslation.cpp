#include "core/LocalTranslation.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Visnip::LocalMt {
namespace {

// Each slot of the local server prefills the shared context once; on a CPU
// this prefill is paid per slot, so the context stays small.
constexpr int kContextTokenBudget = 320;
constexpr int kContextLineChars = 160;

bool isHan(QChar ch)
{
    const ushort u = ch.unicode();
    return u >= 0x3400 && u <= 0x9FFF;
}

// Wide East Asian characters (ideographs, kana, fullwidth forms, CJK punctuation).
bool isWide(QChar ch)
{
    const ushort u = ch.unicode();
    return (u >= 0x2E80 && u <= 0x9FFF) || (u >= 0xAC00 && u <= 0xD7AF)
        || (u >= 0xF900 && u <= 0xFAFF) || (u >= 0xFF00 && u <= 0xFF60)
        || (u >= 0x3000 && u <= 0x303F);
}

bool isAsciiLetter(QChar ch)
{
    return (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
        || (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'));
}

int hanCount(const QString& text)
{
    return static_cast<int>(std::count_if(text.cbegin(), text.cend(), isHan));
}

int latinLetterCount(const QString& text)
{
    return static_cast<int>(std::count_if(text.cbegin(), text.cend(), isAsciiLetter));
}

int latinWordCount(const QString& text)
{
    static const QRegularExpression word(QStringLiteral("[A-Za-z]+"));
    int count = 0;
    for (auto it = word.globalMatch(text); it.hasNext(); it.next()) {
        ++count;
    }
    return count;
}

bool endsSentence(const QString& text)
{
    static const QRegularExpression terminal(QStringLiteral("[。！？.!?:：]$"));
    return terminal.match(text.trimmed()).hasMatch();
}

// Short UI copy (button, menu item, tab) as opposed to prose.
bool isShortLabel(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 30 || endsSentence(trimmed)) {
        return false;
    }
    return trimmed.split(QLatin1Char(' '), Qt::SkipEmptyParts).size() <= 3;
}

const QStringList& languageNames()
{
    static const QStringList names = {
        QStringLiteral("Go"), QStringLiteral("JavaScript"), QStringLiteral("TypeScript"),
        QStringLiteral("Python"), QStringLiteral("Rust"), QStringLiteral("C"),
        QStringLiteral("C++"), QStringLiteral("C#"), QStringLiteral("Java"),
        QStringLiteral("Kotlin"), QStringLiteral("Swift"), QStringLiteral("PHP"),
        QStringLiteral("Ruby"), QStringLiteral("HTML"), QStringLiteral("CSS"),
        QStringLiteral("SQL"), QStringLiteral("Lua"), QStringLiteral("Dart"),
        QStringLiteral("Shell"), QStringLiteral("Scala"), QStringLiteral("Vue"),
    };
    return names;
}

bool metadataLine(const QString& text)
{
    static const QRegularExpression decorations(
        QStringLiteral("[\\x{2606}\\x{2605}\\x{2022}\\x{25CF}\\x{25CB}\\x{25A0}\\x{25A1}\\s]+"));
    static const QRegularExpression numeric(QStringLiteral("^[0-9.,+%kKmMbB:/()-]+$"));
    static const QVector<QRegularExpression> languageStats = []() {
        QVector<QRegularExpression> patterns;
        for (const QString& name : languageNames()) {
            patterns.append(QRegularExpression(QStringLiteral("^")
                + QRegularExpression::escape(name)
                + QStringLiteral("(?:\\s+[0-9.,+%kKmMbB]+)*$")));
        }
        return patterns;
    }();
    QString cleaned = text;
    cleaned.replace(decorations, QStringLiteral(" "));
    cleaned = cleaned.trimmed();
    if (numeric.match(cleaned).hasMatch()) {
        return true;
    }
    return std::any_of(languageStats.cbegin(), languageStats.cend(),
                       [&cleaned](const QRegularExpression& pattern) {
                           return pattern.match(cleaned).hasMatch();
                       });
}

bool isFullwidthDigit(QChar ch)
{
    return ch.unicode() >= 0xFF10 && ch.unicode() <= 0xFF19;
}

QString canonicalToken(const QString& token)
{
    if (token.isEmpty() || !(token.front().isDigit() || isFullwidthDigit(token.front()))) {
        return token;
    }
    QString value;
    value.reserve(token.size());
    for (const QChar ch : token) {
        value += isFullwidthDigit(ch) ? QChar(ch.unicode() - 0xFF10 + '0') : ch;
    }
    static const QRegularExpression thousands(QStringLiteral("(?<=[0-9]),(?=[0-9]{3}(?![0-9]))"));
    value.remove(thousands);
    return value;
}

void insertTerms(GlossaryTerms* selected, const GlossaryTerms& terms)
{
    // dict.update semantics: existing keys keep their position.
    for (const auto& term : terms) {
        auto existing = std::find_if(selected->begin(), selected->end(),
                                     [&term](const auto& item) { return item.first == term.first; });
        if (existing != selected->end()) {
            existing->second = term.second;
        } else {
            selected->append(term);
        }
    }
}

GlossaryTerms termList(std::initializer_list<std::pair<const char16_t*, const char16_t*>> items)
{
    GlossaryTerms terms;
    for (const auto& [source, target] : items) {
        terms.append({QString::fromUtf16(source), QString::fromUtf16(target)});
    }
    return terms;
}

const GlossaryTerms& repositoryTerms()
{
    static const GlossaryTerms terms = termList({
        {u"Code", u"代码"}, {u"Issues", u"议题"}, {u"Pull requests", u"合并请求"},
        {u"Agents", u"智能体"}, {u"Actions", u"工作流"}, {u"Projects", u"项目"},
        {u"Security and quality", u"安全与质量"}, {u"Insights", u"统计分析"},
        {u"Settings", u"设置"},
    });
    return terms;
}

const GlossaryTerms& softwareTerms()
{
    static const GlossaryTerms terms = termList({
        {u"code review", u"代码审查"}, {u"coding agent", u"编程智能体"},
        {u"security audit findings", u"审计发现"}, {u"battle-tested", u"经过实战验证"},
    });
    return terms;
}

const GlossaryTerms& accountTerms()
{
    static const GlossaryTerms terms = termList({
        {u"Profile", u"个人资料"}, {u"Repositories", u"仓库"}, {u"Stars", u"星标"},
        {u"Gists", u"代码片段"}, {u"Organizations", u"组织"}, {u"Enterprises", u"企业"},
        {u"Sponsors", u"赞助"}, {u"Settings", u"设置"}, {u"Feature preview", u"功能预览"},
        {u"Sign out", u"退出登录"}, {u"Try Enterprise", u"试用企业版"}, {u"Free", u"免费"},
    });
    return terms;
}

// Interface words a small model otherwise renders in their everyday sense.
const GlossaryTerms& interfaceTerms()
{
    static const GlossaryTerms terms = termList({
        {u"Appearance", u"外观"}, {u"Public", u"公开"}, {u"Marketplace", u"市场"},
        {u"Feed", u"动态"}, {u"Filter", u"筛选"},
    });
    return terms;
}

// Chinese numerals keep the protected-number check exact (no digit is added).
const GlossaryTerms& dateTerms()
{
    static const GlossaryTerms terms = []() {
        const QStringList months = {
            QStringLiteral("一月"), QStringLiteral("二月"), QStringLiteral("三月"),
            QStringLiteral("四月"), QStringLiteral("五月"), QStringLiteral("六月"),
            QStringLiteral("七月"), QStringLiteral("八月"), QStringLiteral("九月"),
            QStringLiteral("十月"), QStringLiteral("十一月"), QStringLiteral("十二月"),
        };
        const QStringList full = {
            QStringLiteral("January"), QStringLiteral("February"), QStringLiteral("March"),
            QStringLiteral("April"), QStringLiteral("May"), QStringLiteral("June"),
            QStringLiteral("July"), QStringLiteral("August"), QStringLiteral("September"),
            QStringLiteral("October"), QStringLiteral("November"), QStringLiteral("December"),
        };
        const QStringList shortNames = {
            QStringLiteral("Jan"), QStringLiteral("Feb"), QStringLiteral("Mar"),
            QStringLiteral("Apr"), QStringLiteral("May"), QStringLiteral("Jun"),
            QStringLiteral("Jul"), QStringLiteral("Aug"), QStringLiteral("Sep"),
            QStringLiteral("Oct"), QStringLiteral("Nov"), QStringLiteral("Dec"),
        };
        GlossaryTerms result;
        for (int i = 0; i < 12; ++i) {
            insertTerms(&result, {{full[i], months[i]}});
        }
        for (int i = 0; i < 12; ++i) {
            insertTerms(&result, {{shortNames[i], months[i]}});
        }
        insertTerms(&result, termList({
            {u"Mon", u"周一"}, {u"Tue", u"周二"}, {u"Wed", u"周三"}, {u"Thu", u"周四"},
            {u"Fri", u"周五"}, {u"Sat", u"周六"}, {u"Sun", u"周日"},
        }));
        return result;
    }();
    return terms;
}

bool containsWord(const QString& text, const QString& word, bool caseInsensitive)
{
    QRegularExpression pattern(QStringLiteral("(?<![A-Za-z])")
                                   + QRegularExpression::escape(word)
                                   + QStringLiteral("(?![A-Za-z])"),
                               caseInsensitive ? QRegularExpression::CaseInsensitiveOption
                                               : QRegularExpression::NoPatternOption);
    return pattern.match(text).hasMatch();
}

bool isRepositoryPath(const QString& text)
{
    static const QRegularExpression path(QStringLiteral("^[A-Za-z0-9_.-]+\\s*/\\s*[A-Za-z0-9_.-]+$"));
    return path.match(text.trimmed()).hasMatch();
}

bool isOpeningPunctuation(QChar ch)
{
    static const QString opening = QStringLiteral("（([{「『“‘《〈【〔");
    return opening.contains(ch);
}

bool isClosingPunctuation(QChar ch)
{
    static const QString closing = QStringLiteral("，。、；：！？）)]}」』”’》〉】〕…·,.;:!?%");
    return closing.contains(ch);
}

bool isWordCharacter(QChar ch)
{
    return !isWide(ch) && (ch.isLetterOrNumber() || ch == QLatin1Char('_')
                           || ch == QLatin1Char('\'') || ch == QLatin1Char('-'));
}

// Can a line break go between text[position - 1] and text[position]?
bool breakAllowed(const QString& text, int position)
{
    const QChar previous = text.at(position - 1);
    const QChar current = text.at(position);
    if (current.isSpace() || previous.isSpace()) {
        return false; // Spaces are handled as separators by the caller.
    }
    if (isClosingPunctuation(current) || isOpeningPunctuation(previous)) {
        return false;
    }
    if (isWordCharacter(previous) && isWordCharacter(current)) {
        return false; // Never split a Latin word, number or identifier.
    }
    return isWide(previous) || isWide(current);
}

QString joinParagraph(const QString& accumulated, const QString& next)
{
    const QString left = accumulated.trimmed();
    const QString right = next.trimmed();
    if (left.isEmpty()) {
        return right;
    }
    if (right.isEmpty()) {
        return left;
    }
    const bool cjkJoin = isWide(left.back()) && isWide(right.front());
    const bool hyphenated = left.size() >= 2 && left.back() == QLatin1Char('-')
        && left.at(left.size() - 2).isLetter() && right.front().isLower();
    return left + (cjkJoin || hyphenated ? QString() : QStringLiteral(" ")) + right;
}

bool continuesParagraph(const QString& paragraph,
                        const QRect& previous,
                        const Translate::TextBlock& next)
{
    const QString nextText = next.mergedText().trimmed();
    const bool prose = paragraph.split(QLatin1Char(' '), Qt::SkipEmptyParts).size() >= 5
        || hanCount(paragraph) >= 16;
    // A short final line ("toolbar button.") still continues the sentence: it
    // starts in lower case or ends it. A capitalised short label does not.
    const bool continuation = !nextText.isEmpty() && !metadataLine(nextText)
        && (latinWordCount(nextText) >= 3 || hanCount(nextText) >= 6
            || nextText.front().isLower() || endsSentence(nextText));
    if (!prose || !continuation || endsSentence(paragraph)) {
        return false;
    }
    // A wrapped line starts at the same margin and is not wider than the line
    // it continues; a much wider next line means the previous one ended early.
    const int height = previous.height();
    const QRect box = next.box;
    return qAbs(box.left() - previous.left()) <= height * 0.5
        && box.x() + box.width() <= previous.x() + previous.width() + height
        && qAbs(box.height() - height) <= height * 0.25;
}

} // namespace

int estimateTokens(const QString& text)
{
    int wide = 0;
    int other = 0;
    for (const QChar ch : text) {
        if (isWide(ch)) {
            ++wide;
        } else if (!ch.isSpace()) {
            ++other;
        }
    }
    return wide + (other + 3) / 4;
}

int chooseConcurrency(const QVector<int>& entryPromptTokens,
                      const QVector<int>& entryOutputTokens,
                      int sharedPrefixTokens,
                      double prefillTokensPerSecond,
                      double decodeTokensPerSecond,
                      int maxSlots)
{
    const int entries = static_cast<int>(entryPromptTokens.size());
    if (entries <= 1 || maxSlots <= 1 || prefillTokensPerSecond <= 0 || decodeTokensPerSecond <= 0) {
        return 1;
    }
    double ownPrompt = 0;
    for (const int tokens : entryPromptTokens) {
        ownPrompt += tokens;
    }
    double totalOutput = 0;
    double longestOutput = 0;
    for (const int tokens : entryOutputTokens) {
        totalOutput += tokens;
        longestOutput = qMax<double>(longestOutput, tokens);
    }
    int best = 1;
    double bestSeconds = std::numeric_limits<double>::max();
    for (const int candidate : {1, 2, 4}) {
        const int width = std::min({candidate, maxSlots, entries});
        if (width != candidate) {
            continue;
        }
        // Every slot in use prefills the shared prefix once; afterwards a
        // slot reuses it. Decoding is bounded by the longest region and by an
        // even split of all output; a wider batch makes each step slightly
        // slower.
        const double prefill = (width * sharedPrefixTokens + ownPrompt) / prefillTokensPerSecond;
        const double steps = qMax(longestOutput, totalOutput / width);
        const double decode = steps * (1.0 + 0.12 * (width - 1)) / decodeTokensPerSecond;
        if (prefill + decode < bestSeconds * 0.97) { // prefer fewer slots on a tie
            bestSeconds = prefill + decode;
            best = width;
        }
    }
    return best;
}

bool supportsTargetLanguage(const QString& target)
{
    return target == QStringLiteral("zh-Hans") || target == QStringLiteral("en");
}

QString promptLanguageName(const QString& target)
{
    static const QHash<QString, QString> names = {
        {QStringLiteral("zh-Hans"), QStringLiteral("中文")},
        {QStringLiteral("en"), QStringLiteral("英语")},
        {QStringLiteral("ja"), QStringLiteral("日语")},
        {QStringLiteral("ko"), QStringLiteral("韩语")},
        {QStringLiteral("fr"), QStringLiteral("法语")},
        {QStringLiteral("de"), QStringLiteral("德语")},
        {QStringLiteral("es"), QStringLiteral("西班牙语")},
        {QStringLiteral("pt"), QStringLiteral("葡萄牙语")},
        {QStringLiteral("it"), QStringLiteral("意大利语")},
        {QStringLiteral("ru"), QStringLiteral("俄语")},
    };
    return names.value(target);
}

bool isPreservedText(const QString& text)
{
    const QString trimmed = text.trimmed();
    static const QRegularExpression url(QStringLiteral("^(?:https?://|www\\.)\\S+$"));
    static const QRegularExpression numeric(QStringLiteral("^[0-9\\s.,+%kKmMbB:/()-]+$"));
    return metadataLine(trimmed) || languageNames().contains(trimmed)
        || url.match(trimmed).hasMatch() || isRepositoryPath(trimmed)
        || numeric.match(trimmed).hasMatch();
}

bool alreadyInTarget(const QString& text, const QString& target)
{
    if (target != QStringLiteral("zh-Hans")) {
        return false;
    }
    const int han = hanCount(text);
    return han >= 3 && han >= latinLetterCount(text);
}

bool looksLikeKeptName(const QString& text)
{
    static const QRegularExpression name(
        QStringLiteral("^[A-Z][A-Za-z0-9+#.]*(?: [A-Z][A-Za-z0-9+#.]*)?$"));
    return name.match(text.trimmed()).hasMatch();
}

QMap<QString, int> protectedTokenCounts(const QString& text)
{
    static const QRegularExpression token(QStringLiteral(
        "[0-9\\x{FF10}-\\x{FF19}]+(?:[.,][0-9\\x{FF10}-\\x{FF19}]+)*"
        "|\\{[A-Za-z_][A-Za-z_0-9]*\\}|%[sd]"));
    QMap<QString, int> counts;
    for (auto it = token.globalMatch(text); it.hasNext();) {
        ++counts[canonicalToken(it.next().captured(0))];
    }
    return counts;
}

bool protectedTokensMatch(const QString& source, const QString& translation)
{
    return protectedTokenCounts(source) == protectedTokenCounts(translation);
}

GlossaryTerms glossaryTerms(const QString& text, const QStringList& pageTexts, const QString& target)
{
    if (target != QStringLiteral("zh-Hans")) {
        return {};
    }
    QSet<QString> labels;
    int labelCount = 0;
    bool repositoryPage = false;
    for (const QString& page : pageTexts) {
        if (isShortLabel(page)) {
            labels.insert(page.trimmed().toCaseFolded());
            ++labelCount;
        }
        repositoryPage = repositoryPage || isRepositoryPath(page);
    }
    GlossaryTerms selected;
    if (repositoryPage && labelCount >= 4) {
        insertTerms(&selected, repositoryTerms());
    }
    insertTerms(&selected, softwareTerms());
    static const QStringList accountLabels = {
        QStringLiteral("profile"), QStringLiteral("repositories"), QStringLiteral("organizations"),
        QStringLiteral("copilot settings"), QStringLiteral("feature preview"), QStringLiteral("sign out"),
    };
    const auto accountHits = std::count_if(accountLabels.cbegin(), accountLabels.cend(),
                                           [&labels](const QString& label) { return labels.contains(label); });
    if (accountHits >= 4) {
        insertTerms(&selected, accountTerms());
    }
    GlossaryTerms found;
    for (const auto& term : std::as_const(selected)) {
        if (containsWord(text, term.first, true)) {
            found.append(term);
        }
    }
    // Single-word disambiguation only for a region that IS that word; inside a
    // phrase ("Public profile") it would override the phrase's own meaning.
    for (const auto& term : interfaceTerms()) {
        if (text.trimmed().compare(term.first, Qt::CaseInsensitive) == 0) {
            found.append(term);
        }
    }
    // Case-sensitive: "May" is a month, "may" is a verb.
    for (const auto& term : dateTerms()) {
        if (containsWord(text, term.first, false)) {
            found.append(term);
        }
    }
    return found;
}

QString buildPageContext(const QStringList& pageTexts)
{
    QStringList lines;
    int budget = 0;
    for (const QString& text : pageTexts) {
        const QString line = text.trimmed().left(kContextLineChars);
        if (line.isEmpty()) {
            continue;
        }
        const int cost = estimateTokens(line) + 1;
        if (budget + cost > kContextTokenBudget) {
            break;
        }
        lines.append(line);
        budget += cost;
    }
    return QStringLiteral("以下是软件界面截图中的文字：\n") + lines.join(QLatin1Char('\n'))
        + QLatin1Char('\n');
}

QString buildPrompt(const QString& pageContext,
                    const GlossaryTerms& terms,
                    const QString& text,
                    const QString& target)
{
    QString glossary;
    if (!terms.isEmpty()) {
        glossary = QStringLiteral("参考下面的翻译：\n");
        for (const auto& term : terms) {
            glossary += term.first + QStringLiteral(" 翻译成 ") + term.second + QLatin1Char('\n');
        }
    }
    return pageContext + glossary + QStringLiteral("参考上面的信息，把下面的文本翻译成")
        + promptLanguageName(target)
        + QStringLiteral("，注意不需要翻译上文，也不要额外解释：\n") + text;
}

QString buildRetryPrompt(const QString& text, const QString& target)
{
    return QStringLiteral("把下面的文本翻译成") + promptLanguageName(target)
        + QStringLiteral("，不要额外解释。保留原文中的数字、占位符、网址和代码名称。\n\n") + text;
}

int maxOutputTokens(const QString& text)
{
    return qMin(1400, 2 * static_cast<int>(text.size()) + 48);
}

QString cleanOutput(const QString& value, const QString& source, const QString& target)
{
    QString result = value.trimmed();
    const QString original = source.trimmed();
    static const QVector<QPair<QString, QString>> wrappers = {
        {QStringLiteral("\""), QStringLiteral("\"")},
        {QStringLiteral("“"), QStringLiteral("”")},
        {QStringLiteral("'"), QStringLiteral("'")},
        {QStringLiteral("「"), QStringLiteral("」")},
    };
    for (const auto& [opening, closing] : wrappers) {
        if (result.size() >= 2 && result.startsWith(opening) && result.endsWith(closing)
            && !original.startsWith(opening)) {
            result = result.mid(opening.size(), result.size() - opening.size() - closing.size()).trimmed();
        }
    }
    if (!original.contains(QLatin1Char('\n'))) {
        QStringList lines;
        for (const QString& line : result.split(QLatin1Char('\n'))) {
            if (!line.trimmed().isEmpty()) {
                lines.append(line.trimmed());
            }
        }
        result = lines.join(target == QStringLiteral("zh-Hans") ? QString() : QStringLiteral(" "));
    }
    return result;
}

QString rejectionReason(const QString& source, const QString& translation, const QString& target)
{
    const QString value = translation.trimmed();
    const QString original = source.trimmed();
    if (value.isEmpty() || value.size() > 8000 || translation.size() > 4 * source.size() + 40) {
        return QStringLiteral("invalid_translation");
    }
    if (!protectedTokensMatch(original, value)) {
        return QStringLiteral("protected_token_mismatch");
    }
    if (isPreservedText(original)) {
        return {};
    }
    if (target == QStringLiteral("zh-Hans")) {
        const bool needsChinese = latinWordCount(original) >= 3
            || (isShortLabel(original) && latinLetterCount(original) > 0);
        if (needsChinese && hanCount(value) == 0) {
            return QStringLiteral("sentence_not_translated");
        }
    } else if (target == QStringLiteral("en")) {
        if (hanCount(original) > 0 && original == value) {
            return QStringLiteral("sentence_not_translated");
        }
    }
    return {};
}

QByteArray buildChatRequest(const QString& prompt, int maxTokens)
{
    const QJsonObject message{{QStringLiteral("role"), QStringLiteral("user")},
                              {QStringLiteral("content"), prompt}};
    const QJsonObject request{
        {QStringLiteral("messages"), QJsonArray{message}},
        {QStringLiteral("temperature"), 0},
        {QStringLiteral("max_tokens"), maxTokens},
        {QStringLiteral("stream"), false},
        // The page-context prefix stays in the slot's KV cache between units.
        {QStringLiteral("cache_prompt"), true},
    };
    return QJsonDocument(request).toJson(QJsonDocument::Compact);
}

bool parseChatResponse(const QByteArray& body, ChatResult* result, QString* error)
{
    const auto fail = [error](const QString& code) {
        if (error) {
            *error = code;
        }
        return false;
    };
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(QStringLiteral("translation_response_error"));
    }
    const QJsonObject root = document.object();
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty() || !choices.first().isObject()) {
        return fail(QStringLiteral("translation_response_error"));
    }
    const QJsonObject choice = choices.first().toObject();
    const QJsonValue content = choice.value(QStringLiteral("message")).toObject().value(QStringLiteral("content"));
    if (!content.isString()) {
        return fail(QStringLiteral("invalid_translation"));
    }
    ChatResult parsed;
    parsed.content = content.toString();
    parsed.finishReason = choice.value(QStringLiteral("finish_reason")).toString();
    const QJsonObject usage = root.value(QStringLiteral("usage")).toObject();
    parsed.promptTokens = usage.value(QStringLiteral("prompt_tokens")).toInt();
    parsed.completionTokens = usage.value(QStringLiteral("completion_tokens")).toInt();
    const QJsonObject timings = root.value(QStringLiteral("timings")).toObject();
    parsed.promptProcessed = timings.value(QStringLiteral("prompt_n")).toInt(parsed.promptTokens);
    parsed.promptMs = timings.value(QStringLiteral("prompt_ms")).toDouble();
    parsed.generationMs = timings.value(QStringLiteral("predicted_ms")).toDouble();
    if (result) {
        *result = parsed;
    }
    return true;
}

ParagraphPlan planParagraphs(const QVector<Translate::TextBlock>& units,
                             const QVector<Translate::TextBlock>& contextBlocks)
{
    QHash<int, int> unitBySourceLine;
    for (int i = 0; i < units.size(); ++i) {
        if (units[i].lines.size() == 1 && units[i].sourceLineIndices.size() == 1) {
            unitBySourceLine.insert(units[i].sourceLineIndices.first(), i);
        }
    }
    QVector<QVector<int>> groups;
    QVector<bool> grouped(units.size(), false);
    for (const Translate::TextBlock& block : contextBlocks) {
        QVector<int> run;
        QString paragraph;
        const auto flush = [&]() {
            if (run.size() >= 2) {
                groups.append(run);
                for (const int member : std::as_const(run)) {
                    grouped[member] = true;
                }
            }
            run.clear();
            paragraph.clear();
        };
        for (const int sourceLine : block.sourceLineIndices) {
            const int unit = unitBySourceLine.value(sourceLine, -1);
            if (unit < 0 || grouped[unit]) {
                flush(); // A preserved or missing line interrupts the paragraph.
                continue;
            }
            if (!run.isEmpty()
                && !continuesParagraph(paragraph, units[run.last()].box, units[unit])) {
                flush();
            }
            run.append(unit);
            paragraph = joinParagraph(paragraph, units[unit].mergedText());
        }
        flush();
    }
    for (int i = 0; i < units.size(); ++i) {
        if (!grouped[i]) {
            groups.append(QVector<int>{i});
        }
    }
    std::sort(groups.begin(), groups.end(),
              [](const QVector<int>& a, const QVector<int>& b) { return a.first() < b.first(); });
    ParagraphPlan plan;
    for (const QVector<int>& group : std::as_const(groups)) {
        QString text;
        for (const int member : group) {
            text = joinParagraph(text, units[member].mergedText());
        }
        plan.texts.append(text);
        plan.members.append(group);
    }
    return plan;
}

QStringList splitAcrossLines(const QString& translation,
                             const QVector<int>& lineWidths,
                             const QString& target)
{
    Q_UNUSED(target);
    const QString text = translation.simplified();
    const int lines = lineWidths.size();
    if (lines <= 0 || text.isEmpty()) {
        return {};
    }
    if (lines == 1) {
        return {text};
    }
    // Candidate cut positions and the display width before each one.
    QVector<int> cumulative(text.size() + 1, 0);
    for (int i = 0; i < text.size(); ++i) {
        cumulative[i + 1] = cumulative[i] + (isWide(text.at(i)) ? 2 : 1);
    }
    QVector<int> candidates;
    for (int i = 1; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char(' ') || breakAllowed(text, i)) {
            candidates.append(i);
        }
    }
    if (candidates.size() < lines - 1) {
        return {};
    }
    double totalWidth = 0;
    for (const int width : lineWidths) {
        totalWidth += qMax(1, width);
    }
    const double totalText = cumulative.last();
    QVector<int> cuts;
    int previousCandidate = -1;
    double consumed = 0;
    for (int line = 0; line < lines - 1; ++line) {
        consumed += qMax(1, lineWidths[line]);
        const double goal = totalText * consumed / totalWidth;
        // Leave at least one candidate for every remaining boundary.
        const int last = static_cast<int>(candidates.size()) - (lines - 1 - line);
        int best = -1;
        double bestDistance = std::numeric_limits<double>::max();
        for (int c = previousCandidate + 1; c <= last; ++c) {
            const double distance = std::abs(cumulative[candidates[c]] - goal);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = c;
            }
        }
        if (best < 0) {
            return {};
        }
        cuts.append(candidates[best]);
        previousCandidate = best;
    }
    QStringList pieces;
    int start = 0;
    cuts.append(static_cast<int>(text.size()));
    for (const int cut : std::as_const(cuts)) {
        const QString piece = text.mid(start, cut - start).trimmed();
        if (piece.isEmpty()) {
            return {};
        }
        pieces.append(piece);
        start = cut;
    }
    return pieces;
}

QStringList distributeTranslations(const ParagraphPlan& plan,
                                   const QVector<Translate::TextBlock>& units,
                                   const QStringList& translations,
                                   const QString& target,
                                   QVector<int>* failedEntries)
{
    if (translations.size() != plan.texts.size() || plan.members.size() != plan.texts.size()) {
        return {};
    }
    QStringList result;
    result.reserve(units.size());
    for (const Translate::TextBlock& unit : units) {
        result.append(unit.mergedText());
    }
    for (int entry = 0; entry < plan.texts.size(); ++entry) {
        const QVector<int>& members = plan.members[entry];
        if (members.isEmpty() || std::any_of(members.cbegin(), members.cend(),
                [&units](int member) { return member < 0 || member >= units.size(); })) {
            return {};
        }
        if (members.size() == 1) {
            result[members.first()] = translations[entry];
            continue;
        }
        if (translations[entry].trimmed() == plan.texts[entry].trimmed()) {
            continue; // Unchanged paragraph: every source line stays as is.
        }
        QVector<int> widths;
        for (const int member : members) {
            widths.append(units[member].box.width());
        }
        const QStringList pieces = splitAcrossLines(translations[entry], widths, target);
        if (pieces.size() != members.size()) {
            if (failedEntries) {
                failedEntries->append(entry);
            }
            continue;
        }
        for (int i = 0; i < members.size(); ++i) {
            result[members[i]] = pieces[i];
        }
    }
    return result;
}

} // namespace Visnip::LocalMt
