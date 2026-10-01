#include "core/QuestionAnswer.h"

#include <QBuffer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace Visnip::Question {

namespace {

constexpr int kMaxImageEdge = 2000;
constexpr int kUpscaleBelowEdge = 400;
constexpr qsizetype kMaxPngBytes = 3500 * 1024;
// Keeps enough room when the raw body is needed for a non-streaming answer.
constexpr qsizetype kMaxRawResponseBytes = 4 * 1024 * 1024;
constexpr int kAnthropicMaxTokens = 16000;
constexpr int kMaxErrorDetailLength = 300;
constexpr auto kAnthropicVersion = "2023-06-01";
constexpr auto kServerSideFallbackBeta = "server-side-fallback-2026-07-01";

QString jsonErrorMessage(const QJsonObject& object)
{
    const QJsonValue error = object.value(QStringLiteral("error"));
    if (error.isObject()) {
        const QJsonObject details = error.toObject();
        for (const QString& key : {QStringLiteral("message"), QStringLiteral("msg")}) {
            const QString message = details.value(key).toString().trimmed();
            if (!message.isEmpty()) {
                return message;
            }
        }
        const QString type = details.value(QStringLiteral("type")).toString().trimmed();
        if (!type.isEmpty()) {
            return type;
        }
    } else if (error.isString() && !error.toString().trimmed().isEmpty()) {
        return error.toString().trimmed();
    }
    // Some gateways wrap failures as {"code": 400, "message": "..."}; a zero
    // code, or a body that carries an answer, is not an error.
    const QJsonValue code = object.value(QStringLiteral("code"));
    const bool failureCode = (code.isDouble() && code.toDouble() != 0)
        || (code.isString() && !code.toString().isEmpty()
            && code.toString() != QStringLiteral("0")
            && code.toString().compare(QStringLiteral("success"), Qt::CaseInsensitive) != 0);
    const bool carriesAnswer = object.contains(QStringLiteral("choices"))
        || object.contains(QStringLiteral("content"));
    if (object.value(QStringLiteral("type")).toString() == QStringLiteral("error")
        || (failureCode && !carriesAnswer)) {
        for (const QString& key : {QStringLiteral("message"), QStringLiteral("msg")}) {
            const QString message = object.value(key).toString().trimmed();
            if (!message.isEmpty()) {
                return message;
            }
        }
    }
    return {};
}

QString joinedContentText(const QJsonValue& content)
{
    if (content.isString()) {
        return content.toString();
    }
    QString text;
    for (const QJsonValue& part : content.toArray()) {
        const QJsonObject object = part.toObject();
        const QString type = object.value(QStringLiteral("type")).toString();
        if (type.isEmpty() || type == QStringLiteral("text")
            || type == QStringLiteral("output_text")) {
            text += object.value(QStringLiteral("text")).toString();
        }
    }
    return text;
}

bool mentionsVisionSupport(const QString& detail)
{
    const QString lower = detail.toLower();
    return lower.contains(QStringLiteral("image"))
        || lower.contains(QStringLiteral("vision"))
        || lower.contains(QStringLiteral("multimodal"))
        || lower.contains(QStringLiteral("multi-modal"))
        || detail.contains(QStringLiteral("图片"))
        || detail.contains(QStringLiteral("图像"));
}

// ---- LaTeX to Unicode -------------------------------------------------------

struct LatexSymbol {
    const char16_t* text;
    bool spaced; // binary operators and relations get spaces around them
};

const QHash<QString, LatexSymbol>& latexSymbols()
{
    static const QHash<QString, LatexSymbol> symbols = {
        {QStringLiteral("alpha"), {u"α", false}},
        {QStringLiteral("beta"), {u"β", false}},
        {QStringLiteral("gamma"), {u"γ", false}},
        {QStringLiteral("delta"), {u"δ", false}},
        {QStringLiteral("epsilon"), {u"ε", false}},
        {QStringLiteral("varepsilon"), {u"ε", false}},
        {QStringLiteral("zeta"), {u"ζ", false}},
        {QStringLiteral("eta"), {u"η", false}},
        {QStringLiteral("theta"), {u"θ", false}},
        {QStringLiteral("vartheta"), {u"ϑ", false}},
        {QStringLiteral("iota"), {u"ι", false}},
        {QStringLiteral("kappa"), {u"κ", false}},
        {QStringLiteral("lambda"), {u"λ", false}},
        {QStringLiteral("mu"), {u"μ", false}},
        {QStringLiteral("nu"), {u"ν", false}},
        {QStringLiteral("xi"), {u"ξ", false}},
        {QStringLiteral("pi"), {u"π", false}},
        {QStringLiteral("varpi"), {u"ϖ", false}},
        {QStringLiteral("rho"), {u"ρ", false}},
        {QStringLiteral("varrho"), {u"ϱ", false}},
        {QStringLiteral("sigma"), {u"σ", false}},
        {QStringLiteral("varsigma"), {u"ς", false}},
        {QStringLiteral("tau"), {u"τ", false}},
        {QStringLiteral("upsilon"), {u"υ", false}},
        {QStringLiteral("phi"), {u"φ", false}},
        {QStringLiteral("varphi"), {u"φ", false}},
        {QStringLiteral("chi"), {u"χ", false}},
        {QStringLiteral("psi"), {u"ψ", false}},
        {QStringLiteral("omega"), {u"ω", false}},
        {QStringLiteral("Gamma"), {u"Γ", false}},
        {QStringLiteral("Delta"), {u"Δ", false}},
        {QStringLiteral("Theta"), {u"Θ", false}},
        {QStringLiteral("Lambda"), {u"Λ", false}},
        {QStringLiteral("Xi"), {u"Ξ", false}},
        {QStringLiteral("Pi"), {u"Π", false}},
        {QStringLiteral("Sigma"), {u"Σ", false}},
        {QStringLiteral("Upsilon"), {u"Υ", false}},
        {QStringLiteral("Phi"), {u"Φ", false}},
        {QStringLiteral("Psi"), {u"Ψ", false}},
        {QStringLiteral("Omega"), {u"Ω", false}},
        {QStringLiteral("times"), {u"×", true}},
        {QStringLiteral("cdot"), {u"·", true}},
        {QStringLiteral("div"), {u"÷", true}},
        {QStringLiteral("pm"), {u"±", true}},
        {QStringLiteral("mp"), {u"∓", true}},
        {QStringLiteral("ast"), {u"∗", true}},
        {QStringLiteral("star"), {u"⋆", true}},
        {QStringLiteral("bullet"), {u"•", true}},
        {QStringLiteral("le"), {u"≤", true}},
        {QStringLiteral("leq"), {u"≤", true}},
        {QStringLiteral("leqslant"), {u"≤", true}},
        {QStringLiteral("ge"), {u"≥", true}},
        {QStringLiteral("geq"), {u"≥", true}},
        {QStringLiteral("geqslant"), {u"≥", true}},
        {QStringLiteral("lt"), {u"<", true}},
        {QStringLiteral("gt"), {u">", true}},
        {QStringLiteral("ne"), {u"≠", true}},
        {QStringLiteral("neq"), {u"≠", true}},
        {QStringLiteral("approx"), {u"≈", true}},
        {QStringLiteral("equiv"), {u"≡", true}},
        {QStringLiteral("sim"), {u"∼", true}},
        {QStringLiteral("simeq"), {u"≃", true}},
        {QStringLiteral("cong"), {u"≅", true}},
        {QStringLiteral("propto"), {u"∝", true}},
        {QStringLiteral("ll"), {u"≪", true}},
        {QStringLiteral("gg"), {u"≫", true}},
        {QStringLiteral("to"), {u"→", true}},
        {QStringLiteral("rightarrow"), {u"→", true}},
        {QStringLiteral("longrightarrow"), {u"→", true}},
        {QStringLiteral("leftarrow"), {u"←", true}},
        {QStringLiteral("gets"), {u"←", true}},
        {QStringLiteral("leftrightarrow"), {u"↔", true}},
        {QStringLiteral("Rightarrow"), {u"⇒", true}},
        {QStringLiteral("Longrightarrow"), {u"⇒", true}},
        {QStringLiteral("implies"), {u"⇒", true}},
        {QStringLiteral("Leftarrow"), {u"⇐", true}},
        {QStringLiteral("Leftrightarrow"), {u"⇔", true}},
        {QStringLiteral("iff"), {u"⇔", true}},
        {QStringLiteral("mapsto"), {u"↦", true}},
        {QStringLiteral("rightleftharpoons"), {u"⇌", true}},
        {QStringLiteral("in"), {u"∈", true}},
        {QStringLiteral("notin"), {u"∉", true}},
        {QStringLiteral("ni"), {u"∋", true}},
        {QStringLiteral("subset"), {u"⊂", true}},
        {QStringLiteral("subseteq"), {u"⊆", true}},
        {QStringLiteral("supset"), {u"⊃", true}},
        {QStringLiteral("supseteq"), {u"⊇", true}},
        {QStringLiteral("cup"), {u"∪", true}},
        {QStringLiteral("cap"), {u"∩", true}},
        {QStringLiteral("setminus"), {u"∖", true}},
        {QStringLiteral("land"), {u"∧", true}},
        {QStringLiteral("wedge"), {u"∧", true}},
        {QStringLiteral("lor"), {u"∨", true}},
        {QStringLiteral("vee"), {u"∨", true}},
        {QStringLiteral("oplus"), {u"⊕", true}},
        {QStringLiteral("otimes"), {u"⊗", true}},
        {QStringLiteral("perp"), {u"⊥", true}},
        {QStringLiteral("parallel"), {u"∥", true}},
        {QStringLiteral("mid"), {u"|", true}},
        {QStringLiteral("infty"), {u"∞", false}},
        {QStringLiteral("partial"), {u"∂", false}},
        {QStringLiteral("nabla"), {u"∇", false}},
        {QStringLiteral("sum"), {u"∑", false}},
        {QStringLiteral("prod"), {u"∏", false}},
        {QStringLiteral("int"), {u"∫", false}},
        {QStringLiteral("iint"), {u"∬", false}},
        {QStringLiteral("iiint"), {u"∭", false}},
        {QStringLiteral("oint"), {u"∮", false}},
        {QStringLiteral("forall"), {u"∀", false}},
        {QStringLiteral("exists"), {u"∃", false}},
        {QStringLiteral("neg"), {u"¬", false}},
        {QStringLiteral("lnot"), {u"¬", false}},
        {QStringLiteral("emptyset"), {u"∅", false}},
        {QStringLiteral("varnothing"), {u"∅", false}},
        {QStringLiteral("angle"), {u"∠", false}},
        {QStringLiteral("triangle"), {u"△", false}},
        {QStringLiteral("square"), {u"□", false}},
        {QStringLiteral("circ"), {u"∘", false}},
        {QStringLiteral("degree"), {u"°", false}},
        {QStringLiteral("cdots"), {u"⋯", false}},
        {QStringLiteral("ldots"), {u"…", false}},
        {QStringLiteral("dots"), {u"…", false}},
        {QStringLiteral("dotsc"), {u"…", false}},
        {QStringLiteral("vdots"), {u"⋮", false}},
        {QStringLiteral("ddots"), {u"⋱", false}},
        {QStringLiteral("prime"), {u"′", false}},
        {QStringLiteral("therefore"), {u"∴", false}},
        {QStringLiteral("because"), {u"∵", false}},
        {QStringLiteral("langle"), {u"⟨", false}},
        {QStringLiteral("rangle"), {u"⟩", false}},
        {QStringLiteral("lfloor"), {u"⌊", false}},
        {QStringLiteral("rfloor"), {u"⌋", false}},
        {QStringLiteral("lceil"), {u"⌈", false}},
        {QStringLiteral("rceil"), {u"⌉", false}},
        {QStringLiteral("vert"), {u"|", false}},
        {QStringLiteral("Vert"), {u"‖", false}},
        {QStringLiteral("lbrace"), {u"{", false}},
        {QStringLiteral("rbrace"), {u"}", false}},
        {QStringLiteral("hbar"), {u"ℏ", false}},
        {QStringLiteral("ell"), {u"ℓ", false}},
        {QStringLiteral("aleph"), {u"ℵ", false}},
        {QStringLiteral("Re"), {u"ℜ", false}},
        {QStringLiteral("Im"), {u"ℑ", false}},
        {QStringLiteral("uparrow"), {u"↑", false}},
        {QStringLiteral("downarrow"), {u"↓", false}},
        {QStringLiteral("quad"), {u"  ", false}},
        {QStringLiteral("qquad"), {u"    ", false}},
    };
    return symbols;
}

// Function names keep their plain spelling and the space that follows them.
bool isLatexFunction(const QString& name)
{
    static const QSet<QString> functions = {
        QStringLiteral("sin"), QStringLiteral("cos"), QStringLiteral("tan"),
        QStringLiteral("cot"), QStringLiteral("sec"), QStringLiteral("csc"),
        QStringLiteral("arcsin"), QStringLiteral("arccos"), QStringLiteral("arctan"),
        QStringLiteral("sinh"), QStringLiteral("cosh"), QStringLiteral("tanh"),
        QStringLiteral("log"), QStringLiteral("ln"), QStringLiteral("lg"),
        QStringLiteral("exp"), QStringLiteral("lim"), QStringLiteral("max"),
        QStringLiteral("min"), QStringLiteral("sup"), QStringLiteral("inf"),
        QStringLiteral("det"), QStringLiteral("gcd"), QStringLiteral("deg"),
        QStringLiteral("arg"), QStringLiteral("dim"), QStringLiteral("ker"),
        QStringLiteral("Pr"), QStringLiteral("mod"), QStringLiteral("bmod"),
    };
    return functions.contains(name);
}

bool isTextCommand(const QString& name)
{
    static const QSet<QString> commands = {
        QStringLiteral("text"), QStringLiteral("textrm"), QStringLiteral("textbf"),
        QStringLiteral("textit"), QStringLiteral("textnormal"), QStringLiteral("mathrm"),
        QStringLiteral("mathbf"), QStringLiteral("mathit"), QStringLiteral("mathsf"),
        QStringLiteral("mathtt"), QStringLiteral("mathcal"), QStringLiteral("mathscr"),
        QStringLiteral("mathfrak"), QStringLiteral("boldsymbol"), QStringLiteral("bm"),
        QStringLiteral("operatorname"), QStringLiteral("mbox"), QStringLiteral("hbox"),
    };
    return commands.contains(name);
}

bool isIgnoredCommand(const QString& name)
{
    static const QSet<QString> commands = {
        QStringLiteral("displaystyle"), QStringLiteral("textstyle"),
        QStringLiteral("scriptstyle"), QStringLiteral("limits"),
        QStringLiteral("nolimits"), QStringLiteral("nonumber"),
        QStringLiteral("notag"), QStringLiteral("big"), QStringLiteral("Big"),
        QStringLiteral("bigg"), QStringLiteral("Bigg"), QStringLiteral("bigl"),
        QStringLiteral("bigr"), QStringLiteral("Bigl"), QStringLiteral("Bigr"),
        QStringLiteral("biggl"), QStringLiteral("biggr"), QStringLiteral("Biggl"),
        QStringLiteral("Biggr"),
    };
    return commands.contains(name);
}

QChar superscriptChar(QChar c)
{
    static const QHash<QChar, QChar> map = {
        {u'0', u'⁰'}, {u'1', u'¹'}, {u'2', u'²'}, {u'3', u'³'}, {u'4', u'⁴'},
        {u'5', u'⁵'}, {u'6', u'⁶'}, {u'7', u'⁷'}, {u'8', u'⁸'}, {u'9', u'⁹'},
        {u'+', u'⁺'}, {u'-', u'⁻'}, {u'−', u'⁻'}, {u'=', u'⁼'}, {u'(', u'⁽'},
        {u')', u'⁾'}, {u'n', u'ⁿ'}, {u'i', u'ⁱ'}, {u'x', u'ˣ'}, {u'y', u'ʸ'},
        {u'k', u'ᵏ'}, {u'm', u'ᵐ'}, {u'T', u'ᵀ'},
    };
    return map.value(c);
}

QChar subscriptChar(QChar c)
{
    static const QHash<QChar, QChar> map = {
        {u'0', u'₀'}, {u'1', u'₁'}, {u'2', u'₂'}, {u'3', u'₃'}, {u'4', u'₄'},
        {u'5', u'₅'}, {u'6', u'₆'}, {u'7', u'₇'}, {u'8', u'₈'}, {u'9', u'₉'},
        {u'+', u'₊'}, {u'-', u'₋'}, {u'−', u'₋'}, {u'=', u'₌'}, {u'(', u'₍'},
        {u')', u'₎'}, {u'a', u'ₐ'}, {u'e', u'ₑ'}, {u'o', u'ₒ'}, {u'x', u'ₓ'},
        {u'h', u'ₕ'}, {u'k', u'ₖ'}, {u'l', u'ₗ'}, {u'm', u'ₘ'}, {u'n', u'ₙ'},
        {u'p', u'ₚ'}, {u's', u'ₛ'}, {u't', u'ₜ'}, {u'i', u'ᵢ'}, {u'j', u'ⱼ'},
        {u'r', u'ᵣ'}, {u'u', u'ᵤ'}, {u'v', u'ᵥ'},
    };
    return map.value(c);
}

QChar doubleStruckChar(QChar c)
{
    static const QHash<QChar, QChar> map = {
        {u'R', u'ℝ'}, {u'N', u'ℕ'}, {u'Z', u'ℤ'}, {u'Q', u'ℚ'}, {u'C', u'ℂ'},
        {u'P', u'ℙ'}, {u'H', u'ℍ'},
    };
    return map.value(c, c);
}

bool isScriptOrCombining(QChar c)
{
    static const QString scripts = QStringLiteral(
        "⁰¹²³⁴⁵⁶⁷⁸⁹⁺⁻⁼⁽⁾ⁿⁱˣʸᵏᵐᵀ₀₁₂₃₄₅₆₇₈₉₊₋₌₍₎ₐₑₒₓₕₖₗₘₙₚₛₜᵢⱼᵣᵤᵥ′°");
    return c.category() == QChar::Mark_NonSpacing || scripts.contains(c);
}

// A term that reads unambiguously without parentheses: a number, or a single
// symbol possibly carrying scripts/accents (x, x², π, 12.5).
bool isAtomicTerm(const QString& term)
{
    if (term.isEmpty()) {
        return true;
    }
    bool allDigits = true;
    int baseCount = 0;
    for (const QChar c : term) {
        if (!c.isDigit() && c != u'.') {
            allDigits = false;
        }
        if (!isScriptOrCombining(c)) {
            ++baseCount;
        }
    }
    return allDigits || baseCount <= 1;
}

QString withCombining(const QString& text, QChar mark)
{
    QString result;
    for (const QChar c : text) {
        result += c;
        if (!c.isSpace()) {
            result += mark;
        }
    }
    return result;
}

class LatexConverter {
public:
    explicit LatexConverter(const QString& source)
        : s_(source)
    {
    }

    QString run()
    {
        QString result = parseSequence(QChar());
        static const QRegularExpression spaces(QStringLiteral("[ \\t]{2,}"));
        result.replace(spaces, QStringLiteral(" "));
        return result.trimmed();
    }

private:
    bool atEnd() const { return pos_ >= s_.size(); }

    void skipSpaces()
    {
        while (!atEnd() && s_.at(pos_).isSpace() && s_.at(pos_) != u'\n') {
            ++pos_;
        }
    }

    // Parses up to (and consumes) `terminator`, or to the end of the input.
    QString parseSequence(QChar terminator)
    {
        QString out;
        while (!atEnd()) {
            const QChar c = s_.at(pos_);
            if (!terminator.isNull() && c == terminator) {
                ++pos_;
                break;
            }
            ++pos_;
            if (c == u'{') {
                out += parseSequence(u'}');
            } else if (c == u'}') {
                // Unbalanced closing brace: drop it.
            } else if (c == u'\\') {
                out += parseCommand();
            } else if (c == u'^') {
                out += superscript(parseArgument());
            } else if (c == u'_') {
                out += subscript(parseArgument());
            } else if (c == u'&' || c == u'~') {
                out += u' ';
            } else if (c == u'\'') {
                out += u'′';
            } else if (c == u'*') {
                out += QStringLiteral("\\*");
            } else {
                out += c;
            }
        }
        return out;
    }

    QString parseArgument()
    {
        skipSpaces();
        if (atEnd()) {
            return {};
        }
        const QChar c = s_.at(pos_++);
        if (c == u'{') {
            return parseSequence(u'}');
        }
        if (c == u'\\') {
            return parseCommand();
        }
        return QString(c);
    }

    QString readCommandName()
    {
        const qsizetype start = pos_;
        while (!atEnd() && s_.at(pos_).isLetter()) {
            ++pos_;
        }
        return s_.mid(start, pos_ - start);
    }

    QString parseCommand()
    {
        if (atEnd()) {
            return {};
        }
        const QChar first = s_.at(pos_);
        if (!first.isLetter()) {
            ++pos_;
            switch (first.unicode()) {
            case u',':
            case u':':
            case u';':
            case u'>':
            case u' ':
                return QStringLiteral(" ");
            case u'!':
                return {};
            case u'\\':
                return QStringLiteral("\n");
            case u'_':
                return QStringLiteral("\\_");
            case u'|':
                return QStringLiteral("‖");
            default:
                return QString(first);
            }
        }

        const QString name = readCommandName();
        if (name == QStringLiteral("frac") || name == QStringLiteral("dfrac")
            || name == QStringLiteral("tfrac") || name == QStringLiteral("cfrac")) {
            const QString numerator = parseArgument().trimmed();
            const QString denominator = parseArgument().trimmed();
            return parenthesized(numerator) + QStringLiteral("/") + parenthesized(denominator);
        }
        if (name == QStringLiteral("sqrt")) {
            skipSpaces();
            QString index;
            if (!atEnd() && s_.at(pos_) == u'[') {
                ++pos_;
                index = parseSequence(u']').trimmed();
            }
            const QString radicand = parseArgument().trimmed();
            QString sign = QStringLiteral("√");
            if (index == QStringLiteral("3")) {
                sign = QStringLiteral("∛");
            } else if (index == QStringLiteral("4")) {
                sign = QStringLiteral("∜");
            } else if (!index.isEmpty()) {
                sign = superscript(index) + sign;
            }
            return sign + parenthesized(radicand);
        }
        if (name == QStringLiteral("mathbb")) {
            QString text = parseArgument();
            for (QChar& c : text) {
                c = doubleStruckChar(c);
            }
            return text;
        }
        if (isTextCommand(name)) {
            return parseArgument();
        }
        if (name == QStringLiteral("vec") || name == QStringLiteral("overrightarrow")) {
            return withCombining(parseArgument(), QChar(0x20D7));
        }
        if (name == QStringLiteral("hat") || name == QStringLiteral("widehat")) {
            return withCombining(parseArgument(), QChar(0x0302));
        }
        if (name == QStringLiteral("bar") || name == QStringLiteral("overline")) {
            return withCombining(parseArgument(), QChar(0x0305));
        }
        if (name == QStringLiteral("tilde") || name == QStringLiteral("widetilde")) {
            return withCombining(parseArgument(), QChar(0x0303));
        }
        if (name == QStringLiteral("dot")) {
            return withCombining(parseArgument(), QChar(0x0307));
        }
        if (name == QStringLiteral("ddot")) {
            return withCombining(parseArgument(), QChar(0x0308));
        }
        if (name == QStringLiteral("underline")) {
            return withCombining(parseArgument(), QChar(0x0332));
        }
        if (name == QStringLiteral("boxed")) {
            return QStringLiteral("**") + parseArgument().trimmed() + QStringLiteral("**");
        }
        if (name == QStringLiteral("pmod")) {
            return QStringLiteral(" (mod ") + parseArgument().trimmed() + QStringLiteral(")");
        }
        if (name == QStringLiteral("left") || name == QStringLiteral("right")) {
            skipSpaces();
            if (!atEnd() && s_.at(pos_) == u'.') {
                ++pos_;
            }
            return {};
        }
        if (name == QStringLiteral("begin") || name == QStringLiteral("end")) {
            const QString environment = parseArgument().trimmed();
            if (name == QStringLiteral("begin")
                && (environment == QStringLiteral("array")
                    || environment == QStringLiteral("tabular"))) {
                parseArgument(); // column specification
            }
            return name == QStringLiteral("begin") ? QString() : QStringLiteral("\n");
        }
        if (name == QStringLiteral("not")) {
            skipSpaces();
            if (!atEnd() && s_.at(pos_) == u'=') {
                ++pos_;
                return QStringLiteral(" ≠ ");
            }
            return QStringLiteral("¬");
        }
        if (isIgnoredCommand(name)) {
            return {};
        }
        if (isLatexFunction(name)) {
            return name == QStringLiteral("bmod") || name == QStringLiteral("mod")
                ? QStringLiteral(" mod ")
                : name;
        }
        const auto symbol = latexSymbols().constFind(name);
        if (symbol != latexSymbols().cend()) {
            const QString text = QString::fromUtf16(symbol->text);
            if (symbol->spaced) {
                return QLatin1Char(' ') + text + QLatin1Char(' ');
            }
            // TeX ignores spaces after a control word: "\pi r" is "πr".
            skipSpaces();
            return text;
        }
        return name;
    }

    static QString parenthesized(const QString& term)
    {
        return isAtomicTerm(term) ? term : QStringLiteral("(") + term + QStringLiteral(")");
    }

    static QString superscript(const QString& raw)
    {
        const QString text = raw.trimmed();
        if (text == QStringLiteral("∘") || text == QStringLiteral("°")) {
            return QStringLiteral("°");
        }
        if (text == QStringLiteral("′") || text == QStringLiteral("′′")) {
            return text;
        }
        QString mapped;
        for (const QChar c : text) {
            const QChar sup = superscriptChar(c);
            if (sup.isNull()) {
                mapped.clear();
                break;
            }
            mapped += sup;
        }
        if (!mapped.isEmpty()) {
            return mapped;
        }
        return text.size() == 1 ? QStringLiteral("^") + text
                                : QStringLiteral("^(") + text + QStringLiteral(")");
    }

    static QString subscript(const QString& raw)
    {
        const QString text = raw.trimmed();
        QString mapped;
        for (const QChar c : text) {
            const QChar sub = subscriptChar(c);
            if (sub.isNull()) {
                mapped.clear();
                break;
            }
            mapped += sub;
        }
        if (!mapped.isEmpty()) {
            return mapped;
        }
        return text.size() == 1 ? QStringLiteral("\\_") + text
                                : QStringLiteral("\\_(") + text + QStringLiteral(")");
    }

    const QString s_;
    qsizetype pos_ = 0;
};

QString convertMath(const QString& latex, bool display)
{
    const QString converted = LatexConverter(latex).run();
    if (!display) {
        QString inlineText = converted;
        inlineText.replace(QLatin1Char('\n'), QStringLiteral("; "));
        return inlineText;
    }
    QStringList lines;
    for (const QString& line : converted.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            lines << trimmed;
        }
    }
    // Display math becomes its own paragraph; two trailing spaces make a
    // Markdown hard line break between its rows.
    return QStringLiteral("\n\n") + lines.join(QStringLiteral("  \n")) + QStringLiteral("\n\n");
}

// Pandoc's rule for single-dollar math keeps prices such as "$5 and $10" out.
qsizetype closingDollar(const QString& text, qsizetype open)
{
    if (open + 1 >= text.size() || text.at(open + 1).isSpace()) {
        return -1;
    }
    for (qsizetype i = open + 1; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == u'\n' && i + 1 < text.size() && text.at(i + 1) == u'\n') {
            return -1; // math never spans paragraphs
        }
        if (c == u'\\') {
            ++i;
            continue;
        }
        if (c == u'$') {
            const bool spaceBefore = text.at(i - 1).isSpace();
            const bool digitAfter = i + 1 < text.size() && text.at(i + 1).isDigit();
            if (!spaceBefore && !digitAfter) {
                return i;
            }
            return -1;
        }
    }
    return -1;
}

QString convertMathInText(const QString& text)
{
    QString out;
    qsizetype i = 0;
    const auto startsWithAt = [&text](qsizetype at, QLatin1String token) {
        return QStringView(text).mid(at).startsWith(token);
    };
    while (i < text.size()) {
        const QChar c = text.at(i);
        if (c == u'`') {
            qsizetype run = 0;
            while (i + run < text.size() && text.at(i + run) == u'`') {
                ++run;
            }
            const QString fence(run, u'`');
            const qsizetype close = text.indexOf(fence, i + run);
            if (close < 0) {
                out += text.mid(i);
                break;
            }
            out += text.mid(i, close + run - i);
            i = close + run;
            continue;
        }
        if (startsWithAt(i, QLatin1String("$$"))) {
            const qsizetype close = text.indexOf(QStringLiteral("$$"), i + 2);
            if (close > i) {
                out += convertMath(text.mid(i + 2, close - i - 2), true);
                i = close + 2;
                continue;
            }
        } else if (startsWithAt(i, QLatin1String("\\["))) {
            const qsizetype close = text.indexOf(QStringLiteral("\\]"), i + 2);
            if (close > i) {
                out += convertMath(text.mid(i + 2, close - i - 2), true);
                i = close + 2;
                continue;
            }
        } else if (startsWithAt(i, QLatin1String("\\("))) {
            const qsizetype close = text.indexOf(QStringLiteral("\\)"), i + 2);
            if (close > i) {
                out += convertMath(text.mid(i + 2, close - i - 2), false);
                i = close + 2;
                continue;
            }
        } else if (c == u'\\' && i + 1 < text.size()) {
            // Keep Markdown escapes such as "\$" intact.
            out += text.mid(i, 2);
            i += 2;
            continue;
        } else if (c == u'$') {
            const qsizetype close = closingDollar(text, i);
            if (close > i) {
                out += convertMath(text.mid(i + 1, close - i - 1), false);
                i = close + 1;
                continue;
            }
        }
        out += c;
        ++i;
    }
    return out;
}

} // namespace

QString defaultPrompt()
{
    return QStringLiteral(
        "你是一名严谨的解题助手。用户会发送一张截图，截图中包含一道或多道题目。请：\n"
        "1. 准确识别截图中的题目，包括题干、选项以及图表中的关键信息。\n"
        "2. 简明地写出解题思路和关键步骤。\n"
        "3. 最后单独一行写出「答案：……」。选择题写明正确选项的字母和内容；多道题按题号分别作答。\n"
        "4. 如果题目不完整、看不清或有歧义，请指出，并在合理假设下作答。\n"
        "书写要求：使用简洁的 Markdown；不要使用 LaTeX，数学公式用 Unicode 符号和普通文字书写，"
        "例如 x² + 2x + 1 = 0、√2、a/b、≤、≥、π、∫。");
}

QString systemPrompt(const QString& customPrompt, const QString& subjectScope)
{
    const QString custom = customPrompt.trimmed();
    QString prompt = custom.isEmpty() ? defaultPrompt() : custom;
    const QString scope = subjectScope.trimmed();
    if (!scope.isEmpty()) {
        prompt += QStringLiteral("\n\n题目所属领域：%1。请按该领域的专业知识和常用方法作答。").arg(scope);
    }
    return prompt;
}

QString userInstruction()
{
    return QStringLiteral("请解答图片中的题目。");
}

QUrl endpointUrl(QuestionApiFormat format, const QString& rawUrl)
{
    const bool anthropic = format == QuestionApiFormat::Anthropic;
    QString text = rawUrl.trimmed();
    if (text.isEmpty()) {
        return QUrl(anthropic ? QStringLiteral("https://api.anthropic.com/v1/messages")
                              : QStringLiteral("https://api.openai.com/v1/chat/completions"));
    }
    if (!text.contains(QStringLiteral("://"))) {
        text.prepend(QStringLiteral("https://"));
    }
    QUrl url(text, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty()
        || (scheme != QStringLiteral("https") && scheme != QStringLiteral("http"))) {
        return {};
    }

    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    const QString endpoint = anthropic ? QStringLiteral("/messages")
                                       : QStringLiteral("/chat/completions");
    if (path.endsWith(endpoint, Qt::CaseInsensitive)) {
        // Already a full endpoint.
    } else if (path.isEmpty()) {
        path = QStringLiteral("/v1") + endpoint;
    } else if (anthropic && !path.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive)) {
        // Anthropic-compatible services publish a base such as
        // https://host/anthropic, to which the SDKs append /v1/messages.
        path += QStringLiteral("/v1") + endpoint;
    } else {
        // OpenAI-compatible bases usually already carry their version
        // segment (/v1, /api/v3, /compatible-mode/v1, ...).
        path += endpoint;
    }
    url.setPath(path);
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

QString anthropicDefaultModel()
{
    return QStringLiteral("claude-opus-5-5");
}

QString effectiveModel(QuestionApiFormat format, const QString& model)
{
    const QString trimmed = model.trimmed();
    if (trimmed.isEmpty() && format == QuestionApiFormat::Anthropic) {
        return anthropicDefaultModel();
    }
    return trimmed;
}

bool usesServerSideFallback(const QUrl& endpoint, const QString& model)
{
    static const QSet<QString> models = {
        QStringLiteral("claude-fable-5-1"),
        QStringLiteral("claude-opus-5-5"),
        QStringLiteral("claude-opus-5"),
        QStringLiteral("claude-sonnet-5-5"),
    };
    return endpoint.host().compare(QStringLiteral("api.anthropic.com"), Qt::CaseInsensitive) == 0
        && models.contains(model);
}

EncodedImage encodeImage(const QImage& source)
{
    if (source.isNull()) {
        return {};
    }
    QImage image = source.convertToFormat(QImage::Format_RGB32);
    image.setDevicePixelRatio(1.0);
    const int longEdge = qMax(image.width(), image.height());
    if (longEdge > kMaxImageEdge) {
        image = image.scaled(QSize(kMaxImageEdge, kMaxImageEdge), Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    } else if (longEdge < kUpscaleBelowEdge) {
        image = image.scaled(image.size() * 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    EncodedImage encoded;
    encoded.size = image.size();
    {
        QBuffer buffer(&encoded.data);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG")) {
            encoded.data.clear();
        }
    }
    encoded.mimeType = QStringLiteral("image/png");
    if (encoded.data.isEmpty() || encoded.data.size() > kMaxPngBytes) {
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        if (image.save(&buffer, "JPEG", 90)
            && (encoded.data.isEmpty() || jpeg.size() < encoded.data.size())) {
            encoded.data = jpeg;
            encoded.mimeType = QStringLiteral("image/jpeg");
        }
    }
    if (encoded.data.isEmpty()) {
        return {};
    }
    return encoded;
}

HttpRequest buildRequest(const RequestInput& input)
{
    HttpRequest request;
    request.url = endpointUrl(input.format, input.apiUrl);
    const QString model = effectiveModel(input.format, input.model);
    const QString base64 = QString::fromLatin1(input.image.data.toBase64());
    const QString apiKey = input.apiKey.trimmed();

    request.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
    request.headers.append({QByteArrayLiteral("Accept"), QByteArrayLiteral("text/event-stream")});

    QJsonObject body;
    body.insert(QStringLiteral("model"), model);
    body.insert(QStringLiteral("stream"), true);

    if (input.format == QuestionApiFormat::Anthropic) {
        if (!apiKey.isEmpty()) {
            request.headers.append({QByteArrayLiteral("x-api-key"), apiKey.toUtf8()});
        }
        request.headers.append({QByteArrayLiteral("anthropic-version"),
                                QByteArray(kAnthropicVersion)});
        body.insert(QStringLiteral("max_tokens"), kAnthropicMaxTokens);
        body.insert(QStringLiteral("system"), input.systemPrompt);
        const QJsonObject image{
            {QStringLiteral("type"), QStringLiteral("image")},
            {QStringLiteral("source"),
             QJsonObject{{QStringLiteral("type"), QStringLiteral("base64")},
                         {QStringLiteral("media_type"), input.image.mimeType},
                         {QStringLiteral("data"), base64}}},
        };
        const QJsonObject text{{QStringLiteral("type"), QStringLiteral("text")},
                               {QStringLiteral("text"), input.userText}};
        body.insert(QStringLiteral("messages"),
                    QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                           {QStringLiteral("content"), QJsonArray{image, text}}}});
        if (usesServerSideFallback(request.url, model)) {
            request.headers.append({QByteArrayLiteral("anthropic-beta"),
                                    QByteArray(kServerSideFallbackBeta)});
            body.insert(QStringLiteral("fallbacks"), QStringLiteral("default"));
        }
    } else {
        if (!apiKey.isEmpty()) {
            request.headers.append({QByteArrayLiteral("Authorization"),
                                    QByteArrayLiteral("Bearer ") + apiKey.toUtf8()});
        }
        const QJsonObject image{
            {QStringLiteral("type"), QStringLiteral("image_url")},
            {QStringLiteral("image_url"),
             QJsonObject{{QStringLiteral("url"),
                          QStringLiteral("data:%1;base64,%2").arg(input.image.mimeType, base64)}}},
        };
        const QJsonObject text{{QStringLiteral("type"), QStringLiteral("text")},
                               {QStringLiteral("text"), input.userText}};
        body.insert(QStringLiteral("messages"),
                    QJsonArray{
                        QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                    {QStringLiteral("content"), input.systemPrompt}},
                        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                    {QStringLiteral("content"), QJsonArray{image, text}}},
                    });
    }
    request.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    return request;
}

StreamParser::StreamParser(QuestionApiFormat format)
    : format_(format)
{
}

void StreamParser::feed(const QByteArray& chunk)
{
    if (!sawData_ && raw_.size() < kMaxRawResponseBytes) {
        raw_ += chunk;
    }
    pending_ += chunk;
    qsizetype newline = pending_.indexOf('\n');
    while (newline >= 0) {
        QByteArray line = pending_.left(newline);
        pending_.remove(0, newline + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        processLine(line);
        newline = pending_.indexOf('\n');
    }
}

void StreamParser::finish()
{
    if (!pending_.isEmpty()) {
        QByteArray line = pending_;
        pending_.clear();
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        processLine(line);
    }
    if (!sawData_) {
        const CompleteResponse response = parseCompleteResponse(raw_);
        text_ = response.text;
        refused_ = response.refused;
        truncated_ = response.truncated;
        if (error_.isEmpty()) {
            error_ = response.error;
        }
    }
    raw_.clear();
    finished_ = true;
}

void StreamParser::processLine(const QByteArray& line)
{
    if (!line.startsWith("data:")) {
        // "event:" names repeat the JSON "type"; comments and blank
        // separators carry nothing.
        return;
    }
    sawData_ = true;
    raw_.clear();
    processData(line.mid(5).trimmed());
}

void StreamParser::processData(const QByteArray& data)
{
    if (data.isEmpty()) {
        return;
    }
    if (data == "[DONE]") {
        finished_ = true;
        return;
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return;
    }
    const QJsonObject object = document.object();
    const QString error = jsonErrorMessage(object);
    if (!error.isEmpty()) {
        error_ = error;
        return;
    }
    // Detect the shape per event so proxies that translate one format into
    // the other still work.
    if (object.contains(QStringLiteral("choices"))) {
        processOpenAi(object);
    } else if (object.contains(QStringLiteral("type"))) {
        processAnthropic(object);
    } else if (format_ == QuestionApiFormat::OpenAiCompatible) {
        processOpenAi(object);
    }
}

void StreamParser::processOpenAi(const QJsonObject& object)
{
    const QJsonArray choices = object.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        return;
    }
    const QJsonObject choice = choices.first().toObject();
    QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();
    if (delta.isEmpty()) {
        delta = choice.value(QStringLiteral("message")).toObject();
    }
    text_ += joinedContentText(delta.value(QStringLiteral("content")));
    for (const QString& key : {QStringLiteral("reasoning_content"), QStringLiteral("reasoning")}) {
        if (!delta.value(key).toString().isEmpty()) {
            reasoning_ = true;
        }
    }
    const QString finishReason = choice.value(QStringLiteral("finish_reason")).toString();
    if (finishReason == QStringLiteral("length")) {
        truncated_ = true;
    } else if (finishReason == QStringLiteral("content_filter")) {
        refused_ = true;
    }
}

void StreamParser::processAnthropic(const QJsonObject& object)
{
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("content_block_start")) {
        const QJsonObject block = object.value(QStringLiteral("content_block")).toObject();
        const QString blockType = block.value(QStringLiteral("type")).toString();
        if (blockType == QStringLiteral("text")) {
            text_ += block.value(QStringLiteral("text")).toString();
        } else if (blockType == QStringLiteral("thinking")
                   || blockType == QStringLiteral("redacted_thinking")) {
            reasoning_ = true;
        }
    } else if (type == QStringLiteral("content_block_delta")) {
        const QJsonObject delta = object.value(QStringLiteral("delta")).toObject();
        const QString deltaType = delta.value(QStringLiteral("type")).toString();
        if (deltaType == QStringLiteral("text_delta")) {
            text_ += delta.value(QStringLiteral("text")).toString();
        } else if (deltaType == QStringLiteral("thinking_delta")) {
            reasoning_ = true;
        }
    } else if (type == QStringLiteral("message_delta")) {
        const QString stopReason = object.value(QStringLiteral("delta")).toObject()
                                       .value(QStringLiteral("stop_reason")).toString();
        if (stopReason == QStringLiteral("max_tokens")) {
            truncated_ = true;
        } else if (stopReason == QStringLiteral("refusal")) {
            refused_ = true;
        }
    } else if (type == QStringLiteral("message_stop")) {
        finished_ = true;
    }
}

CompleteResponse parseCompleteResponse(const QByteArray& body)
{
    CompleteResponse response;
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(body.trimmed(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        response.error = QStringLiteral("无法解析模型服务返回的内容。");
        return response;
    }
    const QJsonObject object = document.object();
    response.error = jsonErrorMessage(object);
    if (!response.error.isEmpty()) {
        return response;
    }

    if (object.contains(QStringLiteral("choices"))) {
        const QJsonObject choice = object.value(QStringLiteral("choices")).toArray()
                                       .first().toObject();
        response.text = joinedContentText(
            choice.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")));
        const QString finishReason = choice.value(QStringLiteral("finish_reason")).toString();
        response.truncated = finishReason == QStringLiteral("length");
        response.refused = finishReason == QStringLiteral("content_filter");
    } else if (object.contains(QStringLiteral("content"))) {
        response.text = joinedContentText(object.value(QStringLiteral("content")));
        const QString stopReason = object.value(QStringLiteral("stop_reason")).toString();
        response.truncated = stopReason == QStringLiteral("max_tokens");
        response.refused = stopReason == QStringLiteral("refusal");
    }
    if (response.text.trimmed().isEmpty() && !response.refused) {
        response.error = QStringLiteral("模型没有返回答案。");
    }
    return response;
}

QString describeHttpError(int status, const QByteArray& body)
{
    QString detail;
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(body.trimmed(), &parseError);
    if (parseError.error == QJsonParseError::NoError && document.isObject()) {
        detail = jsonErrorMessage(document.object());
    } else {
        detail = QString::fromUtf8(body).simplified();
        if (detail.startsWith(QLatin1Char('<'))) {
            detail.clear(); // an HTML error page says nothing useful here
        }
    }
    if (detail.size() > kMaxErrorDetailLength) {
        detail = detail.left(kMaxErrorDetailLength) + QStringLiteral("…");
    }

    QString summary;
    if (status == 401 || status == 403) {
        summary = QStringLiteral("API Key 无效或没有权限");
    } else if (status == 404) {
        summary = QStringLiteral("接口地址或模型不存在");
    } else if (status == 413) {
        summary = QStringLiteral("截图太大，被模型服务拒绝");
    } else if (status == 429) {
        summary = QStringLiteral("请求过于频繁或额度不足");
    } else if (status == 529 || status == 503) {
        summary = QStringLiteral("模型服务繁忙，请稍后再试");
    } else if (status >= 500) {
        summary = QStringLiteral("模型服务暂时不可用");
    } else {
        summary = QStringLiteral("请求被模型服务拒绝");
    }
    QString message = QStringLiteral("%1（HTTP %2）").arg(summary).arg(status);
    if (!detail.isEmpty()) {
        message += QStringLiteral("：") + detail;
    }
    if ((status == 400 || status == 422) && mentionsVisionSupport(detail)) {
        message += QStringLiteral("\n当前模型可能不支持图片输入，请换用支持图片的多模态模型。");
    }
    return message;
}

QString unicodeMath(const QString& markdown)
{
    // Split out fenced code blocks; everything else is converted in runs so
    // that multi-line $$ blocks stay together.
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    QString out;
    QString prose;
    QString fence;
    const auto flushProse = [&out, &prose]() {
        if (!prose.isEmpty()) {
            out += convertMathInText(prose);
            prose.clear();
        }
    };
    for (int index = 0; index < lines.size(); ++index) {
        const QString& line = lines.at(index);
        const QString withNewline = index + 1 < lines.size() ? line + QLatin1Char('\n') : line;
        const QString trimmed = line.trimmed();
        if (fence.isEmpty()) {
            if (trimmed.startsWith(QStringLiteral("```")) || trimmed.startsWith(QStringLiteral("~~~"))) {
                flushProse();
                fence = trimmed.left(3);
                out += withNewline;
            } else {
                prose += withNewline;
            }
        } else {
            out += withNewline;
            if (trimmed.startsWith(fence)) {
                fence.clear();
            }
        }
    }
    flushProse();
    return out;
}

} // namespace Visnip::Question
